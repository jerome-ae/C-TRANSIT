// =============================================================================
// sync.cpp  —  Core 1: Dual-network MQTT sync (WiFi first, GSM fallback)
//              + OTA firmware update support
// =============================================================================
#include "sync.h"
#include "../storage/storage.h"
#include "../transaction/transaction.h"
#include "../logger/logger.h"
#include "../wifi_manager/wifi_manager.h"
#include <LittleFS.h>
#include <HTTPUpdate.h>   // ESP32 OTA over HTTP

// ── Exported timestamps (read by Core 0 LCD animation) ───────────────────────
volatile uint32_t g_last_upload_ms   = 0;
volatile uint32_t g_last_download_ms = 0;

// ── Module state ──────────────────────────────────────────────────────────────
static TaskHandle_t  s_task_handle = nullptr;
static volatile bool s_running     = false;

// ── Fixed buffers (no String class — SAD Section 1.2) ────────────────────────
static char s_at_resp[256];
static char s_dl_buf[512];
static char s_payload[MQTT_PAYLOAD_BUF];

// =============================================================================
//  NETWORK MODE  (persisted to /netmode.dat)
// =============================================================================
NetMode sync_get_net_mode() {
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
//  LOW-LEVEL AT COMMAND ENGINE
// =============================================================================
static void _gsm_flush_rx() { while (Serial2.available()) Serial2.read(); }

static bool _at_send(const char* cmd, const char* expect, uint32_t timeoutMs) {
    _gsm_flush_rx();
    Serial2.println(cmd);
    LOG_DEBUG("GSM", ">> %s", cmd);
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
    LOG_INFO("GSM", "GPRS open");
    return true;
}

static bool _gsm_tcp_connect() {
    if (!_at_send("AT+CIPMUX=0", "OK", GSM_AT_TIMEOUT_MS)) return false;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%d", MQTT_HOST, MQTT_PORT);
    if (!_at_send(cmd, "CONNECT", GSM_TCP_TIMEOUT_MS)) return false;
    LOG_INFO("GSM", "TCP connected");
    return true;
}

// =============================================================================
//  RAW MQTT PACKET BUILDER
// =============================================================================
static void _mw_u16(uint8_t* b, int* p, uint16_t v) {
    b[(*p)++]=(uint8_t)(v>>8); b[(*p)++]=(uint8_t)(v&0xFF);
}
static void _mw_str(uint8_t* b, int* p, const char* s) {
    uint16_t l=(uint16_t)strlen(s); _mw_u16(b,p,l);
    memcpy(b+*p,s,l); *p+=l;
}
static int _mw_rem(uint8_t* b, int r) {
    int p=0;
    do { uint8_t e=r%128; r/=128; if(r>0)e|=0x80; b[p++]=e; } while(r>0);
    return p;
}
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
                for (size_t i=0;i<nlen;i++)
                    if (win[(wp-nlen+i)%sizeof(win)]!=needle[i]){m=false;break;}
                if (m) return true;
            }
        } else vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

// =============================================================================
//  MQTT CONNECT / PUBLISH / SUBSCRIBE (GSM path)
// =============================================================================
static bool _mqtt_connect_packet() {
    static uint8_t pkt[256]; int pos=0;
    uint8_t var[64]; int vpos=0;
    _mw_str(var,&vpos,"MQTT"); var[vpos++]=0x04; var[vpos++]=0xC6;
    _mw_u16(var,&vpos,MQTT_KEEPALIVE_S);
    uint8_t pay[200]; int ppos=0;
    _mw_str(pay,&ppos,MQTT_CLIENT_ID);
    _mw_str(pay,&ppos,MQTT_TOPIC_STATUS);
    _mw_str(pay,&ppos,MQTT_LWT_OFFLINE);
    _mw_str(pay,&ppos,MQTT_USERNAME);
    _mw_str(pay,&ppos,MQTT_PASSWORD);
    int rem=vpos+ppos;
    pkt[pos++]=0x10; pos+=_mw_rem(pkt+pos,rem);
    memcpy(pkt+pos,var,vpos); pos+=vpos;
    memcpy(pkt+pos,pay,ppos); pos+=ppos;
    if (!_gsm_cipsend(pkt,pos)) return false;
    const uint8_t ca[]={0x20,0x02,0x00,0x00};
    if (!_wait_bytes(ca,4,GSM_AT_TIMEOUT_MS)) { LOG_ERROR("GSM","No CONNACK"); return false; }
    LOG_INFO("GSM","MQTT CONNACK OK");
    return true;
}

static bool _mqtt_publish_gsm(const char* topic, const uint8_t* payload,
                               size_t paylen, uint16_t pid, bool retain) {
    static uint8_t pkt[MQTT_PAYLOAD_BUF+64]; int pos=0;
    uint8_t flags=0x30;
    if (retain)       flags|=0x01;
    if (MQTT_QOS > 0) flags|=(MQTT_QOS<<1);
    uint8_t var[64]; int vpos=0;
    _mw_str(var,&vpos,topic);
    if (MQTT_QOS>0) _mw_u16(var,&vpos,pid);
    int rem=vpos+(int)paylen;
    pkt[pos++]=flags; pos+=_mw_rem(pkt+pos,rem);
    memcpy(pkt+pos,var,vpos); pos+=vpos;
    memcpy(pkt+pos,payload,paylen); pos+=(int)paylen;
    if (!_gsm_cipsend(pkt,pos)) return false;
    if (MQTT_QOS==1) {
        const uint8_t ph[]={0x40,0x02};
        if (!_wait_bytes(ph,2,PUBACK_TIMEOUT_MS)) {
            LOG_ERROR("GSM","No PUBACK pid=%d",pid); return false;
        }
        uint8_t ib[2]; _gsm_read(ib,2,500);
        LOG_INFO("GSM","PUBACK OK pid=%d",pid);
    }
    return true;
}

static bool _mqtt_subscribe_gsm() {
    static uint8_t pkt[64]; int pos=0;
    uint8_t var[32]; int vpos=0;
    _mw_u16(var,&vpos,1); _mw_str(var,&vpos,MQTT_TOPIC_RX); var[vpos++]=MQTT_QOS;
    pkt[pos++]=0x82; pos+=_mw_rem(pkt+pos,vpos);
    memcpy(pkt+pos,var,vpos); pos+=vpos;
    if (!_gsm_cipsend(pkt,pos)) return false;
    const uint8_t sb[]={0x90};
    if (!_wait_bytes(sb,1,GSM_AT_TIMEOUT_MS)) return false;
    LOG_INFO("GSM","Subscribed to %s",MQTT_TOPIC_RX);
    return true;
}

static void _mqtt_ping_gsm() {
    const uint8_t pr[]={0xC0,0x00}; _gsm_cipsend(pr,2);
}

// =============================================================================
//  DOWNLINK DRAIN (GSM path)
// =============================================================================
static void _drain_downlink(uint32_t windowMs) {
    uint32_t start=millis();
    while ((millis()-start)<windowMs) {
        if (!Serial2.available()) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        uint8_t hdr=(uint8_t)Serial2.read();
        if ((hdr&0xF0)!=0x30) continue;
        int remaining=0,mult=1; uint8_t enc;
        do {
            uint32_t t=millis();
            while (!Serial2.available()&&(millis()-t)<1000);
            enc=(uint8_t)Serial2.read();
            remaining+=(enc&0x7F)*mult; mult*=128;
        } while(enc&0x80);
        uint8_t tb[2]; _gsm_read(tb,2,500);
        uint16_t tlen=(uint16_t)((tb[0]<<8)|tb[1]); remaining-=2;
        uint8_t skip[64];
        _gsm_read(skip,(tlen<sizeof(skip))?tlen:sizeof(skip),500);
        remaining-=(int)tlen;
        uint8_t qos=(hdr>>1)&0x03; uint16_t pid=0;
        if (qos>0) {
            uint8_t ib[2]; _gsm_read(ib,2,500);
            pid=(uint16_t)((ib[0]<<8)|ib[1]); remaining-=2;
        }
        size_t plen=(remaining<(int)sizeof(s_dl_buf)-1)?(size_t)remaining:sizeof(s_dl_buf)-1;
        int got=_gsm_read((uint8_t*)s_dl_buf,plen,1000);
        s_dl_buf[got]='\0';
        g_last_download_ms=millis();
        LOG_INFO("GSM","Downlink: %s",s_dl_buf);
        sync_process_downlink(s_dl_buf,(unsigned int)got);
        if (qos==1) {
            uint8_t pb[]={0x40,0x02,(uint8_t)(pid>>8),(uint8_t)(pid&0xFF)};
            _gsm_cipsend(pb,4);
        }
    }
}

// =============================================================================
//  OTA FIRMWARE UPDATE
//  Triggered by downlink: SYS:OTA,http://your-server/firmware.bin
//  WiFi must be connected — OTA over GSM is too slow and unreliable.
// =============================================================================
static void _handle_ota(const char* url) {
    LOG_INFO("OTA", "Starting OTA from: %s", url);

    // OTA requires WiFi — check connection
    if (!wifi_manager_is_connected()) {
        LOG_ERROR("OTA", "OTA requires WiFi — not connected, aborting");
        return;
    }

    // HTTPUpdate flashes directly to the App1 partition
    WiFiClient ota_client;
    t_httpUpdate_return ret = httpUpdate.update(ota_client, url);

    switch (ret) {
        case HTTP_UPDATE_FAILED:
            LOG_ERROR("OTA", "OTA failed: %s", httpUpdate.getLastErrorString().c_str());
            break;
        case HTTP_UPDATE_NO_UPDATES:
            LOG_INFO("OTA", "OTA: firmware already up to date");
            break;
        case HTTP_UPDATE_OK:
            LOG_INFO("OTA", "OTA success — rebooting...");
            ESP.restart();   // reboots into new firmware on App1
            break;
    }
}

// =============================================================================
//  DOWNLINK PARSER  (SAD Section 5.4 + OTA + net mode switching)
// =============================================================================
void sync_process_downlink(const char* pay, unsigned int len) {
    if (!pay || !len) return;

    // ── OTA command: SYS:OTA,http://...  ─────────────────────────────────────
    if (strncmp(pay, "SYS:OTA,", 8) == 0) {
        char url[256];
        strncpy(url, pay + 8, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
        _handle_ota(url);
        return;
    }

    // ── Net mode switch: SYS:NET,0 | SYS:NET,1 | SYS:NET,2 ──────────────────
    if (strncmp(pay, "SYS:NET,", 8) == 0) {
        int mode = atoi(pay + 8);
        if (mode >= 0 && mode <= 2) sync_set_net_mode((NetMode)mode);
        return;
    }

    // ── Cold sync complete ────────────────────────────────────────────────────
    if (strncmp(pay, "SYS:SYNC_COMPLETE", 17) == 0) {
        LOG_INFO("SYNC", "Cold sync complete"); return;
    }

    // ── Full list ingest (cold start): SYS:WL,uid1|uid2 ──────────────────────
    if (strncmp(pay, "SYS:", 4) == 0) {
        char tmp[512];
        strncpy(tmp, pay, sizeof(tmp) - 1); tmp[sizeof(tmp)-1]='\0';
        char* colon = strchr(tmp + 4, ':');
        if (!colon) return;
        char list[3]={0}; strncpy(list, tmp+4, 2);
        const char* uids = colon + 1;
        const char* fp   = nullptr;
        if      (strcmp(list,"WL")==0) fp=FILE_WHITELIST;
        else if (strcmp(list,"BL")==0) fp=FILE_BLACKLIST;
        else if (strcmp(list,"DR")==0) fp=FILE_DRIVERS;
        else if (strcmp(list,"AD")==0) fp=FILE_ADMINS;
        if (fp) storage_ingest_chunk(fp, uids);
        return;
    }

    // ── Differential updates: ADD:BL,uid | REM:WL,uid ────────────────────────
    char buf[512];
    strncpy(buf, pay, sizeof(buf)-1); buf[sizeof(buf)-1]='\0';
    char* cmd = strtok(buf, "|");
    while (cmd) {
        char act[4]={0}, lst[3]={0}, uid[9]={0};
        if (sscanf(cmd, "%3[^:]:%2[^,],%8s", act, lst, uid) < 3) {
            cmd=strtok(nullptr,"|"); continue;
        }
        const char* fp=nullptr;
        if      (strcmp(lst,"WL")==0) fp=FILE_WHITELIST;
        else if (strcmp(lst,"BL")==0) fp=FILE_BLACKLIST;
        else if (strcmp(lst,"DR")==0) fp=FILE_DRIVERS;
        else if (strcmp(lst,"AD")==0) fp=FILE_ADMINS;
        if (fp) {
            if      (strcmp(act,"ADD")==0) storage_append_uid(fp,uid);
            else if (strcmp(act,"REM")==0) storage_remove_uid(fp,uid);
        }
        cmd=strtok(nullptr,"|");
    }
}

// =============================================================================
//  TX.LOG FLUSH (shared logic — called by both WiFi and GSM paths)
// =============================================================================
static void _flush_tx_gsm() {
    size_t bytes_read=0;
    int lines=storage_stream_tx_chunk(s_payload,sizeof(s_payload),&bytes_read);
    if (lines==0) { LOG_INFO("SYNC","tx.log empty"); return; }
    LOG_INFO("SYNC","Flushing %d lines via GSM",lines);
    static uint16_t s_pid=1;
    bool ok=_mqtt_publish_gsm(MQTT_TOPIC_TX,(const uint8_t*)s_payload,
                               strlen(s_payload),s_pid++,false);
    if (!ok) { LOG_ERROR("SYNC","GSM publish failed — tx.log preserved"); return; }
    g_last_upload_ms=millis();
    storage_atomic_delete_sent(bytes_read);
    storage_write_sync_ts(transaction_get_ts());
    LOG_INFO("SYNC","GSM flush complete");
}

// =============================================================================
//  GSM SYNC CYCLE
// =============================================================================
static void _run_gsm_sync_cycle() {
    s_running=true;
    bool up=false;
    for (int i=1;i<=GSM_MAX_RETRIES;i++) {
        LOG_INFO("SYNC","GSM wake attempt %d/%d",i,GSM_MAX_RETRIES);
        if (_gsm_wake()) { up=true; break; }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    if (!up) {
        _at_send("AT+CFUN=0","OK",3000);
        s_running=false; return;
    }
    if (!_gsm_open_gprs())       { _gsm_sleep(); s_running=false; return; }
    if (!_gsm_tcp_connect())     { _gsm_sleep(); s_running=false; return; }
    if (!_mqtt_connect_packet()) { _gsm_sleep(); s_running=false; return; }
    _mqtt_publish_gsm(MQTT_TOPIC_STATUS,(const uint8_t*)MQTT_LWT_ONLINE,
                      strlen(MQTT_LWT_ONLINE),0,true);
    _mqtt_subscribe_gsm();
    _flush_tx_gsm();
    _drain_downlink(3000);
    _mqtt_ping_gsm();
    _gsm_sleep();
    s_running=false;
    LOG_INFO("SYNC","GSM sync cycle complete");
}

// =============================================================================
//  PUBLIC API
// =============================================================================
void sync_init() {
    Serial2.begin(GSM_BAUD, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (_at_send("AT","OK",3000)) LOG_INFO("SYNC","SIM800L detected");
    else                          LOG_WARN("SYNC","SIM800L not responding");
    _at_send("ATE0","OK",2000);
    wifi_manager_init();
}

void sync_set_task_handle(TaskHandle_t h) { s_task_handle=h; }
bool sync_is_running()                    { return s_running;  }

void sync_trigger_now() {
    if (s_task_handle) xTaskNotify(s_task_handle,1,eSetValueWithOverwrite);
}

// =============================================================================
//  CORE 1 TASK — THE TRAFFIC COP
// =============================================================================
void sync_task(void* params) {
    (void)params;
    LOG_INFO("SYNC","Core 1 sync_task on Core %d",xPortGetCoreID());
    vTaskDelay(pdMS_TO_TICKS(3000));
    sync_init();

    while (true) {
        uint32_t notif=0;
        xTaskNotifyWait(0,ULONG_MAX,&notif,pdMS_TO_TICKS(SYNC_INTERVAL_MS));
        LOG_INFO("SYNC","Sync triggered");
        s_running=true;

        NetMode mode=sync_get_net_mode();

        if (mode==NET_MODE_GSM) {
            // Forced GSM only
            LOG_INFO("SYNC","Mode: GSM-ONLY");
            _run_gsm_sync_cycle();

        } else if (mode==NET_MODE_WIFI) {
            // Forced WiFi only
            LOG_INFO("SYNC","Mode: WIFI-ONLY");
            if (wifi_manager_is_connected()) wifi_manager_run_sync_cycle();
            else LOG_WARN("SYNC","WiFi-only mode but no WiFi available — skipping");

        } else {
            // Auto: WiFi first, fall back to GSM
            LOG_INFO("SYNC","Mode: AUTO");
            if (wifi_manager_is_connected()) {
                LOG_INFO("SYNC","Using WiFi");
                wifi_manager_run_sync_cycle();
            } else {
                LOG_INFO("SYNC","WiFi unavailable — falling back to GSM");
                _run_gsm_sync_cycle();
            }
        }

        s_running=false;
    }
}
