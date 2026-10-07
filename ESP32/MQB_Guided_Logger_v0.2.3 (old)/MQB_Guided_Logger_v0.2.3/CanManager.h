#pragma once

#include <Arduino.h>
#include "driver/twai.h"

struct CanFrame {
    uint64_t timestampUs;
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
    bool extended;
};

using CanFrameCallback = void (*)(const CanFrame& frame);

class CanManager {
public:
    bool begin();
    void setFrameCallback(CanFrameCallback callback);

    uint64_t totalFrames() const;
    uint32_t framesPerSecond() const;
    bool busActive() const;
    uint32_t lastFrameAgeMs() const;

private:
    static void taskEntry(void* parameter);
    void receiveTask();

    CanFrameCallback _callback = nullptr;

    volatile uint64_t _totalFrames = 0;
    volatile uint32_t _framesPerSecond = 0;
    volatile uint64_t _lastFrameUs = 0;

    TaskHandle_t _taskHandle = nullptr;
};
