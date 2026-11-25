# Simple_Remotesensing_BLEv0.1
ESP32 BLE Remotesensing Controller built with ESP-IDF (NimBLE). Features dual GATT services for real-time fire alarm notifications (READ/NOTIFY) and remote GPIO lighting control (READ/WRITE), optimized for low-memory embedded environments.
# RemoteSensing_BLE (ESP32 / ESP-IDF Bluedroid)

A hybrid **Bluetooth Low Energy (BLE) Beacon and GATT Server** firmware for the ESP32, built on **ESP-IDF** using the **Bluedroid** host stack. Designed for low-power remote sensing nodes, it broadcasts live gas/fire hazard status without requiring pairing while allowing authorized mobile clients to connect and toggle physical actuators (lighting/relays) over GATT.

## System Architecture

```text
+-------------------------------------------------------------------+

|                     ESP32 (RemoteSensing_BLE)                     |
|                                                                   |
|  [Gas/Fire Sensor] ---> set_fire_status() --+                     |
|                                             v                     |
|  [Mobile App (BLE)] <-- GAP Broadcast (4B Mfg Data: Fire | Light) |
|         |                                   ^                     |
|         +-------------> GATT Write (0xEE01) -+--> [GPIO 2 Relay]  |
+-------------------------------------------------------------------+
```

## Key Features
* **Connectionless Telemetry Broadcasting (GAP Beacon):** Packs live Fire/Gas sensor status and current Lighting state into a compact **4-byte Manufacturer-Specific Data** payload inside primary `ADV_IND` packets—readable by any nearby scanner without connection overhead.
* **Remote Actuator Control (GATT Server):** Exposes a custom 16-bit Lighting Service (`0x00EE`) with a `READ` / `WRITE` characteristic (`0xEE01`) to switch `GPIO 2` ON (`0x01`) or OFF (`0x00`) from a smartphone.
* **Power-Balanced Advertising Interval:** Configured between **500 ms (`0x0320`) and 2,000 ms (`0x0C80`)** across all three primary advertising channels (`37`, `38`, `39`) to balance low radio duty cycle with reliable alert delivery in noisy 2.4 GHz environments.
* **Automatic Re-Advertising & RAM Optimization:** Releases unused Bluetooth Classic controller memory (~30 KB) at boot and automatically resumes GAP broadcasting upon client disconnection.

## BLE Protocol Specification

### 1. GAP Advertising Payload (26 / 31 Bytes Max)
Broadcasted passively in `ADV_IND` packets under device name **`ESP32_RemoteSensing`**:

| Byte Index | Field | Value | Description |
| :--- | :--- | :--- | :--- |
| `0` | Company ID (LSB) | `0xFF` | Development / Test Company ID |
| `1` | Company ID (MSB) | `0xFF` | Development / Test Company ID |
| `2` | **Fire / Gas Status** | `0x00` / `0x01` | `0x00` = Safe, `0x01` = Hazard Detected |
| `3` | **Lighting State** | `0x00` / `0x01` | `0x00` = OFF, `0x01` = ON |

### 2. GATT Profile Table

| Service | UUID | Characteristic | UUID | Properties | Payload |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Lighting Control** | `0x00EE` | Light Switch | `0xEE01` | `READ`, `WRITE` | `1 byte` (`0x00`: OFF, `0x01`: ON) |

## Hardware & Software Requirements
* **Microcontroller:** ESP32 Development Board
* **Actuator / Indicator:** Onboard LED or external relay module on **`GPIO 2`**
* **Framework:** ESP-IDF (v4.4 / v5.x) with **Bluedroid** enabled
* **Mobile Test Client:** *nRF Connect for Mobile* or *LightBlue* (Android / iOS)

## Build & Flash Instructions

1. **Enable Bluedroid in Menuconfig:**
   ```bash
   idf.py menuconfig
   ```
   Navigate to: `Component config` → `Bluetooth` → Enable **Bluetooth** → Set Host to **Bluedroid - Dual-mode** (Classic RAM is freed programmatically in `app_main`).

2. **Compile, Flash, and Open Serial Monitor:**
   ```bash
   idf.py build flash monitor
   ```

3. **Verify Operation with nRF Connect:**
   * **Passive Scan:** Locate **`ESP32_RemoteSensing`** in the scanner list and expand **Manufacturer Data** (`0xFFFF`) to observe live `[Fire, Light]` status bytes without connecting.
   * **Active Control:** Tap **Connect**, open Service **`0x00EE`**, and write `01` (Byte Array) to Characteristic **`0xEE01`** to turn `GPIO 2` ON—notice the advertised Manufacturer Data updates immediately upon disconnect.

## Roadmap
* [ ] Integrate FreeRTOS ADC/GPIO task for MQ-series gas/smoke sensor threshold detection.
* [ ] Add BLE Secure Connections (bonding + passkey) on characteristic `0xEE01` to prevent unauthorized writes.
* [ ] Persist last-known lighting state across reboots using ESP-IDF NVS.
