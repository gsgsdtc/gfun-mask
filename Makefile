PROJECT_ROOT := $(shell pwd)
FIRMWARE_DIR := $(PROJECT_ROOT)/firmware
IOS_PROJECT  := $(PROJECT_ROOT)/VoiceMaskApp/VoiceMaskApp.xcodeproj
ESP_IDF      := $(HOME)/esp/v5.5.2/esp-idf/export.sh

.PHONY: all esp ios clean flash monitor

all: esp ios

## ESP32 ──────────────────────────────────────

esp:
	@echo "[ESP32] 编译固件..."
	@bash -c "source $(ESP_IDF) > /dev/null 2>&1 && cd $(FIRMWARE_DIR) && idf.py build"

flash:
	@echo "[ESP32] 烧录固件..."
	@bash -c "source $(ESP_IDF) > /dev/null 2>&1 && cd $(FIRMWARE_DIR) && idf.py -p $$(ls /dev/cu.usb* | head -1) flash"

monitor:
	@echo "[ESP32] 串口监视..."
	@bash -c "source $(ESP_IDF) > /dev/null 2>&1 && cd $(FIRMWARE_DIR) && idf.py -p $$(ls /dev/cu.usb* | head -1) monitor" < /dev/tty

## iOS ────────────────────────────────────────

ios:
	@echo "[iOS] 编译 App..."
	@xcodebuild \
		-project $(IOS_PROJECT) \
		-scheme VoiceMaskApp \
		-configuration Debug \
		-destination "generic/platform=iOS" \
		CODE_SIGN_STYLE=Automatic \
		-allowProvisioningUpdates \
		build \
		| xcpretty 2>/dev/null || cat

## 清理 ───────────────────────────────────────

clean:
	@echo "[ESP32] 清理..."
	@bash -c "source $(ESP_IDF) > /dev/null 2>&1 && cd $(FIRMWARE_DIR) && idf.py fullclean"
	@echo "[iOS] 清理..."
	@xcodebuild -project $(IOS_PROJECT) -scheme VoiceMaskApp clean | xcpretty 2>/dev/null || cat
