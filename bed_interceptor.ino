// Bed control interceptor for Anycubic Kobra X
// Arduino UNO R4 WiFi version - v6 (working production code)
//
// Reduces LED flicker on PV inverter battery mode by intercepting
// the bed heater control signal and converting fractional pulse-width
// firing into slow bang-bang cycles.
//
// Strategy: skip ON bursts shorter than 3 seconds entirely (no
// accumulation). The mainboard's PID self-compensates by demanding
// more in subsequent windows when the bed actually cools down.
//
// See README.md for wiring, theory, and parameter tuning.

const int IO_INPUT  = 2;          // Input from mainboard's bed control wire
const int IO_OUTPUT = 3;          // Output to PSU board's triac driver
const int LED_PIN   = LED_BUILTIN;

// === Tuning parameters ===
const unsigned long WINDOW_MS = 10000;        // Measurement/output cycle (10s)
const float DAMPING_FACTOR = 0.5;             // Halve ON time to absorb thermal inertia
const unsigned long MIN_BURST_MS = 3000;      // Skip ON bursts shorter than this
const unsigned long SAMPLE_US = 500;          // 2 kHz input sampling rate
const unsigned long IDLE_TIMEOUT_MS = 1500;   // Stop output if mainboard idle this long

void setup() {
  pinMode(IO_INPUT, INPUT);
  pinMode(IO_OUTPUT, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(IO_OUTPUT, LOW);
  digitalWrite(LED_PIN, LOW);
  Serial.begin(115200);
  delay(500);
  Serial.println("=== Bed interceptor v6 (skip short bursts) ===");
}

// Drive bed ON for up to maxMs, but abort early if mainboard
// signal stays LOW for IDLE_TIMEOUT_MS (setpoint reached).
// Returns the actual ON time delivered.
unsigned long deliverHeatSafely(unsigned long maxMs) {
  unsigned long start = millis();
  unsigned long lastHighTime = millis();

  digitalWrite(IO_OUTPUT, HIGH);
  digitalWrite(LED_PIN, HIGH);

  while (millis() - start < maxMs) {
    if (digitalRead(IO_INPUT) == HIGH) {
      lastHighTime = millis();
    } else if (millis() - lastHighTime > IDLE_TIMEOUT_MS) {
      Serial.println("  [Early stop: mainboard idle]");
      break;
    }
    delay(5);
  }

  digitalWrite(IO_OUTPUT, LOW);
  digitalWrite(LED_PIN, LOW);
  return millis() - start;
}

void loop() {
  // === Phase 1: Measure mainboard demand over WINDOW_MS ===
  unsigned long activeSamples = 0;
  unsigned long totalSamples = 0;
  unsigned long start = millis();

  while (millis() - start < WINDOW_MS) {
    if (digitalRead(IO_INPUT) == HIGH) activeSamples++;
    totalSamples++;
    delayMicroseconds(SAMPLE_US);
  }

  float duty = (float)activeSamples / (float)totalSamples;
  unsigned long requestedOnTime = (unsigned long)(duty * WINDOW_MS);
  unsigned long onTime = (unsigned long)(requestedOnTime * DAMPING_FACTOR);

  Serial.print("Duty: ");
  Serial.print(duty * 100.0, 1);
  Serial.print("%  Requested: ");
  Serial.print(requestedOnTime);
  Serial.print(" ms  Damped: ");
  Serial.print(onTime);
  Serial.print(" ms");

  // === Phase 2: Skip if too short, otherwise fire ===
  if (onTime < MIN_BURST_MS) {
    Serial.println("  [SKIP - too short]");
    digitalWrite(IO_OUTPUT, LOW);
    digitalWrite(LED_PIN, LOW);

    unsigned long elapsed = millis() - start;
    if (elapsed < WINDOW_MS) {
      delay(WINDOW_MS - elapsed);
    }
    return;
  }

  Serial.println("  -> FIRE");
  unsigned long actualOn = deliverHeatSafely(onTime);

  Serial.print("  Actual ON: ");
  Serial.print(actualOn);
  Serial.println(" ms");

  // Wait remainder of window
  unsigned long elapsed = millis() - start;
  unsigned long remaining = (elapsed < WINDOW_MS) ? (WINDOW_MS - elapsed) : 0;
  digitalWrite(IO_OUTPUT, LOW);
  digitalWrite(LED_PIN, LOW);
  if (remaining > 0) {
    delay(remaining);
  }
}
