PORT ?= /dev/ttyACM0
BUILDDIR ?= build
IDF_PATH ?= $(shell pwd)/esp-idf
IDF_EXPORT_QUIET ?= 0
SHELL := /usr/bin/env bash

.PHONY: prepare clean build flash monitor menuconfig

all: prepare build install

prepare:
	git submodule update --init --recursive
	cd esp-idf; bash install.sh

clean:
	rm -rf "$(BUILDDIR)"

build:
	rm -f build/rom.sms.S
	rm -f build/lcd_controller.*.S
	source "$(IDF_PATH)/export.sh" && idf.py build

install: build
	python3 tools/webusb_push.py "SEGA SMS Emulator" build/main.bin --run

monitor:
	source "$(IDF_PATH)/export.sh" && idf.py monitor -p $(PORT)

menuconfig:
	source "$(IDF_PATH)/export.sh" && idf.py menuconfig

size:
	source "$(IDF_PATH)/export.sh" && idf.py size

size-files:
	source "$(IDF_PATH)/export.sh" && idf.py size-files
