# Firmware Architecture Brainstorm

## MQB Emulator / CAN Gateway

**Status:** Brainstorm / Architecture Proposal\
**Target hardware:** ESP32 prototype, later LILYGO dual-CAN hardware\
**Purpose:** Define a maintainable, secure firmware architecture with a
clear separation between the controller/system firmware and the actual
CAN emulator/translator logic.

------------------------------------------------------------------------

## 1. Goal

The firmware should be split into two clearly separated layers:

1.  **Base Control Layer**
    -   Controls the ESP32/LILYGO hardware and provides all generic
        device functionality.
    -   Should be stable and updated only when necessary.
    -   System updates should require privileged/service access.
    -   Uses a dedicated update package type so it cannot be confused
        with CAN Control updates.
2.  **CAN Control Layer**
    -   Contains the actual MQB/PQ CAN emulator and translator
        functionality.
    -   Should be easy for end users to update through the web
        interface.
    -   Distributed as an encrypted and digitally signed package.
    -   Should be independent from the exact underlying CAN controller
        hardware as much as possible.

The main principle is:

> The Base Control Layer owns the hardware. The CAN Control Layer owns
> the vehicle logic.

------------------------------------------------------------------------

## 2. High-Level Architecture

``` text
ESP32 / LILYGO
|
+-- Boot / Recovery
|
+-- Layer 1: Base Control Layer
|   |
|   +-- Hardware abstraction
|   +-- CAN controller drivers
|   +-- Wi-Fi
|   +-- Web interface
|   +-- Authentication
|   +-- Update manager
|   +-- Storage / NVS
|   +-- Logging
|   +-- Watchdog
|   +-- Diagnostics
|   +-- CAN Runtime API
|   `-- CAN Control loader/runtime
|
`-- Layer 2: CAN Control Layer
    |
    +-- PQ CAN functionality
    +-- MQB CAN functionality
    +-- PQ -> MQB translation
    +-- MQB -> PQ translation
    +-- CAN message scheduling
    +-- Signal decoding
    +-- Signal encoding
    +-- Emulator functions
    `-- Vehicle / feature profiles
```

------------------------------------------------------------------------

# 3. Layer 1 - Base Control Layer

## Responsibilities

The Base Control Layer is the permanent system firmware of the
controller.

It should contain functionality that is independent from a specific
vehicle or CAN translation implementation.

Examples:

-   ESP32 initialization
-   Hardware detection
-   CAN controller initialization
-   CAN transceiver control
-   GPIO handling
-   Wi-Fi access point
-   Wi-Fi client mode
-   Web server
-   Authentication
-   Update handling
-   Configuration storage
-   NVS / flash management
-   Logging
-   Diagnostics
-   Watchdog
-   Crash handling
-   Recovery
-   CAN Runtime API
-   CAN Control package loading

The CAN Control Layer should preferably never directly access ESP32
peripherals.

------------------------------------------------------------------------

## 3.1 Hardware Abstraction

The Base Control Layer should hide the exact CAN hardware implementation
from the CAN Control Layer.

For example, the prototype may use:

``` text
CAN A -> ESP32 TWAI
CAN B -> MCP2515
```

A later LILYGO version could use a different implementation.

The CAN Control Layer should only see something similar to:

``` text
CAN_A.Send(...)
CAN_A.Receive(...)

CAN_B.Send(...)
CAN_B.Receive(...)
```

This means CAN translation code does not need to change when the
hardware changes.

------------------------------------------------------------------------

## 3.2 System Firmware Package

Proposed extension:

``` text
.vsys
```

Example:

``` text
MQBEmulator_System_1.4.0.vsys
```

The extension is only for user clarity. The firmware must never trust
the extension itself.

The package should contain metadata such as:

``` text
Package Type: SYSTEM
Version: 1.4.0
Hardware: ESP32-S3
Hardware Revision: >= 1
Minimum Bootloader: 1.1
Payload Size: ...
SHA-256: ...
Signature: ...
```

The updater must verify the package contents before installation.

------------------------------------------------------------------------

## 3.3 System Firmware Access

System firmware updates should not be available through the normal CAN
update page.

Suggested web interface:

``` text
Settings
|
+-- Device
+-- Network
+-- CAN
+-- CAN Control Update
+-- Diagnostics
`-- Advanced
    `-- System Firmware
        `-- Service authentication required
```

Possible authentication methods:

-   Service password
-   Administrator account
-   Device-specific service credential
-   Later: signed service/update token

The exact authentication mechanism is still an open design decision.

------------------------------------------------------------------------

# 4. Layer 2 - CAN Control Layer

## Responsibilities

The CAN Control Layer contains the actual vehicle-specific
functionality.

Examples:

### PQ

-   Ignition state
-   Engine data
-   ABS/ESP data
-   DSG data
-   Steering wheel data
-   Instrument cluster data
-   Infotainment data
-   Gateway-related messages

### MQB

-   Ignition state
-   Engine data
-   ABS/ESC data
-   DSG data
-   Steering wheel data
-   Instrument cluster data
-   Infotainment data
-   Gateway-related messages

### Translation

``` text
PQ CAN
   |
   v
Decode
   |
   v
Internal Signal
   |
   v
Encode
   |
   v
MQB CAN
```

And in the opposite direction:

``` text
MQB CAN
   |
   v
Decode
   |
   v
Internal Signal
   |
   v
Encode
   |
   v
PQ CAN
```

------------------------------------------------------------------------

## 4.1 CAN Control Package

Proposed extension:

``` text
.vcan
```

Example:

``` text
MQBEmulator_CAN_2.7.3.vcan
```

Using `.vcan` instead of a generic `.bin` makes it much clearer to users
which file belongs in which updater.

Internally, the package could contain:

``` text
Package Type: CAN_CONTROL
Version: 2.7.3
Required System: >= 1.4.0
Runtime API: 3
Hardware: ESP32 / ESP32-S3
Encrypted: YES
Payload Size: ...
SHA-256: ...
Signature: ...
```

------------------------------------------------------------------------

# 5. Encryption and Digital Signing

Encryption and signing solve two different problems.

## Encryption

Purpose:

-   Make extraction of proprietary CAN implementation more difficult.
-   Prevent users from trivially inspecting the distributed CAN Control
    package.

Possible approach:

``` text
CAN Control payload
        |
        v
AES-256 encryption
        |
        v
.vcan package
```

The exact key-management architecture still needs to be designed
carefully.

A shared encryption key compiled into every device should preferably be
avoided as the final production design.

------------------------------------------------------------------------

## Digital Signing

Purpose:

-   Verify that an update was created by the project owner.
-   Reject modified or unofficial packages.
-   Prevent accidental or malicious installation of altered CAN Control
    software.

Concept:

``` text
Build system
     |
     v
CAN Control package
     |
     v
Sign with PRIVATE KEY
     |
     v
.vcan
     |
     v
ESP32
     |
     v
Verify using PUBLIC KEY
     |
     +-- Valid   -> Continue
     |
     `-- Invalid -> Reject
```

The private signing key must never be stored on the ESP32.

The ESP32 only needs the public verification key.

------------------------------------------------------------------------

# 6. Update Separation

The system must explicitly distinguish between both package types.

## Normal CAN Control Update

``` text
CAN Control

Installed version:
2.6.1

System firmware:
1.4.0

[ Select .vcan file ]

[ Install ]
```

Validation:

``` text
Signature             OK
Package type          CAN_CONTROL
Version               2.7.3
System requirement    >= 1.4.0
Runtime API           Compatible
Hardware              Compatible
Payload               Valid
```

Only after all checks succeed may installation start.

------------------------------------------------------------------------

## System Firmware Update

``` text
Advanced
-> System Firmware
-> Service Login

Current System:
1.4.0

[ Select .vsys file ]

[ Install System Firmware ]
```

Validation must explicitly require:

``` text
Package Type: SYSTEM
```

If a `.vcan` package is uploaded:

``` text
Update rejected

Expected package:
SYSTEM

Received package:
CAN_CONTROL
```

The reverse must also be rejected.

------------------------------------------------------------------------

# 7. CAN Runtime API

One of the most important architectural components is the interface
between the Base Control Layer and CAN Control Layer.

Example:

``` text
                CAN Control Layer
                       |
                 Runtime API
                       |
          +------------+------------+
          |                         |
       CAN A API                  CAN B API
          |                         |
       Driver                    Driver
          |                         |
     CAN Controller          CAN Controller
          |                         |
        MQB Bus                  PQ Bus
```

Possible Runtime API functions:

``` text
CAN.Send()
CAN.Subscribe()
CAN.Unsubscribe()
CAN.GetState()
CAN.GetErrorCounters()

Timer.Create()
Timer.Delete()

Config.Get()
Config.Set()

Log.Info()
Log.Warning()
Log.Error()

System.GetVersion()
System.GetHardwareRevision()
```

Exact API definitions should be versioned.

------------------------------------------------------------------------

# 8. Runtime API Versioning

System firmware and CAN Control software should not rely purely on
firmware version numbers.

Introduce a separate:

``` text
CAN Runtime API Version
```

Example:

``` text
System Firmware: 1.4.0
Runtime API: 3
```

A CAN package can declare:

``` text
CAN Control: 2.7.3
Required Runtime API: 3
```

The Base Control Layer can reject incompatible packages.

Example:

``` text
Installed Runtime API: 3
Required Runtime API: 4

CAN Control package cannot be installed.
System firmware update required.
```

Future Base Control releases could temporarily support multiple Runtime
API versions for backward compatibility.

------------------------------------------------------------------------

# 9. CAN Control Internal Structure

Possible logical organization:

``` text
CAN Control
|
+-- Core
|   +-- Scheduler
|   +-- Signal database
|   +-- Translation engine
|   `-- State management
|
+-- PQ
|   +-- Gateway
|   +-- Engine
|   +-- ABS
|   +-- DSG
|   +-- Steering Wheel
|   +-- Cluster
|   `-- Infotainment
|
+-- MQB
|   +-- Gateway
|   +-- Engine
|   +-- ABS
|   +-- DSG
|   +-- Steering Wheel
|   +-- Cluster
|   `-- Infotainment
|
+-- Translators
|   +-- PQ_to_MQB
|   `-- MQB_to_PQ
|
`-- Emulator
    +-- Ignition
    +-- RPM
    +-- Vehicle Speed
    +-- Gear
    +-- Steering Wheel
    `-- Other signals
```

------------------------------------------------------------------------

# 10. Internal Signal Model

Where possible, translation should not directly convert one CAN frame
into another CAN frame.

Instead:

``` text
PQ frame
   |
   v
PQ decoder
   |
   v
Internal signal model
   |
   v
MQB encoder
   |
   v
MQB frame
```

Example internal signals:

``` text
Vehicle.Ignition
Vehicle.Speed
Engine.RPM
Engine.CoolantTemperature
Engine.Torque
Transmission.Gear
Transmission.State
Brake.BrakePedal
Brake.ABSActive
Steering.Angle
Steering.Buttons
```

Advantages:

-   PQ and MQB implementations stay separated.
-   One decoded signal can feed multiple outgoing messages.
-   Easier debugging.
-   Easier support for future platforms.
-   Easier simulation and testing.

------------------------------------------------------------------------

# 11. Update Safety

Updates must be resilient against:

-   Power loss
-   Corrupt uploads
-   Wrong hardware package
-   Wrong package type
-   Interrupted downloads
-   Invalid signatures
-   Incompatible Runtime API versions

For Base Control firmware, dual OTA partitions are preferred.

Example:

``` text
Flash
|
+-- Bootloader
+-- Partition Table
+-- NVS
+-- OTA Metadata
|
+-- System OTA A
|   `-- Current system firmware
|
+-- System OTA B
|   `-- New system firmware
|
`-- CAN Control Storage
```

After a System update:

1.  Write new firmware to inactive OTA partition.
2.  Verify image.
3.  Mark it as pending.
4.  Reboot.
5.  Run startup/self-test.
6.  Mark firmware valid only after successful initialization.
7.  Roll back if startup fails.

------------------------------------------------------------------------

# 12. CAN Control Rollback

The CAN Control Layer should preferably also support rollback.

Possible storage:

``` text
CAN Control A
Current: 2.6.1

CAN Control B
New: 2.7.3
```

Update process:

``` text
Upload
  |
  v
Verify
  |
  v
Decrypt / validate
  |
  v
Write inactive slot
  |
  v
Activate
  |
  v
Restart CAN runtime
  |
  +-- Success -> Keep new version
  |
  `-- Failure -> Restore previous version
```

This requires further investigation depending on the final CAN Control
execution model.

------------------------------------------------------------------------

# 13. Web Interface

The web interface belongs to the Base Control Layer.

Suggested sections:

``` text
Dashboard

CAN Status

CAN Monitor

CAN Configuration

Emulator / Translator

Network

CAN Control Update

Diagnostics

Advanced
`-- System Firmware
```

The UI should display both versions separately:

``` text
System Firmware
1.4.0

CAN Control
2.7.3

Runtime API
3

Hardware
LILYGO Dual CAN Rev A
```

This makes support and diagnostics much easier.

------------------------------------------------------------------------

# 14. Device Startup

Suggested startup sequence:

``` text
Power On
   |
   v
Bootloader
   |
   v
Base Control Layer
   |
   +-- Initialize hardware
   +-- Initialize storage
   +-- Initialize CAN controllers
   +-- Initialize Wi-Fi
   +-- Start web interface
   |
   v
Load CAN Control package
   |
   +-- Verify package
   +-- Verify signature
   +-- Check compatibility
   +-- Decrypt payload
   |
   v
Start CAN Runtime
   |
   v
MQB / PQ Emulator Active
```

If the CAN Control package cannot be loaded, the Base Control Layer
should remain operational.

The device should still provide:

-   Web interface
-   Diagnostics
-   Firmware update
-   CAN Control recovery/update

The CAN outputs should remain in a safe inactive state until a valid CAN
Control package is loaded.

------------------------------------------------------------------------

# 15. Proposed Package Naming

System firmware:

``` text
MQBEmulator_System_1.4.0.vsys
```

CAN Control:

``` text
MQBEmulator_CAN_2.7.3.vcan
```

Potential future packages:

``` text
MQBEmulator_Config_1.0.0.vcfg
MQBEmulator_Diagnostics_1.0.0.vdiag
```

These are only ideas and are not currently part of the architecture.

------------------------------------------------------------------------

# 16. Important Open Design Decision: How Is Layer 2 Executed?

An ESP32 cannot automatically load an arbitrary second binary like a
desktop operating system loads an executable or DLL.

The CAN Control Layer therefore needs an explicit execution model.

Three main approaches should be investigated.

## Option A - Custom Bytecode / Virtual Machine

CAN Control is compiled into project-specific bytecode.

Advantages:

-   Strong separation from system firmware.
-   Easy package updates.
-   Hardware-independent.
-   Good control over available functionality.
-   CAN Control cannot freely access arbitrary ESP32 hardware.

Disadvantages:

-   Requires development of a runtime/VM.
-   More engineering work.
-   Potential execution overhead.

------------------------------------------------------------------------

## Option B - Data-Driven Rules + Native Handlers

Most CAN functionality is represented as data/rules, while complex
functions are implemented in the Base Control Layer.

Advantages:

-   Relatively simple.
-   Fast.
-   Easy to update message definitions.
-   Lower implementation risk.

Disadvantages:

-   Complex new functionality may still require System firmware updates.
-   Less flexible than a true runtime.

------------------------------------------------------------------------

## Option C - Native Secondary Application Image

CAN Control is compiled as native ESP32 code.

Advantages:

-   Maximum performance.
-   Normal C/C++ implementation.
-   Full compiler optimization.

Disadvantages:

-   Harder isolation.
-   Harder linking/API compatibility.
-   Greater risk of CAN Control affecting the system.
-   Update and rollback architecture becomes more complicated.

------------------------------------------------------------------------

# 17. Preferred Direction

Initial preference:

``` text
Hybrid Runtime
```

Use:

-   Native Base Control firmware for hardware-critical and
    performance-critical functionality.
-   A controlled Runtime API.
-   Data-driven CAN definitions where possible.
-   A limited executable/bytecode layer for more complex translation
    logic if required.

This provides a balance between:

-   Performance
-   Security
-   Update flexibility
-   Hardware independence
-   Maintainability

The exact runtime implementation should be prototyped before committing
to the final package format.

------------------------------------------------------------------------

# 18. Security Principles

The project should follow these basic rules:

1.  Never trust a filename or file extension.
2.  Verify package type internally.
3.  Digitally sign all production update packages.
4.  Keep private signing keys outside the device.
5.  Validate hardware compatibility.
6.  Validate Runtime API compatibility.
7.  Validate package integrity before activation.
8.  Keep System and CAN Control update paths separate.
9.  Provide rollback where practical.
10. Do not expose low-level hardware access to CAN Control unless
    required.
11. Keep recovery functionality in the Base Control Layer.
12. CAN transmission should fail safe when CAN Control is invalid or
    unavailable.

------------------------------------------------------------------------

# 19. Development Phases

## Phase 1 - Prototype

-   ESP32
-   Dual CAN setup
-   Basic hardware abstraction
-   CAN A / CAN B API
-   Wi-Fi access point
-   Basic web interface
-   Configuration storage
-   CAN message scheduler
-   Simple PQ/MQB translation

## Phase 2 - Layer Separation

-   Formal CAN Runtime API
-   CAN Control package format
-   Version compatibility
-   Package verification
-   Separate System and CAN update pages
-   CAN Control rollback concept

## Phase 3 - Security

-   Digital signatures
-   Encryption
-   Secure key handling
-   Anti-downgrade policy if required
-   Service authentication
-   Production package tooling

## Phase 4 - LILYGO Hardware

-   Move hardware-specific code into drivers
-   Adapt CAN A / CAN B backend
-   Keep CAN Runtime API unchanged
-   Hardware revision detection
-   Hardware compatibility checks

## Phase 5 - Production Update Infrastructure

-   HTTPS update server
-   Version manifest
-   Automatic update checking
-   Release notes
-   Stable/beta update channels if desired
-   Signed package build pipeline

------------------------------------------------------------------------

# 20. Example Final User Experience

Normal user:

``` text
MQB Emulator

System Firmware
1.4.0

CAN Control
2.7.3

CAN A
MQB - Active

CAN B
PQ - Active

Translator
MQB <-> PQ - Active

[ CAN Monitor ]
[ Configuration ]
[ CAN Control Update ]
```

CAN update:

``` text
CAN Control Update

Current:
2.7.3

[ Select MQBEmulator_CAN_2.8.0.vcan ]

[ Install Update ]
```

System firmware remains hidden under privileged Advanced settings.

------------------------------------------------------------------------

# 21. Summary

The proposed architecture separates the project into two major firmware
layers:

``` text
BASE CONTROL LAYER
Hardware + Wi-Fi + Web + Drivers + Updates + Runtime
                |
                | Stable Runtime API
                v
CAN CONTROL LAYER
MQB + PQ + Emulator + Translator
```

Proposed update formats:

``` text
.vsys
System firmware
Privileged/service update
Signed
Strong validation
OTA/rollback

.vcan
CAN Control package
Normal web update
Encrypted
Signed
Runtime API compatibility check
Rollback where possible
```

The next major architecture task is to prototype the **CAN Runtime API
and Layer 2 execution model**. That decision determines how flexible the
CAN Control Layer can become without requiring frequent Base Control
firmware updates.
