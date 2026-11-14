#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "config.h"
#include "display.h"

class CaptivePortalManager {
private:
    DNSServer dnsServer;
    WebServer server;
    bool active = false;
    bool restartRequested = false;
    unsigned long restartTimer = 0;
    String apName;
    String savedSsid;
    String savedBackend;

public:
    CaptivePortalManager() : server(80) {}

    bool isActive() const {
        return active;
    }

    String getApSsid() const {
        return apName;
    }

    void start(const char* termId = TERMINAL_ID, const char* defaultBackend = BACKEND_BASE_URL) {
        if (active) return;

        apName = "ORCHESTRA-SETUP-" + String(termId);

        Serial.println();
        Serial.println(F("======================================================"));
        Serial.println(F("📡 [CAPTIVE PORTAL] Starting Wi-Fi Provisioning AP..."));
        Serial.printf(F("🏷️ AP SSID:  %s\n"), apName.c_str());
        Serial.println(F("🌐 Web URL:  http://192.168.4.1/"));
        Serial.println(F("======================================================"));

        WiFi.persistent(false);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_AP);

        IPAddress apIP(192, 168, 4, 1);
        IPAddress netMsk(255, 255, 255, 0);
        WiFi.softAPConfig(apIP, apIP, netMsk);
        WiFi.softAP(apName.c_str());

        // DNS Server listening on port 53 redirecting all traffic to 192.168.4.1
        dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
        dnsServer.start(53, "*", apIP);

        // Load existing saved configuration or fallback
        Preferences pref;
        pref.begin("net_cfg", true);
        savedSsid = pref.getString("ssid", WIFI_SSID);
        savedBackend = pref.getString("backend_url", defaultBackend);
        pref.end();

        // 1. Root configuration portal
        server.on("/", HTTP_GET, [this, termId]() {
            String html = F("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                            "<title>ORCHESTRA Wi-Fi Setup</title><style>"
                            "body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,sans-serif;background:#0f172a;color:#f8fafc;padding:16px;margin:0;}"
                            ".box{background:#1e293b;border:1px solid #334155;border-radius:14px;padding:22px;max-width:380px;margin:20px auto;box-shadow:0 12px 30px rgba(0,0,0,0.6);}"
                            "h2{color:#38bdf8;font-size:18px;margin-top:0;display:flex;align-items:center;gap:8px;}"
                            "label{display:block;font-size:12px;font-weight:600;color:#94a3b8;margin-top:14px;margin-bottom:6px;}"
                            "input{width:100%;box-sizing:border-box;padding:11px 12px;border-radius:8px;border:1px solid #475569;background:#0f172a;color:#fff;font-size:14px;}"
                            "input:focus{outline:none;border-color:#38bdf8;}"
                            "button{width:100%;margin-top:20px;padding:12px;border:none;border-radius:8px;background:#0284c7;color:#fff;font-weight:bold;font-size:14px;cursor:pointer;}"
                            "button:hover{background:#0369a1;}"
                            ".footer{margin-top:16px;font-size:11px;color:#64748b;text-align:center;}"
                            "</style></head><body><div class='box'>"
                            "<h2>🎻 ORCHESTRA Ingress Terminal</h2>"
                            "<p style='font-size:12px;color:#94a3b8;'>Over-The-Air Wi-Fi & Backend Provisioning</p>"
                            "<form method='POST' action='/save'>"
                            "<label>Wi-Fi Network Name (SSID)</label>"
                            "<input type='text' name='ssid' value='");
            html += savedSsid;
            html += F("' placeholder='Classroom / Lab Wi-Fi SSID' required>"
                      "<label>Wi-Fi Password</label>"
                      "<input type='password' name='password' placeholder='Leave blank if open Wi-Fi'>"
                      "<label>Node.js Backend Base URL</label>"
                      "<input type='text' name='backend' value='");
            html += savedBackend;
            html += F("' placeholder='http://orchestra.local:5000/api/v1' required>"
                      "<button type='submit'>Save Configuration &amp; Reboot</button>"
                      "</form>"
                      "<div class='footer'>Node ID: ");
            html += termId;
            html += F("<br/>mDNS Discovery: orchestra.local</div></div></body></html>");

            server.send(200, "text/html", html);
        });

        // 2. Save handler
        server.on("/save", HTTP_POST, [this]() {
            String newSsid = server.arg("ssid");
            String newPass = server.arg("password");
            String newBackend = server.arg("backend");

            newSsid.trim();
            newBackend.trim();

            if (newSsid.length() == 0 || newBackend.length() == 0) {
                server.send(400, "text/plain", "SSID and Backend URL are required!");
                return;
            }

            // Normalize backend URL: prepend protocol if omitted, strip trailing slash, ensure /api/v1
            if (!newBackend.startsWith("http://") && !newBackend.startsWith("https://")) {
                newBackend = "http://" + newBackend;
            }
            while (newBackend.endsWith("/")) {
                newBackend = newBackend.substring(0, newBackend.length() - 1);
            }
            if (!newBackend.endsWith("/api/v1")) {
                newBackend += "/api/v1";
            }

            // Save to NVS
            Preferences pref;
            pref.begin("net_cfg", false);
            pref.putString("ssid", newSsid);
            pref.putString("pass", newPass);
            pref.putString("backend_url", newBackend);
            pref.end();

            Serial.println(F("[CAPTIVE PORTAL] New credentials saved to NVS:"));
            Serial.printf(F("  SSID:    %s\n"), newSsid.c_str());
            Serial.printf(F("  Backend: %s\n"), newBackend.c_str());

            String html = F("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                            "<title>Config Saved</title><style>"
                            "body{font-family:sans-serif;background:#0f172a;color:#fff;text-align:center;padding:40px;}"
                            ".card{background:#1e293b;padding:24px;border-radius:12px;max-width:320px;margin:auto;border:1px solid #334155;}"
                            "h2{color:#34d399;}"
                            "</style></head><body><div class='card'>"
                            "<h2>&#10004; Configuration Saved!</h2>"
                            "<p style='font-size:13px;color:#cbd5e1;'>The terminal is rebooting and establishing cloud uplink...</p>"
                            "</div></body></html>");

            server.send(200, "text/html", html);

            restartRequested = true;
            restartTimer = millis();
        });

        // Captive portal probes for Android, iOS, macOS, and Windows
        auto handleRedirect = [this]() {
            server.sendHeader("Location", "http://192.168.4.1/");
            server.send(302, "text/plain", "");
        };

        server.on("/generate_204", handleRedirect);
        server.on("/gen_204", handleRedirect);
        server.on("/hotspot-detect.html", handleRedirect);
        server.on("/canonical.html", handleRedirect);
        server.on("/redirect", handleRedirect);
        server.on("/ncsi.txt", handleRedirect);
        server.on("/connecttest.txt", handleRedirect);
        server.on("/success.txt", handleRedirect);
        server.on("/library/test/success.html", handleRedirect);

        server.onNotFound(handleRedirect);

        server.begin();
        active = true;
        displayMgr.showCaptivePortal(apName.c_str(), "192.168.4.1");
    }

    void handle() {
        if (!active) return;
        dnsServer.processNextRequest();
        server.handleClient();

        if (restartRequested && (millis() - restartTimer >= 1500)) {
            Serial.println(F("[CAPTIVE PORTAL] Rebooting ESP32 into new Wi-Fi credentials..."));
            delay(100);
            ESP.restart();
        }
    }

    void stop() {
        if (!active) return;
        dnsServer.stop();
        server.stop();
        WiFi.softAPdisconnect(true);
        active = false;
        Serial.println(F("[CAPTIVE PORTAL] SoftAP stopped."));
    }
};

extern CaptivePortalManager captivePortal;

#endif // CAPTIVE_PORTAL_H
