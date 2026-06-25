/*
 * Bounded HTTP tail of the same log output `idf.py monitor` prints over
 * serial.
 *
 * esp_log_set_vprintf() is the only supported way to observe every ESP_LOGx
 * call system-wide, and ESP-IDF documents it as safe to call concurrently
 * from multiple tasks. The replacement handler copies each formatted line
 * into a fixed-size ring buffer and still forwards to the previous vprintf,
 * so the USB serial console keeps working exactly as before. One HTTP worker
 * drains the ring buffer for GET /log, the same bounded single-client shape
 * already used for POST /write in http_stream.c.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "log_stream.h"

#define LOG_RING_BUFFER_SIZE 4096
#define LOG_LINE_MAX_LEN 256
#define LOG_REQUEST_QUEUE_LENGTH 1
#define LOG_WORKER_TASK_STACK_SIZE 4096
#define LOG_WORKER_TASK_PRIORITY (tskIDLE_PRIORITY + 4)
#define LOG_DISCONNECT_CHECK_MS 1000
#define LOG_SEND_CHUNK_SIZE 512

typedef struct {
	httpd_req_t *req;
} log_async_request_t;

static const char *TAG = "log_stream";
static char s_ring_buffer[LOG_RING_BUFFER_SIZE];
static size_t s_write_total;
static portMUX_TYPE s_ring_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_previous_vprintf;
static QueueHandle_t s_log_request_queue;
static SemaphoreHandle_t s_log_worker_ready;
static TaskHandle_t s_log_worker_handle;

static int log_capture_vprintf(const char *format, va_list args)
{
	char line[LOG_LINE_MAX_LEN];
	va_list copy;

	va_copy(copy, args);
	const int formatted = vsnprintf(line, sizeof(line), format, copy);
	va_end(copy);

	if (formatted > 0) {
		size_t len = (size_t)formatted;

		if (len > sizeof(line) - 1) {
			len = sizeof(line) - 1;
		}

		taskENTER_CRITICAL(&s_ring_lock);
		for (size_t index = 0; index < len; index++) {
			s_ring_buffer[(s_write_total + index) % LOG_RING_BUFFER_SIZE] = line[index];
		}
		s_write_total += len;
		taskEXIT_CRITICAL(&s_ring_lock);

		if (s_log_worker_handle != NULL) {
			xTaskNotifyGive(s_log_worker_handle);
		}
	}

	return s_previous_vprintf != NULL ? s_previous_vprintf(format, args) : 0;
}

static bool log_client_is_connected(httpd_req_t *req)
{
	const int socket_fd = httpd_req_to_sockfd(req);
	char byte;

	const int received = recv(socket_fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
	if (received == 0) {
		return false;
	}

	if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		return false;
	}

	return true;
}

static esp_err_t log_response_send(httpd_req_t *req)
{
	size_t last_sent;

	ESP_LOGI(TAG, "log client connected");

	httpd_resp_set_type(req, "text/plain");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");

	/*
	 * Start from the oldest byte still held in the ring buffer so a client
	 * sees recent history instead of only lines logged after it connected.
	 */
	taskENTER_CRITICAL(&s_ring_lock);
	last_sent = s_write_total > LOG_RING_BUFFER_SIZE ? s_write_total - LOG_RING_BUFFER_SIZE : 0;
	taskEXIT_CRITICAL(&s_ring_lock);

	ulTaskNotifyTake(pdTRUE, 0);

	while (true) {
		char chunk[LOG_SEND_CHUNK_SIZE];
		size_t available;
		size_t to_copy;

		taskENTER_CRITICAL(&s_ring_lock);
		available = s_write_total - last_sent;
		if (available > LOG_RING_BUFFER_SIZE) {
			/* The buffer wrapped past this client; skip to the oldest survivor. */
			last_sent = s_write_total - LOG_RING_BUFFER_SIZE;
			available = LOG_RING_BUFFER_SIZE;
		}
		to_copy = available < sizeof(chunk) ? available : sizeof(chunk);
		for (size_t index = 0; index < to_copy; index++) {
			chunk[index] = s_ring_buffer[(last_sent + index) % LOG_RING_BUFFER_SIZE];
		}
		taskEXIT_CRITICAL(&s_ring_lock);

		if (to_copy > 0) {
			const esp_err_t err = httpd_resp_send_chunk(req, chunk, to_copy);
			if (err != ESP_OK) {
				ESP_LOGI(TAG, "log client disconnected during send: %s", esp_err_to_name(err));
				return err;
			}

			last_sent += to_copy;
			continue;
		}

		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LOG_DISCONNECT_CHECK_MS));
		if (!log_client_is_connected(req)) {
			ESP_LOGI(TAG, "log client disconnected");
			return ESP_FAIL;
		}
	}
}

static esp_err_t log_request_queue(httpd_req_t *req)
{
	httpd_req_t *copy = NULL;

	if (xSemaphoreTake(s_log_worker_ready, 0) != pdTRUE) {
		return ESP_ERR_NOT_FINISHED;
	}

	esp_err_t err = httpd_req_async_handler_begin(req, &copy);
	if (err != ESP_OK) {
		xSemaphoreGive(s_log_worker_ready);
		return err;
	}

	const log_async_request_t async_req = {
		.req = copy,
	};

	if (xQueueSend(s_log_request_queue, &async_req, 0) != pdTRUE) {
		xSemaphoreGive(s_log_worker_ready);
		httpd_req_async_handler_complete(copy);
		return ESP_FAIL;
	}

	return ESP_OK;
}

static esp_err_t log_get_handler(httpd_req_t *req)
{
	if (log_request_queue(req) == ESP_OK) {
		return ESP_OK;
	}

	httpd_resp_set_status(req, "409 Conflict");
	httpd_resp_set_type(req, "text/plain");
	httpd_resp_sendstr(req, "a /log client is already connected\n");
	return ESP_OK;
}

static const httpd_uri_t log_uri = {
	.uri = "/log",
	.method = HTTP_GET,
	.handler = log_get_handler,
};

static void log_worker_task(void *arg)
{
	while (true) {
		log_async_request_t async_req;

		xSemaphoreGive(s_log_worker_ready);
		if (xQueueReceive(s_log_request_queue, &async_req, portMAX_DELAY) != pdTRUE) {
			continue;
		}

		log_response_send(async_req.req);

		if (httpd_req_async_handler_complete(async_req.req) != ESP_OK) {
			ESP_LOGW(TAG, "log worker failed to complete async request");
		}
	}
}

esp_err_t log_stream_stack_init(void)
{
	s_log_request_queue = xQueueCreate(LOG_REQUEST_QUEUE_LENGTH, sizeof(log_async_request_t));
	s_log_worker_ready = xSemaphoreCreateBinary();
	if (s_log_request_queue == NULL || s_log_worker_ready == NULL) {
		return ESP_ERR_NO_MEM;
	}

	s_previous_vprintf = esp_log_set_vprintf(log_capture_vprintf);
	return ESP_OK;
}

esp_err_t log_stream_register_http_handlers(httpd_handle_t server)
{
	return httpd_register_uri_handler(server, &log_uri);
}

esp_err_t log_stream_start_task(void)
{
	const BaseType_t created = xTaskCreatePinnedToCore(log_worker_task,
							   "log_worker",
							   LOG_WORKER_TASK_STACK_SIZE,
							   NULL,
							   LOG_WORKER_TASK_PRIORITY,
							   &s_log_worker_handle,
							   tskNO_AFFINITY);

	return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
