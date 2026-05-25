#pragma once
// =============================================================================
// sync.h  —  Always-On Core 1 MQTT sync over WiFiClientSecure (port 8883)
// =============================================================================
#include <Arduino.h>
#include "../../include/config.h"

// Exported to main.cpp for the LCD animation — MUST remain volatile
extern volatile uint32_t g_last_upload_ms;
extern volatile uint32_t g_last_download_ms;

void sync_init();
void sync_task(void* params);           // Core 1 FreeRTOS task entry
void sync_trigger_now();                // Signal Core 1 to sync immediately
void sync_set_task_handle(TaskHandle_t h);
bool sync_is_running();

// Called by MQTT callback for inbound rx messages
void sync_process_downlink(const char* payload, unsigned int len);