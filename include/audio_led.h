#ifndef AUDIO_LED_H
#define AUDIO_LED_H

#include <Arduino.h>
#include "config.h"

class AudioLedManager {
private:
    volatile bool quietMode = false;

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

    void setQuietMode(bool quiet) {
        quietMode = quiet;
    }

    bool isQuiet() const {
        return quietMode;
    }

    void toggleQuietMode() {
        quietMode = !quietMode;
    }

    void playKeypadClick() {
        if (quietMode) {
            // Suppressed in quiet mode
            return;
        }
        tone(BUZZER_PIN, 2800, 25);
    }

    void cueAccessGranted() {
        setLeds(true, false);
        if (quietMode) {
            // Dampened subtle tick to prevent disrupting ongoing lectures
            tone(BUZZER_PIN, 1500, 20);
        } else {
            // Default classic high crisp single confirmation beep
            tone(BUZZER_PIN, 2200, 160);
        }
    }

    void cueAccessDenied() {
        setLeds(false, true);
        if (quietMode) {
            // Soft subtle low pulse
            tone(BUZZER_PIN, 350, 40);
        } else {
            // Default two low error pulses
            tone(BUZZER_PIN, 440, 180);
            delay(190);
            tone(BUZZER_PIN, 380, 220);
        }
    }

    void cueSessionStateChanged(bool opened) {
        if (opened) {
            setLeds(true, false);
            if (quietMode) {
                tone(BUZZER_PIN, 1760, 40);
            } else {
                tone(BUZZER_PIN, 1318, 100); // E6
                delay(110);
                tone(BUZZER_PIN, 1760, 120); // A6
                delay(130);
                tone(BUZZER_PIN, 2093, 220); // C7
            }
        } else {
            setLeds(false, true);
            if (quietMode) {
                tone(BUZZER_PIN, 880, 40);
            } else {
                tone(BUZZER_PIN, 1760, 120); // A6
                delay(130);
                tone(BUZZER_PIN, 1318, 120); // E6
                delay(130);
                tone(BUZZER_PIN, 880, 240);  // A5
            }
        }
    }

    void cuePacingViolation() {
        digitalWrite(LED_RED_PIN, HIGH);
        if (quietMode) {
            delay(50);
        } else {
            tone(BUZZER_PIN, 900, 70);
            delay(80);
        }
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
