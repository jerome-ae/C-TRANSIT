#pragma once
#include <Arduino.h>
#include "../../include/config.h"

bool keypad_init();
char keypad_get_key();
bool keypad_read_pin(char* pin_buf, uint8_t pin_len);
