#pragma once
// =============================================================================
// storage.h  —  All LittleFS I/O.
// =============================================================================
#include <Arduino.h>
#include <LittleFS.h>
#include "../../include/config.h"

typedef enum {
    STORAGE_OK = 0, 
    STORAGE_NOT_FOUND = 1, 
    STORAGE_FOUND = 2,
    STORAGE_FULL = 3, 
    STORAGE_ERROR = -1
} StorageResult;

typedef struct {
    uint8_t active;
    char    driver_uid[9];
} SessionData;

// -----------------------------------------------------------------------------
// NOTE: All functions below are strictly thread-safe. 
// They are protected by an internal FreeRTOS Mutex to prevent Core 0 and 
// Core 1 from colliding during simultaneous file access.
// -----------------------------------------------------------------------------

bool          storage_init();

// File checks and line counting (Line-by-line buffered, RAM safe)
StorageResult storage_uid_in_file(const char* path, const char* uid);
int           storage_count_uid_in_tx(const char* uid);
int           storage_get_tx_line_count();
size_t        storage_get_file_size(const char* path);
StorageResult storage_get_pin_for_uid(const char* path, const char* uid, char* out, size_t sz);

// Appends and removals (Protected by 8KB minimum free space guard)
StorageResult storage_append_uid(const char* path, const char* uid);
StorageResult storage_remove_uid(const char* path, const char* uid);
StorageResult storage_append_tx(const char* uid, int amt, unsigned long ts, const char* drv);
StorageResult storage_append_registration(const char* uid, uint32_t otp, const char* agent);

// State & Config management (Overwritten files using "w")
StorageResult storage_read_session(SessionData* out);
StorageResult storage_write_session(uint8_t active, const char* drv_uid);
unsigned long storage_read_sync_ts();
StorageResult storage_write_sync_ts(unsigned long ts);

int           storage_read_fare();                     // <-- NEW
StorageResult storage_write_fare(int fare_amount);     // <-- NEW

// Network Sync specific functions
int           storage_stream_tx_chunk(char* buf, size_t bufsz, size_t* bytes_read);
StorageResult storage_atomic_delete_sent(size_t bytes_sent);
StorageResult storage_ingest_chunk(const char* path, const char* uid_list);
