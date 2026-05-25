// =============================================================================
// sync.cpp  —  Core 1 MQTT sync via SIM800L GSM (AT commands over UART)
// =============================================================================
// SAD References:
//   Section 1.1  — SIM800L constraint: AT commands only, ESP32 owns no TCP stack
//   Section 1.2  — Core 1 owns all GSM/MQTT work; no String class; fixed char[]
//   Section 5.1  — Wake sequence, 3-attempt failsafe, sleep after sync
//   Section 5.2  — Topic routing: /tx uplink, /rx downlink, /status LWT
//   Section 5.3  — QoS 1: publish → wait PUBACK → wipe tx.log atomically
//   Section 5.4  — Differential downlink: ADD:BL,uid | REM:WL,uid
// =============================================================================
#include "sync.h"
#include "../storage/storage.h"
#include "../transaction/transaction.h"
#include "../logger/logger.h"

// ── Animation timestamps (volatile — read by Core 0 LCD task) ────────────────
volatile uint32_t g_last_upload_ms   = 0;
volatile uint32_t g_last_download_ms = 0;

// ── Module state ──────────────────────────────────────────────────────────────
static TaskHandle_t  s_task_handle  = nullptr;
static volatile bool s_running      = false;

// ── Fixed-size buffers (SAD: no String/heap on Core 1) ───────────────────────
static char s_at_resp[256];
static char s_dl_buf[512];
static char s_payload[MQTT_PAYLOAD_BUF];

// =============================================================================
//  LOW-LEVEL AT COMMAND ENGINE
// =============================================================================

static void _gsm_flush_rx() {
    while (Serial2.available()) Serial2.read();
}

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
        if (strstr(s_at_resp, expect)) {
            LOG_DEBUG("GSM", "<< (ok) %s", s_at_resp);
            return true;
        }
        if (strstr(s_at_resp, "ERROR")) {
            LOG_WARN("GSM", "<< ERROR on cmd: %s", cmd);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    LOG_WARN("GSM", "TIMEOUT on cmd: %s  resp: %s", cmd, s_at_resp);
    return false;
}

static void _gsm_write(const uint8_t* buf, size_t len) {
    Serial2.write(buf, len);
}

static int _gsm_read(uint8_t* buf, size_t maxLen, uint32_t timeoutMs) {
    uint32_t start = millis();
    int pos = 0;
    while ((millis() - start) < timeoutMs && pos < (int)maxLen) {
        if (Serial2.available()) {
            buf[pos++] = (uint8_t)Serial2.read();
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return pos;
}

// =============================================================================
//  SIM800L LIFECYCLE  (SAD Section 5.1)
// =============================================================================

static bool _gsm_wake() {
    LOG_INFO("GSM", "Waking modem: AT+CFUN=1");
    if (!_at_send("AT+CFUN=1", "OK", GSM_AT_TIMEOUT_MS)) return false;

    LOG_INFO("GSM", "Waiting for network registration...");
    uint32_t start = millis();
    while ((millis() - start) < GSM_REG_TIMEOUT_MS) {
        _gsm_flush_rx();
        Serial2.println("AT+CREG?");
        vTaskDelay(pdMS_TO_TICKS(1000));
        int pos = 0;
        memset(s_at_resp, 0, sizeof(s_at_resp));
        uint32_t t = millis();
        while ((millis() - t) < 2000 && pos < (int)sizeof(s_at_resp) - 1) {
            if (Serial2.available()) s_at_resp[pos++] = (char)Serial2.read();
        }
        s_at_resp[pos] = '\0';
        if (strstr(s_at_resp, "+CREG: 1") || strstr(s_at_resp, "+CREG: 5") ||
            strstr(s_at_resp, "+CREG:1")  || strstr(s_at_resp, "+CREG:5")) {
            LOG_INFO("GSM", "Network registered");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    LOG_ERROR("GSM", "Network registration timeout");
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
    if (!_at_send("AT+CIFSR", ".",  GSM_AT_TIMEOUT_MS)) {
        LOG_ERROR("GSM", "No IP assigned"); return false;
    }
    LOG_INFO("GSM", "GPRS open. IP: %s", s_at_resp);
    return true;
}

static bool _gsm_tcp_connect() {
    if (!_at_send("AT+CIPMUX=0", "OK", GSM_AT_TIMEOUT_MS)) return false;
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
        "AT+CIPSTART=\"TCP\",\"%s\",%d", MQTT_HOST, MQTT_PORT);
    if (!_at_send(cmd, "CONNECT", GSM_TCP_TIMEOUT_MS)) {
        LOG_ERROR("GSM", "TCP connect failed"); return false;
    }
    LOG_INFO("GSM", "TCP connected");
    return true;
}

// =============================================================================
//  MQTT PACKET BUILDER  (raw MQTT 3.1.1 over SIM800L TCP)
// =============================================================================

static void _mqtt_write_u16(uint8_t* buf, int* pos, uint16_t val) {
    buf[(*pos)++] = (uint8_t)(val >> 8);
    buf[(*pos)++] = (uint8_t)(val & 0xFF);
}

static void _mqtt_write_str(uint8_t* buf, int* pos, const char* s) {
    uint16_t len = (uint16_t)strlen(s);
    _mqtt_write_u16(buf, pos, len);
    memcpy(buf + *pos, s, len);
    *pos += len;
}

static int _mqtt_encode_remaining(uint8_t* buf, int remaining) {
    int pos = 0;
    do {
        uint8_t enc = remaining % 128;
        remaining  /= 128;
        if (remaining > 0) enc |= 0x80;
        buf[pos++] = enc;
    } while (remaining > 0);
    return pos;
}

static bool _gsm_cipsend(const uint8_t* data, size_t len) {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%zu", len);
    if (!_at_send(cmd, ">", GSM_AT_TIMEOUT_MS)) return false;
    _gsm_write(data, len);
    return _at_send("", "SEND OK", GSM_AT_TIMEOUT_MS * 2);
}

static bool _wait_for_bytes(const uint8_t* needle, size_t nlen, uint32_t timeoutMs) {
    uint8_t  window[16] = {0};
    size_t   wpos       = 0;
    uint32_t start      = millis();
    while ((millis() - start) < timeoutMs) {
        if (Serial2.available()) {
            window[wpos % sizeof(window)] = (uint8_t)Serial2.read();
            wpos++;
            if (wpos >= nlen) {
                bool match = true;
                for (size_t i = 0; i < nlen; i++) {
                    if (window[(wpos - nlen + i) % sizeof(window)] != needle[i]) {
                        match = false; break;
                    }
                }
                if (match) return true;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return false;
}

// =============================================================================
//  MQTT CONNECT  (SAD Section 5.2 — LWT on /status topic)
// =============================================================================
static bool _mqtt_connect_packet() {
    static uint8_t pkt[256];
    int pos = 0;

    uint8_t var[64]; int vpos = 0;
    _mqtt_write_str(var, &vpos, "MQTT");
    var[vpos++] = 0x04;   // protocol level 3.1.1
    var[vpos++] = 0xC6;   // flags: CleanSession | Will | Username | Password
    _mqtt_write_u16(var, &vpos, MQTT_KEEPALIVE_S);

    uint8_t pay[200]; int ppos = 0;
    _mqtt_write_str(pay, &ppos, MQTT_CLIENT_ID);
    _mqtt_write_str(pay, &ppos, MQTT_TOPIC_STATUS);
    _mqtt_write_str(pay, &ppos, MQTT_LWT_OFFLINE);
    _mqtt_write_str(pay, &ppos, MQTT_USERNAME);
    _mqtt_write_str(pay, &ppos, MQTT_PASSWORD);

    int remaining = vpos + ppos;
    pkt[pos++] = 0x10;
    pos += _mqtt_encode_remaining(pkt + pos, remaining);
    memcpy(pkt + pos, var, vpos); pos += vpos;
    memcpy(pkt + pos, pay, ppos); pos += ppos;

    if (!_gsm_cipsend(pkt, pos)) return false;

    const uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
    if (!_wait_for_bytes(connack, 4, GSM_AT_TIMEOUT_MS)) {
        LOG_ERROR("GSM", "No CONNACK"); return false;
    }
    LOG_INFO("GSM", "MQTT CONNACK OK");
    return true;
}

// =============================================================================
//  MQTT PUBLISH  (QoS 1 — waits for PUBACK before returning)
// =============================================================================
static bool _mqtt_publish(const char* topic, const uint8_t* payload,
                          size_t paylen, uint16_t packet_id, bool retain) {
    static uint8_t pkt[MQTT_PAYLOAD_BUF + 64];
    int pos = 0;

    uint8_t flags = 0x30;
    if (retain)       flags |= 0x01;
    if (MQTT_QOS > 0) flags |= (MQTT_QOS << 1);

    uint8_t var[64]; int vpos = 0;
    _mqtt_write_str(var, &vpos, topic);
    if (MQTT_QOS > 0) _mqtt_write_u16(var, &vpos, packet_id);

    int remaining = vpos + (int)paylen;
    pkt[pos++] = flags;
    pos += _mqtt_encode_remaining(pkt + pos, remaining);
    memcpy(pkt + pos, var, vpos); pos += vpos;
    memcpy(pkt + pos, payload, paylen); pos += (int)paylen;

    if (!_gsm_cipsend(pkt, pos)) return false;

    if (MQTT_QOS == 1) {
        // SAD Section 5.3: wait for explicit PUBACK before declaring success
        const uint8_t puback_hdr[] = {0x40, 0x02};
        if (!_wait_for_bytes(puback_hdr, 2, PUBACK_TIMEOUT_MS)) {
            LOG_ERROR("GSM", "No PUBACK for packet %d", packet_id);
            return false;
        }
        uint8_t id_bytes[2];
        _gsm_read(id_bytes, 2, 500);
        LOG_INFO("GSM", "PUBACK OK for packet %d", packet_id);
    }
    return true;
}

// =============================================================================
//  MQTT SUBSCRIBE  (to /rx downlink)
// =============================================================================
static bool _mqtt_subscribe() {
    static uint8_t pkt[64];
    int pos = 0;

    uint8_t var[32]; int vpos = 0;
    _mqtt_write_u16(var, &vpos, 1);
    _mqtt_write_str(var, &vpos, MQTT_TOPIC_RX);
    var[vpos++] = MQTT_QOS;

    pkt[pos++] = 0x82;
    pos += _mqtt_encode_remaining(pkt + pos, vpos);
    memcpy(pkt + pos, var, vpos); pos += vpos;

    if (!_gsm_cipsend(pkt, pos)) return false;

    const uint8_t suback[] = {0x90};
    if (!_wait_for_bytes(suback, 1, GSM_AT_TIMEOUT_MS)) {
        LOG_ERROR("GSM", "No SUBACK"); return false;
    }
    LOG_INFO("GSM", "Subscribed to %s", MQTT_TOPIC_RX);
    return true;
}

static void _mqtt_ping() {
    const uint8_t pingreq[] = {0xC0, 0x00};
    _gsm_cipsend(pingreq, 2);
}

// =============================================================================
//  DOWNLINK DRAIN  (reads pending PUBLISH packets on /rx)
// =============================================================================
static void _drain_downlink(uint32_t windowMs) {
    uint32_t start = millis();
    while ((millis() - start) < windowMs) {
        if (!Serial2.available()) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

        uint8_t hdr = (uint8_t)Serial2.read();
        if ((hdr & 0xF0) != 0x30) continue;

        int remaining = 0, mult = 1;
        uint8_t enc;
        do {
            uint32_t t = millis();
            while (!Serial2.available() && (millis() - t) < 1000);
            enc = (uint8_t)Serial2.read();
            remaining += (enc & 0x7F) * mult;
            mult *= 128;
        } while (enc & 0x80);

        uint8_t tlen_buf[2];
        _gsm_read(tlen_buf, 2, 500);
        uint16_t tlen = (uint16_t)((tlen_buf[0] << 8) | tlen_buf[1]);
        remaining -= 2;

        uint8_t skip[64];
        size_t to_skip = (tlen < sizeof(skip)) ? tlen : sizeof(skip);
        _gsm_read(skip, to_skip, 500);
        remaining -= (int)tlen;

        uint8_t qos = (hdr >> 1) & 0x03;
        uint16_t pkt_id = 0;
        if (qos > 0) {
            uint8_t id_buf[2];
            _gsm_read(id_buf, 2, 500);
            pkt_id = (uint16_t)((id_buf[0] << 8) | id_buf[1]);
            remaining -= 2;
        }

        size_t plen = (remaining < (int)sizeof(s_dl_buf) - 1)
                      ? (size_t)remaining : sizeof(s_dl_buf) - 1;
        int got = _gsm_read((uint8_t*)s_dl_buf, plen, 1000);
        s_dl_buf[got] = '\0';

        g_last_download_ms = millis();
        LOG_INFO("GSM", "Downlink: %s", s_dl_buf);
        sync_process_downlink(s_dl_buf, (unsigned int)got);

        if (qos == 1) {
            uint8_t puback[] = {0x40, 0x02,
                                (uint8_t)(pkt_id >> 8),
                                (uint8_t)(pkt_id & 0xFF)};
            _gsm_cipsend(puback, 4);
        }
    }
}

// =============================================================================
//  DOWNLINK PARSER  (SAD Section 5.4)
// =============================================================================
void sync_process_downlink(const char* pay, unsigned int len) {
    if (!pay || !len) return;

    if (strncmp(pay, "SYS:SYNC_COMPLETE", 17) == 0) {
        LOG_INFO("SYNC", "Cold sync complete"); return;
    }

    if (strncmp(pay, "SYS:", 4) == 0) {
        char tmp[512];
        strncpy(tmp, pay, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        char* colon = strchr(tmp + 4, ':');
        if (!colon) return;
        char list[3] = {0};
        strncpy(list, tmp + 4, 2);
        const char* uids = colon + 1;
        const char* fp   = nullptr;
        if      (strcmp(list, "WL") == 0) fp = FILE_WHITELIST;
        else if (strcmp(list, "BL") == 0) fp = FILE_BLACKLIST;
        else if (strcmp(list, "DR") == 0) fp = FILE_DRIVERS;
        else if (strcmp(list, "AD") == 0) fp = FILE_ADMINS;
        if (fp) storage_ingest_chunk(fp, uids);
        return;
    }

    // ADD:BL,uid | REM:WL,uid
    char buf[512];
    strncpy(buf, pay, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char* cmd = strtok(buf, "|");
    while (cmd) {
        char act[4] = {0}, lst[3] = {0}, uid[9] = {0};
        if (sscanf(cmd, "%3[^:]:%2[^,],%8s", act, lst, uid) < 3) {
            cmd = strtok(nullptr, "|"); continue;
        }
        const char* fp = nullptr;
        if      (strcmp(lst, "WL") == 0) fp = FILE_WHITELIST;
        else if (strcmp(lst, "BL") == 0) fp = FILE_BLACKLIST;
        else if (strcmp(lst, "DR") == 0) fp = FILE_DRIVERS;
        else if (strcmp(lst, "AD") == 0) fp = FILE_ADMINS;
        if (fp) {
            if      (strcmp(act, "ADD") == 0) storage_append_uid(fp, uid);
            else if (strcmp(act, "REM") == 0) storage_remove_uid(fp, uid);
        }
        cmd = strtok(nullptr, "|");
    }
}

// =============================================================================
//  TX.LOG FLUSH  (SAD Section 4.2 & 5.3)
// =============================================================================
static void _flush_tx() {
    size_t bytes_read = 0;
    int lines = storage_stream_tx_chunk(s_payload, sizeof(s_payload), &bytes_read);
    if (lines == 0) { LOG_INFO("SYNC", "tx.log empty"); return; }

    LOG_INFO("SYNC", "Flushing %d lines (%zu bytes)", lines, bytes_read);

    static uint16_t s_packet_id = 1;
    bool ok = _mqtt_publish(MQTT_TOPIC_TX,
                            (const uint8_t*)s_payload, strlen(s_payload),
                            s_packet_id++, false);

    if (!ok) {
        // SAD: tx.log is never wiped without PUBACK confirmation
        LOG_ERROR("SYNC", "Publish failed — tx.log preserved");
        return;
    }

    g_last_upload_ms = millis();

    // SAD Section 4.2 — atomic post-PUBACK actions:
    storage_atomic_delete_sent(bytes_read);                    // 1. wipe tx.log
    storage_write_sync_ts(transaction_get_ts());               // 2. update sync.dat
    LOG_INFO("SYNC", "Flush complete. sync.dat updated");
}

// =============================================================================
//  FULL SYNC CYCLE
// =============================================================================
static void _run_sync_cycle() {
    s_running = true;

    // Step 1: Wake modem with retries (SAD Section 5.1)
    bool up = false;
    for (int i = 1; i <= GSM_MAX_RETRIES; i++) {
        LOG_INFO("SYNC", "GSM wake attempt %d/%d", i, GSM_MAX_RETRIES);
        if (_gsm_wake()) { up = true; break; }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    if (!up) {
        LOG_ERROR("SYNC", "GSM failed after %d attempts — sleeping", GSM_MAX_RETRIES);
        _at_send("AT+CFUN=0", "OK", 3000);
        s_running = false; return;
    }

    // Step 2: GPRS
    if (!_gsm_open_gprs())   { _gsm_sleep(); s_running = false; return; }

    // Step 3: TCP
    if (!_gsm_tcp_connect()) { _gsm_sleep(); s_running = false; return; }

    // Step 4: MQTT handshake + LWT
    if (!_mqtt_connect_packet()) { _gsm_sleep(); s_running = false; return; }

    // Publish ONLINE status immediately after connect
    _mqtt_publish(MQTT_TOPIC_STATUS,
                  (const uint8_t*)MQTT_LWT_ONLINE, strlen(MQTT_LWT_ONLINE),
                  0, true);

    // Step 5: Subscribe to /rx
    _mqtt_subscribe();

    // Step 6: Flush tx.log with QoS 1 guarantee
    _flush_tx();

    // Step 7: Drain any pending downlink messages
    _drain_downlink(3000);

    // Step 8: Keepalive ping
    _mqtt_ping();

    // Step 9: Sleep modem (SAD Section 5.1)
    _gsm_sleep();

    s_running = false;
    LOG_INFO("SYNC", "Sync cycle complete");
}

// =============================================================================
//  PUBLIC API
// =============================================================================
void sync_init() {
    Serial2.begin(GSM_BAUD, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (_at_send("AT", "OK", 3000)) {
        LOG_INFO("SYNC", "SIM800L detected");
    } else {
        LOG_WARN("SYNC", "SIM800L not responding — check wiring");
    }
    _at_send("ATE0", "OK", 2000);  // echo off
}

void sync_set_task_handle(TaskHandle_t h) { s_task_handle = h; }
bool sync_is_running()                    { return s_running;   }

void sync_trigger_now() {
    if (s_task_handle) xTaskNotify(s_task_handle, 1, eSetValueWithOverwrite);
}

void sync_task(void* params) {
    (void)params;
    LOG_INFO("SYNC", "Core 1 sync_task on Core %d", xPortGetCoreID());
    vTaskDelay(pdMS_TO_TICKS(3000));
    sync_init();

    while (true) {
        uint32_t notif = 0;
        xTaskNotifyWait(0, ULONG_MAX, &notif, pdMS_TO_TICKS(SYNC_INTERVAL_MS));
        LOG_INFO("SYNC", "Sync triggered");
        _run_sync_cycle();
    }
}
