#ifndef AUDIO_LED_H
#define AUDIO_LED_H

#include <Arduino.h>
#include "config.h"

#define BUZZER_PWM_CHANNEL    0
#define BUZZER_PWM_RES        8     // 8-bit resolution (0..255)
#define BUZZER_DUTY_50        128   // 50% square wave duty cycle for maximum mechanical displacement

class AudioLedManager {
public:
    void begin() {
        pinMode(LED_GREEN_PIN, OUTPUT);
        pinMode(LED_RED_PIN, OUTPUT);
        pinMode(BUZZER_PIN, OUTPUT);

        digitalWrite(LED_GREEN_PIN, LOW);
        digitalWrite(LED_RED_PIN, LOW);
        digitalWrite(BUZZER_PIN, LOW);

        // Arduino ESP32 v3.x uses ledcAttach(pin, freq, resolution)
        ledcAttach(BUZZER_PIN, 2730, BUZZER_PWM_RES);
        silence();
    }

    void setLeds(bool green, bool red) {
        digitalWrite(LED_GREEN_PIN, green ? HIGH : LOW);
        digitalWrite(LED_RED_PIN, red ? HIGH : LOW);
    }

    /**
     * Hardware-level LEDC PWM driver:
     * - Reconfigures the carrier frequency dynamically to the buzzer's acoustic sweet spot
     * - Writes an exact 50% duty cycle (128 / 255) to drive the diaphragm at peak amplitude
     */
    void sound(uint32_t freqHz, uint32_t durationMs) {
        if (freqHz > 0) {
            ledcWriteTone(BUZZER_PIN, freqHz);
        } else {
            silence();
        }
        delay(durationMs);
        silence();
    }

    void silence() {
        ledcWrite(BUZZER_PIN, 0);
    }

    void playKeypadClick() {
        // High-amplitude resonant click at 2730 Hz
        sound(2730, 45);
    }

    void cueAccessGranted() {
        // ✅ Maximum Volume Affirmative Double-Chirp (2400 Hz → 2730 Hz resonant peak)
        setLeds(true, false);
        sound(2400, 150);
        delay(70);
        sound(2730, 260);
    }

    void cueAccessDenied() {
        // ❌ High-volume descending harsh double-buzz
        setLeds(false, true);
        sound(1500, 200);
        delay(80);
        sound(1000, 320);
    }

    void cueSessionStateChanged(bool opened) {
        if (opened) {
            // 🟢 Session OPEN — Loud resonant fanfare
            setLeds(true, false);
            sound(1800, 120);
            delay(60);
            sound(2200, 140);
            delay(60);
            sound(2730, 300);
        } else {
            // 🔴 Session CLOSE — Loud descending warning
            setLeds(false, true);
            sound(2730, 140);
            delay(60);
            sound(2000, 160);
            delay(60);
            sound(1200, 350);
        }
    }

    void cuePacingViolation() {
        // ⚠️ Loud rapid anti-passback stutter
        digitalWrite(LED_RED_PIN, HIGH);
        sound(2730, 90);
        delay(50);
        sound(2200, 90);
        delay(50);
        sound(2730, 110);
        digitalWrite(LED_RED_PIN, LOW);
    }

    void cueBootSuccess() {
        // 🚀 Startup chime
        sound(2200, 120);
        delay(60);
        sound(2730, 220);
        setLeds(true, true);
        delay(250);
        setLeds(false, false);
    }
};

extern AudioLedManager audioLed;

#endif // AUDIO_LED_H
