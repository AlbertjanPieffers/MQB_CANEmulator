# MQB Log Converter v0.1

Python converter for `.mqblog` files created by MQB Guided Logger v0.2.x.

No third-party Python packages are required.

## Requirements

- Windows 10/11, Linux or macOS
- Python 3.10 or newer recommended
- Tkinter

The standard Windows Python installer normally includes Tkinter.

## Start the GUI

Double-click:

    Start_MQB_Log_Converter.bat

or run:

    python mqb_log_converter.py

## Supported exports

- CSV
- Vector ASC
- Linux candump
- JSON

## Command-line examples

Show information:

    python mqb_log_converter.py session.mqblog --info

Export CSV:

    python mqb_log_converter.py session.mqblog -f csv

Export several formats:

    python mqb_log_converter.py session.mqblog -f csv -f asc -f candump -f json

Specify an output file when exporting one format:

    python mqb_log_converter.py session.mqblog -f csv -o converted.csv

## MQBLOG v1 format

Header:

    uint32 magic
    uint16 formatVersion
    uint16 headerSize
    uint32 bitrate
    uint32 reserved

Packed size:

    16 bytes

Record:

    uint64 timestampUs
    uint32 canId
    uint8  type
    uint8  step
    uint8  flags
    uint8  dlc
    uint8  data[8]

Packed size:

    24 bytes

Record types:

    1 = CAN frame
    2 = marker

Flag:

    bit 0 = extended CAN identifier

## Notes

The current ESP32 logger stores marker text in the 8-byte record payload.
This means manually entered marker text is currently limited by the firmware.

A future MQBLOG format can add richer metadata without changing the basic converter workflow.
