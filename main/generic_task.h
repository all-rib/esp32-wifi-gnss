#pragma once

#include "esp_err.h"

esp_err_t generic_stack_init(void);
esp_err_t generic_stack_start_task(void);
void generic_led_client_connected(void);
void generic_led_client_disconnected(void);
void generic_led_packet_consumed(void);
