SHELL := /usr/bin/bash
.ONESHELL:
.SHELLFLAGS := -e -o pipefail -c

# Override these on the command line when using another ESP-IDF installation,
# build directory, serial device, or baud rate:
#
#   make flash PORT=/dev/ttyUSB1
#   make monitor BAUD=115200
IDF_ACTIVATE ?= $(HOME)/.espressif/tools/activate_idf_v6.0.1.sh
BUILD_DIR ?= build
TEST_BUILD_ROOT ?= $(BUILD_DIR)/hardware-tests
TEST_BASE_URL ?= http://192.168.1.1
TEST_WIFI_SSID ?= AgroLine-GNSS
TEST_WIFI_PASSWORD ?= agroline123
TEST_WIFI_IFACE ?= wlan0
TEST_PROGRESS ?= auto
PORT ?= /dev/ttyUSB0
BAUD ?=

ifneq ($(strip $(BAUD)),)
SERIAL_ENV := ESPPORT="$(PORT)" ESPBAUD="$(BAUD)"
else
SERIAL_ENV := ESPPORT="$(PORT)"
endif

ifeq ($(origin PORT), file)
TEST_PORT_ARG :=
else
TEST_PORT_ARG := --port "$(PORT)"
endif

ifneq ($(strip $(BAUD)),)
TEST_BAUD_ARG := --baud "$(BAUD)"
else
TEST_BAUD_ARG :=
endif

TEST_ENV := ESP_TEST_BASE_URL="$(TEST_BASE_URL)" ESP_TEST_WIFI_SSID="$(TEST_WIFI_SSID)" ESP_TEST_WIFI_PASSWORD="$(TEST_WIFI_PASSWORD)" ESP_TEST_WIFI_IFACE="$(TEST_WIFI_IFACE)" ESP_TEST_PROGRESS="$(TEST_PROGRESS)"

.PHONY: help configure build flash monitor flash-monitor factory-reset menuconfig size test-hardware-builds test-hardware-boot test-hardware-manual-wifi test-hardware test-harware

help:
	@printf '%s\n' \
		'make build                  Configure when needed and compile firmware' \
		'make flash                  Build and flash using PORT=/dev/ttyUSB0' \
		'make monitor                Open the interactive serial monitor' \
		'make flash-monitor          Flash, then open the serial monitor' \
		'make factory-reset          Erase OTA selection data and boot factory firmware' \
		'make test-hardware-builds   Build factory-test and OTA test firmware variants only' \
		'make test-hardware-boot     Flash factory-test and verify first UART boot only' \
		'make test-hardware-manual-wifi Full test, but assume host Wi-Fi is already connected' \
		'make test-hardware          Flash over UART, verify logs, OTA, and GNSS paths' \
		'make menuconfig             Open ESP-IDF configuration UI' \
		'make size                   Print firmware size information' \
		'' \
		'Overrides:' \
		'  make flash PORT=/dev/ttyUSB1' \
		'  make monitor BAUD=115200' \
		'  make test-hardware PORT=/dev/ttyUSB1 TEST_BASE_URL=http://192.168.1.1'

configure: $(BUILD_DIR)/build.ninja

$(BUILD_DIR)/build.ninja: CMakeLists.txt main/CMakeLists.txt main/Kconfig.projbuild
	source "$(IDF_ACTIVATE)"
	cmake -S . -B "$(BUILD_DIR)" -G Ninja

build: configure
	source "$(IDF_ACTIVATE)"
	ninja -C "$(BUILD_DIR)"

flash: configure
	source "$(IDF_ACTIVATE)"
	$(SERIAL_ENV) ninja -C "$(BUILD_DIR)" flash

monitor: configure
	source "$(IDF_ACTIVATE)"
	$(SERIAL_ENV) ninja -C "$(BUILD_DIR)" serial-monitor

flash-monitor: flash
	source "$(IDF_ACTIVATE)"
	$(SERIAL_ENV) ninja -C "$(BUILD_DIR)" serial-monitor

# This keeps the factory application intact and only clears ESP-IDF's OTA
# selection state. Use it over USB when a remotely uploaded image misbehaves.
factory-reset: configure
	source "$(IDF_ACTIVATE)"
	$(SERIAL_ENV) ninja -C "$(BUILD_DIR)" erase-otadata

menuconfig: configure
	source "$(IDF_ACTIVATE)"
	ninja -C "$(BUILD_DIR)" menuconfig

size: configure
	source "$(IDF_ACTIVATE)"
	ninja -C "$(BUILD_DIR)" size

test-hardware-builds:
	@mkdir -p "$(TEST_BUILD_ROOT)/logs"
	source "$(IDF_ACTIVATE)" > "$(TEST_BUILD_ROOT)/logs/idf-activate.log" 2>&1
	$(TEST_ENV) python3 tools/hardware_ota_test.py --build-root "$(TEST_BUILD_ROOT)" --build-only

test-hardware-boot:
	@mkdir -p "$(TEST_BUILD_ROOT)/logs"
	source "$(IDF_ACTIVATE)" > "$(TEST_BUILD_ROOT)/logs/idf-activate.log" 2>&1
	$(TEST_ENV) python3 tools/hardware_ota_test.py --build-root "$(TEST_BUILD_ROOT)" --skip-ota $(TEST_PORT_ARG) $(TEST_BAUD_ARG)

test-hardware-manual-wifi:
	@mkdir -p "$(TEST_BUILD_ROOT)/logs"
	source "$(IDF_ACTIVATE)" > "$(TEST_BUILD_ROOT)/logs/idf-activate.log" 2>&1
	$(TEST_ENV) python3 tools/hardware_ota_test.py --build-root "$(TEST_BUILD_ROOT)" --no-manage-wifi $(TEST_PORT_ARG) $(TEST_BAUD_ARG)

test-hardware:
	@mkdir -p "$(TEST_BUILD_ROOT)/logs"
	source "$(IDF_ACTIVATE)" > "$(TEST_BUILD_ROOT)/logs/idf-activate.log" 2>&1
	$(TEST_ENV) python3 tools/hardware_ota_test.py --build-root "$(TEST_BUILD_ROOT)" $(TEST_PORT_ARG) $(TEST_BAUD_ARG)

test-harware: test-hardware
