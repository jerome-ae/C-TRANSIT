#include "auth.h"
#include "../logger/logger.h" // <-- FIXED path

// <-- ADDED: Fallbacks to prevent compilation failure
#ifndef SYNC_TIMEOUT_SECONDS
#define SYNC_TIMEOUT_SECONDS 10800UL
#endif
#ifndef MAX_OFFLINE_TAPS_PER_UID
#define MAX_OFFLINE_TAPS_PER_UID 2
#endif

StaffAuthResult auth_check_staff_uid(const char* uid){
    if(!uid) return STAFF_AUTH_ERROR;
    
    StorageResult dr = storage_uid_in_file(FILE_DRIVERS, uid);
    if(dr == STORAGE_FOUND){ 
        LOG_INFO("AUTH","Driver UID %s found", uid); 
        return STAFF_AUTH_DRIVER_OK; 
    }
    
    StorageResult ar = storage_uid_in_file(FILE_ADMINS, uid);
    if(ar == STORAGE_FOUND){ 
        LOG_INFO("AUTH","Admin UID %s found", uid);  
        return STAFF_AUTH_ADMIN_OK; 
    }
    
    if(dr == STORAGE_ERROR || ar == STORAGE_ERROR) return STAFF_AUTH_ERROR;
    return STAFF_AUTH_NOT_STAFF;
}

StaffAuthResult auth_verify_pin(const char* file, const char* uid, const char* pin){
    if(!file || !uid || !pin) return STAFF_AUTH_ERROR;
    
    char stored[8] = {0};
    if(storage_get_pin_for_uid(file, uid, stored, sizeof(stored)) != STORAGE_FOUND) {
        return STAFF_AUTH_ERROR;
    }
        
    if(strcmp(pin, stored) == 0){
        LOG_INFO("AUTH","PIN OK for %s", uid);
        return (strcmp(file, FILE_DRIVERS) == 0) ? STAFF_AUTH_DRIVER_OK : STAFF_AUTH_ADMIN_OK;
    }
    
    LOG_WARN("AUTH","PIN mismatch for %s", uid);
    return STAFF_AUTH_WRONG_PIN;
}

StudentValidResult auth_validate_tap(const char* uid, unsigned long now, int* out_cnt){
    if(!uid) return STUDENT_VALIDATION_ERROR;
    if(out_cnt) *out_cnt = 0;

    // Step 1 — 3-hour kill switch
    unsigned long last = storage_read_sync_ts();
    if(last > 0){
        unsigned long off = (now > last) ? (now - last) : 0;
        if(off > SYNC_TIMEOUT_SECONDS){
            LOG_WARN("AUTH","3-hr lockdown! offline=%lus", off);
            return STUDENT_SYNC_REQUIRED;
        }
    }

    // Step 2 — Whitelist (identity)
    StorageResult wl = storage_uid_in_file(FILE_WHITELIST, uid);
    if(wl == STORAGE_ERROR)     return STUDENT_VALIDATION_ERROR;
    if(wl == STORAGE_NOT_FOUND){ 
        LOG_INFO("AUTH","REJECT: not in WL %s", uid); 
        return STUDENT_INVALID_CARD; 
    }

    // Step 3 — Blacklist (financial)
    StorageResult bl = storage_uid_in_file(FILE_BLACKLIST, uid);
    if(bl == STORAGE_ERROR) return STUDENT_VALIDATION_ERROR;
    if(bl == STORAGE_FOUND){ 
        LOG_INFO("AUTH","REJECT: BL %s", uid); 
        return STUDENT_INSUFFICIENT; 
    }

    // Step 4 — Offline lookback
    int cnt = storage_count_uid_in_tx(uid);
    if(out_cnt) *out_cnt = cnt;
    if(cnt < 0)  return STUDENT_VALIDATION_ERROR;
    
    if(cnt >= MAX_OFFLINE_TAPS_PER_UID){
        LOG_INFO("AUTH","REJECT: Limit reached %s (%d taps)", uid, cnt);
        return STUDENT_LIMIT_REACHED;
    }

    // Step 5 — Hard drive space check
    int total_tx = storage_get_tx_line_count();
    if(total_tx >= TX_LOG_MAX_LINES){
        LOG_WARN("AUTH","REJECT: TX Log Full!");
        return STUDENT_TX_FULL;
    }

    LOG_INFO("AUTH","APPROVED: %s (tap %d/%d)", uid, cnt+1, MAX_OFFLINE_TAPS_PER_UID);
    return STUDENT_APPROVED;
}