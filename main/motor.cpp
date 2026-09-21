// ---------------------------------------------------------------------------
//  motor.cpp -- Cytron MD13S + BRINGSMART worm-gear motor, driven directly
//               by this board's own GPIOs
//
//  HARDWARE CHANGE: this replaces BOTH uno_link.cpp (the UART master that
//  used to talk to a separate Arduino Uno) AND uno_servo.ino (the sketch
//  that ran ON that Uno). ListofMaterials.xlsx has no Arduino Uno and no
//  L298N anywhere in it -- the motor driver in the BOM is a Cytron MD13S,
//  a "smart" driver with only 2 control pins (PWM speed + DIR direction),
//  simple enough that the ESP32-P4 hub can drive it directly. There is no
//  second microcontroller left in this design at all: the timed-rotation
//  state machine that used to run on the Uno (IDLE/SWEEP_OUT/HOLDING/
//  SWEEP_BACK) now runs right here, ticked by a FreeRTOS task instead of
//  an Arduino loop().
//
//  What stayed the same: the open-loop timing approach itself. The
//  BRINGSMART motor in the BOM is the same 12V/16RPM self-locking worm
//  gear motor as before -- two wires, no position sensor -- so a rotate
//  command still just runs the motor forward for however long the target
//  angle SHOULD take at the rated speed, and trusts it. See config.h's
//  kMotorRpm/kAngleTarget for the numbers, and the note below on
//  recalibrating them.
//
//  Wiring (see the wiring guide -- it still shows the old L298N layout as
//  of this writing and needs a matching update):
//     Motor red wire    -> Cytron MD13S "M1+" (or M1A, depending on the
//                           silkscreen -- check your unit's manual)
//     Motor black wire  -> Cytron MD13S "M1-" / M1B   (swap the two motor
//                           leads if "forward" comes out backwards --
//                           polarity here is arbitrary, just be consistent)
//     Cytron MD13S "PWM" -> this board's GPIO cfg::kMotorPwmPin
//     Cytron MD13S "DIR" -> this board's GPIO cfg::kMotorDirPin
//     Cytron MD13S VCC (logic, 5V or 3.3V per its manual) -> a logic supply
//     Cytron MD13S "VIN"/"+", "GND" (power in, 6-30V) -> the 12V rail,
//                           sized for this motor's stall current
//     Cytron MD13S GND       -> tied to this board's GND (shared ground,
//                           required)
//
//  Recalibrating the timing: if the arm visibly over- or under-shoots
//  cfg::kAngleTarget, measure the real travel with a protractor over
//  several cycles and adjust cfg::kMotorRpm in config.h (lower it if the
//  arm is overshooting -- i.e. actually moving faster than 16 RPM under
//  your load -- raise it if undershooting). Real speed varies with load,
//  supply voltage, and the motor's own manufacturing tolerance, exactly
//  as it did before this rewrite.
// ---------------------------------------------------------------------------

#include "motor.h"

#include "config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "motor";

namespace {

constexpr ledc_mode_t      kLedcMode    = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t     kLedcTimer   = LEDC_TIMER_0;
constexpr ledc_channel_t   kLedcChannel = LEDC_CHANNEL_0;
constexpr ledc_timer_bit_t kLedcResBits = LEDC_TIMER_10_BIT;   // duty 0-1023

enum State : uint8_t { IDLE, SWEEP_OUT, HOLDING, SWEEP_BACK };

SemaphoreHandle_t g_lock = nullptr;

State    g_state          = IDLE;
int      g_angle          = 0;              // estimated, not sensed
uint32_t g_holdMs         = 0;
int64_t  g_holdStartedUs  = 0;
int64_t  g_moveStartedUs  = 0;
uint32_t g_moveDurationMs = 0;

int64_t nowUs() { return esp_timer_get_time(); }

int64_t elapsedMs(int64_t sinceUs)
{
    int64_t e = (nowUs() - sinceUs) / 1000;
    return e < 0 ? 0 : e;
}

float msPerDegree() { return 60000.0f / (cfg::kMotorRpm * 360.0f); }

const char *stateName(State s)
{
    switch (s) {
        case IDLE:       return "idle";
        case SWEEP_OUT:  return "sweep_out";
        case HOLDING:    return "holding";
        case SWEEP_BACK: return "sweep_back";
    }
    return "?";
}

// --- motor drive -----------------------------------------------------------

void motorStop()
{
    ledc_set_duty(kLedcMode, kLedcChannel, 0);
    ledc_update_duty(kLedcMode, kLedcChannel);
}

void motorRun(bool forward)
{
    bool dirHigh = forward ? cfg::kMotorDirForwardIsHigh
                           : !cfg::kMotorDirForwardIsHigh;
    gpio_set_level(static_cast<gpio_num_t>(cfg::kMotorDirPin), dirHigh ? 1 : 0);

    uint32_t maxDuty = (1u << kLedcResBits) - 1;
    uint32_t duty    = maxDuty * static_cast<uint32_t>(cfg::kMotorPwmDutyPercent) / 100;
    ledc_set_duty(kLedcMode, kLedcChannel, duty);
    ledc_update_duty(kLedcMode, kLedcChannel);
}

// --- estimated angle -- mirrors the old uno_servo.ino's interpolation ------

void updateEstimatedAngle()
{
    if (g_state != SWEEP_OUT && g_state != SWEEP_BACK) return;

    int64_t elapsed = elapsedMs(g_moveStartedUs);
    if (static_cast<uint32_t>(elapsed) >= g_moveDurationMs) elapsed = g_moveDurationMs;

    float frac = g_moveDurationMs
        ? static_cast<float>(elapsed) / static_cast<float>(g_moveDurationMs)
        : 1.0f;

    int from = (g_state == SWEEP_OUT) ? cfg::kAngleHome   : cfg::kAngleTarget;
    int to   = (g_state == SWEEP_OUT) ? cfg::kAngleTarget : cfg::kAngleHome;
    g_angle  = from + static_cast<int>((to - from) * frac);
}

uint32_t remainingHoldMs()
{
    if (g_state != HOLDING) return 0;
    int64_t elapsed = elapsedMs(g_holdStartedUs);
    return (static_cast<uint32_t>(elapsed) >= g_holdMs)
        ? 0 : (g_holdMs - static_cast<uint32_t>(elapsed));
}

// --- commands (always called with g_lock held) ------------------------

std::string startRotateLocked(uint32_t hold)
{
    if (g_state != IDLE) return "ERR busy";

    g_holdMs         = hold;
    g_moveDurationMs = static_cast<uint32_t>(cfg::kAngleTarget * msPerDegree());
    g_moveStartedUs  = nowUs();
    g_state          = SWEEP_OUT;
    motorRun(true);   // forward, toward cfg::kAngleTarget
    return "OK R";
}

// Mirrors the old uno_servo.ino's goHomeNow(): returns from wherever the
// arm actually is, not just from the target, so an abort mid-sweep still
// lands close to home instead of overshooting past it.
void goHomeNowLocked()
{
    uint32_t reverseMs;

    if (g_state == SWEEP_OUT) {
        int64_t elapsed = elapsedMs(g_moveStartedUs);
        reverseMs = (static_cast<uint32_t>(elapsed) < g_moveDurationMs)
            ? static_cast<uint32_t>(elapsed) : g_moveDurationMs;
    } else if (g_state == SWEEP_BACK) {
        return;   // already heading home; let the existing timer keep running
    } else {
        // HOLDING, or zero() called while IDLE (a harmless no-op timing-
        // wise, but still run the full leg once in case IDLE was reached
        // by anything other than a completed sweep).
        reverseMs = static_cast<uint32_t>(cfg::kAngleTarget * msPerDegree());
    }

    g_moveDurationMs = reverseMs;
    g_moveStartedUs  = nowUs();
    g_state          = SWEEP_BACK;
    motorRun(false);   // reverse, toward cfg::kAngleHome
}

// --- tick, run every cfg::kMotorTickMs ----------------------------------

void tick()
{
    switch (g_state) {
    case SWEEP_OUT:
        updateEstimatedAngle();
        if (elapsedMs(g_moveStartedUs) >= static_cast<int64_t>(g_moveDurationMs)) {
            g_angle         = cfg::kAngleTarget;
            motorStop();
            g_state         = HOLDING;
            g_holdStartedUs = nowUs();
            ESP_LOGI(TAG, "holding");
        }
        break;

    case HOLDING:
        if (elapsedMs(g_holdStartedUs) >= static_cast<int64_t>(g_holdMs)) {
            goHomeNowLocked();
        }
        break;

    case SWEEP_BACK:
        updateEstimatedAngle();
        if (elapsedMs(g_moveStartedUs) >= static_cast<int64_t>(g_moveDurationMs)) {
            g_angle  = cfg::kAngleHome;
            motorStop();
            g_state  = IDLE;
            g_holdMs = cfg::kHoldMs;   // clear any one-shot override
            ESP_LOGI(TAG, "home");
        }
        break;

    case IDLE:
        // Motor is already off (motorStop() was called the instant we
        // arrived here) -- nothing to do. The worm gear holds position
        // on its own, exactly as it did before this rewrite.
        break;
    }
}

void motorTask(void *)
{
    while (true) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
        tick();
        xSemaphoreGive(g_lock);
        vTaskDelay(pdMS_TO_TICKS(cfg::kMotorTickMs));
    }
}

} // namespace

namespace motor {

bool init()
{
    g_lock = xSemaphoreCreateMutex();
    if (!g_lock) return false;

    gpio_config_t dirCfg = {};
    dirCfg.pin_bit_mask = 1ULL << cfg::kMotorDirPin;
    dirCfg.mode         = GPIO_MODE_OUTPUT;
    if (gpio_config(&dirCfg) != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config (DIR pin) failed");
        return false;
    }
    gpio_set_level(static_cast<gpio_num_t>(cfg::kMotorDirPin), 0);

    ledc_timer_config_t timerCfg = {};
    timerCfg.speed_mode      = kLedcMode;
    timerCfg.timer_num       = kLedcTimer;
    timerCfg.duty_resolution = kLedcResBits;
    timerCfg.freq_hz         = cfg::kMotorPwmFreqHz;
    timerCfg.clk_cfg         = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timerCfg) != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed");
        return false;
    }

    ledc_channel_config_t chanCfg = {};
    chanCfg.gpio_num   = cfg::kMotorPwmPin;
    chanCfg.speed_mode = kLedcMode;
    chanCfg.channel    = kLedcChannel;
    chanCfg.timer_sel  = kLedcTimer;
    chanCfg.duty       = 0;
    chanCfg.hpoint     = 0;
    if (ledc_channel_config(&chanCfg) != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed");
        return false;
    }

    motorStop();
    g_state  = IDLE;
    g_angle  = cfg::kAngleHome;
    g_holdMs = cfg::kHoldMs;

    xTaskCreate(motorTask, "motor_tick", 3072, nullptr, 10, nullptr);

    ESP_LOGI(TAG, "ready: pwm=gpio%d dir=gpio%d, %.2f ms/degree at %.0f RPM, "
                  "target %d deg",
             cfg::kMotorPwmPin, cfg::kMotorDirPin, msPerDegree(),
             cfg::kMotorRpm, cfg::kAngleTarget);
    return true;
}

std::string rotate(uint32_t holdMsOverride)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    std::string reply = startRotateLocked(
        holdMsOverride > 0 ? holdMsOverride : cfg::kHoldMs);
    xSemaphoreGive(g_lock);
    return reply;
}

std::string zero()
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    goHomeNowLocked();
    xSemaphoreGive(g_lock);
    return "OK Z";
}

std::string setHold(uint32_t ms)
{
    if (ms == 0) return "ERR need ms>0";
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_holdMs = ms;
    xSemaphoreGive(g_lock);
    return "OK H:" + std::to_string(ms);
}

Status status()
{
    Status s;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    updateEstimatedAngle();
    s.state       = stateName(g_state);
    s.angle       = g_angle;
    s.remainingMs = remainingHoldMs();
    xSemaphoreGive(g_lock);
    return s;
}

} // namespace motor
