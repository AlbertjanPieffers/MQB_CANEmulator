#include <SPI.h>
#include <mcp_can.h>

/*
 * MQB CAN Bench Simulator v5
 *
 * Hardware:
 *   Arduino + MCP2515
 *
 * CAN:
 *   500 kbit/s
 *   MCP2515 oscillator: 16 MHz
 *   CS pin: 10
 *
 * Periodic messages:
 *   0x3C0 Klemmen_Status_01
 *   0x3BE Motor_14
 *   0x5F0 Dimmung_01
 *   0x6B2 Diagnose_01
 *   0x643 Einheiten_01
 *
 * MFSW:
 *   0x5BF steering-wheel command generator
 *   based on documented MQB/VAG captures
 *
 * Serial:
 *   115200 baud
 */

#define CAN_CS_PIN 10

MCP_CAN CAN(CAN_CS_PIN);

const byte CAN_SPEED = CAN_500KBPS;
const byte CAN_CLOCK = MCP_16MHZ;


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
byte rearLightCounter = 0;


// ============================================================
// MFSW TEST STATE - 0x5BF
// ============================================================

/*
 * Verified against the Wirer / Gabriel Mastny steering-wheel
 * converter documentation for MQB-era VAG vehicles.
 *
 * CAN ID 0x5BF, DLC 8:
 *   data[0] = function/button
 *   data[1] = 0x00
 *   data[2] = wheel direction or press-duration state
 *   data[3] = 0x00
 *   data[4] = steering-wheel generation marker
 *   data[5] = 0x00
 *   data[6] = 0x00
 *   data[7] = 0x00
 *
 * Old steering wheel marker: 0x13
 * New steering wheel documentation uses 0xA3.
 *
 * Wheel direction:
 *   0x01 / 0x0F
 *
 * Normal button press state:
 *   0x01 .. 0x06
 */

struct MfswState
{
    byte functionCode;
    byte stateCode;
    byte generationMarker;

    unsigned long releaseAt;
    unsigned long stopAt;

    bool active;
    bool releasePhase;
};

MfswState mfsw =
{
    0x00,
    0x00,
    0x13,
    0,
    0,
    false,
    false
};

const unsigned long MFSW_PRESS_TIME_MS = 250;
const unsigned long MFSW_RELEASE_TIME_MS = 250;
const unsigned long MFSW_PERIOD_MS = 50;

unsigned long mfswLastSend = 0;

// 0x5BF function codes for the old/MQB-compatible steering-wheel message.
const byte MFSW_MENU_WHEEL    = 0x06;
const byte MFSW_MENU_PRESS    = 0x07;
const byte MFSW_ENTER_MENU    = 0x08;
const byte MFSW_ASSIST        = 0x0C;
const byte MFSW_VOLUME_WHEEL  = 0x12;
const byte MFSW_VOLUME_PRESS  = 0x13;
const byte MFSW_SOURCE        = 0x14;
const byte MFSW_NEXT          = 0x15;
const byte MFSW_PREVIOUS      = 0x16;
const byte MFSW_VOICE         = 0x19;
const byte MFSW_PHONE         = 0x1C;
const byte MFSW_VIEW          = 0x23;

// Direction/state values documented for 0x5BF.
const byte MFSW_DIR_POSITIVE  = 0x01;
const byte MFSW_DIR_NEGATIVE  = 0x0F;
const byte MFSW_SHORT_PRESS   = 0x01;


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

MonitorMode monitorMode = MONITOR_ALL;
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
void updateRearLight01(CanMessage &message);

void sendMessages();
bool sendCanMessage(CanMessage &message);
bool sendRawCan(unsigned long id, bool extended, byte dlc, byte *data);

void updateClock();
void incrementClockOneSecond();
bool isLeapYear(uint16_t year);
byte daysInMonth(uint16_t year, byte month);

void updateMfsw();
void pressMfswButton(byte functionCode, byte pressState = MFSW_SHORT_PRESS);
void turnMfswWheel(byte functionCode, byte direction);
void startMfswEvent(byte functionCode, byte stateCode);
void sendMfswFrame(byte functionCode, byte stateCode);
void sendMfswRelease();

void setBitsIntel(byte *data, byte startBit, byte length, uint32_t value);

void monitorCan();
bool shouldPrintRxFrame(unsigned long id, bool extended);
void printCanFrame(const char *direction, unsigned long id, bool extended, byte dlc, const byte *data);

void processSerial();
void processCommand(String command);
bool parseOnOff(String value, bool &result);

void setIgnitionOn();
void setIgnitionOff();

void printHelp();
void printState();
void printMessages();
void printClock();
void printLighting();
void printMfsw()
{
    Serial.println();
    Serial.println(F("MFSW 0x5BF"));
    Serial.println(F("----------"));

    Serial.print(F("Generation   : 0x"));
    if (mfsw.generationMarker < 0x10) Serial.print('0');
    Serial.println(mfsw.generationMarker, HEX);

    Serial.print(F("Active       : "));
    Serial.println(mfsw.active ? F("YES") : F("NO"));

    Serial.print(F("Function     : 0x"));
    if (mfsw.functionCode < 0x10) Serial.print('0');
    Serial.println(mfsw.functionCode, HEX);

    Serial.print(F("State        : 0x"));
    if (mfsw.stateCode < 0x10) Serial.print('0');
    Serial.println(mfsw.stateCode, HEX);
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

    Serial.println(F("MFSW generator: 0x5BF, verified message layout"));
    Serial.println();
}

void printHelp()
{
    Serial.println();
    Serial.println(F("Commands"));
    Serial.println(F("--------"));
    Serial.println(F("on"));
    Serial.println(F("off"));
    Serial.println(F("ignition on"));
    Serial.println(F("ignition off"));
    Serial.println(F("engine on"));
    Serial.println(F("engine off"));
    Serial.println(F("starter on"));
    Serial.println(F("starter off"));
    Serial.println(F("rpm <value>"));
    Serial.println(F("startstop <0-3>"));
    Serial.println();

    Serial.println(F("terminal s on/off"));
    Serial.println(F("terminal 15 on/off"));
    Serial.println(F("terminal x on/off"));
    Serial.println(F("terminal 50 on/off"));
    Serial.println(F("terminal 75 on/off"));
    Serial.println();

    Serial.println(F("lights on"));
    Serial.println(F("lights off"));
    Serial.println(F("dimming <0-100>"));
    Serial.println(F("night on/off"));
    Serial.println(F("photosensor <0-65535>"));
    Serial.println(F("lighting"));
    Serial.println();

    Serial.println(F("time HH:MM[:SS]"));
    Serial.println(F("date YYYY-MM-DD"));
    Serial.println(F("clock"));
    Serial.println(F("clock run"));
    Serial.println(F("clock stop"));
    Serial.println(F("clock 24h"));
    Serial.println(F("clock 12h"));
    Serial.println();
    Serial.println(F("units metric"));
    Serial.println(F("units imperial"));
    Serial.println(F("temp c"));
    Serial.println(F("temp f"));
    Serial.println(F("dateformat <0-3>"));
    Serial.println(F("language <0-255>"));
    Serial.println(F("speed <0-325>"));
    Serial.println(F("outside <-50..75>"));
    Serial.println(F("fuel <0-125>"));
    Serial.println(F("reverse on/off"));
    Serial.println(F("handbrake on/off"));
    Serial.println();

    Serial.println(F("mfsw volup"));
    Serial.println(F("mfsw voldown"));
    Serial.println(F("mfsw mute"));
    Serial.println(F("mfsw next"));
    Serial.println(F("mfsw previous"));
    Serial.println(F("mfsw source"));
    Serial.println(F("mfsw phone"));
    Serial.println(F("mfsw voice"));
    Serial.println(F("mfsw view"));
    Serial.println(F("mfsw menu"));
    Serial.println(F("mfsw assist"));
    Serial.println(F("mfsw wheel 01"));
    Serial.println(F("mfsw wheel 0f"));
    Serial.println(F("mfsw pressstate <1-6>"));
    Serial.println(F("mfsw marker old"));
    Serial.println(F("mfsw marker new"));
    Serial.println(F("mfsw marker <00-FF>"));
    Serial.println(F("mfsw"));
    Serial.println();

    Serial.println(F("state"));
    Serial.println(F("messages"));
    Serial.println(F("mark"));
    Serial.println();

    Serial.println(F("monitor all"));
    Serial.println(F("monitor rx"));
    Serial.println(F("monitor tx"));
    Serial.println(F("monitor diag"));
    Serial.println(F("monitor off"));
    Serial.println();

    Serial.println(F("help"));
    Serial.println();
}
