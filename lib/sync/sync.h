#pragma once
// =============================================================================
// sync.h  —  Core 1 dual-network MQTT sync + OTA + net mode
// =============================================================================
#include <Arduino.h>
#include "../../include/config.h"

extern volatile uint32_t g_last_upload_ms;
extern volatile uint32_t g_last_download_ms;

// Core 1 task entry & control
void sync_init();
void sync_task(void* params);
void sync_trigger_now();
void sync_set_task_handle(TaskHandle_t h);
bool sync_is_running();

// Downlink parser — called by both WiFi and GSM paths
void sync_process_downlink(const char* payload, unsigned int len);

// Network mode — readable/writable by Core 0 (keypad) and Core 1
NetMode sync_get_net_mode();
void    sync_set_net_mode(NetMode mode);
