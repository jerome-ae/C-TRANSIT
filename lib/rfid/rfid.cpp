#include "rfid.h"
#include "../logger/logger.h"
#include <MFRC522.h>
#include <SPI.h>

// Fallback in case DEBOUNCE_MS wasn't in config.h
#ifndef DEBOUNCE_MS
#define DEBOUNCE_MS 2000
#endif

static MFRC522 s_mfrc(PIN_RFID_SS, PIN_RFID_RST);
static char     s_last_uid[9]  = {0};
static uint32_t s_last_tap_ms  = 0;

static void _bytes_to_hex(const MFRC522::Uid& uid, char* out){
    char* p=out;
    for(uint8_t i=0;i<uid.size&&i<4;i++){ sprintf(p,"%02X",uid.uidByte[i]); p+=2; }
    *p='\0';
}

bool rfid_init(){
    // <-- IMPROVED: Force SPI to use our exact pins to prevent hardware mismatches
    SPI.begin(PIN_RFID_SCK, PIN_RFID_MISO, PIN_RFID_MOSI, PIN_RFID_SS);
    s_mfrc.PCD_Init();
    
    uint8_t fw=s_mfrc.PCD_ReadRegister(MFRC522::VersionReg);
    if(fw==0x00||fw==0xFF){ LOG_ERROR("RFID","Not found (FW=0x%02X)",fw); return false; }
    
    LOG_INFO("RFID","MFRC522 FW=0x%02X",fw);
    return true;
}

RFIDResult rfid_poll(char* out_uid){
    if(!out_uid) return RFID_READ_ERROR;
    if(!s_mfrc.PICC_IsNewCardPresent()) return RFID_NO_CARD;
    if(!s_mfrc.PICC_ReadCardSerial()){
        LOG_WARN("RFID","Serial read failed");
        return RFID_READ_ERROR;
    }
    
    char hex[9]; _bytes_to_hex(s_mfrc.uid, hex);
    uint32_t now=millis();
    
    if(strcmp(hex,s_last_uid)==0 && (now-s_last_tap_ms)<DEBOUNCE_MS){
        s_mfrc.PICC_HaltA(); s_mfrc.PCD_StopCrypto1();
        LOG_DEBUG("RFID","Debounce %s",hex);
        return RFID_DUPLICATE;
    }
    
    strncpy(s_last_uid,hex,sizeof(s_last_uid));
    s_last_tap_ms=now;
    strncpy(out_uid,hex,9);
    
    s_mfrc.PICC_HaltA(); s_mfrc.PCD_StopCrypto1();
    LOG_INFO("RFID","Card: %s",hex);
    return RFID_NEW_CARD;
}

void rfid_clear_debounce(){ memset(s_last_uid,0,sizeof(s_last_uid)); s_last_tap_ms=0; }

bool rfid_self_test(){
    uint8_t fw=s_mfrc.PCD_ReadRegister(MFRC522::VersionReg);
    return (fw!=0x00&&fw!=0xFF);
}