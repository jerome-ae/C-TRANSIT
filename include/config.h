#pragma once
// =============================================================================
// C-TRANSIT TERMINAL — PRODUCTION CONFIGURATION
// =============================================================================
 
// ── 1. SYSTEM IDENTITY ────────────────────────────────────────────────────────
#define TERMINAL_ID       "TERM_01"
#define FIRMWARE_VERSION  "v1.0.2L"
 
// ── 2. OTA ────────────────────────────────────────────────────────────────────
#define OTA_TIMEOUT_MS    120000UL   // 2 minutes max for firmware download

// ── 3. NETWORK MODE ───────────────────────────────────────────────────────────
#define FILE_NET_MODE     "/netmode.dat"
typedef enum {
    NET_MODE_AUTO   = 0,  // try WiFi first, fall back to GSM
    NET_MODE_WIFI   = 1,  // WiFi only
    NET_MODE_GSM    = 2   // GSM only
} NetMode; 

// ── 4. WIFI ───────────────────────────────────────────────────────────────────
#define WIFI_SSID               "Infinix SMART 8"
#define WIFI_PASS               "laplace1"
#define WIFI_CONNECT_TIMEOUT_MS 20000UL

// ── 5. GSM / SIM800L (UART2) ──────────────────────────────────────────────────
#define GSM_RX_PIN          16
#define GSM_TX_PIN          17
#define GSM_BAUD            9600
#define GSM_APN             "gloflat"
#define GSM_REG_TIMEOUT_MS  30000UL
#define GSM_MAX_RETRIES     3
#define GSM_AT_TIMEOUT_MS   5000UL
#define GSM_TCP_TIMEOUT_MS  10000UL

// ── 6. MQTT BROKER & TLS SECURITY ─────────────────────────────────────────────
#define MQTT_HOST          "c698857529f142c98dd9bf344260ab0a.s1.eu.hivemq.cloud"
#define MQTT_PORT          8883 // STRICTLY TLS PORT

#define MQTT_BROKER_USER   "c-transit" 
#define MQTT_BROKER_PASS   "B4c-Transitcuit4cu@2"

#define MQTT_CLIENT_ID     TERMINAL_ID
#define MQTT_KEEPALIVE_S   60
#define MQTT_QOS           1
#define MQTT_TOPIC_TX      "ctransit/" TERMINAL_ID "/tx"
#define MQTT_TOPIC_RX      "ctransit/" TERMINAL_ID "/rx"
#define MQTT_TOPIC_STATUS  "ctransit/" TERMINAL_ID "/status"
#define MQTT_LWT_OFFLINE   "OFFLINE"
#define MQTT_LWT_ONLINE    "ONLINE"
#define MQTT_PAYLOAD_BUF   2048 // Lever 2 requirement
#define PUBACK_TIMEOUT_MS  8000UL

// TLS Memory Guards (Lever 1 & 2)
#define TLS_MIN_HEAP_BYTES      65000UL
#define TLS_RECONNECT_DELAY_MS  5000UL

// ── 7. HARDWARE PINOUTS (Original Safe Spec) ──────────────────────────────────
#define PIN_LED_GREEN  2
#define PIN_LED_RED    4
#define PIN_BUZZER     13  
#define PIN_RFID_SS    5
#define PIN_RFID_RST   255 
#define PIN_RFID_SCK   18
#define PIN_RFID_MISO  19
#define PIN_RFID_MOSI  23

static const uint8_t KEYPAD_ROW_PINS[4] = {27, 14, 26, 25};
static const uint8_t KEYPAD_COL_PINS[4] = {32, 33, 15, 12};
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
#define FS_MOUNT_POINT  "/data"
#define FILE_WHITELIST  "/wl.dat"
#define FILE_BLACKLIST  "/bl.dat"
#define FILE_DRIVERS    "/drv.dat"
#define FILE_ADMINS     "/adm.dat"
#define FILE_TX_LOG     "/tx.log"
#define FILE_SESSION    "/sess.dat"
#define FILE_SYNC       "/sync.dat"
#define FILE_TEMP       "/temp.log"
#define FILE_SYSCFG     "/syscfg.dat"

// ── 9. TIMING & BEHAVIOUR ─────────────────────────────────────────────────────
#define SYNC_INTERVAL_MS          300000UL  // 5 minutes
#define SYNC_TIMEOUT_SECONDS      10800UL   // 3-hour kill switch
#define TX_LOG_MAX_LINES          2000
#define MAX_OFFLINE_TAPS_PER_UID  2
#define DEFAULT_FARE_AMOUNT       -200      // <-- ADDED: Default fallback fare
#define BEEP_SHORT_MS             150
#define BEEP_LONG_MS              800
#define LED_FEEDBACK_MS           2000
#define LCD_RESULT_MS             2000
#define DEBOUNCE_DELAY_MS         8000UL

// ── 10. FREERTOS ──────────────────────────────────────────────────────────────
#define CORE_APP           0
#define CORE_SYNC          1
#define PRIORITY_APP       5
#define PRIORITY_SYNC      3
#define STACK_SIZE_APP     16384 // 16KB
#define STACK_SIZE_SYNC    20480 // 20KB
