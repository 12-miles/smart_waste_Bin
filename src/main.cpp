#include <Arduino.h>
#include <ESP32Servo.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "config.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#define NTFY_TOPIC ""
#endif

enum class LidState { Closed, Opening, Open, Closing };

static Servo lidServo;
static LidState state = LidState::Closed;
static int servoAngle = SERVO_CLOSED_DEG;
static uint32_t lastSeenMs = 0;
static uint32_t lastSampleMs = 0;
static uint32_t lastStepMs = 0;
static uint8_t closeReadings = 0;
static bool personNear = false;

static uint32_t lastFillMs = 0;
static int fillPercent = -1;       // -1 = not measured yet
static uint8_t fullReadings = 0;
static bool binFull = false;
static bool alertPending = false;

static const bool wifiEnabled = (strlen(WIFI_SSID) > 0);

// Returns distance in cm, or -1 if nothing was detected (timeout).
static float readDistanceCm(uint8_t trigPin, uint8_t echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  // 25 ms timeout ~ 4 m max range
  unsigned long duration = pulseIn(echoPin, HIGH, 25000UL);
  if (duration == 0) return -1;
  return duration * 0.0343f / 2.0f;
}

static void beep(uint16_t freq, uint16_t ms) {
  tone(PIN_BUZZER, freq, ms);
}

static void updateLeds(uint32_t now) {
  bool closed = (state == LidState::Closed);
  digitalWrite(PIN_LED_READY, (closed && !binFull) ? HIGH : LOW);
  if (closed) {
    // Closed and full: red blinks. Closed and not full: red off.
    digitalWrite(PIN_LED_OPEN, (binFull && (now / 400) % 2) ? HIGH : LOW);
  } else {
    digitalWrite(PIN_LED_OPEN, HIGH);
  }
}

static void setState(LidState next) {
  state = next;
  switch (state) {
    case LidState::Opening: Serial.println("Lid opening"); break;
    case LidState::Open:    Serial.println("Lid open");    break;
    case LidState::Closing: Serial.println("Lid closing"); break;
    case LidState::Closed:
      Serial.println("Lid closed");
      // Measure fill shortly after the lid settles.
      lastFillMs = millis() - FILL_INTERVAL_MS + 500;
      break;
  }
}

static void sampleProximity(uint32_t now) {
  if (now - lastSampleMs < SAMPLE_INTERVAL_MS) return;
  lastSampleMs = now;

  float cm = readDistanceCm(PIN_TRIG, PIN_ECHO);
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

// Only meaningful with the lid closed, since the sensor sits under the lid.
static void sampleFill(uint32_t now) {
  if (state != LidState::Closed) return;
  if (now - lastFillMs < FILL_INTERVAL_MS) return;
  // Wait a ping interval after the proximity sensor so the two don't hear each other.
  if (now - lastSampleMs < SAMPLE_INTERVAL_MS / 2) return;
  lastFillMs = now;
  lastSampleMs = now;  // delays the next proximity ping as well

  float cm = readDistanceCm(PIN_FILL_TRIG, PIN_FILL_ECHO);
  if (cm < 0) return;  // no echo: keep the previous value

  float pct = (BIN_EMPTY_CM - cm) * 100.0f / (BIN_EMPTY_CM - BIN_FULL_CM);
  fillPercent = constrain((int)(pct + 0.5f), 0, 100);
  Serial.printf("Fill level: %d%% (%.1f cm)\n", fillPercent, cm);

  if (fillPercent >= FULL_PERCENT) {
    if (fullReadings < FULL_CONFIRM_COUNT) fullReadings++;
    if (!binFull && fullReadings >= FULL_CONFIRM_COUNT) {
      binFull = true;
      alertPending = true;
      Serial.println("Bin is full");
      beep(1500, 600);
    }
  } else {
    fullReadings = 0;
    if (binFull && fillPercent < FULL_CLEAR_PERCENT) {
      binFull = false;
      alertPending = false;
      Serial.println("Bin emptied");
    }
  }
}

// Sends the push notification. Blocks for a second or two, so only call with the lid closed.
static bool sendAlert() {
  WiFiClientSecure client;
  client.setInsecure();  // no certificate check: fine for a "bin is full" message
  HTTPClient http;
  if (!http.begin(client, String(NTFY_HOST) + NTFY_TOPIC)) return false;
  http.setTimeout(5000);
  http.addHeader("Title", "Smart bin");
  http.addHeader("Tags", "wastebasket");
  int code = http.POST("The bin is " + String(fillPercent) + "% full. Time to empty it.");
  http.end();
  Serial.printf("Alert sent, HTTP %d\n", code);
  return code >= 200 && code < 300;
}

static void handleAlert(uint32_t now) {
  static uint32_t lastTryMs = 0;
  if (!alertPending || !wifiEnabled) return;
  if (state != LidState::Closed || personNear) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (lastTryMs != 0 && now - lastTryMs < 30000) return;  // retry every 30 s on failure
  lastTryMs = now;
  if (sendAlert()) alertPending = false;
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
  pinMode(PIN_FILL_TRIG, OUTPUT);
  pinMode(PIN_FILL_ECHO, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_READY, OUTPUT);
  pinMode(PIN_LED_OPEN, OUTPUT);

  lidServo.setPeriodHertz(50);
  lidServo.attach(PIN_SERVO, 500, 2400);
  lidServo.write(SERVO_CLOSED_DEG);

  if (wifiEnabled) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);  // connects in the background
    Serial.printf("Connecting to Wi-Fi \"%s\"\n", WIFI_SSID);
  } else {
    Serial.println("Wi-Fi not configured: phone alerts disabled");
  }

  beep(2000, 100);  // power-on chirp
  Serial.println("Smart bin ready");
}

void loop() {
  uint32_t now = millis();
  sampleProximity(now);
  sampleFill(now);
  handleAlert(now);
  updateLeds(now);

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
