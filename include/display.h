#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "config.h"

class DisplayManager {
private:
    Adafruit_SSD1306 display;

public:
    DisplayManager() : display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET_PIN) {}

    bool begin() {
        Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
        if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
            Serial.println(F("[ERR_I2C] SSD1306 allocation failed. Check wiring on GPIO 21 (SDA) / 22 (SCL)."));
            return false;
        }

        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        display.display();
        return true;
    }

    void showBootScreen(const char* version = "v1.0.4") {
        display.clearDisplay();
        
        // Outer mechanical frame
        display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
        
        display.setTextSize(1);
        display.setCursor(18, 8);
        display.print(F("=== ORCHESTRA ==="));
        
        display.setCursor(14, 22);
        display.print(F("HARDWARE TRUST"));
        
        display.setCursor(20, 36);
        display.print(F("ESP32 IoT Node"));
        
        display.setCursor(10, 48);
        display.print(F("Firmware: "));
        display.print(version);

        display.display();
    }

    void showConnectingWiFi(const char* ssid) {
        display.clearDisplay();
        display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);

        display.setTextSize(1);
        display.setCursor(16, 12);
        display.print(F("NETWORK UPLINK"));
        display.drawLine(4, 24, 124, 24, SSD1306_WHITE);

        display.setCursor(8, 30);
        display.print(F("Connecting to:"));
        display.setCursor(8, 42);
        display.print(ssid);

        display.display();
    }

    void showLockedScreen(const char* terminalId, const char* room) {
        display.clearDisplay();

        // Top Status Header Strip
        display.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(4, 2);
        display.print(F("SESSION TRUST: LOCKED"));

        // Main Center Callout
        display.setTextColor(SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(12, 20);
        display.print(F("[ TERMINAL LOCKED ]"));

        display.setCursor(8, 34);
        display.print(F("SCAN FACULTY BADGE"));

        // Bottom Metadata Strip
        display.drawLine(0, 48, SCREEN_WIDTH, 48, SSD1306_WHITE);
        display.setCursor(4, 52);
        display.print(room);
        display.print(F(" | "));
        display.print(terminalId);

        display.display();
    }

    void showIdleScanScreen(const char* sectionCode, int count, bool wifiOk) {
        display.clearDisplay();

        // Top Header
        display.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(4, 2);
        display.print(sectionCode);

        // Wi-Fi Status Flag
        display.setCursor(94, 2);
        display.print(wifiOk ? F("[NET]") : F("[!]"));

        display.setTextColor(SSD1306_WHITE);

        // Main Instructions
        display.setCursor(16, 20);
        display.print(F(">> READY TO SCAN <<"));

        display.setCursor(10, 34);
        display.print(F("Present Student ID"));

        // Footer Keypad Cue
        display.drawLine(0, 48, SCREEN_WIDTH, 48, SSD1306_WHITE);
        display.setCursor(4, 52);
        display.print(F("PRESS # FOR OTP PIN"));

        display.display();
    }

    void showPacingCooldown(float remainingSecs) {
        display.clearDisplay();

        // Top Banner
        display.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(4, 2);
        display.print(F("PACING INTERLOCK"));

        display.setTextColor(SSD1306_WHITE);
        display.setCursor(16, 18);
        display.print(F("ANTI-PROXY HOLD"));

        // Cooldown Timer
        display.setTextSize(2);
        display.setCursor(38, 30);
        display.print(remainingSecs, 1);
        display.print(F("s"));

        // Progress Bar
        display.setTextSize(1);
        int barWidth = (int)((remainingSecs / 3.0f) * 120.0f);
        if (barWidth > 120) barWidth = 120;
        if (barWidth < 0) barWidth = 0;
        display.drawRect(4, 50, 120, 8, SSD1306_WHITE);
        display.fillRect(6, 52, barWidth, 4, SSD1306_WHITE);

        display.display();
    }

    void showAccessGranted(const char* name, const char* status, const char* studentId) {
        display.clearDisplay();

        // Inverted Top Bar
        display.fillRect(0, 0, SCREEN_WIDTH, 14, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(8, 3);
        display.print(F(">> ACCESS GRANTED <<"));

        // Student Info
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(4, 20);
        display.print(name);

        display.setCursor(4, 34);
        display.print(F("ID: "));
        display.print(studentId);

        // Status Badge & Reverse-Receipt Feedback
        display.drawLine(0, 47, SCREEN_WIDTH, 47, SSD1306_WHITE);
        display.setCursor(4, 51);
        display.print(F("STATUS: "));
        display.print(status);

        display.display();
    }

    void showAccessDenied(const char* reason, const char* subtext) {
        display.clearDisplay();

        // Error Header
        display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
        display.drawRect(2, 2, SCREEN_WIDTH - 4, SCREEN_HEIGHT - 4, SSD1306_WHITE);

        display.setTextSize(1);
        display.setCursor(14, 10);
        display.print(F("[ ACCESS DENIED ]"));

        display.setCursor(8, 28);
        display.print(reason);

        display.setCursor(8, 44);
        display.print(subtext);

        display.display();
    }

    void showSessionStateScreen(bool opened, const char* sectionCode) {
        display.clearDisplay();
        display.fillRect(0, 0, SCREEN_WIDTH, 14, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(4, 3);
        display.print(opened ? F("SESSION INITIALIZED") : F("SESSION TERMINATED"));

        display.setTextColor(SSD1306_WHITE);
        display.setCursor(8, 22);
        display.print(F("SECTION: "));
        display.print(sectionCode);

        display.setCursor(8, 36);
        display.print(opened ? F("Manifest Active") : F("Roster Auto-Closed"));

        display.drawLine(0, 48, SCREEN_WIDTH, 48, SSD1306_WHITE);
        display.setCursor(4, 52);
        display.print(opened ? F("Terminal Ingress: OPEN") : F("Reports Emailed"));

        display.display();
    }

    void showKeypadOtpScreen(const char* buffer, int length, unsigned long remainingSecs) {
        display.clearDisplay();

        // Header
        display.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(4, 2);
        display.print(F("KEYPAD OTP INGRESS"));

        display.setTextColor(SSD1306_WHITE);
        display.setCursor(8, 18);
        display.print(F("Enter 6-Digit PIN:"));

        // Render masked PIN slots [ * * * _ _ _ ]
        display.setTextSize(2);
        display.setCursor(18, 30);
        for (int i = 0; i < 6; i++) {
            if (i < length) {
                display.print(buffer[i]); // Or '*' if fully masked
            } else {
                display.print(F("_"));
            }
            if (i < 5) display.print(F(" "));
        }

        // Instructions Footer
        display.setTextSize(1);
        display.drawLine(0, 48, SCREEN_WIDTH, 48, SSD1306_WHITE);
        display.setCursor(4, 52);
        display.print(F("#:Enter *:Clr ("));
        display.print(remainingSecs);
        display.print(F("s)"));

        display.display();
    }

    void showProcessing(const char* message) {
        display.clearDisplay();
        display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);

        display.setTextSize(1);
        display.setCursor(14, 20);
        display.print(F("AUTHENTICATING..."));

        display.setCursor(10, 36);
        display.print(message);

        display.display();
    }
};

extern DisplayManager displayMgr;

#endif // DISPLAY_H
