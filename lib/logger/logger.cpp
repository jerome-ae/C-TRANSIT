#include "logger.h"
#include "../../include/config.h"

// g_terminal_id is defined in main.cpp and loaded from LittleFS at boot
extern char g_terminal_id[];

bool logger_init() {
    Serial.begin(115200); 
    
    // Non-blocking wait for serial port initialization
    vTaskDelay(pdMS_TO_TICKS(500)); 
    
    // Check if the serial peripheral successfully attached
    if (!Serial) {
        return false;
    }
    
    Serial.println("\n\n==========================================");
    Serial.printf ("  C-TRANSIT TERMINAL INITIALIZING\n");
    Serial.printf ("  ID: %s  |  BUILD: %s %s\n", g_terminal_id, __DATE__, __TIME__);
    Serial.println("==========================================\n");

    return true;
}
