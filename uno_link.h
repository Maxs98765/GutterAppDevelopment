#pragma once

#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
//  UART master for the link to the Arduino Uno servo controller. See
//  uno_servo.ino for the exact wire protocol this speaks (single-letter
//  commands, one-line replies).
//
//  Reconstructed header -- uno_link.cpp survived, this declaration did not.
// ---------------------------------------------------------------------------

namespace uno {

struct Status {
    bool        valid       = false;   // false if the Uno didn't answer, or
                                        // answered with something unparsable
    std::string state       = "unknown";
    int         angle       = 0;
    uint32_t    remainingMs = 0;
    std::string raw;                   // the raw reply, kept for diagnostics
};

// Brings up the UART port at cfg::kUnoBaud, on cfg::kUnoTxPin/kUnoRxPin.
bool init();

// Sends `cmd` (a single letter, optionally ":<value>", e.g. "R:4000") and
// waits up to cfg::kLinkReplyTimeoutMs for a one-line reply. Returns the
// reply with the trailing newline stripped, or an empty string on timeout
// (which the caller should treat as "the Uno did not answer").
std::string ask(const std::string &cmd);

// Convenience wrapper around ask("S") that parses the "ST <state> <angle>
// <remaining_ms>" reply into a Status. Status::valid is false if the Uno
// didn't reply at all, or replied with something that didn't parse.
Status status();

} // namespace uno
