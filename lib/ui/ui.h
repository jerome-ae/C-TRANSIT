#pragma once
#include <Arduino.h>
#include "../../include/config.h"

bool ui_init();
void ui_feedback_approved();
void ui_feedback_rejected();
void ui_feedback_otp();
void ui_feedback_lockdown();
void ui_beep_keypad(); 
void ui_led_green(bool on);
void ui_led_red(bool on);
void ui_buzzer(bool on);
void ui_all_off();
