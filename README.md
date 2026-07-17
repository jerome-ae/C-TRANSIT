# C-TRANSIT Terminal Firmware

ESP32-based offline-first transit terminal firmware for contactless fare validation, local rule enforcement, and resilient MQTT synchronization.

This build is designed for field deployment with a strict offline-first model: the terminal continues to operate when Wi-Fi or MQTT is unavailable, queues transactions locally in LittleFS, and syncs them later once connectivity returns.

---

## What this firmware does

- Reads RFID/UIDs and validates them against local whitelists, blacklists, and driver/admin databases.
- Operates fully offline when network is unavailable.
- Stores transactions and runtime state in LittleFS on the ESP32 flash partition.
- Uses MQTT over Wi-Fi for uplink transaction delivery, downlink rule updates, OTA triggers, and terminal identity provisioning.
- Uses a dual-core architecture:
  - Core 0: UI, RFID, keypad, display, state machine
  - Core 1: networking, MQTT, sync, OTA, LittleFS access coordination

---

## Hardware overview

| Component | Details |
|---|---|
| MCU | ESP32-WROOM-32E |
| RFID | MFRC522 over VSPI |
| Display | 16x2 LCD via I2C PCF8574 |
| Keypad | 4x4 matrix keypad |
| Status LEDs | Green and red LEDs |
| Audio | Active buzzer |
| Storage | LittleFS on internal flash |
| Network | Wi-Fi + MQTT (TLS via WiFiClientSecure) |
| Power | 5V input, battery-backed system design |

### Pin map

| Function | GPIO |
|---|---:|
| RFID CS | 5 |
| RFID RST | 255 |
| RFID SCK | 18 |
| RFID MISO | 19 |
| RFID MOSI | 23 |
| LCD SDA | 21 |
| LCD SCL | 22 |
| Keypad rows | 27, 14, 26, 25 |
| Keypad cols | 32, 33, 15, 12 |
| Green LED | 2 |
| Red LED | 4 |
| Buzzer | 13 |

---

## Architecture summary

### Core responsibilities

- [src/main.cpp](src/main.cpp)
  - Boot sequence
  - Hardware initialization
  - Terminal ID load from LittleFS
  - Task creation for Core 0 and Core 1
  - State machine coordination

- [lib/storage/storage.cpp](lib/storage/storage.cpp)
  - Mounts LittleFS
  - Reads and writes terminal identity, sync timestamps, session state, fare config, and transaction queue files
  - Implements atomic delete of sent transaction bytes from tx.log
  - Uses a mutex to protect concurrent access from both cores

- [lib/sync/sync.cpp](lib/sync/sync.cpp)
  - Connects to Wi-Fi and MQTT
  - Publishes uplink transaction payloads to the backend
  - Subscribes to downlink commands and updates
  - Handles status/LWT online/offline messaging
  - Triggers sync immediately on Wi-Fi IP acquisition
  - Uses QoS 1 and PUBACK-gated deletion for reliable delivery

- [lib/transaction/transaction.cpp](lib/transaction/transaction.cpp)
  - Records transactions locally with timestamp and fare data
  - Appends to tx.log for later MQTT delivery

- [lib/statemachine](lib/statemachine)
  - Maintains offline/online state transitions and lockdown behavior

---

## Storage model (LittleFS)

The firmware uses LittleFS as the persistent storage layer. This is not a temporary cache; it is the operating memory for device state and offline transactions.

### Files used

| File | Purpose |
|---|---|
| /wl.dat | Whitelist entries |
| /bl.dat | Blacklist entries |
| /drv.dat | Driver database |
| /adm.dat | Admin database |
| /tx.log | Local transaction queue |
| /sess.dat | Session state |
| /sync.dat | Last successful sync timestamp |
| /netmode.dat | Network mode preference |
| /syscfg.dat | Fare config and other system config |
| /term_id.dat | Persisted terminal identity |

### Important storage rules

- Files are created on first boot if missing.
- The terminal ID is loaded from LittleFS before the sync task starts.
- Transactions are appended to tx.log in a newline-delimited format.
- Sent data is removed only after the publish path confirms success and the broker acknowledgement path is satisfied.
- Storage operations are protected by a FreeRTOS mutex to prevent race conditions between Core 0 and Core 1.

---

## MQTT and sync behavior

### Topic pattern

The firmware builds topics dynamically from the persisted terminal ID:

- Uplink: ctransit/<terminalId>/tx
- Downlink: ctransit/<terminalId>/rx
- Status: ctransit/<terminalId>/status

### Connection behavior

- Wi-Fi connection happens first.
- MQTT connects with a Last Will and Testament (LWT) on the status topic.
- The firmware publishes ONLINE immediately after a successful connection.
- The firmware subscribes to the terminal-specific RX topic after connect and after terminal ID changes.
- The system reuses the same topic namespace even after OTA or identity changes.

### Reliability features

- QoS 1 publish path for important traffic
- PUBACK-gated deletion so tx.log data is only removed after successful delivery confirmation
- Immediate sync trigger on Wi-Fi IP acquisition
- Cooldown logic to avoid sync task thrashing during reconnects
- Non-retained transaction publishes, retained status/LWT messages

---

## Payload formats

### 1. Uplink transaction payload

Transactions are written to tx.log as newline-delimited records and packed into MQTT publishes as one chunk per sync cycle.

Example logical format:

```text
<terminalId>:<uid>,<fare>,<timestamp>,<driverUid>
```

Example:

```text
TERM_01:A1B2C3D4,-200,1708000500,DEADBEEF
```

The sync layer packages these records into the publish payload, using a line-based builder and fixed-size buffers.

### 2. Downlink differential updates

Downlink messages can update local rule sets or trigger control commands.

Examples:

```text
ADD:BL,E5F6G7H8
REM:WL,A1B2C3D4
```

And system commands:

```text
SYS:FARE,-250
SYS:ID,TERM_03
SYS:NET,1
SYS:SYNC_COMPLETE
SYS:OTA,http://example.com/firmware.bin
```

### 3. Status message

The firmware publishes status state to:

```text
ctransit/<terminalId>/status
```

with values:

- ONLINE
- OFFLINE

---

## Offline-first behavior

The system is intentionally designed to work without network access.

### Normal flow

1. The device boots and loads its terminal ID from LittleFS.
2. It initializes its local databases from LittleFS files.
3. It validates taps and records transactions locally.
4. If Wi-Fi is available, it syncs queued transactions to the broker.
5. If the network is down, it keeps the transaction queue intact and retries later.

### What survives reboot

- Terminal identity
- Driver/admin/whitelist/blacklist data
- Session state
- Sync timestamps
- Offline transactions in tx.log

---

## Build and deployment

### Prerequisites

```bash
pip install platformio
```

### Build firmware

```bash
pio run
```

### Build filesystem image

```bash
pio run --target buildfs
```

### Upload filesystem data

```bash
pio run --target uploadfs
```

### Upload firmware

```bash
pio run --target upload
```

### Monitor serial

```bash
pio device monitor --baud 115200
```

---

## Production notes

- The firmware is configured for a production debug profile with reduced verbosity.
- MQTT packet sizes and network buffers are sized for constrained ESP32 memory.
- The code avoids dynamic String allocation in the Core 1 networking path.
- The project uses LittleFS with a partition layout that matches the bootloader-compatible partition mapping currently used by the firmware.

---

## Commit summary for the next change

This update hardens the ESP32 transit terminal firmware for deployment by completing the offline-first sync path, improving MQTT reliability, and making persistence behavior consistent across boot, reconnect, and OTA-safe identity handling. The firmware now loads the terminal ID from LittleFS before the sync task starts, maintains a stable runtime topic namespace for uplink/downlink/status traffic, re-subscribes after reconnects and identity changes, and uses QoS 1 plus PUBACK-gated deletion so transactions are only removed from tx.log after successful broker acknowledgement. Storage is fully LittleFS-backed, payloads are built line-by-line from tx.log without byte-slicing transactions, and the code avoids dynamic String allocations in the network path to stay within the ESP32 memory constraints.


NOTE:
BACKEND LOAD
If payload == "SYS:TIME_REQ":
    publish to ctransit/{device_id}/rx: "SYS:TIME,<current_epoch>"