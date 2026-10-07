#pragma once

#include <Arduino.h>

struct GuidedStep {
    const char* id;
    const char* title;
    const char* instruction;
    uint16_t waitSeconds;
};

class ProfileManager {
public:
    void begin();

    uint8_t currentStep() const;
    size_t stepCount() const;
    const GuidedStep& step() const;

    void reset();
    bool next();
    bool previous();

private:
    uint8_t _currentStep = 0;
};
