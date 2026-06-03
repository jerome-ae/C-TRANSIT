#include "keypad_handler.h"
#include "../display/display.h"
#include "../logger/logger.h"
#include "../ui/ui.h" // For the buzzer
#include <Keypad.h>
#include <ctype.h>    // <-- ADDED: Needed for the toupper() function

static const byte KR=4,KC=4;
static byte s_rows[4], s_cols[4];
static char s_map[4][4];
static Keypad* s_kp=nullptr;

void keypad_init(){
    for(int i=0;i<4;i++){ s_rows[i]=(byte)KEYPAD_ROW_PINS[i]; s_cols[i]=(byte)KEYPAD_COL_PINS[i]; }
    for(int r=0;r<4;r++) for(int c=0;c<4;c++) s_map[r][c]=KEYPAD_MAP[r][c];
    s_kp=new Keypad(makeKeymap(s_map),s_rows,s_cols,KR,KC);
    s_kp->setDebounceTime(50);
    LOG_INFO("KEYPAD","4x4 matrix init");
}

// ── TEMPORARY SERIAL MONITOR BYPASS (V2) ───────────────────────────────
char keypad_get_key() {
    if (Serial.available()) {
        char incoming = Serial.read();
        
        // 1. Ignore invisible "Enter" keys
        if (incoming == '\n' || incoming == '\r') {
            return '\0'; 
        }

        // 2. Force it to uppercase (so 'a' becomes 'A')
        incoming = toupper(incoming);

        // 3. ONLY allow valid keypad characters to pass through
        if ((incoming >= '0' && incoming <= '9') || 
            (incoming >= 'A' && incoming <= 'D') || 
            incoming == '*' || incoming == '#') {
            
            return incoming;
        }
    }
    return '\0'; 
}

bool keypad_read_pin(char* buf,uint8_t len){
    if(!buf||!len||!s_kp) return false;
    uint8_t entered=0;
    memset(buf,0,len+1);
    display_show_pin_prompt(0);
    
    while(entered<len){
        char k=keypad_get_key();
        if(k){
            ui_beep_keypad(); // Make the terminal chirp!
            
            if(k=='*'){ 
                LOG_INFO("KEYPAD","PIN cancel"); 
                return false; 
            }
            
            // This ensures only numbers are added to the PIN buffer
            if(k>='0' && k<='9'){ 
                buf[entered++]=k; 
                display_show_pin_prompt(entered); 
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    buf[len]='\0';
    return true;
}