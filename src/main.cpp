// =============================================================================
// main.cpp — C-Transit Terminal  —  Production build
// =============================================================================
// Core 0: rfid_ui_task  — RFID poll, keypad, display, LEDs, buzzer, state machine
// Core 1: sync_task     — WiFi + MQTT always-on, tx.log flush, diff sync
//
// STATE FLOW:
//  BOOT ──► OFFLINE_LOCKED ──► DRIVER_LOGIN ──► READY
//            READY tap ──► PROCESSING ──► APPROVED | DENIED ──► READY
//            READY 3h  ──► HARD_LOCKDOWN ──► (after sync) ──► READY
//            READY '#' ──► (sync flush) ──► OFFLINE_LOCKED
//            READY 'A' ──► cycle net mode ──► READY
// =============================================================================
#include <Arduino.h>
#include <Wire.h>
#include <esp_random.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "logger.h"
#include "power.h"
#include "display.h"
#include "ui.h"
#include "rfid.h"
#include "keypad_handler.h"
#include "storage.h"
#include "auth.h"
#include "transaction.h"
#include "sync.h"
#include "statemachine.h"

// ── Local Constants ───────────────────────────────────────────────────────────
static const uint32_t LCD_ANIM_FLASH_MS = 500;
static const uint32_t OTP_MIN           = 100000;
static const uint32_t OTP_MAX           = 999999;
static const uint32_t UI_REFRESH_MS     = 500;

// ── Forward declarations ──────────────────────────────────────────────────────
static void rfid_ui_task(void* p);
static bool boot_hardware();
static void handle_offline_locked_tap(const char* uid);
static void handle_ready_tap(const char* uid);
static void handle_register_tap(const char* uid);
static void handle_ready_keypad(char key);      
static void check_lockdown_release();
static void run_ui_refresh();
static void ui_delay(uint32_t ms); // Non-blocking delay for UI holds

static uint32_t s_last_ui_refresh_ms = 0;

// =============================================================================
//  setup
// =============================================================================
void setup() {
    logger_init();
    LOG_INFO("MAIN", "=== BOOT === Terminal: %s", TERMINAL_ID);

    power_init();

    if (!boot_hardware()) {
        display_show_2line("  BOOT FAILED ", " Check Wiring ");
        ui_led_red(true);
        while (true) { power_feed_watchdog(); vTaskDelay(pdMS_TO_TICKS(5000)); }
    }

    // State machine reads sess.dat — determines OFFLINE_LOCKED or READY
    sm_init();

    // Cold-start guard: if wl.dat or drv.dat missing, block until sync delivers DB
    if (storage_get_file_size(FILE_WHITELIST) == 0 ||
        storage_get_file_size(FILE_DRIVERS)   == 0) {
        LOG_WARN("MAIN", "Cold start — empty DB");
        sm_transition(STATE_COLD_SYNC);
    }

    // ── Core 1: always-on sync task ──────────────────────────────────────────
    TaskHandle_t sync_handle = nullptr;
    xTaskCreatePinnedToCore(sync_task, "sync_task",
                            STACK_SIZE_SYNC, nullptr, PRIORITY_SYNC,
                            &sync_handle, CORE_SYNC);
    if (!sync_handle) LOG_ERROR("MAIN", "sync_task creation FAILED");
    else              sync_set_task_handle(sync_handle);

    // ── Core 0: RFID + UI task ───────────────────────────────────────────────
    xTaskCreatePinnedToCore(rfid_ui_task, "rfid_ui",
                            STACK_SIZE_APP, nullptr, PRIORITY_APP, // Fixed Macro
                            nullptr, CORE_APP);

    LOG_INFO("MAIN", "Tasks launched — entering scheduler");
}

void loop() {
    power_feed_watchdog();
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// =============================================================================
//  boot_hardware
// =============================================================================
static bool boot_hardware() {
    bool ok = true;

    if (!display_init()) LOG_WARN("MAIN", "LCD init failed — continuing");
    
    // ── Apply I2C hardening immediately after display init ──
    Wire.setClock(400000);
    Wire.setTimeOut(20);

    // ── Inject the firmware version dynamically onto the boot screen ──
    char boot_msg[17];
    // Formats as " Ver: v1.0       " safely padding or truncating to fit the 16x2 LCD
    snprintf(boot_msg, sizeof(boot_msg), " Ver: %-10.10s", FIRMWARE_VERSION);
    display_show_2line("   C-TRANSIT    ", boot_msg);

    if (!ui_init()) LOG_WARN("MAIN", "UI init failed");

    if (!rfid_init()) {
        LOG_ERROR("MAIN", "RFID CRITICAL failure");
        ok = false;
    }

    if (!keypad_init()) LOG_WARN("MAIN", "Keypad init failed");

    if (!storage_init()) {
        LOG_ERROR("MAIN", "Storage CRITICAL failure");
        ok = false;
    }

    return ok;
}

// =============================================================================
//  ui_delay — Non-blocking wait that keeps the 500ms sync animations alive
// =============================================================================
static void ui_delay(uint32_t ms) {
    uint32_t start = millis();
    while ((millis() - start) < ms) {
        uint32_t now = millis();
        if ((now - s_last_ui_refresh_ms) >= UI_REFRESH_MS) {
            s_last_ui_refresh_ms = now;
            run_ui_refresh();
        }
        power_feed_watchdog();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// =============================================================================
//  run_ui_refresh  —  sync indicator animation on LCD row 0
// =============================================================================
static void run_ui_refresh() {
    uint32_t now = millis();
    bool up = (g_last_upload_ms   > 0) && ((now - g_last_upload_ms)   < LCD_ANIM_FLASH_MS);
    bool dn = (g_last_download_ms > 0) && ((now - g_last_download_ms) < LCD_ANIM_FLASH_MS);
    display_set_sync_indicators(up, dn);
}

// =============================================================================
//  rfid_ui_task — Core 0
// =============================================================================
static void rfid_ui_task(void* p) {
    (void)p;
    esp_task_wdt_add(nullptr);
    LOG_INFO("MAIN", "rfid_ui_task on Core %d", xPortGetCoreID());

    char uid[9] = {0};

    while (true) {
        power_feed_watchdog();

        // ── 500ms UI refresh ─────────────────────────────────────────────────
        uint32_t now_ms = millis();
        if ((now_ms - s_last_ui_refresh_ms) >= UI_REFRESH_MS) {
            s_last_ui_refresh_ms = now_ms;
            run_ui_refresh();

            // tx.log capacity check
            if (sm_get_state() == STATE_READY) {
                if (storage_get_tx_line_count() >= TX_LOG_MAX_LINES)
                    sm_transition(STATE_HARD_LOCKDOWN);
            }

            // Lockdown release check
            if (sm_get_state() == STATE_HARD_LOCKDOWN && !sync_is_running())
                check_lockdown_release();

            // Cold sync completion check
            if (sm_get_state() == STATE_COLD_SYNC && !sync_is_running()) {
                if (storage_get_file_size(FILE_WHITELIST) > 0 &&
                    storage_get_file_size(FILE_DRIVERS)   > 0) {
                    LOG_INFO("MAIN", "Cold sync complete → OFFLINE_LOCKED");
                    sm_transition(STATE_OFFLINE_LOCKED);
                }
            }
        }

        // ── Keypad poll (only meaningful in STATE_READY) ──────────────────────
        if (sm_get_state() == STATE_READY) {
            char key = keypad_get_key();
            if (key) handle_ready_keypad(key);
        }

        // ── RFID poll ─────────────────────────────────────────────────────────
        memset(uid, 0, sizeof(uid));
        RFIDResult rr = rfid_poll(uid);

        if (rr == RFID_NEW_CARD) {
            LOG_INFO("MAIN", "Tap in state %s  uid=%s",
                     sm_state_name(sm_get_state()), uid);

            switch (sm_get_state()) {
                case STATE_OFFLINE_LOCKED: handle_offline_locked_tap(uid); break;
                case STATE_READY:          handle_ready_tap(uid);          break;
                case STATE_REGISTER_MODE:  handle_register_tap(uid);       break;
                case STATE_HARD_LOCKDOWN:
                    display_show_lockdown();
                    ui_feedback_rejected();
                    break;
                case STATE_COLD_SYNC:
                    display_show_cold_start();
                    break;
                default:
                    // DRIVER_LOGIN, PROCESSING, etc. — ignore new taps
                    break;
            }
        }

        if (rr == RFID_READ_ERROR) LOG_WARN("MAIN", "RFID transient read error");

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// =============================================================================
//  handle_ready_keypad
// =============================================================================
static void handle_ready_keypad(char key) {
    switch (key) {
        case '#':
            LOG_INFO("MAIN", "Driver '%s' logout requested via keypad",
                     sm_get_driver_uid());
            sm_driver_logout();
            break;

        case 'A':
            sm_cycle_net_mode();
            break;

        default:
            break;
    }
}

// =============================================================================
//  handle_offline_locked_tap 
// =============================================================================
static void handle_offline_locked_tap(const char* uid) {
    StaffAuthResult uid_r = auth_check_staff_uid(uid);

    if (uid_r == STAFF_AUTH_NOT_STAFF) {
        display_show_2line("  NOT STAFF   ", " Use Staff Card");
        ui_feedback_rejected();
        ui_delay(LCD_RESULT_MS);
        display_show_2line(" STAFF LOGIN  ", " Tap Staff Card");
        return;
    }
    if (uid_r == STAFF_AUTH_ERROR) {
        display_show_2line(" FILE ERROR   ", " Restart Term.");
        ui_feedback_rejected();
        return;
    }

    sm_transition(STATE_DRIVER_LOGIN);

    char pin[5] = {0}; 
    bool got_pin = keypad_read_pin(pin, 4);

    if (!got_pin) {
        display_show_2line(" Login Cancel ", "              ");
        ui_delay(1000);
        sm_transition(STATE_OFFLINE_LOCKED);
        return;
    }

    const char* pin_file = (uid_r == STAFF_AUTH_DRIVER_OK) ? FILE_DRIVERS : FILE_ADMINS;
    StaffAuthResult pin_r = auth_verify_pin(pin_file, uid, pin);

    switch (pin_r) {
        case STAFF_AUTH_DRIVER_OK:
            sm_set_driver_uid(uid);
            sm_transition(STATE_READY);
            LOG_INFO("MAIN", "Driver %s logged in", uid);
            break;
        case STAFF_AUTH_ADMIN_OK:
            sm_set_driver_uid(uid);
            sm_transition(STATE_REGISTER_MODE);
            LOG_INFO("MAIN", "Admin %s → REGISTER_MODE", uid);
            break;
        case STAFF_AUTH_WRONG_PIN:
            display_show_2line("  WRONG PIN   ", "  Try Again   ");
            ui_feedback_rejected();
            ui_delay(LCD_RESULT_MS);
            sm_transition(STATE_OFFLINE_LOCKED);
            break;
        default:
            display_show_2line(" AUTH ERROR   ", " Restart Term.");
            ui_feedback_rejected();
            sm_transition(STATE_OFFLINE_LOCKED);
            break;
    }
}

// =============================================================================
//  handle_ready_tap 
// =============================================================================
static void handle_ready_tap(const char* uid) {
    sm_transition(STATE_PROCESSING);

    unsigned long now = transaction_get_ts();
    int tap_cnt = 0;
    StudentValidResult vr = auth_validate_tap(uid, now, &tap_cnt);

    switch (vr) {
        case STUDENT_APPROVED: {
            TransactionResult tr = transaction_record(uid, sm_get_driver_uid());
            if (tr == TX_RECORDED) {
                sm_transition(STATE_APPROVED);
                ui_feedback_approved();
                sync_trigger_now();
                ui_delay(LCD_RESULT_MS);
                sm_transition(STATE_READY);
            } else if (tr == TX_LOG_FULL) {
                sm_transition(STATE_HARD_LOCKDOWN);
            } else {
                display_show_2line(" WRITE ERROR  ", "  Try Again   ");
                ui_feedback_rejected();
                ui_delay(LCD_RESULT_MS);
                sm_transition(STATE_READY);
            }
            break;
        }
        case STUDENT_SYNC_REQUIRED:
            sm_transition(STATE_DENIED);
            display_show_2line(" SYNC REQUIRED", " Please Wait  ");
            ui_feedback_rejected();
            ui_delay(LCD_RESULT_MS);
            sm_transition(STATE_HARD_LOCKDOWN);
            break;
        case STUDENT_INVALID_CARD:
            sm_transition(STATE_DENIED);
            display_show_status(" INVALID CARD ");
            ui_feedback_rejected();
            ui_delay(LCD_RESULT_MS);
            sm_transition(STATE_READY);
            break;
        case STUDENT_INSUFFICIENT:
            sm_transition(STATE_DENIED);
            display_show_status("INSUFF. FUNDS ");
            ui_feedback_rejected();
            ui_delay(LCD_RESULT_MS);
            sm_transition(STATE_READY);
            break;
        case STUDENT_LIMIT_REACHED:
            sm_transition(STATE_DENIED);
            display_show_status(" LIMIT REACHED");
            ui_feedback_rejected();
            ui_delay(LCD_RESULT_MS);
            sm_transition(STATE_READY);
            break;
        case STUDENT_TX_FULL:
            sm_transition(STATE_HARD_LOCKDOWN);
            break;
        default:
            display_show_2line("  SYSTEM ERR  ", " Try Again... ");
            ui_feedback_rejected();
            ui_delay(LCD_RESULT_MS);
            sm_transition(STATE_READY);
            break; 
    }
}

// =============================================================================
//  handle_register_tap
// =============================================================================
static void handle_register_tap(const char* uid) {
    // PHASE 8 SECURITY NOTE: 'uid' will be hashed here before storage
    if (storage_uid_in_file(FILE_WHITELIST, uid) == STORAGE_FOUND) {
        display_show_2line("ALREADY LINKED", "  Tap New Card");
        ui_feedback_rejected();
        ui_delay(LCD_RESULT_MS);
        display_show_2line(" REGISTER MODE", "  Tap New Card");
        return;
    }

    uint32_t otp = OTP_MIN + (esp_random() % (OTP_MAX - OTP_MIN + 1));
    LOG_INFO("MAIN", "Reg: uid=%s otp=%06lu agent=%s",
             uid, (unsigned long)otp, sm_get_driver_uid());

    display_show_otp(otp);
    ui_feedback_otp();
    
    // Save the PENDING_LINK payload to the hard drive BEFORE triggering the sync!
    storage_append_registration(uid, otp, sm_get_driver_uid());
    
    // Now that the file is safely saved, wake up Core 1 to upload it via MQTT
    sync_trigger_now();

    ui_delay(5000);

    display_show_2line("ADD NEXT? 1=Y ", "          0=N ");
    char key = '\0';
    uint32_t t = millis();
    while ((millis() - t) < 30000) {
        key = keypad_get_key();
        if (key == '1' || key == '0') break;
        
        // Keep UI animations alive during wait
        uint32_t now = millis();
        if ((now - s_last_ui_refresh_ms) >= UI_REFRESH_MS) {
            s_last_ui_refresh_ms = now;
            run_ui_refresh();
        }
        
        power_feed_watchdog();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (key == '1') display_show_2line(" REGISTER MODE", "  Tap New Card");
    else            sm_transition(STATE_OFFLINE_LOCKED);
}

// =============================================================================
//  check_lockdown_release  
// =============================================================================
static void check_lockdown_release() {
    unsigned long sync_ts = storage_read_sync_ts();
    unsigned long now     = transaction_get_ts();
    if (sync_ts == 0) return;
    unsigned long off = (now > sync_ts) ? (now - sync_ts) : 0;
    if (off < SYNC_TIMEOUT_SECONDS) {
        LOG_INFO("MAIN", "Lockdown released — last sync %lus ago", off);
        sm_transition(STATE_READY);
    }
}
