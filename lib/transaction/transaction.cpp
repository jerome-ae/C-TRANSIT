#include "transaction.h"
#include "../logger/logger.h"

static unsigned long s_base = 0;
static unsigned long s_anchor_ms = 0;

void transaction_set_rtc(unsigned long ts){
    if (ts < MIN_VALID_EPOCH) {
        LOG_ERROR("TX", "RTC seed rejected: ts=%lu below minimum valid epoch (%lu)", ts, (unsigned long)MIN_VALID_EPOCH);
        return;
    }
    s_base = ts;
    s_anchor_ms = millis();
    LOG_INFO("TX", "RTC seeded ts=%lu", ts);
}

unsigned long transaction_get_ts(){
    if (s_base == 0) {
        // Clock not seeded — return 0 so callers can detect invalid time.
        // transaction_time_synced() should be checked before relying on this value.
        return 0;
    } else {
        // Rollover-safe calculation: real epoch at seed + seconds elapsed since
        return s_base + ((millis() - s_anchor_ms) / 1000UL);
    }
}

bool transaction_time_synced(){
    return s_base != 0;
}

TransactionResult transaction_record(const char* uid, const char* drv){
    if(!uid || !drv) return TX_ERROR;

    // Refuse to record a transaction until we have a real, RTC-seeded
    // timestamp (via NTP or a broker-pushed SYS:TIME). Without this,
    // ts would silently fall back to seconds-since-boot in transaction_get_ts().
    if(!transaction_time_synced()){
        LOG_WARN("TX", "Reject uid=%s: not yet time-synced", uid);
        return TX_NOT_TIME_SYNCED;
    }

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
