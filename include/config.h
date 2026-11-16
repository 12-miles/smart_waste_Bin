#pragma once

// ---- Pins (change to match your wiring) ----
#define PIN_TRIG        5    // HC-SR04 trigger
#define PIN_ECHO        18   // HC-SR04 echo (5V output: use a voltage divider to 3.3V!)
#define PIN_SERVO       13   // servo signal
#define PIN_BUZZER      25   // active or passive buzzer
#define PIN_LED_READY   26   // green: bin closed, waiting
#define PIN_LED_OPEN    27   // red: lid open

// ---- Behaviour ----
#define OPEN_DISTANCE_CM   30     // open when something is closer than this
#define HOLD_OPEN_MS       3000   // keep lid open this long after the person leaves
#define SERVO_CLOSED_DEG   0
#define SERVO_OPEN_DEG     90
#define SERVO_STEP_MS      10     // delay per degree: higher = slower, gentler lid
#define SAMPLE_INTERVAL_MS 60     // HC-SR04 needs ~60 ms between pings
#define DETECT_COUNT       2      // consecutive close readings needed to open
