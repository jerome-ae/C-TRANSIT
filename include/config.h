#pragma once
// =============================================================================
// C-TRANSIT TERMINAL — PRODUCTION CONFIGURATION
// Single Source of Truth for all hardware and cloud constants.
// =============================================================================

// ── 1. SYSTEM IDENTITY ────────────────────────────────────────────────────────
#define TERMINAL_ID       "TERM_01"
#define FIRMWARE_VERSION  "v3.0-GSM"

// ── 2. GSM / SIM800L ──────────────────────────────────────────────────────────
#define GSM_TX_PIN        17          // ESP32 TX → SIM800L RX
#define GSM_RX_PIN        16          // ESP32 RX → SIM800L TX
#define GSM_BAUD          9600
#define GSM_APN           "gloflat"  // ← set to your carrier APN
                                      //   MTN Nigeria:    "internet"
                                      //   Airtel Nigeria: "airtelgprs.com"
                                      //   Glo Nigeria:    "gloflat"

// Network registration timeout — SIM800L can take up to 30s on cold boot
#define GSM_REG_TIMEOUT_MS    30000UL
// Max AT command retries before giving up and sleeping modem
#define GSM_MAX_RETRIES       3
// How long to wait for each AT response
#define GSM_AT_TIMEOUT_MS     5000UL
// How long to wait after AT+CIPSTART before TCP is ready
#define GSM_TCP_TIMEOUT_MS    10000UL

// ── 3. MQTT BROKER ────────────────────────────────────────────────────────────
// HiveMQ Cloud — port 1883 (plain TCP via SIM800L)
// Note: SIM800L does NOT support TLS natively.
// For TLS you would need an external SSL co-processor.
// Use a broker that allows plain MQTT on port 1883, or set up a local bridge.
#define MQTT_HOST      "broker.hivemq.com"
#define MQTT_PORT      1883
#define MQTT_USERNAME  ""   // leave blank
#define MQTT_PASSWORD  ""   // leave blank
#define MQTT_CLIENT_ID     TERMINAL_ID
#define MQTT_KEEPALIVE_S   60
#define MQTT_QOS           1

// Cloud Topics
#define MQTT_TOPIC_TX      "ctransit/" TERMINAL_ID "/tx"
#define MQTT_TOPIC_RX      "ctransit/" TERMINAL_ID "/rx"
#define MQTT_TOPIC_STATUS  "ctransit/" TERMINAL_ID "/status"

#define MQTT_LWT_OFFLINE   "OFFLINE"
#define MQTT_LWT_ONLINE    "ONLINE"
#define MQTT_PAYLOAD_BUF   4096

// PUBACK wait — how long Core 1 waits for broker acknowledgement before
// considering the publish failed (tx.log is NOT wiped on failure)
#define PUBACK_TIMEOUT_MS  8000UL

// ── 4. HARDWARE PINOUTS ───────────────────────────────────────────────────────
// UI (LEDs & Buzzer)
#define PIN_LED_GREEN  2
#define PIN_LED_RED    4
#define PIN_BUZZER     13

// RFID Scanner (VSPI — isolated, no sharing — SAD Section 1.1)
#define PIN_RFID_SS    5
#define PIN_RFID_RST   27
#define PIN_RFID_SCK   18
#define PIN_RFID_MISO  19
#define PIN_RFID_MOSI  23

// 4×4 Keypad
static const uint8_t KEYPAD_ROW_PINS[4] = {32, 33, 15, 12};
static const uint8_t KEYPAD_COL_PINS[4] = {0,  14, 26, 25};
static const char    KEYPAD_MAP[4][4]   = {
    {'1','2','3','A'},
    {'4','5','6','B'},
    {'7','8','9','C'},
    {'*','0','#','D'}
};

// LCD Display (I2C via PCF8574 backpack)
#define LCD_I2C_ADDR  0x27
#define LCD_COLS      16
#define LCD_ROWS      2

// ── 5. LITTLEFS FILE PATHS  (SAD Section 2.2–2.5) ────────────────────────────
#define FILE_WHITELIST  "/wl.dat"
#define FILE_BLACKLIST  "/bl.dat"
#define FILE_DRIVERS    "/drv.dat"
#define FILE_ADMINS     "/adm.dat"
#define FILE_TX_LOG     "/tx.log"

// ── 6. TIMING & SYSTEM BEHAVIOUR ─────────────────────────────────────────────
#define SYNC_INTERVAL_MS          300000UL  // sync every 5 minutes when idle
#define SYNC_TIMEOUT_SECONDS      10800UL   // 3-hour kill switch (SAD Section 2.2)
#define TX_LOG_MAX_LINES          2000      // hard cap (SAD Section 2.5)
#define MAX_OFFLINE_TAPS_PER_UID  2
#define FARE_AMOUNT               -200

#define BEEP_SHORT_MS    150
#define BEEP_LONG_MS     800
#define LED_FEEDBACK_MS  2000
#define LCD_RESULT_MS    2000

// ── 7. FREERTOS TASK ALLOCATION (SAD Section 1.2) ────────────────────────────
#define CORE_APP        0   // Core 0 — UI, RFID, Keypad, State Machine
#define CORE_SYNC       1   // Core 1 — GSM UART, MQTT, File sync
#define PRIORITY_SYNC   1
#define PRIORITY_RFID   2
#define STACK_SIZE_SYNC 8192
#define STACK_SIZE_APP  8192
