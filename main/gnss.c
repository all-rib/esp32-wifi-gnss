/*
 * GNSS UART ingestion, NMEA pairing, and timer-based quality selection.
 *
 * The parser and UART2 baud scan are adapted from the GNSS path in
 * /home/allan/git/personal/usb-serial-mocker/main/main.c. Unlike that bridge,
 * this firmware publishes the best pair on a timer instead of after N pairs.
 *
 * The tasks deliberately communicate through bounded queues:
 *
 *   init task -> UART2 baud lock -> UART2 bytes ----\
 *   HTTP /write override -> uploaded mock bytes -----+-> parsed GGA/RMC candidates
 *                                                    -> timer filter task
 *                                                    -> latest publication queue
 *                                                    -> HTTP dispatcher
 *
 * A slow HTTP client must never cause an unbounded GNSS backlog.
 */

#include <stdbool.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "gnss.h"

#define UART2_PORT UART_NUM_2
#define UART2_TX_PIN GPIO_NUM_17
#define UART2_RX_PIN GPIO_NUM_16
#define UART_DRIVER_RX_BUFFER_SIZE 2048
#define UART_READ_CHUNK_SIZE 512
#define UART_READ_WAIT_MS 20

#define NMEA_PREFIX_LEN 6
#define NMEA_TYPE_OFFSET 3

#define GNSS_CANDIDATE_QUEUE_LENGTH 8
#define GNSS_TIMER_EVENT_QUEUE_LENGTH 1
#define GNSS_FILTER_CONTROL_QUEUE_LENGTH 1
#define GNSS_PUBLICATION_QUEUE_LENGTH 1

#define GNSS_UART_TASK_STACK_SIZE 4096
#define GNSS_INIT_TASK_STACK_SIZE 4096
#define GNSS_FILTER_TASK_STACK_SIZE 3072
#define GNSS_UART_TASK_PRIORITY (tskIDLE_PRIORITY + 3)
#define GNSS_INIT_TASK_PRIORITY (tskIDLE_PRIORITY + 3)
#define GNSS_FILTER_TASK_PRIORITY (tskIDLE_PRIORITY + 4)

typedef struct {
	int baud_rate;
	uint32_t dwell_ms;
} baud_scan_rate_t;

static const baud_scan_rate_t UART2_BAUD_SCAN_RATES[] = {
	{ 4800, 1400 },
	{ 9600, 1100 },
	{ 19200, 700 },
	{ 38400, 400 },
	{ 57600, 300 },
	{ 115200, 250 },
};

#define UART2_BAUD_SCAN_RATE_COUNT \
	(sizeof(UART2_BAUD_SCAN_RATES) / sizeof(UART2_BAUD_SCAN_RATES[0]))

typedef struct {
	bool locked;
	size_t index;
	TickType_t started_at;
} uart2_baud_scan_state_t;

typedef struct {
	bool in_sentence;
	uint16_t sentence_len;
	uint8_t sentence[GNSS_NMEA_MAX_SENTENCE_LEN];
} nmea_probe_state_t;

typedef struct {
	uint16_t gga_len;
	uint8_t gga[GNSS_NMEA_MAX_SENTENCE_LEN];
	uint16_t rmc_len;
	uint8_t rmc[GNSS_NMEA_MAX_SENTENCE_LEN];
	int hdop_milli;
	uint32_t input_epoch;
} gnss_candidate_t;

typedef struct {
	bool in_sentence;
	uint16_t sentence_len;
	uint8_t sentence[GNSS_NMEA_MAX_SENTENCE_LEN];
	bool has_pending_gga;
	uint16_t pending_gga_len;
	uint8_t pending_gga[GNSS_NMEA_MAX_SENTENCE_LEN];
	int pending_gga_hdop_milli;
} nmea_parser_state_t;

typedef enum {
	GNSS_FILTER_FLUSH_OVERRIDE,
} gnss_filter_control_type_t;

typedef struct {
	gnss_filter_control_type_t type;
	uint32_t input_epoch;
} gnss_filter_control_t;

static const char *TAG = "gnss";
static QueueHandle_t s_candidate_queue;
static QueueHandle_t s_timer_event_queue;
static QueueHandle_t s_filter_control_queue;
static QueueHandle_t s_publication_queue;
static QueueSetHandle_t s_filter_queue_set;
static TimerHandle_t s_selection_timer;
static SemaphoreHandle_t s_parser_mutex;
static SemaphoreHandle_t s_override_flush_done;
static uart2_baud_scan_state_t s_baud_scan_state;
static nmea_probe_state_t s_probe_state;
static nmea_parser_state_t s_parser_state;
static bool s_input_override_active;
static bool s_input_override_closing;
static uint32_t s_input_epoch = 1;

static inline int nmea_hex_digit_to_value(uint8_t digit)
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

static bool nmea_checksum_ok(const uint8_t *sentence, int len)
{
	while (len > 0 && (sentence[len - 1] == '\r' || sentence[len - 1] == '\n')) {
		len--;
	}

	/* Short or non-NMEA-looking buffers cannot carry "$...*hh". */
	if (len < 10 || sentence[0] != '$') {
		return false;
	}

	const uint8_t *star = memchr(sentence + 1, '*', (size_t)(len - 1));

	/* The '*' must be followed by exactly two hexadecimal checksum digits. */
	if (star == NULL || (sentence + len) - star != 3) {
		return false;
	}

	const int high = nmea_hex_digit_to_value(star[1]);
	const int low = nmea_hex_digit_to_value(star[2]);
	if (high < 0 || low < 0) {
		return false;
	}

	/* NMEA checksum is XOR of payload bytes after '$' and before '*'. */
	uint8_t checksum = 0;
	for (const uint8_t *ptr = sentence + 1; ptr < star; ptr++) {
		checksum ^= *ptr;
	}

	return checksum == (uint8_t)((high << 4) | low);
}

static inline void nmea_probe_reset(nmea_probe_state_t *state)
{
	state->in_sentence = false;
	state->sentence_len = 0;
}

static inline bool nmea_sentence_looks_valid(const uint8_t *sentence, int len)
{
	if (len < NMEA_PREFIX_LEN || sentence[0] != '$') {
		return false;
	}

	const uint8_t *type = sentence + NMEA_TYPE_OFFSET;
	return (type[0] == 'G' && type[1] == 'G' && type[2] == 'A') ||
	       (type[0] == 'R' && type[1] == 'M' && type[2] == 'C') ||
	       (type[0] == 'G' && type[1] == 'L' && type[2] == 'L') ||
	       (type[0] == 'V' && type[1] == 'T' && type[2] == 'G') ||
	       (type[0] == 'G' && type[1] == 'S' && type[2] == 'A') ||
	       (type[0] == 'G' && type[1] == 'S' && type[2] == 'V');
}

static bool nmea_probe_feed(nmea_probe_state_t *state, const uint8_t *data, int len)
{
	for (int index = 0; index < len; index++) {
		const uint8_t byte = data[index];

		/* '$' restarts assembly even if an earlier sentence was malformed. */
		if (byte == '$') {
			state->sentence[0] = byte;
			state->sentence_len = 1;
			state->in_sentence = true;
			continue;
		}

		if (!state->in_sentence) {
			continue;
		}

		if (state->sentence_len >= GNSS_NMEA_MAX_SENTENCE_LEN) {
			nmea_probe_reset(state);
			continue;
		}

		state->sentence[state->sentence_len++] = byte;
		if (byte == '\n') {
			const bool valid = nmea_sentence_looks_valid(state->sentence,
								    state->sentence_len) &&
					   nmea_checksum_ok(state->sentence,
							    state->sentence_len);
			nmea_probe_reset(state);
			if (valid) {
				return true;
			}
		}
	}

	return false;
}

static bool nmea_get_field_range(const uint8_t *sentence, int len, int field_number,
				 const uint8_t **field_start, int *field_len)
{
	while (len > 0 && (sentence[len - 1] == '\r' || sentence[len - 1] == '\n')) {
		len--;
	}

	const uint8_t *end = sentence + len;
	const uint8_t *cursor = memchr(sentence, ',', (size_t)len);
	int current_field = 1;

	if (cursor == NULL) {
		return false;
	}

	const uint8_t *start = cursor + 1;
	for (cursor = start; cursor <= end; cursor++) {
		if (cursor == end || *cursor == ',' || *cursor == '*') {
			if (current_field == field_number) {
				*field_start = start;
				*field_len = (int)(cursor - start);
				return *field_len > 0;
			}

			if (cursor == end || *cursor == '*') {
				return false;
			}

			current_field++;
			start = cursor + 1;
		}
	}

	return false;
}

static bool nmea_parse_decimal_milli(const uint8_t *field, int len, int *value)
{
	int integer_part = 0;
	int fractional_part = 0;
	int fractional_digits = 0;
	bool seen_dot = false;
	bool seen_digit = false;

	/* Compare HDOP as integer thousandths: "1.23" becomes 1230. */
	for (int index = 0; index < len; index++) {
		const uint8_t byte = field[index];

		if (byte == '.') {
			if (seen_dot) {
				return false;
			}
			seen_dot = true;
			continue;
		}

		if (byte < '0' || byte > '9') {
			return false;
		}

		seen_digit = true;
		if (!seen_dot) {
			integer_part = (integer_part * 10) + (byte - '0');
		} else if (fractional_digits < 3) {
			fractional_part = (fractional_part * 10) + (byte - '0');
			fractional_digits++;
		}
	}

	if (!seen_digit) {
		return false;
	}

	while (fractional_digits < 3) {
		fractional_part *= 10;
		fractional_digits++;
	}

	*value = (integer_part * 1000) + fractional_part;
	return true;
}

static bool nmea_parse_hdop_milli(const uint8_t *sentence, int len, int *hdop_milli)
{
	const uint8_t *field = NULL;
	int field_len = 0;

	/*
	 * HDOP is GGA field 8. This keeps the reference firmware's quality rule.
	 * Observation: GGA fix-quality field 6 is not rejected yet. Add that gate
	 * if a receiver emits checksum-valid GGA lines while it has no fix.
	 */
	if (len < NMEA_PREFIX_LEN ||
	    sentence[0] != '$' ||
	    memcmp(sentence + NMEA_TYPE_OFFSET, "GGA", 3) != 0) {
		return false;
	}

	if (!nmea_get_field_range(sentence, len, 8, &field, &field_len)) {
		return false;
	}

	return nmea_parse_decimal_milli(field, field_len, hdop_milli);
}

static void gnss_candidate_send(const gnss_candidate_t *candidate)
{
	/*
	 * The filter task has a higher priority than the UART task and should drain
	 * this bounded queue promptly. Do not remove old items from here: members of
	 * a FreeRTOS queue set must be consumed by the task that selected them, one
	 * item per readiness event, or the queue set can retain stale markers.
	 */
	if (xQueueSend(s_candidate_queue, candidate, 0) != pdTRUE) {
		ESP_LOGW(TAG, "candidate queue is full; newest pair was dropped");
	}
}

static void nmea_parser_complete_sentence(nmea_parser_state_t *state)
{
	int hdop_milli = 0;

	/* Corrupted lines must not modify GGA/RMC pairing state. */
	if (!nmea_checksum_ok(state->sentence, state->sentence_len)) {
		return;
	}

	if (nmea_parse_hdop_milli(state->sentence, state->sentence_len, &hdop_milli)) {
		memcpy(state->pending_gga, state->sentence, (size_t)state->sentence_len);
		state->pending_gga_len = state->sentence_len;
		state->pending_gga_hdop_milli = hdop_milli;
		state->has_pending_gga = true;
		return;
	}

	/*
	 * The following RMC completes the candidate. Keeping both original lines
	 * allows the HTTP stream to output bytes without reformatting NMEA.
	 */
	if (state->sentence_len < NMEA_PREFIX_LEN ||
	    memcmp(state->sentence + NMEA_TYPE_OFFSET, "RMC", 3) != 0 ||
	    !state->has_pending_gga) {
		return;
	}

	gnss_candidate_t candidate = {
		.gga_len = state->pending_gga_len,
		.rmc_len = state->sentence_len,
		.hdop_milli = state->pending_gga_hdop_milli,
		/*
		 * nmea_parser_feed() is always called while s_parser_mutex is held.
		 * Capturing the source generation lets the filter discard candidates
		 * that were queued immediately before a UART/HTTP source switch.
		 */
		.input_epoch = s_input_epoch,
	};
	memcpy(candidate.gga, state->pending_gga, candidate.gga_len);
	memcpy(candidate.rmc, state->sentence, candidate.rmc_len);
	gnss_candidate_send(&candidate);

	state->has_pending_gga = false;
	state->pending_gga_len = 0;
}

static void nmea_parser_feed(nmea_parser_state_t *state, const uint8_t *data, int len)
{
	for (int index = 0; index < len; index++) {
		const uint8_t byte = data[index];

		if (byte == '$') {
			state->sentence[0] = byte;
			state->sentence_len = 1;
			state->in_sentence = true;
			continue;
		}

		if (!state->in_sentence) {
			continue;
		}

		/* Ignore malformed overlong lines until the next '$'. */
		if (state->sentence_len >= GNSS_NMEA_MAX_SENTENCE_LEN) {
			state->in_sentence = false;
			state->sentence_len = 0;
			continue;
		}

		state->sentence[state->sentence_len++] = byte;
		if (byte == '\n') {
			nmea_parser_complete_sentence(state);
			state->in_sentence = false;
			state->sentence_len = 0;
		}
	}
}

static uint32_t gnss_input_epoch_get(void)
{
	uint32_t input_epoch;

	xSemaphoreTake(s_parser_mutex, portMAX_DELAY);
	input_epoch = s_input_epoch;
	xSemaphoreGive(s_parser_mutex);

	return input_epoch;
}

static void gnss_input_epoch_advance(void)
{
	s_input_epoch++;

	/* Reserve zero as an easy-to-recognize uninitialized value in diagnostics. */
	if (s_input_epoch == 0) {
		s_input_epoch++;
	}
}

static void gnss_uart_input_feed(const uint8_t *data, int len)
{
	xSemaphoreTake(s_parser_mutex, portMAX_DELAY);

	/*
	 * UART2 continues to be drained while an HTTP mock stream is active. This
	 * prevents the hardware driver buffer from overflowing, but none of those
	 * bytes enter the parser or affect the selected HTTP output.
	 */
	if (!s_input_override_active) {
		nmea_parser_feed(&s_parser_state, data, len);
	}

	xSemaphoreGive(s_parser_mutex);
}

static void uart2_baud_set(size_t index)
{
	s_baud_scan_state.index = index % UART2_BAUD_SCAN_RATE_COUNT;
	s_baud_scan_state.started_at = xTaskGetTickCount();
	nmea_probe_reset(&s_probe_state);

	ESP_ERROR_CHECK(uart_set_baudrate(
		UART2_PORT,
		(uint32_t)UART2_BAUD_SCAN_RATES[s_baud_scan_state.index].baud_rate));
	ESP_ERROR_CHECK(uart_flush_input(UART2_PORT));

	ESP_LOGI(TAG, "probing UART2 at %d baud",
		 UART2_BAUD_SCAN_RATES[s_baud_scan_state.index].baud_rate);
}

static void uart2_baud_scan_service(void)
{
	if (s_baud_scan_state.locked) {
		return;
	}

	const uint32_t dwell_ms = UART2_BAUD_SCAN_RATES[s_baud_scan_state.index].dwell_ms;
	if ((xTaskGetTickCount() - s_baud_scan_state.started_at) >= pdMS_TO_TICKS(dwell_ms)) {
		uart2_baud_set(s_baud_scan_state.index + 1);
	}
}

static void setup_uart2(void)
{
	const uart_config_t uart_config = {
		.baud_rate = UART2_BAUD_SCAN_RATES[0].baud_rate,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};

	ESP_ERROR_CHECK(uart_driver_install(UART2_PORT,
					    UART_DRIVER_RX_BUFFER_SIZE,
					    0,
					    0,
					    NULL,
					    0));
	ESP_ERROR_CHECK(uart_param_config(UART2_PORT, &uart_config));
	ESP_ERROR_CHECK(uart_set_pin(UART2_PORT,
				     UART2_TX_PIN,
				     UART2_RX_PIN,
				     UART_PIN_NO_CHANGE,
				     UART_PIN_NO_CHANGE));
	ESP_ERROR_CHECK(uart_flush_input(UART2_PORT));
}

static void gnss_uart_task(void *arg)
{
	uint8_t buffer[UART_READ_CHUNK_SIZE];

	while (true) {
		const int read_len = uart_read_bytes(UART2_PORT,
						    buffer,
						    sizeof(buffer),
						    pdMS_TO_TICKS(UART_READ_WAIT_MS));

		if (read_len > 0) {
			gnss_uart_input_feed(buffer, read_len);
		}

		/*
		 * Observation: the initialization task found the baud rate before this
		 * steady-state reader started. If antennas can change baud at runtime,
		 * add a silence or checksum-failure timeout that launches a rescan.
		 */
	}
}

static void gnss_init_task(void *arg)
{
	uint8_t buffer[UART_READ_CHUNK_SIZE];

	/*
	 * Hardware setup and baud probing live in their own task because an absent
	 * antenna must not block SoftAP or HTTP initialization. Only this task reads
	 * UART2 until a checksum-valid NMEA sentence proves the active baud rate.
	 */
	setup_uart2();
	xSemaphoreTake(s_parser_mutex, portMAX_DELAY);
	memset(&s_parser_state, 0, sizeof(s_parser_state));
	xSemaphoreGive(s_parser_mutex);
	nmea_probe_reset(&s_probe_state);
	s_baud_scan_state.locked = false;
	uart2_baud_set(0);

	while (!s_baud_scan_state.locked) {
		const int read_len = uart_read_bytes(UART2_PORT,
						    buffer,
						    sizeof(buffer),
						    pdMS_TO_TICKS(UART_READ_WAIT_MS));

		if (read_len > 0 && nmea_probe_feed(&s_probe_state, buffer, read_len)) {
			s_baud_scan_state.locked = true;

			/*
			 * Re-feed the chunk that established the baud rate: it contains a
			 * valid line and may also contain the matching line or later data.
			 * The input wrapper intentionally discards it if /write currently
			 * owns the parser.
			 */
			gnss_uart_input_feed(buffer, read_len);
			ESP_LOGI(TAG, "UART2 locked at %d baud",
				 UART2_BAUD_SCAN_RATES[s_baud_scan_state.index].baud_rate);
			break;
		}

		uart2_baud_scan_service();
	}

	BaseType_t created = xTaskCreatePinnedToCore(gnss_uart_task,
						    "gnss_uart",
						    GNSS_UART_TASK_STACK_SIZE,
						    NULL,
						    GNSS_UART_TASK_PRIORITY,
						    NULL,
						    tskNO_AFFINITY);
	ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

	/* Initialization has transferred UART2 ownership to the steady-state task. */
	vTaskDelete(NULL);
}

static void selection_timer_callback(TimerHandle_t timer)
{
	const uint8_t expired = 1;

	/*
	 * FreeRTOS runs callbacks in its timer service task. Keep this non-blocking:
	 * the GNSS filter task performs copies and queue publication later.
	 */
	xQueueOverwrite(s_timer_event_queue, &expired);
}

static void consider_candidate(gnss_candidate_t *best_candidate,
			       bool *has_best_candidate,
			       const gnss_candidate_t *candidate)
{
	const uint32_t input_epoch = gnss_input_epoch_get();

	/*
	 * Source switches invalidate queued work without mutating a queue-set member
	 * from a second task. Removing queue items here would leave stale queue-set
	 * readiness markers and break future selection.
	 */
	if (candidate->input_epoch != input_epoch) {
		return;
	}

	if (*has_best_candidate && best_candidate->input_epoch != input_epoch) {
		*has_best_candidate = false;
	}

	if (!*has_best_candidate || candidate->hdop_milli < best_candidate->hdop_milli) {
		*best_candidate = *candidate;
		*has_best_candidate = true;
	}
}

static void publish_best_candidate(const gnss_candidate_t *best_candidate,
				   bool *has_best_candidate,
				   uint32_t *next_sequence)
{
	const uint32_t input_epoch = gnss_input_epoch_get();

	if (*has_best_candidate && best_candidate->input_epoch != input_epoch) {
		*has_best_candidate = false;
	}

	if (!*has_best_candidate) {
		/*
		 * No stale repeat is sent. Consumers wait until the antenna produces a
		 * new valid pair, which keeps silence distinguishable from fresh data.
		 */
		return;
	}

	gnss_publication_t publication = {
		.sequence = (*next_sequence)++,
		.len = best_candidate->gga_len + best_candidate->rmc_len,
		.hdop_milli = best_candidate->hdop_milli,
	};
	memcpy(publication.data, best_candidate->gga, best_candidate->gga_len);
	memcpy(publication.data + best_candidate->gga_len,
	       best_candidate->rmc,
	       best_candidate->rmc_len);

	/*
	 * The queue length is intentionally one. If HTTP dispatch is momentarily
	 * late, overwrite an obsolete publication instead of building latency.
	 */
	xQueueOverwrite(s_publication_queue, &publication);
	*has_best_candidate = false;

	ESP_LOGD(TAG, "published GNSS seq=%lu hdop_milli=%d bytes=%u",
		 (unsigned long)publication.sequence,
		 publication.hdop_milli,
		 publication.len);
}

static void gnss_filter_task(void *arg)
{
	gnss_candidate_t best_candidate;
	bool has_best_candidate = false;
	uint32_t next_sequence = 1;

	while (true) {
		QueueSetMemberHandle_t ready = xQueueSelectFromSet(s_filter_queue_set,
								   portMAX_DELAY);

		if (ready == s_candidate_queue) {
			gnss_candidate_t candidate;

			/*
			 * Receive exactly one item for this queue-set readiness event.
			 * A later select consumes the marker for the next queued item.
			 */
			if (xQueueReceive(s_candidate_queue, &candidate, 0) == pdTRUE) {
				consider_candidate(&best_candidate, &has_best_candidate, &candidate);
			}
		} else if (ready == s_timer_event_queue) {
			uint8_t expired;

			xQueueReceive(s_timer_event_queue, &expired, 0);

			/*
			 * Queue-set events retain send order. Candidates queued before this
			 * marker were already considered; later candidates belong to the
			 * next fixed-rate quality window.
			 */
			publish_best_candidate(&best_candidate,
					       &has_best_candidate,
					       &next_sequence);
		} else if (ready == s_filter_control_queue) {
			gnss_filter_control_t control;

			if (xQueueReceive(s_filter_control_queue, &control, 0) != pdTRUE) {
				continue;
			}

			/*
			 * A short mock file can finish before the periodic timer fires.
			 * Flush its best candidate before parser ownership returns to UART.
			 */
			if (control.type == GNSS_FILTER_FLUSH_OVERRIDE &&
			    control.input_epoch == gnss_input_epoch_get()) {
				publish_best_candidate(&best_candidate,
						       &has_best_candidate,
						       &next_sequence);
			}
			xSemaphoreGive(s_override_flush_done);
		}
	}
}

esp_err_t gnss_stack_init(void)
{
	s_candidate_queue = xQueueCreate(GNSS_CANDIDATE_QUEUE_LENGTH,
					 sizeof(gnss_candidate_t));
	s_timer_event_queue = xQueueCreate(GNSS_TIMER_EVENT_QUEUE_LENGTH,
					  sizeof(uint8_t));
	s_filter_control_queue = xQueueCreate(GNSS_FILTER_CONTROL_QUEUE_LENGTH,
					     sizeof(gnss_filter_control_t));
	s_publication_queue = xQueueCreate(GNSS_PUBLICATION_QUEUE_LENGTH,
					  sizeof(gnss_publication_t));
	s_filter_queue_set = xQueueCreateSet(GNSS_CANDIDATE_QUEUE_LENGTH +
					    GNSS_TIMER_EVENT_QUEUE_LENGTH +
					    GNSS_FILTER_CONTROL_QUEUE_LENGTH);
	s_parser_mutex = xSemaphoreCreateMutex();
	s_override_flush_done = xSemaphoreCreateBinary();

	if (s_candidate_queue == NULL ||
	    s_timer_event_queue == NULL ||
	    s_filter_control_queue == NULL ||
	    s_publication_queue == NULL ||
	    s_filter_queue_set == NULL ||
	    s_parser_mutex == NULL ||
	    s_override_flush_done == NULL) {
		return ESP_ERR_NO_MEM;
	}

	if (xQueueAddToSet(s_candidate_queue, s_filter_queue_set) != pdPASS ||
	    xQueueAddToSet(s_timer_event_queue, s_filter_queue_set) != pdPASS ||
	    xQueueAddToSet(s_filter_control_queue, s_filter_queue_set) != pdPASS) {
		return ESP_FAIL;
	}

	s_selection_timer = xTimerCreate("gnss_window",
					 pdMS_TO_TICKS(CONFIG_GNSS_SELECTION_WINDOW_MS),
					 pdTRUE,
					 NULL,
					 selection_timer_callback);
	if (s_selection_timer == NULL) {
		return ESP_ERR_NO_MEM;
	}

	return ESP_OK;
}

esp_err_t gnss_stack_start_tasks(void)
{
	BaseType_t created = xTaskCreatePinnedToCore(gnss_filter_task,
						    "gnss_filter",
						    GNSS_FILTER_TASK_STACK_SIZE,
						    NULL,
						    GNSS_FILTER_TASK_PRIORITY,
						    NULL,
						    tskNO_AFFINITY);
	if (created != pdPASS) {
		return ESP_ERR_NO_MEM;
	}

	/*
	 * Start timed quality windows before UART detection. A /write mock stream
	 * must work even when there is no physical antenna connected.
	 */
	if (xTimerStart(s_selection_timer, 0) != pdPASS) {
		return ESP_FAIL;
	}

	created = xTaskCreatePinnedToCore(gnss_init_task,
					 "gnss_init",
					 GNSS_INIT_TASK_STACK_SIZE,
					 NULL,
					 GNSS_INIT_TASK_PRIORITY,
					 NULL,
					 tskNO_AFFINITY);
	if (created != pdPASS) {
		return ESP_ERR_NO_MEM;
	}

	/*
	 * gnss_init_task() launches gnss_uart_task() only after baud detection
	 * succeeds. The independent timer already serves /write mock input.
	 */
	return ESP_OK;
}

bool gnss_publication_receive(gnss_publication_t *publication, TickType_t ticks_to_wait)
{
	return xQueueReceive(s_publication_queue, publication, ticks_to_wait) == pdTRUE;
}

esp_err_t gnss_input_override_begin(void)
{
	xSemaphoreTake(s_parser_mutex, portMAX_DELAY);

	if (s_input_override_active) {
		xSemaphoreGive(s_parser_mutex);
		return ESP_ERR_INVALID_STATE;
	}

	/*
	 * Reset partial sentence state at each source boundary. Joining half a UART
	 * sentence with half an uploaded sentence could otherwise create misleading
	 * but occasionally checksum-valid input.
	 */
	s_input_override_active = true;
	s_input_override_closing = false;
	gnss_input_epoch_advance();
	memset(&s_parser_state, 0, sizeof(s_parser_state));
	xSemaphoreGive(s_parser_mutex);

	ESP_LOGI(TAG, "HTTP GNSS input override started");
	return ESP_OK;
}

esp_err_t gnss_input_override_feed(const uint8_t *data, size_t len)
{
	if (data == NULL && len > 0) {
		return ESP_ERR_INVALID_ARG;
	}

	xSemaphoreTake(s_parser_mutex, portMAX_DELAY);

	if (!s_input_override_active || s_input_override_closing) {
		xSemaphoreGive(s_parser_mutex);
		return ESP_ERR_INVALID_STATE;
	}

	/*
	 * HTTP feeds small chunks today. Keep the public API correct for larger
	 * callers too because nmea_parser_feed() uses an int-sized byte count.
	 */
	while (len > 0) {
		const int chunk_len = len > INT_MAX ? INT_MAX : (int)len;

		nmea_parser_feed(&s_parser_state, data, chunk_len);
		data += chunk_len;
		len -= (size_t)chunk_len;
	}

	xSemaphoreGive(s_parser_mutex);
	return ESP_OK;
}

void gnss_input_override_end(void)
{
	bool should_flush = false;
	gnss_filter_control_t control;

	xSemaphoreTake(s_parser_mutex, portMAX_DELAY);

	if (s_input_override_active && !s_input_override_closing) {
		/*
		 * Keep UART ignored until the filter confirms that all uploaded
		 * candidates ahead of this control marker have been considered.
		 */
		s_input_override_closing = true;
		control.type = GNSS_FILTER_FLUSH_OVERRIDE;
		control.input_epoch = s_input_epoch;
		should_flush = true;
	}

	xSemaphoreGive(s_parser_mutex);

	if (!should_flush) {
		return;
	}

	/* Discard any impossible stale acknowledgement before issuing this flush. */
	xSemaphoreTake(s_override_flush_done, 0);
	xQueueSend(s_filter_control_queue, &control, portMAX_DELAY);
	xSemaphoreTake(s_override_flush_done, portMAX_DELAY);

	xSemaphoreTake(s_parser_mutex, portMAX_DELAY);
	s_input_override_active = false;
	s_input_override_closing = false;
	gnss_input_epoch_advance();
	memset(&s_parser_state, 0, sizeof(s_parser_state));
	xSemaphoreGive(s_parser_mutex);

	ESP_LOGI(TAG, "HTTP GNSS input override ended");
}
