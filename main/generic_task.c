/*
 * Minimal event-driven LED template subsystem.
 *
 * Copy this init/task/API shape for later independent application work. GPIO2
 * blinks five times during startup, remains off without HTTP clients, and
 * toggles once after each GNSS packet is successfully sent to a stream client.
 */

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "generic_task.h"

#define GENERIC_LED_GPIO GPIO_NUM_2
#define GENERIC_LED_STARTUP_BLINK_COUNT 5
#define GENERIC_LED_STARTUP_TOGGLE_PERIOD_MS 200
#define GENERIC_TASK_STACK_SIZE 2048
#define GENERIC_TASK_PRIORITY (tskIDLE_PRIORITY + 1)

static const char *TAG = "generic_led";
static portMUX_TYPE s_led_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_led_task_handle;
static uint32_t s_connected_client_count;
static uint32_t s_pending_packet_toggles;

static void generic_led_wake_task(void)
{
	TaskHandle_t task_handle;

	/*
	 * Copy the handle while holding the same lock used during startup. Public
	 * notification functions may be called before this task starts running.
	 */
	taskENTER_CRITICAL(&s_led_lock);
	task_handle = s_led_task_handle;
	taskEXIT_CRITICAL(&s_led_lock);

	if (task_handle != NULL) {
		xTaskNotifyGive(task_handle);
	}
}

void generic_led_client_connected(void)
{
	taskENTER_CRITICAL(&s_led_lock);
	s_connected_client_count++;
	taskEXIT_CRITICAL(&s_led_lock);

	generic_led_wake_task();
}

void generic_led_client_disconnected(void)
{
	taskENTER_CRITICAL(&s_led_lock);
	if (s_connected_client_count > 0) {
		s_connected_client_count--;
	}
	taskEXIT_CRITICAL(&s_led_lock);

	generic_led_wake_task();
}

void generic_led_packet_consumed(void)
{
	taskENTER_CRITICAL(&s_led_lock);
	if (s_connected_client_count > 0 && s_pending_packet_toggles < UINT32_MAX) {
		/*
		 * Saturating avoids counter wrap if a future data source produces bytes
		 * far faster than the low-priority indicator task can consume events.
		 */
		s_pending_packet_toggles++;
	}
	taskEXIT_CRITICAL(&s_led_lock);

	generic_led_wake_task();
}

static void generic_task(void *arg)
{
	bool led_on = false;

	/*
	 * Visible power-on indication: five complete blink cycles, then off.
	 * HTTP events may accumulate during this short animation and are processed
	 * immediately afterward.
	 */
	for (int blink = 0; blink < GENERIC_LED_STARTUP_BLINK_COUNT; blink++) {
		ESP_ERROR_CHECK(gpio_set_level(GENERIC_LED_GPIO, 1));
		vTaskDelay(pdMS_TO_TICKS(GENERIC_LED_STARTUP_TOGGLE_PERIOD_MS));
		ESP_ERROR_CHECK(gpio_set_level(GENERIC_LED_GPIO, 0));
		vTaskDelay(pdMS_TO_TICKS(GENERIC_LED_STARTUP_TOGGLE_PERIOD_MS));
	}

	while (true) {
		uint32_t connected_client_count;
		uint32_t packet_toggles;

		/*
		 * Notifications are only wakeups. The protected counters are the source
		 * of truth, so multiple HTTP worker events may safely coalesce here.
		 */
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

		taskENTER_CRITICAL(&s_led_lock);
		connected_client_count = s_connected_client_count;
		packet_toggles = s_pending_packet_toggles;
		s_pending_packet_toggles = 0;
		taskEXIT_CRITICAL(&s_led_lock);

		if (connected_client_count == 0) {
			/*
			 * Disconnecting the last stream client always wins over queued
			 * toggles. The idle indication is a stable off LED.
			 */
			led_on = false;
			ESP_ERROR_CHECK(gpio_set_level(GENERIC_LED_GPIO, 0));
			continue;
		}

		for (uint32_t toggle = 0; toggle < packet_toggles; toggle++) {
			/*
			 * One successfully delivered HTTP chunk causes one state change.
			 * With two clients, one GNSS publication can cause two quick toggles
			 * because each client consumes its own copy of the newest packet.
			 */
			led_on = !led_on;
			ESP_ERROR_CHECK(gpio_set_level(GENERIC_LED_GPIO, led_on));
		}
	}
}

esp_err_t generic_stack_init(void)
{
	ESP_ERROR_CHECK(gpio_reset_pin(GENERIC_LED_GPIO));
	ESP_ERROR_CHECK(gpio_set_direction(GENERIC_LED_GPIO, GPIO_MODE_OUTPUT));
	ESP_ERROR_CHECK(gpio_set_level(GENERIC_LED_GPIO, 0));
	return ESP_OK;
}

esp_err_t generic_stack_start_task(void)
{
	BaseType_t created = xTaskCreatePinnedToCore(generic_task,
						    "generic",
						    GENERIC_TASK_STACK_SIZE,
						    NULL,
						    GENERIC_TASK_PRIORITY,
						    &s_led_task_handle,
						    tskNO_AFFINITY);

	if (created != pdPASS) {
		ESP_LOGE(TAG, "failed to create LED indicator task");
	}

	return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
