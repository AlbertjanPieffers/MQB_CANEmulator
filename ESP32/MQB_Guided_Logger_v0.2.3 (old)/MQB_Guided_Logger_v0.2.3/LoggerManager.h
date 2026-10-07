#pragma once

#include <Arduino.h>
#include <LittleFS.h>
#include "esp_heap_caps.h"

#include "AppConfig.h"
#include "CanManager.h"
#include "LogFormat.h"

class LoggerManager {
public:
    bool begin();
    void loop();

    bool startSession();
    void stopSession();
    bool isLogging() const;

    void pushFrame(const CanFrame& frame, uint8_t step);
    void addMarker(uint8_t step, const String& marker);

    String currentSessionPath() const;
    uint64_t loggedFrames() const;
    uint64_t droppedFrames() const;

    bool usingPsram() const;
    size_t bufferCapacity() const;
    size_t bufferUsed() const;

    size_t storageTotalBytes() const;
    size_t storageUsedBytes() const;
    size_t storageFreeBytes() const;
    uint8_t storageFreePercent() const;
    bool storageReady() const;

    String listSessionsJson();
    bool deleteSession(const String& fileName);

    bool exportCsv(const String& fileName, Print& output);
    bool exportAsc(const String& fileName, Print& output);
    bool exportCandump(const String& fileName, Print& output);

private:
    bool ensureSessionDirectory();
    String makeSessionName();

    bool allocateRingBuffer();
    bool enqueueRecord(const MqbLogRecord& record);
    bool dequeueRecord(MqbLogRecord& record);

    void flushBatch();
    void checkStorage();

    bool validSessionName(const String& fileName) const;
    String buildPath(const String& fileName) const;

    volatile bool _logging = false;
    volatile uint64_t _loggedFrames = 0;
    volatile uint64_t _droppedFrames = 0;

    String _currentPath;

    MqbLogRecord* _ring = nullptr;
    size_t _ringCapacity = 0;
    volatile size_t _writeIndex = 0;
    volatile size_t _readIndex = 0;
    volatile size_t _usedRecords = 0;

    bool _usingPsram = false;

    uint32_t _lastStorageCheckMs = 0;
    bool _storageReady = false;

    portMUX_TYPE _bufferMux = portMUX_INITIALIZER_UNLOCKED;
};
