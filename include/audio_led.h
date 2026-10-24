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
        tone(BUZZER_PIN, 2800, 25);
    }

    void cueAccessGranted() {
        // High crisp single confirmation beep + Green LED
        setLeds(true, false);
        tone(BUZZER_PIN, 2200, 160);
    }

    void cueAccessDenied() {
        // Two low error pulses + Red LED
        setLeds(false, true);
        tone(BUZZER_PIN, 440, 180);
        delay(190);
        tone(BUZZER_PIN, 380, 220);
    }

    void cueSessionStateChanged(bool opened) {
        // Harmonious chime when faculty toggles session trust boundary
        if (opened) {
            setLeds(true, false);
            tone(BUZZER_PIN, 1318, 100); // E6
            delay(110);
            tone(BUZZER_PIN, 1760, 120); // A6
            delay(130);
            tone(BUZZER_PIN, 2093, 220); // C7
        } else {
            setLeds(false, true);
            tone(BUZZER_PIN, 1760, 120); // A6
            delay(130);
            tone(BUZZER_PIN, 1318, 120); // E6
            delay(130);
            tone(BUZZER_PIN, 880, 240);  // A5
        }
    }

    void cuePacingViolation() {
        // Rapid sharp warning chirp to deter proxy tapping attempts
        digitalWrite(LED_RED_PIN, HIGH);
        tone(BUZZER_PIN, 900, 70);
        delay(80);
        digitalWrite(LED_RED_PIN, LOW);
    }

    void cueBootSuccess() {
        tone(BUZZER_PIN, 1200, 80);
        delay(90);
        tone(BUZZER_PIN, 1800, 100);
        setLeds(true, true);
        delay(150);
        setLeds(false, false);
    }
};

extern AudioLedManager audioLed;

#endif // AUDIO_LED_H
