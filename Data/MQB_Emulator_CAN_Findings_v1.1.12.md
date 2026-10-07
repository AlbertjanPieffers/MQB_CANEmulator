# MQB Emulator – CAN / BAP Findings

**Project:** MQB_Emulator  
**Firmware baseline:** v1.1.12  
**Platform:** ESP32-S3 / TWAI  
**Bus:** MQB Infotainment CAN, 500 kbit/s  
**Status:** Work in progress / reverse-engineered from bench captures

---

## 1. Purpose

This document collects the CAN and BAP findings discovered while bench-testing a VW MQB MIB2 infotainment unit.

The current emulator has two main goals:

1. Keep the MIB in a stable, realistic vehicle/gateway state.
2. Decode and later emulate useful infotainment BAP data such as:
   - Bordcomputer / trip data
   - Phone calls
   - Navigation
   - Radio / media metadata
   - Steering wheel controls
   - Vehicle state / terminal state

Important distinction used throughout this document:

- **Confirmed** = repeatedly observed in captures and behavior is clear.
- **Likely** = strong correlation, but exact protocol meaning is not fully proven.
- **Experimental** = current working hypothesis only.

---

# 2. Hardware / CAN setup

Current development setup:

- ESP32-S3
- Internal TWAI controller
- CAN bitrate: **500 kbit/s**
- TWAI TX: **GPIO 17**
- TWAI RX: **GPIO 18**
- External CAN transceiver
- Single MQB infotainment CAN bus

The current firmware is single-bus. A later version is intended to run on a LILYGO T-2CAN for MQB ↔ PQ translation.

---

# 3. Gateway simulation

The MIB needs more than just ignition state. It expects a number of periodically transmitted vehicle and network-management messages.

The current gateway simulation has proven stable enough to keep the MIB awake for more than 30 minutes on the bench.

## 3.1 Confirmed / important periodic messages

| CAN ID | Interval | Example | Notes |
|---|---:|---|---|
| `0x3C0` | ~100 ms | dynamic | Terminal / ignition state |
| `0x3DA` | ~100 ms | `3F 18 00 FE FF F1 FF 00` | Required bench traffic |
| `0x3DB` | ~100 ms | `FE 03 00 00 80 00 00 FE` | Required bench traffic |
| `0x3DC` | ~50 ms | `FF 01 00 00 00 01 00 00` | Required bench traffic |
| `0x3EA` | ~200 ms | `0F 00 00 40 86 FF 00 00` | Required bench traffic |
| `0x585` | ~1 s | `02 3C A0 7F 13 00 00 00` | Required bench traffic |
| `0x663` | ~100 ms | dynamic | Gateway-related state |
| `0x1B000010` | ~200 ms | `10 00 04 02 19 00 00 00` | Gateway Network Management |
| `0x5BF` | ~50 ms | `00 00 00 40` | MFSW idle frame |
| `0x5F0` | periodic | dynamic | Dimming / illumination |

### 0x3C0 ignition / terminal state

For ignition ON:

- Byte 2/3 contain the terminal state.
- Typical active state used by the emulator: `03 00`.

The low nibble of byte 1 behaves as an alive counter.

Checksum lookup table currently used:

```cpp
static const uint8_t k3C0ChecksumByCounter[16] =
{
    0x74, 0xC1, 0x31, 0x84,
    0xFE, 0x4B, 0xBB, 0x0E,
    0x4F, 0xFA, 0x0A, 0xBF,
    0xC5, 0x70, 0x80, 0x35
};
```

---

# 4. Gateway ON / OFF behavior

Firmware v1.1.12 changed `simulategateway off` into a real shutdown instead of merely disabling a subset of gateway frames.

## 4.1 Previous problem

Before v1.1.12:

- The MIB powered down.
- After some time it started booting again.
- Boot stopped halfway.
- Button backlighting stayed illuminated.

Cause:

Several emulator messages were still being transmitted after gateway simulation was disabled.

Notably:

- MFSW idle `0x5BF`
- vehicle message table
- extended vehicle frames
- dimming frame `0x5F0`
- other periodic TX paths

These were enough to partially wake the MIB again.

## 4.2 Current shutdown behavior

On:

```text
simulategateway off
```

the emulator first sends a final shutdown burst:

### Terminal OFF

```text
0x3C0 -> all terminal / ignition flags OFF
```

### Illumination OFF

```text
0x5F0 -> 00 00 00 00 00 00 00 00
```

### Engine / vehicle state OFF

```text
0x3BE -> 00 00 00 00 00 00 00 00
```

This burst is transmitted three times.

After that:

- all periodic emulator TX stops
- MFSW idle TX stops
- gateway NM stops
- extended vehicle TX stops
- BC_MFA provider stops
- MIB is allowed to shut down naturally

This behavior was verified in the v1.1.12 test log.

## 4.3 Re-enabling

On:

```text
simulategateway on
```

the emulator:

- restores ignition state
- restores the previous lighting state
- resets periodic message timers
- restarts gateway NM
- restarts BC_MFA management frames

---

# 5. BAP transport observations

Many infotainment services use extended CAN IDs in the:

```text
0x1733xxxx
```

range.

Examples observed:

```text
0x173327xx
0x173328xx
0x173331xx
0x173332xx
```

A useful pattern is that related services generally use:

```text
...10
...11
```

as application / segmented transport channels.

## 5.1 Segmented transport

Observed segmented messages use frame families such as:

```text
0x80 / 0x8x -> C0, C1, C2...
0x90 / 0x9x -> D0, D1, D2...
```

This is important.

The decoder must **not mix Cx and Dx continuation families**.

Example:

```text
0x90 ...  -> use D0/D1/D2...
0x80 ...  -> use C0/C1/C2...
```

An earlier decoder accepted both continuation families during one transfer. This caused strings such as:

```text
THUIS
```

to be reconstructed incorrectly as:

```text
THU
```

because an unrelated Cx frame was inserted into a Dx transfer.

Firmware v1.1.10 fixed this.

---

# 6. Phone BAP

## 6.1 CAN channels

Confirmed phone-related traffic:

```text
0x17332810
0x17332811
```

The logical provider/channel is therefore associated with **0x28**.

## 6.2 Known functions

| Function | Meaning | Confidence |
|---|---|---|
| `4A 16` | Call state | High |
| `4A 17` | Caller/contact information | Very high |
| `4A 19` | Call transition / disconnect related | Medium-high |
| `4A 21` | Call present / active flag | High |

## 6.3 Call state examples

Observed incoming call:

```text
4A 16 11 01 00 00
```

Observed transition after answering:

```text
4A 16 21 08 00 00
```

Observed disconnect / hangup phase:

```text
4A 16 41 00 00 00
```

Likely state machine:

```text
IDLE
  |
  | 4A16 = 11...
  | 4A17 = caller info
  | 4A21 = 01
  v
RINGING
  |
  | 4A16 = 21...
  v
ACTIVE CALL
  |
  | 4A19 / 4A16 = 41...
  v
DISCONNECTING
  |
  | 4A21 = 00
  v
IDLE
```

## 6.4 Caller data

Caller/contact text is transmitted as segmented BAP data.

Example decoded serial output:

```text
[PHONE] Text: "THUIS" | "+31548545193"
```

The transport parser must preserve the complete continuation sequence or the last character may be lost.

---

# 7. Navigation BAP

## 7.1 CAN channels

Navigation-related traffic:

```text
0x17333210
0x17333211
```

Logical provider/channel is associated with **0x32**.

## 7.2 Known / likely functions

| Function | Meaning | Confidence |
|---|---|---|
| `4C 94` | Road / street text | Very high |
| `4C 95` | Route guidance state | High |
| `4C AE` | Destination/address data | Very high |
| `4C 91` | Navigation state/reset field | Medium |
| `4C 92` | Navigation state/reset field | Medium |
| `4C 98` | Navigation state/reset field | Medium |

## 7.3 Destination example

Observed decoded destination:

```text
[NAVI] Destination text:
"Am Bericher Holz 1" | "Edertal" | "34549" | "34549"
```

Interpretation:

- `Am Bericher Holz 1` = street + house number
- `Edertal` = city / locality
- `34549` = postal code
- second `34549` = still unidentified duplicate/related destination field

Do **not** interpret the final `34549` as a navigation arrow. It is ASCII text and matches the destination postal code.

## 7.4 Street example

Observed function `4C 94` transported street text such as:

```text
Welleweg
```

Likely meaning:

```text
current road / next road / manoeuvre-related street text
```

More captures are needed to distinguish the exact semantic role.

---

# 8. Radio / FM / RDS BAP

## 8.1 CAN channels

Radio / media-related BAP traffic:

```text
0x17333110
0x17333111
```

Logical provider/channel is associated with **0x31**.

## 8.2 FM station information

Function:

```text
4C 55
```

contains FM/RDS station data.

Confirmed examples:

### hr1

```text
80 19 4C 55 03 68 72 31
C0 44 00 00 08 39 39 2E
C1 30 ...
```

Decoded:

```text
Station: hr1
Frequency: 99.0 MHz
```

### hr2

```text
80 19 4C 55 03 68 72 32
C0 44 00 00 08 39 35 2E
C1 35 ...
```

Decoded:

```text
Station: hr2
Frequency: 95.5 MHz
```

### hr3

```text
80 1A 4C 55 03 68 72 33
C0 44 00 00 09 31 30 31
C1 2E 32 ...
```

Decoded:

```text
Station: hr3
Frequency: 101.2 MHz
```

### FFH

Decoded:

```text
Station: FFH
Frequency: 103.7 MHz
```

### planet

Decoded:

```text
Station: planet
Frequency: 104.6 MHz
```

## 8.3 4C55 structure

Strong current interpretation:

```text
4C 55
  |
  +-- first payload field = station-name length
  +-- station name
  +-- intermediate binary/status fields
  +-- frequency string length
  +-- frequency as ASCII
  +-- additional status fields
```

Examples:

```text
03 68 72 31
```

means:

```text
03 = string length
68 72 31 = "hr1"
```

and:

```text
06 70 6C 61 ...
```

starts a 6-character station name:

```text
planet
```

## 8.4 Selected station / preset

Function:

```text
4C 50
```

changes with station selection.

Examples:

```text
4C 50 01 01 00 26 30 01
4C 50 01 01 00 26 30 02
4C 50 01 01 00 26 30 03
...
```

The final byte tracks the selected station / entry index.

Current interpretation:

```text
4C 50 -> selected station / preset / list index
```

Confidence: **High**

## 8.5 Function 4C56

Observed:

```text
4C 56 ...
```

This clearly belongs to radio/media context, but the exact meaning is not yet proven.

Current firmware prints it as:

```text
[RADIO] Context 0x56: ...
```

rather than assigning an incorrect label.

---

# 9. Media metadata

The same `0x17333111` channel also carries media metadata.

Previously captured example:

```text
90 32 4C 55 04 46 61 79
D0 65 48 00 00 14 43 6F
D1 6F 6E 65 2C 20 44 61
D2 76 69 64 20 53 70 65
D3 6B 74 65 72 49 0C 4C
D4 65 73 73 20 49 73 20
D5 4D 6F 72 65 4A 00 00
```

Reassembled application data:

```text
Title : Faye
Artist: Coone, David Spekter
Album : Less Is More
```

Important:

`4C55` is therefore not simply "FM station name" in every operating mode.

It is better understood as a **media/radio information object whose structure depends on the active source/context**.

The FM decoder should therefore remain source-aware.

---

# 10. BC_MFA / Bordcomputer

The target trip-data application is known from MIB firmware as:

```text
BC_MFA
LSG 0x0F
```

However, real CAN captures show important traffic on:

```text
0x17332700
0x17332710
```

with BAP header/channel values involving:

```text
0x19
```

Therefore:

- `LSG 0x0F` should not automatically be equated to CAN channel `0x0F`.
- CAN transport channel `0x27` is confirmed to be relevant.
- There may be multiple logical/addressing layers inside BAP.

## 10.1 Known provider management frames

Observed real provider:

### Config

```text
39 C2 03 00 27 00 03 03
```

### FunctionList

```text
80 08 39 C3 38 07 F8 00
C0 00 00 00 00
```

### Heartbeat

```text
39 C4 0A
```

Real provider captures show Config and Heartbeat recurring approximately every 8 seconds.

## 10.2 Current status

The MIB still repeatedly sends:

```text
19 C2
```

and the emulator responds with:

```text
39 C2 03 00 27 00 03 03
```

but Wagenstatus / trip-data availability has not yet been fully unlocked.

Current conclusion:

- gateway/wake layer is good enough
- BAP provider/app initialization is still incomplete
- do not blindly add guessed trip-data frames yet

---

# 11. Useful serial commands

Common commands:

```text
simulategateway on
simulategateway off

monitor all
monitor rx
monitor tx
monitor diag
monitor off

mfsw volup
mfsw voldown
mfsw next
mfsw previous
mfsw phone
mfsw voice
mfsw left
mfsw right
mfsw up
mfsw down
mfsw ok

lights on
lights off
dimming <0-100>

bcprov on
bcprov off

canstats
state
messages
mark
help
```

---

# 12. Current serial decoder output

Examples:

## Phone

```text
[PHONE] Incoming/ringing state
[PHONE] Text: "THUIS" | "+31548545193"
[PHONE] Call answered/active transition
[PHONE] Disconnect/hangup transition
[PHONE] No active call
```

## Navigation

```text
[NAVI] Destination text: "Am Bericher Holz 1" | "Edertal" | "34549" | "34549"
[NAVI] Road/street text update
[NAVI] Route guidance appears active
[NAVI] Route guidance appears stopped
```

## Radio

```text
[RADIO] Preset/index: 1
[RADIO] Station: hr1
[RADIO] Frequency: 99.0 MHz
```

---

# 13. Recommended next reverse-engineering steps

1. Capture more FM stations with:
   - preset
   - manually tuned frequency
   - station without RDS
   - FM vs DAB if available

2. Capture navigation:
   - straight
   - left turn
   - right turn
   - roundabout
   - route recalculation
   - destination reached

3. Capture phone:
   - incoming number only
   - known contact
   - unknown/private number
   - outgoing call
   - rejected call

4. Continue BC_MFA research:
   - compare real provider responses to repeated `19 C2`
   - identify all `0x173327xx` functions
   - correlate provider functions with Wagenstatus screens
   - reconcile firmware `BC_MFA / LSG 0x0F` with wire channel `0x27`

5. Maintain strict separation between:
   - confirmed mappings
   - likely mappings
   - experimental guesses

---

# 14. Firmware milestones

| Version | Main change |
|---|---|
| v1.1.7 | Periodic BC_MFA 0x27 provider management |
| v1.1.8 | Phone + Navigation serial decoder |
| v1.1.9 | Longer BAP segmented-message reconstruction |
| v1.1.10 | Correct Cx/Dx continuation-family handling |
| v1.1.11 | FM / RDS station decoder |
| v1.1.12 | Proper gateway shutdown + bus-silent OFF state |

---

## Notes

This project is based on capture-driven reverse engineering.

Do not assume that a field means the same thing across every MIB software version, vehicle platform, BAP version or media source.

When possible, preserve raw frames next to decoded interpretations so future findings can be verified against the original bus traffic.
