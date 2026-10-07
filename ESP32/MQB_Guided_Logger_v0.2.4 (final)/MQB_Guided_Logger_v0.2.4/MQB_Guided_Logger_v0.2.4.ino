#include "AppConfig.h"
#include "CanManager.h"
#include "LoggerManager.h"
#include "ProfileManager.h"
#include "WebUi.h"

CanManager canManager;
LoggerManager loggerManager;
ProfileManager profileManager;
WebUi webUi;

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.printf("%s v%s\n", APP_NAME, APP_VERSION);

    if (!loggerManager.begin()) {
        Serial.println("Logger storage initialization failed.");
    }

    profileManager.begin();

    canManager.setFrameCallback([](const CanFrame& frame) {
        loggerManager.pushFrame(frame, profileManager.currentStep());
    });

    if (!canManager.begin()) {
        Serial.println("CAN initialization failed.");
    }

    webUi.begin(canManager, loggerManager, profileManager);

    Serial.println("System ready.");
    Serial.printf("Connect to Wi-Fi: %s\n", APP_WIFI_SSID);
    Serial.println("Open: http://192.168.4.1");
}

void loop() {
    webUi.loop();
    loggerManager.loop();
    delay(1);
}
