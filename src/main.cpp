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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "config.h"
#include "audio_led.h"
#include "display.h"
#include "offline_storage.h"
#include "network_client.h"
#include "captive_portal.h"

// Instantiate Global Singletons
AudioLedManager audioLed;
DisplayManager displayMgr;
OfflineStorageManager offlineStorage;
OrchestraNetworkClient netClient;
CaptivePortalManager captivePortal;

// MFRC522 RFID Hardware Reader Instance
MFRC522 mfrc522(RC522_SS_PIN, RC522_RST_PIN);
static const char* g_readerStatus = "MFRC522 OK";

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
    STATE_CAPTIVE_PORTAL,     // Fallback AP / OTA Wi-Fi & backend configuration
    STATE_LOCKED,             // Hardware session interlock armed, waiting for faculty master badge
    STATE_IDLE_SCAN,          // Session open, ready to interrogate student credentials
    STATE_PACING_COOLDOWN,    // Non-blocking 3.0s anti-proxy interlock
    STATE_OTP_ENTRY,          // Keypad 6-digit numeric PIN entry mode
    STATE_SECTION_SELECTION,  // Hardware keypad prompt when multiple course sections match terminal
    STATE_ENROLLMENT_SCAN,    // Live admin RFID enrollment mode for new student or faculty
    STATE_PROCESSING,         // Sending REST packet to backend
    STATE_FEEDBACK_DISPLAY    // Temporary result splash before state restoration
};

// State Machine Tracking Variables
TerminalState currentState = STATE_BOOT;
TerminalState previousState = STATE_BOOT;
TerminalState stateAfterFeedback = STATE_LOCKED;

bool isTerminalLocked = true;
bool isEnrollmentMode = false;
String enrollmentTargetRole = "STUDENT";
String enrollmentTargetName = "";
String currentSectionCode = DEFAULT_SECTION;

// Pacing Interlock & Non-blocking Timers
unsigned long lastStudentScanTime = 0;
unsigned long feedbackStartTime = 0;
unsigned long otpEntryStartTime = 0;
unsigned long lastFacultyToggleTime = 0;
const unsigned long FACULTY_TOGGLE_COOLDOWN_MS = 4000;

// Section Disambiguation Selection State
unsigned long sectionSelectionStartTime = 0;
const unsigned long SECTION_SELECTION_TIMEOUT_MS = 15000;
int disambiguationCount = 0;
char disambiguationSections[4][24] = {0};
char disambiguationFacultyUid[32] = {0};

// Keypad OTP Buffer
char otpBuffer[7] = {0};
int otpBufferLength = 0;

// FreeRTOS Task Decoupling Data Structures & Queues
enum NetEventType {
    NET_EVENT_NONE = 0,
    NET_EVENT_RFID_TAP,
    NET_EVENT_KEYPAD_OTP,
    NET_EVENT_SELECT_SECTION,
    NET_EVENT_FORCE_RECONNECT
};

struct NetRequestEvent {
    NetEventType type;
    char payload[64];
    unsigned long timestamp;
};

struct NetResponseEvent {
    NetEventType type;
    int httpCode;
    char code[32];
    char message[64];
    char studentName[48];
    char studentId[24];
    char status[24];
    char sectionCode[32];
    char facultyName[48];
    bool isRegistered;
    char cardHolder[48];
    char originalPayload[64];
    char facultyUid[32];
    int sectionCount;
    char sectionCodes[4][24];
};

struct CloudStatusMessage {
    bool isLocked;
    char sectionCode[32];
    bool enrollmentActive;
    char targetRole[16];
    char targetName[48];
};

QueueHandle_t netRequestQueue = NULL;
QueueHandle_t netResponseQueue = NULL;
QueueHandle_t cloudStatusQueue = NULL;
TaskHandle_t networkTaskHandle = NULL;

String currentPendingUid = "";
unsigned long stateProcessingStartTime = 0;

// Forward Declarations
void changeState(TerminalState newState);
void processRfidCard();
void handleKeypadInput();
void handlePacingInterlock();
void handleOfflineCardTap(const String &cardUid);
void handleNetworkResponse(const NetResponseEvent &resp);
void handleCloudStatusUpdate(const CloudStatusMessage &cloudMsg);
void networkWorkerTask(void *pvParameters);
String getCardUidHex(MFRC522::Uid *uid);
void getCurrent12HourClock(char* timeBuf, size_t timeSize, char* dateBuf, size_t dateSize);
void copyApiToNetResponse(const TerminalApiResponse &api, const char* originalPayload, NetResponseEvent &out);

// Helper: Convert TerminalApiResponse to POD NetResponseEvent for FreeRTOS queue transfer
void copyApiToNetResponse(const TerminalApiResponse &api, const char* originalPayload, NetResponseEvent &out) {
    out.httpCode = api.httpCode;
    strncpy(out.code, api.code.c_str(), sizeof(out.code) - 1);
    out.code[sizeof(out.code) - 1] = '\0';

    strncpy(out.message, api.message.c_str(), sizeof(out.message) - 1);
    out.message[sizeof(out.message) - 1] = '\0';

    strncpy(out.studentName, api.studentName.c_str(), sizeof(out.studentName) - 1);
    out.studentName[sizeof(out.studentName) - 1] = '\0';

    strncpy(out.studentId, api.studentId.c_str(), sizeof(out.studentId) - 1);
    out.studentId[sizeof(out.studentId) - 1] = '\0';

    strncpy(out.status, api.status.c_str(), sizeof(out.status) - 1);
    out.status[sizeof(out.status) - 1] = '\0';

    strncpy(out.sectionCode, api.sectionCode.c_str(), sizeof(out.sectionCode) - 1);
    out.sectionCode[sizeof(out.sectionCode) - 1] = '\0';

    strncpy(out.facultyName, api.facultyName.c_str(), sizeof(out.facultyName) - 1);
    out.facultyName[sizeof(out.facultyName) - 1] = '\0';

    out.isRegistered = api.isRegistered;

    strncpy(out.cardHolder, api.cardHolder.c_str(), sizeof(out.cardHolder) - 1);
    out.cardHolder[sizeof(out.cardHolder) - 1] = '\0';

    strncpy(out.facultyUid, api.facultyUid.c_str(), sizeof(out.facultyUid) - 1);
    out.facultyUid[sizeof(out.facultyUid) - 1] = '\0';

    out.sectionCount = api.sectionCount;
    for (int i = 0; i < api.sectionCount && i < 4; i++) {
        strncpy(out.sectionCodes[i], api.sectionCodes[i], sizeof(out.sectionCodes[0]) - 1);
        out.sectionCodes[i][sizeof(out.sectionCodes[0]) - 1] = '\0';
    }

    if (originalPayload != NULL) {
        strncpy(out.originalPayload, originalPayload, sizeof(out.originalPayload) - 1);
        out.originalPayload[sizeof(out.originalPayload) - 1] = '\0';
    } else {
        out.originalPayload[0] = '\0';
    }
}

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
// FREERTOS BACKGROUND NETWORK WORKER TASK (PINNED TO CORE 0)
// =========================================================================
void networkWorkerTask(void *pvParameters) {
    Serial.println(F("[TASK] NetworkWorkerTask started on Core 0."));
    unsigned long lastStatusSync = 0;
    unsigned long lastOfflineSync = 0;

    bool lastIsLocked = isTerminalLocked;
    String lastSection = currentSectionCode;
    bool lastEnrollActive = isEnrollmentMode;
    String lastRole = enrollmentTargetRole;
    String lastName = enrollmentTargetName;

    while (true) {
        // While captive portal SoftAP provisioning is active, pause background network operations
        // to prevent mode switching back to STA and radio shut-off.
        if (captivePortal.isActive()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        unsigned long now = millis();

        // 1. Service any pending interactive requests with top priority
        NetRequestEvent req;
        if (netRequestQueue && xQueueReceive(netRequestQueue, &req, pdMS_TO_TICKS(40)) == pdTRUE) {
            if (req.type == NET_EVENT_RFID_TAP) {
                Serial.printf(F("[TASK Core 0] Servicing RFID tap: %s\n"), req.payload);
                TerminalApiResponse apiResp;
                netClient.postRfidTap(String(req.payload), apiResp);

                NetResponseEvent resp;
                copyApiToNetResponse(apiResp, req.payload, resp);
                resp.type = NET_EVENT_RFID_TAP;
                if (netResponseQueue) {
                    xQueueSend(netResponseQueue, &resp, pdMS_TO_TICKS(200));
                }
            } else if (req.type == NET_EVENT_KEYPAD_OTP) {
                Serial.printf(F("[TASK Core 0] Servicing Keypad OTP: %s\n"), req.payload);
                TerminalApiResponse apiResp;
                netClient.postKeypadOtp(String(req.payload), apiResp);

                NetResponseEvent resp;
                copyApiToNetResponse(apiResp, req.payload, resp);
                resp.type = NET_EVENT_KEYPAD_OTP;
                if (netResponseQueue) {
                    xQueueSend(netResponseQueue, &resp, pdMS_TO_TICKS(200));
                }
            } else if (req.type == NET_EVENT_SELECT_SECTION) {
                int secIndex = 1;
                char facUid[32] = {0};
                sscanf(req.payload, "%d,%31s", &secIndex, facUid);
                Serial.printf(F("[TASK Core 0] Servicing Select Section: index %d, faculty %s\n"), secIndex, facUid);
                TerminalApiResponse apiResp;
                netClient.postSelectSection(secIndex, String(facUid), apiResp);

                NetResponseEvent resp;
                copyApiToNetResponse(apiResp, req.payload, resp);
                resp.type = NET_EVENT_SELECT_SECTION;
                if (netResponseQueue) {
                    xQueueSend(netResponseQueue, &resp, pdMS_TO_TICKS(200));
                }
            } else if (req.type == NET_EVENT_FORCE_RECONNECT) {
                Serial.println(F("[TASK Core 0] Immediate Wi-Fi reconnect triggered."));
                netClient.forceReconnectNow();
            }
        }

        // Cooperative yield to ESP-IDF Wi-Fi/LwIP stack on Core 0
        vTaskDelay(pdMS_TO_TICKS(10));
        now = millis();

        // 2. Wi-Fi state machine maintenance
        netClient.maintainConnection(now);

        // 3. Periodic Cloud Status Sync (every 2500ms when connected)
        if (netClient.isConnected() && (now - lastStatusSync >= 2500)) {
            // Prioritize interactive user taps over background poll
            if (netRequestQueue && uxQueueMessagesWaiting(netRequestQueue) == 0) {
                lastStatusSync = now;
                bool cloudIsLocked = lastIsLocked;
                String cloudSection = lastSection;
                bool cloudEnrollmentActive = lastEnrollActive;
                String cloudTargetRole = lastRole;
                String cloudTargetName = lastName;
                bool cloudQuiet = audioLed.isQuiet();

                if (netClient.syncTerminalStatus(cloudIsLocked, cloudSection, cloudEnrollmentActive, cloudTargetRole, cloudTargetName, cloudQuiet)) {
                    audioLed.setQuietMode(cloudQuiet);

                    bool changed = (cloudIsLocked != lastIsLocked) ||
                                   (cloudSection != lastSection) ||
                                   (cloudEnrollmentActive != lastEnrollActive) ||
                                   (cloudTargetRole != lastRole) ||
                                   (cloudTargetName != lastName);

                    if (changed) {
                        lastIsLocked = cloudIsLocked;
                        lastSection = cloudSection;
                        lastEnrollActive = cloudEnrollmentActive;
                        lastRole = cloudTargetRole;
                        lastName = cloudTargetName;

                        CloudStatusMessage msg;
                        msg.isLocked = cloudIsLocked;
                        strncpy(msg.sectionCode, cloudSection.c_str(), sizeof(msg.sectionCode) - 1);
                        msg.sectionCode[sizeof(msg.sectionCode) - 1] = '\0';
                        msg.enrollmentActive = cloudEnrollmentActive;
                        strncpy(msg.targetRole, cloudTargetRole.c_str(), sizeof(msg.targetRole) - 1);
                        msg.targetRole[sizeof(msg.targetRole) - 1] = '\0';
                        strncpy(msg.targetName, cloudTargetName.c_str(), sizeof(msg.targetName) - 1);
                        msg.targetName[sizeof(msg.targetName) - 1] = '\0';

                        if (cloudStatusQueue) {
                            xQueueSend(cloudStatusQueue, &msg, pdMS_TO_TICKS(50));
                        }
                    }
                }
            }
        }

        // Cooperative yield
        vTaskDelay(pdMS_TO_TICKS(10));
        now = millis();

        // 4. Background Offline Attendance Batch Sync (every 4000ms when records exist)
        if (netClient.isConnected() && (offlineStorage.getPendingCount() > 0) && (now - lastOfflineSync >= 4000)) {
            if (netRequestQueue && uxQueueMessagesWaiting(netRequestQueue) == 0) {
                lastOfflineSync = now;
                String actionToSync = offlineStorage.getOfflineSessionAction();
                String facultyUid = offlineStorage.getOfflineFacultyUid();

                // Safety guard: if session is active locally, don't accidentally CLOSE
                if (!lastIsLocked && actionToSync == "CLOSE") {
                    actionToSync = "CONTINUE";
                }

                std::vector<OfflineAttendanceRecord> records = offlineStorage.getRecords();
                if (!records.empty()) {
                    int syncedCount = 0;
                    bool ok = netClient.postOfflineSync(
                        TERMINAL_ID,
                        actionToSync,
                        facultyUid,
                        records,
                        syncedCount
                    );
                    if (ok) {
                        offlineStorage.clearRecords();
                        Serial.printf(F("[OFFLINE SYNC] Background upload of %d records succeeded on Core 0.\n"), syncedCount);
                    }
                }
            }
        }

        // Cooperative yield
        vTaskDelay(pdMS_TO_TICKS(10));
        now = millis();

        // 5. Periodic 60-Second Fleet Health Heartbeat (P5 Watchdog Telemetry)
        // Dispatches immediately upon initial Wi-Fi connection, then every 60s
        static unsigned long lastHeartbeatTime = 0;
        if (netClient.isConnected() && (lastHeartbeatTime == 0 || (now - lastHeartbeatTime >= 60000))) {
            if (netRequestQueue && uxQueueMessagesWaiting(netRequestQueue) == 0) {
                lastHeartbeatTime = now;
                int rssi = WiFi.RSSI();
                uint32_t freeHeap = ESP.getFreeHeap();
                uint32_t uptimeSec = millis() / 1000;

                // Read reader status safely from cached hardware check (avoid cross-core SPI collision)
                const char* readerStat = g_readerStatus ? g_readerStatus : "MFRC522 OK";

                bool serverQuiet = audioLed.isQuiet();
                if (netClient.postHeartbeat(rssi, freeHeap, uptimeSec, readerStat, "v1.0.4-PRO", serverQuiet)) {
                    audioLed.setQuietMode(serverQuiet);
                    Serial.printf(F("[HEARTBEAT] Emitted telemetry: RSSI %d dBm | FreeHeap %u bytes | Reader %s | Quiet %s\n"),
                                  rssi, freeHeap, readerStat, serverQuiet ? "ON" : "OFF");
                }
            }
        }

        // Cooperative yield to keep Core 0 responsive
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

// =========================================================================
// SYSTEM INITIALIZATION (SETUP)
// =========================================================================
void setup() {
    // Hardware Brown-Out Detector (BOD) remains active (register disable removed for power stability)
    Serial.begin(SERIAL_BAUD_RATE);
    delay(250);

    Serial.println();
    Serial.println(F("======================================================"));
    Serial.println(F("🎻 ORCHESTRA ESP32 DOORWAY INGRESS TERMINAL STARTING"));
    Serial.printf(F("🏷️ Terminal Node ID:    %s\n"), TERMINAL_ID);
    Serial.printf(F("📍 Classroom Location:  %s\n"), ROOM_NUMBER);
    Serial.printf(F("🛡️ Security API Token:  %s\n"), HARDWARE_API_KEY);
    Serial.println(F("======================================================"));

    // 0. Initialize Offline Storage & Local Professor Cache from NVS
    offlineStorage.begin();

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
        g_readerStatus = "ERR_SPI";
        Serial.println(F("[WARNING] MFRC522 communication failed or not connected. Check SPI wiring."));
    } else {
        g_readerStatus = "MFRC522 OK";
        // Boost MFRC522 receiver sensitivity to maximum (48 dB) for instant coupling and longer range
        mfrc522.PCD_SetAntennaGain(mfrc522.RxGain_max);
        mfrc522.PCD_AntennaOn();
        Serial.println(F("[HARDWARE] MFRC522 13.56MHz Transceiver OK (Antenna Gain set to 48dB MAX)."));
    }

    // 4. Initialize Wi-Fi Network Subsystem (Fast Non-blocking probe)
    displayMgr.showConnectingWiFi("Scanning Wi-Fi...");
    netClient.begin();

    bool wifiConnected = netClient.quickBootConnect(4500);

    if (wifiConnected && netClient.isConnected()) {
        displayMgr.showConnectingWiFi(WiFi.SSID().c_str());
        delay(400);

        // Sync Initial Session State with Cloud Backend
        bool lockedFromCloud = true;
        String sectionFromCloud = "";
        bool enrollFromCloud = false;
        String roleFromCloud = "STUDENT";
        String nameFromCloud = "";
        bool quietFromCloud = false;
        if (netClient.syncTerminalStatus(lockedFromCloud, sectionFromCloud, enrollFromCloud, roleFromCloud, nameFromCloud, quietFromCloud)) {
            isTerminalLocked = lockedFromCloud;
            currentSectionCode = sectionFromCloud;
            isEnrollmentMode = enrollFromCloud;
            enrollmentTargetRole = roleFromCloud;
            enrollmentTargetName = nameFromCloud;
            audioLed.setQuietMode(quietFromCloud);
            Serial.printf(F("[CLOUD] Initial terminal state synced: %s (Section: %s | Enrollment: %s (%s) | Quiet: %s)\n"),
                          isTerminalLocked ? "LOCKED" : "OPEN", currentSectionCode.c_str(),
                          isEnrollmentMode ? enrollmentTargetRole.c_str() : "OFF", enrollmentTargetName.c_str(),
                          quietFromCloud ? "ON" : "OFF");
        }
    } else {
        Serial.println(F("[WIFI] Configured Wi-Fi failed after retry timeout! Launching Fallback Captive Portal..."));
        captivePortal.start(TERMINAL_ID, BACKEND_BASE_URL);
    }

    // 5. Initialize FreeRTOS Queues & Network Worker Task pinned to Core 0
    netRequestQueue = xQueueCreate(8, sizeof(NetRequestEvent));
    netResponseQueue = xQueueCreate(8, sizeof(NetResponseEvent));
    cloudStatusQueue = xQueueCreate(4, sizeof(CloudStatusMessage));

    xTaskCreatePinnedToCore(
        networkWorkerTask,
        "NetworkWorker",
        8192,
        NULL,
        1,
        &networkTaskHandle,
        0
    );

    // Initial state transition
    if (captivePortal.isActive()) {
        changeState(STATE_CAPTIVE_PORTAL);
    } else if (isEnrollmentMode) {
        changeState(STATE_ENROLLMENT_SCAN);
    } else if (isTerminalLocked) {
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

    // 1. Receive asynchronous cloud session state updates from Core 0 NetworkWorkerTask
    CloudStatusMessage cloudMsg;
    if (cloudStatusQueue && xQueueReceive(cloudStatusQueue, &cloudMsg, 0) == pdTRUE) {
        handleCloudStatusUpdate(cloudMsg);
    }

    // 2. Periodic 1-second display refresh during idle states to keep the 12-hour clock ticking live
    static unsigned long lastClockTick = 0;
    if (now - lastClockTick >= 1000) {
        lastClockTick = now;
        if (currentState == STATE_LOCKED) {
            char timeBuf[16] = {0};
            char dateBuf[24] = {0};
            getCurrent12HourClock(timeBuf, sizeof(timeBuf), dateBuf, sizeof(dateBuf));
            displayMgr.showLockedScreen(TERMINAL_ID, ROOM_NUMBER, timeBuf, dateBuf, netClient.isConnected());
        } else if (currentState == STATE_IDLE_SCAN) {
            char timeBuf[16] = {0};
            char dateBuf[24] = {0};
            getCurrent12HourClock(timeBuf, sizeof(timeBuf), dateBuf, sizeof(dateBuf));
            displayMgr.showIdleScanScreen(currentSectionCode.c_str(), 0, netClient.isConnected(), timeBuf, dateBuf);
        } else if (currentState == STATE_ENROLLMENT_SCAN) {
            displayMgr.showEnrollmentScanningScreen(enrollmentTargetRole.c_str(), enrollmentTargetName.c_str());
        }
    }

    // 3. State Machine Dispatcher
    switch (currentState) {
        case STATE_CAPTIVE_PORTAL: {
            audioLed.setLeds(true, true); // Dual solid LEDs in captive portal setup
            captivePortal.handle();

            char key = matrixKeypad.getKey();
            if (key) {
                audioLed.playKeypadClick();
                if (key == '*') {
                    Serial.println(F("[CAPTIVE PORTAL] '*' pressed! Rebooting ESP32 into network sweep..."));
                    displayMgr.showConnectingWiFi("REBOOTING...");
                    delay(400);
                    ESP.restart();
                } else if (key == 'D') {
                    Serial.println(F("[CAPTIVE PORTAL] 'D' pressed! Exiting captive portal mode on keypad request."));
                    captivePortal.stop();
                    WiFi.mode(WIFI_OFF);
                    changeState(STATE_LOCKED);
                }
            }
            break;
        }

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

        case STATE_ENROLLMENT_SCAN: {
            audioLed.setLeds(true, true); // Dual solid LEDs indicates admin enrollment mode
            processRfidCard();            // Scans any RFID tag to link directly to new user
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

        case STATE_SECTION_SELECTION: {
            unsigned long elapsed = now - sectionSelectionStartTime;
            if (elapsed >= SECTION_SELECTION_TIMEOUT_MS) {
                Serial.println(F("[DISAMBIGUATION] Selection timed out. Auto-selecting earliest section (1)..."));
                NetRequestEvent req;
                req.type = NET_EVENT_SELECT_SECTION;
                snprintf(req.payload, sizeof(req.payload), "1,%s", disambiguationFacultyUid);
                req.timestamp = millis();
                if (netRequestQueue) {
                    xQueueSend(netRequestQueue, &req, 0);
                }
                displayMgr.showProcessing("Activating Class...");
                stateProcessingStartTime = millis();
                changeState(STATE_PROCESSING);
                break;
            }
            static unsigned long lastSecTick = 0;
            if (now - lastSecTick >= 500) {
                lastSecTick = now;
                unsigned long remainingSecs = (SECTION_SELECTION_TIMEOUT_MS - elapsed) / 1000;
                displayMgr.showSectionSelectionScreen(disambiguationSections, disambiguationCount, remainingSecs);
            }
            handleKeypadInput();
            break;
        }

        case STATE_PROCESSING: {
            NetResponseEvent resp;
            if (netResponseQueue && xQueueReceive(netResponseQueue, &resp, 0) == pdTRUE) {
                handleNetworkResponse(resp);
            } else if (millis() - stateProcessingStartTime >= 4000) {
                Serial.println(F("[TIMEOUT] Network worker timeout! Fallback to offline storage."));
                handleOfflineCardTap(currentPendingUid);
            }
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

    // High-frequency sensor scanning yields briefly to FreeRTOS IDLE task on Core 1
    vTaskDelay(pdMS_TO_TICKS(1));
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
        case STATE_CAPTIVE_PORTAL:
            audioLed.setLeds(true, true);
            displayMgr.showCaptivePortal(captivePortal.getApSsid().c_str(), "192.168.4.1");
            Serial.println(F("[FSM] State -> STATE_CAPTIVE_PORTAL (Wi-Fi Provisioning AP Active)"));
            break;

        case STATE_LOCKED:
            isTerminalLocked = true;
            audioLed.setLeds(false, true);
            displayMgr.showLockedScreen(TERMINAL_ID, ROOM_NUMBER, timeBuf, dateBuf, netClient.isConnected());
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

        case STATE_SECTION_SELECTION:
            sectionSelectionStartTime = millis();
            displayMgr.showSectionSelectionScreen(disambiguationSections, disambiguationCount, SECTION_SELECTION_TIMEOUT_MS / 1000);
            Serial.printf(F("[FSM] State -> STATE_SECTION_SELECTION (%d sections to select)\n"), disambiguationCount);
            break;

        case STATE_ENROLLMENT_SCAN:
            audioLed.setLeds(true, true);
            displayMgr.showEnrollmentScanningScreen(enrollmentTargetRole.c_str(), enrollmentTargetName.c_str());
            Serial.printf(F("[FSM] State -> STATE_ENROLLMENT_SCAN (Live Admin Provisioning for %s: %s)\n"),
                          enrollmentTargetRole.c_str(), enrollmentTargetName.c_str());
            break;

        case STATE_PROCESSING:
            Serial.println(F("[FSM] State -> STATE_PROCESSING (Awaiting Core 0 Network Response)"));
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

    // Throttle display redraws to 10 FPS (100ms) to free up CPU and I2C bus bandwidth
    static unsigned long lastPacingDraw = 0;
    if (millis() - lastPacingDraw >= 100) {
        lastPacingDraw = millis();
        displayMgr.showPacingCooldown(remainingSeconds);
    }

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
// OFFLINE CARD TAP & LOCAL SESSION TRUST HANDLER
// =========================================================================
void handleOfflineCardTap(const String &cardUid) {
    netClient.resetCooldown(millis()); // Ingress Active Hold: reset 60s Wi-Fi cooldown on every tap
    Serial.printf(F("[OFFLINE TAP] Evaluating card 0x%s against local cache...\n"), cardUid.c_str());

    String profName, profRole;
    bool isProf = offlineStorage.isAuthorizedProfessor(cardUid, profName, profRole);

    if (isProf) {
        // Faculty Rapid Re-trigger Debounce Guard
        if (millis() - lastFacultyToggleTime < FACULTY_TOGGLE_COOLDOWN_MS) {
            Serial.printf(F("[DEBOUNCE OFFLINE] Faculty badge 0x%s presented within toggle cooldown (%lu ms). Suppressing tap.\n"),
                          cardUid.c_str(), millis() - lastFacultyToggleTime);
            return;
        }

        // Faculty Master Badge tapped offline!
        lastFacultyToggleTime = millis();
        if (isTerminalLocked) {
            // UNLOCK & OPEN OFFLINE SESSION
            isTerminalLocked = false;
            offlineStorage.markOfflineSessionStarted(cardUid);
            audioLed.cueSessionStateChanged(true);
            displayMgr.showOfflineSessionState(true, profName.c_str());
            stateAfterFeedback = STATE_IDLE_SCAN;
            changeState(STATE_FEEDBACK_DISPLAY);
        } else {
            // LOCK & CLOSE OFFLINE SESSION: Enforce Session Ownership!
            // Professor 2 cannot terminate Professor 1's session (only initiating prof or ADMIN)
            String initiatingUid = offlineStorage.getOfflineFacultyUid();
            String cleanUid = OfflineStorageManager::normalizeUid(cardUid);

            if (profRole != "ADMIN" && initiatingUid.length() > 0 && !cleanUid.equalsIgnoreCase(initiatingUid)) {
                Serial.printf(F("[SECURITY OFFLINE] Session was opened by Prof 0x%s. Denied close attempt by 0x%s\n"),
                              initiatingUid.c_str(), cleanUid.c_str());
                audioLed.cueAccessDenied();
                displayMgr.showAccessDenied("NOT OWNER", "Session Owned By", "Another Professor");
                stateAfterFeedback = STATE_IDLE_SCAN;
                changeState(STATE_FEEDBACK_DISPLAY);
                return;
            }

            isTerminalLocked = true;
            offlineStorage.markOfflineSessionClosed(cardUid);
            audioLed.cueSessionStateChanged(false);
            displayMgr.showOfflineSessionState(false, profName.c_str());
            stateAfterFeedback = STATE_LOCKED;
            changeState(STATE_FEEDBACK_DISPLAY);
        }
        return;
    }

    // Not a faculty master badge
    if (isTerminalLocked) {
        // Terminal locked: Student badge presented
        audioLed.cueAccessDenied();
        displayMgr.showAccessDenied("TERMINAL LOCKED", "Scan Prof Badge", "Offline Mode");
        stateAfterFeedback = STATE_LOCKED;
        changeState(STATE_FEEDBACK_DISPLAY);
        return;
    }

    // Terminal is OPEN: Verify student credentials against local enrollment roster
    String sName, sId;
    bool isStudent = offlineStorage.findStudent(cardUid, sName, sId);

    // STRICT DEFENSE: Unregistered, unidentified, or foreign NFC cards are REJECTED!
    if (!isStudent) {
        Serial.printf(F("[SECURITY OFFLINE] Unregistered or foreign NFC card 0x%s REJECTED.\n"), cardUid.c_str());
        audioLed.cueAccessDenied();
        displayMgr.showAccessDenied("UNREGISTERED TAG", "Badge Not Found", "Register Online");
        stateAfterFeedback = STATE_IDLE_SCAN;
        changeState(STATE_FEEDBACK_DISPLAY);
        return;
    }

    // Local anti-passback defense for verified student
    if (offlineStorage.isAlreadyLogged(cardUid)) {
        Serial.printf(F("[ANTI-PASSBACK OFFLINE] Student 0x%s (%s) already logged in buffer!\n"),
                      cardUid.c_str(), sName.c_str());
        audioLed.cueAccessDenied();
        displayMgr.showAccessDenied("ANTI-PASSBACK", "Already Logged", "In Offline Cache");
        stateAfterFeedback = STATE_IDLE_SCAN;
        changeState(STATE_FEEDBACK_DISPLAY);
        return;
    }

    // Append authorized student record to offline queue (NVS backed)
    bool saved = offlineStorage.addRecord(cardUid);
    if (saved) {
        audioLed.cueAccessGranted();

        // Recognized student: Show actual name, student ID, and offline ingress badge!
        char statBuf[24];
        snprintf(statBuf, sizeof(statBuf), "OFFLINE #%d", offlineStorage.getPendingCount());
        displayMgr.showAccessGranted(sName.c_str(), statBuf, sId.c_str());

        lastStudentScanTime = millis();
        stateAfterFeedback = STATE_PACING_COOLDOWN; // 3.0s anti-proxy interlock
        changeState(STATE_FEEDBACK_DISPLAY);
    } else {
        audioLed.cueAccessDenied();
        displayMgr.showAccessDenied("QUEUE FULL", "Offline Storage", "Reconnect Wi-Fi");
        stateAfterFeedback = STATE_IDLE_SCAN;
        changeState(STATE_FEEDBACK_DISPLAY);
    }
}

// =========================================================================
// SPI MFRC522 RFID CARD PROCESSING
// =========================================================================
void processRfidCard() {
    if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
        return;
    }

    // Instant physical feedback (< 1ms) confirming antenna coupling
    audioLed.playKeypadClick();

    // Ingress Active Hold & Instant Priority Abort:
    // Reset 60s cooldown and immediately abort any background Wi-Fi scan in flight
    netClient.resetCooldown(millis());

    String cardUid = getCardUidHex(&(mfrc522.uid));
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();

    // Faculty Rapid Re-trigger & Card-Held Debounce Guard:
    // If a faculty badge was toggled within 4000ms, suppress subsequent reads of faculty cards
    String dummyName, dummyRole;
    bool isFacultyBadge = offlineStorage.isAuthorizedProfessor(cardUid, dummyName, dummyRole);
    if (isFacultyBadge && (millis() - lastFacultyToggleTime < FACULTY_TOGGLE_COOLDOWN_MS)) {
        Serial.printf(F("[DEBOUNCE] Faculty badge 0x%s presented within toggle cooldown (%lu ms). Suppressing tap.\n"),
                      cardUid.c_str(), millis() - lastFacultyToggleTime);
        return;
    }

    Serial.println();
    Serial.printf(F("[RFID EVENT] Interrogated ISO 14443A Tag: 0x%s\n"), cardUid.c_str());

    // If offline / Wi-Fi disconnected: directly invoke offline trust handler (ZERO intermediate OLED frame lag)
    if (!netClient.isConnected()) {
        Serial.println(F("[OFFLINE] Fast-path local offline storage validation..."));
        handleOfflineCardTap(cardUid);
        return;
    }

    // ONLINE: Dispatch via FreeRTOS queue to Core 0 NetworkWorkerTask (Zero loop blocking!)
    displayMgr.showProcessing("Validating Tag...");
    currentPendingUid = cardUid;
    stateProcessingStartTime = millis();

    NetRequestEvent req;
    req.type = NET_EVENT_RFID_TAP;
    strncpy(req.payload, cardUid.c_str(), sizeof(req.payload) - 1);
    req.payload[sizeof(req.payload) - 1] = '\0';
    req.timestamp = millis();

    if (!netRequestQueue || xQueueSend(netRequestQueue, &req, 0) != pdTRUE) {
        Serial.println(F("[QUEUE FULL] Network request queue full! Fallback to offline storage."));
        handleOfflineCardTap(cardUid);
        return;
    }

    changeState(STATE_PROCESSING);
}

// =========================================================================
// ASYNCHRONOUS CLOUD STATUS & NETWORK RESPONSE HANDLERS
// =========================================================================
void handleCloudStatusUpdate(const CloudStatusMessage &cloudMsg) {
    bool cloudIsLocked = cloudMsg.isLocked;
    String cloudSection = String(cloudMsg.sectionCode);
    bool cloudEnrollmentActive = cloudMsg.enrollmentActive;
    String cloudTargetRole = String(cloudMsg.targetRole);
    String cloudTargetName = String(cloudMsg.targetName);

    // CASE 0: Live Admin RFID Enrollment Mode toggled
    if (!isEnrollmentMode && cloudEnrollmentActive) {
        Serial.printf(F("[REMOTE EVENT] Admin initiated RFID Enrollment Mode for %s (%s)!\n"),
                      cloudTargetRole.c_str(), cloudTargetName.c_str());
        isEnrollmentMode = true;
        enrollmentTargetRole = cloudTargetRole;
        enrollmentTargetName = cloudTargetName;
        audioLed.cueAccessGranted();
        changeState(STATE_ENROLLMENT_SCAN);
    } else if (isEnrollmentMode && !cloudEnrollmentActive) {
        Serial.println(F("[REMOTE EVENT] Admin closed RFID Enrollment Mode. Restoring state..."));
        isEnrollmentMode = false;
        enrollmentTargetName = "";
        audioLed.playKeypadClick();
        changeState(isTerminalLocked ? STATE_LOCKED : STATE_IDLE_SCAN);
    } else if (isEnrollmentMode && cloudEnrollmentActive && (cloudTargetRole != enrollmentTargetRole || cloudTargetName != enrollmentTargetName)) {
        enrollmentTargetRole = cloudTargetRole;
        enrollmentTargetName = cloudTargetName;
        displayMgr.showEnrollmentScanningScreen(enrollmentTargetRole.c_str(), enrollmentTargetName.c_str());
    }
    // CASE A & B: Normal Session State Transitions when not in enrollment mode
    else if (!isEnrollmentMode) {
        // CASE A: Terminal was ACTIVE / OPEN locally, but session was TERMINATED on website!
        if (!isTerminalLocked && cloudIsLocked) {
            Serial.println(F("[REMOTE EVENT] Active session terminated from website! Arming hardware lock..."));
            lastFacultyToggleTime = millis();
            isTerminalLocked = true;
            offlineStorage.resetSessionState();
            audioLed.cueSessionStateChanged(false); // Auditory tone & red LED feedback
            displayMgr.showSessionStateScreen(false, currentSectionCode.c_str());
            stateAfterFeedback = STATE_LOCKED;
            changeState(STATE_FEEDBACK_DISPLAY);
        }
        // CASE B: Terminal was LOCKED locally, but session was STARTED on website!
        else if (isTerminalLocked && !cloudIsLocked) {
            Serial.println(F("[REMOTE EVENT] Session opened from website! Unlocking hardware terminal..."));
            lastFacultyToggleTime = millis();
            isTerminalLocked = false;
            offlineStorage.resetSessionState();
            currentSectionCode = cloudSection;
            audioLed.cueSessionStateChanged(true); // Auditory chime & green LED feedback
            displayMgr.showSessionStateScreen(true, currentSectionCode.c_str(), "Web Console");
            stateAfterFeedback = STATE_IDLE_SCAN;
            changeState(STATE_FEEDBACK_DISPLAY);
        }
    }
}

void handleNetworkResponse(const NetResponseEvent &resp) {
    if (resp.type == NET_EVENT_KEYPAD_OTP) {
        if (strcmp(resp.code, "ACCESS_GRANTED") == 0) {
            audioLed.cueAccessGranted();
            displayMgr.showAccessGranted(resp.studentName, resp.status, resp.studentId);
            lastStudentScanTime = millis();
            stateAfterFeedback = STATE_PACING_COOLDOWN;
            changeState(STATE_FEEDBACK_DISPLAY);
        } else if (strcmp(resp.code, "ALREADY_RECORDED") == 0) {
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
        return;
    }

    if (resp.type == NET_EVENT_SELECT_SECTION) {
        Serial.printf(F("[SELECT SECTION RESPONSE] HTTP %d | Code: %s | Section: %s\n"),
                      resp.httpCode, resp.code, resp.sectionCode);
        if (strcmp(resp.code, "SESSION_STARTED") == 0) {
            lastFacultyToggleTime = millis();
            isTerminalLocked = false;
            offlineStorage.resetSessionState();
            currentSectionCode = String(resp.sectionCode);
            audioLed.cueSessionStateChanged(true);
            displayMgr.showSessionStateScreen(true, currentSectionCode.c_str(), resp.facultyName);
            stateAfterFeedback = STATE_IDLE_SCAN;
            changeState(STATE_FEEDBACK_DISPLAY);
        } else {
            audioLed.cueAccessDenied();
            displayMgr.showAccessDenied("SELECT FAILED", resp.message[0] ? resp.message : "Section Error", "Tap Badge Again");
            stateAfterFeedback = STATE_LOCKED;
            changeState(STATE_FEEDBACK_DISPLAY);
        }
        return;
    }

    // Otherwise: NET_EVENT_RFID_TAP
    String cardUid = String(resp.originalPayload);

    // If request failed due to actual network/transport failure (timeout or unreachable), fallback to offline storage
    if (resp.httpCode <= 0 || strcmp(resp.code, "OFFLINE") == 0 || strcmp(resp.code, "ERR_HTTP_FAIL") == 0) {
        Serial.println(F("[OFFLINE FALLBACK] Server unreachable during tap. Saving locally..."));
        handleOfflineCardTap(cardUid);
        return;
    }

    Serial.printf(F("[BACKEND RESPONSE] HTTP %d | Code: %s | Message: %s\n"),
                  resp.httpCode, resp.code, resp.message);

    if (strcmp(resp.code, "SECTION_DISAMBIGUATION_REQUIRED") == 0) {
        disambiguationCount = resp.sectionCount;
        for (int i = 0; i < disambiguationCount && i < 4; i++) {
            strncpy(disambiguationSections[i], resp.sectionCodes[i], sizeof(disambiguationSections[0]) - 1);
            disambiguationSections[i][sizeof(disambiguationSections[0]) - 1] = '\0';
        }
        strncpy(disambiguationFacultyUid, resp.facultyUid, sizeof(disambiguationFacultyUid) - 1);
        disambiguationFacultyUid[sizeof(disambiguationFacultyUid) - 1] = '\0';
        sectionSelectionStartTime = millis();
        audioLed.cueAccessGranted();
        changeState(STATE_SECTION_SELECTION);
    }
    else if (strcmp(resp.code, "ENROLLMENT_CAPTURED") == 0) {
        // Tag captured live for Admin Enrollment Mode
        audioLed.cueAccessGranted(); // Positive chirp
        displayMgr.showEnrollmentCapturedScreen(
            cardUid.c_str(),
            enrollmentTargetRole.c_str(),
            resp.isRegistered,
            resp.cardHolder
        );
        stateAfterFeedback = isEnrollmentMode ? STATE_ENROLLMENT_SCAN : (isTerminalLocked ? STATE_LOCKED : STATE_IDLE_SCAN);
        changeState(STATE_FEEDBACK_DISPLAY);
    }
    else if (strcmp(resp.code, "SESSION_STARTED") == 0) {
        // Faculty Master Badge tapped to open session
        lastFacultyToggleTime = millis();
        isTerminalLocked = false;
        offlineStorage.resetSessionState();
        currentSectionCode = String(resp.sectionCode);
        audioLed.cueSessionStateChanged(true);
        displayMgr.showSessionStateScreen(true, currentSectionCode.c_str(), resp.facultyName);
        stateAfterFeedback = STATE_IDLE_SCAN;
        changeState(STATE_FEEDBACK_DISPLAY);
    }
    else if (strcmp(resp.code, "SESSION_CLOSED") == 0) {
        // Faculty Master Badge tapped to terminate session
        lastFacultyToggleTime = millis();
        isTerminalLocked = true;
        offlineStorage.resetSessionState();
        audioLed.cueSessionStateChanged(false);
        displayMgr.showSessionStateScreen(false, currentSectionCode.c_str());
        stateAfterFeedback = STATE_LOCKED;
        changeState(STATE_FEEDBACK_DISPLAY);
    }
    else if (strcmp(resp.code, "ACCESS_GRANTED") == 0) {
        // Valid student card scanned on time or late
        audioLed.cueAccessGranted();
        displayMgr.showAccessGranted(resp.studentName, resp.status, resp.studentId);
        lastStudentScanTime = millis();
        stateAfterFeedback = STATE_PACING_COOLDOWN; // Activate 3.0s anti-proxy interlock
        changeState(STATE_FEEDBACK_DISPLAY);
    }
    else if (strcmp(resp.code, "NOT_ENROLLED") == 0) {
        // Student not enrolled in this section
        audioLed.cueAccessDenied();
        displayMgr.showAccessDenied("NOT ENROLLED", "Not In Class", "Check Faculty");
        stateAfterFeedback = STATE_IDLE_SCAN;
        changeState(STATE_FEEDBACK_DISPLAY);
    }
    else if (strcmp(resp.code, "UNAUTHORIZED_FACULTY") == 0 || strcmp(resp.code, "NOT_SESSION_OWNER") == 0) {
        // Unauthorized faculty (not assigned to this room/terminal, or attempting to close another's session)
        audioLed.cueAccessDenied();
        displayMgr.showAccessDenied("UNAUTHORIZED", "Not Assigned", "Wrong Instructor");
        stateAfterFeedback = isTerminalLocked ? STATE_LOCKED : STATE_IDLE_SCAN;
        changeState(STATE_FEEDBACK_DISPLAY);
    }
    else if (strcmp(resp.code, "ALREADY_RECORDED") == 0) {
        // Anti-passback violation (card already tapped for this session)
        audioLed.cueAccessDenied();
        displayMgr.showAccessDenied("ANTI-PASSBACK", "Already Recorded", "Attendance Logged");
        stateAfterFeedback = STATE_IDLE_SCAN;
        changeState(STATE_FEEDBACK_DISPLAY);
    }
    else if (strcmp(resp.code, "SESSION_LOCKED") == 0) {
        // Student card presented while terminal is locked
        audioLed.cueAccessDenied();
        displayMgr.showAccessDenied("TERMINAL LOCKED", "Tap Faculty Badge", "To Open Session");
        stateAfterFeedback = STATE_LOCKED;
        changeState(STATE_FEEDBACK_DISPLAY);
    }
    else if (strcmp(resp.code, "UNREGISTERED_CARD") == 0 || strcmp(resp.code, "UNREGISTERED_TAG") == 0) {
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

    // Double '#' shortcut detection on Locked, Idle, or initial OTP state to force Wi-Fi reconnect sweep
    static unsigned long lastHashPressTime = 0;
    if (key == '#') {
        unsigned long now = millis();
        if (now - lastHashPressTime <= 800 && lastHashPressTime > 0) {
            // Second '#' pressed within 800ms! Double '#' detected!
            lastHashPressTime = 0;
            Serial.println(F("[KEYPAD] Double '#' recognized! Dispatching background Wi-Fi scan via FreeRTOS..."));
            displayMgr.showConnectingWiFi("Scanning Wi-Fi...");
            NetRequestEvent req;
            req.type = NET_EVENT_FORCE_RECONNECT;
            req.payload[0] = '\0';
            req.timestamp = millis();
            if (netRequestQueue) {
                xQueueSend(netRequestQueue, &req, 0);
            }
            if (currentState == STATE_OTP_ENTRY) {
                otpBufferLength = 0;
                otpBuffer[0] = '\0';
                changeState(isTerminalLocked ? STATE_LOCKED : STATE_IDLE_SCAN);
            }
            return;
        }
        lastHashPressTime = now;
    } else {
        // Any other key resets the double '#' tracker
        lastHashPressTime = 0;
    }

    // Lecture Quiet Mode toggle shortcut on key 'C' during LOCKED or IDLE_SCAN
    if ((currentState == STATE_LOCKED || currentState == STATE_IDLE_SCAN) && key == 'C') {
        audioLed.toggleQuietMode();
        bool q = audioLed.isQuiet();
        Serial.printf(F("[KEYPAD] 'C' pressed! Toggled Lecture Quiet Mode: %s\n"),
                      q ? "ON (DAMPENED AUDIO)" : "OFF (NORMAL AUDIO)");
        displayMgr.showQuietModeNotice(q);
        delay(700);
        changeState(currentState);
        return;
    }

    // Fallback Captive Portal shortcut on key 'D' during LOCKED or IDLE_SCAN
    if ((currentState == STATE_LOCKED || currentState == STATE_IDLE_SCAN) && key == 'D') {
        Serial.println(F("[KEYPAD] 'D' pressed! Launching Fallback Captive Portal Wi-Fi Provisioning..."));
        captivePortal.start(TERMINAL_ID, BACKEND_BASE_URL);
        changeState(STATE_CAPTIVE_PORTAL);
        return;
    }

    // If in SECTION DISAMBIGUATION mode, process numeric choices 1..count
    if (currentState == STATE_SECTION_SELECTION) {
        if (key >= '1' && key <= ('0' + disambiguationCount)) {
            int selectedIndex = key - '0';
            Serial.printf(F("[KEYPAD] Section %d selected from OLED menu\n"), selectedIndex);
            NetRequestEvent req;
            req.type = NET_EVENT_SELECT_SECTION;
            snprintf(req.payload, sizeof(req.payload), "%d,%s", selectedIndex, disambiguationFacultyUid);
            req.timestamp = millis();
            if (netRequestQueue) {
                xQueueSend(netRequestQueue, &req, 0);
            }
            displayMgr.showProcessing("Activating Class...");
            stateProcessingStartTime = millis();
            changeState(STATE_PROCESSING);
        } else if (key == '*' || key == 'C') {
            audioLed.cueAccessDenied();
            Serial.println(F("[KEYPAD] Section selection aborted. Reverting to LOCKED."));
            changeState(STATE_LOCKED);
        }
        return;
    }

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
                if (!netClient.isConnected()) {
                    audioLed.cueAccessDenied();
                    displayMgr.showAccessDenied("OFFLINE MODE", "OTP Requires", "Active Wi-Fi Link");
                    stateAfterFeedback = STATE_IDLE_SCAN;
                    changeState(STATE_FEEDBACK_DISPLAY);
                    return;
                }

                Serial.printf(F("[KEYPAD] Dispatching 6-Digit OTP via FreeRTOS: %s\n"), otpBuffer);
                displayMgr.showProcessing("Validating OTP...");
                stateProcessingStartTime = millis();

                NetRequestEvent req;
                req.type = NET_EVENT_KEYPAD_OTP;
                strncpy(req.payload, otpBuffer, sizeof(req.payload) - 1);
                req.payload[sizeof(req.payload) - 1] = '\0';
                req.timestamp = millis();

                if (!netRequestQueue || xQueueSend(netRequestQueue, &req, 0) != pdTRUE) {
                    audioLed.cueAccessDenied();
                    displayMgr.showAccessDenied("QUEUE FULL", "Network Busy", "Try Again");
                    stateAfterFeedback = STATE_IDLE_SCAN;
                    changeState(STATE_FEEDBACK_DISPLAY);
                    return;
                }

                changeState(STATE_PROCESSING);
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
