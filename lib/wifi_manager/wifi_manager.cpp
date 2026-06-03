#include "wifi_manager.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include "../../include/config.h"
#include "../storage/storage.h"
#include "../transaction/transaction.h"
#include "../logger/logger.h"
#include "../sync/sync.h" // Needed to pass downlinks to your existing parser

// ── Networking Objects ───────────────────────────────────────────────────────
static WiFiClient espClient;
static PubSubClient mqttClient(espClient);

// ── Fixed Buffer for Uploads ─────────────────────────────────────────────────
static char s_wifi_payload[MQTT_PAYLOAD_BUF];

// =============================================================================
//  MQTT DOWNLINK CALLBACK (Triggered automatically when data arrives)
// =============================================================================
static void _wifi_mqtt_callback(char* topic, byte* payload, unsigned int length) {
    if (strcmp(topic, MQTT_TOPIC_RX) != 0) return;

    // PubSubClient payloads are not null-terminated, so we copy it to a safe buffer
    char dl_buf[512];
    unsigned int cpylen = (length < sizeof(dl_buf) - 1) ? length : sizeof(dl_buf) - 1;
    memcpy(dl_buf, payload, cpylen);
    dl_buf[cpylen] = '\0';

    g_last_download_ms = millis();
    LOG_INFO("WIFI", "Downlink received on %s", topic);
    
    // Pass it to your exact same parser from sync.cpp!
    sync_process_downlink(dl_buf, cpylen);
}

// =============================================================================
//  PUBLIC API
// =============================================================================

void wifi_manager_init() {
    LOG_INFO("WIFI", "Initializing Wi-Fi module...");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(); 
    
    // Setup MQTT Broker details
    mqttClient.setServer(MQTT_HOST, MQTT_PORT);
    mqttClient.setCallback(_wifi_mqtt_callback);
}

// =============================================================================
//  UPDATED: The Connection Check (Now actively attempts to connect!)
// =============================================================================

bool wifi_manager_is_connected() {
    // If already connected, great!
    if (WiFi.status() == WL_CONNECTED) return true;
    
    LOG_INFO("WIFI", "Attempting to connect to SSID: %s", WIFI_SSID);
    
    // ── THE FIX: Deep clean the Wi-Fi cache before connecting ──
    WiFi.disconnect(true, true); 
    vTaskDelay(pdMS_TO_TICKS(100)); // Brief pause for the radio to reset
    
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    // ── THE FIX: Increased timeout to 10 seconds (20 attempts) ──
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        LOG_INFO("WIFI", "Connected! IP: %s", WiFi.localIP().toString().c_str());
        return true;
    } else {
        LOG_WARN("WIFI", "Wi-Fi connection failed. Router rejected or out of range.");
        return false;
    }
}

// =============================================================================
//  WI-FI FULL SYNC CYCLE
// =============================================================================
void wifi_manager_run_sync_cycle() {
    // We already know Wi-Fi is connected because the Traffic Cop checked!

    // Step 1: Connect to MQTT Broker
    if (!mqttClient.connected()) {
        LOG_INFO("WIFI", "Connecting to MQTT Broker...");
        
        // ── FIX 1: Added ', false' at the end to disable Clean Session! ──
        if (!mqttClient.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD, 
                                MQTT_TOPIC_STATUS, 1, true, MQTT_LWT_OFFLINE, false)) {
            LOG_ERROR("WIFI", "MQTT Connection failed, rc=%d", mqttClient.state());
            return;
        }
        
        mqttClient.publish(MQTT_TOPIC_STATUS, MQTT_LWT_ONLINE, true);
        mqttClient.subscribe(MQTT_TOPIC_RX, MQTT_QOS);
        LOG_INFO("WIFI", "MQTT Connected & Subscribed to %s", MQTT_TOPIC_RX);
    }

    // Step 2: Flush tx.log to the cloud
    size_t bytes_read = 0;
    int lines = storage_stream_tx_chunk(s_wifi_payload, sizeof(s_wifi_payload), &bytes_read);
    
    if (lines > 0) {
        LOG_INFO("WIFI", "Flushing %d lines (%zu bytes) via Wi-Fi...", lines, bytes_read);
        
        if (mqttClient.publish(MQTT_TOPIC_TX, s_wifi_payload)) {
            g_last_upload_ms = millis();
            storage_atomic_delete_sent(bytes_read);
            storage_write_sync_ts(transaction_get_ts());
            LOG_INFO("WIFI", "Wi-Fi Flush complete. sync.dat updated.");
        } else {
            LOG_ERROR("WIFI", "Wi-Fi Publish failed — tx.log preserved.");
        }
    } else {
        LOG_INFO("WIFI", "tx.log is empty. Nothing to upload.");
    }

    // Step 3: Yield to let PubSubClient check for any incoming downlinks
    uint32_t start = millis();
    
    // ── FIX 2: Increased listening window from 3000ms to 10000ms! ──
    while (millis() - start < 10000) {
        mqttClient.loop(); 
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    LOG_INFO("WIFI", "Wi-Fi Sync cycle complete.");
}