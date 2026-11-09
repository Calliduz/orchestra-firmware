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

    /**
     * Helper to print pixel-perfect centered text on the 128x64 display.
     * Automatically truncates text beyond maxChars to strictly prevent unwanted line wraps.
     */
    void printCentered(const char* text, int y, int size = 1, uint16_t color = SSD1306_WHITE, int maxChars = 21) {
        if (!text) return;

        char buf[32];
        int limit = (size == 2) ? 10 : maxChars;
        if (limit > 31) limit = 31;
        strncpy(buf, text, limit);
        buf[limit] = '\0';

        int16_t x1, y1;
        uint16_t w, h;
        display.setTextSize(size);
        display.setTextColor(color);
        display.getTextBounds(buf, 0, y, &x1, &y1, &w, &h);
        int x = (SCREEN_WIDTH - (int)w) / 2;
        if (x < 0) x = 0;
        display.setCursor(x, y);
        display.print(buf);
    }

    /**
     * Boot Splash Screen
     */
    void showBootScreen(const char* version = "v1.0.4") {
        display.clearDisplay();
        display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);

        // Header
        display.fillRect(0, 0, SCREEN_WIDTH, 14, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(16, 3);
        display.print(F("=== ORCHESTRA ==="));

        // Subtitle
        printCentered("HARDWARE TRUST NODE", 22, 1, SSD1306_WHITE);
        printCentered("ESP32 Edge Terminal", 34, 1, SSD1306_WHITE);

        char verBuf[24];
        snprintf(verBuf, sizeof(verBuf), "Firmware %s", version);
        printCentered(verBuf, 48, 1, SSD1306_WHITE);

        display.display();
    }

    /**
     * Wi-Fi Connection In-Progress Screen
     */
    void showConnectingWiFi(const char* ssid) {
        display.clearDisplay();
        display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);

        display.fillRect(0, 0, SCREEN_WIDTH, 14, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(18, 3);
        display.print(F("NETWORK UPLINK"));

        printCentered("Connecting Wi-Fi...", 24, 1, SSD1306_WHITE);
        printCentered(ssid, 38, 1, SSD1306_WHITE, 18);

        display.display();
    }

    /**
     * Idle Screen when Session is Locked: Displays 12-hour clock, date, room, and prompt
     */
    void showLockedScreen(const char* terminalId, const char* room, const char* timeStr = nullptr, const char* dateStr = nullptr) {
        display.clearDisplay();

        // Top Status Strip (Inverted)
        display.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(4, 2);
        display.print(F("ORCHESTRA"));
        display.setCursor(76, 2);
        display.print(F("[LOCKED]"));

        if (timeStr && strlen(timeStr) > 0) {
            // Live 12-Hour Digital Clock Display (Size 2)
            printCentered(timeStr, 16, 2, SSD1306_WHITE);

            // Date underneath clock
            if (dateStr && strlen(dateStr) > 0) {
                printCentered(dateStr, 34, 1, SSD1306_WHITE);
            }
        } else {
            // Fallback static prompt
            printCentered("[ TERMINAL LOCKED ]", 20, 1, SSD1306_WHITE);
            printCentered("Waiting for Faculty", 34, 1, SSD1306_WHITE);
        }

        // Bottom Strip: Room & Scan prompt
        display.drawLine(0, 46, SCREEN_WIDTH, 46, SSD1306_WHITE);
        char botBuf[24];
        snprintf(botBuf, sizeof(botBuf), "%s: TAP FACULTY", room);
        printCentered(botBuf, 50, 1, SSD1306_WHITE, 20);

        display.display();
    }

    /**
     * Idle Screen when Session is Open: Displays Section, 12-hour clock, and Tap prompt
     */
    void showIdleScanScreen(const char* sectionCode, int count, bool wifiOk, const char* timeStr = nullptr, const char* dateStr = nullptr) {
        display.clearDisplay();

        // Top Header Strip (Inverted)
        display.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(4, 2);

        // Clamp section code on top header to 13 chars
        char secHeader[14];
        strncpy(secHeader, sectionCode, 13);
        secHeader[13] = '\0';
        display.print(secHeader);

        // Network Status Flag
        display.setCursor(96, 2);
        display.print(wifiOk ? F("[NET]") : F("[!]"));

        if (timeStr && strlen(timeStr) > 0) {
            // Live 12-Hour Clock
            printCentered(timeStr, 16, 2, SSD1306_WHITE);
            printCentered(">> READY TO SCAN <<", 34, 1, SSD1306_WHITE);
        } else {
            printCentered(">> READY TO SCAN <<", 20, 1, SSD1306_WHITE);
            printCentered("Present Student Card", 34, 1, SSD1306_WHITE);
        }

        // Bottom Keypad Cue
        display.drawLine(0, 46, SCREEN_WIDTH, 46, SSD1306_WHITE);
        printCentered("PRESS # FOR OTP PIN", 50, 1, SSD1306_WHITE);

        display.display();
    }

    /**
     * Pacing Interlock Cooldown Screen (3.0s anti-proxy countdown)
     */
    void showPacingCooldown(float remainingSecs) {
        display.clearDisplay();

        // Top Banner
        display.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(4, 2);
        display.print(F("PACING INTERLOCK"));
        display.setCursor(92, 2);
        display.print(F("[HOLD]"));

        printCentered("ANTI-PROXY COOLDOWN", 16, 1, SSD1306_WHITE);

        // Cooldown Timer in Size 2
        char timerBuf[16];
        snprintf(timerBuf, sizeof(timerBuf), "%.1fs", remainingSecs);
        printCentered(timerBuf, 28, 2, SSD1306_WHITE);

        // Smooth Progress Bar
        int barWidth = (int)((remainingSecs / 3.0f) * 108.0f);
        if (barWidth > 108) barWidth = 108;
        if (barWidth < 0) barWidth = 0;
        display.drawRect(8, 48, 112, 8, SSD1306_WHITE);
        display.fillRect(10, 50, barWidth, 4, SSD1306_WHITE);

        display.display();
    }

    /**
     * Access Granted Screen (On-time or Late check-in)
     */
    void showAccessGranted(const char* name, const char* status, const char* studentId) {
        display.clearDisplay();

        // Inverted Top Bar
        display.fillRect(0, 0, SCREEN_WIDTH, 14, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.setTextSize(1);

        if (strcmp(status, "LATE") == 0) {
            display.setCursor(6, 3);
            display.print(F(">> CHECK-IN (LATE) <<"));
        } else {
            display.setCursor(4, 3);
            display.print(F(">> ACCESS GRANTED <<"));
        }

        // Student Info (Truncate to 20 chars)
        char cleanName[22];
        strncpy(cleanName, name, 20);
        cleanName[20] = '\0';
        printCentered(cleanName, 18, 1, SSD1306_WHITE, 20);

        char idBuf[22];
        snprintf(idBuf, sizeof(idBuf), "ID: %s", studentId);
        printCentered(idBuf, 32, 1, SSD1306_WHITE, 20);

        // Status Badge Strip
        display.drawLine(0, 46, SCREEN_WIDTH, 46, SSD1306_WHITE);
        char statBuf[22];
        snprintf(statBuf, sizeof(statBuf), "STATUS: %s", status);
        printCentered(statBuf, 50, 1, SSD1306_WHITE, 20);

        display.display();
    }

    /**
     * Clean, Pixel-Aligned Access Denied Screen
     * Displays a distinct top bar and two clean centered lines without text overflow.
     */
    void showAccessDenied(const char* line1, const char* line2 = nullptr, const char* line3 = nullptr) {
        display.clearDisplay();

        // Inverted Top Bar
        display.fillRect(0, 0, SCREEN_WIDTH, 14, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(14, 3);
        display.print(F("[ ACCESS DENIED ]"));

        // Center Reason Lines (each strictly capped at 20 chars)
        printCentered(line1, 20, 1, SSD1306_WHITE, 20);

        if (line2 && strlen(line2) > 0) {
            printCentered(line2, 33, 1, SSD1306_WHITE, 20);
        }

        // Bottom Divider & Guidance
        display.drawLine(0, 46, SCREEN_WIDTH, 46, SSD1306_WHITE);
        if (line3 && strlen(line3) > 0) {
            printCentered(line3, 50, 1, SSD1306_WHITE, 20);
        } else {
            printCentered("INGRESS REJECTED", 50, 1, SSD1306_WHITE, 20);
        }

        display.display();
    }

    /**
     * Live RFID Card Intercept / Registration Screen
     * Shows card hex UID and status on OLED
     */
    void showCardScannedScreen(const char* cardUid, bool isRegistered) {
        display.clearDisplay();
        display.fillRect(0, 0, SCREEN_WIDTH, 14, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(6, 3);
        display.print(isRegistered ? F("[ TAG RECOGNIZED ]") : F("[ RFID INTERCEPT ]"));

        char uidBuf[22];
        snprintf(uidBuf, sizeof(uidBuf), "UID: 0x%s", cardUid);
        printCentered(uidBuf, 22, 1, SSD1306_WHITE, 20);

        display.drawLine(0, 44, SCREEN_WIDTH, 44, SSD1306_WHITE);
        if (isRegistered) {
            printCentered("Assigned Credential", 49, 1, SSD1306_WHITE, 20);
        } else {
            printCentered("Ready for Register", 49, 1, SSD1306_WHITE, 20);
        }
        display.display();
    }

    /**
     * Session State Change Screen (Started or Closed)
     */
    void showSessionStateScreen(bool opened, const char* sectionCode, const char* facultyName = nullptr) {
        display.clearDisplay();
        display.fillRect(0, 0, SCREEN_WIDTH, 14, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(6, 3);
        display.print(opened ? F("[ SESSION OPENED ]") : F("[ SESSION CLOSED ]"));

        char secBuf[22];
        snprintf(secBuf, sizeof(secBuf), "%s", sectionCode);
        printCentered(secBuf, 20, 1, SSD1306_WHITE, 20);

        if (opened) {
            if (facultyName && strlen(facultyName) > 0) {
                char facBuf[22];
                snprintf(facBuf, sizeof(facBuf), "Prof. %s", facultyName);
                printCentered(facBuf, 33, 1, SSD1306_WHITE, 20);
            } else {
                printCentered("Manifest Active", 33, 1, SSD1306_WHITE, 20);
            }
        } else {
            printCentered("Roster Finalized", 33, 1, SSD1306_WHITE, 20);
        }

        display.drawLine(0, 46, SCREEN_WIDTH, 46, SSD1306_WHITE);
        printCentered(opened ? "Ready For Ingress" : "Reports Dispatched", 50, 1, SSD1306_WHITE, 20);

        display.display();
    }

    /**
     * 4x4 Keypad PIN Entry Screen
     */
    void showKeypadOtpScreen(const char* buffer, int length, unsigned long remainingSecs) {
        display.clearDisplay();

        // Header
        display.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(6, 2);
        display.print(F("KEYPAD OTP INGRESS"));

        printCentered("Enter 6-Digit PIN:", 16, 1, SSD1306_WHITE);

        // Render formatted PIN slots [ 1 2 3 _ _ _ ] centered
        char pinDisplay[20] = {0};
        int pos = 0;
        for (int i = 0; i < 6; i++) {
            if (i < length) {
                pinDisplay[pos++] = buffer[i];
            } else {
                pinDisplay[pos++] = '_';
            }
            if (i < 5) pinDisplay[pos++] = ' ';
        }
        pinDisplay[pos] = '\0';
        printCentered(pinDisplay, 28, 2, SSD1306_WHITE);

        // Instructions Footer
        display.drawLine(0, 46, SCREEN_WIDTH, 46, SSD1306_WHITE);
        char footBuf[22];
        snprintf(footBuf, sizeof(footBuf), "#:Submit *:Clr (%lus)", remainingSecs);
        printCentered(footBuf, 50, 1, SSD1306_WHITE, 20);

        display.display();
    }

    /**
     * Validating / Authenticating In-Progress Screen
     */
    void showProcessing(const char* message) {
        display.clearDisplay();
        display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);

        display.fillRect(0, 0, SCREEN_WIDTH, 14, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(14, 3);
        display.print(F("AUTHENTICATING..."));

        printCentered(message, 30, 1, SSD1306_WHITE, 20);
        printCentered("Please Wait", 44, 1, SSD1306_WHITE, 20);

        display.display();
    }
};

extern DisplayManager displayMgr;

#endif // DISPLAY_H
