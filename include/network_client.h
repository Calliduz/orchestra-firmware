#ifndef NETWORK_CLIENT_H
#define NETWORK_CLIENT_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>
#include <algorithm>
#include "esp_wifi.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "config.h"
#include "offline_storage.h"

struct TerminalApiResponse {
    int httpCode;
    String code;
    String message;
    String studentName;
    String studentId;
    String status;
    String sectionCode;
    String facultyName;
    bool isRegistered;
    String cardHolder;
};

class OrchestraNetworkClient {
private:
    enum WifiConnState {
        WIFI_CONN_DISCONNECTED, // In cooldown between reconnect sweeps (zero radio/CPU usage)
        WIFI_CONN_PROBING,      // Asynchronously connecting to current candidate AP
        WIFI_CONN_ACTIVE        // Uplink established and confirmed
    };

    WifiConnState connState = WIFI_CONN_DISCONNECTED;
    size_t currentApIndex = 0;
    unsigned long probeStartTime = 0;
    unsigned long lastReconnectSweepTime = 0;

    void configureChannels() {
        wifi_country_t country = {
            .cc = "PH",
            .schan = 1,
            .nchan = 14,
            .max_tx_power = 15,
            .policy = WIFI_COUNTRY_POLICY_MANUAL
        };
        esp_wifi_set_country(&country);
    }

public:
    void begin() {
        WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable BOD to prevent Wi-Fi peak current resets
        WiFi.persistent(false);                     // Do not wear flash NVS on Wi-Fi state shifts
        WiFi.mode(WIFI_STA);
        WiFi.setTxPower(WIFI_POWER_15dBm);
        WiFi.setAutoReconnect(false);               // Controlled non-blockingly by firmware state machine
        configureChannels();
        for (size_t i = 0; i < WIFI_NETWORK_COUNT; i++) {
            Serial.printf("[WIFI] Registered AP [%u]: %s\n", (unsigned)(i + 1), WIFI_NETWORKS[i].ssid);
        }
    }

    bool isConnected() {
        return (WiFi.status() == WL_CONNECTED);
    }

    /**
     * Fast boot-time connection attempt (max 2000ms).
     * If Wi-Fi is available, connects quickly.
     * If router is off / network down, exits immediately within 2s without freezing boot!
     */
    bool quickBootConnect(uint32_t maxWaitMs = 2000) {
        if (WIFI_NETWORK_COUNT == 0) return false;

        const char* ssid = WIFI_NETWORKS[0].ssid;
        const char* pass = WIFI_NETWORKS[0].password;
        Serial.printf("[WIFI BOOT] Probing primary AP '%s' (max %lums)...\n", ssid, (unsigned long)maxWaitMs);

        WiFi.disconnect(true);
        delay(60);
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, pass);

        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - start < maxWaitMs)) {
            delay(100);
            if (WiFi.status() == WL_CONNECT_FAILED) break;
        }

        if (WiFi.status() == WL_CONNECTED) {
            connState = WIFI_CONN_ACTIVE;
            Serial.printf("[WIFI BOOT] Connected to '%s'! IP: %s\n",
                          WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
            configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
            return true;
        }

        Serial.println(F("[WIFI BOOT] No immediate uplink. Entering OFFLINE STANDBY mode immediately."));
        WiFi.disconnect(true);
        connState = WIFI_CONN_DISCONNECTED;
        lastReconnectSweepTime = millis();
        return false;
    }

    /**
     * 100% NON-BLOCKING Wi-Fi Connection & Failover State Machine.
     * ZERO delay(), ZERO while() loops, ZERO synchronous scans!
     * Executes in microseconds on every loop() pass, keeping RFID scanning
     * and keypad polling running at maximum speed even during network outages.
     */
    void maintainConnection(unsigned long now) {
        wl_status_t status = WiFi.status();

        // 1. Currently Active Link
        if (status == WL_CONNECTED) {
            if (connState != WIFI_CONN_ACTIVE) {
                connState = WIFI_CONN_ACTIVE;
                Serial.printf(F("[WIFI] Background uplink established to '%s'! IP: %s\n"),
                              WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
                configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
            }
            return;
        }

        // 2. Link was dropped while previously connected
        if (connState == WIFI_CONN_ACTIVE) {
            Serial.println(F("[WIFI] Uplink dropped! Powering down radio for quiet 60s offline operation..."));
            connState = WIFI_CONN_DISCONNECTED;
            currentApIndex = 0;
            lastReconnectSweepTime = now;
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF); // Power down 2.4GHz radio to eliminate RF noise on 13.56MHz MFRC522 coil
            return;
        }

        // 3. In-flight Non-blocking Association Attempt (Max 2.0s probe)
        if (connState == WIFI_CONN_PROBING) {
            // Succeeded in background
            if (status == WL_CONNECTED) {
                connState = WIFI_CONN_ACTIVE;
                Serial.printf(F("[WIFI] Reconnected to '%s'! IP: %s\n"),
                              WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
                configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
                return;
            }

            // If connection failed or 2.0s elapsed:
            if (status == WL_CONNECT_FAILED || (now - probeStartTime >= 2000)) {
                // Enter quiet 60-second offline sleep; power down radio so RFID runs at peak sensitivity
                connState = WIFI_CONN_DISCONNECTED;
                currentApIndex = 0;
                lastReconnectSweepTime = now;
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
                Serial.println(F("[WIFI] Probe timed out. Radio powered off for 60s cooldown (MFRC522 at max sensitivity)."));
            }
            return;
        }

        // 4. Disconnected / Cooldown Period
        // Probes primary AP only once every 60 seconds (97% of time radio is completely dormant)
        if (connState == WIFI_CONN_DISCONNECTED) {
            if (now - lastReconnectSweepTime >= 60000) {
                lastReconnectSweepTime = now;
                const char* primarySsid = WIFI_NETWORKS[0].ssid;
                const char* primaryPass = WIFI_NETWORKS[0].password;
                Serial.printf("[WIFI] Waking radio for quick background probe: '%s'...\n", primarySsid);
                WiFi.mode(WIFI_STA);
                WiFi.disconnect(false);
                WiFi.begin(primarySsid, primaryPass);
                connState = WIFI_CONN_PROBING;
                probeStartTime = now;
            }
        }
    }

    bool syncTerminalStatus(bool &isLocked, String &sectionCode, bool &enrollmentActive, String &targetRole, String &targetName) {
        if (!isConnected()) return false;

        HTTPClient http;
        String url = String(BACKEND_BASE_URL) + "/hardware/status/" + String(TERMINAL_ID);
        http.begin(url);
        http.addHeader("x-api-key", HARDWARE_API_KEY);
        http.setTimeout(2500);

        int httpCode = http.GET();
        bool success = false;

        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, payload);
            if (!err) {
                isLocked = doc["isLocked"] | true;
                sectionCode = doc["session"]["sectionCode"] | DEFAULT_SECTION;
                enrollmentActive = doc["enrollment"]["active"] | false;
                targetRole = doc["enrollment"]["targetRole"] | "STUDENT";
                targetName = doc["enrollment"]["targetName"] | "";

                // Parse authorized faculty instructor list and update local offline cache
                if (doc["authorizedFaculty"].is<JsonArray>()) {
                    JsonArray facArr = doc["authorizedFaculty"].as<JsonArray>();
                    std::vector<FacultyCacheRecord> facList;
                    for (JsonObject item : facArr) {
                        String u = item["rfidUid"] | "";
                        String n = item["name"] | "";
                        String r = item["role"] | "FACULTY";
                        if (u.length() > 0) {
                            facList.push_back({u, n, r});
                        }
                    }
                    if (!facList.empty()) {
                        offlineStorage.updateFacultyList(facList);
                    }
                }

                success = true;
            }
        }
        http.end();
        return success;
    }

    bool postRfidTap(const String &rfidUid, TerminalApiResponse &outResponse) {
        if (!isConnected()) {
            outResponse.httpCode = 0;
            outResponse.code = "OFFLINE";
            outResponse.message = "No Wi-Fi Link";
            return false;
        }

        HTTPClient http;
        String url = String(BACKEND_BASE_URL) + "/hardware/tap";
        http.begin(url);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("x-api-key", HARDWARE_API_KEY);
        http.setTimeout(2500);

        JsonDocument reqDoc;
        reqDoc["terminalId"] = TERMINAL_ID;
        reqDoc["rfidUid"] = rfidUid;

        String reqJson;
        serializeJson(reqDoc, reqJson);

        int httpCode = http.POST(reqJson);
        outResponse.httpCode = httpCode;

        if (httpCode > 0) {
            String resJson = http.getString();
            JsonDocument resDoc;
            DeserializationError err = deserializeJson(resDoc, resJson);

            if (!err) {
                outResponse.code = resDoc["code"] | "UNKNOWN";
                outResponse.message = resDoc["message"] | "";
                outResponse.studentName = resDoc["data"]["studentName"] | "";
                outResponse.studentId = resDoc["data"]["studentId"] | "";
                outResponse.status = resDoc["data"]["status"] | "";
                outResponse.sectionCode = resDoc["data"]["sectionCode"] | DEFAULT_SECTION;
                outResponse.facultyName = resDoc["data"]["facultyName"] | "";
                outResponse.isRegistered = resDoc["data"]["isRegistered"] | false;
                outResponse.cardHolder = resDoc["data"]["cardHolderName"] | "";
            }
        } else {
            outResponse.code = "ERR_HTTP_FAIL";
            outResponse.message = http.errorToString(httpCode);
        }

        http.end();
        return (httpCode == 200 || httpCode == 201);
    }

    bool postKeypadOtp(const String &otpPin, TerminalApiResponse &outResponse) {
        if (!isConnected()) {
            outResponse.httpCode = 0;
            outResponse.code = "OFFLINE";
            outResponse.message = "No Wi-Fi Link";
            return false;
        }

        HTTPClient http;
        String url = String(BACKEND_BASE_URL) + "/hardware/otp";
        http.begin(url);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("x-api-key", HARDWARE_API_KEY);
        http.setTimeout(2500);

        JsonDocument reqDoc;
        reqDoc["terminalId"] = TERMINAL_ID;
        reqDoc["otp"] = otpPin;

        String reqJson;
        serializeJson(reqDoc, reqJson);

        int httpCode = http.POST(reqJson);
        outResponse.httpCode = httpCode;

        if (httpCode > 0) {
            String resJson = http.getString();
            JsonDocument resDoc;
            DeserializationError err = deserializeJson(resDoc, resJson);

            if (!err) {
                outResponse.code = resDoc["code"] | "UNKNOWN";
                outResponse.message = resDoc["message"] | "";
                outResponse.studentName = resDoc["data"]["studentName"] | "";
                outResponse.studentId = resDoc["data"]["studentId"] | "";
                outResponse.status = resDoc["data"]["status"] | "";
                outResponse.sectionCode = resDoc["data"]["sectionCode"] | DEFAULT_SECTION;
                outResponse.facultyName = resDoc["data"]["facultyName"] | "";
            }
        } else {
            outResponse.code = "ERR_HTTP_FAIL";
            outResponse.message = http.errorToString(httpCode);
        }

        http.end();
        return (httpCode == 200 || httpCode == 201);
    }

    bool postOfflineSync(const String &termId, const String &sessionAction, const String &facultyUid, const std::vector<OfflineAttendanceRecord> &records, int &outSyncedCount) {
        if (!isConnected()) return false;

        HTTPClient http;
        String url = String(BACKEND_BASE_URL) + "/hardware/offline-sync";
        http.begin(url);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("x-api-key", HARDWARE_API_KEY);
        http.setTimeout(12000);

        JsonDocument reqDoc;
        reqDoc["terminalId"] = termId;
        reqDoc["sessionAction"] = sessionAction;
        if (facultyUid.length() > 0) {
            reqDoc["facultyUid"] = facultyUid;
        }

        JsonArray recArr = reqDoc["records"].to<JsonArray>();
        for (const auto &rec : records) {
            JsonObject item = recArr.add<JsonObject>();
            item["rfidUid"] = rec.rfidUid;
            item["timestamp"] = rec.timestamp;
            item["authMethod"] = rec.authMethod;
        }

        String reqJson;
        serializeJson(reqDoc, reqJson);

        Serial.printf(F("[OFFLINE SYNC HTTP] Dispatching %u queued records to backend...\n"), records.size());
        int httpCode = http.POST(reqJson);

        bool success = false;
        outSyncedCount = 0;

        if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
            String resJson = http.getString();
            JsonDocument resDoc;
            DeserializationError err = deserializeJson(resDoc, resJson);
            if (!err && (resDoc["success"] | false)) {
                outSyncedCount = resDoc["syncedCount"] | (int)records.size();
                success = true;
                Serial.printf(F("[OFFLINE SYNC HTTP] Success! Synced %d records with server.\n"), outSyncedCount);
            }
        } else {
            Serial.printf(F("[OFFLINE SYNC HTTP] Failed with code %d\n"), httpCode);
        }

        http.end();
        return success;
    }
};

extern OrchestraNetworkClient netClient;

#endif // NETWORK_CLIENT_H
