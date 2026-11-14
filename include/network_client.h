#ifndef NETWORK_CLIENT_H
#define NETWORK_CLIENT_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>
#include <algorithm>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "config.h"
#include "offline_storage.h"
#include "display.h"

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
    String facultyUid;
    int sectionCount;
    char sectionCodes[4][24];
};

class OrchestraNetworkClient {
private:
    SemaphoreHandle_t netMutex = NULL;
    String customSsid;
    String customPass;
    String customBackendUrl;
    String lastResolvedBackendUrl;
    unsigned long lastMdnsResolveTime = 0;
    bool mdnsStarted = false;

    void ensureNetMutex() {
        if (netMutex == NULL) {
            netMutex = xSemaphoreCreateRecursiveMutex();
        }
    }

    void lockNet() {
        ensureNetMutex();
        if (netMutex) {
            xSemaphoreTakeRecursive(netMutex, portMAX_DELAY);
        }
    }

    void unlockNet() {
        if (netMutex) {
            xSemaphoreGiveRecursive(netMutex);
        }
    }

    class NetLockGuard {
        OrchestraNetworkClient &client;
    public:
        explicit NetLockGuard(OrchestraNetworkClient &c) : client(c) { client.lockNet(); }
        ~NetLockGuard() { client.unlockNet(); }
    };

    enum WifiConnState {
        WIFI_CONN_DISCONNECTED, // In cooldown between reconnect sweeps (radio OFF)
        WIFI_CONN_SCANNING,     // Asynchronously scanning for known APs in background (0ms CPU)
        WIFI_CONN_PROBING,      // Asynchronously connecting to candidate AP
        WIFI_CONN_ACTIVE        // Uplink established and confirmed
    };

    WifiConnState connState = WIFI_CONN_DISCONNECTED;
    unsigned long probeStartTime = 0;
    unsigned long scanStartTime = 0;
    unsigned long lastReconnectSweepTime = 0;
    const unsigned long WIFI_OFFLINE_COOLDOWN_MS = 60000; // 60s quiet window (radio OFF) for pure 13.56MHz RFID sensitivity

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
    OrchestraNetworkClient() {
        ensureNetMutex();
    }

    void begin() {
        NetLockGuard guard(*this);
        WiFi.persistent(false);                     // Do not wear flash NVS on Wi-Fi state shifts
        WiFi.mode(WIFI_STA);
        WiFi.setTxPower(WIFI_POWER_15dBm);
        WiFi.setAutoReconnect(false);               // Controlled non-blockingly by firmware state machine
        configureChannels();

        // Load custom provisioned credentials from NVS if available
        Preferences pref;
        pref.begin("net_cfg", true);
        customSsid = pref.getString("ssid", "");
        customPass = pref.getString("pass", "");
        customBackendUrl = pref.getString("backend_url", "");
        pref.end();

        if (customSsid.length() > 0) {
            Serial.printf("[WIFI NVS] Loaded provisioned AP: %s (Backend: %s)\n",
                          customSsid.c_str(),
                          customBackendUrl.length() > 0 ? customBackendUrl.c_str() : BACKEND_BASE_URL);
        }

        for (size_t i = 0; i < WIFI_NETWORK_COUNT; i++) {
            Serial.printf("[WIFI] Registered AP [%u]: %s\n", (unsigned)(i + 1), WIFI_NETWORKS[i].ssid);
        }
    }

    String getBackendUrl() const {
        String base = (customBackendUrl.length() > 0) ? customBackendUrl : String(BACKEND_BASE_URL);
        base.trim();
        if (!base.startsWith("http://") && !base.startsWith("https://")) {
            base = "http://" + base;
        }
        while (base.endsWith("/")) {
            base = base.substring(0, base.length() - 1);
        }
        if (!base.endsWith("/api/v1")) {
            base += "/api/v1";
        }
        return base;
    }

    void setupMdns() {
        if (!mdnsStarted) {
            String host = "orchestra-" + String(TERMINAL_ID);
            host.toLowerCase();
            if (MDNS.begin(host.c_str())) {
                MDNS.addService("http", "tcp", 80);
                mdnsStarted = true;
                Serial.printf(F("[mDNS] Responder started: http://%s.local\n"), host.c_str());
            }
        }
    }

    String resolveBackendUrl(const String &baseUrl) {
        if (baseUrl.indexOf(".local") == -1) {
            return baseUrl;
        }

        unsigned long now = millis();
        if (lastResolvedBackendUrl.length() > 0 && (now - lastMdnsResolveTime < 60000)) {
            return lastResolvedBackendUrl;
        }

        int protoIdx = baseUrl.indexOf("://");
        int hostStart = (protoIdx != -1) ? protoIdx + 3 : 0;
        int hostEnd = baseUrl.indexOf(':', hostStart);
        if (hostEnd == -1) {
            hostEnd = baseUrl.indexOf('/', hostStart);
        }
        if (hostEnd == -1) {
            hostEnd = baseUrl.length();
        }

        String host = baseUrl.substring(hostStart, hostEnd);
        String mdnsName = host;
        if (mdnsName.endsWith(".local")) {
            mdnsName = mdnsName.substring(0, mdnsName.length() - 6);
        }

        Serial.printf(F("[mDNS] Resolving '%s' via mDNS query...\n"), mdnsName.c_str());
        if (!mdnsStarted) {
            setupMdns();
        }

        // Apply resolution cooldown timer on both success and failure to prevent repeated 1500ms stalls
        lastMdnsResolveTime = now;
        IPAddress resolvedIp = MDNS.queryHost(mdnsName.c_str(), 1500);

        if (resolvedIp != INADDR_NONE && resolvedIp.toString() != "0.0.0.0") {
            Serial.printf(F("[mDNS] Resolved %s -> %s\n"), host.c_str(), resolvedIp.toString().c_str());
            String newUrl = baseUrl;
            newUrl.replace(host, resolvedIp.toString());
            lastResolvedBackendUrl = newUrl;
            return newUrl;
        }

        Serial.printf(F("[mDNS] Query for '%s' returned fallback.\n"), host.c_str());
        return baseUrl;
    }

    bool isConnected() {
        return (WiFi.status() == WL_CONNECTED);
    }

    /**
     * Ingress Active Hold & Instant Priority Abort:
     * Resets the 60-second Wi-Fi reconnect timer to ensure zero RF activity while students are tapping.
     * If an async scan or probe happens to be in flight when a card touches the antenna,
     * immediately aborts the scan and cuts radio power in microseconds!
     */
    void resetCooldown(unsigned long now) {
        NetLockGuard guard(*this);
        lastReconnectSweepTime = now;
        if (connState == WIFI_CONN_SCANNING) {
            WiFi.scanDelete();
            WiFi.mode(WIFI_OFF);
            connState = WIFI_CONN_DISCONNECTED;
            Serial.println(F("[WIFI INTERLOCK] Card tap detected during background scan! Aborted scan & powered OFF radio."));
        } else if (connState == WIFI_CONN_PROBING) {
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            connState = WIFI_CONN_DISCONNECTED;
            Serial.println(F("[WIFI INTERLOCK] Card tap detected during association probe! Aborted probe & powered OFF radio."));
        }
    }

    /**
     * Manual Wi-Fi Reconnect Trigger (Double-tap '#' key):
     * Instantly initiates a background active Wi-Fi scan without waiting for the 60s timer.
     */
    void forceReconnectNow() {
        NetLockGuard guard(*this);
        if (connState == WIFI_CONN_ACTIVE) {
            Serial.println(F("[WIFI MANUAL] Wi-Fi link already active."));
            return;
        }
        Serial.println(F("[WIFI MANUAL] Double '#' pressed! Forcing immediate background Wi-Fi scan..."));
        WiFi.scanDelete();
        WiFi.mode(WIFI_STA);
        WiFi.scanNetworks(true, false, false, 120); // ASYNC = true! Zero blocking!
        connState = WIFI_CONN_SCANNING;
        scanStartTime = millis();
        lastReconnectSweepTime = millis();
    }

    /**
     * Fast boot-time connection attempt (max 4500ms).
     * 1. Performs a fast active scan (120ms/chan) to discover which configured AP is actually nearby.
     * 2. Automatically locks onto the AP with the STRONGEST signal (RSSI).
     * 3. Directly associates using the scanned channel & BSSID to eliminate channel hunt delays.
     * 4. If no configured AP is nearby, exits immediately into OFFLINE STANDBY in ~1.5s without freezing boot!
     */
    bool quickBootConnect(uint32_t maxConnectWaitMs = 4500) {
        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(true);
        delay(60);

        Serial.println(F("[WIFI BOOT] Performing fast active scan (120ms/channel) for known APs..."));
        displayMgr.showConnectingWiFi("Scanning Wi-Fi...");

        // Fast active scan: 120ms per channel * 13 channels ~= 1.5 seconds total
        int16_t numNetworks = WiFi.scanNetworks(false, false, false, 120);

        int bestMatchIndex = -1;
        int bestScannedIndex = -1;
        int32_t bestRssi = -999;
        bool customMatched = false;

        if (numNetworks > 0) {
            Serial.printf("[WIFI BOOT] Visible 2.4GHz networks found: %d\n", numNetworks);
            for (int i = 0; i < numNetworks; i++) {
                String s = WiFi.SSID(i);
                int32_t r = WiFi.RSSI(i);
                Serial.printf("  -> [%d] SSID: '%s', RSSI: %ld dBm, Ch: %d\n", i, s.c_str(), (long)r, (int)WiFi.channel(i));

                // Check provisioned NVS AP first
                if (customSsid.length() > 0 && s.equals(customSsid)) {
                    if (r > bestRssi) {
                        bestRssi = r;
                        bestScannedIndex = i;
                        customMatched = true;
                    }
                }

                // Check built-in configured APs
                for (size_t j = 0; j < WIFI_NETWORK_COUNT; j++) {
                    if (s.equals(WIFI_NETWORKS[j].ssid)) {
                        if (r > bestRssi) {
                            bestRssi = r;
                            bestMatchIndex = (int)j;
                            bestScannedIndex = i;
                            customMatched = false;
                        }
                    }
                }
            }
        } else {
            Serial.println(F("[WIFI BOOT] No 2.4GHz networks detected in quick scan."));
        }

        const char* targetSsid = nullptr;
        const char* targetPass = nullptr;
        uint8_t targetChannel = 0;
        const uint8_t* targetBssid = nullptr;

        if (customMatched) {
            targetSsid = customSsid.c_str();
            targetPass = customPass.c_str();
            targetChannel = (uint8_t)WiFi.channel(bestScannedIndex);
            targetBssid = WiFi.BSSID(bestScannedIndex);
            Serial.printf("[WIFI BOOT] Best matching NVS AP: '%s' (RSSI: %ld dBm, Ch: %d)\n",
                          targetSsid, (long)bestRssi, (int)targetChannel);
        } else if (bestMatchIndex >= 0) {
            targetSsid = WIFI_NETWORKS[bestMatchIndex].ssid;
            targetPass = WIFI_NETWORKS[bestMatchIndex].password;
            targetChannel = (uint8_t)WiFi.channel(bestScannedIndex);
            targetBssid = WiFi.BSSID(bestScannedIndex);
            Serial.printf("[WIFI BOOT] Best matching AP: '%s' (RSSI: %ld dBm, Ch: %d)\n",
                          targetSsid, (long)bestRssi, (int)targetChannel);
        } else if (customSsid.length() > 0) {
            Serial.printf("[WIFI BOOT] Probing custom NVS AP fallback: %s\n", customSsid.c_str());
            targetSsid = customSsid.c_str();
            targetPass = customPass.c_str();
        } else if (WIFI_NETWORK_COUNT > 0) {
            Serial.println(F("[WIFI BOOT] No known AP matched in scan; probing primary AP fallback..."));
            targetSsid = WIFI_NETWORKS[0].ssid;
            targetPass = WIFI_NETWORKS[0].password;
        } else {
            return false;
        }

        displayMgr.showConnectingWiFi(targetSsid);
        Serial.printf("[WIFI BOOT] Connecting to '%s' (timeout %lums)...\n", targetSsid, (unsigned long)maxConnectWaitMs);

        if (targetChannel > 0 && targetBssid != nullptr) {
            WiFi.begin(targetSsid, targetPass, targetChannel, targetBssid);
        } else {
            WiFi.begin(targetSsid, targetPass);
        }

        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - start < maxConnectWaitMs)) {
            delay(100);
            if (WiFi.status() == WL_CONNECT_FAILED) break;
        }

        WiFi.scanDelete();

        if (WiFi.status() == WL_CONNECTED) {
            connState = WIFI_CONN_ACTIVE;
            Serial.printf("[WIFI BOOT] Uplink established to '%s'! IP: %s\n",
                          WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
            configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
            setupMdns();
            return true;
        }

        Serial.printf("[WIFI BOOT] Connection to '%s' timed out. Entering OFFLINE STANDBY mode.\n", targetSsid);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
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
        NetLockGuard guard(*this);
        wl_status_t status = WiFi.status();

        // 1. Currently Active Link
        if (status == WL_CONNECTED) {
            if (connState != WIFI_CONN_ACTIVE) {
                connState = WIFI_CONN_ACTIVE;
                Serial.printf(F("[WIFI] Background uplink established to '%s'! IP: %s\n"),
                              WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
                configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
                setupMdns();
            }
            return;
        }

        // 2. Link was dropped while previously connected
        if (connState == WIFI_CONN_ACTIVE) {
            Serial.println(F("[WIFI] Uplink dropped! Powering down radio for quiet offline operation..."));
            connState = WIFI_CONN_DISCONNECTED;
            lastReconnectSweepTime = now;
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF); // Power down 2.4GHz radio to eliminate RF noise on 13.56MHz MFRC522 coil
            return;
        }

        // 3. In-flight Non-blocking Association Attempt (Max 4.0s probe)
        if (connState == WIFI_CONN_PROBING) {
            if (status == WL_CONNECTED) {
                connState = WIFI_CONN_ACTIVE;
                Serial.printf(F("[WIFI] Reconnected to '%s'! IP: %s\n"),
                              WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
                configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
                setupMdns();
                return;
            }

            if (status == WL_CONNECT_FAILED || (now - probeStartTime >= 4000)) {
                connState = WIFI_CONN_DISCONNECTED;
                lastReconnectSweepTime = now;
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
                Serial.println(F("[WIFI] Background association timed out. Radio powered OFF for 20s cooldown."));
            }
            return;
        }

        // 4. In-flight Asynchronous Background Scan
        if (connState == WIFI_CONN_SCANNING) {
            int16_t scanStatus = WiFi.scanComplete();

            if (scanStatus == WIFI_SCAN_RUNNING) {
                // Safety guard: if scan hangs for > 3.5s, cancel and power off
                if (now - scanStartTime >= 3500) {
                    WiFi.scanDelete();
                    WiFi.mode(WIFI_OFF);
                    connState = WIFI_CONN_DISCONNECTED;
                    lastReconnectSweepTime = now;
                }
                return; // Non-blocking! Loop continues immediately
            }

            if (scanStatus > 0) {
                int bestMatch = -1;
                int bestScanIdx = -1;
                int32_t bestRssi = -999;
                bool customMatched = false;

                for (int i = 0; i < scanStatus; i++) {
                    String s = WiFi.SSID(i);
                    int32_t r = WiFi.RSSI(i);

                    // Check custom provisioned NVS AP first
                    if (customSsid.length() > 0 && s.equals(customSsid)) {
                        if (r > bestRssi) {
                            bestRssi = r;
                            bestScanIdx = i;
                            customMatched = true;
                        }
                    }

                    // Check built-in configured APs
                    for (size_t j = 0; j < WIFI_NETWORK_COUNT; j++) {
                        if (s.equals(WIFI_NETWORKS[j].ssid)) {
                            if (r > bestRssi) {
                                bestRssi = r;
                                bestMatch = (int)j;
                                bestScanIdx = i;
                                customMatched = false;
                            }
                        }
                    }
                }

                if (customMatched || bestMatch >= 0) {
                    const char* targetSsid = customMatched ? customSsid.c_str() : WIFI_NETWORKS[bestMatch].ssid;
                    const char* targetPass = customMatched ? customPass.c_str() : WIFI_NETWORKS[bestMatch].password;
                    uint8_t chan = (uint8_t)WiFi.channel(bestScanIdx);
                    const uint8_t* bssid = WiFi.BSSID(bestScanIdx);

                    Serial.printf(F("[WIFI] Background scan found '%s' (%ld dBm). Probing uplink...\n"),
                                  targetSsid, (long)bestRssi);
                    WiFi.begin(targetSsid, targetPass, chan, bssid);
                    WiFi.scanDelete();
                    connState = WIFI_CONN_PROBING;
                    probeStartTime = now;
                    return;
                }
            }

            // Scan completed with 0 matches or error: power down radio and wait 20s
            WiFi.scanDelete();
            WiFi.mode(WIFI_OFF);
            connState = WIFI_CONN_DISCONNECTED;
            lastReconnectSweepTime = now;
            return;
        }

        // 5. Disconnected / Cooldown Period (60 seconds)
        // Radio is 100% OFF during cooldown for pure 13.56MHz RFID sensitivity
        if (connState == WIFI_CONN_DISCONNECTED) {
            if (now - lastReconnectSweepTime >= WIFI_OFFLINE_COOLDOWN_MS) {
                lastReconnectSweepTime = now;
                WiFi.mode(WIFI_STA);
                WiFi.scanNetworks(true, false, false, 120); // ASYNC = true! Zero blocking!
                connState = WIFI_CONN_SCANNING;
                scanStartTime = now;
            }
        }
    }

    bool syncTerminalStatus(bool &isLocked, String &sectionCode, bool &enrollmentActive, String &targetRole, String &targetName, bool &outQuietMode) {
        if (!isConnected()) return false;

        HTTPClient http;
        String url = resolveBackendUrl(getBackendUrl()) + "/hardware/status/" + String(TERMINAL_ID);
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
                outQuietMode = doc["isQuietMode"] | doc["session"]["isQuietMode"] | false;

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

    bool syncTerminalStatus(bool &isLocked, String &sectionCode, bool &enrollmentActive, String &targetRole, String &targetName) {
        bool dummyQuiet = false;
        return syncTerminalStatus(isLocked, sectionCode, enrollmentActive, targetRole, targetName, dummyQuiet);
    }

    bool postHeartbeat(int rssi, uint32_t freeHeap, uint32_t uptimeSec, const char* readerStatus, const char* version, bool &outQuietMode) {
        if (!isConnected()) return false;

        HTTPClient http;
        String url = resolveBackendUrl(getBackendUrl()) + "/hardware/heartbeat";
        http.begin(url);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("x-api-key", HARDWARE_API_KEY);
        http.setTimeout(2500);

        JsonDocument reqDoc;
        reqDoc["terminalId"] = TERMINAL_ID;
        reqDoc["rssi"] = rssi;
        reqDoc["freeHeap"] = freeHeap;
        reqDoc["uptimeSec"] = uptimeSec;
        reqDoc["readerStatus"] = (readerStatus && strlen(readerStatus) > 0) ? readerStatus : "OK";
        reqDoc["version"] = (version && strlen(version) > 0) ? version : "v1.0.4";

        String reqJson;
        serializeJson(reqDoc, reqJson);

        int httpCode = http.POST(reqJson);
        bool success = false;

        if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
            String resJson = http.getString();
            JsonDocument resDoc;
            DeserializationError err = deserializeJson(resDoc, resJson);
            if (!err) {
                if (resDoc["data"]["isQuietMode"].is<bool>()) {
                    outQuietMode = resDoc["data"]["isQuietMode"].as<bool>();
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
        String url = resolveBackendUrl(getBackendUrl()) + "/hardware/tap";
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
                outResponse.facultyUid = resDoc["data"]["facultyUid"] | "";

                outResponse.sectionCount = 0;
                if (resDoc["data"]["sections"].is<JsonArray>()) {
                    JsonArray secArr = resDoc["data"]["sections"].as<JsonArray>();
                    for (JsonObject s : secArr) {
                        if (outResponse.sectionCount < 4) {
                            const char* sCode = s["sectionCode"] | "";
                            strncpy(outResponse.sectionCodes[outResponse.sectionCount], sCode, sizeof(outResponse.sectionCodes[0]) - 1);
                            outResponse.sectionCodes[outResponse.sectionCount][sizeof(outResponse.sectionCodes[0]) - 1] = '\0';
                            outResponse.sectionCount++;
                        }
                    }
                }
            }
        } else {
            outResponse.code = "ERR_HTTP_FAIL";
            outResponse.message = http.errorToString(httpCode);
        }

        http.end();
        return (httpCode > 0);
    }

    bool postSelectSection(int index, const String &facultyUid, TerminalApiResponse &outResponse) {
        if (!isConnected()) {
            outResponse.httpCode = 0;
            outResponse.code = "OFFLINE";
            outResponse.message = "No Wi-Fi Link";
            return false;
        }

        HTTPClient http;
        String url = resolveBackendUrl(getBackendUrl()) + "/hardware/select-section";
        http.begin(url);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("x-api-key", HARDWARE_API_KEY);
        http.setTimeout(2500);

        JsonDocument reqDoc;
        reqDoc["terminalId"] = TERMINAL_ID;
        reqDoc["index"] = index;
        if (facultyUid.length() > 0) {
            reqDoc["facultyUid"] = facultyUid;
        }

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
                outResponse.sectionCode = resDoc["data"]["sectionCode"] | DEFAULT_SECTION;
                outResponse.facultyName = resDoc["data"]["facultyName"] | "";
            }
        } else {
            outResponse.code = "ERR_HTTP_FAIL";
            outResponse.message = http.errorToString(httpCode);
        }

        http.end();
        return (httpCode > 0);
    }

    bool postKeypadOtp(const String &otpPin, TerminalApiResponse &outResponse) {
        if (!isConnected()) {
            outResponse.httpCode = 0;
            outResponse.code = "OFFLINE";
            outResponse.message = "No Wi-Fi Link";
            return false;
        }

        HTTPClient http;
        String url = resolveBackendUrl(getBackendUrl()) + "/hardware/otp";
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
        return (httpCode > 0);
    }

    bool postOfflineSync(const String &termId, const String &sessionAction, const String &facultyUid, const std::vector<OfflineAttendanceRecord> &records, int &outSyncedCount) {
        if (!isConnected()) return false;

        HTTPClient http;
        String url = resolveBackendUrl(getBackendUrl()) + "/hardware/offline-sync";
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
