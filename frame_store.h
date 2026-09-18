#pragma once

#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ---------------------------------------------------------------------------
//  Thread-safe single-slot handoff for the most recent camera frame.
//
//  One frame is held at a time. The USB/UVC callback writes with put(),
//  overwriting whatever was there; the HTTP server reads with take(),
//  which only copies out a frame if it is newer than the sequence number
//  the caller already has. Never blocks the producer for long: put() drops
//  the frame (and counts the drop) rather than wait if the lock is busy.
//
//  Reconstructed header -- frame_store.cpp survived, this declaration did
//  not. Matches every call site in app_main.cpp, uvc_source.cpp, and
//  http_api.cpp.
// ---------------------------------------------------------------------------

class FrameStore {
public:
    // Allocates a `capacity`-byte buffer in PSRAM and the lock that guards
    // it. Returns false if either allocation failed (check PSRAM is
    // enabled in sdkconfig if this fails).
    bool init(size_t capacity);
    void deinit();

    // Copies `len` bytes in, replacing whatever frame was held before.
    // Returns false (and counts a drop) if len exceeds capacity, or if the
    // store is currently busy being read.
    bool put(const uint8_t *data, size_t len);

    // Copies the current frame into `dst` only if its sequence number does
    // not match `*seq` (the caller's "last seen" value) -- i.e. only if a
    // newer frame has arrived. On success, updates *outLen and *seq and
    // returns true. Returns false if there is nothing new yet, dst is too
    // small, or the store is busy.
    bool take(uint8_t *dst, size_t dstCapacity, size_t *outLen, uint32_t *seq);

    size_t   capacity() const { return m_capacity; }
    uint32_t sequence() const { return m_seq; }
    uint32_t dropped()  const { return m_dropped; }

private:
    SemaphoreHandle_t m_lock     = nullptr;
    uint8_t          *m_buf      = nullptr;
    size_t            m_capacity = 0;
    size_t            m_len      = 0;
    uint32_t          m_seq      = 0;
    uint32_t          m_dropped  = 0;
};
