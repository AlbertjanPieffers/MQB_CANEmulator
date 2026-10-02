# MQB CAN Emulator ESP32-S3 v1.0.1

Fix for the ESP32-S3 compile errors reported in v1.0.

## Fixed

Explicit forward declarations were added for functions that are called before
their definitions, including:

- processCommand
- printState
- printHelp
- monitorCan
- processSerial
- printCanFrame
- shouldPrintRxFrame
- printMessages
- printClock
- printLighting
- printMfsw
- parseOnOff
- setIgnitionOn / setIgnitionOff

The v1.0 errors were declaration-order errors, not missing implementations.

## Arduino IDE

Open:

ArduinoIDE/MQB_Emulator_ESP32S3_v1_0_1/MQB_Emulator_ESP32S3_v1_0_1.ino

The folder and .ino filename deliberately match.

## Hardware

Normal ESP32-S3 + external CAN transceiver such as SN65HVD230.

Default TWAI pins:

- TX GPIO 5
- RX GPIO 4
- 500 kbit/s

Change the pin constants at the top of the sketch if needed.

This remains MQB-only. No LILYGO-specific code and no PQ/MQB translator.
