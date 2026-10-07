# MQB Guided Logger v0.2.2

Bench-focused ESP32-S3 CAN logger for MQB infotainment and BAP research.

## v0.2.1 download fix

- Fixed broken browser downloads caused by writing raw generated data into a chunked HTTP response.
- CSV / ASC / candump now use proper HTTP chunks.
- Added direct `.mqblog` download with an exact Content-Length.

## Changes from v0.1

- Compact binary `.mqblog` storage
- PSRAM ring buffer when available
- Automatic fallback to internal RAM
- Dropped-frame counter
- Buffer usage indicator
- LittleFS free-space indicator
- Automatic recording stop at 5% free storage
- On-demand CSV export
- On-demand Vector ASC export
- On-demand Linux candump export
- Firmware update through WebUI
- CAN remains listen-only

## Binary record size

Each CAN record uses the packed `MqbLogRecord` structure.

This is substantially smaller than storing every frame as a CSV line.

## Wi-Fi

SSID:

    MQB-LOGGER

Password:

    mqblogger

WebUI:

    http://192.168.4.1

## ESP32-S3 N16R8 settings

Typical Arduino IDE settings:

- Board: ESP32S3 Dev Module
- Flash Size: 16MB
- PSRAM: OPI PSRAM
- USB CDC On Boot: Enabled if required
- Partition Scheme: use an OTA-capable partition with a sizeable LittleFS/SPIFFS data partition

If PSRAM is not configured correctly, v0.2 still runs using internal RAM with a smaller ring buffer.

## CAN

Default:

    500 kbit/s
    TWAI_MODE_LISTEN_ONLY

Pins are configured in `AppConfig.h`.

## Important storage note

LittleFS capacity depends on the Arduino partition scheme.

For long logging sessions, the next hardware step should still be microSD. v0.2 is intended to make internal-flash logging much more efficient and safe.


## Custom 16 MB partition layout

v0.2.2 includes a `partitions.csv` file in the sketch folder.

Select:

    Board: ESP32S3 Dev Module
    Flash Size: 16MB (128Mb)
    PSRAM: OPI PSRAM
    Partition Scheme: Custom

The partition map is:

    NVS          0x009000 - 0x00DFFF   20 KB
    OTA data     0x00E000 - 0x00FFFF    8 KB
    OTA app 0    0x010000 - 0x30FFFF    3 MB
    OTA app 1    0x310000 - 0x60FFFF    3 MB
    LittleFS     0x610000 - 0xFFFFFF  9.94 MiB

This keeps two complete OTA firmware slots while leaving almost 10 MiB for logger sessions.

### Important when changing partition layout

When switching from an older partition scheme to this custom layout, do one full erase before uploading:

    Tools
    -> Erase All Flash Before Sketch Upload
    -> Enabled

Upload the firmware once, then set the option back to:

    Disabled

This prevents old filesystem data from a previous layout from confusing LittleFS.

### OTA behavior

The WebUI firmware updater writes the new firmware to the inactive OTA application slot.

The compiled application must remain below 3 MB.

The `.mqblog` sessions are stored only in the LittleFS partition and are not overwritten by a normal OTA firmware update.
