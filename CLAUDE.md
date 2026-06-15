# Pump Controller — Project Rules & Architecture

## System Overview

Remote pump controller using STM32G0 (master) + Quectel EC200U modem for cloud connectivity,
and STM32F103 Blue Pill (slave) for relay control + flow meter via LoRa.

---

## Hardware

### Master — 7semi STM32G0 board
- MCU: STM32G070KBT6
- Modem: Quectel EC200U (MQTT over LTE)
- LoRa: RYLR998 on USART3 (PB8=TX, PB9=RX) — address 1
- Project: `c:\Users\admin\Documents\PlatformIO\Projects\STM32_EC200`
- Linker: `STM32G070KBTX_APP.ld`
- Uses `--specs=nano.specs` → **NO float printf**, use integer arithmetic only

### Slave — Blue Pill STM32F103
- MCU: STM32F103C6T6 (genuine ST chip, IDCODE = 0x1ba01477)
- LoRa: RYLR998 on USART2 (PA2=TX, PA3=RX) — address 2
- Relay1: PB12, Relay2: PB13
- Flow meter: YF-DN50 on PB0 (EXTI0, rising edge, pull-up)
- Debug UART: USART1 PA9 TX at 115200
- LED: PC13 (active LOW)
- Project: `c:\Users\admin\Documents\PlatformIO\Projects\STM32_BluePill_LoRa`
- Linker: `STM32F103C6TX_FLASH.ld` — ORIGIN=0x08000000, LENGTH=32K
- Upload: ST-Link (upload_protocol = stlink), NO fix_cputapid.py
- Uses `--specs=nano.specs` → **NO float printf**, use integer arithmetic only

---

## Pump IDs & Firmware

| Pump ID | Relay | Device | Firmware file |
|---------|-------|--------|---------------|
| pump01  | relay1 | 7semi board site01 | firmware.bin (PUMP_ID="01") |
| pump02  | relay2 | 7semi board site01 | firmware.bin (PUMP_ID="01") |
| pump03  | relay1 | 7semi board site02 | firmware_test.bin (PUMP_ID="03") |
| pump04  | relay2 | 7semi board site02 | firmware_test.bin (PUMP_ID="03") |

- PUMP_ID defined in `include/modem.h`
- Default (repo) PUMP_ID = "03"
- Build pump01/02: change to "01", build → firmware.bin, restore to "03"
- Push to `_pumpstarter_sync` repo → Railway OTA picks up automatically

---

## Firebase Structure

```
pumps/{pumpId}/settings     ← user-saved settings (Flutter app writes, bridge reads)
pumps/{pumpId}/status       ← live device state (firmware writes via MQTT)
pumps/{deviceId}/slave_log  ← LoRa heartbeat log (master writes)
```

### slave_log fields
```json
{
  "event": "lora_hb",
  "r3": 0,        // relay3 state (-1=unknown, 0=OFF, 1=ON)
  "r4": 0,        // relay4 state
  "rssi": -21,    // LoRa RSSI (0 = no signal)
  "snr": 9,       // LoRa SNR
  "age_s": 5,     // seconds since last heartbeat received
  "fl": 43.6,     // flow rate L/min (formatted as integer: %lu.%lu)
  "tv": 150,      // total volume litres since last Blue Pill boot
  "ts": 1234567890000
}
```

---

## Flow Meter

- Model: YF-DN50
- Formula: `Hz = 0.2 × Q` (Q in L/min)
- K-factor: **12 pulses per litre**
- 1 pulse = 83.33 mL

### Blue Pill calculation (integer only, no floats)
```c
flow_lpm_x10   = pulses * 50U;           // pulses/sec × 5 L/min × 10
flow_total_ml += pulses * 1000U / 12U;   // 83.33 mL per pulse
```

### Heartbeat format sent by Blue Pill
```
S:R1:ON|R2:OFF|FL:12.5|TV:150
```
FL = L/min with 1 decimal, TV = total litres (integer)

### tv resets on Blue Pill power cycle (RAM variable, not persistent)

---

## LoRa Configuration

- Network ID: 18
- Master address: 1
- Slave address: 2
- Band: 865000000 (865 MHz)
- CRFOP: 15

### Commands (master → slave)
- `P1:ON` / `P1:OFF` — relay1
- `P2:ON` / `P2:OFF` — relay2
- `STATUS?` — request status

### Heartbeat (slave → master every 30 seconds)
- `S:R1:ON|R2:OFF|FL:0.0|TV:0`

---

## COM Ports (Dev Machine)

| Port | Device | Use |
|------|--------|-----|
| COM43 | STM32G0 debug UART | Watch firmware debug prints |
| COM46 | Quectel Modem Port | — |
| COM47 | Quectel AP Log Port | Internal modem debug (MBEDTLS logs) |
| COM50 | Quectel AT Port | Send AT commands manually |
| COM51 | Quectel USB Serial | — |

### Test LoRa via AT commands (COM50 at 115200):
```
AT+QMTPUBEX=0,1,0,0,"pump/03/cmd",13
{"relay1":1}
```

---

## MQTT Topics

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `pump/{pumpId}/cmd` | App → device | Relay commands (relay1, relay2) |
| `pump/{pumpId}/status` | Device → Firebase | Live status (voltage, current, relay states) |
| `pump/{pumpId}/slave_log` | Device → Firebase | LoRa heartbeat data |
| `pump/{pumpId}/settings` | Bridge → device | Push settings to firmware |

---

## Firmware Watchdogs & Recovery

- **2-minute MQTT offline watchdog** — `NVIC_SystemReset()` if MQTT offline > 2 min
- **CFUN reinit** — modem reinitialized if MQTT reconnect fails repeatedly
- **AT&F on init** — clears EC200U NVRAM to restore SMS notifications
- **SMS reception** — works during MQTT offline periods (3 fixes applied)
- `mqtt_offline_since_ms` reset on CFUN reinit to avoid watchdog race

---

## Master UART Connections (STM32G0)

| UART | Pins | Connected to |
|------|------|-------------|
| USART1 | PA9=TX, PA10=RX | Quectel EC200U modem |
| USART3 | PB8=TX, PB9=RX | RYLR998 LoRa module |
| Debug  | — | Via Quectel AP log (COM47) or STM32 UART (COM43) |

---

## Settings Fields (pumps/{pumpId}/settings)

| Field | Type | Description |
|-------|------|-------------|
| `ov` | float | Over-voltage threshold (V) — relay1 only |
| `uv` | float | Under-voltage threshold (V) — relay1 only |
| `pl` | float | Phase loss threshold (V) — relay1 only |
| `uv_rst` | int | UV/PL restart delay (seconds) |
| `dry_i` | float | Dry-run current threshold (A) |
| `dry_t` | int | Dry-run trip time (seconds) |
| `dry_en` | int | Dry-run protection enabled (0/1) |
| `start_t` | int | Startup delay (seconds) |
| `hp` | int | HP rating (5 or 75 for 7.5HP) |

---

## Critical Coding Rules

1. **NO float printf** — both STM32G0 and Blue Pill use `--specs=nano.specs`.
   Using `%.1f` or `%.0f` causes `.rodata` overflow (~4KB penalty).
   Always use integer formatting: `%lu.%lu` for decimals.

2. **Blue Pill linker at 0x08000000** — no bootloader needed. Do NOT set
   `SCB->VTOR` in main.c. Do NOT use fix_cputapid.py.

3. **PUMP_ID in modem.h** — always restore to "03" after building pump01/02.

4. **Integer-only flow math** — K=12 pulses/litre for YF-DN50.

5. **Bridge forwards full JSON** — no need to modify bridge when adding new
   fields to slave_log; it passes through the complete payload.

6. **LORA_OTA_DISABLED** — LoRa OTA is disabled in current firmware.

---

## Bridge (Railway)

- Node.js service subscribed to MQTT
- Forwards all MQTT messages to Firebase in real-time
- Source: `bridge/` directory
- No changes needed when adding new telemetry fields

---

## Flutter App

- Source: `flutter_app/lib/main.dart`
- Settings page uses real-time Firebase listener (`onValue`) for multi-device sync
- `_settingsSub` listens to `pumps/{pumpId}/settings`
- `_devSub` listens to `pumps/{deviceId}/status` (Active on device card)
- HP presets: 5HP (dry_i=3.0), 7.5HP (dry_i=4.5)

---

## OTA Firmware Update

- Repo: `_pumpstarter_sync` (separate git repo)
- firmware.bin → pump01/02 (PUMP_ID=01)
- firmware_test.bin → pump03/04 (PUMP_ID=03)
- Push to GitHub → Railway service picks up and serves via HTTP
- Blue Pill has no OTA (must flash via ST-Link physically)
