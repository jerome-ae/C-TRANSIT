#include "transaction.h"
#include "../logger/logger.h" // <-- FIXED path

// <-- ADDED: Fallback if FARE_AMOUNT is missing from config
#ifndef FARE_AMOUNT
#define FARE_AMOUNT -200
#endif

static unsigned long s_base = 0;
static unsigned long s_anchor_ms = 0;

void transaction_set_rtc(unsigned long ts){
    s_base = ts; 
    s_anchor_ms = millis();
    LOG_INFO("TX","RTC seeded ts=%lu", ts);
}

unsigned long transaction_get_ts(){
    if (s_base == 0) {
        return millis() / 1000UL;
    } else {
        return s_base + ((millis() - s_anchor_ms) / 1000UL);
    }
}

TransactionResult transaction_record(const char* uid, const char* drv){
    if(!uid || !drv) return TX_ERROR;
    
    unsigned long ts = transaction_get_ts();
    LOG_INFO("TX","Record uid=%s amt=%d ts=%lu drv=%s", uid, FARE_AMOUNT, ts, drv);
    
    StorageResult r = storage_append_tx(uid, FARE_AMOUNT, ts, drv);
    if(r == STORAGE_OK)   return TX_RECORDED;
    if(r == STORAGE_FULL) return TX_LOG_FULL;
    
    return TX_ERROR;
}