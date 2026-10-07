#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <DNSServer.h>

#include "CanManager.h"
#include "LoggerManager.h"
#include "ProfileManager.h"

class WebUi {
public:
    void begin(
        CanManager& canManager,
        LoggerManager& loggerManager,
        ProfileManager& profileManager
    );

    void loop();

private:
    void configureRoutes();

    void handleRoot();
    void handleStatus();

    void handleStartSession();
    void handleStopSession();
    void handleNextStep();
    void handlePreviousStep();
    void handleAddMarker();

    void handleListSessions();
    void handleDeleteSession();
    void handleExport();
    void handleRawDownload();

    void handleFirmwareUploadPage();
    void handleFirmwareUpload();
    void handleFirmwareUploadFinished();

    WebServer _server {80};
    DNSServer _dns;

    CanManager* _can = nullptr;
    LoggerManager* _logger = nullptr;
    ProfileManager* _profile = nullptr;
};
