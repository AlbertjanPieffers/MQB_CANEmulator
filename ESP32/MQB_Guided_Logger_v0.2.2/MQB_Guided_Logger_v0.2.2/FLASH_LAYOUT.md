# Flash layout

Target device:

    ESP32-S3 N16R8
    16 MB flash
    8 MB OPI PSRAM

Total address space:

    0x000000 - 0xFFFFFF

Custom application/data layout:

    0x009000  NVS        0x005000
    0x00E000  OTA data   0x002000
    0x010000  OTA 0      0x300000
    0x310000  OTA 1      0x300000
    0x610000  LittleFS   0x9F0000

The bootloader and partition table occupy the standard reserved region below 0x9000.

Why two 3 MB app slots?

- Browser OTA needs an inactive application slot.
- The logger firmware is comfortably below 3 MB.
- Two slots permit A/B OTA operation.
- The remaining flash can be used for logging.

Why LittleFS uses subtype `spiffs`

ESP32 partition tables use the generic filesystem subtype value traditionally named `spiffs`.
Arduino LittleFS can mount this partition normally.
