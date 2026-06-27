// =============================================================================
// sync.cpp  —  Core 1: Dual-network MQTT sync (WiFi first, GSM fallback)
//              + OTA firmware update support
// =============================================================================
#include "sync.h"
#include "../storage/storage.h"
#include "../transaction/transaction.h"
#include "../logger/logger.h"
#include <LittleFS.h>
#include <HTTPUpdate.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

// ── Exported timestamps (read by Core 0 LCD animation) ───────────────────────
volatile uint32_t g_last_upload_ms   = 0;
volatile uint32_t g_last_download_ms = 0;

// ── Module state ──────────────────────────────────────────────────────────────
static TaskHandle_t  s_task_handle = nullptr;
static volatile bool s_running     = false;

// ── Network Clients ───────────────────────────────────────────────────────────
static WiFiClientSecure s_wifi_client;
static PubSubClient     s_mqtt(s_wifi_client);

// ── Fixed buffers (no String class) ───────────────────────────────────────────
static char s_at_resp[256];
static char s_dl_buf[512];
static char s_payload[MQTT_PAYLOAD_BUF];

// =============================================================================
//  TLS HEAP GUARD (Lever 2)
// =============================================================================
static bool _heap_ok() {
    size_t free_heap = ESP.getFreeHeap();
    if (free_heap < TLS_MIN_HEAP_BYTES) {
        LOG_ERROR("SYNC", "TLS blocked: Free heap %zu < %lu", free_heap, TLS_MIN_HEAP_BYTES);
        return false;
    }
    return true;
}

// =============================================================================
//  NETWORK MODE
// =============================================================================
NetMode sync_get_net_mode() {
    if (!LittleFS.exists(FILE_NET_MODE)) return NET_MODE_AUTO;

    File f = LittleFS.open(FILE_NET_MODE, "r");
    if (!f) return NET_MODE_AUTO;
    
    char buf[4] = {0};
    int  len    = 0;
    while (f.available() && len < 3) { buf[len++] = (char)f.read(); }
    buf[len] = '\0';
    f.close();
    
    int val = atoi(buf);
    if (val == NET_MODE_WIFI) return NET_MODE_WIFI;
    if (val == NET_MODE_GSM)  return NET_MODE_GSM;
    
    return NET_MODE_AUTO;
}
 
void sync_set_net_mode(NetMode mode) {
    File f = LittleFS.open(FILE_NET_MODE, "w");
    if (!f) { LOG_ERROR("SYNC", "Cannot write netmode.dat"); return; }
    f.printf("%d\n", (int)mode);
    f.close();
    const char* names[] = {"AUTO", "WIFI-ONLY", "GSM-ONLY"};
    LOG_INFO("SYNC", "Network mode set to: %s", names[(int)mode]);
}

// =============================================================================
//  WIFI & SECURE MQTT IMPLEMENTATION
// =============================================================================
static void _mqtt_callback(char* topic, byte* payload, unsigned int length) {
    g_last_download_ms = millis();
    
    // Copy to our fixed buffer to guarantee null termination without String allocation
    size_t copy_len = (length < sizeof(s_dl_buf) - 1) ? length : sizeof(s_dl_buf) - 1;
    memcpy(s_dl_buf, payload, copy_len);
    s_dl_buf[copy_len] = '\0';
    
    LOG_INFO("WIFI", "Downlink Rx: %s", s_dl_buf);
    sync_process_downlink(s_dl_buf, copy_len);
}

static bool _wifi_connect() {
    if (WiFi.status() == WL_CONNECTED) return true;
    
    LOG_INFO("WIFI", "Connecting to %s...", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        LOG_INFO("WIFI", "Connected. IP: %s", WiFi.localIP().toString().c_str());
        return true;
    }
    LOG_WARN("WIFI", "Connection timeout");
    return false;
}

static bool _mqtt_connect_wifi() {
    if (s_mqtt.connected()) return true;
    if (!_wifi_connect()) return false;
    if (!_heap_ok()) return false; // Lever 2 Guard
    
    // Lever 1: Skip cert verification (To be secured by Phase 8 HMAC)
    s_wifi_client.setInsecure(); 
    
    s_mqtt.setServer(MQTT_HOST, MQTT_PORT);
    s_mqtt.setCallback(_mqtt_callback);
    s_mqtt.setBufferSize(MQTT_PAYLOAD_BUF);
    
    LOG_INFO("WIFI", "TLS Handshake to %s:%d...", MQTT_HOST, MQTT_PORT);
    
    if (s_mqtt.connect(MQTT_CLIENT_ID, MQTT_BROKER_USER, MQTT_BROKER_PASS, 
                       MQTT_TOPIC_STATUS, MQTT_QOS, true, MQTT_LWT_OFFLINE)) {
        LOG_INFO("WIFI", "MQTT Connected");
        s_mqtt.publish(MQTT_TOPIC_STATUS, MQTT_LWT_ONLINE, true);
        s_mqtt.subscribe(MQTT_TOPIC_RX, MQTT_QOS);
        return true;
    }
    
    LOG_ERROR("WIFI", "MQTT Connect Failed, rc=%d", s_mqtt.state());
    return false;
} 

static void _flush_tx_wifi() {
    size_t bytes_read = 0;
    int lines = storage_stream_tx_chunk(s_payload, sizeof(s_payload), &bytes_read);
    
    if (lines == 0) return;
    
    LOG_INFO("WIFI", "Flushing %d lines", lines);
    if (s_mqtt.publish(MQTT_TOPIC_TX, s_payload, false)) {
        g_last_upload_ms = millis();
        storage_atomic_delete_sent(bytes_read);
        storage_write_sync_ts(transaction_get_ts());
    } else {
        LOG_ERROR("WIFI", "Publish buffer overflow");
    }
}

// =============================================================================
//  LOW-LEVEL AT COMMAND ENGINE (GSM)
// =============================================================================
static void _gsm_flush_rx() { while (Serial2.available()) Serial2.read(); }

static bool _at_send(const char* cmd, const char* expect, uint32_t timeoutMs) {
    _gsm_flush_rx();
    Serial2.println(cmd);
    uint32_t start = millis();
    int      pos   = 0;
    memset(s_at_resp, 0, sizeof(s_at_resp));
    while ((millis() - start) < timeoutMs) {
        while (Serial2.available() && pos < (int)sizeof(s_at_resp) - 1) {
            s_at_resp[pos++] = (char)Serial2.read();
            s_at_resp[pos]   = '\0';
        }
        if (strstr(s_at_resp, expect))  { return true;  }
        if (strstr(s_at_resp, "ERROR")) { return false; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

static void _gsm_write(const uint8_t* buf, size_t len) { Serial2.write(buf, len); }

static int _gsm_read(uint8_t* buf, size_t maxLen, uint32_t timeoutMs) {
    uint32_t start = millis(); int pos = 0;
    while ((millis() - start) < timeoutMs && pos < (int)maxLen) {
        if (Serial2.available()) buf[pos++] = (uint8_t)Serial2.read();
        else vTaskDelay(pdMS_TO_TICKS(10));
    }
    return pos;
}

// =============================================================================
//  SIM800L LIFECYCLE
// =============================================================================
static bool _gsm_wake() {
    if (!_at_send("AT+CFUN=1", "OK", GSM_AT_TIMEOUT_MS)) return false;
    uint32_t start = millis();
    while ((millis() - start) < GSM_REG_TIMEOUT_MS) {
        _gsm_flush_rx();
        Serial2.println("AT+CREG?");
        vTaskDelay(pdMS_TO_TICKS(1000));
        int pos = 0; memset(s_at_resp, 0, sizeof(s_at_resp));
        uint32_t t = millis();
        while ((millis() - t) < 2000 && pos < (int)sizeof(s_at_resp) - 1)
            if (Serial2.available()) s_at_resp[pos++] = (char)Serial2.read();
        s_at_resp[pos] = '\0';
        if (strstr(s_at_resp, "+CREG: 1") || strstr(s_at_resp, "+CREG: 5") ||
            strstr(s_at_resp, "+CREG:1")  || strstr(s_at_resp, "+CREG:5"))
            return true;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    return false;
}

static void _gsm_sleep() {
    _at_send("AT+CIPSHUT", "SHUT OK", 3000);
    _at_send("AT+CFUN=0",  "OK",      3000);
    LOG_INFO("GSM", "Modem sleeping");
}

static bool _gsm_open_gprs() {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+CSTT=\"%s\"", GSM_APN);
    if (!_at_send(cmd, "OK", GSM_AT_TIMEOUT_MS)) return false;
    if (!_at_send("AT+CIICR", "OK", 10000))       return false;
    if (!_at_send("AT+CIFSR", ".",  GSM_AT_TIMEOUT_MS)) return false;
    return true;
}

static bool _gsm_tcp_connect() {
    if (!_at_send("AT+CIPMUX=0", "OK", GSM_AT_TIMEOUT_MS)) return false;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%d", MQTT_HOST, 1883); 
    return _at_send(cmd, "CONNECT", GSM_TCP_TIMEOUT_MS);
}

// =============================================================================
//  RAW MQTT PACKET BUILDER (GSM)
// =============================================================================
static void _mw_u16(uint8_t* b, int* p, uint16_t v) { b[(*p)++]=(uint8_t)(v>>8); b[(*p)++]=(uint8_t)(v&0xFF); }
static void _mw_str(uint8_t* b, int* p, const char* s) { uint16_t l=(uint16_t)strlen(s); _mw_u16(b,p,l); memcpy(b+*p,s,l); *p+=l; }
static int _mw_rem(uint8_t* b, int r) { int p=0; do { uint8_t e=r%128; r/=128; if(r>0)e|=0x80; b[p++]=e; } while(r>0); return p; }
static bool _gsm_cipsend(const uint8_t* data, size_t len) {
    char cmd[32]; snprintf(cmd,sizeof(cmd),"AT+CIPSEND=%zu",len);
    if (!_at_send(cmd,">",GSM_AT_TIMEOUT_MS)) return false;
    _gsm_write(data,len);
    return _at_send("","SEND OK",GSM_AT_TIMEOUT_MS*2);
}
static bool _wait_bytes(const uint8_t* needle, size_t nlen, uint32_t tms) {
    uint8_t win[16]={0}; size_t wp=0; uint32_t start=millis();
    while ((millis()-start)<tms) {
        if (Serial2.available()) {
            win[wp%sizeof(win)]=(uint8_t)Serial2.read(); wp++;
            if (wp>=nlen) {
                bool m=true;
                for (size_t i=0;i<nlen;i++) if (win[(wp-nlen+i)%sizeof(win)]!=needle[i]){m=false;break;}
                if (m) return true;
            }
        } else vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

static bool _mqtt_connect_packet() {
    static uint8_t pkt[256]; int pos=0; uint8_t var[64]; int vpos=0;
    _mw_str(var,&vpos,"MQTT"); var[vpos++]=0x04; var[vpos++]=0xC4;
    _mw_u16(var,&vpos,MQTT_KEEPALIVE_S);
    uint8_t pay[200]; int ppos=0;
    _mw_str(pay,&ppos,MQTT_CLIENT_ID);
    _mw_str(pay,&ppos,MQTT_TOPIC_STATUS); _mw_str(pay,&ppos,MQTT_LWT_OFFLINE);
    _mw_str(pay,&ppos,MQTT_BROKER_USER); _mw_str(pay,&ppos,MQTT_BROKER_PASS);
    int rem=vpos+ppos; pkt[pos++]=0x10; pos+=_mw_rem(pkt+pos,rem);
    memcpy(pkt+pos,var,vpos); pos+=vpos; memcpy(pkt+pos,pay,ppos); pos+=ppos;
    if (!_gsm_cipsend(pkt,pos)) return false;
    const uint8_t ca[]={0x20,0x02,0x00,0x00};
    return _wait_bytes(ca,4,GSM_AT_TIMEOUT_MS);
}

static bool _mqtt_publish_gsm(const char* topic, const uint8_t* payload, size_t paylen, uint16_t pid, bool retain) {
    static uint8_t pkt[MQTT_PAYLOAD_BUF+64]; int pos=0; uint8_t flags=0x30;
    if (retain) flags|=0x01;
    if (MQTT_QOS > 0) flags|=(MQTT_QOS<<1);
    uint8_t var[64]; int vpos=0; _mw_str(var,&vpos,topic);
    if (MQTT_QOS>0) _mw_u16(var,&vpos,pid);
    int rem=vpos+(int)paylen; pkt[pos++]=flags; pos+=_mw_rem(pkt+pos,rem);
    memcpy(pkt+pos,var,vpos); pos+=vpos; memcpy(pkt+pos,payload,paylen); pos+=(int)paylen;
    if (!_gsm_cipsend(pkt,pos)) return false;
    if (MQTT_QOS==1) {
        const uint8_t ph[]={0x40,0x02};
        if (!_wait_bytes(ph,2,PUBACK_TIMEOUT_MS)) return false;
        uint8_t ib[2]; _gsm_read(ib,2,500);
    }
    return true;
}

static void _flush_tx_gsm() {
    size_t bytes_read=0;
    int lines=storage_stream_tx_chunk(s_payload,sizeof(s_payload),&bytes_read);
    if (lines==0) return;
    static uint16_t s_pid=1;
    if (_mqtt_publish_gsm(MQTT_TOPIC_TX,(const uint8_t*)s_payload, strlen(s_payload),s_pid++,false)) {
        g_last_upload_ms=millis();
        storage_atomic_delete_sent(bytes_read);
        storage_write_sync_ts(transaction_get_ts());
    }
}

// =============================================================================
//  OTA & DOWNLINK PARSER
// =============================================================================
static void _handle_ota(const char* url) {
    LOG_INFO("OTA", "Starting OTA from: %s", url);
    if (WiFi.status() != WL_CONNECTED) { LOG_ERROR("OTA", "Requires WiFi"); return; }
    WiFiClient ota_client;
    t_httpUpdate_return ret = httpUpdate.update(ota_client, url);
    if (ret == HTTP_UPDATE_OK) ESP.restart();
}

void sync_process_downlink(const char* pay, unsigned int len) {
    if (!pay || !len) return;

    // ── Dynamic Fare Update: SYS:FARE,-250 ──────────────────────────────────
    if (strncmp(pay, "SYS:FARE,", 9) == 0) {
        int new_fare = atoi(pay + 9);
        storage_write_fare(new_fare);
        return;
    }

    if (strncmp(pay, "SYS:OTA,", 8) == 0) {
        char url[256]; strncpy(url, pay + 8, sizeof(url) - 1); url[sizeof(url) - 1] = '\0';
        _handle_ota(url); return;
    }
    
    if (strncmp(pay, "SYS:NET,", 8) == 0) {
        int mode = atoi(pay + 8);
        if (mode >= 0 && mode <= 2) sync_set_net_mode((NetMode)mode);
        return;
    }
    
    // ── THE FIX IS HERE ──────────────────────────────────────────────────────
    if (strncmp(pay, "SYS:SYNC_COMPLETE", 17) == 0) {
        LOG_INFO("SYNC", "Backend confirmed sync. Lifting lockdown.");
        storage_write_sync_ts(transaction_get_ts()); 
        return;
    }
    // ─────────────────────────────────────────────────────────────────────────

    if (strncmp(pay, "SYS:", 4) == 0) {
        char tmp[512]; strncpy(tmp, pay, sizeof(tmp) - 1); tmp[sizeof(tmp)-1]='\0';
        char* colon = strchr(tmp + 4, ':'); if (!colon) return;
        char list[3]={0}; strncpy(list, tmp+4, 2);
        const char* uids = colon + 1; const char* fp = nullptr;
        if      (strcmp(list,"WL")==0) fp=FILE_WHITELIST;
        else if (strcmp(list,"BL")==0) fp=FILE_BLACKLIST;
        else if (strcmp(list,"DR")==0) fp=FILE_DRIVERS;
        else if (strcmp(list,"AD")==0) fp=FILE_ADMINS;
        if (fp) storage_ingest_chunk(fp, uids);
        return;
    }

    char buf[512]; strncpy(buf, pay, sizeof(buf)-1); buf[sizeof(buf)-1]='\0';
    char* cmd = strtok(buf, "|");
    while (cmd) {
        char act[4]={0}, lst[3]={0}, uid[9]={0};
        if (sscanf(cmd, "%3[^:]:%2[^,],%8s", act, lst, uid) >= 3) {
            const char* fp=nullptr;
            if      (strcmp(lst,"WL")==0) fp=FILE_WHITELIST;
            else if (strcmp(lst,"BL")==0) fp=FILE_BLACKLIST;
            else if (strcmp(lst,"DR")==0) fp=FILE_DRIVERS;
            else if (strcmp(lst,"AD")==0) fp=FILE_ADMINS;
            if (fp) {
                if      (strcmp(act,"ADD")==0) storage_append_uid(fp,uid);
                else if (strcmp(act,"REM")==0) storage_remove_uid(fp,uid);
            }
        }
        cmd=strtok(nullptr,"|");
    }
}

// =============================================================================
//  CORE 1 TASK — PERSISTENT TLS LOOP
// =============================================================================
void sync_init() {
    Serial2.begin(GSM_BAUD, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
    vTaskDelay(pdMS_TO_TICKS(1000));
    _at_send("ATE0","OK",2000);
    WiFi.mode(WIFI_STA);
}

void sync_set_task_handle(TaskHandle_t h) { s_task_handle=h; }
bool sync_is_running()                    { return s_running;  }
void sync_trigger_now() {
    if (s_task_handle) xTaskNotify(s_task_handle,1,eSetValueWithOverwrite);
}

void sync_task(void* params) {
    (void)params;
    sync_init();
    
    uint32_t last_sync = 0;
    
    // ── THE STATE MACHINE COUNTERS ──
    enum ActiveNet { USE_WIFI, USE_GSM };
    ActiveNet current_net = USE_WIFI;
    
    uint8_t wifi_fails = 0;
    uint8_t gsm_fails = 0;
    
    while (true) {
        uint32_t notif = 0;
        bool triggered = xTaskNotifyWait(0, ULONG_MAX, &notif, pdMS_TO_TICKS(50)) == pdTRUE;
        bool time_to_sync = (millis() - last_sync) > SYNC_INTERVAL_MS;
        
        NetMode mode = sync_get_net_mode();
        
        // Override the state machine if the user forced a specific mode via MQTT
        if (mode == NET_MODE_WIFI) current_net = USE_WIFI;
        if (mode == NET_MODE_GSM)  current_net = USE_GSM;

        // =========================================================
        //  STATE 1: TRYING WI-FI
        // =========================================================
        if (current_net == USE_WIFI) {
            // Only spam Wi-Fi if we actually have data to send, or if it's the 60s check
            if (triggered || time_to_sync || wifi_fails == 0) { 
                if (_mqtt_connect_wifi()) {
                    wifi_fails = 0; // Success! Reset the counter.
                    s_mqtt.loop();
                    if (triggered || time_to_sync) {
                        s_running = true;
                        _flush_tx_wifi();
                        last_sync = millis();
                        s_running = false;
                    }
                } else {
                    // Failed. Increment counter.
                    wifi_fails++;
                    LOG_WARN("SYNC", "Wi-Fi fail %d/6", wifi_fails);
                    
                    if (wifi_fails >= 6 && mode == NET_MODE_AUTO) {
                        LOG_ERROR("SYNC", "Wi-Fi hit 6 failures. Flipping to GSM!");
                        current_net = USE_GSM;
                        gsm_fails = 0; // Reset GSM counter for a fresh start
                    }
                }
            }
        } 
        
        // =========================================================
        //  STATE 2: TRYING GSM
        // =========================================================
        else if (current_net == USE_GSM) {
            // GSM uses too much battery to stay connected. Only wake up if we MUST send data.
            if (triggered || time_to_sync) {
                LOG_WARN("SYNC", "Waking up SIM800L Module...");
                s_running = true;
                
                if (_gsm_wake() && _gsm_open_gprs() && _gsm_tcp_connect() && _mqtt_connect_packet()) {
                    gsm_fails = 0; // Success! Reset the counter.
                    _mqtt_publish_gsm(MQTT_TOPIC_STATUS, (const uint8_t*)MQTT_LWT_ONLINE, strlen(MQTT_LWT_ONLINE), 0, true);
                    _flush_tx_gsm();
                } else {
                    // Failed. Increment counter.
                    gsm_fails++;
                    LOG_ERROR("SYNC", "GSM fail %d/3", gsm_fails);
                    
                    if (gsm_fails >= 3 && mode == NET_MODE_AUTO) {
                        LOG_ERROR("SYNC", "GSM hit 3 failures. Flipping back to Wi-Fi!");
                        current_net = USE_WIFI;
                        wifi_fails = 0; // Reset Wi-Fi counter for a fresh start
                    }
                }
                
                _gsm_sleep(); // Always send GSM back to sleep
                last_sync = millis();
                s_running = false;
            }
        }
    }
}