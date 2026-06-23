#pragma once
#include <Arduino.h>
#include "../../include/config.h"
#include "../storage/storage.h"

typedef enum { 
    TX_RECORDED = 0, 
    TX_LOG_FULL = 1, 
    TX_ERROR = -1 
} TransactionResult;

void              transaction_set_rtc(unsigned long unix_ts);
unsigned long     transaction_get_ts();
TransactionResult transaction_record(const char* uid, const char* drv_uid);
