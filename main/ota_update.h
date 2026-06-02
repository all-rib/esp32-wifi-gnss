#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/*
 * HTTP OTA is intentionally isolated from the GNSS streams. The implementation
 * is reduced from ESP-IDF's system/ota/native_ota_example and registers:
 *
 *   POST /flash
 *   GET  /flash/status
 *   POST /flash/confirm
 *   POST /flash/rollback
 */
esp_err_t ota_update_stack_init(void);
esp_err_t ota_update_register_http_handlers(httpd_handle_t server);
esp_err_t ota_update_start_task(void);
