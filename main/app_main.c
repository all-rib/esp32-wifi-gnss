/*
 * Application entry point.
 *
 * Each subsystem has an initialization function and one or more FreeRTOS
 * tasks. Keeping resource creation separate from task startup makes ownership
 * explicit and gives future subsystems a small template to follow.
 */

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "app_version.h"
#include "generic_task.h"
#include "gnss.h"
#include "http_stream.h"
#include "ota_update.h"
#include "wifi_ap.h"

static const char *TAG = "app_main";

static void init_nvs(void)
{
	esp_err_t err = nvs_flash_init();

	/*
	 * NVS can be incompatible after partition-table or ESP-IDF changes. Erasing
	 * it is acceptable here because this firmware does not store user data yet.
	 */
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}

	ESP_ERROR_CHECK(err);
}

void app_main(void)
{
	init_nvs();

	/*
	 * Start the LED indicator early so its five-blink startup sequence runs while
	 * the remaining stacks initialize. The GNSS init task performs UART setup and
	 * baud scan later so a disconnected antenna cannot delay Wi-Fi or HTTP.
	 */
	ESP_ERROR_CHECK(generic_stack_init());
	ESP_ERROR_CHECK(generic_stack_start_task());

	ESP_ERROR_CHECK(wifi_ap_stack_init());
	ESP_ERROR_CHECK(gnss_stack_init());
	ESP_ERROR_CHECK(ota_update_stack_init());
	ESP_ERROR_CHECK(http_stream_stack_init());

	/*
	 * Start the OTA worker before SoftAP begins accepting clients. A newly
	 * booted OTA image may be awaiting explicit remote confirmation.
	 */
	ESP_ERROR_CHECK(ota_update_start_task());
	ESP_ERROR_CHECK(wifi_ap_start_task());
	ESP_ERROR_CHECK(gnss_stack_start_tasks());
	ESP_ERROR_CHECK(http_stream_start_tasks());

	ESP_LOGI(TAG, "version:%s", app_version_string());
	ESP_LOGI(TAG, APP_BOOT_NORMAL_MARKER);
}
