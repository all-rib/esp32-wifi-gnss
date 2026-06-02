#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define GNSS_NMEA_MAX_SENTENCE_LEN 128
#define GNSS_PUBLICATION_MAX_LEN (GNSS_NMEA_MAX_SENTENCE_LEN * 2)

/*
 * A publication contains the original selected GGA and RMC bytes. Parsing is
 * used only to decide which pair to publish; the HTTP body remains raw NMEA.
 */
typedef struct {
	uint32_t sequence;
	uint16_t len;
	int hdop_milli;
	uint8_t data[GNSS_PUBLICATION_MAX_LEN];
} gnss_publication_t;

esp_err_t gnss_stack_init(void);
esp_err_t gnss_stack_start_tasks(void);
bool gnss_publication_receive(gnss_publication_t *publication, TickType_t ticks_to_wait);

/*
 * A single HTTP writer can temporarily replace UART2 as the parser input.
 * The same checksum, pairing, HDOP selection, timer, and publication path is
 * used for both sources so mock files exercise the real GNSS pipeline.
 */
esp_err_t gnss_input_override_begin(void);
esp_err_t gnss_input_override_feed(const uint8_t *data, size_t len);
void gnss_input_override_end(void);
