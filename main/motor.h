#pragma once

#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
//  Direct control of the Cytron MD13S + BRINGSMART worm-gear motor, running
//  on this board's own GPIOs. Replaces uno_link.cpp/.h AND uno_servo.ino --
//  there is no Arduino Uno in the BOM, so the whole state machine that used
//  to live on that second microcontroller (IDLE/SWEEP_OUT/HOLDING/
//  SWEEP_BACK, open-loop timed positioning) now runs here instead, ticked
//  by a FreeRTOS task rather than an Arduino loop().
//
//  The public shape deliberately mirrors the old uno:: namespace's ask()
//  results (a Status struct, plain "OK ..."/"ERR ..." reply strings) so
//  http_api.cpp's route handlers barely had to change -- see http_api.cpp.
// ---------------------------------------------------------------------------

namespace motor {

struct Status {
    std::string state       = "idle";   // "idle" | "sweep_out" | "holding" | "sweep_back"
    int         angle       = 0;        // estimated, not sensed -- see motor.cpp
    uint32_t    remainingMs = 0;        // only meaningful while state == "holding"
};

// Configures the PWM (LEDC) and DIR GPIOs and starts the background tick
// task. Call once, after net::start() and before api::start().
bool init();

// Starts a sweep to cfg::kAngleTarget, holds for holdMsOverride (0 = use
// cfg::kHoldMs), then returns home automatically. Returns "OK R" normally,
// or "ERR busy" if a rotate/return is already in progress (mirrors the old
// serial protocol's reply text so http_api.cpp's JSON body needs no
// changes).
std::string rotate(uint32_t holdMsOverride);

// Aborts whatever is happening and heads home now, from wherever the arm
// actually is (partial-return if called mid-sweep -- see motor.cpp).
// Always succeeds; always returns "OK Z".
std::string zero();

// Changes the default hold duration used by rotate(0) from now on.
std::string setHold(uint32_t ms);

Status status();

} // namespace motor
