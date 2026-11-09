/**
 * =========================================================================
 * ORCHESTRA: Orchestrated RFID Classroom Attendance Framework
 * ESP32 Edge Terminal Firmware (C++ / PlatformIO)
 * =========================================================================
 * Authors: Agbada, Altichie, Cabunoc, Dalupang, Macadat, Malate
 * Institution: College of Computer Studies, AR101 Computer Architecture
 * 
 * Hardware Drivers:
 * - SPI MFRC522 13.56 MHz RFID Transceiver (ISO/IEC 14443A)
 * - I2C SSD1306 0.96" Monochrome Graphic OLED (128x64)
 * - 4x4 Matrix Keypad (Hardware OTP Fallback & Session Commands)
 * - Active Piezo Buzzer & Dual Status LEDs (Auditory/Visual Telemetry)
 * 
 * Architectural Features:
 * - Hardware-Enforced Session Trust Boundary (Default LOCKED state)
 * - 3.0-Second (3000ms) Non-Blocking Firmware Pacing Interlock via millis()
 * - Ephemeral 60s Keypad OTP Ingress Channel
 * - Resilient REST Telemetry Client with Auto-Reconnection
 * =========================================================================
 */

#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Keypad.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#include "config.h"
#include "audio_led.h"
#include "display.h"
#include "network_client.h"

// Instantiate Global Singletons
AudioLedManager audioLed;
DisplayManager displayMgr;
OrchestraNetworkClient netClient;

// MFRC522 RFID Hardware Reader Instance
MFRC522 mfrc522(RC522_SS_PIN, RC522_RST_PIN);

// 4x4 Matrix Keypad Key Definition & Instance
char keypadKeys[KEYPAD_ROWS][KEYPAD_COLS] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};
Keypad matrixKeypad = Keypad(
    makeKeymap(keypadKeys),
    (byte*)KEYPAD_ROW_PINS,
    (byte*)KEYPAD_COL_PINS,
    KEYPAD_ROWS,
    KEYPAD_COLS
);

// Finite State Machine Enumeration
enum TerminalState {
    STATE_BOOT,
    STATE_CONNECTING_WIFI,
    STATE_LOCKED,             // Hardware session interlock armed, waiting for faculty master badge
    STATE_IDLE_SCAN,          // Session open, ready to interrogate student credentials
    STATE_PACING_COOLDOWN,    // Non-blocking 3.0s anti-proxy interlock
    STATE_OTP_ENTRY,          // Keypad 6-digit numeric PIN entry mode
    STATE_PROCESSING,         // Sending REST packet to backend
    STATE_FEEDBACK_DISPLAY    // Temporary result splash before state restoration
};

// State Machine Tracking Variables
TerminalState currentState = STATE_BOOT;
TerminalState previousState = STATE_BOOT;
TerminalState stateAfterFeedback = STATE_LOCKED;

bool isTerminalLocked = true;
String currentSectionCode = DEFAULT_SECTION;

// Pacing Interlock & Non-blocking Timers
unsigned long lastStudentScanTime = 0;
unsigned long feedbackStartTime = 0;
unsigned long otpEntryStartTime = 0;

// Keypad OTP Buffer
char otpBuffer[7] = {0};
int otpBufferLength = 0;

// Forward Declarations
void changeState(TerminalState newState);
void processRfidCard();
void handleKeypadInput();
void handlePacingInterlock();
String getCardUidHex(MFRC522::Uid *uid);
void getCurrent12HourClock(char* timeBuf, size_t timeSize, char* dateBuf, size_t dateSize);

// Helper: Extract 12-hour formatted time (AM/PM) and date from RTC / SNTP
void getCurrent12HourClock(char* timeBuf, size_t timeSize, char* dateBuf, size_t dateSize) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10)) {
        int hour12 = timeinfo.tm_hour % 12;
        if (hour12 == 0) hour12 = 12;
        const char* ampm = (timeinfo.tm_hour >= 12) ? "PM" : "AM";
        snprintf(timeBuf, timeSize, "%d:%02d %s", hour12, timeinfo.tm_min, ampm);
        strftime(dateBuf, dateSize, "%a, %b %d", &timeinfo);
    } else {
        timeBuf[0] = '\0';
        dateBuf[0] = '\0';
    }
}

// =========================================================================
// SYSTEM INITIALIZATION (SETUP)
// =========================================================================
void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable brownout detector immediately
    Serial.begin(SERIAL_BAUD_RATE);
    delay(250);

    Serial.println();
    Serial.println(F("======================================================"));
    Serial.println(F("🎻 ORCHESTRA ESP32 DOORWAY INGRESS TERMINAL STARTING"));
    Serial.printf(F("🏷️ Terminal Node ID:    %s\n"), TERMINAL_ID);
    Serial.printf(F("📍 Classroom Location:  %s\n"), ROOM_NUMBER);
    Serial.printf(F("🛡️ Security API Token:  %s\n"), HARDWARE_API_KEY);
    Serial.println(F("======================================================"));

    // 1. Initialize Auditory & Visual Feedback Cues
    audioLed.begin();
    audioLed.setLeds(false, false);

    // 2. Initialize I2C SSD1306 Graphic Display
    if (!displayMgr.begin()) {
        Serial.println(F("[FATAL] SSD1306 OLED display init failed. Check wiring!"));
    }
    displayMgr.showBootScreen("v1.0.4-PRO");
    audioLed.cueBootSuccess();
    delay(1000);

    // 3. Initialize SPI Bus and MFRC522 Transceiver
    SPI.begin(RC522_SCK_PIN, RC522_MISO_PIN, RC522_MOSI_PIN, RC522_SS_PIN);
    mfrc522.PCD_Init();
    delay(50);
    byte mfrcVersion = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
    Serial.printf(F("[HARDWARE] MFRC522 SPI Transceiver Version: 0x%02X\n"), mfrcVersion);
    if (mfrcVersion == 0x00 || mfrcVersion == 0xFF) {
        Serial.println(F("[WARNING] MFRC522 communication failed or not connected. Check SPI wiring."));
    } else {
        Serial.println(F("[HARDWARE] MFRC522 SPI 13.56 MHz Transceiver OK."));
    }

    // 4. Initialize Wi-Fi Network Subsystem
    displayMgr.showConnectingWiFi("Multi-AP Auto");
    netClient.begin();

    bool wifiConnected = netClient.connectBestNetwork(8000);

    if (wifiConnected && netClient.isConnected()) {
        displayMgr.showConnectingWiFi(WiFi.SSID().c_str());
        delay(600);

        // Sync Initial Session State with Cloud Backend
        bool lockedFromCloud = true;
        String sectionFromCloud = "";
        if (netClient.syncTerminalStatus(lockedFromCloud, sectionFromCloud)) {
            isTerminalLocked = lockedFromCloud;
            currentSectionCode = sectionFromCloud;
            Serial.printf(F("[CLOUD] Initial terminal state synced: %s (Section: %s)\n"),
                          isTerminalLocked ? "LOCKED" : "OPEN", currentSectionCode.c_str());
        }
    } else {
        Serial.println(F("[WIFI] Connection timed out. Operating in offline standby mode."));
    }

    // Initial state transition
    if (isTerminalLocked) {
        changeState(STATE_LOCKED);
    } else {
        changeState(STATE_IDLE_SCAN);
    }
}

// =========================================================================
// MAIN NON-BLOCKING EXECUTION LOOP
// =========================================================================
void loop() {
    unsigned long now = millis();

    // 1. Maintain background Wi-Fi connection
    netClient.maintainConnection(now);

    // 2. Periodic 1-second display refresh during idle states to keep the 12-hour clock ticking live
    static unsigned long lastClockTick = 0;
    if (now - lastClockTick >= 1000) {
        lastClockTick = now;
        if (currentState == STATE_LOCKED) {
            char timeBuf[16] = {0};
            char dateBuf[24] = {0};
            getCurrent12HourClock(timeBuf, sizeof(timeBuf), dateBuf, sizeof(dateBuf));
            displayMgr.showLockedScreen(TERMINAL_ID, ROOM_NUMBER, timeBuf, dateBuf);
        } else if (currentState == STATE_IDLE_SCAN) {
            char timeBuf[16] = {0};
            char dateBuf[24] = {0};
            getCurrent12HourClock(timeBuf, sizeof(timeBuf), dateBuf, sizeof(dateBuf));
            displayMgr.showIdleScanScreen(currentSectionCode.c_str(), 0, netClient.isConnected(), timeBuf, dateBuf);
        }
    }

    // 3. Periodic Cloud Session State Synchronization (Every 2.5s)
    // Synchronizes remote session termination or activation triggered from the web portal
    static unsigned long lastSessionSyncTime = 0;
    if (netClient.isConnected() && (currentState == STATE_LOCKED || currentState == STATE_IDLE_SCAN)) {
        if (now - lastSessionSyncTime >= 2500) {
            lastSessionSyncTime = now;
            bool cloudIsLocked = isTerminalLocked;
            String cloudSection = currentSectionCode;
            if (netClient.syncTerminalStatus(cloudIsLocked, cloudSection)) {
                // CASE A: Terminal was ACTIVE / OPEN locally, but session was TERMINATED on website!
                if (!isTerminalLocked && cloudIsLocked) {
                    Serial.println(F("[REMOTE EVENT] Active session terminated from website! Arming hardware lock..."));
                    isTerminalLocked = true;
                    audioLed.cueSessionStateChanged(false); // Auditory tone & red LED feedback
                    displayMgr.showSessionStateScreen(false, currentSectionCode.c_str());
                    stateAfterFeedback = STATE_LOCKED;
                    changeState(STATE_FEEDBACK_DISPLAY);
                }
                // CASE B: Terminal was LOCKED locally, but session was STARTED on website!
                else if (isTerminalLocked && !cloudIsLocked) {
                    Serial.println(F("[REMOTE EVENT] Session opened from website! Unlocking hardware terminal..."));
                    isTerminalLocked = false;
                    currentSectionCode = cloudSection;
                    audioLed.cueSessionStateChanged(true); // Auditory chime & green LED feedback
                    displayMgr.showSessionStateScreen(true, currentSectionCode.c_str(), "Web Console");
                    stateAfterFeedback = STATE_IDLE_SCAN;
                    changeState(STATE_FEEDBACK_DISPLAY);
                }
            }
        }
    }

    // 4. State Machine Dispatcher
    switch (currentState) {
        case STATE_LOCKED: {
            audioLed.setLeds(false, true); // Solid red status LED during locked state
            processRfidCard();             // RFID cards are evaluated; student taps yield 403, faculty badge unlocks
            handleKeypadInput();           // Keypad can be used to query state
            break;
        }

        case STATE_IDLE_SCAN: {
            audioLed.setLeds(true, false); // Solid green status LED during active scan state
            processRfidCard();             // Scans student badges or faculty badge to close session
            handleKeypadInput();           // '#' key triggers OTP entry mode
            break;
        }

        case STATE_PACING_COOLDOWN: {
            handlePacingInterlock();
            break;
        }

        case STATE_OTP_ENTRY: {
            unsigned long elapsed = now - otpEntryStartTime;
            if (elapsed >= KEYPAD_OTP_TIMEOUT_MS) {
                Serial.println(F("[KEYPAD] OTP entry timed out. Reverting to idle."));
                changeState(STATE_IDLE_SCAN);
                break;
            }
            static unsigned long lastOtpTick = 0;
            if (now - lastOtpTick >= 1000) {
                lastOtpTick = now;
                unsigned long remainingSecs = (KEYPAD_OTP_TIMEOUT_MS - elapsed) / 1000;
                displayMgr.showKeypadOtpScreen(otpBuffer, otpBufferLength, remainingSecs);
            }
            handleKeypadInput();
            break;
        }

        case STATE_FEEDBACK_DISPLAY: {
            if (now - feedbackStartTime >= RESULT_DISPLAY_DURATION_MS) {
                audioLed.setLeds(false, false);
                changeState(stateAfterFeedback);
            }
            break;
        }

        default:
            break;
    }
}

// =========================================================================
// FINITE STATE MACHINE TRANSITION HANDLER
// =========================================================================
void changeState(TerminalState newState) {
    previousState = currentState;
    currentState = newState;

    char timeBuf[16] = {0};
    char dateBuf[24] = {0};
    getCurrent12HourClock(timeBuf, sizeof(timeBuf), dateBuf, sizeof(dateBuf));

    switch (newState) {
        case STATE_LOCKED:
            isTerminalLocked = true;
            audioLed.setLeds(false, true);
            displayMgr.showLockedScreen(TERMINAL_ID, ROOM_NUMBER, timeBuf, dateBuf);
            Serial.println(F("[FSM] State -> STATE_LOCKED (Hardware Interlock Armed)"));
            break;

        case STATE_IDLE_SCAN:
            isTerminalLocked = false;
            audioLed.setLeds(true, false);
            displayMgr.showIdleScanScreen(currentSectionCode.c_str(), 0, netClient.isConnected(), timeBuf, dateBuf);
            Serial.println(F("[FSM] State -> STATE_IDLE_SCAN (Ingress Matrix Open)"));
            break;

        case STATE_PACING_COOLDOWN:
            Serial.println(F("[FSM] State -> STATE_PACING_COOLDOWN (3.0s Non-Blocking Hold)"));
            break;

        case STATE_OTP_ENTRY:
            otpBufferLength = 0;
            otpBuffer[0] = '\0';
            otpEntryStartTime = millis();
            audioLed.playKeypadClick();
            displayMgr.showKeypadOtpScreen(otpBuffer, otpBufferLength, KEYPAD_OTP_TIMEOUT_MS / 1000);
            Serial.println(F("[FSM] State -> STATE_OTP_ENTRY (Keypad 6-Digit Ingress Mode)"));
            break;

        case STATE_FEEDBACK_DISPLAY:
            feedbackStartTime = millis();
            Serial.println(F("[FSM] State -> STATE_FEEDBACK_DISPLAY"));
            break;

        default:
            break;
    }
}

// =========================================================================
// HARDWARE PACING INTERLOCK (3.0s Non-Blocking Cooldown via millis())
// =========================================================================
void handlePacingInterlock() {
    unsigned long elapsed = millis() - lastStudentScanTime;

    if (elapsed >= PACING_COOLDOWN_MS) {
        // Cooldown period elapsed successfully; re-arm scanner
        changeState(STATE_IDLE_SCAN);
        return;
    }

    float remainingSeconds = (float)(PACING_COOLDOWN_MS - elapsed) / 1000.0f;
    displayMgr.showPacingCooldown(remainingSeconds);

    // If an RFID card is presented during the pacing cooldown, reject immediately
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
        String cardUid = getCardUidHex(&(mfrc522.uid));
        mfrc522.PICC_HaltA();
        mfrc522.PCD_StopCrypto1();

        Serial.printf(F("[PACING VIOLATION] Card 0x%s presented during active cooldown (%.1fs remaining)!\n"),
                      cardUid.c_str(), remainingSeconds);
        audioLed.cuePacingViolation();
    }
}

// =========================================================================
// SPI MFRC522 RFID CARD PROCESSING
// =========================================================================
void processRfidCard() {
    if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
        return;
    }

    String cardUid = getCardUidHex(&(mfrc522.uid));
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();

    Serial.println();
    Serial.printf(F("[RFID EVENT] Interrogated ISO 14443A Tag: 0x%s\n"), cardUid.c_str());
    displayMgr.showProcessing("Validating RFID...");

    TerminalApiResponse response;
    bool reqSuccess = netClient.postRfidTap(cardUid, response);

    Serial.printf(F("[BACKEND RESPONSE] HTTP %d | Code: %s | Message: %s\n"),
                  response.httpCode, response.code.c_str(), response.message.c_str());

    if (response.code == "SESSION_STARTED") {
        // Faculty Master Badge tapped to open session
        isTerminalLocked = false;
        currentSectionCode = response.sectionCode;
        audioLed.cueSessionStateChanged(true);
        displayMgr.showSessionStateScreen(true, currentSectionCode.c_str(), response.facultyName.c_str());
        stateAfterFeedback = STATE_IDLE_SCAN;
        changeState(STATE_FEEDBACK_DISPLAY);
    }
    else if (response.code == "SESSION_CLOSED") {
        // Faculty Master Badge tapped to terminate session
        isTerminalLocked = true;
        audioLed.cueSessionStateChanged(false);
        displayMgr.showSessionStateScreen(false, currentSectionCode.c_str());
        stateAfterFeedback = STATE_LOCKED;
        changeState(STATE_FEEDBACK_DISPLAY);
    }
    else if (response.code == "ACCESS_GRANTED") {
        // Valid student card scanned on time or late
        audioLed.cueAccessGranted();
        displayMgr.showAccessGranted(response.studentName.c_str(), response.status.c_str(), response.studentId.c_str());
        lastStudentScanTime = millis();
        stateAfterFeedback = STATE_PACING_COOLDOWN; // Activate 3.0s anti-proxy interlock
        changeState(STATE_FEEDBACK_DISPLAY);
    }
    else if (response.code == "NOT_ENROLLED") {
        // Student not enrolled in this section
        audioLed.cueAccessDenied();
        displayMgr.showAccessDenied("NOT ENROLLED", "Not In Course Roster", "Check With Faculty");
        stateAfterFeedback = STATE_IDLE_SCAN;
        changeState(STATE_FEEDBACK_DISPLAY);
    }
    else if (response.code == "NOT_SESSION_OWNER") {
        // Another faculty member attempting to close session
        audioLed.cueAccessDenied();
        displayMgr.showAccessDenied("NOT SESSION OWNER", "Active Session Exists", "Cannot Close Session");
        stateAfterFeedback = STATE_IDLE_SCAN;
        changeState(STATE_FEEDBACK_DISPLAY);
    }
    else if (response.code == "ALREADY_RECORDED") {
        // Anti-passback violation (card already tapped for this session)
        audioLed.cueAccessDenied();
        displayMgr.showAccessDenied("ANTI-PASSBACK", "Already Recorded", "Attendance Logged");
        stateAfterFeedback = STATE_IDLE_SCAN;
        changeState(STATE_FEEDBACK_DISPLAY);
    }
    else if (response.code == "SESSION_LOCKED") {
        // Student card presented while terminal is locked
        audioLed.cueAccessDenied();
        displayMgr.showAccessDenied("TERMINAL LOCKED", "Tap Faculty Badge", "To Open Session");
        stateAfterFeedback = STATE_LOCKED;
        changeState(STATE_FEEDBACK_DISPLAY);
    }
    else if (response.code == "UNREGISTERED_CARD" || response.code == "UNREGISTERED_TAG") {
        audioLed.cueAccessDenied();
        displayMgr.showCardScannedScreen(cardUid.c_str(), false);
        stateAfterFeedback = isTerminalLocked ? STATE_LOCKED : STATE_IDLE_SCAN;
        changeState(STATE_FEEDBACK_DISPLAY);
    }
    else {
        // Generic network error or offline fallback
        audioLed.cueAccessDenied();
        displayMgr.showAccessDenied("SYSTEM NOTICE", "Tap Again", "Check Connection");
        stateAfterFeedback = isTerminalLocked ? STATE_LOCKED : STATE_IDLE_SCAN;
        changeState(STATE_FEEDBACK_DISPLAY);
    }
}

// =========================================================================
// 4x4 MATRIX KEYPAD INPUT HANDLER
// =========================================================================
void handleKeypadInput() {
    char key = matrixKeypad.getKey();
    if (!key) return;

    audioLed.playKeypadClick();
    Serial.printf(F("[KEYPAD] Key pressed: '%c'\n"), key);

    // If currently in IDLE SCAN mode and '#' is pressed, enter OTP mode
    if (currentState == STATE_IDLE_SCAN && key == '#') {
        changeState(STATE_OTP_ENTRY);
        return;
    }

    // In OTP ENTRY Mode
    if (currentState == STATE_OTP_ENTRY) {
        unsigned long elapsed = millis() - otpEntryStartTime;
        if (elapsed >= KEYPAD_OTP_TIMEOUT_MS) {
            // Timeout expired
            Serial.println(F("[KEYPAD] OTP entry timed out. Reverting to idle."));
            changeState(STATE_IDLE_SCAN);
            return;
        }

        unsigned long remainingSecs = (KEYPAD_OTP_TIMEOUT_MS - elapsed) / 1000;

        // '*' acts as backspace or abort
        if (key == '*') {
            if (otpBufferLength > 0) {
                otpBufferLength--;
                otpBuffer[otpBufferLength] = '\0';
                displayMgr.showKeypadOtpScreen(otpBuffer, otpBufferLength, remainingSecs);
            } else {
                // Exit OTP mode if buffer already empty
                Serial.println(F("[KEYPAD] OTP cancelled by user."));
                changeState(STATE_IDLE_SCAN);
                return;
            }
        }
        // '#' acts as submit/commit when 6 digits are buffered
        else if (key == '#') {
            if (otpBufferLength == 6) {
                Serial.printf(F("[KEYPAD] Submitting 6-Digit OTP: %s\n"), otpBuffer);
                displayMgr.showProcessing("Validating OTP...");

                TerminalApiResponse response;
                bool success = netClient.postKeypadOtp(String(otpBuffer), response);

                if (response.code == "ACCESS_GRANTED") {
                    audioLed.cueAccessGranted();
                    displayMgr.showAccessGranted(response.studentName.c_str(), response.status.c_str(), response.studentId.c_str());
                    lastStudentScanTime = millis();
                    stateAfterFeedback = STATE_PACING_COOLDOWN;
                    changeState(STATE_FEEDBACK_DISPLAY);
                } else if (response.code == "ALREADY_RECORDED") {
                    audioLed.cueAccessDenied();
                    displayMgr.showAccessDenied("ANTI-PASSBACK", "Already Recorded", "Attendance Logged");
                    stateAfterFeedback = STATE_IDLE_SCAN;
                    changeState(STATE_FEEDBACK_DISPLAY);
                } else {
                    audioLed.cueAccessDenied();
                    displayMgr.showAccessDenied("INVALID OTP", "Code Expired/Wrong", "Generate New PIN");
                    stateAfterFeedback = STATE_IDLE_SCAN;
                    changeState(STATE_FEEDBACK_DISPLAY);
                }
            } else {
                // Less than 6 digits entered
                audioLed.cuePacingViolation();
                Serial.println(F("[KEYPAD] Insufficient digits for submission."));
            }
        }
        // Numeric Keys ('0' to '9')
        else if (key >= '0' && key <= '9') {
            if (otpBufferLength < 6) {
                otpBuffer[otpBufferLength] = key;
                otpBufferLength++;
                otpBuffer[otpBufferLength] = '\0';
                displayMgr.showKeypadOtpScreen(otpBuffer, otpBufferLength, remainingSecs);
            }
        }
    }
}

// =========================================================================
// HELPER: Convert MFRC522 UID Bytes to Hexadecimal String
// =========================================================================
String getCardUidHex(MFRC522::Uid *uid) {
    String hexStr = "";
    for (byte i = 0; i < uid->size; i++) {
        if (uid->uidByte[i] < 0x10) {
            hexStr += "0";
        }
        hexStr += String(uid->uidByte[i], HEX);
    }
    hexStr.toUpperCase();
    return hexStr;
}
