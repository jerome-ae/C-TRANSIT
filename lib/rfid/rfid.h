#pragma once
#include <Arduino.h>
#include "../../include/config.h"

typedef enum { 
    RFID_NO_CARD = 0, 
    RFID_NEW_CARD = 1, 
    RFID_DUPLICATE = 2, 
    RFID_READ_ERROR = 3 
} RFIDResult;

bool       rfid_init();
RFIDResult rfid_poll(char* out_uid);   // out_uid must be >=9 bytes
bool       rfid_watchdog_tick();
bool       rfid_is_faulted();
void       rfid_mark_activity();
void       rfid_clear_debounce();
bool       rfid_self_test();
