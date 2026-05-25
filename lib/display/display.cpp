#include "display.h"
#include "logger.h"
#include <LiquidCrystal_I2C.h>
#include <Wire.h>

// <-- ADDED: Definitions for the arrow injection positions
#define LCD_COL_UPLOAD   14
#define LCD_COL_DOWNLOAD 15

static LiquidCrystal_I2C s_lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);

static void _pad(uint8_t row, const char* text){
    s_lcd.setCursor(0,row);
    char b[LCD_COLS+1];
    snprintf(b,sizeof(b),"%-*.*s",LCD_COLS,LCD_COLS,text?text:"");
    s_lcd.print(b);
}

bool display_init(){
    Wire.begin(21,22);
    s_lcd.init();
    s_lcd.backlight();
    Wire.beginTransmission(LCD_I2C_ADDR);
    if(Wire.endTransmission()!=0){
        LOG_ERROR("DISPLAY","LCD not found at 0x%02X",LCD_I2C_ADDR);
        return false;
    }
    s_lcd.clear();
    LOG_INFO("DISPLAY","LCD OK at 0x%02X",LCD_I2C_ADDR);
    return true;
}

void display_show_idle(){ _pad(0,"  C-TRANSIT   "); _pad(1," >> Tap to Ride"); }
void display_show_status(const char* m){ _pad(0,"  C-TRANSIT   "); _pad(1,m); }
void display_show_2line(const char* l1,const char* l2){ _pad(0,l1); _pad(1,l2); }

void display_show_otp(uint32_t otp){
    char b[17]; snprintf(b,sizeof(b),"    %06lu    ",(unsigned long)otp);
    _pad(0,"   OTP CODE:  "); _pad(1,b);
}

void display_show_pin_prompt(uint8_t entered){
    char m[5]={0};
    for(int i=0;i<4;i++) m[i]=(i<entered)?'*':'_';
    char b[17]; snprintf(b,sizeof(b)," PIN: %4s     ",m);
    _pad(0," STAFF LOGIN  "); _pad(1,b);
}

void display_show_lockdown(){ _pad(0,"  !! LOCKED !!");  _pad(1,"  SYNC NEEDED "); }
void display_show_syncing()  { _pad(0,"  SYNCING...  "); _pad(1,"  Please Wait "); }
void display_show_cold_start(){ _pad(0," SYSTEM BLANK "); _pad(1," REQUESTING.. "); }

// Write sync indicators at fixed positions on row 0 without re-drawing the row
void display_set_sync_indicators(bool upload_active, bool download_active){
    s_lcd.setCursor(LCD_COL_UPLOAD,  0);
    s_lcd.print(upload_active   ? '^' : ' ');
    s_lcd.setCursor(LCD_COL_DOWNLOAD, 0);
    s_lcd.print(download_active ? 'v' : ' ');
}

void display_clear(){ s_lcd.clear(); }
void display_set_backlight(bool on){ on?s_lcd.backlight():s_lcd.noBacklight(); }