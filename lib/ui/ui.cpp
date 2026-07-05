#include "ui.h"
#include "logger.h"

bool ui_init(){
    pinMode(PIN_LED_GREEN, OUTPUT); digitalWrite(PIN_LED_GREEN, LOW);
    pinMode(PIN_LED_RED,   OUTPUT); digitalWrite(PIN_LED_RED,   LOW);
    pinMode(PIN_BUZZER,    OUTPUT); digitalWrite(PIN_BUZZER,    LOW);
    
    // Quick sanity check - if the pins are somehow invalid (impossible on ESP32 macro setup, but good practice)
    LOG_INFO("UI", "Pins init G=%d R=%d BZ=%d", PIN_LED_GREEN, PIN_LED_RED, PIN_BUZZER);
    return true;
}

void ui_led_green(bool on){ digitalWrite(PIN_LED_GREEN, on ? HIGH : LOW); }
void ui_led_red  (bool on){ digitalWrite(PIN_LED_RED,   on ? HIGH : LOW); }
void ui_buzzer   (bool on){ digitalWrite(PIN_BUZZER,    on ? HIGH : LOW); }
void ui_all_off()         { ui_led_green(false); ui_led_red(false); ui_buzzer(false); }

void ui_feedback_approved(){
    ui_led_green(true); ui_buzzer(true);
    vTaskDelay(pdMS_TO_TICKS(BEEP_SHORT_MS));
    ui_buzzer(false);
    vTaskDelay(pdMS_TO_TICKS(LED_FEEDBACK_MS - BEEP_SHORT_MS));
    ui_led_green(false);
}

void ui_feedback_rejected(){
    ui_led_red(true); ui_buzzer(true);
    vTaskDelay(pdMS_TO_TICKS(BEEP_LONG_MS));
    ui_buzzer(false);
    if(LED_FEEDBACK_MS > BEEP_LONG_MS) {
        vTaskDelay(pdMS_TO_TICKS(LED_FEEDBACK_MS - BEEP_LONG_MS));
    }
    ui_led_red(false);
}

void ui_feedback_otp(){
    for(int i=0; i<2; i++){
        ui_buzzer(true);  vTaskDelay(pdMS_TO_TICKS(150));
        ui_buzzer(false); vTaskDelay(pdMS_TO_TICKS(150));
    }
}

void ui_feedback_lockdown(){
    for(int i=0; i<3; i++){
        ui_led_red(true);  vTaskDelay(pdMS_TO_TICKS(200));
        ui_led_red(false); vTaskDelay(pdMS_TO_TICKS(150));
    }
}

void ui_beep_keypad(){
    ui_buzzer(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    ui_buzzer(false);
}
