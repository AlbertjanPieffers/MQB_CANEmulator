#pragma once

#include <Arduino.h>

static constexpr uint32_t MQBLOG_MAGIC = 0x474C514D; // "MQLG" little-endian marker
static constexpr uint16_t MQBLOG_VERSION = 1;

enum class LogRecordType : uint8_t {
    CanFrame = 1,
    Marker = 2
};

struct __attribute__((packed)) MqbLogHeader {
    uint32_t magic;
    uint16_t formatVersion;
    uint16_t headerSize;
    uint32_t bitrate;
    uint32_t reserved;
};

struct __attribute__((packed)) MqbLogRecord {
    uint64_t timestampUs;
    uint32_t canId;
    uint8_t type;
    uint8_t step;
    uint8_t flags;
    uint8_t dlc;
    uint8_t data[8];
};

static constexpr uint8_t LOG_FLAG_EXTENDED = 0x01;
