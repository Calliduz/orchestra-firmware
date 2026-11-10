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
        display.setTextWrap(false);
        display.display();
        return true;
    }

    /**
     * Draw uniform centered inverted top header bar (0..12px height).
     * Prevents text overflowing right screen border on 128x64 SSD1306 OLED.
     */
    void drawHeader(const char* title) {
        if (!title) return;
        display.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.setTextSize(1);
        display.setTextWrap(false);

        char buf[20];
        strncpy(buf, title, 18);
        buf[18] = '\0';
        int len = strlen(buf);
        int x = (SCREEN_WIDTH - (len * 6)) / 2;
        if (x < 2) x = 2;
        display.setCursor(x, 2);
        display.print(buf);
    }

    /**
     * Helper to print pixel-perfect centered text on the 128x64 display.
     * Automatically truncates text beyond maxChars to strictly prevent unwanted line wraps.
     */
    void printCentered(const char* text, int y, int size = 1, uint16_t color = SSD1306_WHITE, int maxChars = 18) {
        if (!text) return;

        char buf[22];
        int limit = (size == 2) ? 8 : maxChars;
        if (limit > 18) limit = 18;
        strncpy(buf, text, limit);
        buf[limit] = '\0';

        display.setTextSize(size);
        display.setTextColor(color);
        display.setTextWrap(false);

        int16_t x1, y1;
        uint16_t w, h;
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

        drawHeader("ORCHESTRA NODE");

        printCentered("HARDWARE TRUST NODE", 20, 1, SSD1306_WHITE, 19);
        printCentered("ESP32 Door Terminal", 32, 1, SSD1306_WHITE, 19);

        char verBuf[22];
        snprintf(verBuf, sizeof(verBuf), "Firmware %s", version);
        printCentered(verBuf, 46, 1, SSD1306_WHITE, 18);

        display.display();
    }

    /**
     * Wi-Fi Connection In-Progress Screen
     */
    void showConnectingWiFi(const char* ssid) {
        display.clearDisplay();

        drawHeader("NETWORK UPLINK");

        printCentered("Connecting Wi-Fi...", 22, 1, SSD1306_WHITE, 19);
        printCentered(ssid, 36, 1, SSD1306_WHITE, 18);

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
            // Live 12-Hour Digital Clock Display (Size 2, 8 chars max: e.g. "12:45 PM")
            printCentered(timeStr, 16, 2, SSD1306_WHITE, 10);

            // Date underneath clock
            if (dateStr && strlen(dateStr) > 0) {
                printCentered(dateStr, 34, 1, SSD1306_WHITE, 18);
            }
        } else {
            printCentered("TERMINAL LOCKED", 20, 1, SSD1306_WHITE, 18);
            printCentered("Awaiting Faculty", 34, 1, SSD1306_WHITE, 18);
        }

        // Bottom Strip: Clear Prompt
        display.drawLine(0, 46, SCREEN_WIDTH, 46, SSD1306_WHITE);
        printCentered("TAP FACULTY BADGE", 50, 1, SSD1306_WHITE, 18);

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

        // Clamp section code on top header to 11 chars
        char secHeader[13];
        strncpy(secHeader, sectionCode, 11);
        secHeader[11] = '\0';
        display.print(secHeader);

        // Network Status Flag
        display.setCursor(96, 2);
        display.print(wifiOk ? F("[NET]") : F("[!]"));

        if (timeStr && strlen(timeStr) > 0) {
            // Live 12-Hour Clock
            printCentered(timeStr, 16, 2, SSD1306_WHITE, 10);
            printCentered("READY FOR INGRESS", 34, 1, SSD1306_WHITE, 18);
        } else {
            printCentered("READY FOR INGRESS", 20, 1, SSD1306_WHITE, 18);
            printCentered("Present Student Card", 34, 1, SSD1306_WHITE, 20);
        }

        // Bottom Keypad Cue
        display.drawLine(0, 46, SCREEN_WIDTH, 46, SSD1306_WHITE);
        printCentered("PRESS # FOR PIN OTP", 50, 1, SSD1306_WHITE, 19);

        display.display();
    }

    /**
     * Pacing Interlock Cooldown Screen (3.0s anti-proxy countdown)
     */
    void showPacingCooldown(float remainingSecs) {
        display.clearDisplay();

        drawHeader("PACING COOLDOWN");

        printCentered("ANTI-PROXY HOLD", 16, 1, SSD1306_WHITE, 18);

        // Cooldown Timer in Size 2
        char timerBuf[12];
        snprintf(timerBuf, sizeof(timerBuf), "%.1fs", remainingSecs);
        printCentered(timerBuf, 28, 2, SSD1306_WHITE, 8);

        // Centered Progress Bar
        int barWidth = (int)((remainingSecs / 3.0f) * 96.0f);
        if (barWidth > 96) barWidth = 96;
        if (barWidth < 0) barWidth = 0;
        display.drawRect(14, 48, 100, 8, SSD1306_WHITE);
        display.fillRect(16, 50, barWidth, 4, SSD1306_WHITE);

        display.display();
    }

    /**
     * Access Granted Screen (On-time or Late check-in)
     */
    void showAccessGranted(const char* name, const char* status, const char* studentId) {
        display.clearDisplay();

        if (strcmp(status, "LATE") == 0) {
            drawHeader("CHECK-IN (LATE)");
        } else {
            drawHeader("ACCESS GRANTED");
        }

        // Student Info
        char cleanName[22];
        strncpy(cleanName, name, 18);
        cleanName[18] = '\0';
        printCentered(cleanName, 18, 1, SSD1306_WHITE, 18);

        char idBuf[22];
        snprintf(idBuf, sizeof(idBuf), "ID: %s", studentId);
        printCentered(idBuf, 31, 1, SSD1306_WHITE, 18);

        // Status Badge Strip
        display.drawLine(0, 46, SCREEN_WIDTH, 46, SSD1306_WHITE);
        char statBuf[22];
        snprintf(statBuf, sizeof(statBuf), "STATUS: %s", status);
        printCentered(statBuf, 50, 1, SSD1306_WHITE, 18);

        display.display();
    }

    /**
     * Clean, Pixel-Aligned Access Denied Screen
     * Displays a distinct top bar and two clean centered lines without text overflow.
     */
    void showAccessDenied(const char* line1, const char* line2 = nullptr, const char* line3 = nullptr) {
        display.clearDisplay();

        drawHeader("ACCESS DENIED");

        // Center Reason Lines (each strictly capped at 18 chars)
        printCentered(line1, 18, 1, SSD1306_WHITE, 18);

        if (line2 && strlen(line2) > 0) {
            printCentered(line2, 30, 1, SSD1306_WHITE, 18);
        }

        // Bottom Divider & Guidance
        display.drawLine(0, 44, SCREEN_WIDTH, 44, SSD1306_WHITE);
        if (line3 && strlen(line3) > 0) {
            printCentered(line3, 49, 1, SSD1306_WHITE, 18);
        } else {
            printCentered("INGRESS REJECTED", 49, 1, SSD1306_WHITE, 18);
        }

        display.display();
    }

    /**
     * Live Admin RFID Enrollment Scanning Screen
     * Clearly displays that admin is adding a new student or faculty!
     */
    void showEnrollmentScanningScreen(const char* targetRole = "STUDENT", const char* targetName = nullptr) {
        display.clearDisplay();

        drawHeader("ADMIN ENROLLMENT");

        char roleBuf[22];
        if (targetRole && (strcmp(targetRole, "FACULTY") == 0 || strcmp(targetRole, "ADMIN") == 0)) {
            snprintf(roleBuf, sizeof(roleBuf), "ROLE: NEW FACULTY");
        } else {
            snprintf(roleBuf, sizeof(roleBuf), "ROLE: NEW STUDENT");
        }

        if (targetName && strlen(targetName) > 0) {
            printCentered(roleBuf, 15, 1, SSD1306_WHITE, 18);

            char nameBuf[22];
            snprintf(nameBuf, sizeof(nameBuf), "%.18s", targetName);
            printCentered(nameBuf, 25, 1, SSD1306_WHITE, 18);

            printCentered("SCANNING LIVE RFID", 35, 1, SSD1306_WHITE, 18);
        } else {
            printCentered(roleBuf, 16, 1, SSD1306_WHITE, 18);
            printCentered("SCANNING LIVE RFID", 29, 1, SSD1306_WHITE, 18);
        }

        // Bottom Strip
        display.drawLine(0, 45, SCREEN_WIDTH, 45, SSD1306_WHITE);
        printCentered(">> TAP NEW BADGE <<", 50, 1, SSD1306_WHITE, 19);

        display.display();
    }

    /**
     * Live Admin RFID Enrollment Tag Captured Screen
     */
    void showEnrollmentCapturedScreen(const char* cardUid, const char* targetRole = "STUDENT", bool isRegistered = false, const char* cardHolder = nullptr) {
        display.clearDisplay();

        drawHeader(isRegistered ? "[ TAG RE-MAPPED ]" : "[ BADGE CAPTURED ]");

        char uidBuf[22];
        snprintf(uidBuf, sizeof(uidBuf), "UID: 0x%s", cardUid);
        printCentered(uidBuf, 16, 1, SSD1306_WHITE, 18);

        if (isRegistered && cardHolder && strlen(cardHolder) > 0) {
            char holdBuf[22];
            snprintf(holdBuf, sizeof(holdBuf), "%.16s", cardHolder);
            printCentered(holdBuf, 28, 1, SSD1306_WHITE, 18);
        } else {
            printCentered("Live Tag Captured", 28, 1, SSD1306_WHITE, 18);
        }

        display.drawLine(0, 44, SCREEN_WIDTH, 44, SSD1306_WHITE);
        printCentered("Confirm in Admin UI", 49, 1, SSD1306_WHITE, 19);

        display.display();
    }

    /**
     * Live RFID Card Intercept / Registration Screen (Generic)
     */
    void showCardScannedScreen(const char* cardUid, bool isRegistered) {
        display.clearDisplay();
        drawHeader(isRegistered ? "[ TAG RECOGNIZED ]" : "[ RFID INTERCEPT ]");

        char uidBuf[22];
        snprintf(uidBuf, sizeof(uidBuf), "UID: 0x%s", cardUid);
        printCentered(uidBuf, 20, 1, SSD1306_WHITE, 18);

        display.drawLine(0, 44, SCREEN_WIDTH, 44, SSD1306_WHITE);
        printCentered(isRegistered ? "Assigned Credential" : "Ready for Register", 49, 1, SSD1306_WHITE, 19);
        display.display();
    }

    /**
     * Session State Change Screen (Started or Closed)
     */
    void showSessionStateScreen(bool opened, const char* sectionCode, const char* facultyName = nullptr) {
        display.clearDisplay();
        drawHeader(opened ? "SESSION OPENED" : "SESSION CLOSED");

        char secBuf[22];
        snprintf(secBuf, sizeof(secBuf), "%s", sectionCode);
        printCentered(secBuf, 18, 1, SSD1306_WHITE, 18);

        if (opened) {
            if (facultyName && strlen(facultyName) > 0) {
                char facBuf[22];
                snprintf(facBuf, sizeof(facBuf), "Prof. %.12s", facultyName);
                printCentered(facBuf, 30, 1, SSD1306_WHITE, 18);
            } else {
                printCentered("Manifest Active", 30, 1, SSD1306_WHITE, 18);
            }
        } else {
            printCentered("Roster Finalized", 30, 1, SSD1306_WHITE, 18);
        }

        display.drawLine(0, 44, SCREEN_WIDTH, 44, SSD1306_WHITE);
        printCentered(opened ? "Ready For Ingress" : "Terminal Locked", 49, 1, SSD1306_WHITE, 18);

        display.display();
    }

    /**
     * Offline Session State Change Screen (Opened / Closed locally while offline)
     */
    void showOfflineSessionState(bool opened, const char* facultyName = nullptr) {
        display.clearDisplay();
        drawHeader(opened ? "[ OFFLINE OPEN ]" : "[ OFFLINE LOCK ]");

        if (facultyName && strlen(facultyName) > 0) {
            char facBuf[22];
            snprintf(facBuf, sizeof(facBuf), "Prof. %.12s", facultyName);
            printCentered(facBuf, 18, 1, SSD1306_WHITE, 18);
        } else {
            printCentered("FACULTY OVERRIDE", 18, 1, SSD1306_WHITE, 18);
        }

        printCentered(opened ? "LOCAL SESSION OPEN" : "SESSION CLOSED", 30, 1, SSD1306_WHITE, 18);

        display.drawLine(0, 44, SCREEN_WIDTH, 44, SSD1306_WHITE);
        printCentered(opened ? "Ready For Ingress" : "Queued For Sync", 49, 1, SSD1306_WHITE, 18);

        display.display();
    }

    /**
     * Offline Attendance Buffered Screen (Stored in NVS Queue)
     */
    void showOfflineAttendanceSaved(const char* cardUid, int queuedCount) {
        display.clearDisplay();
        drawHeader("[ OFFLINE SAVED ]");

        char uidBuf[22];
        snprintf(uidBuf, sizeof(uidBuf), "UID: 0x%s", cardUid);
        printCentered(uidBuf, 18, 1, SSD1306_WHITE, 18);

        printCentered("Buffered In NVS", 30, 1, SSD1306_WHITE, 18);

        display.drawLine(0, 44, SCREEN_WIDTH, 44, SSD1306_WHITE);
        char qBuf[22];
        snprintf(qBuf, sizeof(qBuf), "Queue #%d (Pending)", queuedCount);
        printCentered(qBuf, 49, 1, SSD1306_WHITE, 19);

        display.display();
    }

    /**
     * Offline Sync Complete Screen (Triggered when Wi-Fi reconnects)
     */
    void showOfflineSyncComplete(int syncedCount) {
        display.clearDisplay();
        drawHeader("[ SYNC COMPLETE ]");

        printCentered("Wi-Fi Reconnected", 18, 1, SSD1306_WHITE, 18);

        char countBuf[22];
        snprintf(countBuf, sizeof(countBuf), "Uploaded: %d Taps", syncedCount);
        printCentered(countBuf, 30, 1, SSD1306_WHITE, 18);

        display.drawLine(0, 44, SCREEN_WIDTH, 44, SSD1306_WHITE);
        printCentered("Cloud Synced OK", 49, 1, SSD1306_WHITE, 18);

        display.display();
    }

    /**
     * 4x4 Keypad PIN Entry Screen: Fixes size 2 overflow with 6 aligned digit boxes!
     */
    void showKeypadOtpScreen(const char* buffer, int length, unsigned long remainingSecs) {
        display.clearDisplay();

        drawHeader("KEYPAD OTP INGRESS");

        printCentered("Enter 6-Digit PIN:", 15, 1, SSD1306_WHITE, 18);

        // Draw 6 clean digit frames centered on 128px screen:
        // 6 boxes of width 14px, 4px gap = 6*14 + 5*4 = 104px. Start X = (128 - 104) / 2 = 12px.
        for (int i = 0; i < 6; i++) {
            int bx = 12 + (i * 18);
            display.drawRect(bx, 26, 14, 15, SSD1306_WHITE);
            display.setCursor(bx + 4, 30);
            display.setTextSize(1);
            display.setTextColor(SSD1306_WHITE);
            if (i < length) {
                display.print(buffer[i]);
            } else {
                display.print('-');
            }
        }

        // Instructions Footer
        display.drawLine(0, 45, SCREEN_WIDTH, 45, SSD1306_WHITE);
        char footBuf[22];
        snprintf(footBuf, sizeof(footBuf), "#:OK *:Clr (%lus)", remainingSecs);
        printCentered(footBuf, 49, 1, SSD1306_WHITE, 18);

        display.display();
    }

    /**
     * Validating / Authenticating In-Progress Screen
     */
    void showProcessing(const char* message) {
        display.clearDisplay();

        drawHeader("AUTHENTICATING");

        printCentered(message, 26, 1, SSD1306_WHITE, 18);
        printCentered("Please Wait...", 40, 1, SSD1306_WHITE, 18);

        display.display();
    }
};

extern DisplayManager displayMgr;

#endif // DISPLAY_H
