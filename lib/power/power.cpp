#include "power.h"
#include "logger.h"
#include <esp_task_wdt.h>
#include <esp_idf_version.h>
#include <rom/rtc.h>

// 15 seconds is a safe buffer. If the code freezes for 15s, reboot.
#define WDT_TIMEOUT_SECONDS 15 

static bool s_wdt_reset_occurred = false;

void power_init() {
    // ── 1. Log reset reason from the previous boot ────────────────────────────
    RESET_REASON r0 = rtc_get_reset_reason(0);
    RESET_REASON r1 = rtc_get_reset_reason(1);
    
    LOG_INFO("PWR", "Boot reset reasons — Core0: %d  Core1: %d", (int)r0, (int)r1);

    if(r0 == TG0WDT_SYS_RESET || r0 == TG1WDT_SYS_RESET || r0 == RTCWDT_SYS_RESET) {
        s_wdt_reset_occurred = true;
        LOG_ERROR("PWR", "CRITICAL: System recovered from a Watchdog Timeout crash!");
    }

    // ── 2. Arm the Task Watchdog Timer ─────────────────────────────────────────
    LOG_INFO("PWR", "Initializing Task Watchdog (%d seconds)", WDT_TIMEOUT_SECONDS);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    // ESP-IDF 5.x API (espressif32 >= 6.0.0)
    esp_task_wdt_config_t cfg = {
        .timeout_ms     = WDT_TIMEOUT_SECONDS * 1000,
        .idle_core_mask = 0,      // Do NOT watch idle tasks (they legitimately sleep)
        .trigger_panic  = true    // Emit full backtrace + reboot on expiry
    };
    
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if(err != ESP_OK) {
        LOG_WARN("PWR", "WDT reconfigure failed, attempting init...");
        esp_task_wdt_init(&cfg);
    }
#else
    // ESP-IDF 4.x API (Legacy)
    esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);
#endif

    // Register the current task (setup() / loop() task) so it feeds the WDT
    esp_task_wdt_add(NULL);

    LOG_INFO("PWR", "Watchdog armed and active.");
}

void power_feed_watchdog() {
    esp_task_wdt_reset();
}

bool power_wdt_reset_detected() {
    return s_wdt_reset_occurred;
}