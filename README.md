# C-Transit Terminal Firmware
**ESP32-WROOM-32E · PlatformIO · Arduino + FreeRTOS · WiFiClientSecure MQTT**

Production offline-first payment terminal for campus transport.
Built from System Architecture Document (SAD) Rev 1.0.

---

## Hardware (Microscale Invoice #11840)

| Component | GPIO / Interface |
|---|---|
| ESP32-WROOM-32E 38-pin | — |
| RC522 RFID Kit | VSPI: CS=5, RST=27, SCK=18, MISO=19, MOSI=23 |
| LCD 1602 + PCF8574 I2C backpack | SDA=21, SCL=22 (addr 0x27) |
| 4×4 Hard Matrix Keypad | Rows: 13,14,26,25 · Cols: 32,33,15,12 |
| 5mm Green LED + 220 Ω | GPIO 2 |
| 5mm Red LED + 220 Ω | GPIO 4 |
| TMB12A05 Active Buzzer | GPIO 0 |
| 18650 3200 mAh + Charge Controller | → Rocker Switch → ESP32 Vin |

> ⚠️ SD card modules are **NOT used**. Storage is LittleFS on internal flash.
> ⚠️ SIM800L is replaced by **Wi-Fi** in this build. Power the ESP32 from 5 V.

---

## Project Structure

```
ctransit-firmware/
├── platformio.ini          # Build config — platform pinned to espressif32 @ 6.5.0
├── partitions.csv          # App0 1.5 MB · App1 1.5 MB · LittleFS 960 KB
├── include/
│   └── config.h            # Every pin, constant, and tunable in one place
├── src/
│   └── main.cpp            # Boot, task creation, state dispatch loop
├── lib/
│   ├── logger/             # Tagged serial macros  [RFID] [GSM] [AUTH] …
│   ├── power/              # WDT arm + brownout detection
│   ├── storage/            # All LittleFS I/O, free-space guard, atomic swap
│   ├── display/            # LCD 16×2 via I2C PCF8574
│   ├── ui/                 # Green/red LEDs + active buzzer
│   ├── rfid/               # MFRC522 VSPI driver + 8-second debounce
│   ├── keypad/             # 4×4 matrix scanner, blocking PIN collector
│   ├── auth/               # 5-step offline validation tree + staff 2FA
│   ├── transaction/        # RTC from millis offset + tx.log write
│   ├── sync/               # WiFiClientSecure MQTT, QoS1, LWT, diff sync
│   └── statemachine/       # 11-state FSM, session persistence
└── data/                   # Files uploaded to LittleFS via --target uploadfs
    ├── drv.dat             # DEADBEEF,1234  ← replace before deployment
    ├── adm.dat             # CAFEBABE,9999  ← replace before deployment
    ├── wl.dat              # Whitelisted UIDs (one per line)
    ├── bl.dat              # Blacklisted UIDs (empty at first boot)
    ├── tx.log              # Transaction queue (empty at first boot)
    ├── sess.dat            # Session state seed: 0,NONE
    └── sync.dat            # Sync timestamp seed: 0
```

---

## LittleFS Filesystem Rules

This project enforces the following rules in every file that touches the FS:

1. **LittleFS only** — SPIFFS is deprecated for frequent read/write on ESP32.
2. **`partitions.csv`** subtype column = `littlefs` (not `spiffs`).
3. **Mount call**: `LittleFS.begin(true, "/data", 10, "littlefs")`
   — the 4th argument must match the **Name** column in `partitions.csv`.
4. **`platformio.ini`**: `board_build.filesystem = littlefs`
   — do **not** add `-D ARDUINO_ESP32_LITTLEFS`; that flag is for the old
   `lorol/LittleFS_esp32` library and conflicts with the built-in.
5. Transaction writes always use append mode `"a"`.
   Files are deleted **only** after a confirmed MQTT PUBACK.
6. Free space is checked before every write (`FS_HEADROOM_BYTES = 8 KB`).

### Mount Error Diagnosis

If `LittleFS.begin()` returns false, check **all three** match:

| Where | Must be |
|---|---|
| `partitions.csv` subtype | `littlefs` |
| `LittleFS.begin()` 4th arg | `"littlefs"` |
| `platformio.ini` | `board_build.filesystem = littlefs` |

A mismatch on any one causes a silent mount failure.

---

## State Machine

```
BOOT ──► [sess.dat active?] ──► READY  (power-loss recovery)
     ──► [wl.dat empty?]   ──► COLD_SYNC
     ──► [default]         ──► OFFLINE_LOCKED

OFFLINE_LOCKED  ──[driver tap]──► DRIVER_LOGIN ──[PIN ok]──► READY
OFFLINE_LOCKED  ──[admin tap] ──► DRIVER_LOGIN ──[PIN ok]──► REGISTER_MODE

READY ──[student tap]──► PROCESSING ──[APPROVED]──► APPROVED ──► READY
                                    ──[DENIED]  ──► DENIED   ──► READY
READY ──[3-hr timeout]──► HARD_LOCKDOWN ──[sync done]──► READY
READY ──[tx.log full] ──► HARD_LOCKDOWN

REGISTER_MODE ──[0 or timeout]──► OFFLINE_LOCKED
COLD_SYNC     ──[SYNC_COMPLETE]──► OFFLINE_LOCKED
```

---

## MQTT Topics

| Topic | Direction | Purpose |
|---|---|---|
| `ctransit/TERM_01/tx` | Publish | Transaction payload flush |
| `ctransit/TERM_01/rx` | Subscribe | Differential whitelist / blacklist updates |
| `ctransit/TERM_01/status` | LWT | `ONLINE` / `OFFLINE` presence |

Uplink payload format (pipe-delimited, no JSON):
```
TERM_01:A1B2C3D4,-200,1708000500,DEADBEEF|E5F6G7H8,-200,1708000545,DEADBEEF
```

Downlink differential format:
```
ADD:BL,E5F6G7H8|REM:WL,A1B2C3D4
```

Cold-sync chunks:
```
SYS:WL,A1B2C3D4|E5F6G7H8|J9K0L1M2
SYS:SYNC_COMPLETE
```

---

## LCD Animation

The UI task runs at 500 ms. It writes two sync indicators into **row 0**:

| Column | Symbol | Meaning |
|---|---|---|
| 14 | `^` | Upload active (MQTT publish fired within last 400 ms) |
| 15 | `v` | Download active (MQTT message received within last 400 ms) |

Driven by `extern volatile uint32_t g_last_upload_ms` and
`g_last_download_ms` defined in `sync.cpp`, read in `main.cpp`.

---

## Build & Flash

### Prerequisites
```bash
pip install platformio          # or install PlatformIO IDE in VS Code
```

### 1 — Set credentials
Edit `platformio.ini`:
```ini
-D WIFI_SSID=\"YourNetworkName\"
-D WIFI_PASS=\"YourPassword\"
```

Edit `data/drv.dat` with real driver UIDs and PINs:
```
AABBCCDD,5678    # UID (8 hex chars), comma, 4-digit PIN
```

### 2 — Flash LittleFS partition (seed files)
```bash
pio run --target uploadfs
```
> Must be done **before** uploading firmware on a blank device.
> Also run this any time you edit files in the `data/` folder.

### 3 — Flash firmware
```bash
pio run --target upload
```

### 4 — Monitor serial output
```bash
pio device monitor --baud 115200
```

### 5 — Full erase (factory reset)
```bash
pio run --target erase
pio run --target uploadfs
pio run --target upload
```

---

## Debug Tags

| Tag | Module |
|---|---|
| `[MAIN]` | Boot, task creation, state dispatch |
| `[RFID]` | Card detection, debounce |
| `[AUTH]` | Validation tree, staff login |
| `[TX]` | Transaction recording, RTC |
| `[STORAGE]` | LittleFS reads/writes, free-space checks |
| `[SYNC]` | WiFi, MQTT publish/subscribe |
| `[DISPLAY]` | LCD writes |
| `[UI]` | LED + buzzer events |
| `[POWER]` | WDT, brownout, reset reason |
| `[SM]` | State machine transitions |
| `[KEYPAD]` | Key presses, PIN entry |

Disable all output for a silent production build:
```c
// include/config.h
#define DEBUG_MODE  0
```

---

## Common Failure Scenarios

| Symptom | Cause | Fix |
|---|---|---|
| `LittleFS.begin() failed` | Partition subtype / label mismatch | Check all three: partitions.csv, LittleFS.begin() 4th arg, platformio.ini filesystem |
| LCD blank after power-on | Contrast pot not adjusted | Turn PCF8574 blue pot until characters appear |
| LCD address not found | I2C addr is 0x3F, not 0x27 | Run I2C scanner, update `LCD_I2C_ADDR` in config.h |
| RFID never reads | Wrong SPI pins or bus shared | Verify VSPI: CS=5, RST=27, SCK=18, MISO=19, MOSI=23 |
| WiFi connect timeout | Wrong SSID/password | Update `WIFI_SSID` / `WIFI_PASS` in platformio.ini build_flags |
| MQTT connect fails | Wrong broker/port or TLS issue | Verify `MQTT_HOST`, `MQTT_PORT=8883`; `setInsecure()` bypasses cert for dev |
| Taps rejected after 3 h | 3-hour kill switch (sync.dat) | Confirm WiFi + MQTT syncing and PUBACK writing sync.dat |
| WDT reset on boot | Hardware init freeze | Read serial backtrace — which module is hung? |
| Driver PIN never accepted | drv.dat not on device | Run `pio run --target uploadfs` then reflash |
