#include <SPI.h>
#include <mcp_can.h>

/*
 * MQB CAN Bench Simulator
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
    byte dimming58xd;
    byte dimming58xs;
    byte dimming58xt;
    bool nightDesign;
    uint16_t photoSensor;
};

LightingState lighting =
{
    100,
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
    bool clock24h;
    byte dateFormat;
    bool temperatureFahrenheit;
};

UnitsState units =
{
    true,
    0,
    false
};


// ============================================================
// MFSW TEST STATE
// ============================================================

enum MfswMode
{
    MFSW_MODE_5C1,
    MFSW_MODE_5BF,
    MFSW_MODE_BOTH
};

struct MfswState
{
    MfswMode mode;
    byte activeCode;
    unsigned long releaseAt;
    bool pressed;
};

MfswState mfsw =
{
    MFSW_MODE_BOTH,
    0x00,
    0,
    false
};

const byte MFSW_RELEASE  = 0x00;
const byte MFSW_NEXT     = 0x02;
const byte MFSW_PREVIOUS = 0x03;
const byte MFSW_VOL_UP   = 0x06;
const byte MFSW_VOL_DOWN = 0x07;
const byte MFSW_PHONE    = 0x1A;
const byte MFSW_UP       = 0x22;
const byte MFSW_DOWN     = 0x23;
const byte MFSW_OK       = 0x28;
const byte MFSW_VOICE    = 0x2A;
const byte MFSW_MUTE     = 0x2B;

const unsigned long MFSW_PRESS_TIME_MS = 300;
const unsigned long MFSW_PERIOD_MS = 100;
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

void sendMessages();
bool sendCanMessage(CanMessage &message);
bool sendRawCan(unsigned long id, bool extended, byte dlc, byte *data);

void updateClock();
void incrementClockOneSecond();
bool isLeapYear(uint16_t year);
byte daysInMonth(uint16_t year, byte month);

void updateMfsw();
void pressMfsw(byte code);
void sendMfswFrame(unsigned long id, byte code);

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
void printMfsw();


// ============================================================
// MESSAGE TABLE
// ============================================================

CanMessage messages[] =
{
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
        100,
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
    }
};

const byte MESSAGE_COUNT = sizeof(messages) / sizeof(messages[0]);


// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println(F("======================================"));
    Serial.println(F("MQB CAN Bench Simulator"));
    Serial.println(F("======================================"));
    Serial.println();

    Serial.print(F("Initializing MCP2515... "));

    byte result = CAN.begin(MCP_ANY, CAN_SPEED, CAN_CLOCK);

    if (result != CAN_OK)
    {
        Serial.println(F("FAILED"));

        while (true)
        {
            delay(1000);
        }
    }

    Serial.println(F("OK"));

    CAN.setMode(MCP_NORMAL);

    Serial.println(F("CAN mode : NORMAL"));
    Serial.println(F("CAN speed: 500 kbit/s"));
    Serial.println(F("CAN clock: 16 MHz"));
    Serial.println();

    unsigned long now = millis();

    for (byte i = 0; i < MESSAGE_COUNT; i++)
    {
        messages[i].lastSend = now - messages[i].interval;
    }

    clockState.lastTick = now;
    mfswLastSend = now;

    printState();
    printHelp();
}


// ============================================================
// MAIN LOOP
// ============================================================

void loop()
{
    updateClock();
    updateMfsw();
    sendMessages();
    monitorCan();
    processSerial();
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
     * Experimental MQB DBC mapping:
     *
     * bits 0..7   DI_KL_58xd
     * bits 8..14  DI_KL_58xs
     * bit  15     DI_Display_Nachtdesign
     * bits 16..22 DI_KL_58xt
     * bits 24..39 DI_Fotosensor
     */

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
     * Year is encoded as years since 2000 in this bench
     * implementation. This can easily be changed if the target
     * cluster/5F capture shows another raw year convention.
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
    /*
     * Keep this frame deliberately conservative.
     *
     * The exact raw enum values for every unit differ between
     * DBC revisions. The frame is transmitted so it can be
     * tested on the bench without inventing unrelated unit data.
     *
     * Byte 0 bit 0 is used here as the 12/24 h test flag.
     * Byte 0 bits 1..2 are used as the date-format test value.
     * Byte 0 bit 3 is used as the C/F test flag.
     *
     * These three fields are experimental until confirmed by a
     * matching MQB capture for the exact 5F generation.
     */

    for (byte i = 0; i < 8; i++)
    {
        message.data[i] = 0x00;
    }

    if (units.clock24h)
    {
        message.data[0] |= 0x01;
    }

    message.data[0] |= (units.dateFormat & 0x03) << 1;

    if (units.temperatureFahrenheit)
    {
        message.data[0] |= 0x08;
    }
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
// EXPERIMENTAL MFSW
// ============================================================

void pressMfsw(byte code)
{
    mfsw.activeCode = code;
    mfsw.pressed = true;
    mfsw.releaseAt = millis() + MFSW_PRESS_TIME_MS;

    Serial.print(F("MFSW press: 0x"));

    if (code < 0x10)
    {
        Serial.print('0');
    }

    Serial.println(code, HEX);
}

void updateMfsw()
{
    unsigned long now = millis();

    if (mfsw.pressed && (long)(now - mfsw.releaseAt) >= 0)
    {
        mfsw.activeCode = MFSW_RELEASE;
        mfsw.pressed = false;
    }

    if ((now - mfswLastSend) < MFSW_PERIOD_MS)
    {
        return;
    }

    mfswLastSend = now;

    if (mfsw.mode == MFSW_MODE_5C1 || mfsw.mode == MFSW_MODE_BOTH)
    {
        sendMfswFrame(0x5C1, mfsw.activeCode);
    }

    if (mfsw.mode == MFSW_MODE_5BF || mfsw.mode == MFSW_MODE_BOTH)
    {
        sendMfswFrame(0x5BF, mfsw.activeCode);
    }
}

void sendMfswFrame(unsigned long id, byte code)
{
    /*
     * PQ-style test payload.
     *
     * 0x5C1 is sent as:
     *   code 00 00 50
     *
     * 0x5BF is sent as:
     *   code 00 00 00 00 00 00 00
     *
     * This is intentionally marked experimental for MQB 5F.
     */

    byte data[8] = {0};

    data[0] = code;

    if (id == 0x5C1)
    {
        data[3] = 0x50;
        sendRawCan(id, false, 4, data);
    }
    else
    {
        sendRawCan(id, false, 8, data);
    }
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

    if (command.startsWith("dimming "))
    {
        int value = command.substring(8).toInt();

        if (value < 0) value = 0;
        if (value > 100) value = 100;

        lighting.dimming58xd = (byte)value;
        lighting.dimming58xs = (byte)value;
        lighting.dimming58xt = (byte)value;

        Serial.print(F("Dimming = "));
        Serial.print(value);
        Serial.println(F("%"));
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
        units.clock24h = true;
        Serial.println(F("Clock format test = 24 h"));
        return;
    }

    if (command == "clock 12h")
    {
        units.clock24h = false;
        Serial.println(F("Clock format test = 12 h"));
        return;
    }


    // --------------------------------------------------------
    // EXPERIMENTAL MFSW
    // --------------------------------------------------------

    if (command == "mfsw volup")
    {
        pressMfsw(MFSW_VOL_UP);
        return;
    }

    if (command == "mfsw voldown")
    {
        pressMfsw(MFSW_VOL_DOWN);
        return;
    }

    if (command == "mfsw mute")
    {
        pressMfsw(MFSW_MUTE);
        return;
    }

    if (command == "mfsw next")
    {
        pressMfsw(MFSW_NEXT);
        return;
    }

    if (command == "mfsw previous")
    {
        pressMfsw(MFSW_PREVIOUS);
        return;
    }

    if (command == "mfsw phone")
    {
        pressMfsw(MFSW_PHONE);
        return;
    }

    if (command == "mfsw voice")
    {
        pressMfsw(MFSW_VOICE);
        return;
    }

    if (command == "mfsw up")
    {
        pressMfsw(MFSW_UP);
        return;
    }

    if (command == "mfsw down")
    {
        pressMfsw(MFSW_DOWN);
        return;
    }

    if (command == "mfsw ok")
    {
        pressMfsw(MFSW_OK);
        return;
    }

    if (command == "mfsw mode 5c1")
    {
        mfsw.mode = MFSW_MODE_5C1;
        Serial.println(F("MFSW mode = 0x5C1"));
        return;
    }

    if (command == "mfsw mode 5bf")
    {
        mfsw.mode = MFSW_MODE_5BF;
        Serial.println(F("MFSW mode = 0x5BF"));
        return;
    }

    if (command == "mfsw mode both")
    {
        mfsw.mode = MFSW_MODE_BOTH;
        Serial.println(F("MFSW mode = 0x5C1 + 0x5BF"));
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
    Serial.println(F("MFSW test"));
    Serial.println(F("---------"));

    Serial.print(F("Mode         : "));

    if (mfsw.mode == MFSW_MODE_5C1)
    {
        Serial.println(F("0x5C1"));
    }
    else if (mfsw.mode == MFSW_MODE_5BF)
    {
        Serial.println(F("0x5BF"));
    }
    else
    {
        Serial.println(F("0x5C1 + 0x5BF"));
    }

    Serial.print(F("Active code  : 0x"));

    if (mfsw.activeCode < 0x10)
    {
        Serial.print('0');
    }

    Serial.println(mfsw.activeCode, HEX);
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

    Serial.println(F("MFSW test generator: 0x5C1 / 0x5BF, 100 ms"));
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

    Serial.println(F("mfsw volup"));
    Serial.println(F("mfsw voldown"));
    Serial.println(F("mfsw mute"));
    Serial.println(F("mfsw next"));
    Serial.println(F("mfsw previous"));
    Serial.println(F("mfsw phone"));
    Serial.println(F("mfsw voice"));
    Serial.println(F("mfsw up"));
    Serial.println(F("mfsw down"));
    Serial.println(F("mfsw ok"));
    Serial.println(F("mfsw mode 5c1"));
    Serial.println(F("mfsw mode 5bf"));
    Serial.println(F("mfsw mode both"));
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
