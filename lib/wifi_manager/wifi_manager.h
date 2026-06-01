#pragma once
#include <Arduino.h>

// Initialize the Wi-Fi hardware
void wifi_manager_init();

// Quick check if the ESP32 is currently connected to the router
bool wifi_manager_is_connected();

// The main loop that connects, uploads tx.log, downloads updates, and disconnects
void wifi_manager_run_sync_cycle();