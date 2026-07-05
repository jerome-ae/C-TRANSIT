#pragma once
#include <Arduino.h>

bool logger_init();

// Automatically injects the millis() timestamp for precise network latency tracking
#define LOG_INFO(cat, fmt, ...)  Serial.printf("[%08lu] [INFO ] [%-5s] " fmt "\n", millis(), cat, ##__VA_ARGS__)
#define LOG_WARN(cat, fmt, ...)  Serial.printf("[%08lu] [WARN ] [%-5s] " fmt "\n", millis(), cat, ##__VA_ARGS__)
#define LOG_ERROR(cat, fmt, ...) Serial.printf("[%08lu] [ERROR] [%-5s] " fmt "\n", millis(), cat, ##__VA_ARGS__)
#define LOG_DEBUG(cat, fmt, ...) Serial.printf("[%08lu] [DEBUG] [%-5s] " fmt "\n", millis(), cat, ##__VA_ARGS__)
