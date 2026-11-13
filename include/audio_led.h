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
        // Sharp high click — clearly audible keypad feedback
        tone(BUZZER_PIN, 3800, 20);
    }

    void cueAccessGranted() {
        // ✅ Double rising chirp — crisp, positive, unmistakable
        // C7 → E7 rising pair, punchy and bright
        setLeds(true, false);
        tone(BUZZER_PIN, 2093, 90);   // C7
        delay(100);
        tone(BUZZER_PIN, 2637, 140);  // E7 (higher, longer = triumphant)
    }

    void cueAccessDenied() {
        // ❌ Harsh descending buzz pair — clearly negative, hard to ignore
        setLeds(false, true);
        tone(BUZZER_PIN, 600, 150);
        delay(160);
        tone(BUZZER_PIN, 300, 250);   // Drop an octave — unmistakably wrong
    }

    void cueSessionStateChanged(bool opened) {
        if (opened) {
            // 🟢 Session OPEN — ascending tri-tone fanfare (E6→A6→C7→E7)
            setLeds(true, false);
            tone(BUZZER_PIN, 1318, 90);  // E6
            delay(100);
            tone(BUZZER_PIN, 1760, 90);  // A6
            delay(100);
            tone(BUZZER_PIN, 2093, 110); // C7
            delay(120);
            tone(BUZZER_PIN, 2637, 220); // E7 — high bright finish
        } else {
            // 🔴 Session CLOSE — descending resolution (A6→E6→A5)
            setLeds(false, true);
            tone(BUZZER_PIN, 1760, 110); // A6
            delay(120);
            tone(BUZZER_PIN, 1318, 110); // E6
            delay(120);
            tone(BUZZER_PIN, 880, 280);  // A5 — long low closure
        }
    }

    void cuePacingViolation() {
        // ⚠️ Anti-passback deterrent — sharp high-low-high stutter
        digitalWrite(LED_RED_PIN, HIGH);
        tone(BUZZER_PIN, 3200, 60);
        delay(70);
        tone(BUZZER_PIN, 1600, 60);
        delay(70);
        tone(BUZZER_PIN, 3200, 60);
        delay(70);
        digitalWrite(LED_RED_PIN, LOW);
    }

    void cueBootSuccess() {
        // 🚀 Startup chime — confident rising pair, both LEDs flash
        tone(BUZZER_PIN, 1760, 90);   // A6
        delay(100);
        tone(BUZZER_PIN, 2637, 150);  // E7
        setLeds(true, true);
        delay(200);
        setLeds(false, false);
    }
};

extern AudioLedManager audioLed;

#endif // AUDIO_LED_H
