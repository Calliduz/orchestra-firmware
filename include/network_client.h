#ifndef NETWORK_CLIENT_H
#define NETWORK_CLIENT_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "config.h"

struct TerminalApiResponse {
    int httpCode;
    String code;
    String message;
    String studentName;
    String studentId;
    String status;
    String sectionCode;
};

class NetworkClient {
private:
    unsigned long lastWifiCheckTime = 0;

public:
    void begin() {
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        Serial.printf("[WIFI] Connecting to SSID: %s\n", WIFI_SSID);
    }

    bool isConnected() {
        return (WiFi.status() == WL_CONNECTED);
    }

    void maintainConnection(unsigned long now) {
        if (!isConnected()) {
            if (now - lastWifiCheckTime >= WIFI_RECONNECT_INTERVAL_MS) {
                lastWifiCheckTime = now;
                Serial.println(F("[WIFI] Disconnected. Re-triggering connection..."));
                WiFi.disconnect();
                WiFi.reconnect();
            }
        }
    }

    bool syncTerminalStatus(bool &isLocked, String &sectionCode) {
        if (!isConnected()) return false;

        HTTPClient http;
        String url = String(BACKEND_BASE_URL) + "/hardware/status/" + String(TERMINAL_ID);
        http.begin(url);
        http.addHeader("x-api-key", HARDWARE_API_KEY);
        http.setTimeout(4000);

        int httpCode = http.GET();
        bool success = false;

        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, payload);
            if (!err) {
                isLocked = doc["isLocked"] | true;
                sectionCode = doc["session"]["sectionCode"] | DEFAULT_SECTION;
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
        http.setTimeout(4000);

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
        http.setTimeout(4000);

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
            }
        } else {
            outResponse.code = "ERR_HTTP_FAIL";
            outResponse.message = http.errorToString(httpCode);
        }

        http.end();
        return (httpCode == 200 || httpCode == 201);
    }
};

extern NetworkClient netClient;

#endif // NETWORK_CLIENT_H
