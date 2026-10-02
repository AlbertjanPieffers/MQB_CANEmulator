#include <Arduino.h>
#include "driver/twai.h"

/*
 * ESP32-S3 + SN65HVD230 minimal CAN sniffer / diagnostics
 *
 * GPIO17 -> SN65HVD230 D / TXD
 * GPIO18 <- SN65HVD230 R / RXD
 *
 * CAN: 500 kbit/s
 * Mode: LISTEN ONLY
 */

static constexpr gpio_num_t CAN_TX_PIN = GPIO_NUM_17;
static constexpr gpio_num_t CAN_RX_PIN = GPIO_NUM_18;

static constexpr uint32_t SERIAL_BAUD = 115200;
static constexpr uint32_t STATUS_INTERVAL_MS = 2000;

uint32_t rxFrameCount = 0;
uint32_t lastStatusMs = 0;

const char *twaiStateToString(twai_state_t state)
{
    switch (state)
    {
        case TWAI_STATE_STOPPED:    return "STOPPED";
        case TWAI_STATE_RUNNING:    return "RUNNING";
        case TWAI_STATE_BUS_OFF:    return "BUS_OFF";
        case TWAI_STATE_RECOVERING: return "RECOVERING";
        default:                    return "UNKNOWN";
    }
}

void printFrame(const twai_message_t &msg)
{
    Serial.print('[');
    Serial.print(millis());
    Serial.print(F(" ms] "));

    if (msg.extd)
    {
        Serial.print(F("EXT 0x"));
        if (msg.identifier < 0x10000000UL) Serial.print('0');
        if (msg.identifier < 0x01000000UL) Serial.print('0');
        if (msg.identifier < 0x00100000UL) Serial.print('0');
        if (msg.identifier < 0x00010000UL) Serial.print('0');
        if (msg.identifier < 0x00001000UL) Serial.print('0');
        if (msg.identifier < 0x00000100UL) Serial.print('0');
        if (msg.identifier < 0x00000010UL) Serial.print('0');
    }
    else
    {
        Serial.print(F("STD 0x"));
        if (msg.identifier < 0x100UL) Serial.print('0');
        if (msg.identifier < 0x10UL) Serial.print('0');
    }

    Serial.print(msg.identifier, HEX);
    Serial.print(F("  DLC "));
    Serial.print(msg.data_length_code);
    Serial.print(F("  "));

    if (msg.rtr)
    {
        Serial.print(F("RTR"));
    }
    else
    {
        for (uint8_t i = 0; i < msg.data_length_code && i < 8; i++)
        {
            if (msg.data[i] < 0x10) Serial.print('0');
            Serial.print(msg.data[i], HEX);
            if (i + 1 < msg.data_length_code) Serial.print(' ');
        }
    }

    Serial.println();
}

void printTwaiStatus()
{
    twai_status_info_t status = {};

    esp_err_t result = twai_get_status_info(&status);

    if (result != ESP_OK)
    {
        Serial.print(F("[STATUS] twai_get_status_info failed: "));
        Serial.println((int)result);
        return;
    }

    Serial.println();
    Serial.println(F("========== TWAI STATUS =========="));

    Serial.print(F("State               : "));
    Serial.println(twaiStateToString(status.state));

    Serial.print(F("RX frames seen      : "));
    Serial.println(rxFrameCount);

    Serial.print(F("RX messages waiting : "));
    Serial.println(status.msgs_to_rx);

    Serial.print(F("TX messages waiting : "));
    Serial.println(status.msgs_to_tx);

    Serial.print(F("TX error counter    : "));
    Serial.println(status.tx_error_counter);

    Serial.print(F("RX error counter    : "));
    Serial.println(status.rx_error_counter);

    Serial.print(F("TX failed count     : "));
    Serial.println(status.tx_failed_count);

    Serial.print(F("RX missed count     : "));
    Serial.println(status.rx_missed_count);

    Serial.print(F("Arbitration lost    : "));
    Serial.println(status.arb_lost_count);

    Serial.print(F("Bus error count     : "));
    Serial.println(status.bus_error_count);

    Serial.println(F("================================="));
    Serial.println();
}

bool initTwai()
{
    twai_general_config_t general =
        TWAI_GENERAL_CONFIG_DEFAULT(
            CAN_TX_PIN,
            CAN_RX_PIN,
            TWAI_MODE_LISTEN_ONLY
        );

    general.tx_queue_len = 0;
    general.rx_queue_len = 128;

    twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t result = twai_driver_install(&general, &timing, &filter);

    if (result != ESP_OK)
    {
        Serial.print(F("twai_driver_install failed: "));
        Serial.println((int)result);
        return false;
    }

    result = twai_start();

    if (result != ESP_OK)
    {
        Serial.print(F("twai_start failed: "));
        Serial.println((int)result);
        return false;
    }

    return true;
}

void setup()
{
    Serial.begin(SERIAL_BAUD);
    delay(2500);

    Serial.println();
    Serial.println(F("=========================================="));
    Serial.println(F("ESP32-S3 + SN65HVD230 CAN Sniffer Test"));
    Serial.println(F("=========================================="));
    Serial.println();

    Serial.print(F("CAN TX pin : GPIO"));
    Serial.println((int)CAN_TX_PIN);

    Serial.print(F("CAN RX pin : GPIO"));
    Serial.println((int)CAN_RX_PIN);

    Serial.println(F("Bitrate    : 500 kbit/s"));
    Serial.println(F("Mode       : LISTEN ONLY"));
    Serial.println();

    if (!initTwai())
    {
        Serial.println(F("TWAI initialization FAILED."));
        return;
    }

    Serial.println(F("TWAI initialization OK."));
    Serial.println(F("Listening..."));
    Serial.println();

    lastStatusMs = millis();
}

void loop()
{
    twai_message_t message = {};

    while (twai_receive(&message, 0) == ESP_OK)
    {
        rxFrameCount++;
        printFrame(message);
    }

    uint32_t now = millis();

    if ((now - lastStatusMs) >= STATUS_INTERVAL_MS)
    {
        lastStatusMs = now;
        printTwaiStatus();
    }

    delay(1);
}
