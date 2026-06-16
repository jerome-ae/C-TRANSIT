// =============================================================================
// storage.cpp  —  LittleFS I/O  (no String class — fixed char[] only)
// =============================================================================
// SAD Section 1.2: Dynamic String class allocation is strictly prohibited.
// All string manipulation uses fixed-size char arrays and standard C functions.
// =============================================================================
#include "storage.h"
#include "../logger/logger.h"
#include <LittleFS.h>
#include <freertos/semphr.h>

// ── Internal file paths not in config.h ──────────────────────────────────────
#ifndef FILE_SESSION
#define FILE_SESSION  "/sess.dat"
#endif
#ifndef FILE_SYNC_TS
#define FILE_SYNC_TS  "/sync.dat"
#endif
#ifndef FILE_TX_TEMP
#define FILE_TX_TEMP  "/tx_tmp.dat"
#endif
#ifndef TX_LINE_MAX
#define TX_LINE_MAX   80
#endif

// ── Constants ─────────────────────────────────────────────────────────────────
static const size_t FS_HEADROOM_BYTES = 8192;
static const size_t LINE_BUF          = 96;   // max chars in any CSV line

// ── Mutex ─────────────────────────────────────────────────────────────────────
static SemaphoreHandle_t s_mtx = nullptr;
static bool _lock()   { return s_mtx && xSemaphoreTake(s_mtx, pdMS_TO_TICKS(3000)) == pdTRUE; }
static void _unlock() { if (s_mtx) xSemaphoreGive(s_mtx); }

// ── Free-space guard ──────────────────────────────────────────────────────────
static bool _has_space(size_t needed = 256) {
    size_t free_bytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    if (free_bytes < (needed + FS_HEADROOM_BYTES)) {
        LOG_ERROR("STORAGE", "Low space: free=%zu need=%zu", free_bytes, needed);
        return false;
    }
    return true;
}

// ── Read one line from an open file into a fixed buffer ───────────────────────
// Returns number of chars written (0 on empty line or EOF).
// Consumes the '\n'. Skips '\r'.
static int _read_line(File& f, char* buf, size_t bufsz) {
    int len = 0;
    while (f.available() && len < (int)bufsz - 1) {
        char c = (char)f.read();
        if (c == '\n') break;
        if (c == '\r') continue;
        buf[len++] = c;
    }
    buf[len] = '\0';
    return len;
}

// ── Extract the UID field from a CSV line (everything before first comma) ─────
static void _extract_uid(const char* line, char* uid_out, size_t uid_sz) {
    size_t i = 0;
    while (line[i] && line[i] != ',' && i < uid_sz - 1) {
        uid_out[i] = line[i];
        i++;
    }
    uid_out[i] = '\0';
}

// =============================================================================
//  storage_init
// =============================================================================
bool storage_init() {
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) { LOG_ERROR("STORAGE", "Mutex create failed"); return false; }

    if (!LittleFS.begin(true, "/littlefs", 10, "spiffs")) {
        LOG_ERROR("STORAGE", "LittleFS mount failed");
        return false;
    }

    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    LOG_INFO("STORAGE", "Mounted: total=%zuKB used=%zuKB free=%zuKB",
             total / 1024, used / 1024, (total - used) / 1024);

    const char* needed[] = {
        FILE_WHITELIST, FILE_BLACKLIST, FILE_TX_LOG,
        FILE_SYNC_TS,   FILE_DRIVERS,   FILE_ADMINS
    };
    for (auto p : needed) {
        if (!LittleFS.exists(p)) {
            File f = LittleFS.open(p, "w");
            if (f) { f.close(); LOG_WARN("STORAGE", "Created %s", p); }
            else   { LOG_ERROR("STORAGE", "Could not create %s", p); }
        }
    }
    if (!LittleFS.exists(FILE_SESSION)) storage_write_session(0, "NONE");
    return true;
}

// =============================================================================
//  storage_uid_in_file
//  Buffered line-by-line search — never loads full file into RAM.
//  Matches on the UID field (everything before the first comma, or full line).
// =============================================================================
StorageResult storage_uid_in_file(const char* path, const char* uid) {
    if (!_lock()) return STORAGE_ERROR;
    File f = LittleFS.open(path, "r");
    if (!f) { _unlock(); return STORAGE_NOT_FOUND; }

    char     line[LINE_BUF];
    char     file_uid[16];
    StorageResult res = STORAGE_NOT_FOUND;

    while (f.available()) {
        if (_read_line(f, line, sizeof(line)) == 0) continue;
        _extract_uid(line, file_uid, sizeof(file_uid));
        if (strcmp(file_uid, uid) == 0) { res = STORAGE_FOUND; break; }
    }

    f.close();
    _unlock();
    return res;
}

// =============================================================================
//  storage_append_uid
// =============================================================================
StorageResult storage_append_uid(const char* path, const char* uid) {
    if (!_lock()) return STORAGE_ERROR;
    if (!_has_space(32)) { _unlock(); return STORAGE_FULL; }

    File f = LittleFS.open(path, "a");
    if (!f) { _unlock(); return STORAGE_ERROR; }

    f.printf("%s\n", uid);
    f.close();
    _unlock();
    LOG_DEBUG("STORAGE", "append_uid %s → %s", uid, path);
    return STORAGE_OK;
}

// =============================================================================
//  storage_remove_uid
//  Rewrites file to a temp, skipping any line whose UID field matches.
// =============================================================================
StorageResult storage_remove_uid(const char* path, const char* uid) {
    if (!_lock()) return STORAGE_ERROR;
    if (!_has_space(128)) { _unlock(); return STORAGE_FULL; }

    File src = LittleFS.open(path,        "r");
    File dst = LittleFS.open(FILE_TX_TEMP, "w");
    if (!src || !dst) {
        if (src) src.close();
        if (dst) dst.close();
        _unlock();
        return STORAGE_ERROR;
    }

    char line[LINE_BUF];
    char file_uid[16];
    bool found = false;

    while (src.available()) {
        if (_read_line(src, line, sizeof(line)) == 0) continue;
        _extract_uid(line, file_uid, sizeof(file_uid));
        if (strcmp(file_uid, uid) == 0) { found = true; continue; }
        dst.printf("%s\n", line);
    }

    src.close();
    dst.close();
    LittleFS.remove(path);
    LittleFS.rename(FILE_TX_TEMP, path);
    _unlock();

    LOG_DEBUG("STORAGE", "remove_uid %s from %s: %s",
              uid, path, found ? "removed" : "not found");
    return found ? STORAGE_OK : STORAGE_NOT_FOUND;
}

// =============================================================================
//  storage_append_tx
//  SAD Section 2.5: format [UID],[AMOUNT],[TIMESTAMP],[DRIVER_UID]
//  Enforces 2000-line hard cap before writing.
// =============================================================================
StorageResult storage_append_tx(const char* uid, int amt,
                                unsigned long ts, const char* drv) {
    if (!_lock()) return STORAGE_ERROR;

    // Count existing lines for capacity check
    int cnt = 0;
    {
        File c = LittleFS.open(FILE_TX_LOG, "r");
        if (c) {
            char line[LINE_BUF];
            while (c.available()) {
                if (_read_line(c, line, sizeof(line)) > 0) cnt++;
            }
            c.close();
        }
    }

    if (cnt >= TX_LOG_MAX_LINES) {
        _unlock();
        LOG_WARN("STORAGE", "tx.log at hard cap (%d lines)", TX_LOG_MAX_LINES);
        return STORAGE_FULL;
    }
    if (!_has_space(128)) { _unlock(); return STORAGE_FULL; }

    File f = LittleFS.open(FILE_TX_LOG, "a");
    if (!f) { _unlock(); return STORAGE_ERROR; }

    f.printf("%s,%d,%lu,%s\n", uid, amt, ts, drv);
    f.close();
    _unlock();

    LOG_DEBUG("STORAGE", "tx appended line %d: %s,%d,%lu", cnt + 1, uid, amt, ts);
    return STORAGE_OK;
}

// =============================================================================
//  storage_count_uid_in_tx
//  SAD Section 4.1 Step 5: lookback limit check.
// =============================================================================
int storage_count_uid_in_tx(const char* uid) {
    if (!_lock()) return -1;
    File f = LittleFS.open(FILE_TX_LOG, "r");
    if (!f) { _unlock(); return 0; }

    char line[LINE_BUF];
    char file_uid[16];
    int  cnt = 0;

    while (f.available()) {
        if (_read_line(f, line, sizeof(line)) == 0) continue;
        _extract_uid(line, file_uid, sizeof(file_uid));
        if (strcmp(file_uid, uid) == 0) cnt++;
    }

    f.close();
    _unlock();
    return cnt;
}

// =============================================================================
//  storage_get_tx_line_count
// =============================================================================
int storage_get_tx_line_count() {
    if (!_lock()) return -1;
    File f = LittleFS.open(FILE_TX_LOG, "r");
    if (!f) { _unlock(); return 0; }

    char line[LINE_BUF];
    int  cnt = 0;
    while (f.available()) {
        if (_read_line(f, line, sizeof(line)) > 0) cnt++;
    }

    f.close();
    _unlock();
    return cnt;
}

// =============================================================================
//  storage_get_pin_for_uid
//  Reads drv.dat or adm.dat, finds the matching UID, copies PIN to out.
//  Format: [UID],[4-DIGIT-PIN]
// =============================================================================
StorageResult storage_get_pin_for_uid(const char* path, const char* uid,
                                      char* out, size_t sz) {
    if (!out || sz < 2) return STORAGE_ERROR;
    if (!_lock()) return STORAGE_ERROR;

    File f = LittleFS.open(path, "r");
    if (!f) { _unlock(); return STORAGE_NOT_FOUND; }

    char line[LINE_BUF];
    char file_uid[16];
    StorageResult res = STORAGE_NOT_FOUND;

    while (f.available()) {
        if (_read_line(f, line, sizeof(line)) == 0) continue;

        // Split on comma
        char* comma = strchr(line, ',');
        if (!comma) continue;

        *comma = '\0';
        strncpy(file_uid, line, sizeof(file_uid) - 1);
        file_uid[sizeof(file_uid) - 1] = '\0';

        if (strcmp(file_uid, uid) == 0) {
            strncpy(out, comma + 1, sz - 1);
            out[sz - 1] = '\0';
            res = STORAGE_FOUND;
            break;
        }
    }

    f.close();
    _unlock();
    return res;
}

// =============================================================================
//  storage_read_session  /  storage_write_session
//  SAD Section 2.2: sess.dat format [STATUS],[ACTIVE_UID]
// =============================================================================
StorageResult storage_read_session(SessionData* out) {
    if (!out) return STORAGE_ERROR;
    out->active = 0;
    strncpy(out->driver_uid, "NONE", sizeof(out->driver_uid));

    if (!_lock()) return STORAGE_ERROR;
    File f = LittleFS.open(FILE_SESSION, "r");
    if (!f) { _unlock(); return STORAGE_NOT_FOUND; }

    char line[LINE_BUF];
    int  len = _read_line(f, line, sizeof(line));
    f.close();
    _unlock();

    if (len == 0) return STORAGE_ERROR;

    char* comma = strchr(line, ',');
    if (!comma) return STORAGE_ERROR;

    *comma = '\0';
    out->active = (uint8_t)atoi(line);
    strncpy(out->driver_uid, comma + 1, sizeof(out->driver_uid) - 1);
    out->driver_uid[sizeof(out->driver_uid) - 1] = '\0';
    return STORAGE_OK;
}

StorageResult storage_write_session(uint8_t active, const char* uid) {
    if (!_lock()) return STORAGE_ERROR;
    if (!_has_space(32)) { _unlock(); return STORAGE_FULL; }

    File f = LittleFS.open(FILE_SESSION, "w");
    if (!f) { _unlock(); return STORAGE_ERROR; }

    f.printf("%d,%s\n", active, uid ? uid : "NONE");
    f.close();
    _unlock();
    return STORAGE_OK;
}

// =============================================================================
//  storage_read_sync_ts  /  storage_write_sync_ts
//  SAD Section 2.2: sync.dat stores a single Unix timestamp.
// =============================================================================
unsigned long storage_read_sync_ts() {
    if (!_lock()) return 0;
    File f = LittleFS.open(FILE_SYNC_TS, "r");
    if (!f) { _unlock(); return 0; }

    char line[LINE_BUF];
    int  len = _read_line(f, line, sizeof(line));
    f.close();
    _unlock();

    return (len > 0) ? (unsigned long)strtoul(line, nullptr, 10) : 0UL;
}

StorageResult storage_write_sync_ts(unsigned long ts) {
    if (!_lock()) return STORAGE_ERROR;
    if (!_has_space(32)) { _unlock(); return STORAGE_FULL; }

    File f = LittleFS.open(FILE_SYNC_TS, "w");
    if (!f) { _unlock(); return STORAGE_ERROR; }

    f.printf("%lu\n", ts);
    f.close();
    _unlock();
    LOG_DEBUG("STORAGE", "sync.dat = %lu", ts);
    return STORAGE_OK;
}

// =============================================================================
//  storage_get_file_size
// =============================================================================
size_t storage_get_file_size(const char* path) {
    if (!LittleFS.exists(path)) return 0;
    File f = LittleFS.open(path, "r");
    if (!f) return 0;
    size_t sz = f.size();
    f.close();
    return sz;
}

// =============================================================================
//  storage_stream_tx_chunk
//  Reads tx.log line by line into buf, prepending TERMINAL_ID in RAM.
//  SAD Section 2.5: Terminal ID must NOT be written to file —
//  it is dynamically prepended here before MQTT transmission.
//  Format: TERM_ID:UID,AMT,TS,DRV|UID,AMT,TS,DRV|...
// =============================================================================
int storage_stream_tx_chunk(char* buf, size_t bufsz, size_t* bytes_read) {
    if (!buf || !bytes_read) return 0;
    *bytes_read = 0;
    if (!_lock()) return 0;

    File f = LittleFS.open(FILE_TX_LOG, "r");
    if (!f) { _unlock(); return 0; }

    char   line[LINE_BUF];
    char   piece[TX_LINE_MAX + 32];
    int    lines = 0;
    size_t used  = 0;

    while (f.available()) {
        if (_read_line(f, line, sizeof(line)) == 0) continue;

        int plen = (lines == 0)
            ? snprintf(piece, sizeof(piece), "%s:%s",  TERMINAL_ID, line)
            : snprintf(piece, sizeof(piece), "|%s",                 line);

        if (plen <= 0 || used + (size_t)plen + 1 >= bufsz) break;

        memcpy(buf + used, piece, (size_t)plen);
        used += (size_t)plen;
        lines++;
        *bytes_read = (size_t)f.position();
    }

    f.close();
    _unlock();
    buf[used] = '\0';
    LOG_INFO("STORAGE", "tx chunk: %d lines, %zu bytes", lines, used);
    return lines;
}

// =============================================================================
//  storage_atomic_delete_sent
//  SAD Section 4.2: called ONLY after PUBACK is received.
//  Seeks past the bytes already transmitted and rewrites the remainder.
// =============================================================================
StorageResult storage_atomic_delete_sent(size_t bytes_sent) {
    if (bytes_sent == 0) return STORAGE_OK;
    if (!_lock()) return STORAGE_ERROR;
    if (!_has_space(256)) { _unlock(); return STORAGE_FULL; }

    File src = LittleFS.open(FILE_TX_LOG,   "r");
    File dst = LittleFS.open(FILE_TX_TEMP,  "w");
    if (!src || !dst) {
        if (src) src.close();
        if (dst) dst.close();
        _unlock();
        return STORAGE_ERROR;
    }

    src.seek((uint32_t)bytes_sent);
    uint8_t chunk[64];
    while (src.available()) {
        size_t n = src.read(chunk, sizeof(chunk));
        if (n) dst.write(chunk, n);
    }

    src.close();
    dst.close();
    LittleFS.remove(FILE_TX_LOG);
    LittleFS.rename(FILE_TX_TEMP, FILE_TX_LOG);

    _unlock();
    LOG_INFO("STORAGE", "atomic_delete: removed %zu sent bytes", bytes_sent);
    return STORAGE_OK;
}

// =============================================================================
//  storage_ingest_chunk
//  Parses pipe-delimited UID list from downlink and appends to a file.
//  Uses strtok on a local fixed buffer — no String, no heap.
// =============================================================================
StorageResult storage_ingest_chunk(const char* path, const char* uid_list) {
    if (!path || !uid_list) return STORAGE_ERROR;
    if (!_lock()) return STORAGE_ERROR;
    if (!_has_space(512)) { _unlock(); return STORAGE_FULL; }

    File f = LittleFS.open(path, "a");
    if (!f) { _unlock(); return STORAGE_ERROR; }

    char buf[512];
    strncpy(buf, uid_list, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int   cnt = 0;
    char* tok = strtok(buf, "|");
    while (tok) {
        // Trim leading spaces
        while (*tok == ' ') tok++;
        if (strlen(tok)) { f.printf("%s\n", tok); cnt++; }
        tok = strtok(nullptr, "|");
    }

    f.close();
    _unlock();
    LOG_INFO("STORAGE", "ingest_chunk: %d UIDs → %s", cnt, path);
    return STORAGE_OK;
}


// =============================================================================
//  storage_append_registration
//  Formats and saves an OTP registration to tx.log
// =============================================================================
StorageResult storage_append_registration(const char* uid, uint32_t otp, const char* agent) {
    // 1. Lock the hard drive so the Wi-Fi chip doesn't try to read it while we are writing
    if (!_lock()) return STORAGE_ERROR;
    
    // 2. Safety check: Do we have at least 128 bytes of free space?
    if (!_has_space(128)) { _unlock(); return STORAGE_FULL; }

    // 3. Open tx.log in "a" (append) mode
    File f = LittleFS.open(FILE_TX_LOG, "a");
    if (!f) { _unlock(); return STORAGE_ERROR; }

    // 4. Format the exact string the Node.js backend expects, and add a newline (\n)
    f.printf("PENDING_LINK:%s,%06lu,%s\n", uid, (unsigned long)otp, agent);
    
    // 5. Save, close, and unlock
    f.close();
    _unlock();
    
    LOG_DEBUG("STORAGE", "Saved OTP payload to tx.log for %s", uid);
    return STORAGE_OK;
}