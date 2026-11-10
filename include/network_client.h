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
    unsigned long lastWifiCheckTime = 0;

    struct Candidate {
        int indexInConfig;
        int rssi;
        String ssid;
    };

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
        WiFi.mode(WIFI_STA);
        WiFi.setTxPower(WIFI_POWER_15dBm);
        configureChannels();
        for (size_t i = 0; i < WIFI_NETWORK_COUNT; i++) {
            Serial.printf("[WIFI] Registered AP [%u]: %s\n", (unsigned)(i + 1), WIFI_NETWORKS[i].ssid);
        }
    }

    bool isConnected() {
        return (WiFi.status() == WL_CONNECTED);
    }

    bool connectBestNetwork(uint32_t timeoutPerApMs = 8000) {
        WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
        Serial.println(F("[WIFI] Scanning 2.4GHz Wi-Fi channels (1-14)..."));
        WiFi.disconnect(true);
        delay(100);
        WiFi.mode(WIFI_STA);
        WiFi.setTxPower(WIFI_POWER_15dBm);
        configureChannels();

        int numNetworks = WiFi.scanNetworks(false, false);
        Serial.printf("[WIFI] Scan complete. Found %d networks in vicinity:\n", numNetworks);

        std::vector<Candidate> candidates;

        if (numNetworks > 0) {
            for (int i = 0; i < numNetworks; i++) {
                String foundSsid = WiFi.SSID(i);
                int32_t rssi = WiFi.RSSI(i);
                wifi_auth_mode_t auth = WiFi.encryptionType(i);
                Serial.printf("  [%02d] %-22s | RSSI: %4d dBm | Auth: %d\n", i + 1, foundSsid.c_str(), (int)rssi, (int)auth);

                for (size_t c = 0; c < WIFI_NETWORK_COUNT; c++) {
                    if (foundSsid.equals(WIFI_NETWORKS[c].ssid)) {
                        candidates.push_back({(int)c, (int)rssi, foundSsid});
                        break;
                    }
                }
            }
            WiFi.scanDelete();
        }

        // Sort matched networks by strongest signal
        if (!candidates.empty()) {
            std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
                return a.rssi > b.rssi;
            });

            Serial.printf("[WIFI] Found %u matching AP(s) from configured roster:\n", candidates.size());
            for (const auto &cand : candidates) {
                Serial.printf("  -> Candidate '%s' (RSSI: %d dBm)\n", cand.ssid.c_str(), cand.rssi);
            }

            for (const auto &cand : candidates) {
                const char* ssid = WIFI_NETWORKS[cand.indexInConfig].ssid;
                const char* pass = WIFI_NETWORKS[cand.indexInConfig].password;

                Serial.printf("[WIFI] Attempting connection to '%s'...\n", ssid);
                WiFi.disconnect(true);
                delay(100);
                WiFi.begin(ssid, pass);

                unsigned long start = millis();
                while (WiFi.status() != WL_CONNECTED && (millis() - start < timeoutPerApMs)) {
                    delay(250);
                    Serial.print(F("."));
                    if (WiFi.status() == WL_CONNECT_FAILED) {
                        Serial.println(F("\n[WIFI] Handshake failed (bad credentials or auth rejection)."));
                        break;
                    }
                }
                Serial.println();

                if (WiFi.status() == WL_CONNECTED) {
                    Serial.printf(F("[WIFI] Uplink established to '%s'! IP: %s | Channel: %d\n"),
                                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.channel());
                    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
                    return true;
                } else {
                    Serial.printf(F("[WIFI] Failed to connect to '%s' (status code: %d).\n"), ssid, WiFi.status());
                }
            }
        }

        // Direct fallback: try each configured network in order (handles hidden SSIDs / unresponsive scans)
        Serial.println(F("[WIFI] Trying direct association on configured APs..."));
        for (size_t i = 0; i < WIFI_NETWORK_COUNT; i++) {
            const char* ssid = WIFI_NETWORKS[i].ssid;
            const char* pass = WIFI_NETWORKS[i].password;

            Serial.printf("[WIFI] Direct try: '%s'...\n", ssid);
            WiFi.disconnect(true);
            delay(100);
            WiFi.begin(ssid, pass);

            unsigned long start = millis();
            while (WiFi.status() != WL_CONNECTED && (millis() - start < timeoutPerApMs)) {
                delay(250);
                Serial.print(F("."));
                if (WiFi.status() == WL_CONNECT_FAILED) {
                    break;
                }
            }
            Serial.println();

            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf(F("[WIFI] Connected via direct association: '%s' | IP: %s\n"),
                              WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
                configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
                return true;
            }
        }

        Serial.println(F("[WIFI] Unable to establish connection to any configured AP."));
        return false;
    }

    void maintainConnection(unsigned long now) {
        if (!isConnected()) {
            if (now - lastWifiCheckTime >= WIFI_RECONNECT_INTERVAL_MS) {
                lastWifiCheckTime = now;
                Serial.println(F("[WIFI] Disconnected from AP. Re-initiating failover connection..."));
                connectBestNetwork(6000);
            }
        }
    }

    bool syncTerminalStatus(bool &isLocked, String &sectionCode, bool &enrollmentActive, String &targetRole, String &targetName) {
        if (!isConnected()) return false;

        HTTPClient http;
        String url = String(BACKEND_BASE_URL) + "/hardware/status/" + String(TERMINAL_ID);
        http.begin(url);
        http.addHeader("x-api-key", HARDWARE_API_KEY);
        http.setTimeout(8000);

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
        http.setTimeout(8000);

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
        http.setTimeout(8000);

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
};

extern OrchestraNetworkClient netClient;

#endif // NETWORK_CLIENT_H
