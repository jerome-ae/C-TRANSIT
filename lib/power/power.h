#pragma once
#include <Arduino.h>

// Initialize power management, check reset reasons, and arm the Watchdog
void power_init();

// Pat the watchdog so it knows the main loop is still alive
void power_feed_watchdog();

// Returns true if the ESP32 was forcefully rebooted by the Watchdog previously
bool power_wdt_reset_detected();