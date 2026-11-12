#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// =========================================================================
// NETWORK & CLOUD BACKEND CONFIGURATION
// =========================================================================

// Structure for multi-network Wi-Fi provisioning
struct WifiCredential {
    const char* ssid;
    const char* password;
};

// Configured Wi-Fi Access Points (auto-connects to the strongest available network)
const WifiCredential WIFI_NETWORKS[] = {
    {"Copper",             "20220201"},
    {"Copper 5G",          "20220201"},
    {"Compound 1",         "12032005"},
    {"HatdogCheesedog",    "12032005@+"},
    {"WirelessNet",        "12032005@+"},
    {"WirelessNet-5G",     "12032005@+"},
    {"iPhone",             "cheesedog1203"},
    {"Iphone",             "20220201"},
    {"GodCalli",           "notcalli"},
    {"Jisoo",              "00000000"},
    {"Lucky 8",            "D@lupang000"},
    {"IK603B PC (1)",      "QCU.Lab603#"},
    {"ORCHESTRA_LAB_WIFI", "SecureLabPass2026"}
};
const size_t WIFI_NETWORK_COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);

// Legacy / fallback defines
#define WIFI_SSID             WIFI_NETWORKS[0].ssid
#define WIFI_PASSWORD         WIFI_NETWORKS[0].password

// Local IP or domain of Orchestra Node.js backend server
#define BACKEND_BASE_URL      "http://192.168.100.100:5000/api/v1"

// Hardware Identity & Security Bearer Token
#define TERMINAL_ID           "TERM-CL304"
#define HARDWARE_API_KEY      "ORCHESTRA_ESP32_SECURE_TOKEN_2026"
#define ROOM_NUMBER           "LAB-304"
#define DEFAULT_SECTION       "AR101-BSIT3A"

// NTP Time Configuration (UTC+8 for Philippines / Asia Manila)
#define NTP_SERVER_1          "pool.ntp.org"
#define NTP_SERVER_2          "time.nist.gov"
#define GMT_OFFSET_SEC        (8 * 3600)
#define DAYLIGHT_OFFSET_SEC   0

// =========================================================================
// HARDWARE PIN DEFINITIONS (ESP32-WROOM-32 / NodeMCU-32S)
// =========================================================================

// SPI Bus: MFRC522 13.56 MHz RFID Reader
#define RC522_SS_PIN          5    // SPI Chip Select (SDA/SS)
#define RC522_RST_PIN         16   // Hardware Reset Pin
#define RC522_SCK_PIN         18   // SPI Clock
#define RC522_MISO_PIN        19   // SPI Master In Slave Out
#define RC522_MOSI_PIN        23   // SPI Master Out Slave In

// I2C Bus: SSD1306 0.96" Monochrome OLED (128x64)
#define OLED_SDA_PIN          21   // Hardware I2C Data
#define OLED_SCL_PIN          22   // Hardware I2C Clock
#define OLED_I2C_ADDR         0x3C // Default SSD1306 I2C Address
#define SCREEN_WIDTH          128  // Display width in pixels
#define SCREEN_HEIGHT         64   // Display height in pixels
#define OLED_RESET_PIN        -1   // Shared with ESP32 reset

// Visual & Auditory Cues
#define LED_GREEN_PIN         2    // Access Granted / Active indicator
#define LED_RED_PIN           4    // Locked / Access Denied indicator
#define BUZZER_PIN            15   // Piezo Buzzer PWM Output

// 4x4 Matrix Keypad GPIO Mapping
#define KEYPAD_ROWS           4
#define KEYPAD_COLS           4
const byte KEYPAD_ROW_PINS[KEYPAD_ROWS] = {13, 12, 14, 27};
const byte KEYPAD_COL_PINS[KEYPAD_COLS] = {26, 25, 33, 32};

// =========================================================================
// TIMING CONSTANTS & HARDWARE INTERLOCK THRESHOLDS
// =========================================================================

// Multi-Tier Anti-Proxy: Non-blocking 3.0s cooldown between sequential card reads
#define PACING_COOLDOWN_MS            3000

// Inactivity timeout for Keypad 6-Digit OTP entry mode
#define KEYPAD_OTP_TIMEOUT_MS         15000

// Duration to display result splash screen (Access Granted / Denied)
#define RESULT_DISPLAY_DURATION_MS    2500

// Wi-Fi Non-blocking reconnection retry interval
#define WIFI_RECONNECT_INTERVAL_MS    10000

// Serial Telemetry Baud Rate
#define SERIAL_BAUD_RATE              115200

#endif // CONFIG_H
