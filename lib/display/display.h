#pragma once
#include <Arduino.h>
#include "../../include/config.h"

bool display_init();
void display_show_idle();
void display_show_2line(const char* l1,const char* l2);
void display_show_status(const char* msg);
void display_show_otp(uint32_t otp);
void display_show_pin_prompt(uint8_t entered);
void display_show_lockdown();
void display_show_syncing();
void display_show_cold_start();

// Inject upload/download animation symbols into an already-displayed row 0
// col 14 = '^' (upload) if active, ' ' otherwise
// col 15 = 'v' (download) if active, ' ' otherwise
void display_set_sync_indicators(bool upload_active, bool download_active);

void display_clear();
void display_set_backlight(bool on);