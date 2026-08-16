#pragma once
#include <Arduino.h>
#include "../../include/config.h"
#include "../storage/storage.h"

typedef enum { 
    TX_RECORDED         = 0, 
    TX_LOG_FULL         = 1, 
    TX_NOT_TIME_SYNCED  = 2,
    TX_ERROR            = -1 
} TransactionResult;

void              transaction_set_rtc(unsigned long unix_ts);
unsigned long     transaction_get_ts();
bool              transaction_time_synced();
bool              transaction_init();   // ← Restore time from cache at boot
TransactionResult transaction_record(const char* uid, const char* drv_uid, int fare, char loc);