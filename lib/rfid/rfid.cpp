#include "rfid.h"
#include <SPI.h>
#include <MFRC522.h>
#include "../logger/logger.h"
#include "../../include/config.h"

// Instantiate the reader using the pins from config.h
static MFRC522 s_mfrc522(PIN_RFID_SS, 255);

static uint8_t s_rfid_watchdog_failures = 0;
static bool    s_rfid_watchdog_faulted = false;

static bool _rfid_register_is_healthy(byte v) {
    return (v != 0x00 && v != 0xFF && v != 0x88);
}

static bool _rfid_soft_recover() {
    LOG_WARN("RFID", "Soft-recovering MFRC522 registers");
    s_mfrc522.PCD_Reset();
    vTaskDelay(pdMS_TO_TICKS(25));
    s_mfrc522.PCD_Init();
    vTaskDelay(pdMS_TO_TICKS(10));

    byte v = s_mfrc522.PCD_ReadRegister(s_mfrc522.VersionReg);
    if (_rfid_register_is_healthy(v)) {
        LOG_INFO("RFID", "Recovery OK. Version: 0x%02X", v);
        return true;
    }

    LOG_WARN("RFID", "Recovery attempt failed. Version: 0x%02X", v);
    return false;
}

bool rfid_init() {
    // ── THE FIX: Explicitly boot the SPI bus before init ──
    SPI.begin(PIN_RFID_SCK, PIN_RFID_MISO, PIN_RFID_MOSI, PIN_RFID_SS);
    
    s_mfrc522.PCD_Init();
    
    // Safety check just like the standalone test
    byte v = s_mfrc522.PCD_ReadRegister(s_mfrc522.VersionReg);
    if (!_rfid_register_is_healthy(v)) {
        LOG_ERROR("RFID", "Init failed! Version: 0x%02X", v);
        return false;
    }

    s_rfid_watchdog_failures = 0;
    s_rfid_watchdog_faulted = false;
    LOG_INFO("RFID", "Module OK. Version: 0x%02X", v);
    return true;
}

bool rfid_watchdog_tick() {
    byte v = s_mfrc522.PCD_ReadRegister(s_mfrc522.VersionReg);
    if (_rfid_register_is_healthy(v)) {
        s_rfid_watchdog_failures = 0;
        s_rfid_watchdog_faulted = false;
        return true;
    }

    s_rfid_watchdog_failures++;
    LOG_WARN("RFID", "Watchdog saw dead register signature 0x%02X (attempt %u/3)",
             v, s_rfid_watchdog_failures);

    if (!_rfid_soft_recover()) {
        if (s_rfid_watchdog_failures >= 3) {
            s_rfid_watchdog_faulted = true;
            LOG_ERROR("RFID", "RFID watchdog exhausted after 3 failed recovery attempts");
        }
        return false;
    }

    s_rfid_watchdog_failures = 0;
    s_rfid_watchdog_faulted = false;
    return true;
}

bool rfid_is_faulted() {
    return s_rfid_watchdog_faulted;
}

void rfid_mark_activity() {
    s_rfid_watchdog_failures = 0;
    s_rfid_watchdog_faulted = false;
}

RFIDResult rfid_poll(char* uid_out) {
    // Check if a card is present and readable
    if (!s_mfrc522.PICC_IsNewCardPresent() || !s_mfrc522.PICC_ReadCardSerial()) {
        return RFID_NO_CARD;
    }

    rfid_mark_activity();
    
    // Convert the raw bytes into a clean Hex String for our database
    uid_out[0] = '\0';
    char hex_buf[3];
    
    // We only take the first 4 bytes (8 characters) to fit our char[9] buffers safely
    byte read_size = (s_mfrc522.uid.size < 4) ? s_mfrc522.uid.size : 4;
    
    for (byte i = 0; i < read_size; i++) {
        snprintf(hex_buf, sizeof(hex_buf), "%02X", s_mfrc522.uid.uidByte[i]);
        strncat(uid_out, hex_buf, 8 - strlen(uid_out));
    }
    
    // Command the card to go to sleep so we don't read it 1,000 times a second
    s_mfrc522.PICC_HaltA(); 

    s_mfrc522.PCD_StopCrypto1();
    
    return RFID_NEW_CARD;
}
