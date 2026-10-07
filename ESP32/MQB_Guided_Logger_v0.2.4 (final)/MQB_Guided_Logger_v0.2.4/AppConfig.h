#pragma once

#include <Arduino.h>
#include "driver/gpio.h"

static constexpr const char* APP_NAME = "MQB Guided Logger";
static constexpr const char* APP_HARDWARE_NAME = "ESP32-S3 N16R8";
static constexpr const char* APP_VERSION = "0.2.4.1";

static constexpr const char* APP_WIFI_SSID = "MQB-LOGGER";
static constexpr const char* APP_WIFI_PASSWORD = "mqblogger";

static constexpr gpio_num_t CAN_TX_PIN = GPIO_NUM_17;
static constexpr gpio_num_t CAN_RX_PIN = GPIO_NUM_18;

static constexpr uint32_t CAN_BITRATE = 500000;
static constexpr size_t CAN_QUEUE_LENGTH = 1024;

static constexpr const char* SESSION_DIR = "/sessions";
static constexpr const char* LITTLEFS_BASE_PATH = "/littlefs";
static constexpr const char* LITTLEFS_PARTITION_LABEL = "littlefs";

// Ring buffer capacities.
// PSRAM is preferred. Internal RAM is used when PSRAM is unavailable.
static constexpr size_t LOGGER_PSRAM_RECORD_CAPACITY = 8192;
static constexpr size_t LOGGER_INTERNAL_RECORD_CAPACITY = 1024;

// Number of records written per LoggerManager::loop() iteration.
static constexpr size_t LOGGER_WRITE_BATCH = 128;

// Stop recording when LittleFS has this percentage or less remaining.
static constexpr uint8_t LOGGER_MIN_FREE_PERCENT = 5;

// Storage check interval while logging.
static constexpr uint32_t LOGGER_STORAGE_CHECK_INTERVAL_MS = 1000;
