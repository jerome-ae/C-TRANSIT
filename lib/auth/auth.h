#pragma once
#include <Arduino.h>
#include "../../include/config.h"
#include "../storage/storage.h"

typedef enum {
    STAFF_AUTH_DRIVER_OK = 1, 
    STAFF_AUTH_ADMIN_OK = 2,
    STAFF_AUTH_WRONG_PIN = 3, 
    STAFF_AUTH_NOT_STAFF = 4, 
    STAFF_AUTH_ERROR = -1
} StaffAuthResult;

typedef enum {
    STUDENT_APPROVED = 0,        
    STUDENT_SYNC_REQUIRED = 1,
    STUDENT_INVALID_CARD = 2,  
    STUDENT_INSUFFICIENT = 3,
    STUDENT_LIMIT_REACHED = 4, 
    STUDENT_TX_FULL = 5,
    STUDENT_VALIDATION_ERROR = -1
} StudentValidResult;

StaffAuthResult    auth_check_staff_uid(const char* uid);
StaffAuthResult    auth_verify_pin(const char* file, const char* uid, const char* pin);
StudentValidResult auth_validate_tap(const char* uid, unsigned long now, int* out_cnt);
