#include "transaction.h"
#include "../logger/logger.h"
#include <LittleFS.h>

static unsigned long s_base = 0;
static unsigned long s_anchor_ms = 0;

void transaction_set_rtc(unsigned long ts){
    if (ts < MIN_VALID_EPOCH) {
        LOG_ERROR("TX", "RTC seed rejected: ts=%lu below minimum valid epoch (%lu)", ts, (unsigned long)MIN_VALID_EPOCH);
        return;
    }
    if (ts > MAX_REASONABLE_EPOCH) {
        LOG_ERROR("TX", "RTC seed rejected: ts=%lu beyond maximum valid epoch (%lu)", ts, (unsigned long)MAX_REASONABLE_EPOCH);
        return;
    }
    s_base = ts;
    s_anchor_ms = millis();

    // Persist immediately so reboots don't lose the time
    storage_write_time_cache(s_base, s_anchor_ms);

    LOG_INFO("TX", "RTC seeded + cached: ts=%lu", ts);
}

unsigned long transaction_get_ts(){
    if (s_base == 0) {
        // Clock not seeded — return 0 so callers can detect invalid time.
        return 0;
    } else {
        // Signed math prevents millis() rollover underflow
        long diff = (long)millis() - (long)s_anchor_ms;
        if (diff < 0) diff = 0;
        return s_base + ((unsigned long)diff / 1000UL);
    }
}

bool transaction_time_synced(){
    return s_base != 0;
}

bool transaction_init() {
    unsigned long cached_epoch = 0;
    unsigned long cached_ms = 0;

    if (storage_read_time_cache(&cached_epoch, &cached_ms) == STORAGE_OK) {
        unsigned long current_ms = millis();
        LOG_INFO("TX", "Cache raw: epoch=%lu cached_ms=%lu current_ms=%lu",
                 cached_epoch, cached_ms, current_ms);

        // Signed math handles millis() rollover safely
        long ms_diff = (long)current_ms - (long)cached_ms;
        unsigned long elapsed;
        if (ms_diff > 0) {
            elapsed = (unsigned long)(ms_diff / 1000UL);
        } else {
            elapsed = 0;
        }

        unsigned long now = cached_epoch + elapsed;

        LOG_INFO("TX", "Cache calc: ms_diff=%ld elapsed=%lu now=%lu",
                 ms_diff, elapsed, now);

        if (now > MAX_REASONABLE_EPOCH || now < MIN_VALID_EPOCH) {
            LOG_WARN("TX", "Cache rejected: calculated epoch %lu out of range [%lu, %lu]",
                     now, (unsigned long)MIN_VALID_EPOCH, (unsigned long)MAX_REASONABLE_EPOCH);
            LittleFS.remove(FILE_TIME_CACHE);
            return false;
        }

        s_base = now;
        s_anchor_ms = millis();
        LOG_INFO("TX", "Clock seeded from cache: epoch=%lu (drift=%lu sec)", now, elapsed);
        return true;
    }

    LOG_WARN("TX", "No valid time cache — waiting for network time");
    return false;
}

TransactionResult transaction_record(const char* uid, const char* drv, int fare, char loc){
    if(!uid || !drv) return TX_ERROR;

    // Refuse to record until we have real time
    if(!transaction_time_synced()){
        LOG_WARN("TX", "Reject uid=%s: not yet time-synced", uid);
        return TX_NOT_TIME_SYNCED;
    }

    unsigned long ts = transaction_get_ts();

    LOG_INFO("TX", "Record uid=%s amt=%d ts=%lu drv=%s loc=%c", uid, fare, ts, drv, loc);

    StorageResult r = storage_append_tx(uid, fare, ts, drv, loc);
    if(r == STORAGE_OK)   return TX_RECORDED;
    if(r == STORAGE_FULL) return TX_LOG_FULL;

    return TX_ERROR;
}