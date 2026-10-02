# MQB Emulator ESP32-S3 v1.1.1 - TripData Lab

Based on the working ESP32-S3 emulator v1.0.1.

Changes:
- CAN pins fixed to the user's working GPIO17 TX / GPIO18 RX.
- Bordcomputer BAP_Config updated from real MQB capture: `33 C2 03 00 0F 00 06 06`.
- Added captured FunctionList and HeartbeatConfig replies.
- Added experimental Bordcomputer Function 0x39 trip-distance test.
- `bctrip <km>` sends a candidate trip distance with uint24 little-endian scaling of 0.01 km.
- `bctrip on` enables periodic transmission every 1000 ms.
- Web UI includes a Trip data lab card.

Important: Function 0x39 = trip distance is a strong capture-derived hypothesis, not yet confirmed by the MIB display.

Serial examples:
- `bcprov on`
- `bctrip 123.40`
- `bctrip on`
- `bctrip off`


## v1.1.1 fix
- Fixed ESP32/Arduino C++ const-correctness in `sendRawCan()`.
- The CAN transmit helper now accepts `const byte *data`, so captured fixed payload arrays such as BAP_Config, FunctionList and HeartbeatConfig compile correctly.
- GPIO17 TX / GPIO18 RX preserved.
