# MQB Guided Logger v0.2 Architecture

## Capture path

    Vehicle CAN
        |
        v
    CAN transceiver
        |
        v
    ESP32-S3 TWAI
        |
        v
    CanManager RX task
        |
        v
    LoggerManager ring buffer
        |
        +--> PSRAM when available
        |
        +--> Internal RAM fallback
        |
        v
    Batch writer
        |
        v
    LittleFS .mqblog

## Export path

    .mqblog
       |
       +--> CSV
       |
       +--> Vector ASC
       |
       +--> candump

Conversion happens only when the user exports a stored session.

## Storage protection

LoggerManager checks free LittleFS space every second.

When free storage reaches 5%, recording is stopped and the remaining ring buffer is flushed.

This prevents the repeated LittleFS "No more free space" error seen during the first bench test.

## Future work

- Full text marker table instead of 8-byte marker payload
- `.mqbtest` profile upload
- Vehicle metadata
- Unique CAN-ID statistics
- Before/after step comparison
- Changed-byte analysis
- microSD support
- Signed firmware packages
- OTA rollback strategy
