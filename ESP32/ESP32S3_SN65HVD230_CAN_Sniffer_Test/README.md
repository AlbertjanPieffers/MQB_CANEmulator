# ESP32-S3 + SN65HVD230 CAN Sniffer Test

Minimal diagnostic sketch for checking the CAN hardware without the MQB emulator.

## Pins
- GPIO17 -> SN65HVD230 D / TXD
- GPIO18 <- SN65HVD230 R / RXD
- 3.3 V -> VCC
- GND -> GND
- CAN-H -> CAN-H
- CAN-L -> CAN-L

## CAN
- 500 kbit/s
- Listen-only mode
- No CAN transmission
- Accept-all filter

## Serial
115200 baud

With the MIB and gateway active, frames should appear immediately.
Every 2 seconds the sketch also prints TWAI diagnostics.
