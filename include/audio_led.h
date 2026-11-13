#ifndef AUDIO_LED_H
#define AUDIO_LED_H

#include <Arduino.h>
#include "config.h"

class AudioLedManager {
public:
    void begin() {
        pinMode(LED_GREEN_PIN, OUTPUT);
        pinMode(LED_RED_PIN, OUTPUT);
        pinMode(BUZZER_PIN, OUTPUT);

        digitalWrite(LED_GREEN_PIN, LOW);
        digitalWrite(LED_RED_PIN, LOW);
        digitalWrite(BUZZER_PIN, LOW);
    }

    void setLeds(bool green, bool red) {
        digitalWrite(LED_GREEN_PIN, green ? HIGH : LOW);
        digitalWrite(LED_RED_PIN, red ? HIGH : LOW);
    }

    void playKeypadClick() {
        // High-resonant punchy click
        tone(BUZZER_PIN, 2700, 35);
    }

    void cueAccessGranted() {
        // ✅ High-resonant double pulse (resonant peak around 2400-2700 Hz)
        // Sustained duration to maximize acoustic transducer volume in classrooms
        setLeds(true, false);
        tone(BUZZER_PIN, 2400, 160);
        delay(180);
        tone(BUZZER_PIN, 2700, 260); // Longer resonant tone for maximum sound pressure
    }

    void cueAccessDenied() {
        // ❌ Louder, longer dual warning buzz
        setLeds(false, true);
        tone(BUZZER_PIN, 1200, 220);
        delay(240);
        tone(BUZZER_PIN, 800, 340);
    }

    void cueSessionStateChanged(bool opened) {
        if (opened) {
            // 🟢 Session OPEN — Loud resonant rising flourish
            setLeds(true, false);
            tone(BUZZER_PIN, 1500, 140);
            delay(150);
            tone(BUZZER_PIN, 2000, 140);
            delay(150);
            tone(BUZZER_PIN, 2700, 320); // Long peak resonance
        } else {
            // 🔴 Session CLOSE — Loud descending sequence
            setLeds(false, true);
            tone(BUZZER_PIN, 2400, 150);
            delay(160);
            tone(BUZZER_PIN, 1800, 160);
            delay(170);
            tone(BUZZER_PIN, 1000, 350);
        }
    }

    void cuePacingViolation() {
        // ⚠️ Loud anti-passback alert
        digitalWrite(LED_RED_PIN, HIGH);
        tone(BUZZER_PIN, 2700, 100);
        delay(110);
        tone(BUZZER_PIN, 2000, 100);
        delay(110);
        tone(BUZZER_PIN, 2700, 120);
        delay(130);
        digitalWrite(LED_RED_PIN, LOW);
    }

    void cueBootSuccess() {
        // 🚀 Startup chime
        tone(BUZZER_PIN, 2000, 140);
        delay(150);
        tone(BUZZER_PIN, 2700, 240);
        setLeds(true, true);
        delay(250);
        setLeds(false, false);
    }
};

extern AudioLedManager audioLed;

#endif // AUDIO_LED_H
