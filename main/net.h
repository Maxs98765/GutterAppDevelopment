#pragma once

#include <string>

// ---------------------------------------------------------------------------
//  Brings the hub onto the network. On the ESP32-P4 this means bringing up
//  the ESP32-C6 co-processor over SDIO first -- see net.cpp and
//  PROBE-NOTES.md for why that ordering is not optional -- then joining
//  WiFi, or falling back to the hub's own access point if that fails or
//  no credentials were given.
//
//  Reconstructed header -- net.cpp survived, this declaration did not.
// ---------------------------------------------------------------------------

namespace net {

// Full bring-up sequence: co-processor, NVS, netif, event loop, WiFi.
// Returns false only if the co-processor link itself could not be
// established. A failed WiFi join still returns true, because start()
// falls back to access-point mode automatically in that case.
bool start();

// Optional: advertises the hub at http://<cfg::kMdnsName>.local/. Safe to
// call even if start() fell back to access-point mode.
void startMdns();

bool        apMode();       // true if running as its own access point
std::string ipAddress();    // "0.0.0.0" until connected
std::string slaveVersion(); // the C6's reported firmware version, or
                             // "unknown" if it could not be read

} // namespace net
