#include <Arduino.h>
#include <ESP32Servo.h>
#include "config.h"

enum class LidState { Closed, Opening, Open, Closing };

static Servo lidServo;
static LidState state = LidState::Closed;
static int servoAngle = SERVO_CLOSED_DEG;
static uint32_t lastSeenMs = 0;
static uint32_t lastSampleMs = 0;
static uint32_t lastStepMs = 0;
static uint8_t closeReadings = 0;
static bool personNear = false;

// Returns distance in cm, or -1 if nothing was detected (timeout).
static float readDistanceCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  // 25 ms timeout ~ 4 m max range
  unsigned long duration = pulseIn(PIN_ECHO, HIGH, 25000UL);
  if (duration == 0) return -1;
  return duration * 0.0343f / 2.0f;
}

static void beep(uint16_t freq, uint16_t ms) {
  tone(PIN_BUZZER, freq, ms);
}

static void setLeds() {
  bool closed = (state == LidState::Closed);
  digitalWrite(PIN_LED_READY, closed ? HIGH : LOW);
  digitalWrite(PIN_LED_OPEN, closed ? LOW : HIGH);
}

static void setState(LidState next) {
  state = next;
  setLeds();
  switch (state) {
    case LidState::Opening: Serial.println("Lid opening"); break;
    case LidState::Open:    Serial.println("Lid open");    break;
    case LidState::Closing: Serial.println("Lid closing"); break;
    case LidState::Closed:  Serial.println("Lid closed");  break;
  }
}

static void sampleSensor(uint32_t now) {
  if (now - lastSampleMs < SAMPLE_INTERVAL_MS) return;
  lastSampleMs = now;

  float cm = readDistanceCm();
  bool close = (cm > 0 && cm < OPEN_DISTANCE_CM);

  // Require a few consecutive readings so one noisy echo doesn't open the lid.
  if (close) {
    if (closeReadings < DETECT_COUNT) closeReadings++;
  } else {
    closeReadings = 0;
  }
  personNear = (closeReadings >= DETECT_COUNT);
  if (personNear) lastSeenMs = now;
}

// Moves the servo one degree at a time toward the target. Returns true when it arrives.
static bool stepServoToward(int target, uint32_t now) {
  if (servoAngle == target) return true;
  if (now - lastStepMs < SERVO_STEP_MS) return false;
  lastStepMs = now;
  servoAngle += (target > servoAngle) ? 1 : -1;
  lidServo.write(servoAngle);
  return servoAngle == target;
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_READY, OUTPUT);
  pinMode(PIN_LED_OPEN, OUTPUT);

  lidServo.setPeriodHertz(50);
  lidServo.attach(PIN_SERVO, 500, 2400);
  lidServo.write(SERVO_CLOSED_DEG);

  setLeds();
  beep(2000, 100);  // power-on chirp
  Serial.println("Smart bin ready");
}

void loop() {
  uint32_t now = millis();
  sampleSensor(now);

  switch (state) {
    case LidState::Closed:
      if (personNear) {
        beep(2500, 150);  // someone arrived
        setState(LidState::Opening);
      }
      break;

    case LidState::Opening:
      if (stepServoToward(SERVO_OPEN_DEG, now)) setState(LidState::Open);
      break;

    case LidState::Open:
      if (now - lastSeenMs >= HOLD_OPEN_MS) setState(LidState::Closing);
      break;

    case LidState::Closing:
      if (personNear) {
        // Someone came back while closing: reopen instead of shutting on their hand.
        setState(LidState::Opening);
      } else if (stepServoToward(SERVO_CLOSED_DEG, now)) {
        setState(LidState::Closed);
      }
      break;
  }
}
