#include "transaction.h"
#include "../logger/logger.h"

static unsigned long s_base = 0;
static unsigned long s_anchor_ms = 0;

void transaction_set_rtc(unsigned long ts){
    s_base = ts; 
    s_anchor_ms = millis();
    LOG_INFO("TX", "RTC seeded ts=%lu", ts);
}

unsigned long transaction_get_ts(){
    if (s_base == 0) {
        // Fallback if RTC was never seeded (should be blocked by state machine)
        return millis() / 1000UL;
    } else {
        // Rollover-safe calculation
        return s_base + ((millis() - s_anchor_ms) / 1000UL);
    }
}

TransactionResult transaction_record(const char* uid, const char* drv){
    if(!uid || !drv) return TX_ERROR;
    
    unsigned long ts = transaction_get_ts();
    
    // ── THE FIX: Fetch the fare from the LittleFS system config file
    int current_fare = storage_read_fare();
    
    // PHASE 8 SECURITY NOTE: 
    // In Phase 8, 'uid' will be hashed before being written. 
    // We will also generate and append an HMAC signature to this transaction 
    // row to guarantee data integrity against manual LittleFS tampering.
    LOG_INFO("TX", "Record uid=%s amt=%d ts=%lu drv=%s", uid, current_fare, ts, drv);
    
    StorageResult r = storage_append_tx(uid, current_fare, ts, drv);
    if(r == STORAGE_OK)   return TX_RECORDED;
    if(r == STORAGE_FULL) return TX_LOG_FULL;
    
    return TX_ERROR;
}
