/*
  Arduino MQB Bordcomputer Convenience-CAN Replay v1.1

  Purpose:
    - Connect an Arduino Uno + MCP2515 shield/module to the suspected MQB
      Convenience CAN bus.
    - Monitor traffic.
    - Replay the exact 0x17330F10 Bordcomputer frames captured from the
      user-provided Speed.zip file.
    - Preserve the original relative timing between those frames.

  Hardware baseline:
    Arduino Uno
    MCP2515 with 16 MHz crystal
    MCP2515 CS  -> D10
    MCP2515 INT -> D2
    CAN-H/CAN-L -> target CAN bus
    Common GND

  Library:
    "MCP_CAN_lib" / mcp_can.h

  Default bus speed:
    500 kbit/s

  Serial:
    115200 baud

  Commands:
    help
    monitor on
    monitor off
    replay once
    replay on
    replay off
    stats

  IMPORTANT:
    This sketch does not assume that 0x17330F10 is correct for the
    Infotainment CAN. The whole point is to test it on the suspected
    Convenience CAN segment.

    Captured replay:
      56 frames
      source: Speed.zip / Speed/0-103-0 kmh all ID.txt
      capture span: 64393 ms

  Notes:
    - Replay bytes are stored in PROGMEM to preserve Uno SRAM.
    - The sketch also highlights any 0x17330F00 / 0x17330F01 /
      0x17330F10 frames it sees on the bus.
*/

#include <SPI.h>
#include <mcp_can.h>
#include <avr/pgmspace.h>

static const byte CAN_CS_PIN = 10;
static const byte CAN_INT_PIN = 2;

MCP_CAN CAN(CAN_CS_PIN);

// MQB Bordcomputer BAP IDs
static const unsigned long BC_REQ_ID  = 0x17330F00UL;
static const unsigned long BC_AUX_ID  = 0x17330F01UL;
static const unsigned long BC_DATA_ID = 0x17330F10UL;

struct ReplayFrame
{
  uint32_t delayFromPreviousMs;
  uint8_t dlc;
  uint8_t data[8];
};

const ReplayFrame replayFrames[] PROGMEM =
{
  {0UL, 8, {0x80, 0x12, 0x33, 0xFD, 0x99, 0x01, 0x00, 0x03}},
  {810UL, 8, {0xA0, 0x12, 0x43, 0xDA, 0x99, 0x01, 0x00, 0xFF}},
  {51UL, 8, {0xC0, 0xFF, 0x00, 0x65, 0x00, 0x00, 0x00, 0x00}},
  {755UL, 8, {0x43, 0xFE, 0x14, 0x00, 0x02, 0x03, 0x80, 0x00}},
  {247UL, 8, {0x43, 0xFE, 0x98, 0x01, 0x00, 0x03, 0x80, 0x00}},
  {710UL, 8, {0x43, 0xD5, 0x95, 0x01, 0x00, 0xFF, 0xFF, 0x00}},
  {400UL, 8, {0x43, 0xD5, 0x90, 0x01, 0x00, 0xFF, 0xFF, 0x00}},
  {48UL, 8, {0x43, 0xFE, 0x90, 0x01, 0x00, 0x03, 0x80, 0x00}},
  {2939UL, 8, {0x80, 0x08, 0x33, 0xC3, 0x38, 0x07, 0xEF, 0xFD}},
  {991UL, 3, {0x33, 0xC4, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00}},
  {2940UL, 8, {0xD1, 0x10, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00}},
  {2030UL, 8, {0xC0, 0xFF, 0x00, 0x66, 0x00, 0x00, 0x00, 0x00}},
  {64UL, 8, {0xE0, 0xFF, 0x00, 0x66, 0x00, 0x00, 0x00, 0x00}},
  {92UL, 8, {0xE1, 0x9B, 0x01, 0x00, 0x00, 0x14, 0x00, 0x00}},
  {2525UL, 5, {0x33, 0xD2, 0x00, 0x96, 0x00, 0x00, 0x00, 0x00}},
  {2430UL, 6, {0x43, 0xF9, 0x44, 0x02, 0x00, 0x01, 0x00, 0x00}},
  {2984UL, 8, {0x90, 0x12, 0x33, 0xD8, 0x99, 0x01, 0x00, 0xFF}},
  {105UL, 8, {0xD1, 0x10, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00}},
  {1128UL, 8, {0x90, 0x12, 0x43, 0xD8, 0x99, 0x01, 0x00, 0xFF}},
  {515UL, 8, {0x43, 0xD5, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00}},
  {47UL, 8, {0x43, 0xFE, 0x00, 0x00, 0x00, 0x03, 0x80, 0x00}},
  {2789UL, 8, {0xC0, 0xFF, 0x00, 0x66, 0x00, 0x00, 0x00, 0x00}},
  {943UL, 8, {0xA0, 0x12, 0x33, 0xDA, 0x99, 0x01, 0x00, 0xFF}},
  {1023UL, 6, {0x33, 0xDB, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00}},
  {987UL, 4, {0x33, 0xDC, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00}},
  {2153UL, 6, {0x43, 0xF9, 0x62, 0x02, 0x00, 0x01, 0x00, 0x00}},
  {1889UL, 8, {0x43, 0xD5, 0x94, 0x01, 0x00, 0xFF, 0xFF, 0x00}},
  {1444UL, 8, {0x43, 0xFE, 0xC0, 0x00, 0x00, 0x03, 0x80, 0x00}},
  {600UL, 8, {0x43, 0xFE, 0x90, 0x01, 0x00, 0x03, 0x80, 0x00}},
  {1994UL, 8, {0xC0, 0x80, 0x00, 0x99, 0x01, 0x00, 0x03, 0x80}},
  {2997UL, 5, {0xC0, 0x80, 0x00, 0x00, 0x67, 0x00, 0x00, 0x00}},
  {2442UL, 8, {0x43, 0xD5, 0x97, 0x01, 0x00, 0xFF, 0xFF, 0x00}},
  {49UL, 8, {0x43, 0xFE, 0x97, 0x01, 0x00, 0x03, 0x80, 0x00}},
  {552UL, 8, {0x43, 0xFE, 0x96, 0x01, 0x00, 0x03, 0x80, 0x00}},
  {69UL, 8, {0x90, 0x12, 0x43, 0xD8, 0x99, 0x01, 0x00, 0xFF}},
  {893UL, 8, {0x43, 0xD5, 0xE4, 0x00, 0x00, 0xFF, 0xFF, 0x00}},
  {388UL, 8, {0x43, 0xD5, 0x5D, 0x01, 0x00, 0xFF, 0xFF, 0x00}},
  {48UL, 8, {0x43, 0xFE, 0x5D, 0x01, 0x00, 0x03, 0x80, 0x00}},
  {2969UL, 6, {0x43, 0xF9, 0x80, 0x02, 0x00, 0x01, 0x00, 0x00}},
  {1044UL, 4, {0xC0, 0x80, 0x01, 0x16, 0x00, 0x00, 0x00, 0x00}},
  {6055UL, 8, {0x33, 0xD6, 0xA0, 0x00, 0x00, 0xFF, 0xFF, 0x00}},
  {999UL, 7, {0x33, 0xD7, 0x40, 0x93, 0x09, 0x00, 0x00, 0x00}},
  {1006UL, 8, {0x90, 0x12, 0x33, 0xD8, 0x99, 0x01, 0x00, 0xFF}},
  {49UL, 8, {0xD0, 0xFF, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00}},
  {591UL, 8, {0x43, 0xD5, 0x90, 0x01, 0x00, 0xFF, 0xFF, 0x00}},
  {50UL, 8, {0x43, 0xFE, 0x90, 0x01, 0x00, 0x03, 0x80, 0x00}},
  {982UL, 8, {0xC0, 0xFF, 0x00, 0x67, 0x00, 0x00, 0x00, 0x00}},
  {1804UL, 8, {0xE0, 0xFF, 0x00, 0x67, 0x00, 0x00, 0x00, 0x00}},
  {1363UL, 6, {0x43, 0xF9, 0x8A, 0x02, 0x00, 0x01, 0x00, 0x00}},
  {809UL, 8, {0x80, 0x12, 0x43, 0xD9, 0x99, 0x01, 0x00, 0xFF}},
  {51UL, 8, {0xA0, 0x12, 0x43, 0xDA, 0x99, 0x01, 0x00, 0xFF}},
  {51UL, 8, {0xC0, 0xFF, 0x00, 0x67, 0x00, 0x00, 0x00, 0x00}},
  {48UL, 8, {0xE0, 0xFF, 0x00, 0x67, 0x00, 0x00, 0x00, 0x00}},
  {498UL, 8, {0x43, 0xFE, 0x96, 0x01, 0x00, 0x03, 0x80, 0x00}},
  {101UL, 8, {0x43, 0xFE, 0x0A, 0x00, 0x02, 0x03, 0x80, 0x00}},
  {2852UL, 8, {0x43, 0xD5, 0x0A, 0x00, 0x02, 0xFF, 0xFF, 0x00}},
};

static const uint8_t REPLAY_FRAME_COUNT =
  sizeof(replayFrames) / sizeof(replayFrames[0]);

bool monitorEnabled = true;
bool replayActive = false;
bool replayLoop = false;

uint8_t replayIndex = 0;
unsigned long replayNextDue = 0;
unsigned long replaySent = 0;
unsigned long rxCount = 0;
unsigned long txErrors = 0;
unsigned long bcReqCount = 0;
unsigned long bcAuxCount = 0;
unsigned long bcDataCount = 0;

static const unsigned long REPLAY_LOOP_GAP_MS = 1000UL;

String serialBuffer;

void printHexByte(uint8_t value)
{
  if (value < 0x10)
    Serial.print('0');
  Serial.print(value, HEX);
}

void printFrame(const char *direction,
                unsigned long id,
                bool extended,
                uint8_t dlc,
                const uint8_t *data)
{
  Serial.print(millis());
  Serial.print(F(" ms  "));
  Serial.print(direction);
  Serial.print(F("  "));
  Serial.print(extended ? F("EXT  0x") : F("STD  0x"));
  Serial.print(id, HEX);
  Serial.print(F("  ["));
  Serial.print(dlc);
  Serial.print(F("]  "));

  for (uint8_t i = 0; i < dlc; i++)
  {
    printHexByte(data[i]);
    if (i + 1 < dlc)
      Serial.print(' ');
  }

  if (id == BC_REQ_ID)
    Serial.print(F("  [BC REQUEST 0x17330F00]"));
  else if (id == BC_AUX_ID)
    Serial.print(F("  [BC AUX 0x17330F01]"));
  else if (id == BC_DATA_ID)
    Serial.print(F("  [BC DATA 0x17330F10]"));

  Serial.println();
}

void readReplayFrame(uint8_t index, ReplayFrame &frame)
{
  memcpy_P(&frame, &replayFrames[index], sizeof(ReplayFrame));
}

bool sendExtendedFrame(unsigned long id, uint8_t dlc, const uint8_t *data)
{
  // MCP_CAN sendMsgBuf(id, ext, len, buf)
  byte result = CAN.sendMsgBuf(id, 1, dlc, (byte *)data);

  if (result == CAN_OK)
  {
    if (monitorEnabled)
      printFrame("TX", id, true, dlc, data);
    return true;
  }

  txErrors++;
  Serial.print(F("CAN TX ERROR, code="));
  Serial.println(result);
  return false;
}

void startReplay(bool loopMode)
{
  replayActive = true;
  replayLoop = loopMode;
  replayIndex = 0;
  replaySent = 0;
  replayNextDue = millis();

  Serial.print(F("BC replay START: "));
  Serial.println(loopMode ? F("LOOP") : F("ONCE"));
  Serial.print(F("Frames: "));
  Serial.println(REPLAY_FRAME_COUNT);
}

void stopReplay()
{
  replayActive = false;
  replayLoop = false;
  replayIndex = 0;
  Serial.println(F("BC replay OFF"));
}

void updateReplay()
{
  if (!replayActive)
    return;

  unsigned long now = millis();

  if ((long)(now - replayNextDue) < 0)
    return;

  ReplayFrame frame;
  readReplayFrame(replayIndex, frame);

  sendExtendedFrame(BC_DATA_ID, frame.dlc, frame.data);
  replaySent++;

  replayIndex++;

  if (replayIndex >= REPLAY_FRAME_COUNT)
  {
    if (replayLoop)
    {
      replayIndex = 0;
      replayNextDue = now + REPLAY_LOOP_GAP_MS;
      Serial.println(F("BC replay loop restart"));
    }
    else
    {
      replayActive = false;
      Serial.println(F("BC replay COMPLETE"));
    }

    return;
  }

  ReplayFrame nextFrame;
  readReplayFrame(replayIndex, nextFrame);
  replayNextDue = now + nextFrame.delayFromPreviousMs;
}

void readCan()
{
  while (CAN_MSGAVAIL == CAN.checkReceive())
  {
    unsigned long rawId = 0;
    byte dlc = 0;
    byte data[8] = {0};

    CAN.readMsgBuf(&rawId, &dlc, data);
    rxCount++;

    // MCP_CAN_lib may encode frame-type flags in the upper bits of the
    // returned ID. Bit 31 is commonly used to indicate an extended frame.
    // Always mask the actual 29-bit CAN identifier before comparing/printing.
    bool extended = (rawId & 0x80000000UL) != 0;
    unsigned long id = rawId & 0x1FFFFFFFUL;

    bool isBcReq  = (id == BC_REQ_ID);
    bool isBcAux  = (id == BC_AUX_ID);
    bool isBcData = (id == BC_DATA_ID);
    bool isBc = isBcReq || isBcAux || isBcData;

    if (isBcReq)  bcReqCount++;
    if (isBcAux)  bcAuxCount++;
    if (isBcData) bcDataCount++;

    // Always print Bordcomputer traffic, even if general monitoring is off.
    if (monitorEnabled || isBc)
      printFrame("RX", id, extended, dlc, data);
  }
}

void printHelp()
{
  Serial.println();
  Serial.println(F("Commands:"));
  Serial.println(F("  help"));
  Serial.println(F("  monitor on"));
  Serial.println(F("  monitor off"));
  Serial.println(F("  replay once"));
  Serial.println(F("  replay on"));
  Serial.println(F("  replay off"));
  Serial.println(F("  stats"));
  Serial.println();
}

void printStats()
{
  Serial.print(F("RX="));
  Serial.print(rxCount);
  Serial.print(F("  replayTX="));
  Serial.print(replaySent);
  Serial.print(F("  txErrors="));
  Serial.print(txErrors);
  Serial.print(F("  replay="));
  Serial.print(replayActive ? F("ON") : F("OFF"));
  Serial.print(F("  mode="));
  Serial.println(replayLoop ? F("LOOP") : F("ONCE"));

  Serial.print(F("BC 0x17330F00 RX="));
  Serial.print(bcReqCount);
  Serial.print(F("  0x17330F01 RX="));
  Serial.print(bcAuxCount);
  Serial.print(F("  0x17330F10 RX="));
  Serial.println(bcDataCount);
}

void processCommand(String command)
{
  command.trim();
  command.toLowerCase();

  if (command == "help")
  {
    printHelp();
    return;
  }

  if (command == "monitor on")
  {
    monitorEnabled = true;
    Serial.println(F("CAN monitor ON"));
    return;
  }

  if (command == "monitor off")
  {
    monitorEnabled = false;
    Serial.println(F("CAN monitor OFF (BC IDs still shown)"));
    return;
  }

  if (command == "replay once")
  {
    startReplay(false);
    return;
  }

  if (command == "replay on")
  {
    startReplay(true);
    return;
  }

  if (command == "replay off")
  {
    stopReplay();
    return;
  }

  if (command == "stats")
  {
    printStats();
    return;
  }

  Serial.print(F("Unknown command: "));
  Serial.println(command);
}

void readSerial()
{
  while (Serial.available())
  {
    char c = Serial.read();

    if (c == '\r')
      continue;

    if (c == '\n')
    {
      if (serialBuffer.length() > 0)
        processCommand(serialBuffer);

      serialBuffer = "";
      continue;
    }

    // Prevent unbounded String growth on accidental garbage input.
    if (serialBuffer.length() < 64)
      serialBuffer += c;
  }
}

void setup()
{
  Serial.begin(115200);
  delay(400);

  Serial.println();
  Serial.println(F("MQB Bordcomputer Convenience-CAN Replay v1.1"));
  Serial.println(F("Arduino Uno + MCP2515 16 MHz"));
  Serial.println(F("CAN: 500 kbit/s"));
  Serial.println();

  pinMode(CAN_INT_PIN, INPUT);

  // MCP_16MHZ is critical for the user's 16 MHz MCP2515 hardware.
  byte result = CAN.begin(MCP_ANY, CAN_500KBPS, MCP_16MHZ);

  if (result != CAN_OK)
  {
    Serial.print(F("MCP2515 init FAILED, code="));
    Serial.println(result);

    while (true)
    {
      delay(1000);
    }
  }

  CAN.setMode(MCP_NORMAL);

  Serial.println(F("MCP2515 init OK"));
  Serial.println(F("Monitor is ON"));
  Serial.println(F("First test: observe bus traffic BEFORE replaying."));
  Serial.println(F("Extended CAN IDs are masked to the real 29-bit value."));
  printHelp();
}

void loop()
{
  readSerial();
  readCan();
  updateReplay();
}
