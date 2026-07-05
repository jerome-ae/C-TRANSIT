#include "statemachine.h"
#include "../storage/storage.h"
#include "../display/display.h"
#include "../ui/ui.h"
#include "../sync/sync.h"
#include "../logger/logger.h"

// Volatile keyword ensures cross-core memory safety
static volatile TerminalState s_state  = STATE_BOOT;
static volatile char          s_drv[9] = {0};

const char* sm_state_name(TerminalState s) {
    switch(s) {
        case STATE_BOOT:           return "BOOT";
        case STATE_OFFLINE_LOCKED: return "OFFLINE_LOCKED";
        case STATE_DRIVER_LOGIN:   return "DRIVER_LOGIN";
        case STATE_READY:          return "READY";
        case STATE_PROCESSING:     return "PROCESSING";
        case STATE_APPROVED:       return "APPROVED";
        case STATE_DENIED:         return "DENIED";
        case STATE_HARD_LOCKDOWN:  return "HARD_LOCKDOWN";
        case STATE_COLD_SYNC:      return "COLD_SYNC";
        case STATE_REGISTER_MODE:  return "REGISTER_MODE";
        case STATE_ERROR:          return "ERROR";
        default:                   return "UNKNOWN";
    }
}

TerminalState sm_get_state()         { return s_state; }
const char* sm_get_driver_uid()    { return (const char*)s_drv; }

void sm_set_driver_uid(const char* u) {
    if (u) {
        strncpy((char*)s_drv, u, sizeof(s_drv) - 1);
        s_drv[sizeof(s_drv) - 1] = '\0';
    } else {
        memset((void*)s_drv, 0, sizeof(s_drv));
    }
}

// =============================================================================
//  sm_init  —  Boot recovery
// =============================================================================
void sm_init() {
    SessionData sess;
    StorageResult r = storage_read_session(&sess);
    if (r == STORAGE_OK && sess.active == 1
        && strlen(sess.driver_uid) > 0
        && strcmp(sess.driver_uid, "NONE") != 0) {
        sm_set_driver_uid(sess.driver_uid);
        s_state = STATE_READY;
        storage_write_session(1, sess.driver_uid);
        display_show_idle();
        LOG_INFO("SM", "Session recovered → READY  drv=%s", sess.driver_uid);
    } else {
        s_state = STATE_OFFLINE_LOCKED;
        display_show_2line(" STAFF LOGIN  ", " Tap Staff Card");
        LOG_INFO("SM", "No session → OFFLINE_LOCKED");
    }
    
    // Yield briefly to let the RTOS process the init IO
    vTaskDelay(pdMS_TO_TICKS(10));
}

// =============================================================================
//  sm_driver_logout  —  Called when driver presses '#' in READY state
// =============================================================================
void sm_driver_logout() {
    LOG_INFO("SM", "Driver %s logging out", (const char*)s_drv);

    // Flush any pending taps before locking
    sync_trigger_now();

    // Clear session from LittleFS — prevents WDT recovery resuming a closed shift
    storage_write_session(0, "NONE");

    sm_transition(STATE_OFFLINE_LOCKED);
}

// =============================================================================
//  sm_cycle_net_mode  —  Called when driver presses 'A' in READY state
//  Cycles: AUTO → WIFI ONLY → GSM ONLY → AUTO
// =============================================================================
void sm_cycle_net_mode() {
    NetMode current = sync_get_net_mode();
    NetMode next;
    const char* label;

    if      (current == NET_MODE_AUTO) { next = NET_MODE_WIFI; label = "WIFI ONLY";  }
    else if (current == NET_MODE_WIFI) { next = NET_MODE_GSM;  label = "GSM ONLY";   }
    else                               { next = NET_MODE_AUTO; label = "AUTO (W+G)"; }

    sync_set_net_mode(next);
    LOG_INFO("SM", "Net mode → %s", label);

    // Show briefly on LCD then return to idle
    // NOTE: This intentionally blocks RFID reads on Core 0 for 1.5 seconds. 
    // This is safe because it is a manual admin action, not a student transaction path.
    display_show_2line("NET MODE:", label);
    ui_feedback_approved();
    vTaskDelay(pdMS_TO_TICKS(1500));
    display_show_idle();
}

// =============================================================================
//  sm_transition  —  State transitions
// =============================================================================
void sm_transition(TerminalState ns) {
    if (ns == s_state) return;
    LOG_INFO("SM", "%s → %s", sm_state_name(s_state), sm_state_name(ns));

    switch (ns) {
        case STATE_OFFLINE_LOCKED:
            memset((void*)s_drv, 0, sizeof(s_drv));
            storage_write_session(0, "NONE");
            ui_all_off();
            display_show_2line(" STAFF LOGIN  ", " Tap Staff Card");
            break;

        case STATE_DRIVER_LOGIN:
            display_show_pin_prompt(0);
            break;

        case STATE_READY:
            storage_write_session(1, (const char*)s_drv);
            display_show_idle();
            break;

        case STATE_PROCESSING:
            display_show_status("  Processing..");
            break;

        case STATE_APPROVED:
            display_show_status("   APPROVED   ");
            break;

        case STATE_DENIED:
            display_show_status("    DENIED    ");
            break;

        case STATE_HARD_LOCKDOWN:
            display_show_lockdown();
            ui_feedback_lockdown();
            sync_trigger_now();
            LOG_WARN("SM", "Hard Lockdown — sync triggered");
            break;

        case STATE_COLD_SYNC:
            display_show_cold_start();
            sync_trigger_now();
            break;

        case STATE_REGISTER_MODE:
            display_show_2line(" REGISTER MODE", "  Tap New Card");
            break;

        case STATE_ERROR:
            display_show_2line("  SYSTEM ERROR", " Restart Device");
            ui_led_red(true);
            LOG_ERROR("SM", "FATAL error state");
            break;

        default: break;
    }

    s_state = ns;
}
