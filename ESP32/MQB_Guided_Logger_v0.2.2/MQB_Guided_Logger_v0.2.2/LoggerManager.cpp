#include "LoggerManager.h"

#include "esp_timer.h"

bool LoggerManager::begin() {
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed.");
        return false;
    }

    if (!ensureSessionDirectory()) {
        Serial.println("Could not create sessions directory.");
        return false;
    }

    if (!allocateRingBuffer()) {
        Serial.println("Logger ring buffer allocation failed.");
        return false;
    }

    Serial.printf(
        "LittleFS: %u bytes total, %u bytes free\n",
        static_cast<unsigned>(storageTotalBytes()),
        static_cast<unsigned>(storageFreeBytes())
    );

    Serial.printf(
        "Logger buffer: %u records (%s)\n",
        static_cast<unsigned>(_ringCapacity),
        _usingPsram ? "PSRAM" : "internal RAM"
    );

    return true;
}

bool LoggerManager::ensureSessionDirectory() {
    if (LittleFS.exists(SESSION_DIR)) {
        return true;
    }

    return LittleFS.mkdir(SESSION_DIR);
}

bool LoggerManager::allocateRingBuffer() {
    _usingPsram = false;

    if (psramFound()) {
        _ringCapacity = LOGGER_PSRAM_RECORD_CAPACITY;

        _ring = static_cast<MqbLogRecord*>(
            heap_caps_malloc(
                _ringCapacity * sizeof(MqbLogRecord),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
            )
        );

        if (_ring != nullptr) {
            _usingPsram = true;
            return true;
        }
    }

    _ringCapacity = LOGGER_INTERNAL_RECORD_CAPACITY;

    _ring = static_cast<MqbLogRecord*>(
        heap_caps_malloc(
            _ringCapacity * sizeof(MqbLogRecord),
            MALLOC_CAP_8BIT
        )
    );

    return _ring != nullptr;
}

String LoggerManager::makeSessionName() {
    const uint64_t stamp = esp_timer_get_time();

    char fileName[72];
    snprintf(
        fileName,
        sizeof(fileName),
        "%s/session_%llu.mqblog",
        SESSION_DIR,
        stamp
    );

    return String(fileName);
}

bool LoggerManager::startSession() {
    stopSession();

    if (storageFreePercent() <= LOGGER_MIN_FREE_PERCENT) {
        Serial.println("Not enough free LittleFS space to start a session.");
        return false;
    }

    _currentPath = makeSessionName();

    File file = LittleFS.open(_currentPath, FILE_WRITE);

    if (!file) {
        Serial.println("Could not create session file.");
        return false;
    }

    MqbLogHeader header {};
    header.magic = MQBLOG_MAGIC;
    header.formatVersion = MQBLOG_VERSION;
    header.headerSize = sizeof(MqbLogHeader);
    header.bitrate = CAN_BITRATE;
    header.reserved = 0;

    const size_t written = file.write(
        reinterpret_cast<const uint8_t*>(&header),
        sizeof(header)
    );

    file.close();

    if (written != sizeof(header)) {
        LittleFS.remove(_currentPath);
        return false;
    }

    portENTER_CRITICAL(&_bufferMux);
    _writeIndex = 0;
    _readIndex = 0;
    _usedRecords = 0;
    portEXIT_CRITICAL(&_bufferMux);

    _loggedFrames = 0;
    _droppedFrames = 0;
    _lastStorageCheckMs = millis();
    _logging = true;

    Serial.printf("Session started: %s\n", _currentPath.c_str());
    return true;
}

void LoggerManager::stopSession() {
    if (!_logging && _usedRecords == 0) {
        return;
    }

    _logging = false;

    while (bufferUsed() > 0) {
        flushBatch();
        delay(1);
    }

    Serial.println("Session stopped.");
}

bool LoggerManager::isLogging() const {
    return _logging;
}

bool LoggerManager::enqueueRecord(const MqbLogRecord& record) {
    bool success = false;

    portENTER_CRITICAL(&_bufferMux);

    if (_usedRecords < _ringCapacity) {
        _ring[_writeIndex] = record;
        _writeIndex = (_writeIndex + 1) % _ringCapacity;
        _usedRecords++;
        success = true;
    }

    portEXIT_CRITICAL(&_bufferMux);

    return success;
}

bool LoggerManager::dequeueRecord(MqbLogRecord& record) {
    bool success = false;

    portENTER_CRITICAL(&_bufferMux);

    if (_usedRecords > 0) {
        record = _ring[_readIndex];
        _readIndex = (_readIndex + 1) % _ringCapacity;
        _usedRecords--;
        success = true;
    }

    portEXIT_CRITICAL(&_bufferMux);

    return success;
}

void LoggerManager::pushFrame(const CanFrame& frame, uint8_t step) {
    if (!_logging || _ring == nullptr) {
        return;
    }

    MqbLogRecord record {};
    record.timestampUs = frame.timestampUs;
    record.canId = frame.id;
    record.type = static_cast<uint8_t>(LogRecordType::CanFrame);
    record.step = step;
    record.flags = frame.extended ? LOG_FLAG_EXTENDED : 0;
    record.dlc = frame.dlc;

    memcpy(record.data, frame.data, frame.dlc);

    if (!enqueueRecord(record)) {
        _droppedFrames++;
        return;
    }

    _loggedFrames++;
}

void LoggerManager::addMarker(uint8_t step, const String& marker) {
    if (!_logging) {
        return;
    }

    // Marker records are compact numeric markers in v0.2.
    // The first 8 characters are stored as bytes. Guided step identity
    // remains available through the step field.
    MqbLogRecord record {};
    record.timestampUs = esp_timer_get_time();
    record.canId = 0;
    record.type = static_cast<uint8_t>(LogRecordType::Marker);
    record.step = step;
    record.flags = 0;

    const uint8_t len = min<size_t>(marker.length(), 8);
    record.dlc = len;

    for (uint8_t i = 0; i < len; i++) {
        record.data[i] = static_cast<uint8_t>(marker[i]);
    }

    if (!enqueueRecord(record)) {
        _droppedFrames++;
    }
}

void LoggerManager::loop() {
    if (_usedRecords > 0) {
        flushBatch();
    }

    if (_logging) {
        checkStorage();
    }
}

void LoggerManager::flushBatch() {
    if (_currentPath.isEmpty()) {
        return;
    }

    File file = LittleFS.open(_currentPath, FILE_APPEND);

    if (!file) {
        return;
    }

    MqbLogRecord record {};

    for (size_t i = 0; i < LOGGER_WRITE_BATCH; i++) {
        if (!dequeueRecord(record)) {
            break;
        }

        file.write(
            reinterpret_cast<const uint8_t*>(&record),
            sizeof(record)
        );
    }

    file.close();
}

void LoggerManager::checkStorage() {
    const uint32_t now = millis();

    if ((now - _lastStorageCheckMs) < LOGGER_STORAGE_CHECK_INTERVAL_MS) {
        return;
    }

    _lastStorageCheckMs = now;

    if (storageFreePercent() <= LOGGER_MIN_FREE_PERCENT) {
        Serial.println("Storage guard triggered. Stopping logger before LittleFS is full.");
        stopSession();
    }
}

String LoggerManager::currentSessionPath() const {
    return _currentPath;
}

uint64_t LoggerManager::loggedFrames() const {
    return _loggedFrames;
}

uint64_t LoggerManager::droppedFrames() const {
    return _droppedFrames;
}

bool LoggerManager::usingPsram() const {
    return _usingPsram;
}

size_t LoggerManager::bufferCapacity() const {
    return _ringCapacity;
}

size_t LoggerManager::bufferUsed() const {
    size_t value;

    portENTER_CRITICAL(const_cast<portMUX_TYPE*>(&_bufferMux));
    value = _usedRecords;
    portEXIT_CRITICAL(const_cast<portMUX_TYPE*>(&_bufferMux));

    return value;
}

size_t LoggerManager::storageTotalBytes() const {
    return LittleFS.totalBytes();
}

size_t LoggerManager::storageUsedBytes() const {
    return LittleFS.usedBytes();
}

size_t LoggerManager::storageFreeBytes() const {
    const size_t total = storageTotalBytes();
    const size_t used = storageUsedBytes();

    return used >= total ? 0 : total - used;
}

uint8_t LoggerManager::storageFreePercent() const {
    const size_t total = storageTotalBytes();

    if (total == 0) {
        return 0;
    }

    return static_cast<uint8_t>(
        (storageFreeBytes() * 100ULL) / total
    );
}

bool LoggerManager::validSessionName(const String& fileName) const {
    if (fileName.isEmpty()) {
        return false;
    }

    if (
        fileName.indexOf("..") >= 0 ||
        fileName.indexOf("/") >= 0 ||
        fileName.indexOf("\\") >= 0
    ) {
        return false;
    }

    return fileName.endsWith(".mqblog");
}

String LoggerManager::buildPath(const String& fileName) const {
    return String(SESSION_DIR) + "/" + fileName;
}

String LoggerManager::listSessionsJson() {
    String json = "[";

    File directory = LittleFS.open(SESSION_DIR);

    if (!directory || !directory.isDirectory()) {
        return "[]";
    }

    bool first = true;
    File file = directory.openNextFile();

    while (file) {
        if (!file.isDirectory()) {
            String name = String(file.name());
            String shortName = name;

            if (shortName.startsWith(String(SESSION_DIR) + "/")) {
                shortName.remove(0, strlen(SESSION_DIR) + 1);
            }

            if (shortName.endsWith(".mqblog")) {
                if (!first) {
                    json += ",";
                }

                json += "{";
                json += "\"name\":\"" + shortName + "\",";
                json += "\"size\":" + String(file.size());
                json += "}";

                first = false;
            }
        }

        file = directory.openNextFile();
    }

    json += "]";
    return json;
}

bool LoggerManager::deleteSession(const String& fileName) {
    if (!validSessionName(fileName)) {
        return false;
    }

    const String path = buildPath(fileName);

    if (!LittleFS.exists(path)) {
        return false;
    }

    if (path == _currentPath && _logging) {
        return false;
    }

    return LittleFS.remove(path);
}

bool LoggerManager::exportCsv(const String& fileName, Print& output) {
    if (!validSessionName(fileName)) {
        return false;
    }

    File file = LittleFS.open(buildPath(fileName), FILE_READ);

    if (!file) {
        return false;
    }

    MqbLogHeader header {};

    if (file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
        file.close();
        return false;
    }

    if (header.magic != MQBLOG_MAGIC) {
        file.close();
        return false;
    }

    output.println("timestamp_us,step,event,can_id,format,dlc,data");

    MqbLogRecord record {};

    while (file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record)) {
        if (record.type == static_cast<uint8_t>(LogRecordType::CanFrame)) {
            char dataString[3 * 8 + 1] = {0};
            size_t pos = 0;

            for (uint8_t i = 0; i < record.dlc && i < 8; i++) {
                const int written = snprintf(
                    &dataString[pos],
                    sizeof(dataString) - pos,
                    i == 0 ? "%02X" : " %02X",
                    record.data[i]
                );

                if (written <= 0) {
                    break;
                }

                pos += static_cast<size_t>(written);
            }

            output.printf(
                "%llu,%u,,0x%lX,%s,%u,\"%s\"\n",
                record.timestampUs,
                record.step,
                static_cast<unsigned long>(record.canId),
                (record.flags & LOG_FLAG_EXTENDED) ? "EXT" : "STD",
                record.dlc,
                dataString
            );
        }
        else if (record.type == static_cast<uint8_t>(LogRecordType::Marker)) {
            char marker[9] = {0};

            for (uint8_t i = 0; i < record.dlc && i < 8; i++) {
                marker[i] = static_cast<char>(record.data[i]);
            }

            output.printf(
                "%llu,%u,\"%s\",,,,\n",
                record.timestampUs,
                record.step,
                marker
            );
        }
    }

    file.close();
    return true;
}

bool LoggerManager::exportAsc(const String& fileName, Print& output) {
    if (!validSessionName(fileName)) {
        return false;
    }

    File file = LittleFS.open(buildPath(fileName), FILE_READ);

    if (!file) {
        return false;
    }

    MqbLogHeader header {};

    if (file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
        file.close();
        return false;
    }

    if (header.magic != MQBLOG_MAGIC) {
        file.close();
        return false;
    }

    output.println("base hex timestamps absolute");
    output.println("internal events logged");

    bool firstFrame = true;
    uint64_t firstTimestamp = 0;

    MqbLogRecord record {};

    while (file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record)) {
        if (record.type != static_cast<uint8_t>(LogRecordType::CanFrame)) {
            continue;
        }

        if (firstFrame) {
            firstTimestamp = record.timestampUs;
            firstFrame = false;
        }

        const double seconds =
            static_cast<double>(record.timestampUs - firstTimestamp) / 1000000.0;

        output.printf(
            "%.6f 1 %lX Rx d %u",
            seconds,
            static_cast<unsigned long>(record.canId),
            record.dlc
        );

        for (uint8_t i = 0; i < record.dlc && i < 8; i++) {
            output.printf(" %02X", record.data[i]);
        }

        output.print("\n");
    }

    file.close();
    return true;
}

bool LoggerManager::exportCandump(const String& fileName, Print& output) {
    if (!validSessionName(fileName)) {
        return false;
    }

    File file = LittleFS.open(buildPath(fileName), FILE_READ);

    if (!file) {
        return false;
    }

    MqbLogHeader header {};

    if (file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
        file.close();
        return false;
    }

    if (header.magic != MQBLOG_MAGIC) {
        file.close();
        return false;
    }

    bool firstFrame = true;
    uint64_t firstTimestamp = 0;

    MqbLogRecord record {};

    while (file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record)) {
        if (record.type != static_cast<uint8_t>(LogRecordType::CanFrame)) {
            continue;
        }

        if (firstFrame) {
            firstTimestamp = record.timestampUs;
            firstFrame = false;
        }

        const double seconds =
            static_cast<double>(record.timestampUs - firstTimestamp) / 1000000.0;

        output.printf(
            "(%.6f) can0 %lX#",
            seconds,
            static_cast<unsigned long>(record.canId)
        );

        for (uint8_t i = 0; i < record.dlc && i < 8; i++) {
            output.printf("%02X", record.data[i]);
        }

        output.print("\n");
    }

    file.close();
    return true;
}
