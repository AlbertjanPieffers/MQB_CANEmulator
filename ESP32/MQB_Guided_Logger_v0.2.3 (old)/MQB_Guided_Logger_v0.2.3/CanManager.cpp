#include "CanManager.h"
#include "AppConfig.h"

#include "esp_timer.h"

bool CanManager::begin() {
    twai_general_config_t generalConfig =
        TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_LISTEN_ONLY);

    generalConfig.rx_queue_len = CAN_QUEUE_LENGTH;

    twai_timing_config_t timingConfig;

    switch (CAN_BITRATE) {
        case 125000:
            timingConfig = TWAI_TIMING_CONFIG_125KBITS();
            break;
        case 250000:
            timingConfig = TWAI_TIMING_CONFIG_250KBITS();
            break;
        case 500000:
            timingConfig = TWAI_TIMING_CONFIG_500KBITS();
            break;
        default:
            Serial.println("Unsupported CAN bitrate.");
            return false;
    }

    twai_filter_config_t filterConfig = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t result = twai_driver_install(
        &generalConfig,
        &timingConfig,
        &filterConfig
    );

    if (result != ESP_OK) {
        Serial.printf("TWAI install failed: %d\n", result);
        return false;
    }

    result = twai_start();

    if (result != ESP_OK) {
        Serial.printf("TWAI start failed: %d\n", result);
        return false;
    }

    BaseType_t taskResult = xTaskCreatePinnedToCore(
        taskEntry,
        "can_rx",
        4096,
        this,
        4,
        &_taskHandle,
        0
    );

    if (taskResult != pdPASS) {
        Serial.println("Failed to create CAN receive task.");
        return false;
    }

    Serial.println("CAN started in listen-only mode.");
    return true;
}

void CanManager::setFrameCallback(CanFrameCallback callback) {
    _callback = callback;
}

uint64_t CanManager::totalFrames() const {
    return _totalFrames;
}

uint32_t CanManager::framesPerSecond() const {
    return _framesPerSecond;
}

bool CanManager::busActive() const {
    return lastFrameAgeMs() < 1000;
}

uint32_t CanManager::lastFrameAgeMs() const {
    if (_lastFrameUs == 0) {
        return UINT32_MAX;
    }

    const uint64_t now = esp_timer_get_time();
    return static_cast<uint32_t>((now - _lastFrameUs) / 1000ULL);
}

void CanManager::taskEntry(void* parameter) {
    static_cast<CanManager*>(parameter)->receiveTask();
}

void CanManager::receiveTask() {
    twai_message_t message;

    uint32_t secondCounter = 0;
    uint64_t secondStartUs = esp_timer_get_time();

    while (true) {
        if (twai_receive(&message, pdMS_TO_TICKS(100)) != ESP_OK) {
            continue;
        }

        CanFrame frame {};
        frame.timestampUs = esp_timer_get_time();
        frame.id = message.identifier;
        frame.dlc = min<uint8_t>(message.data_length_code, 8);
        frame.extended = message.extd;

        memcpy(frame.data, message.data, frame.dlc);

        _lastFrameUs = frame.timestampUs;
        _totalFrames++;
        secondCounter++;

        const uint64_t now = frame.timestampUs;

        if ((now - secondStartUs) >= 1000000ULL) {
            _framesPerSecond = secondCounter;
            secondCounter = 0;
            secondStartUs = now;
        }

        if (_callback != nullptr) {
            _callback(frame);
        }
    }
}
