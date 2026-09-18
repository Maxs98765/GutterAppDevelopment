// ---------------------------------------------------------------------------
//  uno_servo.ino  --  Arduino Uno actuator, driving a 12V DC worm-gear motor
//
//  HARDWARE CHANGE: this sketch originally drove a true RC hobby servo via
//  the Arduino Servo library (PWM pulse width -> shaft angle, 0-180 deg).
//  The actual part in hand is a DC12V 16RPM worm-gear motor: two wires
//  only (power +/-), no signal wire, no internal position electronics.
//  Sending a servo PWM pulse to a motor like this does nothing useful --
//  it needs to be switched and reversed with real current through a motor
//  driver (H-bridge), and "angle" has to be estimated open-loop, by
//  running the motor for a calculated time at its rated speed.
//
//  What stayed the same: the serial link protocol to the ESP32 hub is
//  UNCHANGED (same commands, same replies), so net.cpp / http_api.cpp /
//  the phone app all keep working without modification. Only this
//  sketch, and the physical wiring below, changed.
//
//  Link protocol (newline-terminated ASCII, 9600 baud) -- unchanged:
//     R          rotate: sweep to ANGLE_TARGET, hold HOLD_MS, return home
//     R:<ms>     rotate with a one-shot hold override, e.g. "R:4000"
//     Z          zero now: abort any hold/motion and return home
//     H:<ms>     set the default hold duration until next reset
//     S          status
//     P          ping
//
//  Replies -- unchanged:
//     OK <cmd>
//     ERR <reason>
//     ST <state> <angle> <remaining_ms>
//     PONG
//
//  Wiring (see the project's wiring guide artifact for the full diagram):
//     Motor red wire    -> L298N OUT1
//     Motor black wire  -> L298N OUT2   (swap OUT1/OUT2 if "forward" comes
//                                        out backwards -- polarity here is
//                                        arbitrary, just be consistent)
//     L298N IN1         -> Uno D6
//     L298N IN2         -> Uno D7
//     L298N ENA         -> Uno D9        (remove the ENA jumper on the
//                                         board so this pin controls it --
//                                         with the jumper left in, the
//                                         board ignores this pin and the
//                                         motor free-runs whenever IN1/IN2
//                                         are set, which breaks the timing)
//     L298N +12V, GND   -> a separate 12V supply, sized for this motor's
//                          stall current, NOT the Uno's 5V/USB power
//     L298N GND         -> tied to Uno GND (shared ground, required)
//
//     Uno D10 (SoftwareSerial RX) <- ESP32 TX, direct
//     Uno D11 (SoftwareSerial TX) -> ESP32 RX, THROUGH A DIVIDER (5V->3.3V)
// ---------------------------------------------------------------------------

#include <SoftwareSerial.h>

// =======================  TWEAK EVERYTHING HERE  ===========================

// --- pins ---
const uint8_t MOTOR_ENA_PIN = 9;    // L298N ENA -- HIGH = driver enabled
const uint8_t MOTOR_IN1_PIN = 6;    // L298N IN1
const uint8_t MOTOR_IN2_PIN = 7;    // L298N IN2

const uint8_t LINK_RX_PIN = 10;     // <- ESP32 TX, direct
const uint8_t LINK_TX_PIN = 11;     // -> ESP32 RX, THROUGH A DIVIDER (5V -> 3V3)

// --- link ---
const long LINK_BAUD  = 9600;       // keep low; SoftwareSerial is fussy
const long DEBUG_BAUD = 115200;     // USB serial, for watching what happens

// --- motion ---
int ANGLE_HOME   = 0;                // resting angle, degrees
int ANGLE_TARGET = 120;              // angle the rotate command drives to

// How long to sit at ANGLE_TARGET before coming back. This is the number
// meant to be changed freely: 10 s = 10000. Same knob as before.
unsigned long HOLD_MS = 10000UL;

// The motor's rated output speed, from its label (DC12V 16RPM). This is
// the whole basis for the "120 degrees" timing below -- there is no shaft
// position sensor, so a rotate command just runs the motor forward for
// however long 120 degrees SHOULD take at this speed, and trusts it.
//
// Real speed varies with load, supply voltage, and the motor's own
// manufacturing tolerance, so this drifts over repeated cycles. If the
// arm visibly over- or under-shoots ANGLE_TARGET, measure the real
// travel with a protractor over several cycles and adjust this number
// (lower MOTOR_RPM if it's overshooting -- i.e. actually moving faster
// than 16 RPM under your load -- raise it if undershooting).
float MOTOR_RPM = 16.0f;

// Derived: milliseconds it takes the motor to turn one degree at
// MOTOR_RPM. Do not edit this directly -- edit MOTOR_RPM above instead.
float msPerDegree() { return 60000.0f / (MOTOR_RPM * 360.0f); }

// Cut power to the driver's ENA pin whenever the motor isn't actively
// moving. A worm gear is self-locking (the gear geometry resists being
// back-driven), so the arm holds its position with the motor completely
// off during HOLDING -- unlike a hobby servo, nothing here needs to stay
// powered to keep from drifting.

// ===========================================================================

SoftwareSerial link(LINK_RX_PIN, LINK_TX_PIN);

enum State : uint8_t { IDLE, SWEEP_OUT, HOLDING, SWEEP_BACK };

State         state         = IDLE;
int           angle         = ANGLE_HOME;   // estimated, not sensed
unsigned long holdMs        = HOLD_MS;      // active hold, may be overridden
unsigned long holdStarted   = 0;
unsigned long moveStartedAt = 0;
unsigned long moveDurationMs = 0;           // how long the current leg runs

char    rxBuf[24];
uint8_t rxLen = 0;

// --- motor helpers -----------------------------------------------------

void motorStop() {
  digitalWrite(MOTOR_ENA_PIN, LOW);
  digitalWrite(MOTOR_IN1_PIN, LOW);
  digitalWrite(MOTOR_IN2_PIN, LOW);
}

void motorRun(bool forward) {
  digitalWrite(MOTOR_IN1_PIN, forward ? HIGH : LOW);
  digitalWrite(MOTOR_IN2_PIN, forward ? LOW  : HIGH);
  digitalWrite(MOTOR_ENA_PIN, HIGH);
}

// --- status --------------------------------------------------------------

const char *stateName() {
  switch (state) {
    case IDLE:       return "idle";
    case SWEEP_OUT:  return "sweep_out";
    case HOLDING:    return "holding";
    case SWEEP_BACK: return "sweep_back";
  }
  return "?";
}

unsigned long remainingMs() {
  if (state != HOLDING) return 0;
  unsigned long elapsed = millis() - holdStarted;
  return (elapsed >= holdMs) ? 0 : (holdMs - elapsed);
}

// Interpolates a reported angle from elapsed time during a sweep, so
// GET /status still shows a moving number the way it did with the real
// servo, even though nothing is actually measuring the shaft.
void updateEstimatedAngle() {
  if (state == SWEEP_OUT || state == SWEEP_BACK) {
    unsigned long elapsed = millis() - moveStartedAt;
    if (elapsed >= moveDurationMs) elapsed = moveDurationMs;
    float frac = moveDurationMs ? (float)elapsed / (float)moveDurationMs : 1.0f;
    int from = (state == SWEEP_OUT) ? ANGLE_HOME : ANGLE_TARGET;
    int to   = (state == SWEEP_OUT) ? ANGLE_TARGET : ANGLE_HOME;
    angle = from + (int)((to - from) * frac);
  }
}

void sendStatus() {
  updateEstimatedAngle();
  char out[48];
  snprintf(out, sizeof(out), "ST %s %d %lu", stateName(), angle, remainingMs());
  link.println(out);
  Serial.println(out);
}

// --- commands --------------------------------------------------------------

void startRotate(unsigned long hold) {
  holdMs        = hold;
  moveDurationMs = (unsigned long)(ANGLE_TARGET * msPerDegree());
  moveStartedAt = millis();
  state         = SWEEP_OUT;
  motorRun(true);   // forward, toward ANGLE_TARGET
}

// Returns home from wherever the arm actually is right now, not just from
// ANGLE_TARGET -- if this is called mid-sweep-out, it only reverses for
// as long as the forward leg has actually been running so far, so an
// abort mid-motion still lands close to home instead of overshooting
// past it.
void goHomeNow() {
  unsigned long reverseMs;

  if (state == SWEEP_OUT) {
    unsigned long elapsed = millis() - moveStartedAt;
    reverseMs = (elapsed < moveDurationMs) ? elapsed : moveDurationMs;
  } else if (state == SWEEP_BACK) {
    // Already heading home; let the existing timer keep running rather
    // than restarting it.
    return;
  } else {
    // HOLDING, or a Z sent while IDLE (harmless no-op timing-wise, but we
    // still run the full leg once in case IDLE was reached by anything
    // other than a completed sweep).
    reverseMs = (unsigned long)(ANGLE_TARGET * msPerDegree());
  }

  moveDurationMs = reverseMs;
  moveStartedAt  = millis();
  state          = SWEEP_BACK;
  motorRun(false);   // reverse, toward ANGLE_HOME
}

void handleCommand(char *line) {
  // strip stray whitespace/CR
  while (*line == ' ' || *line == '\r' || *line == '\t') line++;
  if (*line == '\0') return;

  char  cmd = toupper(line[0]);
  char *arg = strchr(line, ':');
  long  val = arg ? atol(arg + 1) : -1;

  switch (cmd) {
    case 'R':
      if (state != IDLE) {
        // Open-loop timing only stays trustworthy if every rotate starts
        // from a fully-completed, known-home position. Refuse to stack a
        // new rotate on top of one still in progress; send Z first.
        link.println("ERR busy");
        break;
      }
      startRotate(val > 0 ? (unsigned long)val : HOLD_MS);
      link.println("OK R");
      break;

    case 'Z':
      goHomeNow();
      link.println("OK Z");
      break;

    case 'H':
      if (val > 0) {
        HOLD_MS = (unsigned long)val;
        link.print("OK H:");
        link.println(HOLD_MS);
      } else {
        link.println("ERR need H:<ms>");
      }
      break;

    case 'S':
      sendStatus();
      break;

    case 'P':
      link.println("PONG");
      break;

    default:
      link.println("ERR unknown");
      break;
  }
}

void readLink() {
  while (link.available()) {
    char c = link.read();
    if (c == '\n' || c == '\r') {
      if (rxLen) {
        rxBuf[rxLen] = '\0';
        handleCommand(rxBuf);
        rxLen = 0;
      }
    } else if (rxLen < sizeof(rxBuf) - 1) {
      rxBuf[rxLen++] = c;
    }
  }
}

// --- motion tick -----------------------------------------------------------

void tickMotion() {
  switch (state) {
    case SWEEP_OUT:
      updateEstimatedAngle();
      if (millis() - moveStartedAt >= moveDurationMs) {
        angle       = ANGLE_TARGET;
        motorStop();
        state       = HOLDING;
        holdStarted = millis();
        Serial.println("holding");
      }
      break;

    case HOLDING:
      if (millis() - holdStarted >= holdMs) goHomeNow();
      break;

    case SWEEP_BACK:
      updateEstimatedAngle();
      if (millis() - moveStartedAt >= moveDurationMs) {
        angle  = ANGLE_HOME;
        motorStop();
        state  = IDLE;
        holdMs = HOLD_MS;   // clear any one-shot override
        Serial.println("home");
      }
      break;

    case IDLE:
      // Motor is already off (motorStop() was called the instant we
      // arrived here) -- nothing to do. The worm gear holds position
      // on its own.
      break;
  }
}

// --- sketch ----------------------------------------------------------------

void setup() {
  Serial.begin(DEBUG_BAUD);
  link.begin(LINK_BAUD);

  pinMode(MOTOR_ENA_PIN, OUTPUT);
  pinMode(MOTOR_IN1_PIN, OUTPUT);
  pinMode(MOTOR_IN2_PIN, OUTPUT);
  motorStop();

  angle = ANGLE_HOME;

  Serial.println(F("uno_servo ready (DC gear-motor build, open-loop timing)"));
  Serial.print(F("ms per degree at rated speed: "));
  Serial.println(msPerDegree());
  link.println("OK boot");
}

void loop() {
  readLink();
  tickMotion();
}
