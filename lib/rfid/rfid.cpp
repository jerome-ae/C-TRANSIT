#include "rfid.h"
#include <SPI.h>
#include <MFRC522.h>
#include "../logger/logger.h"
#include "../../include/config.h"

// Instantiate the reader using the pins from config.h
static MFRC522 s_mfrc522(PIN_RFID_SS, PIN_RFID_RST);

bool rfid_init() {
    // ── THE FIX: Explicitly boot the SPI bus before init ──
    SPI.begin(PIN_RFID_SCK, PIN_RFID_MISO, PIN_RFID_MOSI, PIN_RFID_SS);
    
    s_mfrc522.PCD_Init();
    
    // Safety check just like the standalone test
    byte v = s_mfrc522.PCD_ReadRegister(s_mfrc522.VersionReg);
    if (v == 0x00 || v == 0xFF) {
        LOG_ERROR("RFID", "Init failed! Version: 0x%02X", v);
        return false;
    }
    
    LOG_INFO("RFID", "Module OK. Version: 0x%02X", v);
    return true;
}

RFIDResult rfid_poll(char* uid_out) {
    // Check if a card is present and readable
    if (!s_mfrc522.PICC_IsNewCardPresent() || !s_mfrc522.PICC_ReadCardSerial()) {
        return RFID_NO_CARD;
    }
    
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
    
    return RFID_NEW_CARD;
}
