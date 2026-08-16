// =============================================================================
// sync.cpp  —  Core 1: Dual-network MQTT sync (WiFi first, GSM fallback)
//              + OTA firmware update support
// =============================================================================
#include "sync.h"
#include "../storage/storage.h"
#include "../transaction/transaction.h"
#include "../logger/logger.h"
#include "../display/display.h"
#include <LittleFS.h>
#include <HTTPUpdate.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <time.h>
#include <esp_task_wdt.h>

// ── Exported timestamps (read by Core 0 LCD animation) ───────────────────────
volatile uint32_t g_last_upload_ms   = 0;
volatile uint32_t g_last_download_ms = 0;

// ── Runtime MQTT topic buffers ───────────────────────────────────────────────
extern char g_device_id[];
static char s_topic_tx[48];
static char s_topic_rx[48];
static char s_topic_status[48];

// ── Module state ──────────────────────────────────────────────────────────────
static TaskHandle_t  s_task_handle = nullptr;
static volatile bool s_running     = false;
static volatile uint32_t s_last_trigger_ms = 0;
static const uint32_t SYNC_TRIGGER_COOLDOWN_MS = 2000UL;

// ── Network Clients ───────────────────────────────────────────────────────────
static WiFiClientSecure s_wifi_client;
static PubSubClient     s_mqtt(s_wifi_client);

// ── Fixed buffers (no String class) ───────────────────────────────────────────
static char s_at_resp[256];
static char s_dl_buf[512];
static char s_payload[MQTT_PAYLOAD_BUF];

// ── Forward declarations ──────────────────────────────────────────────────────
static void _rebuild_topics_from_device_id();
static bool _mqtt_subscribe_rx();
static bool _heap_ok();
static bool _wifi_connect();
static bool _ntp_sync_time();
static bool _mqtt_connect_wifi();
static bool _commit_acknowledged_delete(size_t bytes_to_delete);
static void _flush_tx_wifi();
static void _mqtt_callback(char* topic, byte* payload, unsigned int length);
static void _wifi_got_ip_handler(WiFiEvent_t event, WiFiEventInfo_t info);

// GSM forward declarations
static void _gsm_flush_rx();
static bool _at_send(const char* cmd, const char* expect, uint32_t timeoutMs);
static void _gsm_write(const uint8_t* buf, size_t len);
static int  _gsm_read(uint8_t* buf, size_t maxLen, uint32_t timeoutMs);
static bool _gsm_wake();
static void _gsm_sleep();
static bool _gsm_open_gprs();
static bool _gsm_tcp_connect();
static bool _mqtt_connect_packet();
static bool _mqtt_publish_gsm(const char* topic, const uint8_t* payload, size_t paylen, uint16_t pid, bool retain);
static void _flush_tx_gsm();
static void _handle_ota(const char* url);

// Raw MQTT builders (GSM only now)
static void _mw_u16(uint8_t* b, int* p, uint16_t v);
static void _mw_str(uint8_t* b, int* p, const char* s);
static int  _mw_rem(uint8_t* b, int r);
static bool _wait_bytes(const uint8_t* needle, size_t nlen, uint32_t tms);

// =============================================================================
//  TOPIC BUILDER
// =============================================================================
static void _rebuild_topics_from_device_id() {
    snprintf(s_topic_tx,     sizeof(s_topic_tx),     "ctransit/%s/tx",     g_device_id);
    snprintf(s_topic_rx,     sizeof(s_topic_rx),     "ctransit/%s/rx",     g_device_id);
    snprintf(s_topic_status, sizeof(s_topic_status), "ctransit/%s/status", g_device_id);
}

// =============================================================================
//  MQTT SUBSCRIBE
// =============================================================================
static bool _mqtt_subscribe_rx() {
    if (!s_mqtt.connected()) return false;
    bool ok = s_mqtt.subscribe(s_topic_rx, MQTT_QOS);
    if (ok) {
        LOG_INFO("WIFI", "Subscribed to %s", s_topic_rx);
    } else {
        LOG_ERROR("WIFI", "Subscribe failed for %s", s_topic_rx);
    }
    return ok;
}

// =============================================================================
//  TLS HEAP GUARD
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
//  MQTT CALLBACK — FIRES ON INCOMING DOWNLINKS
// =============================================================================
static void _mqtt_callback(char* topic, byte* payload, unsigned int length) {
    g_last_download_ms = millis();
    
    size_t copy_len = (length < sizeof(s_dl_buf) - 1) ? length : sizeof(s_dl_buf) - 1;
    memcpy(s_dl_buf, payload, copy_len);
    s_dl_buf[copy_len] = '\0';
    
    LOG_INFO("WIFI", "Downlink Rx [%s]: %s", topic, s_dl_buf);
    sync_process_downlink(s_dl_buf, copy_len);
}

// =============================================================================
//  WIFI EVENT HANDLER
// =============================================================================
static void _wifi_got_ip_handler(WiFiEvent_t event, WiFiEventInfo_t info) {
    (void)info;
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        LOG_INFO("WIFI", "Got IP, triggering immediate sync");
        sync_trigger_now();
    }
}

// =============================================================================
//  WIFI CONNECT
// =============================================================================
static bool _wifi_connect() {
    if (WiFi.status() == WL_CONNECTED) return true;
    
    LOG_INFO("WIFI", "Connecting to %s...", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        IPAddress ip = WiFi.localIP();
        char ipbuf[16];
        snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        LOG_INFO("WIFI", "Connected. IP: %s", ipbuf);
        _ntp_sync_time();
        return true;
    }
    LOG_WARN("WIFI", "Connection timeout");
    return false;
}

// =============================================================================
//  NTP TIME SYNC — Always runs on WiFi connect. Updates sync.dat.
// =============================================================================
static bool _ntp_sync_time() {
    LOG_INFO("NTP", "Requesting time from %s / %s...", NTP_SERVER_1, NTP_SERVER_2);
    configTime(NTP_GMT_OFFSET_SEC, NTP_DST_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);

    struct tm timeinfo;
    uint32_t start = millis();
    bool got_time = false;
    while ((millis() - start) < NTP_SYNC_TIMEOUT_MS) {
        if (getLocalTime(&timeinfo, 250)) { got_time = true; break; }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!got_time) {
        LOG_WARN("NTP", "Sync timed out");
        return false;
    }

    time_t now;
    time(&now);
    if ((unsigned long)now < MIN_VALID_EPOCH) {
        LOG_WARN("NTP", "Got implausible epoch=%lu, discarding", (unsigned long)now);
        return false;
    }

    transaction_set_rtc((unsigned long)now);
    LOG_INFO("NTP", "Time synced via NTP: epoch=%lu", (unsigned long)now);

    storage_write_sync_ts((unsigned long)now);
    LOG_INFO("NTP", "sync.dat updated: %lu", (unsigned long)now);

    return true;
}

// =============================================================================
//  MQTT CONNECT — WIFI
// =============================================================================
static bool _mqtt_connect_wifi() {
    if (s_mqtt.connected()) return true;
    if (!_wifi_connect()) return false;
    if (!_heap_ok()) return false;
    
    s_wifi_client.setInsecure(); 
    
    s_mqtt.setServer(MQTT_HOST, MQTT_PORT);
    s_mqtt.setCallback(_mqtt_callback);
    s_mqtt.setBufferSize(MQTT_PAYLOAD_BUF);
    s_mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
    
    LOG_INFO("WIFI", "TLS Handshake to %s:%d...", MQTT_HOST, MQTT_PORT);
    
    if (s_mqtt.connect(g_device_id, MQTT_BROKER_USER, MQTT_BROKER_PASS,
                       s_topic_status, MQTT_QOS, true, MQTT_LWT_OFFLINE, false)) {
        LOG_INFO("WIFI", "MQTT Connected");
        
        bool online_ok = s_mqtt.publish(s_topic_status, MQTT_LWT_ONLINE, true);
        LOG_INFO("WIFI", "ONLINE publish %s", online_ok ? "OK" : "FAILED");
        
        s_mqtt.loop();
        _mqtt_subscribe_rx();
        
        {
            uint32_t drain_start = millis();
            while (millis() - drain_start < 500) {
                s_mqtt.loop();
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        return true;
    }
    
    LOG_ERROR("WIFI", "MQTT Connect Failed, rc=%d", s_mqtt.state());
    return false;
}

// =============================================================================
//  COMMIT DELETE AFTER PUBACK
// =============================================================================
static bool _commit_acknowledged_delete(size_t bytes_to_delete) {
    if (bytes_to_delete == 0) return true;

    StorageResult res = storage_atomic_delete_sent(bytes_to_delete);
    if (res == STORAGE_OK) {
        if (transaction_time_synced()) {
            storage_write_sync_ts(transaction_get_ts());
        } else {
            LOG_WARN("WIFI", "Sync ts skipped — clock not yet synced");
        }
        return true;
    }

    LOG_ERROR("WIFI", "Delete after PUBACK failed: %d", (int)res);
    return false;
}

// =============================================================================
//  FLUSH TX LOG — WIFI
// =============================================================================
static void _flush_tx_wifi() {
    size_t bytes_read = 0;
    int lines = storage_stream_tx_chunk(s_payload, sizeof(s_payload), &bytes_read);
    
    if (lines == 0) return;
    
    LOG_INFO("WIFI", "Flushing %d lines (%zu bytes)", lines, strlen(s_payload));
    
    if (s_mqtt.publish(s_topic_tx, s_payload, strlen(s_payload))) {
        g_last_upload_ms = millis();
        
        uint32_t ack_start = millis();
        while (millis() - ack_start < 1000) {
            s_mqtt.loop();
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        
        _commit_acknowledged_delete(bytes_read);
    } else {
        LOG_ERROR("WIFI", "Publish failed — MQTT may be disconnected");
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
        esp_task_wdt_reset();
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
//  RAW MQTT PACKET BUILDERS (GSM ONLY)
// =============================================================================
static void _mw_u16(uint8_t* b, int* p, uint16_t v) { b[(*p)++]=(uint8_t)(v>>8); b[(*p)++]=(uint8_t)(v&0xFF); }
static void _mw_str(uint8_t* b, int* p, const char* s) { uint16_t l=(uint16_t)strlen(s); _mw_u16(b,p,l); memcpy(b+*p,s,l); *p+=l; }
static int _mw_rem(uint8_t* b, int r) { int p=0; do { uint8_t e=r%128; r/=128; if(r>0)e|=0x80; b[p++]=e; } while(r>0); return p; }

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

static bool _gsm_cipsend(const uint8_t* data, size_t len) {
    char cmd[32]; snprintf(cmd,sizeof(cmd),"AT+CIPSEND=%zu",len);
    if (!_at_send(cmd,">",GSM_AT_TIMEOUT_MS)) return false;
    _gsm_write(data,len);
    return _at_send("","SEND OK",GSM_AT_TIMEOUT_MS*2);
}

static bool _mqtt_connect_packet() {
    static uint8_t pkt[256]; int pos=0; uint8_t var[64]; int vpos=0;
    _mw_str(var,&vpos,"MQTT"); var[vpos++]=0x04; var[vpos++]=0xC2;
    _mw_u16(var,&vpos,MQTT_KEEPALIVE_S);
    uint8_t pay[200]; int ppos=0;
    _mw_str(pay,&ppos,g_device_id);
    _mw_str(pay,&ppos,s_topic_status); _mw_str(pay,&ppos,MQTT_LWT_OFFLINE);
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
    if (_mqtt_publish_gsm(s_topic_tx,(const uint8_t*)s_payload, strlen(s_payload),s_pid++,false)) {
        g_last_upload_ms=millis();
        _commit_acknowledged_delete(bytes_read);
    }
}

// =============================================================================
//  OTA HANDLER
// =============================================================================
static void _handle_ota(const char* url) {
    if (!url || !*url) {
        LOG_ERROR("OTA", "Invalid OTA URL");
        return;
    }

    LOG_INFO("OTA", "Starting OTA from: %s", url);
    display_show_2line("  OTA UPDATE   ", " Downloading.. ");

    if (!_heap_ok()) {
        LOG_ERROR("OTA", "OTA blocked: insufficient heap");
        display_show_2line("  OTA FAILED   ", "  Low Memory   ");
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        if (!_wifi_connect()) {
            LOG_ERROR("OTA", "OTA blocked: Wi-Fi unavailable");
            display_show_2line("  OTA FAILED   ", "  No WiFi      ");
            return;
        }
    }

    WiFiClientSecure ota_client;
    ota_client.setInsecure();
    ota_client.setTimeout(15000);

    httpUpdate.rebootOnUpdate(false);
    httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    esp_task_wdt_delete(nullptr);
    t_httpUpdate_return ret = httpUpdate.update(ota_client, url);
    esp_task_wdt_add(nullptr);

    if (ret == HTTP_UPDATE_OK) {
        LOG_INFO("OTA", "Update SUCCESS. Finalizing flash...");
        display_show_2line("  OTA COMPLETE ", "  Rebooting... ");
        ota_client.stop();
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP.restart();
        return;
    }

    ota_client.stop();

    if (ret == HTTP_UPDATE_NO_UPDATES) {
        LOG_INFO("OTA", "No firmware update available");
        display_show_2line("  OTA UPDATE   ", "  No New Firmware");
        return;
    }

    LOG_ERROR("OTA", "Update FAILED. Code=%d", httpUpdate.getLastError());
    display_show_2line("  OTA FAILED   ", "  Retry Later  ");
}

// =============================================================================
//  DOWNLINK PARSER
// =============================================================================
void sync_process_downlink(const char* pay, unsigned int len) {
    if (!pay || !len) return;

    if (strncmp(pay, "SYS:FARE_A,", 11) == 0) {
        int fare = atoi(pay + 11);
        storage_write_fare_for_loc('A', fare);
        LOG_INFO("SYNC", "Fare[A] updated -> %d", fare);
        return;
    }
    if (strncmp(pay, "SYS:FARE_B,", 11) == 0) {
        int fare = atoi(pay + 11);
        storage_write_fare_for_loc('B', fare);
        LOG_INFO("SYNC", "Fare[B] updated -> %d", fare);
        return;
    }
    if (strncmp(pay, "SYS:FARE_C,", 11) == 0) {
        int fare = atoi(pay + 11);
        storage_write_fare_for_loc('C', fare);
        LOG_INFO("SYNC", "Fare[C] updated -> %d", fare);
        return;
    }

    if (strncmp(pay, "SYS:FARE,", 9) == 0) {
        int new_fare = atoi(pay + 9);
        storage_write_fare(new_fare);
        storage_write_fare_for_loc('A', new_fare);
        storage_write_fare_for_loc('B', new_fare);
        storage_write_fare_for_loc('C', new_fare);
        LOG_INFO("SYNC", "Global fare updated -> %d (all locations)", new_fare);
        return;
    }

    if (strncmp(pay, "SYS:LOC,", 8) == 0) {
        char loc = pay[8];
        if (loc == 'A' || loc == 'B' || loc == 'C') {
            storage_write_location(loc);
            LOG_INFO("SYNC", "Location updated -> %c", loc);
        } else {
            LOG_WARN("SYNC", "Invalid location: %c", loc);
        }
        return;
    }

    if (strncmp(pay, "SYS:OTA,", 8) == 0) {
        char url[256]; strncpy(url, pay + 8, sizeof(url) - 1); url[sizeof(url) - 1] = '\0';
        display_show_2line("  OTA UPDATE   ", "  Received...  ");
        _handle_ota(url); return;
    }
    
    if (strncmp(pay, "SYS:TIME,", 9) == 0) {
        unsigned long epoch = strtoul(pay + 9, nullptr, 10);
        if (epoch >= MIN_VALID_EPOCH) {
            transaction_set_rtc(epoch);
            storage_write_sync_ts(epoch);
            LOG_INFO("SYNC", "Time synced via broker: epoch=%lu, sync.dat updated", epoch);
        } else {
            LOG_WARN("SYNC", "Ignored implausible SYS:TIME payload: %s", pay);
        }
        return;
    }

    if (strncmp(pay, "SYS:NET,", 8) == 0) {
        int mode = atoi(pay + 8);
        if (mode >= 0 && mode <= 2) sync_set_net_mode((NetMode)mode);
        return;
    }
    
    if (strncmp(pay, "SYS:SYNC_COMPLETE", 17) == 0) {
        LOG_INFO("SYNC", "Backend confirmed sync. Lifting lockdown.");
        if (transaction_time_synced()) {
            storage_write_sync_ts(transaction_get_ts());
        } else {
            LOG_WARN("SYNC", "SYNC_COMPLETE received but clock not synced — sync.dat not updated");
        }
        return;
    }

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
        char act[4]={0}, lst[3]={0}, uid[16]={0}, pin[9]={0};
        if (sscanf(cmd, "%3[^:]:%2[^,],%15[^,],%8s", act, lst, uid, pin) >= 3) {
            const char* fp=nullptr;
            if      (strcmp(lst,"WL")==0) fp=FILE_WHITELIST;
            else if (strcmp(lst,"BL")==0) fp=FILE_BLACKLIST;
            else if (strcmp(lst,"DR")==0) fp=FILE_DRIVERS;
            else if (strcmp(lst,"AD")==0) fp=FILE_ADMINS;
            if (fp) {
                if (strcmp(act,"ADD")==0) {
                    if ((strcmp(lst,"DR")==0 || strcmp(lst,"AD")==0) && pin[0] != '\0') {
                        storage_append_uid_with_pin(fp, uid, pin);
                    } else {
                        storage_append_uid(fp, uid);
                    }
                } else if (strcmp(act,"REM")==0) {
                    storage_remove_uid(fp, uid);
                }
            }
        }
        cmd=strtok(nullptr,"|");
    }
}

// =============================================================================
//  INIT
// =============================================================================
void sync_init() {
    Serial2.begin(GSM_BAUD, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
    vTaskDelay(pdMS_TO_TICKS(1000));
    _at_send("ATE0","OK",2000);
    WiFi.mode(WIFI_STA);
    WiFi.onEvent(_wifi_got_ip_handler, ARDUINO_EVENT_WIFI_STA_GOT_IP);

    _rebuild_topics_from_device_id();
    LOG_INFO("SYNC", "Topics bound to device: %s", g_device_id);
}

void sync_set_task_handle(TaskHandle_t h) { s_task_handle=h; }
bool sync_is_running()                    { return s_running;  }

void sync_trigger_now() {
    if (!s_task_handle) return;

    uint32_t now = millis();
    if ((now - s_last_trigger_ms) < SYNC_TRIGGER_COOLDOWN_MS) {
        LOG_DEBUG("SYNC", "Sync trigger suppressed by cooldown");
        return;
    }

    s_last_trigger_ms = now;
    xTaskNotify(s_task_handle, 1, eSetValueWithOverwrite);
}

// =============================================================================
//  CORE 1 TASK — MAIN LOOP
// =============================================================================
void sync_task(void* params) {
    (void)params;
    esp_task_wdt_add(nullptr);
    sync_init();
    
    uint32_t last_sync = 0;
    
    enum ActiveNet { USE_WIFI, USE_GSM };
    ActiveNet current_net = USE_WIFI;
    
    uint8_t wifi_fails = 0;
    uint8_t gsm_fails = 0;
    
    while (true) {
        esp_task_wdt_reset();

        uint32_t notif = 0;
        bool triggered = xTaskNotifyWait(0, ULONG_MAX, &notif, pdMS_TO_TICKS(100)) == pdTRUE;
        bool time_to_sync = (millis() - last_sync) > SYNC_INTERVAL_MS;
        
        NetMode mode = sync_get_net_mode();
        
        if (mode == NET_MODE_WIFI) current_net = USE_WIFI;
        if (mode == NET_MODE_GSM)  current_net = USE_GSM;

        if (current_net == USE_WIFI) {
            if (triggered || time_to_sync || wifi_fails == 0) { 
                if (_mqtt_connect_wifi()) {
                    wifi_fails = 0;
                    
                    s_mqtt.loop();
                    
                    if (triggered || time_to_sync) {
                        s_running = true;
                        _flush_tx_wifi();
                        last_sync = millis();
                        LOG_INFO("SYS", "WIFI SYNC OK | Free Heap: %d B | Stack Free: %d words", 
                                 ESP.getFreeHeap(), 
                                 uxTaskGetStackHighWaterMark(s_task_handle));
                        s_running = false;
                    }

                    // Periodic time cache refresh
                    static uint32_t last_cache_write_ms = 0;
                    if (transaction_time_synced() && 
                        (millis() - last_cache_write_ms) >= TIME_CACHE_INTERVAL_MS) {
                        last_cache_write_ms = millis();
                        storage_write_time_cache(transaction_get_ts(), millis());
                    }
                } else {
                    wifi_fails++;
                    LOG_WARN("SYNC", "Wi-Fi fail %d/6", wifi_fails);
                    
                    if (wifi_fails >= 6 && mode == NET_MODE_AUTO) {
                        LOG_ERROR("SYNC", "Wi-Fi hit 6 failures. Flipping to GSM!");
                        current_net = USE_GSM;
                        gsm_fails = 0;
                    }
                }
            }
        } 
        
        else if (current_net == USE_GSM) {
            if (triggered || time_to_sync) {
                LOG_WARN("SYNC", "Waking up SIM800L Module...");
                s_running = true;
                
                if (_gsm_wake() && _gsm_open_gprs() && _gsm_tcp_connect() && _mqtt_connect_packet()) {
                    gsm_fails = 0;
                    bool online_ok = _mqtt_publish_gsm(s_topic_status, (const uint8_t*)MQTT_LWT_ONLINE, strlen(MQTT_LWT_ONLINE), 0, true);
                    LOG_INFO("GSM", "ONLINE publish %s", online_ok ? "ACKed" : "FAILED");
                    
                    if (!transaction_time_synced()) {
                        LOG_INFO("GSM", "Requesting network time from tower...");
                        
                        _at_send("AT+CLTS=1", "OK", GSM_AT_TIMEOUT_MS);
                        
                        if (_at_send("AT+CCLK?", "+CCLK:", GSM_AT_TIMEOUT_MS)) {
                            char* cclk = strstr(s_at_resp, "+CCLK:");
                            if (cclk) {
                                char* q1 = strchr(cclk, '"');
                                if (q1) {
                                    int yr = 0, mo = 0, dy = 0, hr = 0, mn = 0, sc = 0, tz_hr = 0, tz_mn = 0;
                                    char tz_sign = '+';
                                    
                                    int fields = sscanf(q1 + 1, "%2d/%2d/%2d,%2d:%2d:%2d%c%2d",
                                                       &yr, &mo, &dy, &hr, &mn, &sc, &tz_sign, &tz_mn);
                                    
                                    if (fields >= 6) {
                                        struct tm t = {};
                                        t.tm_year = yr + 100;
                                        t.tm_mon  = mo - 1;
                                        t.tm_mday = dy;
                                        t.tm_hour = hr;
                                        t.tm_min  = mn;
                                        t.tm_sec  = sc;
                                        
                                        time_t epoch = mktime(&t);
                                        
                                        if (fields >= 7) {
                                            int offset_sec = tz_hr * 3600 + tz_mn * 60;
                                            if (tz_sign == '-') offset_sec = -offset_sec;
                                            epoch -= offset_sec;
                                        }
                                        
                                        if (epoch > 0 && (unsigned long)epoch >= MIN_VALID_EPOCH) {
                                            transaction_set_rtc((unsigned long)epoch);
                                            storage_write_sync_ts((unsigned long)epoch);
                                            LOG_INFO("GSM", "Time synced via tower: epoch=%lu (UTC), sync.dat updated", (unsigned long)epoch);
                                        } else {
                                            LOG_WARN("GSM", "Tower time invalid: epoch=%ld", (long)epoch);
                                        }
                                    }
                                }
                            }
                        }
                        
                        if (!transaction_time_synced()) {
                            LOG_WARN("GSM", "Tower time unavailable — will retry next cycle");
                        }
                    }
                    
                    _flush_tx_gsm();
                    
                    LOG_INFO("SYS", "GSM SYNC OK | Free Heap: %d B | Stack Free: %d words", 
                             ESP.getFreeHeap(), 
                             uxTaskGetStackHighWaterMark(s_task_handle));

                    // Periodic time cache refresh
                    static uint32_t last_cache_write_ms = 0;
                    if (transaction_time_synced() && 
                        (millis() - last_cache_write_ms) >= TIME_CACHE_INTERVAL_MS) {
                        last_cache_write_ms = millis();
                        storage_write_time_cache(transaction_get_ts(), millis());
                    }
                } else {
                    gsm_fails++;
                    LOG_ERROR("SYNC", "GSM fail %d/3", gsm_fails);
                    
                    if (gsm_fails >= 3 && mode == NET_MODE_AUTO) {
                        LOG_ERROR("SYNC", "GSM hit 3 failures. Flipping back to Wi-Fi!");
                        current_net = USE_WIFI;
                        wifi_fails = 0;
                    }
                }
                
                _gsm_sleep();
                last_sync = millis();
                s_running = false;
            }
        }
    }
}