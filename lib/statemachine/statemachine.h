#pragma once
#include <Arduino.h>
#include "../../include/config.h"

typedef enum {
    STATE_BOOT=0,
    STATE_OFFLINE_LOCKED=1,    // waiting for driver card tap
    STATE_DRIVER_LOGIN=2,      // driver found — collecting PIN
    STATE_READY=3,             // Tap & Go — accepting student taps
    STATE_PROCESSING=4,        // card read, running validation
    STATE_APPROVED=5,          // showing approved result
    STATE_DENIED=6,            // showing denied result
    STATE_HARD_LOCKDOWN=7,     // 3-hr or tx full — blocked
    STATE_COLD_SYNC=8,         // blank terminal, awaiting DB
    STATE_REGISTER_MODE=9,     // admin provisioning cards
    STATE_ERROR=10
} TerminalState;

// ── Existing API (unchanged) ──────────────────────────────────────────────────
void          sm_init();
TerminalState sm_get_state();
void          sm_transition(TerminalState s);
const char*   sm_state_name(TerminalState s);
const char*   sm_get_driver_uid();
void          sm_set_driver_uid(const char* uid);

// ── New ───────────────────────────────────────────────────────────────────────
void sm_driver_logout();     // '#' in STATE_READY — flush sync, clear session, lock
void sm_cycle_net_mode();    // 'A' in STATE_READY — cycle AUTO → WIFI → GSM → AUTO
