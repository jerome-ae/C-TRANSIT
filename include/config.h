#pragma once
// =============================================================================
// C-TRANSIT TERMINAL — PRODUCTION CONFIGURATION
// =============================================================================

// ── 1. SYSTEM IDENTITY ────────────────────────────────────────────────────────
#define TERMINAL_ID       "TERM_01"
#define FIRMWARE_VERSION  "v4.0-DUAL"

// ── 2. OTA ────────────────────────────────────────────────────────────────────
// OTA is triggered by a downlink message: SYS:OTA,http://your-server/firmware.bin
// The ESP32 downloads and flashes to the App1 partition, then reboots.
#define OTA_TIMEOUT_MS    120000UL   // 2 minutes max for firmware download

// ── 3. NETWORK MODE ───────────────────────────────────────────────────────────
// Stored in LittleFS as /netmode.dat — "0" = Auto, "1" = WiFi only, "2" = GSM only
// Changeable at runtime via keypad sequence 'A' in PILOT mode
#define FILE_NET_MODE     "/netmode.dat"
typedef enum {
    NET_MODE_AUTO   = 0,  // try WiFi first, fall back to GSM
    NET_MODE_WIFI   = 1,  // WiFi only
    NET_MODE_GSM    = 2   // GSM only
} NetMode;

// ── 4. WIFI ───────────────────────────────────────────────────────────────────
#define WIFI_SSID              "0x1324646"
#define WIFI_PASS              "tensazangetsu12"
#define WIFI_CONNECT_TIMEOUT_MS 10000UL

// ── 5. GSM / SIM800L ──────────────────────────────────────────────────────────
#define GSM_TX_PIN          17
#define GSM_RX_PIN          16
#define GSM_BAUD            9600
#define GSM_APN             "gloflat"
#define GSM_REG_TIMEOUT_MS  30000UL
#define GSM_MAX_RETRIES     3
#define GSM_AT_TIMEOUT_MS   5000UL
#define GSM_TCP_TIMEOUT_MS  10000UL

// ── 6. MQTT BROKER ────────────────────────────────────────────────────────────
#define MQTT_HOST          "broker.hivemq.com"
#define MQTT_PORT          1883
#define MQTT_USERNAME      ""
#define MQTT_PASSWORD      ""
#define MQTT_CLIENT_ID     TERMINAL_ID
#define MQTT_KEEPALIVE_S   60
#define MQTT_QOS           1
#define MQTT_TOPIC_TX      "ctransit/" TERMINAL_ID "/tx"
#define MQTT_TOPIC_RX      "ctransit/" TERMINAL_ID "/rx"
#define MQTT_TOPIC_STATUS  "ctransit/" TERMINAL_ID "/status"
#define MQTT_LWT_OFFLINE   "OFFLINE"
#define MQTT_LWT_ONLINE    "ONLINE"
#define MQTT_PAYLOAD_BUF   4096
#define PUBACK_TIMEOUT_MS  8000UL

// ── 7. HARDWARE PINOUTS ───────────────────────────────────────────────────────
#define PIN_LED_GREEN  2
#define PIN_LED_RED    4
#define PIN_BUZZER     13
#define PIN_RFID_SS    5
#define PIN_RFID_RST   27
#define PIN_RFID_SCK   18
#define PIN_RFID_MISO  19
#define PIN_RFID_MOSI  23

static const uint8_t KEYPAD_ROW_PINS[4] = {32, 33, 15, 12};
static const uint8_t KEYPAD_COL_PINS[4] = {0,  14, 26, 25};
static const char    KEYPAD_MAP[4][4]   = {
    {'1','2','3','A'},
    {'4','5','6','B'},
    {'7','8','9','C'},
    {'*','0','#','D'}
};

#define LCD_I2C_ADDR  0x27
#define LCD_COLS      16
#define LCD_ROWS      2

// ── 8. LITTLEFS FILE PATHS ────────────────────────────────────────────────────
#define FILE_WHITELIST  "/wl.dat"
#define FILE_BLACKLIST  "/bl.dat"
#define FILE_DRIVERS    "/drv.dat"
#define FILE_ADMINS     "/adm.dat"
#define FILE_TX_LOG     "/tx.log"

// ── 9. TIMING & BEHAVIOUR ─────────────────────────────────────────────────────
#define SYNC_INTERVAL_MS          20000UL  // 5 minutes
#define SYNC_TIMEOUT_SECONDS      10800UL   // 3-hour kill switch
#define TX_LOG_MAX_LINES          2000
#define MAX_OFFLINE_TAPS_PER_UID  2
#define FARE_AMOUNT               -200
#define BEEP_SHORT_MS             150
#define BEEP_LONG_MS              800
#define LED_FEEDBACK_MS           2000
#define LCD_RESULT_MS             2000

// ── 10. FREERTOS ──────────────────────────────────────────────────────────────
#define CORE_APP        0
#define CORE_SYNC       1
#define PRIORITY_SYNC   1
#define PRIORITY_RFID   2
#define STACK_SIZE_SYNC 8192
#define STACK_SIZE_APP  8192
