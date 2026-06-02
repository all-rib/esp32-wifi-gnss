/*
 * Bounded HTTP OTA upload with SHA-256 integrity verification and rollback.
 *
 * Reduced from ESP-IDF's system/ota/native_ota_example. That example downloads
 * an image with esp_http_client. This firmware already owns an HTTP server, so
 * its dedicated worker receives POST /flash bytes and feeds the same OTA APIs:
 *
 *   esp_ota_get_next_update_partition()
 *   esp_ota_begin()
 *   esp_ota_write()
 *   esp_ota_end()
 *   esp_ota_set_boot_partition()
 *
 * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE makes a newly booted image pending
 * verification. It must receive POST /flash/confirm before the timer expires,
 * otherwise the reboot causes ESP-IDF's bootloader to select the older image.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "psa/crypto.h"

#include "ota_update.h"

#define OTA_REQUEST_QUEUE_LENGTH 1
#define OTA_WORKER_TASK_STACK_SIZE 7168
#define OTA_WORKER_TASK_PRIORITY (tskIDLE_PRIORITY + 4)
#define OTA_WRITE_BUFFER_SIZE 1024
#define OTA_SHA256_LEN 32
#define OTA_SHA256_HEX_LEN (OTA_SHA256_LEN * 2)
#define OTA_RESTART_DELAY_MS 750
#define OTA_MAX_CONSECUTIVE_RECV_TIMEOUTS 3
#define OTA_SHA256_HEADER "X-Firmware-SHA256"

typedef struct {
	httpd_req_t *req;
	const esp_partition_t *update_partition;
	uint8_t expected_sha256[OTA_SHA256_LEN];
} ota_async_request_t;

static const char *TAG = "ota_update";
static QueueHandle_t s_ota_request_queue;
static SemaphoreHandle_t s_ota_worker_ready;
static TimerHandle_t s_restart_timer;
static TimerHandle_t s_confirmation_timer;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_confirmation_pending;
static bool s_restart_scheduled;

static bool ota_confirmation_pending_get(void)
{
	bool pending;

	taskENTER_CRITICAL(&s_state_lock);
	pending = s_confirmation_pending;
	taskEXIT_CRITICAL(&s_state_lock);

	return pending;
}

static void ota_confirmation_pending_set(bool pending)
{
	taskENTER_CRITICAL(&s_state_lock);
	s_confirmation_pending = pending;
	taskEXIT_CRITICAL(&s_state_lock);
}

static bool ota_restart_scheduled_get(void)
{
	bool scheduled;

	taskENTER_CRITICAL(&s_state_lock);
	scheduled = s_restart_scheduled;
	taskEXIT_CRITICAL(&s_state_lock);

	return scheduled;
}

static esp_err_t ota_send_text(httpd_req_t *req, const char *status, const char *text)
{
	if (status != NULL) {
		httpd_resp_set_status(req, status);
	}
	httpd_resp_set_type(req, "text/plain");
	return httpd_resp_sendstr(req, text);
}

static int hex_digit_value(char digit)
{
	if (digit >= '0' && digit <= '9') {
		return digit - '0';
	}

	if (digit >= 'A' && digit <= 'F') {
		return digit - 'A' + 10;
	}

	if (digit >= 'a' && digit <= 'f') {
		return digit - 'a' + 10;
	}

	return -1;
}

static bool ota_sha256_header_parse(httpd_req_t *req, uint8_t output[OTA_SHA256_LEN])
{
	char value[OTA_SHA256_HEX_LEN + 1];

	if (httpd_req_get_hdr_value_len(req, OTA_SHA256_HEADER) != OTA_SHA256_HEX_LEN ||
	    httpd_req_get_hdr_value_str(req, OTA_SHA256_HEADER, value, sizeof(value)) != ESP_OK) {
		return false;
	}

	for (size_t index = 0; index < OTA_SHA256_LEN; index++) {
		const int high = hex_digit_value(value[index * 2]);
		const int low = hex_digit_value(value[(index * 2) + 1]);

		if (high < 0 || low < 0) {
			return false;
		}

		output[index] = (uint8_t)((high << 4) | low);
	}

	return true;
}

static void ota_restart_callback(TimerHandle_t timer)
{
	ESP_LOGW(TAG, "restarting into selected firmware");
	esp_restart();
}

static void ota_confirmation_timeout_callback(TimerHandle_t timer)
{
	/*
	 * Rebooting while the running image is still ESP_OTA_IMG_PENDING_VERIFY
	 * makes ESP-IDF's bootloader mark it aborted and select the older image.
	 */
	if (ota_confirmation_pending_get()) {
		ESP_LOGE(TAG, "firmware confirmation timed out; rebooting to trigger rollback");
		esp_restart();
	}
}

static void ota_restart_schedule(void)
{
	bool start_timer = false;

	taskENTER_CRITICAL(&s_state_lock);
	if (!s_restart_scheduled) {
		s_restart_scheduled = true;
		start_timer = true;
	}
	taskEXIT_CRITICAL(&s_state_lock);

	if (start_timer && xTimerStart(s_restart_timer, 0) != pdPASS) {
		/*
		 * Reaching this branch means the image was already selected for boot.
		 * Restart directly instead of leaving the device in an ambiguous state.
		 */
		ESP_LOGE(TAG, "failed to schedule restart timer; restarting immediately");
		esp_restart();
	}
}

static esp_err_t ota_request_queue(httpd_req_t *req,
				   const esp_partition_t *update_partition,
				   const uint8_t expected_sha256[OTA_SHA256_LEN])
{
	httpd_req_t *copy = NULL;

	/*
	 * OTA owns one fixed worker. This prevents overlapping erase/write sessions
	 * and bounds heap use even if a client sends repeated requests.
	 */
	if (xSemaphoreTake(s_ota_worker_ready, 0) != pdTRUE) {
		return ESP_ERR_NOT_FINISHED;
	}

	esp_err_t err = httpd_req_async_handler_begin(req, &copy);
	if (err != ESP_OK) {
		xSemaphoreGive(s_ota_worker_ready);
		return err;
	}

	ota_async_request_t async_req = {
		.req = copy,
		.update_partition = update_partition,
	};
	memcpy(async_req.expected_sha256, expected_sha256, sizeof(async_req.expected_sha256));

	if (xQueueSend(s_ota_request_queue, &async_req, 0) != pdTRUE) {
		xSemaphoreGive(s_ota_worker_ready);
		httpd_req_async_handler_complete(copy);
		return ESP_FAIL;
	}

	return ESP_OK;
}

static esp_err_t ota_flash_post_handler(httpd_req_t *req)
{
	uint8_t expected_sha256[OTA_SHA256_LEN];

	if (ota_restart_scheduled_get()) {
		return ota_send_text(req, "409 Conflict", "a reboot is already scheduled\n");
	}

	if (ota_confirmation_pending_get()) {
		return ota_send_text(req,
				     "409 Conflict",
				     "confirm or roll back the running firmware before another update\n");
	}

	/*
	 * ESP-IDF's HTTP server does not decode chunked request bodies. A finite
	 * Content-Length also lets us reject images that cannot fit before erasing
	 * any part of the inactive application slot.
	 */
	if (req->content_len == 0) {
		return ota_send_text(req,
				     "411 Length Required",
				     "POST /flash requires a non-empty Content-Length body\n");
	}

	if (!ota_sha256_header_parse(req, expected_sha256)) {
		return ota_send_text(req,
				     "400 Bad Request",
				     "missing or invalid X-Firmware-SHA256 header\n");
	}

	const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
	if (update_partition == NULL) {
		return ota_send_text(req,
				     "503 Service Unavailable",
				     "no inactive OTA application slot is available\n");
	}

	if (req->content_len > update_partition->size) {
		return ota_send_text(req,
				     "413 Content Too Large",
				     "firmware image does not fit the inactive OTA slot\n");
	}

	if (ota_request_queue(req, update_partition, expected_sha256) == ESP_OK) {
		return ESP_OK;
	}

	return ota_send_text(req, "409 Conflict", "another firmware upload is already active\n");
}

static const httpd_uri_t ota_flash_uri = {
	.uri = "/flash",
	.method = HTTP_POST,
	.handler = ota_flash_post_handler,
};

static esp_err_t ota_status_get_handler(httpd_req_t *req)
{
	char response[256];
	const esp_partition_t *running = esp_ota_get_running_partition();
	const esp_partition_t *boot = esp_ota_get_boot_partition();
	const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

	snprintf(response,
		 sizeof(response),
		 "running=%s\nboot=%s\nconfirmation_pending=%s\nrollback_possible=%s\nnext_slot=%s\nnext_slot_bytes=%u\n",
		 running != NULL ? running->label : "<none>",
		 boot != NULL ? boot->label : "<none>",
		 ota_confirmation_pending_get() ? "yes" : "no",
		 esp_ota_check_rollback_is_possible() ? "yes" : "no",
		 next != NULL ? next->label : "<none>",
		 next != NULL ? (unsigned int)next->size : 0);

	return ota_send_text(req, NULL, response);
}

static const httpd_uri_t ota_status_uri = {
	.uri = "/flash/status",
	.method = HTTP_GET,
	.handler = ota_status_get_handler,
};

static esp_err_t ota_confirm_post_handler(httpd_req_t *req)
{
	if (!ota_confirmation_pending_get()) {
		return ota_send_text(req, "409 Conflict", "running firmware is not awaiting confirmation\n");
	}

	const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "failed to confirm running firmware: %s", esp_err_to_name(err));
		return ota_send_text(req, "500 Internal Server Error", "failed to confirm running firmware\n");
	}

	ota_confirmation_pending_set(false);
	xTimerStop(s_confirmation_timer, 0);
	ESP_LOGI(TAG, "running firmware confirmed");
	return ota_send_text(req, NULL, "running firmware confirmed\n");
}

static const httpd_uri_t ota_confirm_uri = {
	.uri = "/flash/confirm",
	.method = HTTP_POST,
	.handler = ota_confirm_post_handler,
};

static esp_err_t ota_rollback_post_handler(httpd_req_t *req)
{
	if (ota_restart_scheduled_get()) {
		return ota_send_text(req, "409 Conflict", "a reboot is already scheduled\n");
	}

	if (!esp_ota_check_rollback_is_possible()) {
		return ota_send_text(req, "409 Conflict", "no older bootable OTA image is available\n");
	}

	const esp_err_t err = esp_ota_mark_app_invalid_rollback();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "failed to select rollback firmware: %s", esp_err_to_name(err));
		return ota_send_text(req, "500 Internal Server Error", "failed to select rollback firmware\n");
	}

	ESP_LOGW(TAG, "manual rollback selected");
	const esp_err_t response_err = ota_send_text(req, NULL, "rollback selected; rebooting\n");
	ota_restart_schedule();
	return response_err;
}

static const httpd_uri_t ota_rollback_uri = {
	.uri = "/flash/rollback",
	.method = HTTP_POST,
	.handler = ota_rollback_post_handler,
};

static esp_err_t ota_flash_receive(const ota_async_request_t *async_req,
				   bool *restart_required)
{
	uint8_t buffer[OTA_WRITE_BUFFER_SIZE];
	uint8_t actual_sha256[OTA_SHA256_LEN];
	size_t actual_sha256_len = 0;
	size_t remaining = async_req->req->content_len;
	size_t written = 0;
	int consecutive_timeouts = 0;
	esp_ota_handle_t ota_handle = 0;
	bool ota_active = false;
	bool hash_active = false;
	psa_hash_operation_t hash_operation = PSA_HASH_OPERATION_INIT;
	esp_err_t result = ESP_FAIL;

	*restart_required = false;

	psa_status_t psa_status = psa_hash_setup(&hash_operation, PSA_ALG_SHA_256);
	if (psa_status != PSA_SUCCESS) {
		ESP_LOGE(TAG, "failed to initialize SHA-256 operation: %ld", (long)psa_status);
		ota_send_text(async_req->req, "500 Internal Server Error", "failed to initialize checksum\n");
		goto cleanup;
	}
	hash_active = true;

	/*
	 * Sequential writes erase flash incrementally and match the ordered HTTP
	 * body. The handler already checked Content-Length against partition size.
	 */
	result = esp_ota_begin(async_req->update_partition,
			       OTA_WITH_SEQUENTIAL_WRITES,
			       &ota_handle);
	if (result != ESP_OK) {
		ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(result));
		ota_send_text(async_req->req, "500 Internal Server Error", "failed to begin OTA update\n");
		goto cleanup;
	}
	ota_active = true;

	ESP_LOGI(TAG,
		 "receiving %u-byte firmware image into %s at 0x%lx",
		 (unsigned int)remaining,
		 async_req->update_partition->label,
		 (unsigned long)async_req->update_partition->address);

	while (remaining > 0) {
		const size_t requested = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
		const int received = httpd_req_recv(async_req->req, (char *)buffer, requested);

		if (received == HTTPD_SOCK_ERR_TIMEOUT) {
			if (++consecutive_timeouts < OTA_MAX_CONSECUTIVE_RECV_TIMEOUTS) {
				continue;
			}

			ESP_LOGW(TAG, "firmware upload timed out with %u bytes remaining",
				 (unsigned int)remaining);
			ota_send_text(async_req->req, "408 Request Timeout", "firmware upload timed out\n");
			goto cleanup;
		}

		if (received <= 0) {
			ESP_LOGW(TAG, "firmware uploader disconnected with %u bytes remaining",
				 (unsigned int)remaining);
			ota_send_text(async_req->req, "400 Bad Request", "incomplete firmware upload\n");
			goto cleanup;
		}
		consecutive_timeouts = 0;

		psa_status = psa_hash_update(&hash_operation, buffer, (size_t)received);
		if (psa_status != PSA_SUCCESS) {
			ESP_LOGE(TAG, "SHA-256 update failed: %ld", (long)psa_status);
			ota_send_text(async_req->req, "500 Internal Server Error", "checksum calculation failed\n");
			goto cleanup;
		}

		result = esp_ota_write(ota_handle, buffer, (size_t)received);
		if (result != ESP_OK) {
			ESP_LOGE(TAG, "esp_ota_write failed after %u bytes: %s",
				 (unsigned int)written,
				 esp_err_to_name(result));
			ota_send_text(async_req->req, "422 Unprocessable Content", "firmware write validation failed\n");
			goto cleanup;
		}

		written += (size_t)received;
		remaining -= (size_t)received;
	}

	psa_status = psa_hash_finish(&hash_operation,
				     actual_sha256,
				     sizeof(actual_sha256),
				     &actual_sha256_len);
	hash_active = false;
	if (psa_status != PSA_SUCCESS || actual_sha256_len != sizeof(actual_sha256)) {
		ESP_LOGE(TAG, "SHA-256 finish failed: %ld", (long)psa_status);
		ota_send_text(async_req->req, "500 Internal Server Error", "checksum calculation failed\n");
		goto cleanup;
	}

	/*
	 * This comparison detects transport corruption. It does not authenticate
	 * who built the image. Production devices should require signed firmware.
	 */
	if (memcmp(actual_sha256, async_req->expected_sha256, sizeof(actual_sha256)) != 0) {
		ESP_LOGW(TAG, "uploaded firmware SHA-256 does not match request header");
		ota_send_text(async_req->req, "422 Unprocessable Content", "firmware SHA-256 mismatch\n");
		goto cleanup;
	}

	result = esp_ota_end(ota_handle);
	ota_active = false;
	if (result != ESP_OK) {
		ESP_LOGE(TAG, "esp_ota_end rejected image: %s", esp_err_to_name(result));
		ota_send_text(async_req->req, "422 Unprocessable Content", "ESP-IDF rejected firmware image\n");
		goto cleanup;
	}

	result = esp_ota_set_boot_partition(async_req->update_partition);
	if (result != ESP_OK) {
		ESP_LOGE(TAG, "failed to select OTA boot partition: %s", esp_err_to_name(result));
		ota_send_text(async_req->req, "500 Internal Server Error", "failed to select uploaded firmware\n");
		goto cleanup;
	}

	ESP_LOGI(TAG, "firmware image accepted; reboot scheduled");
	ota_send_text(async_req->req,
		      NULL,
		      "firmware accepted; rebooting into pending image\n"
		      "reconnect and POST /flash/confirm before the rollback timeout\n");
	*restart_required = true;
	result = ESP_OK;

cleanup:
	if (hash_active) {
		psa_hash_abort(&hash_operation);
	}
	if (ota_active) {
		esp_ota_abort(ota_handle);
	}

	return result;
}

static void ota_worker_task(void *arg)
{
	while (true) {
		ota_async_request_t async_req;

		/* Signal fixed capacity immediately before waiting for owned work. */
		xSemaphoreGive(s_ota_worker_ready);
		if (xQueueReceive(s_ota_request_queue, &async_req, portMAX_DELAY) != pdTRUE) {
			continue;
		}

		bool restart_required = false;
		const int socket_fd = httpd_req_to_sockfd(async_req.req);
		httpd_handle_t server = async_req.req->handle;
		const esp_err_t result = ota_flash_receive(&async_req, &restart_required);

		if (httpd_req_async_handler_complete(async_req.req) != ESP_OK) {
			ESP_LOGW(TAG, "OTA worker failed to complete async request");
		}

		if (restart_required) {
			ota_restart_schedule();
		} else if (result != ESP_OK && socket_fd >= 0) {
			/*
			 * A failed upload may leave unread body bytes in the TCP stream.
			 * Close that session so they cannot be parsed as another request.
			 */
			httpd_sess_trigger_close(server, socket_fd);
		}
	}
}

esp_err_t ota_update_stack_init(void)
{
	s_ota_request_queue = xQueueCreate(OTA_REQUEST_QUEUE_LENGTH,
					  sizeof(ota_async_request_t));
	s_ota_worker_ready = xSemaphoreCreateBinary();
	s_restart_timer = xTimerCreate("ota_restart",
				       pdMS_TO_TICKS(OTA_RESTART_DELAY_MS),
				       pdFALSE,
				       NULL,
				       ota_restart_callback);
	s_confirmation_timer = xTimerCreate("ota_confirm",
					    pdMS_TO_TICKS(CONFIG_OTA_CONFIRM_TIMEOUT_SECONDS * 1000),
					    pdFALSE,
					    NULL,
					    ota_confirmation_timeout_callback);

	if (s_ota_request_queue == NULL ||
	    s_ota_worker_ready == NULL ||
	    s_restart_timer == NULL ||
	    s_confirmation_timer == NULL) {
		return ESP_ERR_NO_MEM;
	}

	const psa_status_t psa_status = psa_crypto_init();
	if (psa_status != PSA_SUCCESS) {
		ESP_LOGE(TAG, "PSA crypto initialization failed: %ld", (long)psa_status);
		return ESP_FAIL;
	}

	const esp_partition_t *running = esp_ota_get_running_partition();
	esp_ota_img_states_t ota_state;

	if (running != NULL &&
	    esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
	    ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
		/*
		 * Confirmation is deliberately remote: booting and starting an AP is
		 * not enough evidence that the updated HTTP workflow still works.
		 */
		ota_confirmation_pending_set(true);
		ESP_LOGW(TAG,
			 "running firmware is pending confirmation; POST /flash/confirm within %d seconds",
			 CONFIG_OTA_CONFIRM_TIMEOUT_SECONDS);
		if (xTimerStart(s_confirmation_timer, 0) != pdPASS) {
			return ESP_FAIL;
		}
	}

	return ESP_OK;
}

esp_err_t ota_update_register_http_handlers(httpd_handle_t server)
{
	ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ota_flash_uri));
	ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ota_status_uri));
	ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ota_confirm_uri));
	ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ota_rollback_uri));
	return ESP_OK;
}

esp_err_t ota_update_start_task(void)
{
	BaseType_t created = xTaskCreatePinnedToCore(ota_worker_task,
						    "ota_update",
						    OTA_WORKER_TASK_STACK_SIZE,
						    NULL,
						    OTA_WORKER_TASK_PRIORITY,
						    NULL,
						    tskNO_AFFINITY);

	return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
