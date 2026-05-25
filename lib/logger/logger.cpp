#include "logger.h"
#include "../../include/config.h"

void logger_init() {
    Serial.begin(115200); 
    
    // Give the serial port a moment to wake up so we don't miss the first print
    delay(500); 
    
    Serial.println("\n\n==========================================");
    Serial.printf ("  C-TRANSIT TERMINAL INITIALIZING\n");
    Serial.printf ("  ID: %s  |  BUILD: %s %s\n", TERMINAL_ID, __DATE__, __TIME__);
    Serial.println("==========================================\n");
}