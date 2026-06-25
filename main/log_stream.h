#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/*
 * HTTP tail of the same log output `idf.py monitor` prints over serial.
 * Registers:
 *
 *   GET /log
 */
esp_err_t log_stream_stack_init(void);
esp_err_t log_stream_register_http_handlers(httpd_handle_t server);
esp_err_t log_stream_start_task(void);
