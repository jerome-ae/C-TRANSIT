#include "keypad_handler.h"
#include "../display/display.h"
#include "../logger/logger.h"
#include "../ui/ui.h"
#include "../power/power.h" // Needed to feed the WDT during blocking reads
#include <Keypad.h>
#include <ctype.h>

static byte s_rows[4], s_cols[4];
static char s_map[4][4];

// Pre-allocate the keypad object memory statically instead of using "new"
static Keypad s_kp = Keypad(makeKeymap(s_map), s_rows, s_cols, 4, 4);
static bool s_is_initialized = false;

bool keypad_init(){
    for(int i=0; i<4; i++){ 
        s_rows[i] = (byte)KEYPAD_ROW_PINS[i]; 
        s_cols[i] = (byte)KEYPAD_COL_PINS[i]; 
    }
    for(int r=0; r<4; r++) {
        for(int c=0; c<4; c++) {
            s_map[r][c] = KEYPAD_MAP[r][c];
        }
    }
    
    // We must rebuild the Keypad instance since we updated the arrays
    s_kp = Keypad(makeKeymap(s_map), s_rows, s_cols, 4, 4);
    s_kp.setDebounceTime(50);
    
    s_is_initialized = true;
    LOG_INFO("KEYPAD", "4x4 matrix initialized");
    return true;
}

char keypad_get_key() {
    if (!s_is_initialized) {
        LOG_ERROR("KEYPAD", "CRITICAL: keypad_init() was not called.");
        return '\0';
    }

    char k = s_kp.getKey();
    
    if (k != NO_KEY) {
        LOG_INFO("KEYPAD", "Hardware Key Pressed: %c", k);
        return k;
    }
    
    return '\0'; 
}

bool keypad_read_pin(char* buf, uint8_t len){
    if(!buf || !len || !s_is_initialized) return false;
    
    uint8_t entered = 0;
    memset(buf, 0, len+1);
    display_show_pin_prompt(0);
    
    while(entered < len){
        // CRITICAL: Feed the watchdog so we don't crash if the user takes >15 seconds to type!
        power_feed_watchdog();
        
        char k = keypad_get_key();
        if(k){
            ui_beep_keypad(); 
            
            if(k == '*'){ 
                LOG_INFO("KEYPAD", "PIN cancel"); 
                return false; 
            }
            
            if(k >= '0' && k <= '9'){ 
                buf[entered++] = k; 
                display_show_pin_prompt(entered); 
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    buf[len] = '\0';
    return true;
}
