// ---------------------------------------------------------------------------
//  uno_servo.ino  --  Arduino Uno servo actuator
//
//  Listens on a software serial link (to the ESP32) for single-letter
//  commands, sweeps a servo to a target angle, holds it, then returns home.
//  Everything is non-blocking, so the link stays responsive during the hold.
//
//  Link protocol (newline-terminated ASCII, 9600 baud):
//     R          rotate: sweep to ANGLE_TARGET, hold HOLD_MS, return home
//     R:<ms>     rotate with a one-shot hold override, e.g. "R:4000"
//     Z          zero now: abort any hold and return home immediately
//     H:<ms>     set the default hold duration until next reset
//     S          status
//     P          ping
//
//  Replies:
//     OK <cmd>
//     ERR <reason>
//     ST <state> <angle> <remaining_ms>
//     PONG
// ---------------------------------------------------------------------------

#include <Servo.h>
#include <SoftwareSerial.h>

// =======================  TWEAK EVERYTHING HERE  ===========================

// --- pins ---
const uint8_t SERVO_PIN   = 9;    // servo signal (orange/white wire)
const uint8_t LINK_RX_PIN = 10;   // <- S3 GPIO17, direct
const uint8_t LINK_TX_PIN = 11;   // -> S3 GPIO18, THROUGH A DIVIDER (5V -> 3V3)

// --- link ---
const long LINK_BAUD  = 9600;     // keep low; SoftwareSerial + Servo are fussy
const long DEBUG_BAUD = 115200;   // USB serial, for watching what happens

// --- motion ---
int ANGLE_HOME   = 0;             // resting angle, degrees
int ANGLE_TARGET = 180;           // angle the button drives to, degrees

// How long to sit at ANGLE_TARGET before coming back. This is the number
// you asked to be able to change freely: 10 s = 10000.
unsigned long HOLD_MS = 10000UL;

// Sweep shaping. SWEEP_STEP_DEG = 0 snaps instantly (hard on cheap servos and
// on the Uno's 5 V rail). Larger step / smaller delay = faster sweep.
int           SWEEP_STEP_DEG = 3;
unsigned long SWEEP_STEP_MS  = 12;

// Pulse widths that correspond to true 0 deg and true 180 deg on YOUR servo.
// Stock library defaults are 544/2400. If 180 overshoots and the servo buzzes
// or stalls at the end stop, pull SERVO_MAX_US down (2300, 2200, ...).
const int SERVO_MIN_US = 544;
const int SERVO_MAX_US = 2400;

// Cut the servo signal this long after reaching home, so it stops humming and
// drawing current while idle. Set to 0 to keep it powered and stiff.
const unsigned long DETACH_AFTER_MS = 400;

// ===========================================================================

SoftwareSerial link(LINK_RX_PIN, LINK_TX_PIN);
Servo servo;

enum State : uint8_t { IDLE, SWEEP_OUT, HOLDING, SWEEP_BACK };

State         state        = IDLE;
int           angle        = ANGLE_HOME;
int           targetAngle  = ANGLE_HOME;
unsigned long holdMs       = HOLD_MS;   // active hold, may be overridden
unsigned long holdStarted  = 0;
unsigned long lastStepAt   = 0;
unsigned long homeReachedAt = 0;
bool          attached     = false;

char    rxBuf[24];
uint8_t rxLen = 0;

// --- servo helpers ---------------------------------------------------------

void servoAttach() {
  if (!attached) {
    servo.attach(SERVO_PIN, SERVO_MIN_US, SERVO_MAX_US);
    attached = true;
  }
}

void servoRelease() {
  if (attached) {
    servo.detach();
    attached = false;
  }
}

void writeAngle(int a) {
  angle = constrain(a, 0, 180);
  servoAttach();
  servo.write(angle);
}

// --- status ----------------------------------------------------------------

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

void sendStatus() {
  char out[48];
  snprintf(out, sizeof(out), "ST %s %d %lu", stateName(), angle, remainingMs());
  link.println(out);
  Serial.println(out);
}

// --- commands --------------------------------------------------------------

void startRotate(unsigned long hold) {
  holdMs      = hold;
  targetAngle = ANGLE_TARGET;
  state       = SWEEP_OUT;
  lastStepAt  = millis();
  servoAttach();
  if (SWEEP_STEP_DEG <= 0) writeAngle(targetAngle);
}

void goHomeNow() {
  targetAngle = ANGLE_HOME;
  state       = SWEEP_BACK;
  lastStepAt  = millis();
  servoAttach();
  if (SWEEP_STEP_DEG <= 0) writeAngle(targetAngle);
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

// Steps `angle` toward targetAngle. Returns true once it has arrived.
bool stepToward() {
  if (angle == targetAngle) return true;
  if (SWEEP_STEP_DEG <= 0) { writeAngle(targetAngle); return true; }
  if (millis() - lastStepAt < SWEEP_STEP_MS) return false;

  lastStepAt = millis();
  int delta  = targetAngle - angle;
  int step   = (abs(delta) < SWEEP_STEP_DEG) ? abs(delta) : SWEEP_STEP_DEG;
  writeAngle(angle + (delta > 0 ? step : -step));
  return angle == targetAngle;
}

void tickMotion() {
  switch (state) {
    case SWEEP_OUT:
      if (stepToward()) {
        state       = HOLDING;
        holdStarted = millis();
        Serial.println("holding");
      }
      break;

    case HOLDING:
      if (millis() - holdStarted >= holdMs) goHomeNow();
      break;

    case SWEEP_BACK:
      if (stepToward()) {
        state         = IDLE;
        homeReachedAt = millis();
        holdMs        = HOLD_MS;   // clear any one-shot override
        Serial.println("home");
      }
      break;

    case IDLE:
      if (DETACH_AFTER_MS && attached &&
          millis() - homeReachedAt >= DETACH_AFTER_MS) {
        servoRelease();
      }
      break;
  }
}

// --- sketch ----------------------------------------------------------------

void setup() {
  Serial.begin(DEBUG_BAUD);
  link.begin(LINK_BAUD);

  writeAngle(ANGLE_HOME);
  homeReachedAt = millis();

  Serial.println(F("uno_servo ready"));
  link.println("OK boot");
}

void loop() {
  readLink();
  tickMotion();
}
