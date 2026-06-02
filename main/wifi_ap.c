/*
 * Wi-Fi SoftAP setup and station event processing.
 *
 * Reduced from ESP-IDF's wifi/getting_started/softAP example. The ESP-IDF
 * event callback stays short: it copies station events into a FreeRTOS queue.
 * The application task owns logging and any future connection policy.
 */

#include <stdint.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"

#include "wifi_ap.h"

#define STREAM_AP_IP_A 192
#define STREAM_AP_IP_B 168
#define STREAM_AP_IP_C 1
#define STREAM_AP_IP_D 1

#define WIFI_EVENT_QUEUE_LENGTH 8
#define WIFI_STACK_TASK_STACK_SIZE 3072
#define WIFI_STACK_TASK_PRIORITY (tskIDLE_PRIORITY + 3)

static const char *TAG = "wifi_ap";
static QueueHandle_t s_wifi_event_queue;

typedef enum {
	WIFI_STATION_CONNECTED,
	WIFI_STATION_DISCONNECTED,
} wifi_station_event_type_t;

typedef struct {
	wifi_station_event_type_t type;
	uint8_t mac[6];
	uint8_t aid;
	uint16_t reason;
} wifi_station_event_t;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
			       int32_t event_id, void *event_data)
{
	wifi_station_event_t station_event;

	/*
	 * Do not perform slow work in the ESP-IDF callback. Future station-specific
	 * cleanup belongs in wifi_stack_task(), which runs outside the event loop.
	 */
	if (event_id == WIFI_EVENT_AP_STACONNECTED) {
		wifi_event_ap_staconnected_t *event = event_data;

		station_event.type = WIFI_STATION_CONNECTED;
		memcpy(station_event.mac, event->mac, sizeof(station_event.mac));
		station_event.aid = event->aid;
		station_event.reason = 0;
	} else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
		wifi_event_ap_stadisconnected_t *event = event_data;

		station_event.type = WIFI_STATION_DISCONNECTED;
		memcpy(station_event.mac, event->mac, sizeof(station_event.mac));
		station_event.aid = event->aid;
		station_event.reason = event->reason;
	} else {
		return;
	}

	/*
	 * Dropping an event affects diagnostics, not the Wi-Fi driver's internal
	 * state. Increase this queue if future connection policy makes every event
	 * mandatory.
	 */
	if (xQueueSend(s_wifi_event_queue, &station_event, 0) != pdTRUE) {
		ESP_LOGW(TAG, "station event queue is full");
	}
}

static void wifi_stack_task(void *arg)
{
	wifi_station_event_t station_event;

	while (true) {
		if (xQueueReceive(s_wifi_event_queue, &station_event, portMAX_DELAY) != pdTRUE) {
			continue;
		}

		if (station_event.type == WIFI_STATION_CONNECTED) {
			ESP_LOGI(TAG, "station " MACSTR " joined, AID=%d",
				 MAC2STR(station_event.mac), station_event.aid);
		} else {
			ESP_LOGI(TAG, "station " MACSTR " left, AID=%d, reason=%d",
				 MAC2STR(station_event.mac), station_event.aid,
				 station_event.reason);
		}
	}
}

static void set_softap_ip(esp_netif_t *ap_netif)
{
	esp_netif_ip_info_t ip_info = { 0 };

	IP4_ADDR(&ip_info.ip, STREAM_AP_IP_A, STREAM_AP_IP_B, STREAM_AP_IP_C, STREAM_AP_IP_D);
	IP4_ADDR(&ip_info.gw, STREAM_AP_IP_A, STREAM_AP_IP_B, STREAM_AP_IP_C, STREAM_AP_IP_D);
	IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

	/*
	 * Restart DHCP after changing the access point address so connected
	 * stations receive addresses in the 192.168.1.0/24 network.
	 */
	ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
	ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));
	ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));
}

esp_err_t wifi_ap_stack_init(void)
{
	s_wifi_event_queue = xQueueCreate(WIFI_EVENT_QUEUE_LENGTH,
					 sizeof(wifi_station_event_t));
	if (s_wifi_event_queue == NULL) {
		return ESP_ERR_NO_MEM;
	}

	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
	if (ap_netif == NULL) {
		return ESP_ERR_NO_MEM;
	}
	set_softap_ip(ap_netif);

	wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&config));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
							    ESP_EVENT_ANY_ID,
							    &wifi_event_handler,
							    NULL,
							    NULL));

	wifi_config_t wifi_config = {
		.ap = {
			.ssid = CONFIG_STREAM_WIFI_SSID,
			.ssid_len = strlen(CONFIG_STREAM_WIFI_SSID),
			.channel = CONFIG_STREAM_WIFI_CHANNEL,
			.password = CONFIG_STREAM_WIFI_PASSWORD,
			.max_connection = CONFIG_STREAM_MAX_STA_CONNECTIONS,
			.authmode = WIFI_AUTH_WPA2_PSK,
		},
	};

	/*
	 * An empty password intentionally creates an open AP. Non-empty passwords
	 * should contain at least eight characters or esp_wifi_set_config() fails.
	 */
	if (strlen(CONFIG_STREAM_WIFI_PASSWORD) == 0) {
		wifi_config.ap.authmode = WIFI_AUTH_OPEN;
	}

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());

	ESP_LOGI(TAG, "SoftAP ready: SSID=%s channel=%d IP=192.168.1.1 max_stations=%d",
		 CONFIG_STREAM_WIFI_SSID, CONFIG_STREAM_WIFI_CHANNEL,
		 CONFIG_STREAM_MAX_STA_CONNECTIONS);
	/*
	 * Print connection credentials after startup so a technician can configure
	 * a phone or laptop using only the serial monitor. This intentionally exposes
	 * the AP password in logs; remove this line if logs become externally visible.
	 */
	ESP_LOGI(TAG, "SoftAP credentials: SSID=%s password=%s",
		 CONFIG_STREAM_WIFI_SSID,
		 strlen(CONFIG_STREAM_WIFI_PASSWORD) > 0
			 ? CONFIG_STREAM_WIFI_PASSWORD
			 : "<open network>");
	return ESP_OK;
}

esp_err_t wifi_ap_start_task(void)
{
	BaseType_t created = xTaskCreatePinnedToCore(wifi_stack_task,
						    "wifi_stack",
						    WIFI_STACK_TASK_STACK_SIZE,
						    NULL,
						    WIFI_STACK_TASK_PRIORITY,
						    NULL,
						    tskNO_AFFINITY);

	return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
