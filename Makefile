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
PORT ?= /dev/ttyUSB0
BAUD ?=

ifneq ($(strip $(BAUD)),)
SERIAL_ENV := ESPPORT="$(PORT)" ESPBAUD="$(BAUD)"
else
SERIAL_ENV := ESPPORT="$(PORT)"
endif

.PHONY: help configure build flash monitor flash-monitor factory-reset menuconfig size

help:
	@printf '%s\n' \
		'make build                  Configure when needed and compile firmware' \
		'make flash                  Build and flash using PORT=/dev/ttyUSB0' \
		'make monitor                Open the interactive serial monitor' \
		'make flash-monitor          Flash, then open the serial monitor' \
		'make factory-reset          Erase OTA selection data and boot factory firmware' \
		'make menuconfig             Open ESP-IDF configuration UI' \
		'make size                   Print firmware size information' \
		'' \
		'Overrides:' \
		'  make flash PORT=/dev/ttyUSB1' \
		'  make monitor BAUD=115200'

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
