/*
 * Bounded asynchronous HTTP read fan-out and GNSS mock-input upload.
 *
 * Reduced from ESP-IDF's protocols/http_server/async_handlers example.
 * Fixed workers keep long-running read responses and write uploads out of the
 * HTTP daemon so one connected client cannot stop it from accepting another.
 *
 * The HTTP dispatcher is the only consumer of the GNSS publication queue. It
 * copies each publication to a snapshot and wakes every stream worker. Giving
 * each client the publication queue directly would be incorrect: one client
 * would remove bytes before another client could see them.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "generic_task.h"
#include "gnss.h"
#include "http_stream.h"
#include "ota_update.h"

#define HTTP_REQUEST_QUEUE_LENGTH CONFIG_STREAM_MAX_HTTP_CLIENTS
#define HTTP_WRITE_REQUEST_QUEUE_LENGTH 1
#define HTTP_STACK_TASK_STACK_SIZE 3072
#define HTTP_WORKER_TASK_STACK_SIZE 4096
#define HTTP_WRITE_WORKER_TASK_STACK_SIZE 4096
#define HTTP_STACK_TASK_PRIORITY (tskIDLE_PRIORITY + 4)
#define HTTP_WORKER_TASK_PRIORITY (tskIDLE_PRIORITY + 4)
#define HTTP_WRITE_WORKER_TASK_PRIORITY (tskIDLE_PRIORITY + 4)
#define HTTP_DISCONNECT_CHECK_MS 1000
#define HTTP_WRITE_BUFFER_SIZE 512

typedef struct {
	httpd_req_t *req;
} http_async_request_t;

static const char *TAG = "http_stream";
static QueueHandle_t s_http_request_queue;
static QueueHandle_t s_http_write_request_queue;
static SemaphoreHandle_t s_http_worker_ready;
static SemaphoreHandle_t s_http_write_worker_ready;
static SemaphoreHandle_t s_latest_mutex;
static TaskHandle_t s_worker_handles[CONFIG_STREAM_MAX_HTTP_CLIENTS];
static uint8_t s_worker_indexes[CONFIG_STREAM_MAX_HTTP_CLIENTS];
static gnss_publication_t s_latest_publication;
static bool s_has_latest_publication;

static bool latest_publication_copy_if_new(gnss_publication_t *publication,
					   uint32_t last_sequence)
{
	bool copied = false;

	xSemaphoreTake(s_latest_mutex, portMAX_DELAY);
	if (s_has_latest_publication && s_latest_publication.sequence != last_sequence) {
		*publication = s_latest_publication;
		copied = true;
	}
	xSemaphoreGive(s_latest_mutex);

	return copied;
}

static bool stream_client_is_connected(httpd_req_t *req)
{
	const int socket_fd = httpd_req_to_sockfd(req);
	char byte;

	/*
	 * Sends detect most disconnects. This non-blocking peek also releases a
	 * worker when the antenna is silent and there is no next send to fail.
	 */
	const int received = recv(socket_fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
	if (received == 0) {
		return false;
	}

	if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		return false;
	}

	return true;
}

static esp_err_t stream_response_send(httpd_req_t *req, uint8_t worker_index)
{
	uint32_t last_sequence = 0;

	ESP_LOGI(TAG, "stream client connected on worker %u", worker_index);

	httpd_resp_set_type(req, "application/octet-stream");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");

	/*
	 * Notifications can accumulate while a worker is idle. Clear that wakeup;
	 * the snapshot below remains the source of truth for the newest bytes.
	 */
	ulTaskNotifyTake(pdTRUE, 0);

	while (true) {
		gnss_publication_t publication;

		if (latest_publication_copy_if_new(&publication, last_sequence)) {
			/*
			 * Do not hold s_latest_mutex during socket writes. A slow client
			 * must not block publication to the other stream worker.
			 */
			esp_err_t err = httpd_resp_send_chunk(req,
							     (const char *)publication.data,
							     publication.len);
			if (err != ESP_OK) {
				ESP_LOGI(TAG, "stream worker %u disconnected during send: %s",
					 worker_index, esp_err_to_name(err));
				return err;
			}

			/*
			 * Count only successful socket writes. The LED indicator task
			 * serializes GPIO access when two HTTP workers send concurrently.
			 */
			generic_led_packet_consumed();
			last_sequence = publication.sequence;
			continue;
		}

		/*
		 * Updates coalesce naturally. If several packets arrive while this
		 * worker is busy, its next loop sends the newest snapshot and skips
		 * obsolete intermediate points instead of building a backlog.
		 */
		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(HTTP_DISCONNECT_CHECK_MS));
		if (!stream_client_is_connected(req)) {
			ESP_LOGI(TAG, "stream worker %u client disconnected", worker_index);
			return ESP_FAIL;
		}
	}
}

static esp_err_t stream_request_queue(httpd_req_t *req)
{
	httpd_req_t *copy = NULL;
	ESP_RETURN_ON_ERROR(httpd_req_async_handler_begin(req, &copy),
			    TAG,
			    "failed to clone async HTTP request");

	/*
	 * The counting semaphore is the fixed capacity limit. Resource exhaustion
	 * returns 503 immediately rather than allocating unbounded client state.
	 */
	if (xSemaphoreTake(s_http_worker_ready, 0) != pdTRUE) {
		httpd_req_async_handler_complete(copy);
		return ESP_ERR_NOT_FINISHED;
	}

	const http_async_request_t async_req = {
		.req = copy,
	};

	if (xQueueSend(s_http_request_queue, &async_req, 0) != pdTRUE) {
		xSemaphoreGive(s_http_worker_ready);
		httpd_req_async_handler_complete(copy);
		return ESP_FAIL;
	}

	return ESP_OK;
}

static esp_err_t read_get_handler(httpd_req_t *req)
{
	if (stream_request_queue(req) == ESP_OK) {
		return ESP_OK;
	}

	httpd_resp_set_status(req, "503 Service Unavailable");
	httpd_resp_set_type(req, "text/plain");
	httpd_resp_sendstr(req, "all stream workers are in use\n");
	return ESP_OK;
}

static const httpd_uri_t read_uri = {
	.uri = "/stream",
	.method = HTTP_GET,
	.handler = read_get_handler,
};

static esp_err_t write_request_queue(httpd_req_t *req)
{
	httpd_req_t *copy = NULL;

	/*
	 * Only one writer may own the parser override. Take capacity before cloning
	 * the async request so a rejected upload stays under HTTP daemon ownership.
	 */
	if (xSemaphoreTake(s_http_write_worker_ready, 0) != pdTRUE) {
		return ESP_ERR_NOT_FINISHED;
	}

	esp_err_t err = httpd_req_async_handler_begin(req, &copy);
	if (err != ESP_OK) {
		xSemaphoreGive(s_http_write_worker_ready);
		return err;
	}

	const http_async_request_t async_req = {
		.req = copy,
	};

	if (xQueueSend(s_http_write_request_queue, &async_req, 0) != pdTRUE) {
		xSemaphoreGive(s_http_write_worker_ready);
		httpd_req_async_handler_complete(copy);
		return ESP_FAIL;
	}

	return ESP_OK;
}

static esp_err_t write_post_handler(httpd_req_t *req)
{
	/*
	 * ESP-IDF's HTTP server does not currently decode chunked request bodies.
	 * A mock upload therefore needs a finite Content-Length, which curl adds
	 * automatically for --data-binary @file.
	 */
	if (req->content_len == 0) {
		httpd_resp_set_status(req, "411 Length Required");
		httpd_resp_set_type(req, "text/plain");
		httpd_resp_sendstr(req, "POST /write requires a non-empty Content-Length body\n");
		return ESP_OK;
	}

	if (write_request_queue(req) == ESP_OK) {
		return ESP_OK;
	}

	httpd_resp_set_status(req, "409 Conflict");
	httpd_resp_set_type(req, "text/plain");
	httpd_resp_sendstr(req, "another GNSS writer is already active\n");
	return ESP_OK;
}

static const httpd_uri_t write_uri = {
	.uri = "/write",
	.method = HTTP_POST,
	.handler = write_post_handler,
};

static void http_stream_worker_task(void *arg)
{
	const uint8_t worker_index = *(uint8_t *)arg;

	while (true) {
		http_async_request_t async_req;

		/* Signal capacity immediately before waiting for one owned request. */
		xSemaphoreGive(s_http_worker_ready);
		if (xQueueReceive(s_http_request_queue, &async_req, portMAX_DELAY) != pdTRUE) {
			continue;
		}

		generic_led_client_connected();
		stream_response_send(async_req.req, worker_index);
		generic_led_client_disconnected();

		/*
		 * Async completion returns socket ownership to the HTTP server after a
		 * disconnect or error. Forgetting this leaks the bounded worker pool.
		 */
		if (httpd_req_async_handler_complete(async_req.req) != ESP_OK) {
			ESP_LOGW(TAG, "worker %u failed to complete async request", worker_index);
		}
	}
}

static esp_err_t write_request_receive(httpd_req_t *req)
{
	uint8_t buffer[HTTP_WRITE_BUFFER_SIZE];
	size_t remaining = req->content_len;
	esp_err_t result = gnss_input_override_begin();

	if (result != ESP_OK) {
		httpd_resp_set_status(req, "409 Conflict");
		httpd_resp_set_type(req, "text/plain");
		httpd_resp_sendstr(req, "GNSS input override is already active\n");
		return ESP_OK;
	}

	ESP_LOGI(TAG, "GNSS writer connected with %u-byte body",
		 (unsigned int)remaining);

	while (remaining > 0) {
		const size_t requested = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
		const int received = httpd_req_recv(req, (char *)buffer, requested);

		/*
		 * A paced mock stream may intentionally pause between chunks. Keep its
		 * parser override until bytes resume or the socket reports a hard error.
		 */
		if (received == HTTPD_SOCK_ERR_TIMEOUT) {
			continue;
		}

		if (received <= 0) {
			ESP_LOGW(TAG, "GNSS writer disconnected with %u bytes remaining",
				 (unsigned int)remaining);
			result = ESP_FAIL;
			break;
		}

		result = gnss_input_override_feed(buffer, (size_t)received);
		if (result != ESP_OK) {
			ESP_LOGE(TAG, "GNSS override feed failed: %s", esp_err_to_name(result));
			break;
		}

		remaining -= (size_t)received;
	}

	/*
	 * Ending the override synchronously flushes the best uploaded candidate
	 * before UART2 parser ownership resumes, including for very small files.
	 */
	gnss_input_override_end();

	if (result != ESP_OK) {
		return result;
	}

	ESP_LOGI(TAG, "GNSS writer upload complete");
	httpd_resp_set_type(req, "text/plain");
	return httpd_resp_sendstr(req, "GNSS mock stream accepted\n");
}

static void http_write_worker_task(void *arg)
{
	while (true) {
		http_async_request_t async_req;

		xSemaphoreGive(s_http_write_worker_ready);
		if (xQueueReceive(s_http_write_request_queue, &async_req, portMAX_DELAY) != pdTRUE) {
			continue;
		}

		write_request_receive(async_req.req);

		if (httpd_req_async_handler_complete(async_req.req) != ESP_OK) {
			ESP_LOGW(TAG, "write worker failed to complete async request");
		}
	}
}

static void http_stack_task(void *arg)
{
	gnss_publication_t publication;

	while (true) {
		if (!gnss_publication_receive(&publication, portMAX_DELAY)) {
			continue;
		}

		/* Copy first, then wake workers. The mutex is never held during I/O. */
		xSemaphoreTake(s_latest_mutex, portMAX_DELAY);
		s_latest_publication = publication;
		s_has_latest_publication = true;
		xSemaphoreGive(s_latest_mutex);

		for (size_t index = 0; index < CONFIG_STREAM_MAX_HTTP_CLIENTS; index++) {
			if (s_worker_handles[index] != NULL) {
				xTaskNotifyGive(s_worker_handles[index]);
			}
		}
	}
}

esp_err_t http_stream_stack_init(void)
{
	s_http_request_queue = xQueueCreate(HTTP_REQUEST_QUEUE_LENGTH,
					   sizeof(http_async_request_t));
	s_http_write_request_queue = xQueueCreate(HTTP_WRITE_REQUEST_QUEUE_LENGTH,
						 sizeof(http_async_request_t));
	s_http_worker_ready = xSemaphoreCreateCounting(CONFIG_STREAM_MAX_HTTP_CLIENTS, 0);
	s_http_write_worker_ready = xSemaphoreCreateBinary();
	s_latest_mutex = xSemaphoreCreateMutex();
	if (s_http_request_queue == NULL ||
	    s_http_write_request_queue == NULL ||
	    s_http_worker_ready == NULL ||
	    s_http_write_worker_ready == NULL ||
	    s_latest_mutex == NULL) {
		return ESP_ERR_NO_MEM;
	}

	httpd_config_t config = HTTPD_DEFAULT_CONFIG();

	/*
	 * Reserve sockets for two /stream clients, one /write client, one /flash
	 * client, and one excess request that can receive an HTTP error. Wi-Fi
	 * station count and HTTP socket count are separate limits.
	 */
	config.lru_purge_enable = false;
	config.max_uri_handlers = 6;
	config.max_open_sockets = CONFIG_STREAM_MAX_HTTP_CLIENTS + 3;

	httpd_handle_t server = NULL;
	ESP_LOGI(TAG, "starting HTTP server on port %d for %d read clients and one writer",
		 config.server_port, CONFIG_STREAM_MAX_HTTP_CLIENTS);
	ESP_ERROR_CHECK(httpd_start(&server, &config));
	ESP_ERROR_CHECK(httpd_register_uri_handler(server, &read_uri));
	ESP_ERROR_CHECK(httpd_register_uri_handler(server, &write_uri));
	ESP_ERROR_CHECK(ota_update_register_http_handlers(server));

	return ESP_OK;
}

esp_err_t http_stream_start_tasks(void)
{
	for (uint8_t index = 0; index < CONFIG_STREAM_MAX_HTTP_CLIENTS; index++) {
		s_worker_indexes[index] = index;

		BaseType_t created = xTaskCreatePinnedToCore(http_stream_worker_task,
							    "http_worker",
							    HTTP_WORKER_TASK_STACK_SIZE,
							    &s_worker_indexes[index],
							    HTTP_WORKER_TASK_PRIORITY,
							    &s_worker_handles[index],
							    tskNO_AFFINITY);
		if (created != pdPASS) {
			return ESP_ERR_NO_MEM;
		}
	}

	BaseType_t created = xTaskCreatePinnedToCore(http_write_worker_task,
						    "http_write",
						    HTTP_WRITE_WORKER_TASK_STACK_SIZE,
						    NULL,
						    HTTP_WRITE_WORKER_TASK_PRIORITY,
						    NULL,
						    tskNO_AFFINITY);
	if (created != pdPASS) {
		return ESP_ERR_NO_MEM;
	}

	created = xTaskCreatePinnedToCore(http_stack_task,
						    "http_stack",
						    HTTP_STACK_TASK_STACK_SIZE,
						    NULL,
						    HTTP_STACK_TASK_PRIORITY,
						    NULL,
						    tskNO_AFFINITY);
	return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
