#include <Arduino.h>
#include "driver/twai.h"

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ArduinoOTA.h>

/*
 * MQB CAN Emulator v1.1.6
 * ESP32-S3 + external CAN transceiver (e.g. SN65HVD230)
 *
 * This is a single-bus MQB Infotainment CAN emulator.
 * There is NO PQ bus and NO PQ<->MQB translation layer in this version.
 *
 * CAN:
 *   500 kbit/s
 *   Classic CAN / CAN 2.0
 *   ESP32-S3 internal TWAI controller
 *
 * Default pins:
 *   TWAI TX -> GPIO 5
 *   TWAI RX -> GPIO 4
 *
 * Change CAN_TX_PIN / CAN_RX_PIN below to match your wiring.
 *
 * Existing MQB emulator functions are ported from the Uno project,
 * including MFSW, vehicle state, media/phone feedback decoding,
 * status watch and the experimental Bordcomputer LSG 0x0F probe.
 *
 * Network:
 *   Wi-Fi station mode if credentials are configured.
 *   Fallback access point otherwise.
 *
 * OTA:
 *   ArduinoOTA
 *   Browser firmware upload at /update
 *
 * Web UI:
 *   Dashboard and controls at /
 *   JSON state at /api/state
 *   Existing serial commands via /api/command?cmd=...
 */

// ============================================================
// ESP32-S3 TWAI PIN CONFIGURATION
// ============================================================

static constexpr gpio_num_t CAN_TX_PIN = GPIO_NUM_17;
static constexpr gpio_num_t CAN_RX_PIN = GPIO_NUM_18;

// ============================================================
// NETWORK / OTA SETTINGS
// ============================================================

// Leave empty to start the fallback access point immediately.
static const char *WIFI_SSID = "";
static const char *WIFI_PASSWORD = "";

static const char *AP_SSID = "MQB-Emulator";
static const char *AP_PASSWORD = "change-me-123";

static const char *OTA_HOSTNAME = "mqb-emulator";
static const char *OTA_PASSWORD = "change-me-ota";

// ============================================================
// CAN / WEB STATE
// ============================================================

bool canReady = false;

uint32_t canRxCount = 0;
uint32_t canTxCount = 0;
uint32_t canTxErrorCount = 0;

WebServer webServer(80);
bool webReady = false;
bool otaReady = false;

String networkMode = "OFF";
IPAddress networkIp;

// Master switch for the bench gateway replacement.
// Default ON so the MIB can wake and see the captured vehicle-state traffic
// immediately after power-up.
bool simulateGatewayEnabled = true;


// ============================================================
// VEHICLE STATE
// ============================================================

struct VehicleState
{
    bool terminalS;
    bool terminal15;
    bool terminalX;
    bool terminal50;
    bool terminal75;

    bool engineRunning;

    byte startStopStatus;
    byte startStopDriverRequest;

    uint16_t engineRpm;
};

VehicleState vehicle =
{
    true,
    true,
    false,
    false,
    true,
    true,
    0,
    0,
    850
};


// ============================================================
// LIGHTING STATE
// ============================================================

struct LightingState
{
    bool lightsEnabled;

    // DBC: DI_KL_58xd is raw 0..253, not percent.
    byte dimming58xd;

    // DBC: DI_KL_58xs and DI_KL_58xt are 0..100 percent.
    byte dimming58xs;
    byte dimming58xt;

    bool nightDesign;
    uint16_t photoSensor;
};

LightingState lighting =
{
    true,
    0xFD,
    100,
    100,
    false,
    0
};


// ============================================================
// CLOCK STATE
// ============================================================

struct ClockState
{
    uint16_t year;
    byte month;
    byte day;
    byte hour;
    byte minute;
    byte second;

    bool running;
    unsigned long lastTick;
};

ClockState clockState =
{
    2026,
    9,
    29,
    20,
    0,
    0,
    true,
    0
};


// ============================================================
// UNITS STATE
// ============================================================

struct UnitsState
{
    byte dateFormat;          // KBI_Einheit_Datum, bits 0..1
    byte pressureUnit;        // KBI_Einheit_Druck, bits 2..3
    bool distanceMiles;       // KBI_Einheit_Streckenanz, bit 4
    bool mfaSpeedMph;         // KBI_MFA_v_Einheit_02, bit 5
    bool temperatureF;        // KBI_Einheit_Temp, bit 6
    bool clock12h;            // KBI_Einheit_Uhrzeit, bit 7
    byte consumptionUnit;     // KBI_Einheit_Verbrauch, bits 8..9
    byte volumeUnit;          // KBI_Einheit_Volumen, bits 10..11
    byte language;            // KBI_Einheit_Sprache, bits 16..23
};

UnitsState units =
{
    0,
    0,
    false,
    false,
    false,
    false,
    0,
    0,
    0
};


// ============================================================
// ADDITIONAL BENCH VEHICLE DATA
// ============================================================

struct BenchVehicleData
{
    bool reverse;
    bool parkingBrake;
    float speedKph;
    float outsideTemperatureC;
    byte fuelLiters;
};

BenchVehicleData benchData =
{
    false,
    false,
    0.0,
    20.0,
    30
};

byte kombi01Counter = 0;
// ============================================================
// EXTENDED DBC VEHICLE DATA - LOW SRAM
// ============================================================

struct ExtendedVehicleData
{
    int16_t oilTempC10;
    int16_t coolantTempC10;
    int16_t intakeTempC10;
    uint16_t oilPressureCentiBar;
    uint16_t boostCentiBar;
    byte throttlePercent;
    byte oilLevelPercent;
    bool clutch;
    bool kickdown;
    bool mil;
    bool engineWarning;
    bool oilWarning;
    bool absLamp;
    bool espLamp;
    bool airbagLamp;
    bool steeringLamp;
};

ExtendedVehicleData extData =
{
    900, 900, 250,
    300, 100,
    0, 100,
    false, false, false, false, false,
    false, false, false, false
};

unsigned long lastMotor04 = 0;
unsigned long lastMotor07 = 0;
unsigned long lastMotor20 = 0;
unsigned long lastMotor26 = 0;

void sendExtendedVehicleMessages();
void sendMotor04();
void sendMotor07();
void sendMotor20();
void sendMotor26();

// ============================================================
// MFSW STATE - 0x5BF, GOLF MK7 INFOTAINMENT CAN CAPTURE FORMAT
// ============================================================

struct MfswState
{
    byte buttonCode;
    unsigned long releaseAt;
    bool active;
    bool releasePending;
};

MfswState mfsw =
{
    0x00,
    0,
    false,
    false
};

// Captured Golf Mk7 / MQB infotainment CAN button codes.
const byte MFSW_RIGHT_MENU = 0x02;
const byte MFSW_LEFT_MENU  = 0x03;
const byte MFSW_UP         = 0x04;
const byte MFSW_DOWN       = 0x05;
const byte MFSW_OK         = 0x07;
const byte MFSW_VOLUME_UP  = 0x10;
const byte MFSW_VOLUME_DOWN= 0x11;
const byte MFSW_NEXT       = 0x15;
const byte MFSW_PREVIOUS   = 0x16;
const byte MFSW_VOICE      = 0x19;
const byte MFSW_PHONE      = 0x1C;

const unsigned long MFSW_PRESS_TIME_MS = 150;
const unsigned long MFSW_PERIOD_MS = 50;

unsigned long mfswLastSend = 0;


// ============================================================
// INFOTAINMENT FEEDBACK / ASCII DECODER
// ============================================================

// Volume feedback seen on extended infotainment CAN.
// 0x17333110:
//   3C 52 XX ... = periodic volume state
//   4C 52 XX ... = volume event/update
//   4C 6F .. .. .. XX = volume acknowledgement
bool volumeFeedbackEnabled = true;
bool asciiDecoderEnabled = true;

bool volumeKnown = false;
byte lastVolume = 0;

// Lightweight segmented ASCII decoder.
//
// Observed on 0x17332810:
//   B0 0B 4A 11 0A 47 61 6C
//   F0 61 78 79 20 41 35 31
//
// which reconstructs:
//   "Galaxy A51"
//
// The decoder is deliberately generic for the 0x1733xxxx family,
// because track/artist/phone-book text may use nearby logical channels.
bool asciiMessageActive = false;
unsigned long asciiActiveId = 0;
byte asciiExpectedLength = 0;
byte asciiPrintedLength = 0;

// Media metadata transport observed on EXT 0x17333111.
//
// Example captured after MFSW Next:
//   90 32 4C 55 04 46 61 79
//   D0 65 48 00 00 14 43 6F
//   D1 6F 6E 65 2C 20 44 61
//   D2 76 69 64 20 53 70 65
//   D3 6B 74 65 72 49 0C 4C
//   D4 65 73 73 20 49 73 20
//   D5 4D 6F 72 65 4A 00 00
//
// Reassembled payload:
//   04 "Faye"
//   48 00 00
//   14 "Coone, David Spekter"
//   49
//   0C "Less Is More"
//   4A 00 00
//
// For this capture that maps cleanly to Title / Artist / Album.
bool mediaMetadataEnabled = true;
bool mediaMessageActive = false;
byte mediaBuffer[56];
byte mediaBufferLength = 0;
byte mediaExpectedSequence = 0;
unsigned long mediaLastFrameMs = 0;

// Vehicle-status discovery window.
// A warning/status command can arm a short capture window so only the
// interesting Infotainment/BAP traffic after that change is printed.
bool vehicleStatusProbeEnabled = true;
bool vehicleStatusProbeActive = false;
unsigned long vehicleStatusProbeUntil = 0;
const unsigned long VEHICLE_STATUS_PROBE_MS = 3000UL;

// Minimal experimental BC_MFA provider on BAP logical channel 0x27.
bool bcProviderEnabled = false;

// Bordcomputer / trip-data laboratory.
// Provider channel: request 0x17332700, response 0x17332710.
// Trip-function semantics are still under reverse engineering.
bool bcTripTestEnabled = false;
float bcTripDistanceKm = 123.40f;
unsigned long bcTripLastSend = 0;
static const unsigned long BC_TRIP_INTERVAL_MS = 1000UL;

// Suppress repeated identical ACK lines.
bool volumeAckKnown = false;
byte lastVolumeAck = 0;


// ============================================================
// VCDS / UDS COMPONENT IDENTITY SPOOF - BENCH SUPPORT
// ============================================================
//
// Captured 5F diagnostic pair:
//   Tester -> 5F : 0x773
//   5F -> Tester : 0x7DD
//
// VCDS reads the component text with:
//   22 F1 97
//
// The real MIB returns:
//   "MU-S-N-ER    "
//
// This bench implementation can answer with:
//   "MQB-PQ-BRIDGE"
//
// IMPORTANT:
// This firmware currently has only one CAN interface. Therefore it cannot
// suppress the real MIB response when the MIB is connected to the same bus.
// Keep this feature OFF when benching with the real MIB on the same bus.
//
// In the future dual-CAN bridge version this same handler should be used on
// the PQ-facing side while DID F197 from the MIB-facing side is filtered.
//

static constexpr unsigned long VCDS_5F_REQUEST_ID  = 0x773UL;
static constexpr unsigned long VCDS_5F_RESPONSE_ID = 0x7DDUL;

bool diagIdentitySpoofEnabled = false;
bool diagF197AwaitingFlowControl = false;

// F197 contains 13 ASCII characters in this test response.
static const char DIAG_COMPONENT_NAME[14] = "MQB-PQ-BRIDGE";


void decodeInfotainmentFeedback(
    unsigned long id,
    bool extended,
    byte dlc,
    const byte *data
);

void decodeVolumeFeedback(byte dlc, const byte *data);
void decodeAsciiTransport(unsigned long id, byte dlc, const byte *data);
void printAsciiPayload(const byte *data, byte startIndex, byte dlc);

void decodeMediaMetadata(unsigned long id, byte dlc, const byte *data);
void resetMediaMetadata();
void appendMediaBytes(const byte *data, byte startIndex, byte dlc);
void tryPrintMediaMetadata();
void printMediaField(const __FlashStringHelper *label, const byte *data, byte len);

const __FlashStringHelper *mfswButtonName(byte code);
void armVehicleStatusProbe(const __FlashStringHelper *reason);
void decodeVehicleStatusProbe(unsigned long id, byte dlc, const byte *data);
void processBcProvider(unsigned long id, bool extended, byte dlc, const byte *data);
void printCompactExtFrame(
    const __FlashStringHelper *prefix,
    unsigned long id,
    byte dlc,
    const byte *data
);


// ============================================================
// MONITOR CONFIGURATION
// ============================================================

enum MonitorMode
{
    MONITOR_OFF,
    MONITOR_ALL,
    MONITOR_RX,
    MONITOR_TX,
    MONITOR_DIAG
};

MonitorMode monitorMode = MONITOR_OFF;
unsigned int markerCounter = 0;


// ============================================================
// KLEMMEN_STATUS_01
// ============================================================

const byte klemmenChecksum[16] =
{
    0x74, 0xC1, 0x31, 0x84,
    0xFE, 0x4B, 0xBB, 0x0E,
    0x4F, 0xFA, 0x0A, 0xBF,
    0xC5, 0x70, 0x80, 0x35
};

byte klemmenCounter = 0;


// ============================================================
// CAN MESSAGE SYSTEM
// ============================================================

struct CanMessage;
typedef void (*MessageUpdateFunction)(CanMessage &message);

struct CanMessage
{
    const char *name;
    unsigned long id;
    bool extended;
    byte dlc;
    unsigned long interval;
    unsigned long lastSend;
    bool enabled;
    byte data[8];
    MessageUpdateFunction update;
};


// ============================================================
// DECLARATIONS
// ============================================================

void updateKlemmenStatus(CanMessage &message);
void updateMotor14(CanMessage &message);
void updateDimming01(CanMessage &message);
void updateDiagnose01(CanMessage &message);
void updateEinheiten01(CanMessage &message);
void updateBCM01(CanMessage &message);
void updateKombi01(CanMessage &message);
void updateKombi02(CanMessage &message);

void sendMessages();
bool sendCanMessage(CanMessage &message);
bool sendRawCan(unsigned long id, bool extended, byte dlc, const byte *data);

void processDiagnosticSpoof(
    unsigned long id,
    bool extended,
    byte dlc,
    const byte *data
);
void sendDiagF197FirstFrame();
void sendDiagF197ConsecutiveFrames();

void updateClock();
void incrementClockOneSecond();
bool isLeapYear(uint16_t year);
byte daysInMonth(uint16_t year, byte month);

void updateMfsw();
void startMfswEvent(byte buttonCode);
void sendMfswPressed(byte buttonCode);
void sendMfswRelease();
void sendMfswIdle();


// ============================================================
// MESSAGE TABLE
// ============================================================

CanMessage messages[] =
{
    // Captured Gateway Network Management / node-presence frame.
    // Bench use only with the physical MQB gateway disconnected.
    // Together with Klemmen_Status_01 below this is the first gateway-wake test.
    {
        "Gateway_NM",
        0x1B000010UL,
        true,
        8,
        200,
        0,
        true,
        {0x10, 0x00, 0x04, 0x02, 0x19, 0x00, 0x00, 0x00},
        NULL
    },

    // Capture-exact vehicle-state frames present with the real gateway/vehicle
    // network and absent during the first ESP32-only test.
    // Kept static in v1.1.5 so this test isolates the missing wake/status layer.
    {
        "GatewayCapture_3DA",
        0x3DA,
        false,
        8,
        100,
        0,
        true,
        {0x3F, 0x18, 0x00, 0xFE, 0xFF, 0xF1, 0xFF, 0x00},
        NULL
    },

    {
        "GatewayCapture_3DB",
        0x3DB,
        false,
        8,
        100,
        0,
        true,
        {0xFE, 0x03, 0x00, 0x00, 0x80, 0x00, 0x00, 0xFE},
        NULL
    },

    {
        "GatewayCapture_3DC",
        0x3DC,
        false,
        8,
        50,
        0,
        true,
        {0xFF, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00},
        NULL
    },

    {
        "GatewayCapture_3EA",
        0x3EA,
        false,
        8,
        200,
        0,
        true,
        {0x0F, 0x00, 0x00, 0x40, 0x86, 0xFF, 0x00, 0x00},
        NULL
    },

    {
        "GatewayCapture_585",
        0x585,
        false,
        8,
        1000,
        0,
        true,
        {0x02, 0x3C, 0xA0, 0x7F, 0x13, 0x00, 0x00, 0x00},
        NULL
    },

    {
        "GatewayCapture_663",
        0x663,
        false,
        8,
        100,
        0,
        true,
        {0x70, 0x28, 0x01, 0x0E, 0x5F, 0x87, 0x00, 0x04},
        NULL
    },

    {
        "Klemmen_Status_01",
        0x3C0,
        false,
        4,
        100,
        0,
        true,
        {0x74, 0x00, 0x03, 0x00, 0, 0, 0, 0},
        updateKlemmenStatus
    },

    {
        "Motor_14",
        0x3BE,
        false,
        8,
        100,
        0,
        true,
        {0, 0, 0x04, 0, 0x80, 0, 0, 0},
        updateMotor14
    },

    {
        "Dimmung_01",
        0x5F0,
        false,
        8,
        200,
        0,
        true,
        {0, 100, 100, 0, 0, 0, 0, 0},
        updateDimming01
    },

    {
        "Diagnose_01",
        0x6B2,
        false,
        8,
        1000,
        0,
        true,
        {0, 0, 0, 0, 0, 0, 0, 0},
        updateDiagnose01
    },

    {
        "Einheiten_01",
        0x643,
        false,
        8,
        1000,
        0,
        true,
        {0, 0, 0, 0, 0, 0, 0, 0},
        updateEinheiten01
    },

    // DBC-confirmed: BCM_01, decimal 1626 = 0x65A.
    // Includes the reverse-light switch at bit 30.
    {
        "BCM_01",
        0x65A,
        false,
        8,
        1000,
        0,
        true,
        {0, 0, 0, 0, 0, 0, 0, 0},
        updateBCM01
    },

    // DBC-confirmed: Kombi_01, decimal 779 = 0x30B.
    // Includes handbrake and displayed vehicle speed.
    {
        "Kombi_01",
        0x30B,
        false,
        8,
        50,
        0,
        true,
        {0, 0, 0, 0, 0, 0, 0, 0},
        updateKombi01
    },

    // DBC-confirmed: Kombi_02, decimal 1719 = 0x6B7.
    // Includes fuel content and filtered outside temperature.
    {
        "Kombi_02",
        0x6B7,
        false,
        8,
        1000,
        0,
        true,
        {0, 0, 0, 0, 0, 0, 0, 0},
        updateKombi02
    }
};

const byte MESSAGE_COUNT = sizeof(messages) / sizeof(messages[0]);



// ============================================================
// FORWARD DECLARATIONS
// ============================================================
//
// Arduino normally generates function prototypes for .ino files, but the
// combination of lambdas used by WebServer and functions declared later in
// the sketch can prevent the generated prototypes from being sufficient.
// Keep these explicit declarations so the ESP32-S3 build is deterministic.
//

void setBitsIntel(byte *data, byte startBit, byte length, uint32_t value);

void setIgnitionOn();
void setIgnitionOff();

void setGatewaySimulation(bool enabled);
bool isGatewaySimulationMessage(const CanMessage &message);

void monitorCan();

bool shouldPrintRxFrame(unsigned long id, bool extended);
void printCanFrame(
    const char *direction,
    unsigned long id,
    bool extended,
    byte dlc,
    const byte *data
);

void processSerial();
void processCommand(String command);

bool parseOnOff(String value, bool &result);

void printState();
void printLighting();
void printClock();
void printMfsw();
void printMessages();
void printHelp();
void sendBcConfig();
void sendBcFunctionList();
void sendBcHeartbeatConfig();
void sendBcTripDistance();
void updateBcTripData();

// ============================================================
// ESP32-S3 CAN / WIFI / WEB / OTA INFRASTRUCTURE
// ============================================================

void initCan();
void initNetwork();
void initWebUi();
void initOta();

String makeStateJson();

void initCan()
{
    Serial.print(F("Initializing TWAI 500 kbit/s... "));

    twai_general_config_t general =
        TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);

    general.tx_queue_len = 32;
    general.rx_queue_len = 64;

    twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t result = twai_driver_install(&general, &timing, &filter);

    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
    {
        Serial.print(F("install failed: "));
        Serial.println((int)result);
        return;
    }

    result = twai_start();

    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
    {
        Serial.print(F("start failed: "));
        Serial.println((int)result);
        return;
    }

    canReady = true;
    Serial.println(F("OK"));

    Serial.print(F("TWAI TX GPIO: "));
    Serial.println((int)CAN_TX_PIN);
    Serial.print(F("TWAI RX GPIO: "));
    Serial.println((int)CAN_RX_PIN);
    Serial.println();
}


// ============================================================
// NETWORK
// ============================================================

void initNetwork()
{
    WiFi.mode(WIFI_MODE_NULL);
    delay(100);

    bool connected = false;

    if (strlen(WIFI_SSID) > 0)
    {
        Serial.print(F("Connecting WiFi: "));
        Serial.println(WIFI_SSID);

        WiFi.mode(WIFI_STA);
        WiFi.setHostname(OTA_HOSTNAME);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

        unsigned long start = millis();

        while (WiFi.status() != WL_CONNECTED && (millis() - start) < 12000UL)
        {
            delay(250);
            Serial.print('.');
        }

        Serial.println();

        if (WiFi.status() == WL_CONNECTED)
        {
            connected = true;
            networkMode = "STA";
            networkIp = WiFi.localIP();

            Serial.print(F("WiFi connected: "));
            Serial.println(networkIp);
        }
    }

    if (!connected)
    {
        Serial.println(F("Starting fallback access point..."));

        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASSWORD);

        networkMode = "AP";
        networkIp = WiFi.softAPIP();

        Serial.print(F("AP SSID: "));
        Serial.println(AP_SSID);
        Serial.print(F("AP IP  : "));
        Serial.println(networkIp);
    }
}


// ============================================================
// WEB UI
// ============================================================

static const char WEB_PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MQB CAN Emulator</title>
<style>
body{font-family:system-ui,Arial,sans-serif;margin:0;background:#111827;color:#e5e7eb}
main{max-width:1050px;margin:auto;padding:18px}
h1{font-size:24px;margin:0 0 6px}
small{color:#9ca3af}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:12px;margin-top:16px}
.card{background:#1f2937;border:1px solid #374151;border-radius:12px;padding:14px}
.row{display:flex;justify-content:space-between;gap:10px;margin:7px 0}
button{border:0;border-radius:8px;padding:9px 12px;margin:4px;background:#374151;color:#fff;cursor:pointer}
button:hover{background:#4b5563}
input{background:#111827;color:#fff;border:1px solid #4b5563;border-radius:7px;padding:8px}
pre{white-space:pre-wrap;word-break:break-word;background:#0b1220;padding:10px;border-radius:8px}
a{color:#93c5fd}
</style>
</head>
<body>
<main>
<h1>MQB CAN Emulator</h1>
<small>ESP32-S3 · internal TWAI · Infotainment CAN</small>

<div class="grid">
<div class="card">
<h3>Vehicle</h3>
<div class="row"><span>Terminal 15</span><b id="t15">-</b></div>
<div class="row"><span>Engine</span><b id="eng">-</b></div>
<div class="row"><span>RPM</span><b id="rpm">-</b></div>
<div class="row"><span>Speed</span><b id="speed">-</b></div>
<div class="row"><span>Reverse</span><b id="rev">-</b></div>
<div class="row"><span>Handbrake</span><b id="hb">-</b></div>
<button onclick="cmd('on')">Ignition ON</button>
<button onclick="cmd('off')">Ignition OFF</button>
<button onclick="cmd('engine on')">Engine ON</button>
<button onclick="cmd('engine off')">Engine OFF</button>
</div>

<div class="card">
<h3>CAN</h3>
<div class="row"><span>Ready</span><b id="canready">-</b></div>
<div class="row"><span>RX</span><b id="rx">-</b></div>
<div class="row"><span>TX</span><b id="tx">-</b></div>
<div class="row"><span>TX errors</span><b id="txe">-</b></div>
<button onclick="cmd('simulategateway on')">Simulate Gateway ON</button>
<button onclick="cmd('simulategateway off')">Simulate Gateway OFF</button>
</div>

<div class="card">
<h3>MFSW</h3>
<button onclick="cmd('mfsw volup')">Volume +</button>
<button onclick="cmd('mfsw voldown')">Volume -</button>
<button onclick="cmd('mfsw previous')">Previous</button>
<button onclick="cmd('mfsw next')">Next</button>
<button onclick="cmd('mfsw phone')">Phone</button>
<button onclick="cmd('mfsw voice')">Voice</button>
<button onclick="cmd('mfsw left')">Left</button>
<button onclick="cmd('mfsw right')">Right</button>
<button onclick="cmd('mfsw up')">Up</button>
<button onclick="cmd('mfsw down')">Down</button>
<button onclick="cmd('mfsw ok')">OK</button>
</div>

<div class="card">
<h3>Bench functions</h3>
<button onclick="cmd('lights on')">Lights ON</button>
<button onclick="cmd('lights off')">Lights OFF</button>
<button onclick="cmd('reverse on')">Reverse ON</button>
<button onclick="cmd('reverse off')">Reverse OFF</button>
<button onclick="cmd('handbrake on')">Handbrake ON</button>
<button onclick="cmd('handbrake off')">Handbrake OFF</button>
<button onclick="cmd('bcprov on')">BC_MFA 0x27 ON</button>
<button onclick="cmd('bcprov off')">BC_MFA 0x27 OFF</button>
</div>

<div class="card">
<h3>Trip data lab</h3>
<div class="row"><span>Provider</span><b>BC_MFA 0x27</b></div>
<div class="row"><span>Trip mapping</span><b>Not confirmed yet</b></div>
<input id="tripkm" type="number" step="0.01" value="123.40" style="width:45%">
<button onclick="sendTrip()">Send km</button>
<button onclick="cmd('bctrip on')">Periodic ON</button>
<button onclick="cmd('bctrip off')">Periodic OFF</button>
<p><small>Trip-data TX is intentionally disabled until the 0x27 function mapping is confirmed.</small></p>
</div>

<div class="card">
<h3>VCDS identity</h3>
<div class="row"><span>DID</span><b>F197</b></div>
<div class="row"><span>Component</span><b>MQB-PQ-BRIDGE</b></div>
<button onclick="cmd('diagspoof on')">Spoof ON</button>
<button onclick="cmd('diagspoof off')">Spoof OFF</button>
<p><small>Single-CAN bench test only. Do not enable while the real MIB is also replying on the same bus.</small></p>
</div>

<div class="card">
<h3>Raw command</h3>
<input id="raw" style="width:70%" placeholder="e.g. rpm 1200">
<button onclick="sendRaw()">Send</button>
<pre id="result">Ready</pre>
</div>

<div class="card">
<h3>Firmware</h3>
<div class="row"><span>Network</span><b id="net">-</b></div>
<div class="row"><span>IP</span><b id="ip">-</b></div>
<p><a href="/update">Open firmware update page</a></p>
<p><small>ArduinoOTA is also active.</small></p>
</div>
</div>
</main>

<script>
async function cmd(c){
  const r=await fetch('/api/command?cmd='+encodeURIComponent(c));
  result.textContent=await r.text();
  setTimeout(refresh,120);
}
function sendRaw(){cmd(document.getElementById('raw').value)}
function sendTrip(){cmd('bctrip '+document.getElementById('tripkm').value)}
async function refresh(){
  try{
    const s=await (await fetch('/api/state')).json();
    const yn=v=>v?'ON':'OFF';
    t15.textContent=yn(s.terminal15);
    eng.textContent=yn(s.engineRunning);
    rpm.textContent=s.rpm;
    speed.textContent=s.speedKph.toFixed(1)+' km/h';
    rev.textContent=yn(s.reverse);
    hb.textContent=yn(s.handbrake);
    canready.textContent=yn(s.canReady);
    rx.textContent=s.canRx;
    tx.textContent=s.canTx;
    txe.textContent=s.canTxErrors;
    net.textContent=s.networkMode;
    ip.textContent=s.ip;
  }catch(e){}
}
setInterval(refresh,1000); refresh();
</script>
</body>
</html>
)HTML";

String makeStateJson()
{
    String json;
    json.reserve(420);

    json += '{';

    json += F("\"terminal15\":");
    json += vehicle.terminal15 ? F("true") : F("false");

    json += F(",\"engineRunning\":");
    json += vehicle.engineRunning ? F("true") : F("false");

    json += F(",\"rpm\":");
    json += vehicle.engineRpm;

    json += F(",\"speedKph\":");
    json += String(benchData.speedKph, 1);

    json += F(",\"reverse\":");
    json += benchData.reverse ? F("true") : F("false");

    json += F(",\"handbrake\":");
    json += benchData.parkingBrake ? F("true") : F("false");

    json += F(",\"canReady\":");
    json += canReady ? F("true") : F("false");

    json += F(",\"canRx\":");
    json += canRxCount;

    json += F(",\"canTx\":");
    json += canTxCount;

    json += F(",\"canTxErrors\":");
    json += canTxErrorCount;

    json += F(",\"networkMode\":\"");
    json += networkMode;
    json += '"';

    json += F(",\"ip\":\"");
    json += networkIp.toString();
    json += '"';

    json += '}';
    return json;
}

void initWebUi()
{
    if (WiFi.getMode() == WIFI_MODE_NULL)
    {
        return;
    }

    webServer.on("/", HTTP_GET, []()
    {
        webServer.send_P(200, "text/html", WEB_PAGE);
    });

    webServer.on("/api/state", HTTP_GET, []()
    {
        webServer.send(200, "application/json", makeStateJson());
    });

    webServer.on("/api/command", HTTP_GET, []()
    {
        if (!webServer.hasArg("cmd"))
        {
            webServer.send(400, "text/plain", "Missing cmd");
            return;
        }

        String command = webServer.arg("cmd");
        command.trim();

        if (command.length() == 0)
        {
            webServer.send(400, "text/plain", "Empty command");
            return;
        }

        processCommand(command);

        webServer.send(
            200,
            "text/plain",
            String("Command accepted: ") + command
        );
    });

    webServer.on("/update", HTTP_GET, []()
    {
        static const char UPDATE_PAGE[] PROGMEM = R"UPD(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Firmware Update</title></head>
<body style="font-family:system-ui;max-width:700px;margin:40px auto;padding:0 15px">
<h2>MQB Emulator firmware update</h2>
<p>Select a compiled ESP32-S3 firmware <code>.bin</code>.</p>
<form method="POST" action="/update" enctype="multipart/form-data">
<input type="file" name="update" accept=".bin" required>
<input type="submit" value="Upload firmware">
</form>
<p><a href="/">Back</a></p>
</body></html>
)UPD";

        webServer.send_P(200, "text/html", UPDATE_PAGE);
    });

    webServer.on(
        "/update",
        HTTP_POST,
        []()
        {
            bool ok = !Update.hasError();

            webServer.send(
                ok ? 200 : 500,
                "text/plain",
                ok ? "Update complete. Rebooting..." : "Update failed."
            );

            if (ok)
            {
                delay(500);
                ESP.restart();
            }
        },
        []()
        {
            HTTPUpload &upload = webServer.upload();

            if (upload.status == UPLOAD_FILE_START)
            {
                Serial.printf("Web OTA start: %s\n", upload.filename.c_str());

                if (!Update.begin(UPDATE_SIZE_UNKNOWN))
                {
                    Update.printError(Serial);
                }
            }
            else if (upload.status == UPLOAD_FILE_WRITE)
            {
                if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
                {
                    Update.printError(Serial);
                }
            }
            else if (upload.status == UPLOAD_FILE_END)
            {
                if (Update.end(true))
                {
                    Serial.printf("Web OTA complete: %u bytes\n", upload.totalSize);
                }
                else
                {
                    Update.printError(Serial);
                }
            }
        }
    );

    webServer.onNotFound([]()
    {
        webServer.send(404, "text/plain", "Not found");
    });

    webServer.begin();
    webReady = true;

    Serial.println(F("Web UI started"));
}


// ============================================================
// ARDUINO OTA
// ============================================================

void initOta()
{
    if (WiFi.getMode() == WIFI_MODE_NULL)
    {
        return;
    }

    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]()
    {
        Serial.println(F("ArduinoOTA start"));
    });

    ArduinoOTA.onEnd([]()
    {
        Serial.println(F("ArduinoOTA complete"));
    });

    ArduinoOTA.onError([](ota_error_t error)
    {
        Serial.print(F("ArduinoOTA error: "));
        Serial.println((unsigned int)error);
    });

    ArduinoOTA.begin();
    otaReady = true;

    Serial.println(F("ArduinoOTA started"));
}


// ============================================================
// GATEWAY SIMULATION MASTER SWITCH
// ============================================================

bool isGatewaySimulationMessage(const CanMessage &message)
{
    if (message.extended && message.id == 0x1B000010UL)
    {
        return true;
    }

    if (!message.extended)
    {
        switch (message.id)
        {
            case 0x3DA:
            case 0x3DB:
            case 0x3DC:
            case 0x3EA:
            case 0x585:
            case 0x663:
                return true;
        }
    }

    return false;
}

void setGatewaySimulation(bool enabled)
{
    simulateGatewayEnabled = enabled;

    // 0x3C0 itself remains part of the normal emulator message set.
    // The master switch changes its terminal state through the existing
    // ignition helper.
    if (enabled)
    {
        setIgnitionOn();
    }
    else
    {
        setIgnitionOff();
    }

    for (byte i = 0; i < MESSAGE_COUNT; i++)
    {
        if (!isGatewaySimulationMessage(messages[i]))
        {
            continue;
        }

        messages[i].enabled = enabled;

        if (enabled)
        {
            messages[i].lastSend = 0;
        }
    }

    Serial.println();
    Serial.println(F("======================================"));
    Serial.print(F("SIMULATE GATEWAY: "));
    Serial.println(enabled ? F("ON") : F("OFF"));
    Serial.println(F("======================================"));

    if (enabled)
    {
        Serial.println(F("Gateway NM     : 0x1B000010 @ 200 ms"));
        Serial.println(F("Ignition       : Terminal S + 15 ON via 0x3C0"));
        Serial.println(F("Captured state : 0x3DA 0x3DB 0x3DC 0x3EA 0x585 0x663"));
    }
    else
    {
        Serial.println(F("Gateway NM and captured state replay disabled."));
        Serial.println(F("Ignition state set OFF."));
    }

    Serial.println();
}


// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println(F("======================================"));
    Serial.println(F("MQB CAN Emulator v1.1.6"));
    Serial.println(F("ESP32-S3 / internal TWAI"));
    Serial.println(F("======================================"));
    Serial.println();

    initCan();
    initNetwork();
    initWebUi();
    initOta();

    // Default bench behavior: emulate the gateway/vehicle-network presence
    // immediately after boot. This is intentionally ON by default.
    //
    // IMPORTANT: use this only with the physical MQB gateway disconnected.
    setGatewaySimulation(true);

    unsigned long now = millis();

    // Stagger startup traffic.
    for (byte i = 0; i < MESSAGE_COUNT; i++)
    {
        messages[i].lastSend = now + ((unsigned long)i * 3UL);
    }

    lastMotor04 = now + 7UL;
    lastMotor07 = now + 17UL;
    lastMotor20 = now + 27UL;
    lastMotor26 = now + 37UL;

    clockState.lastTick = now;
    mfswLastSend = now;

    printState();
    printHelp();

    Serial.println();

    if (webReady)
    {
        Serial.print(F("Web UI: http://"));
        Serial.print(networkIp);
        Serial.println(F("/"));
    }
}

void loop()
{
    updateClock();
    updateMfsw();

    sendMessages();
    sendExtendedVehicleMessages();
    updateBcTripData();

    monitorCan();
    processSerial();

    if (webReady)
    {
        webServer.handleClient();
    }

    if (otaReady)
    {
        ArduinoOTA.handle();
    }

    delay(1);
}


// ============================================================
// GENERIC INTEL/LITTLE-ENDIAN BIT WRITER
// ============================================================

void setBitsIntel(byte *data, byte startBit, byte length, uint32_t value)
{
    for (byte i = 0; i < length; i++)
    {
        byte absoluteBit = startBit + i;
        byte byteIndex = absoluteBit / 8;
        byte bitIndex = absoluteBit % 8;
        byte mask = (1 << bitIndex);

        if ((value >> i) & 0x01UL)
        {
            data[byteIndex] |= mask;
        }
        else
        {
            data[byteIndex] &= ~mask;
        }
    }
}


// ============================================================
// PERIODIC MESSAGE SCHEDULER
// ============================================================

void sendMessages()
{
    unsigned long now = millis();

    for (byte i = 0; i < MESSAGE_COUNT; i++)
    {
        CanMessage &message = messages[i];

        if (!message.enabled)
        {
            continue;
        }

        if ((now - message.lastSend) >= message.interval)
        {
            message.lastSend = now;

            if (message.update != NULL)
            {
                message.update(message);
            }

            sendCanMessage(message);
        }
    }
}


// ============================================================
// CAN TRANSMISSION
// ============================================================

bool sendCanMessage(CanMessage &message)
{
    return sendRawCan(message.id, message.extended, message.dlc, message.data);
}

bool sendRawCan(unsigned long id, bool extended, byte dlc, const byte *data)
{
    if (!canReady || dlc > 8)
    {
        return false;
    }

    twai_message_t message = {};
    message.identifier = id & 0x1FFFFFFFUL;
    message.extd = extended ? 1 : 0;
    message.rtr = 0;
    message.data_length_code = dlc;

    for (byte i = 0; i < dlc; i++)
    {
        message.data[i] = data[i];
    }

    esp_err_t result = twai_transmit(&message, pdMS_TO_TICKS(5));

    if (result != ESP_OK)
    {
        canTxErrorCount++;

        Serial.print(F("CAN TX ERROR: 0x"));
        Serial.println(id, HEX);
        return false;
    }

    canTxCount++;

    if (monitorMode == MONITOR_ALL || monitorMode == MONITOR_TX)
    {
        printCanFrame("TX", id, extended, dlc, data);
    }

    return true;
}


// ============================================================
// VCDS / UDS DID F197 COMPONENT IDENTITY SPOOF
// ============================================================

void processDiagnosticSpoof(
    unsigned long id,
    bool extended,
    byte dlc,
    const byte *data
)
{
    if (!diagIdentitySpoofEnabled ||
        extended ||
        id != VCDS_5F_REQUEST_ID ||
        dlc == 0)
    {
        return;
    }

    // ISO-TP single-frame UDS request:
    //   03 22 F1 97
    if (dlc >= 4 &&
        data[0] == 0x03 &&
        data[1] == 0x22 &&
        data[2] == 0xF1 &&
        data[3] == 0x97)
    {
        sendDiagF197FirstFrame();
        diagF197AwaitingFlowControl = true;

        Serial.println(F("[DIAG] VCDS requested DID F197"));
        Serial.println(F("[DIAG] Sending component identity: MQB-PQ-BRIDGE"));
        return;
    }

    // ISO-TP flow control from tester:
    //   30 BS STmin ...
    if (diagF197AwaitingFlowControl &&
        dlc >= 3 &&
        (data[0] & 0xF0) == 0x30)
    {
        const byte flowStatus = data[0] & 0x0F;

        if (flowStatus == 0x00)
        {
            sendDiagF197ConsecutiveFrames();
        }
        else
        {
            Serial.print(F("[DIAG] F197 flow control status = 0x"));
            Serial.println(flowStatus, HEX);
        }

        diagF197AwaitingFlowControl = false;
    }
}

void sendDiagF197FirstFrame()
{
    // UDS payload:
    //   62 F1 97 + 13 ASCII bytes = 16 bytes total.
    //
    // ISO-TP first frame:
    //   10 10 | 62 F1 97 | "MQB"
    const byte frame[8] =
    {
        0x10, 0x10,
        0x62, 0xF1, 0x97,
        (byte)DIAG_COMPONENT_NAME[0],
        (byte)DIAG_COMPONENT_NAME[1],
        (byte)DIAG_COMPONENT_NAME[2]
    };

    sendRawCan(VCDS_5F_RESPONSE_ID, false, 8, frame);
}

void sendDiagF197ConsecutiveFrames()
{
    // Remaining 10 characters after "MQB":
    //   "-PQ-BRIDGE"
    const byte frame21[8] =
    {
        0x21,
        (byte)DIAG_COMPONENT_NAME[3],
        (byte)DIAG_COMPONENT_NAME[4],
        (byte)DIAG_COMPONENT_NAME[5],
        (byte)DIAG_COMPONENT_NAME[6],
        (byte)DIAG_COMPONENT_NAME[7],
        (byte)DIAG_COMPONENT_NAME[8],
        (byte)DIAG_COMPONENT_NAME[9]
    };

    const byte frame22[8] =
    {
        0x22,
        (byte)DIAG_COMPONENT_NAME[10],
        (byte)DIAG_COMPONENT_NAME[11],
        (byte)DIAG_COMPONENT_NAME[12],
        0xAA, 0xAA, 0xAA, 0xAA
    };

    sendRawCan(VCDS_5F_RESPONSE_ID, false, 8, frame21);

    // The capture used STmin = 1 ms.
    delay(1);

    sendRawCan(VCDS_5F_RESPONSE_ID, false, 8, frame22);
}


// ============================================================
// KLEMMEN_STATUS_01 - 0x3C0
// ============================================================

void updateKlemmenStatus(CanMessage &message)
{
    byte terminalFlags = 0x00;

    if (vehicle.terminalS)  terminalFlags |= (1 << 0);
    if (vehicle.terminal15) terminalFlags |= (1 << 1);
    if (vehicle.terminalX)  terminalFlags |= (1 << 2);
    if (vehicle.terminal50) terminalFlags |= (1 << 3);

    message.data[0] = klemmenChecksum[klemmenCounter];
    message.data[1] = klemmenCounter & 0x0F;
    message.data[2] = terminalFlags;
    message.data[3] = 0x00;

    klemmenCounter++;

    if (klemmenCounter > 15)
    {
        klemmenCounter = 0;
    }
}


// ============================================================
// MOTOR_14 - 0x3BE
// ============================================================

void updateMotor14(CanMessage &message)
{
    for (byte i = 0; i < 8; i++)
    {
        message.data[i] = 0x00;
    }

    message.data[1] |= (vehicle.startStopStatus & 0x03) << 4;

    if (vehicle.terminal75)
    {
        message.data[2] |= (1 << 2);
    }

    if (vehicle.terminal50)
    {
        message.data[2] |= (1 << 3);
    }

    message.data[3] |= vehicle.startStopDriverRequest & 0x03;

    if (vehicle.engineRunning)
    {
        message.data[4] |= (1 << 7);
    }

    // DBC: MO_Kuppl_schalter bit 37, MO_Kickdown bit 40.
    setBitsIntel(message.data, 37, 1, extData.clutch ? 1 : 0);
    setBitsIntel(message.data, 40, 1, extData.kickdown ? 1 : 0);
}


// ============================================================
// DIMMUNG_01 - 0x5F0
// ============================================================

void updateDimming01(CanMessage &message)
{
    for (byte i = 0; i < 8; i++)
    {
        message.data[i] = 0x00;
    }

    /*
     * DBC-confirmed mapping:
     *
     * bits 0..7   DI_KL_58xd              raw 0..253
     * bits 8..14  DI_KL_58xs              0..100 %
     * bit  15     DI_Display_Nachtdesign
     * bits 16..22 DI_KL_58xt              0..100 %
     * bits 24..39 DI_Fotosensor
     *
     * Important v4 change:
     * DI_KL_58xd is no longer treated as a percentage.
     *
     * Volkswagen-group display simulation documentation uses:
     *   58xd = 0xFD
     *   58xs = 100
     *   58xt = 100
     *
     * lights off explicitly drives all dimming values to zero.
     */

    if (!lighting.lightsEnabled)
    {
        return;
    }

    message.data[0] = lighting.dimming58xd;
    message.data[1] = lighting.dimming58xs & 0x7F;

    if (lighting.nightDesign)
    {
        message.data[1] |= 0x80;
    }

    message.data[2] = lighting.dimming58xt & 0x7F;
    message.data[3] = lighting.photoSensor & 0xFF;
    message.data[4] = (lighting.photoSensor >> 8) & 0xFF;
}


// ============================================================
// DIAGNOSE_01 - 0x6B2
// ============================================================

void updateDiagnose01(CanMessage &message)
{
    for (byte i = 0; i < 8; i++)
    {
        message.data[i] = 0x00;
    }

    /*
     * MQB DBC time/date mapping:
     *
     * bits 28..34  UH_Jahr
     * bits 35..38  UH_Monat
     * bits 39..43  UH_Tag
     * bits 44..48  UH_Stunde
     * bits 49..54  UH_Minute
     * bits 55..60  UH_Sekunde
     *
     * DBC scaling for UH_Jahr is factor 1, offset 2000.
     * Therefore the raw value is year - 2000.
     */

    uint16_t yearRaw = 0;

    if (clockState.year >= 2000)
    {
        yearRaw = clockState.year - 2000;
    }

    if (yearRaw > 127)
    {
        yearRaw = 127;
    }

    setBitsIntel(message.data, 28, 7, yearRaw);
    setBitsIntel(message.data, 35, 4, clockState.month);
    setBitsIntel(message.data, 39, 5, clockState.day);
    setBitsIntel(message.data, 44, 5, clockState.hour);
    setBitsIntel(message.data, 49, 6, clockState.minute);
    setBitsIntel(message.data, 55, 6, clockState.second);
}


// ============================================================
// EINHEITEN_01 - 0x643
// ============================================================

void updateEinheiten01(CanMessage &message)
{
    for (byte i = 0; i < 8; i++)
    {
        message.data[i] = 0x00;
    }

    /*
     * Exact DBC bit positions:
     *
     * 0|2   KBI_Einheit_Datum
     * 2|2   KBI_Einheit_Druck
     * 4|1   KBI_Einheit_Streckenanz
     * 5|1   KBI_MFA_v_Einheit_02
     * 6|1   KBI_Einheit_Temp
     * 7|1   KBI_Einheit_Uhrzeit
     * 8|2   KBI_Einheit_Verbrauch
     * 10|2  KBI_Einheit_Volumen
     * 16|8  KBI_Einheit_Sprache
     */

    setBitsIntel(message.data, 0, 2, units.dateFormat & 0x03);
    setBitsIntel(message.data, 2, 2, units.pressureUnit & 0x03);
    setBitsIntel(message.data, 4, 1, units.distanceMiles ? 1 : 0);
    setBitsIntel(message.data, 5, 1, units.mfaSpeedMph ? 1 : 0);
    setBitsIntel(message.data, 6, 1, units.temperatureF ? 1 : 0);
    setBitsIntel(message.data, 7, 1, units.clock12h ? 1 : 0);
    setBitsIntel(message.data, 8, 2, units.consumptionUnit & 0x03);
    setBitsIntel(message.data, 10, 2, units.volumeUnit & 0x03);
    setBitsIntel(message.data, 16, 8, units.language);
}


// ============================================================
// BCM_01 - 0x65A
// ============================================================

void updateBCM01(CanMessage &message)
{
    for (byte i = 0; i < 8; i++)
    {
        message.data[i] = 0x00;
    }

    // BCM1_Kl_15_HW_erkannt, bit 17.
    setBitsIntel(message.data, 17, 1, vehicle.terminal15 ? 1 : 0);

    // BCM1_Rueckfahrlicht_Schalter, bit 30.
    setBitsIntel(message.data, 30, 1, benchData.reverse ? 1 : 0);
}


// ============================================================
// KOMBI_01 - 0x30B
// ============================================================

void updateKombi01(CanMessage &message)
{
    for (byte i = 0; i < 8; i++)
    {
        message.data[i] = 0x00;
    }

    // Kombi_01_BZ, bits 8..11.
    setBitsIntel(message.data, 8, 4, kombi01Counter & 0x0F);

    // KBI_Handbremse, bit 23.
    setBitsIntel(message.data, 23, 1, benchData.parkingBrake ? 1 : 0);

    // KBI_V_Digital, bits 24..32, raw 1 km/h.
    uint16_t digitalSpeed = (uint16_t)(benchData.speedKph + 0.5);

    if (digitalSpeed > 511)
    {
        digitalSpeed = 511;
    }

    setBitsIntel(message.data, 24, 9, digitalSpeed);

    // KBI_angez_Geschw, bits 48..57, scale 0.32 km/h per bit.
    uint16_t displaySpeed = (uint16_t)((benchData.speedKph / 0.32) + 0.5);

    if (displaySpeed > 1023)
    {
        displaySpeed = 1023;
    }

    setBitsIntel(message.data, 48, 10, displaySpeed);

    // DBC-confirmed warning lamp bits.
    setBitsIntel(message.data, 0, 1, extData.absLamp ? 1 : 0);
    setBitsIntel(message.data, 1, 1, extData.espLamp ? 1 : 0);
    setBitsIntel(message.data, 3, 1, extData.airbagLamp ? 1 : 0);
    setBitsIntel(message.data, 5, 1, extData.steeringLamp ? 1 : 0);
    setBitsIntel(message.data, 22, 1, extData.oilWarning ? 1 : 0);

    // KBI_Einheit_Tacho, bit 58. 0 is used for metric bench mode.
    setBitsIntel(message.data, 58, 1, units.distanceMiles ? 1 : 0);

    kombi01Counter = (kombi01Counter + 1) & 0x0F;
}


// ============================================================
// KOMBI_02 - 0x6B7
// ============================================================

void updateKombi02(CanMessage &message)
{
    for (byte i = 0; i < 8; i++)
    {
        message.data[i] = 0x00;
    }

    // KBI_Inhalt_Tank, bits 40..46, liters.
    byte fuel = benchData.fuelLiters;

    if (fuel > 125)
    {
        fuel = 125;
    }

    setBitsIntel(message.data, 40, 7, fuel);

    // KBI_FStatus_Tank, bit 47. Keep zero = normal/valid test state.
    setBitsIntel(message.data, 47, 1, 0);

    // KBI_QBit_Aussen_Temp_gef, bit 55.
    setBitsIntel(message.data, 55, 1, 1);

    // KBI_Aussen_Temp_gef, bits 56..63, factor 0.5, offset -50.
    float temp = benchData.outsideTemperatureC;

    if (temp < -50.0) temp = -50.0;
    if (temp > 75.0) temp = 75.0;

    byte rawTemp = (byte)(((temp + 50.0) / 0.5) + 0.5);
    setBitsIntel(message.data, 56, 8, rawTemp);
}


// ============================================================
// EXTENDED DBC MESSAGES
// ============================================================

void sendExtendedVehicleMessages()
{
    unsigned long now = millis();
    if ((now - lastMotor04) >= 100) { lastMotor04 = now; sendMotor04(); }
    if ((now - lastMotor07) >= 500) { lastMotor07 = now; sendMotor07(); }
    if ((now - lastMotor20) >= 100) { lastMotor20 = now; sendMotor20(); }
    if ((now - lastMotor26) >= 500) { lastMotor26 = now; sendMotor26(); }
}

void sendMotor04()
{
    byte d[8] = {0};
    uint16_t oilRaw = extData.oilPressureCentiBar / 4; // 0.04 bar/bit
    if (oilRaw > 250) oilRaw = 250;
    uint16_t rpmRaw = vehicle.engineRpm / 3;           // 3 rpm/bit
    if (rpmRaw > 4094) rpmRaw = 4094;
    uint16_t boostRaw = extData.boostCentiBar;         // 0.01 bar/bit
    if (boostRaw > 510) boostRaw = 510;
    setBitsIntel(d, 16, 8, oilRaw);
    setBitsIntel(d, 24, 12, rpmRaw);
    setBitsIntel(d, 39, 9, boostRaw);
    sendRawCan(0x107, false, 8, d);
}

void sendMotor07()
{
    byte d[8] = {0};
    int16_t iat10 = extData.intakeTempC10;
    int16_t oil10 = extData.oilTempC10;
    int16_t coolant10 = extData.coolantTempC10;
    if (iat10 < -480) iat10 = -480; if (iat10 > 1417) iat10 = 1417;
    if (oil10 < -600) oil10 = -600; if (oil10 > 1920) oil10 = 1920;
    if (coolant10 < -480) coolant10 = -480; if (coolant10 > 1417) coolant10 = 1417;
    byte iatRaw = (byte)(((long)iat10 + 480L) * 4L / 30L);       // 0.75 C/bit, -48
    byte oilRaw = (byte)((oil10 + 600) / 10);                    // 1 C/bit, -60
    byte coolantRaw = (byte)(((long)coolant10 + 480L) * 4L / 30L);
    setBitsIntel(d, 0, 1, 1); setBitsIntel(d, 1, 1, 1); setBitsIntel(d, 2, 1, 1);
    setBitsIntel(d, 8, 8, iatRaw);
    setBitsIntel(d, 16, 8, oilRaw);
    setBitsIntel(d, 24, 8, coolantRaw);
    sendRawCan(0x640, false, 8, d);
}

void sendMotor20()
{
    byte d[8] = {0};
    byte raw = (byte)(((uint16_t)extData.throttlePercent * 10U + 2U) / 4U); // 0.4 %/bit
    setBitsIntel(d, 12, 8, raw);
    setBitsIntel(d, 20, 1, 1); // valid
    sendRawCan(0x121, false, 8, d);
}

void sendMotor26()
{
    byte d[8] = {0};
    byte level = extData.oilLevelPercent;
    if (level > 100) level = 100;
    byte levelRaw = (byte)((level + 6) / 12.5); // 12.5 %/bit, rounded approximately
    if (levelRaw > 8) levelRaw = 8;
    setBitsIntel(d, 12, 1, 1); // WIV display active
    setBitsIntel(d, 13, 1, extData.oilWarning ? 1 : 0);
    setBitsIntel(d, 16, 4, levelRaw);
    setBitsIntel(d, 24, 1, 1); // oil system active
    setBitsIntel(d, 27, 1, vehicle.engineRunning ? 1 : 0);
    setBitsIntel(d, 48, 1, extData.engineWarning ? 1 : 0);
    setBitsIntel(d, 49, 1, extData.mil ? 1 : 0);
    setBitsIntel(d, 56, 1, extData.oilWarning ? 1 : 0);
    setBitsIntel(d, 60, 1, extData.oilWarning ? 1 : 0);
    sendRawCan(0x3C7, false, 8, d);
}

// ============================================================
// CLOCK
// ============================================================

void updateClock()
{
    if (!clockState.running)
    {
        return;
    }

    unsigned long now = millis();

    while ((now - clockState.lastTick) >= 1000)
    {
        clockState.lastTick += 1000;
        incrementClockOneSecond();
    }
}

void incrementClockOneSecond()
{
    clockState.second++;

    if (clockState.second < 60)
    {
        return;
    }

    clockState.second = 0;
    clockState.minute++;

    if (clockState.minute < 60)
    {
        return;
    }

    clockState.minute = 0;
    clockState.hour++;

    if (clockState.hour < 24)
    {
        return;
    }

    clockState.hour = 0;
    clockState.day++;

    if (clockState.day <= daysInMonth(clockState.year, clockState.month))
    {
        return;
    }

    clockState.day = 1;
    clockState.month++;

    if (clockState.month <= 12)
    {
        return;
    }

    clockState.month = 1;
    clockState.year++;
}

bool isLeapYear(uint16_t year)
{
    if ((year % 400) == 0) return true;
    if ((year % 100) == 0) return false;
    return (year % 4) == 0;
}

byte daysInMonth(uint16_t year, byte month)
{
    switch (month)
    {
        case 1:  return 31;
        case 2:  return isLeapYear(year) ? 29 : 28;
        case 3:  return 31;
        case 4:  return 30;
        case 5:  return 31;
        case 6:  return 30;
        case 7:  return 31;
        case 8:  return 31;
        case 9:  return 30;
        case 10: return 31;
        case 11: return 30;
        case 12: return 31;
    }

    return 31;
}


// ============================================================
// MFSW - 0x5BF, GOLF MK7 INFOTAINMENT CAN CAPTURE FORMAT
// ============================================================

void startMfswEvent(byte buttonCode)
{
    unsigned long now = millis();

    mfsw.buttonCode = buttonCode;
    mfsw.releaseAt = now + MFSW_PRESS_TIME_MS;
    mfsw.active = true;
    mfsw.releasePending = true;

    // Send the first press frame immediately.
    sendMfswPressed(buttonCode);
    mfswLastSend = now;

    Serial.print(F("[MFSW] "));
    Serial.print(mfswButtonName(buttonCode));
    Serial.print(F(" (0x"));
    if (buttonCode < 0x10) Serial.print('0');
    Serial.print(buttonCode, HEX);
    Serial.println(')');
}

void updateMfsw()
{
    unsigned long now = millis();

    if ((now - mfswLastSend) < MFSW_PERIOD_MS)
    {
        return;
    }

    mfswLastSend = now;

    if (mfsw.active)
    {
        if ((long)(now - mfsw.releaseAt) < 0)
        {
            // Captured pressed format: XX 00 01 40.
            sendMfswPressed(mfsw.buttonCode);
            return;
        }

        if (mfsw.releasePending)
        {
            // Captured release format: 00 00 01 40.
            sendMfswRelease();
            mfsw.releasePending = false;
            mfsw.active = false;
            return;
        }
    }

    // Captured periodic idle format: 00 00 00 40.
    sendMfswIdle();
}

void sendMfswPressed(byte buttonCode)
{
    byte data[4] = { buttonCode, 0x00, 0x01, 0x40 };
    sendRawCan(0x5BF, false, 4, data);
}

void sendMfswRelease()
{
    byte data[4] = { 0x00, 0x00, 0x01, 0x40 };
    sendRawCan(0x5BF, false, 4, data);
}

void sendMfswIdle()
{
    byte data[4] = { 0x00, 0x00, 0x00, 0x40 };
    sendRawCan(0x5BF, false, 4, data);
}


// ============================================================
// IGNITION
// ============================================================

void setIgnitionOn()
{
    vehicle.terminalS = true;
    vehicle.terminal15 = true;
    vehicle.terminalX = false;
    vehicle.terminal50 = false;
    vehicle.terminal75 = true;

    Serial.println();
    Serial.println(F("======================================"));
    Serial.println(F("IGNITION ON"));
    Serial.println(F("======================================"));

    printState();
}

void setIgnitionOff()
{
    vehicle.terminalS = false;
    vehicle.terminal15 = false;
    vehicle.terminalX = false;
    vehicle.terminal50 = false;
    vehicle.terminal75 = false;

    vehicle.engineRunning = false;
    vehicle.engineRpm = 0;
    vehicle.startStopStatus = 0;
    vehicle.startStopDriverRequest = 0;

    Serial.println();
    Serial.println(F("======================================"));
    Serial.println(F("IGNITION OFF"));
    Serial.println(F("======================================"));

    printState();
}


// ============================================================
// CAN RECEIVE MONITOR
// ============================================================

void monitorCan()
{
    if (!canReady)
    {
        return;
    }

    twai_message_t rx = {};

    while (twai_receive(&rx, 0) == ESP_OK)
    {
        canRxCount++;

        bool extended = rx.extd != 0;
        unsigned long id = rx.identifier & 0x1FFFFFFFUL;
        byte len = rx.data_length_code;

        if (len > 8)
        {
            len = 8;
        }

        byte buffer[8] = {0};

        for (byte i = 0; i < len; i++)
        {
            buffer[i] = rx.data[i];
        }

        processDiagnosticSpoof(id, extended, len, buffer);
        decodeInfotainmentFeedback(id, extended, len, buffer);
        processBcProvider(id, extended, len, buffer);

        if (shouldPrintRxFrame(id, extended))
        {
            printCanFrame("RX", id, extended, len, buffer);
        }
    }
}

void decodeInfotainmentFeedback(
    unsigned long id,
    bool extended,
    byte dlc,
    const byte *data
)
{
    if (!extended)
    {
        return;
    }

    if (volumeFeedbackEnabled && id == 0x17333110UL)
    {
        decodeVolumeFeedback(dlc, data);
    }

    if (asciiDecoderEnabled)
    {
        decodeAsciiTransport(id, dlc, data);
    }

    if (mediaMetadataEnabled)
    {
        decodeMediaMetadata(id, dlc, data);
    }

    if (vehicleStatusProbeEnabled && vehicleStatusProbeActive)
    {
        decodeVehicleStatusProbe(id, dlc, data);
    }
}

void decodeVolumeFeedback(byte dlc, const byte *data)
{
    if (dlc < 3)
    {
        return;
    }

    // Periodic/event volume property:
    //   3C 52 XX ...
    //   4C 52 XX ...
    if ((data[0] == 0x3C || data[0] == 0x4C) && data[1] == 0x52)
    {
        byte volume = data[2];

        if (!volumeKnown)
        {
            volumeKnown = true;
            lastVolume = volume;

            Serial.print(F("[AUDIO] Volume = "));
            Serial.println(volume);
            return;
        }

        if (volume != lastVolume)
        {
            Serial.print(F("[AUDIO] Volume "));
            Serial.print(lastVolume);
            Serial.print(F(" -> "));
            Serial.println(volume);
            lastVolume = volume;
        }

        return;
    }

    // Observed acknowledgement:
    //   4C 6F 01 00 00 XX
    if (dlc >= 6 && data[0] == 0x4C && data[1] == 0x6F)
    {
        byte volume = data[5];

        if (!volumeAckKnown || volume != lastVolumeAck)
        {
            volumeAckKnown = true;
            lastVolumeAck = volume;

            Serial.print(F("[AUDIO] Volume ACK = "));
            Serial.println(volume);
        }
    }
}

void decodeAsciiTransport(unsigned long id, byte dlc, const byte *data)
{
    if (dlc == 0)
    {
        return;
    }

    // Limit the automatic decoder to the infotainment extended-ID family.
    // This prevents random printable bytes on unrelated buses from flooding
    // the serial monitor.
    if ((id & 0xFFFF0000UL) != 0x17330000UL)
    {
        return;
    }

    // Start of a segmented text message observed in the captures.
    // Known layout example:
    //   B0 0B 4A 11 0A 47 61 6C
    //                ^^ expected text length
    //                   "Gal"
    if ((data[0] & 0xF0) == 0xB0 && dlc >= 6)
    {
        asciiMessageActive = true;
        asciiActiveId = id;
        asciiExpectedLength = data[4];
        asciiPrintedLength = 0;

        if (id == 0x17332810UL)
        {
            Serial.print(F("[PHONE/TEXT] \""));
        }
        else
        {
            Serial.print(F("[TEXT 0x"));
            Serial.print(id, HEX);
            Serial.print(F(" ch=0x"));
            if (data[2] < 0x10) Serial.print('0');
            Serial.print(data[2], HEX);
            Serial.print(F(" idx=0x"));
            if (data[3] < 0x10) Serial.print('0');
            Serial.print(data[3], HEX);
            Serial.print(F("] \""));
        }

        printAsciiPayload(data, 5, dlc);

        if (asciiExpectedLength > 0 &&
            asciiPrintedLength >= asciiExpectedLength)
        {
            Serial.println('"');
            asciiMessageActive = false;
        }

        return;
    }

    // Continuation frames observed as F0, F1, F2 ...
    if (asciiMessageActive &&
        id == asciiActiveId &&
        (data[0] & 0xF0) == 0xF0)
    {
        printAsciiPayload(data, 1, dlc);

        if (asciiExpectedLength > 0 &&
            asciiPrintedLength >= asciiExpectedLength)
        {
            Serial.println('"');
            asciiMessageActive = false;
        }

        return;
    }
}

void printAsciiPayload(const byte *data, byte startIndex, byte dlc)
{
    for (byte i = startIndex; i < dlc; i++)
    {
        if (asciiExpectedLength > 0 &&
            asciiPrintedLength >= asciiExpectedLength)
        {
            break;
        }

        byte value = data[i];

        // Zero is commonly padding / string termination.
        if (value == 0x00)
        {
            if (asciiMessageActive)
            {
                Serial.println('"');
                asciiMessageActive = false;
            }
            return;
        }

        if (value >= 0x20 && value <= 0x7E)
        {
            Serial.write(value);
        }
        else
        {
            // Preserve the position without dumping binary control bytes.
            Serial.print('.');
        }

        asciiPrintedLength++;
    }
}



void decodeMediaMetadata(unsigned long id, byte dlc, const byte *data)
{
    if (id != 0x17333111UL || dlc == 0)
    {
        return;
    }

    unsigned long now = millis();

    // Drop an incomplete old transfer if the sender went quiet.
    if (mediaMessageActive && (now - mediaLastFrameMs) > 300UL)
    {
        resetMediaMetadata();
    }

    // Start frame observed for media metadata.
    // Header bytes 0..3 are transport/application information.
    // Payload starts at byte 4.
    if (data[0] == 0x90 && dlc >= 5)
    {
        resetMediaMetadata();
        mediaMessageActive = true;
        mediaExpectedSequence = 0;
        mediaLastFrameMs = now;

        appendMediaBytes(data, 4, dlc);
        tryPrintMediaMetadata();
        return;
    }

    // Continuation frames D0, D1, D2 ...
    if (mediaMessageActive && (data[0] & 0xF0) == 0xD0)
    {
        byte sequence = data[0] & 0x0F;

        // Normally sequential. If a frame is repeated, ignore it.
        // If a sequence is skipped, abandon this transfer rather than
        // printing corrupt metadata.
        if (sequence < mediaExpectedSequence)
        {
            return;
        }

        if (sequence != mediaExpectedSequence)
        {
            resetMediaMetadata();
            return;
        }

        mediaExpectedSequence++;
        mediaLastFrameMs = now;

        appendMediaBytes(data, 1, dlc);
        tryPrintMediaMetadata();
        return;
    }
}

void resetMediaMetadata()
{
    mediaMessageActive = false;
    mediaBufferLength = 0;
    mediaExpectedSequence = 0;
    mediaLastFrameMs = 0;
}

void appendMediaBytes(const byte *data, byte startIndex, byte dlc)
{
    for (byte i = startIndex; i < dlc; i++)
    {
        if (mediaBufferLength >= sizeof(mediaBuffer))
        {
            resetMediaMetadata();
            return;
        }

        mediaBuffer[mediaBufferLength++] = data[i];
    }
}

void printMediaField(
    const __FlashStringHelper *label,
    const byte *data,
    byte len
)
{
    Serial.print(label);
    Serial.print(F(": "));

    for (byte i = 0; i < len; i++)
    {
        byte value = data[i];

        if (value >= 0x20 && value <= 0x7E)
        {
            Serial.write(value);
        }
        else
        {
            Serial.print('.');
        }
    }

    Serial.println();
}

void tryPrintMediaMetadata()
{
    if (!mediaMessageActive || mediaBufferLength < 2)
    {
        return;
    }

    // Observed payload grammar:
    //
    //   [titleLen] [title...]
    //   48 00 00
    //   [artistLen] [artist...]
    //   49
    //   [albumLen] [album...]
    //   4A ...
    //
    // Do not print until all three complete strings and the final 0x4A
    // marker are present.

    byte pos = 0;

    byte titleLen = mediaBuffer[pos++];

    if (titleLen == 0 || titleLen > 64)
    {
        return;
    }

    if ((unsigned int)pos + titleLen > mediaBufferLength)
    {
        return;
    }

    byte titlePos = pos;
    pos += titleLen;

    if ((unsigned int)pos + 4 > mediaBufferLength)
    {
        return;
    }

    if (mediaBuffer[pos] != 0x48)
    {
        // Different 0x90 message type on the same CAN ID.
        // Keep collecting, but do not interpret it as media metadata.
        return;
    }

    pos++;

    // In the captured stream two zero bytes follow field marker 0x48.
    if (mediaBuffer[pos] == 0x00) pos++;
    if (pos < mediaBufferLength && mediaBuffer[pos] == 0x00) pos++;

    if (pos >= mediaBufferLength)
    {
        return;
    }

    byte artistLen = mediaBuffer[pos++];

    if (artistLen > 64)
    {
        return;
    }

    if ((unsigned int)pos + artistLen > mediaBufferLength)
    {
        return;
    }

    byte artistPos = pos;
    pos += artistLen;

    if ((unsigned int)pos + 2 > mediaBufferLength)
    {
        return;
    }

    if (mediaBuffer[pos++] != 0x49)
    {
        return;
    }

    byte albumLen = mediaBuffer[pos++];

    if (albumLen > 64)
    {
        return;
    }

    if ((unsigned int)pos + albumLen > mediaBufferLength)
    {
        return;
    }

    byte albumPos = pos;
    pos += albumLen;

    if (pos >= mediaBufferLength)
    {
        return;
    }

    if (mediaBuffer[pos] != 0x4A)
    {
        return;
    }

    Serial.println();
    Serial.println(F("[MEDIA UPDATE]"));
    printMediaField(F("Title "), &mediaBuffer[titlePos], titleLen);
    printMediaField(F("Artist"), &mediaBuffer[artistPos], artistLen);
    printMediaField(F("Album "), &mediaBuffer[albumPos], albumLen);
    Serial.println();

    resetMediaMetadata();
}



const __FlashStringHelper *mfswButtonName(byte code)
{
    switch (code)
    {
        case MFSW_RIGHT_MENU:  return F("Right");
        case MFSW_LEFT_MENU:   return F("Left");
        case MFSW_UP:          return F("Up");
        case MFSW_DOWN:        return F("Down");
        case MFSW_OK:          return F("OK");
        case MFSW_VOLUME_UP:   return F("Volume +");
        case MFSW_VOLUME_DOWN: return F("Volume -");
        case MFSW_NEXT:        return F("Next track");
        case MFSW_PREVIOUS:    return F("Previous track");
        case MFSW_VOICE:       return F("Voice");
        case MFSW_PHONE:       return F("Phone");
    }

    return F("Unknown");
}

void printCompactExtFrame(
    const __FlashStringHelper *prefix,
    unsigned long id,
    byte dlc,
    const byte *data
)
{
    Serial.print(prefix);
    Serial.print(F(" 0x"));
    Serial.print(id, HEX);
    Serial.print(F("  "));

    for (byte i = 0; i < dlc; i++)
    {
        if (data[i] < 0x10) Serial.print('0');
        Serial.print(data[i], HEX);

        if (i + 1 < dlc)
        {
            Serial.print(' ');
        }
    }

    Serial.println();
}

void armVehicleStatusProbe(const __FlashStringHelper *reason)
{
    if (!vehicleStatusProbeEnabled)
    {
        return;
    }

    vehicleStatusProbeActive = true;
    vehicleStatusProbeUntil = millis() + VEHICLE_STATUS_PROBE_MS;

    Serial.println();
    Serial.print(F("[STATUS PROBE] "));
    Serial.println(reason);
}

void decodeVehicleStatusProbe(unsigned long id, byte dlc, const byte *data)
{
    unsigned long now = millis();

    if ((long)(now - vehicleStatusProbeUntil) >= 0)
    {
        vehicleStatusProbeActive = false;
        Serial.println(F("[STATUS PROBE] done"));
        Serial.println();
        return;
    }

    if (dlc == 0)
    {
        return;
    }

    // Candidate status data is expected on the Infotainment extended family.
    // Skip known audio/media/text channels so the probe stays useful.
    if ((id & 0xFFFF0000UL) != 0x17330000UL)
    {
        return;
    }

    if (id == 0x17333110UL ||
        id == 0x17333111UL ||
        id == 0x17332810UL)
    {
        return;
    }

    byte type = data[0];

    // Favor complete property/application frames and segmented-message starts.
    // Continuation-only traffic is intentionally suppressed.
    if (type == 0x30 ||
        type == 0x38 ||
        type == 0x39 ||
        type == 0x3A ||
        type == 0x3C ||
        type == 0x3D ||
        type == 0x4C ||
        (type & 0xF0) == 0x80 ||
        (type & 0xF0) == 0x90)
    {
        printCompactExtFrame(F("[STATUS]"), id, dlc, data);
    }
}



void sendBcConfig()
{
    // Capture-derived BC_MFA / logical channel 0x27 BAP_Config.
    //
    // MIB request:
    //   0x17332700  19 C2
    //
    // Provider response captured on:
    //   0x17332710  39 C2 03 00 27 00 03 03
    const byte response[8] =
    {
        0x39, 0xC2,
        0x03, 0x00,
        0x27, 0x00,
        0x03, 0x03
    };

    sendRawCan(0x17332710UL, true, 8, response);

    Serial.println(
        F("[BC 0x27 TX] BAP_Config 39 C2 03 00 27 00 03 03")
    );
}


void sendBcFunctionList()
{
    // Capture-derived FunctionList for provider 0x27.
    //
    // Captured response:
    //   80 08 39 C3 38 07 F8 00
    //   C0 00 00 00 00
    const byte startFrame[8] =
    {
        0x80, 0x08,
        0x39, 0xC3,
        0x38, 0x07,
        0xF8, 0x00
    };

    const byte continuation[5] =
    {
        0xC0, 0x00, 0x00, 0x00, 0x00
    };

    sendRawCan(0x17332710UL, true, 8, startFrame);
    delay(10);
    sendRawCan(0x17332710UL, true, 5, continuation);

    Serial.println(F("[BC 0x27 TX] FunctionList"));
}


void sendBcHeartbeatConfig()
{
    // Capture-derived HeartbeatConfig:
    //   0x17332710  39 C4 0A
    const byte response[3] =
    {
        0x39, 0xC4, 0x0A
    };

    sendRawCan(0x17332710UL, true, 3, response);

    Serial.println(F("[BC 0x27 TX] HeartbeatConfig 39 C4 0A"));
}


void sendBcTripDistance()
{
    // Experimental only.
    //
    // The old 0x0F-based 43 F9 reply is NOT considered valid for provider
    // 0x27. Keep this function disabled until a real 0x27 request/response
    // for the desired trip property has been identified in the capture.
    Serial.println(
        F("[BC 0x27] Trip-data reply not sent: function mapping not confirmed")
    );
}


void updateBcTripData()
{
    // Deliberately disabled for BC_MFA 0x27 until the actual trip property
    // request/response mapping has been reconstructed.
    if (!bcProviderEnabled || !bcTripTestEnabled)
    {
        return;
    }

    unsigned long now = millis();

    if ((unsigned long)(now - bcTripLastSend) < BC_TRIP_INTERVAL_MS)
    {
        return;
    }

    bcTripLastSend = now;
    sendBcTripDistance();
}


void processBcProvider(
    unsigned long id,
    bool extended,
    byte dlc,
    const byte *data
)
{
    if (!bcProviderEnabled ||
        !extended ||
        id != 0x17332700UL ||
        dlc < 2)
    {
        return;
    }

    Serial.print(F("[BC 0x27 RX] "));

    for (byte i = 0; i < dlc; i++)
    {
        if (data[i] < 0x10)
        {
            Serial.print('0');
        }

        Serial.print(data[i], HEX);

        if (i + 1 < dlc)
        {
            Serial.print(' ');
        }
    }

    Serial.println();

    // Standard BAP Get requests observed for logical channel 0x27.
    //
    // Request header for this provider is 0x19.
    if (data[0] == 0x19 && data[1] == 0xC2)
    {
        sendBcConfig();
        return;
    }

    if (data[0] == 0x19 && data[1] == 0xC3)
    {
        sendBcFunctionList();
        return;
    }

    if (data[0] == 0x19 && data[1] == 0xC4)
    {
        sendBcHeartbeatConfig();
        return;
    }

    // Do not guess application-function replies yet.
    // Log all other requests so the next MIB action can be mapped directly
    // against the known-good gateway capture.
    Serial.println(F("[BC 0x27] Unhandled request"));
}


bool shouldPrintRxFrame(unsigned long id, bool extended)
{
    switch (monitorMode)
    {
        case MONITOR_OFF:
        case MONITOR_TX:
            return false;

        case MONITOR_RX:
        case MONITOR_ALL:
            return true;

        case MONITOR_DIAG:
            return extended;
    }

    return false;
}

void printCanFrame(
    const char *direction,
    unsigned long id,
    bool extended,
    byte dlc,
    const byte *data
)
{
    Serial.print(millis());
    Serial.print(F(" ms  "));
    Serial.print(direction);
    Serial.print(F("  "));

    if (extended)
    {
        Serial.print(F("EXT  0x"));
    }
    else
    {
        Serial.print(F("STD  0x"));
    }

    if (!extended)
    {
        if (id < 0x100) Serial.print('0');
        if (id < 0x10)  Serial.print('0');
    }

    Serial.print(id, HEX);
    Serial.print(F("  ["));
    Serial.print(dlc);
    Serial.print(F("]  "));

    for (byte i = 0; i < dlc; i++)
    {
        if (data[i] < 0x10)
        {
            Serial.print('0');
        }

        Serial.print(data[i], HEX);

        if (i < (dlc - 1))
        {
            Serial.print(' ');
        }
    }

    Serial.println();
}


// ============================================================
// SERIAL INPUT
// ============================================================

String serialBuffer = "";

void processSerial()
{
    while (Serial.available())
    {
        char c = Serial.read();

        if (c == '\r')
        {
            continue;
        }

        if (c == '\n')
        {
            serialBuffer.trim();

            if (serialBuffer.length() > 0)
            {
                processCommand(serialBuffer);
            }

            serialBuffer = "";
        }
        else
        {
            serialBuffer += c;
        }
    }
}


// ============================================================
// SERIAL COMMAND PROCESSOR
// ============================================================

void processCommand(String command)
{
    command.trim();
    command.toLowerCase();

    if (command == "canstats")
    {
        Serial.print(F("CAN RX/TX/ERR: "));
        Serial.print(canRxCount);
        Serial.print('/');
        Serial.print(canTxCount);
        Serial.print('/');
        Serial.println(canTxErrorCount);
        return;
    }

    if (command == "help")
    {
        printHelp();
        return;
    }

    if (command == "simulategateway on")
    {
        setGatewaySimulation(true);
        return;
    }

    if (command == "simulategateway off")
    {
        setGatewaySimulation(false);
        return;
    }

    if (command == "simulategateway")
    {
        Serial.print(F("[GATEWAY] Simulation = "));
        Serial.println(simulateGatewayEnabled ? F("ON") : F("OFF"));
        return;
    }

    if (command == "diagspoof on")
    {
        diagIdentitySpoofEnabled = true;
        diagF197AwaitingFlowControl = false;

        Serial.println(F("[DIAG] F197 component spoof = ON"));
        Serial.println(F("[DIAG] Component = MQB-PQ-BRIDGE"));
        Serial.println(F("[DIAG] Warning: single-CAN bench mode only."));
        return;
    }

    if (command == "diagspoof off")
    {
        diagIdentitySpoofEnabled = false;
        diagF197AwaitingFlowControl = false;

        Serial.println(F("[DIAG] F197 component spoof = OFF"));
        return;
    }

    if (command == "diagspoof")
    {
        Serial.print(F("[DIAG] F197 component spoof = "));
        Serial.println(diagIdentitySpoofEnabled ? F("ON") : F("OFF"));
        Serial.println(F("[DIAG] Component = MQB-PQ-BRIDGE"));
        return;
    }

    if (command == "bcprov on")
    {
        bcProviderEnabled = true;
        Serial.println(F("[BC 0x27] Provider = ON"));
        return;
    }

    if (command == "bcprov off")
    {
        bcProviderEnabled = false;
        Serial.println(F("[BC 0x27] Provider = OFF"));
        return;
    }

    if (command == "bctrip on")
    {
        bcProviderEnabled = true;
        bcTripTestEnabled = false;
        Serial.println(F("[BC 0x27] Trip TX disabled until function mapping is confirmed"));
        return;
    }

    if (command == "bctrip off")
    {
        bcTripTestEnabled = false;
        Serial.println(F("[BC] Trip test OFF"));
        return;
    }

    if (command.startsWith("bctrip "))
    {
        float km = command.substring(7).toFloat();

        if (km < 0.0f) km = 0.0f;
        if (km > 167772.15f) km = 167772.15f;

        bcTripDistanceKm = km;
        bcProviderEnabled = true;
        bcTripTestEnabled = false;

        Serial.print(F("[BC 0x27] Stored test distance = "));
        Serial.print(bcTripDistanceKm, 2);
        Serial.println(F(" km"));
        Serial.println(F("[BC 0x27] Not transmitted: function mapping not confirmed"));
        return;
    }

    if (command == "state")
    {
        printState();
        return;
    }

    if (command == "messages")
    {
        printMessages();
        return;
    }

    if (command == "clock")
    {
        printClock();
        return;
    }

    if (command == "lighting")
    {
        printLighting();
        return;
    }

    if (command == "mfsw")
    {
        printMfsw();
        return;
    }

    if (command == "on" || command == "ignition on")
    {
        setIgnitionOn();
        return;
    }

    if (command == "off" || command == "ignition off")
    {
        setIgnitionOff();
        return;
    }

    if (command == "engine on")
    {
        vehicle.terminalS = true;
        vehicle.terminal15 = true;
        vehicle.terminalX = false;
        vehicle.terminal50 = false;
        vehicle.terminal75 = true;
        vehicle.engineRunning = true;

        if (vehicle.engineRpm == 0)
        {
            vehicle.engineRpm = 850;
        }

        Serial.println(F("Engine ON"));
        printState();
        return;
    }

    if (command == "engine off")
    {
        vehicle.engineRunning = false;
        vehicle.engineRpm = 0;

        Serial.println(F("Engine OFF"));
        printState();
        return;
    }

    if (command == "starter on")
    {
        vehicle.terminalS = true;
        vehicle.terminal15 = true;
        vehicle.terminalX = false;
        vehicle.terminal50 = true;
        vehicle.terminal75 = false;
        vehicle.engineRunning = false;

        Serial.println(F("Starter ON"));
        printState();
        return;
    }

    if (command == "starter off")
    {
        vehicle.terminal50 = false;

        if (vehicle.terminal15)
        {
            vehicle.terminal75 = true;
        }

        Serial.println(F("Starter OFF"));
        printState();
        return;
    }

    if (command.startsWith("rpm "))
    {
        long rpm = command.substring(4).toInt();

        if (rpm < 0) rpm = 0;
        if (rpm > 16000) rpm = 16000;

        vehicle.engineRpm = (uint16_t)rpm;
        vehicle.engineRunning = rpm > 0;

        Serial.print(F("Engine RPM = "));
        Serial.println(vehicle.engineRpm);
        return;
    }

    if (command.startsWith("startstop "))
    {
        int status = command.substring(10).toInt();

        if (status < 0) status = 0;
        if (status > 3) status = 3;

        vehicle.startStopStatus = (byte)status;

        Serial.print(F("Start/stop status = "));
        Serial.println(vehicle.startStopStatus);
        return;
    }

    if (command.startsWith("terminal "))
    {
        int firstSpace = command.indexOf(' ');
        int secondSpace = command.indexOf(' ', firstSpace + 1);

        if (secondSpace < 0)
        {
            Serial.println(F("Usage: terminal <s|15|x|50|75> <on|off>"));
            return;
        }

        String terminalName = command.substring(firstSpace + 1, secondSpace);
        String value = command.substring(secondSpace + 1);
        bool state;

        if (!parseOnOff(value, state))
        {
            Serial.println(F("Expected ON or OFF."));
            return;
        }

        if      (terminalName == "s")  vehicle.terminalS = state;
        else if (terminalName == "15") vehicle.terminal15 = state;
        else if (terminalName == "x")  vehicle.terminalX = state;
        else if (terminalName == "50") vehicle.terminal50 = state;
        else if (terminalName == "75") vehicle.terminal75 = state;
        else
        {
            Serial.println(F("Unknown terminal."));
            return;
        }

        Serial.print(F("Terminal "));
        Serial.print(terminalName);
        Serial.print(F(" = "));
        Serial.println(state ? F("ON") : F("OFF"));
        return;
    }


    // --------------------------------------------------------
    // LIGHTING / DIMMING
    // --------------------------------------------------------

    if (command == "lights on")
    {
        lighting.lightsEnabled = true;

        if (lighting.dimming58xd == 0 &&
            lighting.dimming58xs == 0 &&
            lighting.dimming58xt == 0)
        {
            lighting.dimming58xd = 0xFD;
            lighting.dimming58xs = 100;
            lighting.dimming58xt = 100;
        }

        Serial.println(F("Interior/display lighting ON"));
        return;
    }

    if (command == "lights off")
    {
        lighting.lightsEnabled = false;
        lighting.nightDesign = false;

        Serial.println(F("Interior/display lighting OFF"));
        Serial.println(F("Dimmung_01 now transmits 00 00 00 00 00 00 00 00"));
        return;
    }

    if (command.startsWith("dimming "))
    {
        int value = command.substring(8).toInt();

        if (value < 0) value = 0;
        if (value > 100) value = 100;

        lighting.lightsEnabled = true;

        // 58xs and 58xt are real percentages.
        lighting.dimming58xs = (byte)value;
        lighting.dimming58xt = (byte)value;

        // 58xd is raw 0..253. Scale user-friendly 0..100 to 0..253.
        lighting.dimming58xd = (byte)((value * 253L) / 100L);

        Serial.print(F("Dimming = "));
        Serial.print(value);
        Serial.print(F("%, 58xd raw = "));
        Serial.println(lighting.dimming58xd);
        return;
    }

    if (command.startsWith("night "))
    {
        bool value;

        if (!parseOnOff(command.substring(6), value))
        {
            Serial.println(F("Usage: night on/off"));
            return;
        }

        lighting.nightDesign = value;

        Serial.print(F("Night design = "));
        Serial.println(value ? F("ON") : F("OFF"));
        return;
    }

    if (command.startsWith("photosensor "))
    {
        long value = command.substring(12).toInt();

        if (value < 0) value = 0;
        if (value > 65535) value = 65535;

        lighting.photoSensor = (uint16_t)value;

        Serial.print(F("Photo sensor = "));
        Serial.println(lighting.photoSensor);
        return;
    }


    // --------------------------------------------------------
    // CLOCK / DATE
    // --------------------------------------------------------

    if (command.startsWith("time "))
    {
        String value = command.substring(5);
        int p1 = value.indexOf(':');
        int p2 = value.indexOf(':', p1 + 1);

        if (p1 < 0)
        {
            Serial.println(F("Usage: time HH:MM[:SS]"));
            return;
        }

        int hour = value.substring(0, p1).toInt();
        int minute;
        int second = 0;

        if (p2 >= 0)
        {
            minute = value.substring(p1 + 1, p2).toInt();
            second = value.substring(p2 + 1).toInt();
        }
        else
        {
            minute = value.substring(p1 + 1).toInt();
        }

        if (hour < 0 || hour > 23 ||
            minute < 0 || minute > 59 ||
            second < 0 || second > 59)
        {
            Serial.println(F("Invalid time."));
            return;
        }

        clockState.hour = hour;
        clockState.minute = minute;
        clockState.second = second;
        clockState.lastTick = millis();

        printClock();
        return;
    }

    if (command.startsWith("date "))
    {
        String value = command.substring(5);
        int p1 = value.indexOf('-');
        int p2 = value.indexOf('-', p1 + 1);

        if (p1 < 0 || p2 < 0)
        {
            Serial.println(F("Usage: date YYYY-MM-DD"));
            return;
        }

        int year = value.substring(0, p1).toInt();
        int month = value.substring(p1 + 1, p2).toInt();
        int day = value.substring(p2 + 1).toInt();

        if (year < 2000 || year > 2127 ||
            month < 1 || month > 12 ||
            day < 1 || day > daysInMonth(year, month))
        {
            Serial.println(F("Invalid date."));
            return;
        }

        clockState.year = year;
        clockState.month = month;
        clockState.day = day;
        clockState.lastTick = millis();

        printClock();
        return;
    }

    if (command == "clock run")
    {
        clockState.running = true;
        clockState.lastTick = millis();
        Serial.println(F("Clock running."));
        return;
    }

    if (command == "clock stop")
    {
        clockState.running = false;
        Serial.println(F("Clock stopped."));
        return;
    }

    if (command == "clock 24h")
    {
        units.clock12h = false;
        Serial.println(F("Clock format test = 24 h"));
        return;
    }

    if (command == "clock 12h")
    {
        units.clock12h = true;
        Serial.println(F("Clock format test = 12 h"));
        return;
    }


    // --------------------------------------------------------
    // DBC-CONFIRMED UNITS / VEHICLE DATA
    // --------------------------------------------------------

    if (command == "units metric")
    {
        units.distanceMiles = false;
        units.mfaSpeedMph = false;
        units.temperatureF = false;
        Serial.println(F("Units = metric bench preset"));
        return;
    }

    if (command == "units imperial")
    {
        units.distanceMiles = true;
        units.mfaSpeedMph = true;
        units.temperatureF = true;
        Serial.println(F("Units = imperial bench preset"));
        return;
    }

    if (command == "temp c")
    {
        units.temperatureF = false;
        Serial.println(F("Temperature unit = Celsius"));
        return;
    }

    if (command == "temp f")
    {
        units.temperatureF = true;
        Serial.println(F("Temperature unit = Fahrenheit"));
        return;
    }

    if (command.startsWith("dateformat "))
    {
        int value = command.substring(11).toInt();

        if (value < 0 || value > 3)
        {
            Serial.println(F("Usage: dateformat <0-3>"));
            return;
        }

        units.dateFormat = (byte)value;
        Serial.print(F("KBI_Einheit_Datum raw = "));
        Serial.println(units.dateFormat);
        return;
    }

    if (command.startsWith("language "))
    {
        int value = command.substring(9).toInt();

        if (value < 0) value = 0;
        if (value > 255) value = 255;

        units.language = (byte)value;
        Serial.print(F("KBI_Einheit_Sprache raw = "));
        Serial.println(units.language);
        return;
    }

    if (command.startsWith("speed "))
    {
        float value = command.substring(6).toFloat();

        if (value < 0.0) value = 0.0;
        if (value > 325.0) value = 325.0;

        benchData.speedKph = value;
        Serial.print(F("Vehicle speed = "));
        Serial.print(benchData.speedKph, 1);
        Serial.println(F(" km/h"));
        return;
    }

    if (command.startsWith("outside "))
    {
        float value = command.substring(8).toFloat();

        if (value < -50.0) value = -50.0;
        if (value > 75.0) value = 75.0;

        benchData.outsideTemperatureC = value;
        Serial.print(F("Outside temperature = "));
        Serial.print(benchData.outsideTemperatureC, 1);
        Serial.println(F(" C"));
        return;
    }

    if (command.startsWith("fuel "))
    {
        int value = command.substring(5).toInt();

        if (value < 0) value = 0;
        if (value > 125) value = 125;

        benchData.fuelLiters = (byte)value;
        Serial.print(F("Fuel content = "));
        Serial.print(benchData.fuelLiters);
        Serial.println(F(" L"));
        return;
    }

    if (command.startsWith("reverse "))
    {
        bool value;

        if (!parseOnOff(command.substring(8), value))
        {
            Serial.println(F("Usage: reverse on/off"));
            return;
        }

        benchData.reverse = value;
        Serial.print(F("Reverse = "));
        Serial.println(value ? F("ON") : F("OFF"));
        return;
    }

    if (command.startsWith("handbrake "))
    {
        bool value;

        if (!parseOnOff(command.substring(10), value))
        {
            Serial.println(F("Usage: handbrake on/off"));
            return;
        }

        benchData.parkingBrake = value;
        Serial.print(F("[VEHICLE] Handbrake = "));
        Serial.println(value ? F("ON") : F("OFF"));
        armVehicleStatusProbe(value ? F("Handbrake ON") : F("Handbrake OFF"));
        return;
    }


    // --------------------------------------------------------
    // MFSW - 0x5BF, GOLF MK7 INFOTAINMENT CAN CAPTURE FORMAT
    // --------------------------------------------------------

    if (command == "mfsw volup")
    {
        startMfswEvent(MFSW_VOLUME_UP);
        return;
    }

    if (command == "mfsw voldown")
    {
        startMfswEvent(MFSW_VOLUME_DOWN);
        return;
    }

    if (command == "mfsw next")
    {
        startMfswEvent(MFSW_NEXT);
        return;
    }

    if (command == "mfsw previous")
    {
        startMfswEvent(MFSW_PREVIOUS);
        return;
    }

    if (command == "mfsw phone")
    {
        startMfswEvent(MFSW_PHONE);
        return;
    }

    if (command == "mfsw voice")
    {
        startMfswEvent(MFSW_VOICE);
        return;
    }

    if (command == "mfsw left")
    {
        startMfswEvent(MFSW_LEFT_MENU);
        return;
    }

    if (command == "mfsw right")
    {
        startMfswEvent(MFSW_RIGHT_MENU);
        return;
    }

    if (command == "mfsw up")
    {
        startMfswEvent(MFSW_UP);
        return;
    }

    if (command == "mfsw down")
    {
        startMfswEvent(MFSW_DOWN);
        return;
    }

    if (command == "mfsw ok")
    {
        startMfswEvent(MFSW_OK);
        return;
    }

    if (command == "mfsw idle")
    {
        mfsw.active = false;
        mfsw.releasePending = false;
        mfsw.buttonCode = 0x00;
        sendMfswIdle();
        Serial.println(F("MFSW idle frame sent: 00 00 00 40"));
        return;
    }

    if (command == "mfsw release")
    {
        mfsw.active = false;
        mfsw.releasePending = false;
        mfsw.buttonCode = 0x00;
        sendMfswRelease();
        Serial.println(F("MFSW release frame sent: 00 00 01 40"));
        return;
    }


    // --------------------------------------------------------
    // INFOTAINMENT FEEDBACK / ASCII
    // --------------------------------------------------------

    if (command == "feedback on")
    {
        volumeFeedbackEnabled = true;
        Serial.println(F("Volume feedback = ON"));
        return;
    }

    if (command == "feedback off")
    {
        volumeFeedbackEnabled = false;
        Serial.println(F("Volume feedback = OFF"));
        return;
    }

    if (command == "ascii on")
    {
        asciiDecoderEnabled = true;
        Serial.println(F("Infotainment ASCII decoder = ON"));
        return;
    }

    if (command == "ascii off")
    {
        asciiDecoderEnabled = false;
        asciiMessageActive = false;
        Serial.println(F("Infotainment ASCII decoder = OFF"));
        return;
    }

    if (command == "feedback")
    {
        Serial.print(F("Volume feedback: "));
        Serial.println(volumeFeedbackEnabled ? F("ON") : F("OFF"));

        if (volumeKnown)
        {
            Serial.print(F("Last volume    : "));
            Serial.println(lastVolume);
        }
        else
        {
            Serial.println(F("Last volume    : unknown"));
        }

        Serial.print(F("ASCII decoder  : "));
        Serial.println(asciiDecoderEnabled ? F("ON") : F("OFF"));

        Serial.print(F("Media metadata : "));
        Serial.println(mediaMetadataEnabled ? F("ON") : F("OFF"));
        return;
    }

    if (command == "media on")
    {
        mediaMetadataEnabled = true;
        resetMediaMetadata();
        Serial.println(F("Media metadata decoder = ON"));
        return;
    }

    if (command == "media off")
    {
        mediaMetadataEnabled = false;
        resetMediaMetadata();
        Serial.println(F("Media metadata decoder = OFF"));
        return;
    }

    if (command == "media")
    {
        Serial.print(F("Media metadata decoder = "));
        Serial.println(mediaMetadataEnabled ? F("ON") : F("OFF"));
        return;
    }


    // --------------------------------------------------------
    // CLEAN SERIAL / DISCOVERY / VEHICLE STATUS HUNTER
    // --------------------------------------------------------

    if (command == "statuswatch on")
    {
        vehicleStatusProbeEnabled = true;
        Serial.println(F("[SYSTEM] Vehicle-status probe = ON"));
        return;
    }

    if (command == "statuswatch off")
    {
        vehicleStatusProbeEnabled = false;
        vehicleStatusProbeActive = false;
        Serial.println(F("[SYSTEM] Vehicle-status probe = OFF"));
        return;
    }

    if (command == "statuswatch")
    {
        Serial.print(F("[SYSTEM] Vehicle-status probe = "));
        Serial.println(vehicleStatusProbeEnabled ? F("ON") : F("OFF"));
        return;
    }

    // --------------------------------------------------------
    // MARK / MONITOR
    // --------------------------------------------------------

    if (command == "mark")
    {
        markerCounter++;

        Serial.println();
        Serial.println(F("======================================"));
        Serial.print(F("USER MARK "));
        Serial.print(markerCounter);
        Serial.print(F(" @ "));
        Serial.print(millis());
        Serial.println(F(" ms"));
        Serial.println(F("======================================"));
        Serial.println();
        return;
    }

    if (command == "monitor all" || command == "monitor on")
    {
        monitorMode = MONITOR_ALL;
        Serial.println(F("CAN monitor: RX + TX"));
        return;
    }

    if (command == "monitor rx")
    {
        monitorMode = MONITOR_RX;
        Serial.println(F("CAN monitor: RX only"));
        return;
    }

    if (command == "monitor tx")
    {
        monitorMode = MONITOR_TX;
        Serial.println(F("CAN monitor: TX only"));
        return;
    }

    if (command == "monitor diag")
    {
        monitorMode = MONITOR_DIAG;
        Serial.println(F("Diagnostic monitor: RX extended frames only."));
        return;
    }

    if (command == "monitor off")
    {
        monitorMode = MONITOR_OFF;
        Serial.println(F("CAN monitor OFF"));
        return;
    }

    // Extended DBC vehicle simulation commands.
    if (command.startsWith("oiltemp ")) { float v=command.substring(8).toFloat(); if(v<-60)v=-60;if(v>192)v=192;extData.oilTempC10=(int16_t)(v*10);Serial.println(F("Oil temperature updated"));return; }
    if (command.startsWith("coolant ")) { float v=command.substring(8).toFloat(); if(v<-48)v=-48;if(v>141.7)v=141.7;extData.coolantTempC10=(int16_t)(v*10);Serial.println(F("Coolant temperature updated"));return; }
    if (command.startsWith("iat ")) { float v=command.substring(4).toFloat(); if(v<-48)v=-48;if(v>141.7)v=141.7;extData.intakeTempC10=(int16_t)(v*10);Serial.println(F("Intake temperature updated"));return; }
    if (command.startsWith("oilpressure ")) { float v=command.substring(12).toFloat(); if(v<0)v=0;if(v>10)v=10;extData.oilPressureCentiBar=(uint16_t)(v*100);Serial.println(F("Oil pressure updated"));return; }
    if (command.startsWith("boost ")) { float v=command.substring(6).toFloat(); if(v<0)v=0;if(v>5.1)v=5.1;extData.boostCentiBar=(uint16_t)(v*100);Serial.println(F("Boost pressure updated"));return; }
    if (command.startsWith("throttle ")) { int v=command.substring(9).toInt();if(v<0)v=0;if(v>100)v=100;extData.throttlePercent=(byte)v;Serial.println(F("Throttle updated"));return; }
    if (command.startsWith("oillevel ")) { int v=command.substring(9).toInt();if(v<0)v=0;if(v>100)v=100;extData.oilLevelPercent=(byte)v;Serial.println(F("Oil level updated"));return; }

    if (command.startsWith("clutch ")) { bool v;if(!parseOnOff(command.substring(7),v)){Serial.println(F("Usage: clutch on/off"));return;}extData.clutch=v;Serial.println(v?F("Clutch ON"):F("Clutch OFF"));return; }
    if (command.startsWith("kickdown ")) { bool v;if(!parseOnOff(command.substring(9),v)){Serial.println(F("Usage: kickdown on/off"));return;}extData.kickdown=v;Serial.println(v?F("Kickdown ON"):F("Kickdown OFF"));return; }
    if (command.startsWith("mil ")) { bool v;if(!parseOnOff(command.substring(4),v)){Serial.println(F("Usage: mil on/off"));return;}extData.mil=v;Serial.println(v?F("[VEHICLE] MIL = ON"):F("[VEHICLE] MIL = OFF"));armVehicleStatusProbe(v?F("MIL ON"):F("MIL OFF"));return; }
    if (command.startsWith("enginewarn ")) { bool v;if(!parseOnOff(command.substring(11),v)){Serial.println(F("Usage: enginewarn on/off"));return;}extData.engineWarning=v;Serial.println(v?F("[VEHICLE] Engine warning = ON"):F("[VEHICLE] Engine warning = OFF"));armVehicleStatusProbe(v?F("Engine warning ON"):F("Engine warning OFF"));return; }
    if (command.startsWith("oilwarn ")) { bool v;if(!parseOnOff(command.substring(8),v)){Serial.println(F("Usage: oilwarn on/off"));return;}extData.oilWarning=v;Serial.println(v?F("[VEHICLE] Oil warning = ON"):F("[VEHICLE] Oil warning = OFF"));armVehicleStatusProbe(v?F("Oil warning ON"):F("Oil warning OFF"));return; }
    if (command.startsWith("abslamp ")) { bool v;if(!parseOnOff(command.substring(8),v)){Serial.println(F("Usage: abslamp on/off"));return;}extData.absLamp=v;Serial.println(v?F("[VEHICLE] ABS warning = ON"):F("[VEHICLE] ABS warning = OFF"));armVehicleStatusProbe(v?F("ABS warning ON"):F("ABS warning OFF"));return; }
    if (command.startsWith("esplamp ")) { bool v;if(!parseOnOff(command.substring(8),v)){Serial.println(F("Usage: esplamp on/off"));return;}extData.espLamp=v;Serial.println(v?F("[VEHICLE] ESP warning = ON"):F("[VEHICLE] ESP warning = OFF"));armVehicleStatusProbe(v?F("ESP warning ON"):F("ESP warning OFF"));return; }
    if (command.startsWith("airbaglamp ")) { bool v;if(!parseOnOff(command.substring(11),v)){Serial.println(F("Usage: airbaglamp on/off"));return;}extData.airbagLamp=v;Serial.println(v?F("[VEHICLE] Airbag warning = ON"):F("[VEHICLE] Airbag warning = OFF"));armVehicleStatusProbe(v?F("Airbag warning ON"):F("Airbag warning OFF"));return; }
    if (command.startsWith("steeringlamp ")) { bool v;if(!parseOnOff(command.substring(13),v)){Serial.println(F("Usage: steeringlamp on/off"));return;}extData.steeringLamp=v;Serial.println(v?F("[VEHICLE] Steering warning = ON"):F("[VEHICLE] Steering warning = OFF"));armVehicleStatusProbe(v?F("Steering warning ON"):F("Steering warning OFF"));return; }

    Serial.print(F("Unknown command: "));
    Serial.println(command);
    Serial.println(F("Type 'help' for available commands."));
}


// ============================================================
// HELPERS / STATUS
// ============================================================

bool parseOnOff(String value, bool &result)
{
    value.trim();
    value.toLowerCase();

    if (value == "on")
    {
        result = true;
        return true;
    }

    if (value == "off")
    {
        result = false;
        return true;
    }

    return false;
}

void printState()
{
    Serial.println();
    Serial.println(F("Vehicle state"));
    Serial.println(F("-------------"));

    Serial.print(F("Terminal S  : "));
    Serial.println(vehicle.terminalS ? F("ON") : F("OFF"));

    Serial.print(F("Terminal 15 : "));
    Serial.println(vehicle.terminal15 ? F("ON") : F("OFF"));

    Serial.print(F("Terminal X  : "));
    Serial.println(vehicle.terminalX ? F("ON") : F("OFF"));

    Serial.print(F("Terminal 50 : "));
    Serial.println(vehicle.terminal50 ? F("ON") : F("OFF"));

    Serial.print(F("Terminal 75 : "));
    Serial.println(vehicle.terminal75 ? F("ON") : F("OFF"));

    Serial.print(F("Engine      : "));
    Serial.println(vehicle.engineRunning ? F("RUNNING") : F("STOPPED"));

    Serial.print(F("RPM         : "));
    Serial.println(vehicle.engineRpm);

    byte flags = 0;

    if (vehicle.terminalS)  flags |= 0x01;
    if (vehicle.terminal15) flags |= 0x02;
    if (vehicle.terminalX)  flags |= 0x04;
    if (vehicle.terminal50) flags |= 0x08;

    Serial.print(F("3C0 flags   : 0x"));

    if (flags < 0x10)
    {
        Serial.print('0');
    }

    Serial.println(flags, HEX);

    printLighting();
    printClock();
    printMfsw();
}

void printLighting()
{
    Serial.println();
    Serial.println(F("Lighting"));
    Serial.println(F("--------"));

    Serial.print(F("Lights enabled: "));
    Serial.println(lighting.lightsEnabled ? F("YES") : F("NO"));

    Serial.print(F("Dimming 58xd : "));
    Serial.println(lighting.dimming58xd);

    Serial.print(F("Dimming 58xs : "));
    Serial.println(lighting.dimming58xs);

    Serial.print(F("Dimming 58xt : "));
    Serial.println(lighting.dimming58xt);

    Serial.print(F("Night design : "));
    Serial.println(lighting.nightDesign ? F("ON") : F("OFF"));

    Serial.print(F("Photo sensor : "));
    Serial.println(lighting.photoSensor);
}

void printClock()
{
    Serial.println();
    Serial.println(F("Clock"));
    Serial.println(F("-----"));

    if (clockState.day < 10) Serial.print('0');
    Serial.print(clockState.day);
    Serial.print('-');

    if (clockState.month < 10) Serial.print('0');
    Serial.print(clockState.month);
    Serial.print('-');

    Serial.println(clockState.year);

    if (clockState.hour < 10) Serial.print('0');
    Serial.print(clockState.hour);
    Serial.print(':');

    if (clockState.minute < 10) Serial.print('0');
    Serial.print(clockState.minute);
    Serial.print(':');

    if (clockState.second < 10) Serial.print('0');
    Serial.println(clockState.second);

    Serial.print(F("Running      : "));
    Serial.println(clockState.running ? F("YES") : F("NO"));
}

void printMfsw()
{
    Serial.println();
    Serial.println(F("MFSW 0x5BF - Golf Mk7 capture format"));
    Serial.println(F("------------------------------------"));
    Serial.println(F("DLC          : 4"));
    Serial.println(F("Idle         : 00 00 00 40"));
    Serial.println(F("Release      : 00 00 01 40"));

    Serial.print(F("Active       : "));
    Serial.println(mfsw.active ? F("YES") : F("NO"));

    Serial.print(F("Button code  : 0x"));
    if (mfsw.buttonCode < 0x10) Serial.print('0');
    Serial.println(mfsw.buttonCode, HEX);
}

void printMessages()
{
    Serial.println();
    Serial.println(F("CAN messages"));
    Serial.println(F("------------"));

    for (byte i = 0; i < MESSAGE_COUNT; i++)
    {
        CanMessage &message = messages[i];

        Serial.print(F("["));
        Serial.print(i);
        Serial.print(F("] "));
        Serial.print(message.name);
        Serial.print(F("  0x"));
        Serial.print(message.id, HEX);
        Serial.print(F("  "));
        Serial.print(message.interval);
        Serial.print(F(" ms  "));
        Serial.println(message.enabled ? F("ENABLED") : F("DISABLED"));
    }

    Serial.println(F("MFSW generator: 0x5BF, DLC 4, Golf Mk7 capture format"));
    Serial.println();
}

void printHelp()
{
    Serial.println();
    Serial.println(F("MQB Emulator ESP32-S3 v1.1.6 Simulate Gateway + BC_MFA 0x27"));
    Serial.println(F("simulategateway on/off | simulategateway"));
    Serial.println(F("on/off | engine on/off | rpm <n>"));
    Serial.println(F("lights on/off | dimming <0-100>"));
    Serial.println(F("speed/outside/fuel | handbrake/reverse on/off"));
    Serial.println(F("mfsw volup/voldown/next/previous"));
    Serial.println(F("mfsw phone/voice/left/right/up/down/ok"));
    Serial.println(F("feedback | ascii on/off | media on/off"));
    Serial.println(F("abslamp/esplamp/mil/oilwarn on/off"));
    Serial.println(F("enginewarn/airbaglamp/steeringlamp on/off"));
    Serial.println(F("statuswatch on/off | bcprov on/off  (BC_MFA 0x27)"));
    Serial.println(F("diagspoof on/off | diagspoof  (VCDS F197 bench test)"));
    Serial.println(F("bctrip <km>  (store only; TX disabled until 0x27 mapping is confirmed)"));
    Serial.println(F("monitor all/rx/tx/diag/off"));
    Serial.println(F("canstats"));
    Serial.println(F("state | messages | mark | help"));
    Serial.println();
}
