#include "ProfileManager.h"

static const GuidedStep DEFAULT_STEPS[] = {
    {"ignition_off", "Ignition OFF", "Leave the ignition OFF and do not touch the infotainment system.", 8},
    {"ignition_on", "Ignition ON", "Switch the ignition ON. Do not touch the infotainment system yet.", 10},
    {"home_screen", "Home screen", "Wait on the normal infotainment home screen.", 6},
    {"vehicle_menu", "Open Vehicle menu", "Open the Vehicle / CAR menu and wait.", 8},
    {"vehicle_status", "Vehicle Status", "Open Vehicle Status and leave the screen untouched.", 10},
    {"vehicle_return", "Think Blue Trainer", "Open Think Blue Trainer and wait.", 10},
    {"driving_data", "Driving Data", "Open Driving Data / Trip Data.", 10},
    {"since_start", "Since Start", "Select the Since Start view and wait.", 10},
    {"since_refuel", "Since Refuel", "Select the Since Refuel view and wait.", 10},
    {"long_term", "Long Term", "Select the Long Term view and wait.", 10},
    {"home_return", "Return Home", "Return to the infotainment home screen.", 6},
    {"ignition_off_end", "Ignition OFF", "Switch the ignition OFF and wait.", 10},
    {"finished", "Finished", "The guided capture is complete. Stop the session and export the log.", 0}
};

void ProfileManager::begin() {
    reset();
}

uint8_t ProfileManager::currentStep() const {
    return _currentStep;
}

size_t ProfileManager::stepCount() const {
    return sizeof(DEFAULT_STEPS) / sizeof(DEFAULT_STEPS[0]);
}

const GuidedStep& ProfileManager::step() const {
    return DEFAULT_STEPS[_currentStep];
}

void ProfileManager::reset() {
    _currentStep = 0;
}

bool ProfileManager::next() {
    if (_currentStep + 1 >= stepCount()) {
        return false;
    }

    _currentStep++;
    return true;
}

bool ProfileManager::previous() {
    if (_currentStep == 0) {
        return false;
    }

    _currentStep--;
    return true;
}
