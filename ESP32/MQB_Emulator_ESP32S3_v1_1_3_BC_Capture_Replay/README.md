# MQB Emulator ESP32-S3 v1.1.3 - TripData Lab

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


## v1.1.3 fix
- Fixed ESP32/Arduino C++ const-correctness in `sendRawCan()`.
- The CAN transmit helper now accepts `const byte *data`, so captured fixed payload arrays such as BAP_Config, FunctionList and HeartbeatConfig compile correctly.
- GPIO17 TX / GPIO18 RX preserved.


## v1.1.3 - standalone USB/Serial fix

All debug output now uses a non-blocking `DebugSerial` wrapper. When the
Arduino Serial Monitor is closed (or no USB serial host is connected), debug
text is dropped instead of allowing USB serial output to stall the main loop.

This is important because CAN, Wi-Fi, the Web UI and OTA all run from the same
main application loop. They must continue running without a Serial Monitor.

Serial input remains available when a monitor is connected.

## Future external CAN data file

A future version can load CAN definitions from a separate binary file stored
in LittleFS or in a dedicated flash data partition. That makes it possible to
update CAN datasets independently of the firmware. This is intentionally not
enabled in v1.1.3 yet.


## v1.1.3 - real Bordcomputer capture replay

This version adds a raw replay of the real-car Bordcomputer provider traffic
from `Speed.zip`, file `0-103-0 kmh all ID.txt`.

Only CAN ID `0x17330F10` is replayed. The table contains exactly 56
captured frames spanning 64.393 seconds. Payload bytes and
relative frame timing are preserved.

Commands:

- `bcreplay once` - play the real capture once
- `bcreplay on` - continuously loop the real capture
- `bcreplay off` - stop replay

The existing experimental `bctrip` generator is still present, but starting
one replay mode disables the other to avoid mixing synthetic Function 0x39
traffic with the captured stream.

Recommended first test:
1. Ignition/MIB bench normally running.
2. Open the vehicle/trip-data screen.
3. Run `bcreplay once`.
4. Observe the MIB and the incoming `0x17330F00` requests.
5. If useful, try `bcreplay on`.
