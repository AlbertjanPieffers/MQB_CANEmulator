# Flash layout

Target:

    ESP32-S3 N16R8
    16 MB flash
    8 MB OPI PSRAM

Partition map:

    0x009000  NVS        0x005000   20 KiB
    0x00E000  OTA data   0x002000    8 KiB
    0x010000  OTA 0      0x300000    3 MiB
    0x310000  OTA 1      0x300000    3 MiB
    0x610000  Core dump  0x010000   64 KiB
    0x620000  LittleFS   0x9E0000  9.875 MiB

The filesystem partition is explicitly named and typed:

    littlefs, data, littlefs

The firmware mounts it using partition label:

    littlefs
