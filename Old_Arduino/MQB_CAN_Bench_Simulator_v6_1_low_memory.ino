#include <SPI.h>
#include <mcp_can.h>

/*
 * MQB CAN Bench Simulator v6.1 Low-Memory
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
 * Experimental MFSW:
 *   PQ-style 0x5C1 / 0x5BF test generator
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
// MFSW STATE - 0x5BF
// ============================================================

enum MfswMode
{
    MFSW_MODE_PULSE,
    MFSW_MODE_CYCLIC
};

struct MfswState
{
    byte functionCode;
    byte stateCode;
    byte generationMarker;
    unsigned long releaseAt;
    unsigned long stopAt;
    bool active;
    bool releasePhase;
    MfswMode mode;
};

MfswState mfsw =
{
    0x00,
    0x00,
    0x13,
    0,
    0,
    false,
    false,
    MFSW_MODE_PULSE
};

const byte MFSW_MENU_WHEEL   = 0x06;
const byte MFSW_MENU_PRESS   = 0x07;
const byte MFSW_ENTER_MENU   = 0x08;
const byte MFSW_ASSIST       = 0x0C;
const byte MFSW_VOLUME_WHEEL = 0x12;
const byte MFSW_VOLUME_PRESS = 0x13;
const byte MFSW_SOURCE       = 0x14;
const byte MFSW_NEXT         = 0x15;
const byte MFSW_PREVIOUS     = 0x16;
const byte MFSW_VOICE        = 0x19;
const byte MFSW_PHONE        = 0x1C;
const byte MFSW_VIEW         = 0x23;

const byte MFSW_DIR_POSITIVE = 0x01;
const byte MFSW_DIR_NEGATIVE = 0x0F;
const byte MFSW_SHORT_PRESS  = 0x01;

const unsigned long MFSW_PRESS_TIME_MS = 250;
const unsigned long MFSW_RELEASE_TIME_MS = 250;
const unsigned long MFSW_PERIOD_MS = 50;

unsigned long mfswLastSend = 0;


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
// MQB INFOTAINMENT REST-BUS TEST
// ============================================================

/*
 * The message IDs and cycle times below follow the MQB Infotainment
 * Gateway message list published for CANSim4.
 *
 * Important:
 * CANSim4 documents IDs/DLC/cycle times, but not the payload bytes.
 * Therefore the new gateway/rest-bus frames in this experimental mode
 * start with neutral zero payloads unless this sketch already has a
 * DBC-backed implementation for that message.
 *
 * This mode is intended to determine whether the MIB sleep behaviour
 * changes when the expected MQB traffic is present.
 */

bool restBusEnabled = false;

struct RestBusMessage
{
    uint16_t id;
    uint16_t interval;
    byte dlc;
};

// Static rest-bus definitions live in flash instead of SRAM.
// All intervals are multiples of 10 ms.
const RestBusMessage restBusMessages[] PROGMEM =
{
    { 0x040,   50, 8 }, // Airbag_01
    { 0x663,  100, 8 }, // BEM_02
    { 0x101,   20, 8 }, // ESP_02
    { 0x116,  200, 8 }, // ESP_10
    { 0x0FD,   20, 8 }, // ESP_21
    { 0x3DA,  100, 8 }, // Gateway_71
    { 0x3DB,  100, 8 }, // Gateway_72
    { 0x3DC,   50, 8 }, // Gateway_73
    { 0x3EA,  200, 8 }, // Gateway_77
    { 0x6B8, 1000, 8 }, // Kombi_03
    { 0x3D5,  100, 8 }, // Licht_Anf_01
    { 0x585, 1000, 8 }, // Systeminfo_01
    { 0x5B0, 1000, 8 }, // TimeDate
    { 0x6B6,  200, 6 }, // Uhrzeit_01
    { 0x6B4,  200, 8 }  // VIN_01
};

const byte RESTBUS_MESSAGE_COUNT =
    sizeof(restBusMessages) / sizeof(restBusMessages[0]);

unsigned long restBusLastTick = 0;
uint16_t restBusTick = 0;

// One shared zero payload is used for every experimental rest-bus frame.
byte restBusZeroPayload[8] = {0,0,0,0,0,0,0,0};


// ============================================================
// KLEMMEN_STATUS_01
// ============================================================

const byte klemmenChecksum[16] PROGMEM =
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
void pressMfswButton(byte functionCode, byte pressState);
void turnMfswWheel(byte functionCode, byte direction);
void startMfswEvent(byte functionCode, byte stateCode);
void sendMfswFrame(byte functionCode, byte stateCode);
void sendMfswRelease();

void sendRestBusMessages();
void setRestBusEnabled(bool enabled)
{
    restBusEnabled = enabled;
    restBusLastTick = millis();
    restBusTick = 0;

    Serial.print(F("MQB infotainment rest bus: "));
    Serial.println(restBusEnabled ? F("ON") : F("OFF"));
}

void sendRestBusMessages()
{
    if (!restBusEnabled)
    {
        return;
    }

    unsigned long now = millis();

    // A single 10 ms scheduler replaces one 32-bit timer per message.
    while ((now - restBusLastTick) >= 10)
    {
        restBusLastTick += 10;
        restBusTick++;

        for (byte i = 0; i < RESTBUS_MESSAGE_COUNT; i++)
        {
            uint16_t interval = pgm_read_word(&restBusMessages[i].interval);

            if ((restBusTick % (interval / 10)) != 0)
            {
                continue;
            }

            uint16_t id = pgm_read_word(&restBusMessages[i].id);
            byte dlc = pgm_read_byte(&restBusMessages[i].dlc);

            sendRawCan(id, false, dlc, restBusZeroPayload);
        }
    }

    // Prevent the 16-bit scheduler counter from growing indefinitely.
    // 30000 ticks = 300 seconds and is divisible by all configured periods.
    if (restBusTick >= 30000)
    {
        restBusTick = 0;
    }
}

void printRestBus()
{
    Serial.println();
    Serial.println(F("MQB INFOTAINMENT REST BUS"));
    Serial.println(F("-------------------------"));
    Serial.print(F("State: "));
    Serial.println(restBusEnabled ? F("ON") : F("OFF"));
    Serial.println(F("Definitions stored in PROGMEM; shared zero TX payload."));
    Serial.println(F("0x040 Airbag_01       50 ms"));
    Serial.println(F("0x663 BEM_02         100 ms"));
    Serial.println(F("0x101 ESP_02          20 ms"));
    Serial.println(F("0x116 ESP_10         200 ms"));
    Serial.println(F("0x0FD ESP_21          20 ms"));
    Serial.println(F("0x3DA Gateway_71     100 ms"));
    Serial.println(F("0x3DB Gateway_72     100 ms"));
    Serial.println(F("0x3DC Gateway_73      50 ms"));
    Serial.println(F("0x3EA Gateway_77     200 ms"));
    Serial.println(F("0x6B8 Kombi_03      1000 ms"));
    Serial.println(F("0x3D5 Licht_Anf_01   100 ms"));
    Serial.println(F("0x585 Systeminfo_01 1000 ms"));
    Serial.println(F("0x5B0 TimeDate      1000 ms"));
    Serial.println(F("0x6B6 Uhrzeit_01     200 ms"));
    Serial.println(F("0x6B4 VIN_01         200 ms"));
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

bool sendRawCan(unsigned long id, bool extended, byte dlc, byte *data)
{
    byte status = CAN.sendMsgBuf(id, extended ? 1 : 0, dlc, data);

    if (status != CAN_OK)
    {
        Serial.print(F("CAN TX ERROR: 0x"));
        Serial.println(id, HEX);
        return false;
    }

    if (monitorMode == MONITOR_ALL || monitorMode == MONITOR_TX)
    {
        printCanFrame("TX", id, extended, dlc, data);
    }

    return true;
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

    message.data[0] = pgm_read_byte(&klemmenChecksum[klemmenCounter]);
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
// LICHT_HINTEN_01 - 0x3D6
// ============================================================

void updateRearLight01(CanMessage &message)
{
    for (byte i = 0; i < 8; i++)
    {
        message.data[i] = 0x00;
    }

    // Licht_hinten_01_BZ, bits 0..3.
    setBitsIntel(message.data, 0, 4, rearLightCounter & 0x0F);

    // LH_Rueckfahrlicht_aktiv, bit 13.
    setBitsIntel(message.data, 13, 1, benchData.reverse ? 1 : 0);

    rearLightCounter = (rearLightCounter + 1) & 0x0F;
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
// MFSW - 0x5BF
// ============================================================

void startMfswEvent(byte functionCode, byte stateCode)
{
    unsigned long now = millis();

    mfsw.functionCode = functionCode;
    mfsw.stateCode = stateCode;
    mfsw.releaseAt = now + MFSW_PRESS_TIME_MS;
    mfsw.stopAt = mfsw.releaseAt;
    mfsw.active = true;
    mfsw.releasePhase = false;

    if (mfsw.mode == MFSW_MODE_PULSE)
    {
        // Pulse mode: exactly one frame and no guessed release frame.
        sendMfswFrame(functionCode, stateCode);
        mfsw.active = false;
    }

    Serial.print(F("MFSW 0x5BF: mode="));
    Serial.print(mfsw.mode == MFSW_MODE_PULSE ? F("pulse") : F("cyclic"));
    Serial.print(F(" function=0x"));
    if (functionCode < 0x10) Serial.print('0');
    Serial.print(functionCode, HEX);
    Serial.print(F(" state=0x"));
    if (stateCode < 0x10) Serial.print('0');
    Serial.print(stateCode, HEX);
    Serial.print(F(" marker=0x"));
    if (mfsw.generationMarker < 0x10) Serial.print('0');
    Serial.println(mfsw.generationMarker, HEX);
}

void pressMfswButton(byte functionCode, byte pressState)
{
    if (pressState < 0x01) pressState = 0x01;
    if (pressState > 0x06) pressState = 0x06;

    startMfswEvent(functionCode, pressState);
}

void turnMfswWheel(byte functionCode, byte direction)
{
    if (direction != MFSW_DIR_POSITIVE &&
        direction != MFSW_DIR_NEGATIVE)
    {
        Serial.println(F("Invalid MFSW wheel direction"));
        return;
    }

    startMfswEvent(functionCode, direction);
}

void updateMfsw()
{
    if (mfsw.mode != MFSW_MODE_CYCLIC)
    {
        return;
    }

    unsigned long now = millis();

    if ((now - mfswLastSend) < MFSW_PERIOD_MS)
    {
        return;
    }

    mfswLastSend = now;

    if (mfsw.active && (long)(now - mfsw.releaseAt) < 0)
    {
        sendMfswFrame(mfsw.functionCode, mfsw.stateCode);
    }
    else
    {
        mfsw.active = false;

        // Experimental idle frame, used only in cyclic test mode.
        sendMfswFrame(0x00, 0x00);
    }
}

void sendMfswFrame(byte functionCode, byte stateCode)
{
    byte data[8] =
    {
        functionCode,
        0x00,
        0x00,
        0x00,
        stateCode,
        0x00,
        mfsw.generationMarker,
        0x00
    };

    sendRawCan(0x5BF, false, 8, data);
}

void sendMfswRelease()
{
    // Retained for compatibility. Pulse mode deliberately does not use it.
    sendMfswFrame(0x00, 0x00);
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
    while (CAN.checkReceive() == CAN_MSGAVAIL)
    {
        unsigned long rawId = 0;
        byte len = 0;
        byte buffer[8] = {0};

        byte result = CAN.readMsgBuf(&rawId, &len, buffer);

        if (result != CAN_OK)
        {
            return;
        }

        bool extended = (rawId & 0x80000000UL) != 0;
        unsigned long id = rawId & 0x1FFFFFFFUL;

        if (!shouldPrintRxFrame(id, extended))
        {
            continue;
        }

        printCanFrame("RX", id, extended, len, buffer);
    }
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

    if (command == "help")
    {
        printHelp();
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
        Serial.print(F("Handbrake = "));
        Serial.println(value ? F("ON") : F("OFF"));
        return;
    }


    // --------------------------------------------------------
    // MFSW - 0x5BF
    // --------------------------------------------------------

    if (command == "mfsw mode pulse")
    {
        mfsw.mode = MFSW_MODE_PULSE;
        mfsw.active = false;
        Serial.println(F("MFSW mode = PULSE: one 0x5BF frame, no release frame"));
        return;
    }

    if (command == "mfsw mode cyclic")
    {
        mfsw.mode = MFSW_MODE_CYCLIC;
        mfsw.active = false;
        mfswLastSend = 0;
        Serial.println(F("MFSW mode = CYCLIC: 0x5BF every 50 ms"));
        return;
    }

    if (command == "mfsw volup")
    {
        turnMfswWheel(MFSW_VOLUME_WHEEL, MFSW_DIR_POSITIVE);
        return;
    }

    if (command == "mfsw voldown")
    {
        turnMfswWheel(MFSW_VOLUME_WHEEL, MFSW_DIR_NEGATIVE);
        return;
    }

    if (command == "mfsw mute")
    {
        pressMfswButton(MFSW_VOLUME_PRESS, MFSW_SHORT_PRESS);
        return;
    }

    if (command == "mfsw next")
    {
        pressMfswButton(MFSW_NEXT, MFSW_SHORT_PRESS);
        return;
    }

    if (command == "mfsw previous")
    {
        pressMfswButton(MFSW_PREVIOUS, MFSW_SHORT_PRESS);
        return;
    }

    if (command == "mfsw source")
    {
        pressMfswButton(MFSW_SOURCE, MFSW_SHORT_PRESS);
        return;
    }

    if (command == "mfsw phone")
    {
        pressMfswButton(MFSW_PHONE, MFSW_SHORT_PRESS);
        return;
    }

    if (command == "mfsw voice")
    {
        pressMfswButton(MFSW_VOICE, MFSW_SHORT_PRESS);
        return;
    }

    if (command == "mfsw view")
    {
        pressMfswButton(MFSW_VIEW, MFSW_SHORT_PRESS);
        return;
    }

    if (command == "mfsw menu")
    {
        pressMfswButton(MFSW_ENTER_MENU, MFSW_SHORT_PRESS);
        return;
    }

    if (command == "mfsw assist")
    {
        pressMfswButton(MFSW_ASSIST, MFSW_SHORT_PRESS);
        return;
    }

    if (command == "mfsw wheel 01")
    {
        turnMfswWheel(MFSW_VOLUME_WHEEL, 0x01);
        return;
    }

    if (command == "mfsw wheel 0f")
    {
        turnMfswWheel(MFSW_VOLUME_WHEEL, 0x0F);
        return;
    }

    if (command.startsWith("mfsw pressstate "))
    {
        int value = command.substring(16).toInt();

        if (value < 1 || value > 6)
        {
            Serial.println(F("Usage: mfsw pressstate <1-6>"));
            return;
        }

        pressMfswButton(MFSW_VOLUME_PRESS, (byte)value);
        return;
    }

    if (command == "mfsw marker old")
    {
        mfsw.generationMarker = 0x13;
        Serial.println(F("MFSW marker = 0x13"));
        return;
    }

    if (command == "mfsw marker new")
    {
        mfsw.generationMarker = 0xA3;
        Serial.println(F("MFSW marker = 0xA3"));
        return;
    }


    // --------------------------------------------------------
    // MQB INFOTAINMENT REST BUS
    // --------------------------------------------------------

    if (command == "restbus on")
    {
        setRestBusEnabled(true);
        return;
    }

    if (command == "restbus off")
    {
        setRestBusEnabled(false);
        return;
    }

    if (command == "restbus")
    {
        printRestBus();
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
    Serial.println(F("MFSW 0x5BF"));
    Serial.println(F("----------"));

    Serial.print(F("Marker       : 0x"));
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

    Serial.println(F("MFSW generator: 0x5BF"));
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

    Serial.println(F("mfsw mode pulse"));
    Serial.println(F("mfsw mode cyclic"));
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
    Serial.println(F("mfsw"));
    Serial.println();

    Serial.println(F("state"));
    Serial.println(F("messages"));
    Serial.println(F("mark"));
    Serial.println();

    Serial.println(F("restbus on"));
    Serial.println(F("restbus off"));
    Serial.println(F("restbus"));
    Serial.println(F("monitor all"));
    Serial.println(F("monitor rx"));
    Serial.println(F("monitor tx"));
    Serial.println(F("monitor diag"));
    Serial.println(F("monitor off"));
    Serial.println();

    Serial.println(F("help"));
    Serial.println();
}
