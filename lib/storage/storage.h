#pragma once
// =============================================================================
// storage.h  —  All LittleFS I/O.
// =============================================================================
#include <Arduino.h>
#include <LittleFS.h>
#include "../../include/config.h" // <-- FIXED: Relative path

typedef enum {
    STORAGE_OK=0, STORAGE_NOT_FOUND=1, STORAGE_FOUND=2,
    STORAGE_FULL=3, STORAGE_ERROR=-1
} StorageResult;

typedef struct {
    uint8_t active;
    char    driver_uid[9];
} SessionData;

bool          storage_init();
StorageResult storage_uid_in_file(const char* path, const char* uid);
StorageResult storage_append_uid(const char* path, const char* uid);
StorageResult storage_remove_uid(const char* path, const char* uid);
StorageResult storage_append_tx(const char* uid,int amt,unsigned long ts,const char* drv);
int           storage_count_uid_in_tx(const char* uid);
int           storage_get_tx_line_count();
StorageResult storage_get_pin_for_uid(const char* path,const char* uid,char* out,size_t sz);
StorageResult storage_read_session(SessionData* out);
StorageResult storage_write_session(uint8_t active,const char* drv_uid);
unsigned long storage_read_sync_ts();
StorageResult storage_write_sync_ts(unsigned long ts);
size_t        storage_get_file_size(const char* path);
int           storage_stream_tx_chunk(char* buf,size_t bufsz,size_t* bytes_read);
StorageResult storage_atomic_delete_sent(size_t bytes_sent);
StorageResult storage_ingest_chunk(const char* path,const char* uid_list);
StorageResult storage_append_registration(const char* uid, uint32_t otp, const char* agent);