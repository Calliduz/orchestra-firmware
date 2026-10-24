# ORCHESTRA Edge Firmware: ESP32 Doorway Ingress Terminal

**Orchestrated RFID Classroom Attendance Framework with Hardware-Enforced Session Trust and Roster Automation**  
*College of Computer Studies — Department of Computer Science & Information Technology*  
*Authors: Agbada, Altichie, Cabunoc, Dalupang, Macadat, Malate (October 2026)*

---

## 1. Hardware Architecture & Bill of Materials (BOM)

| Component | Specification | Protocol | ESP32 GPIO Pins | Power |
|---|---|---|---|---|
| **Microcontroller** | ESP32-WROOM-32 / NodeMCU-32S (Dual-Core 240MHz, 4MB Flash, 520KB SRAM) | Wi-Fi 802.11 b/g/n, SPI, I2C | — | 5V USB / 3.3V Regulated |
| **RFID Reader** | MFRC522 13.56 MHz Transceiver (ISO/IEC 14443A MIFARE Classic / Ultralight) | SPI Bus | `SS: 5`, `SCK: 18`, `MOSI: 23`, `MISO: 19`, `RST: 16` | **3.3V Only** (5V damages chip) |
| **Graphic OLED** | SSD1306 0.96" Monochrome Display (128x64 pixels, I2C Address `0x3C`) | Hardware I2C | `SDA: 21`, `SCL: 22` | 3.3V |
| **Matrix Keypad** | 4x4 Numeric/Command Keypad (Rows 0-3, Cols 0-3) | GPIO Matrix Scanning | Rows: `13, 12, 14, 27`<br/>Cols: `26, 25, 33, 32` | 3.3V Internal Pullups |
| **Green LED** | 5mm High-Efficiency Green LED (Access Granted indicator) | GPIO Digital Out | `GPIO 2` (via 220Ω resistor) | 3.3V |
| **Red LED** | 5mm High-Efficiency Red LED (Locked / Denied indicator) | GPIO Digital Out | `GPIO 4` (via 220Ω resistor) | 3.3V |
| **Active Buzzer** | 5V / 3.3V Piezo Buzzer (Multi-tone auditory cues) | PWM / Tone Output | `GPIO 15` (via NPN transistor or direct 3.3V) | 3.3V / 5V |

---

## 2. Exhaustive Pin Interconnection Table

```
+-----------------------------------------------------------------------------------------+
| ESP32-WROOM-32 PIN     PERIPHERAL PIN           PERIPHERAL FUNCTION                     |
+-----------------------------------------------------------------------------------------+
| 3.3V                   MFRC522 VCC, SSD1306 VCC 3.3V Power Rail (DO NOT CONNECT TO 5V!) |
| GND                    Common Ground            Ground Plane for all peripherals        |
| GPIO 5                 MFRC522 SDA / SS         SPI Chip Select                         |
| GPIO 18                MFRC522 SCK              SPI Bus Clock                           |
| GPIO 23                MFRC522 MOSI             SPI Master Out Slave In                 |
| GPIO 19                MFRC522 MISO             SPI Master In Slave Out                 |
| GPIO 16                MFRC522 RST              Hardware Transceiver Reset              |
| GPIO 21                SSD1306 SDA              I2C Serial Data                         |
| GPIO 22                SSD1306 SCL              I2C Serial Clock                        |
| GPIO 2                 Green LED Anode          Access Granted / Session Active         |
| GPIO 4                 Red LED Anode            Session Locked / Access Denied          |
| GPIO 15                Active Buzzer (+)        Auditory Status Cues                    |
| GPIO 13                Keypad Pin 1 (Row 0)     Matrix Scanning                         |
| GPIO 12                Keypad Pin 2 (Row 1)     Matrix Scanning                         |
| GPIO 14                Keypad Pin 3 (Row 2)     Matrix Scanning                         |
| GPIO 27                Keypad Pin 4 (Row 3)     Matrix Scanning                         |
| GPIO 26                Keypad Pin 5 (Col 0)     Matrix Scanning                         |
| GPIO 25                Keypad Pin 6 (Col 1)     Matrix Scanning                         |
| GPIO 33                Keypad Pin 7 (Col 2)     Matrix Scanning                         |
| GPIO 32                Keypad Pin 8 (Col 3)     Matrix Scanning                         |
+-----------------------------------------------------------------------------------------+
```

---

## 3. Firmware State Machine & Architectural Guarantees

```
                   +---------------------------+
                   |        STATE_BOOT         |
                   | Initialize SPI, I2C, WiFi |
                   +-------------+-------------+
                                 |
                                 v
+-----------------------------------------------------------------+
|                       STATE_LOCKED                              |
| - Solid RED LED ON                                              |
| - SSD1306: "TERMINAL LOCKED / SCAN MASTER BADGE"                |
| - Student RFID card tap returns 403 SESSION_LOCKED              |
+--------------------------------+--------------------------------+
                                 |
      Faculty Master Badge Tap   |   Faculty Master Badge Tap
      (0xA1B2C3D4 or 0xF4E3D2C1) |   (Second scan closes session)
                                 v
+-----------------------------------------------------------------+
|                     STATE_IDLE_SCAN                             |
| - Solid GREEN LED ON                                            |
| - SSD1306: "AR101-BSIT3A / READY TO SCAN / # FOR OTP"           |
| - Valid card tap triggers 200 OK -> ACCESS_GRANTED              |
+-------------------+-----------------------------+---------------+
                    |                             |
 Student Card Tap   |                             | Keypad '#' Pressed
                    v                             v
+---------------------------+     +-------------------------------+
|   STATE_PACING_COOLDOWN   |     |       STATE_OTP_ENTRY         |
| - 3000ms millis() Hold    |     | - Inactivity TTL: 15 seconds  |
| - OLED countdown progress |     | - Buffer: 6-digit numeric PIN |
| - Rejects proxy-swiping   |     | - '#' to submit, '*' to clear |
+---------------------------+     +-------------------------------+
```

### 3.1 Non-Blocking Firmware Pacing Interlock
To disrupt batch-swiping and proxy attendance, the ESP32 firmware enforces a strict **3.0-second (`3000ms`) pacing window** between consecutive student card scans via embedded `millis()` comparisons:
```cpp
unsigned long elapsed = millis() - lastStudentScanTime;
if (elapsed < PACING_COOLDOWN_MS) {
    // Reader locked, reject card, trigger audio warning chirp
}
```
Because the interlock is non-blocking, the SSD1306 OLED, Wi-Fi background stack, and matrix keypad continue processing smoothly without freezing.

### 3.2 Keypad OTP Fallback Channel
If a student forgets their physical RFID badge:
1. The student generates an ephemeral 60-second PIN on their mobile device via `POST /api/v1/otp/request`.
2. The student presses `#` on the terminal's 4x4 matrix keypad.
3. The SSD1306 OLED switches to `KEYPAD OTP INGRESS` mode with visual slot markers (`[ * * * _ _ _ ]`).
4. Entering 6 digits and pressing `#` dispatches `POST /api/v1/hardware/otp`.
5. Upon verification, the terminal emits a high chime, pulses the Green LED, and logs the attendance method as `KEYPAD_OTP`.

---

## 4. PlatformIO Compilation & Flashing Instructions

### Prerequisites
- [VS Code with PlatformIO IDE Extension](https://platformio.org/) or the PlatformIO Core CLI:
  ```bash
  pip install -U platformio
  ```

### Build & Flash via Command Line
1. Navigate to the firmware directory:
   ```bash
   cd firmware
   ```

2. Compile the firmware:
   ```bash
   pio run
   ```

3. Flash to the ESP32 (connect board via micro-USB / USB-C):
   ```bash
   pio run --target upload
   ```

4. Launch the serial monitor at 115200 baud to view real-time diagnostics:
   ```bash
   pio device monitor --baud 115200
   ```
