#ifndef SC_SPOUT_SENDER_H
#define SC_SPOUT_SENDER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Minimal C wrapper around the Spout2 spoutDX sender (C++).
 *
 * Spout2's prebuilt library exports MSVC-mangled symbols, which cannot be
 * linked with the MinGW/GCC toolchain used here, so the Spout2 core is
 * compiled from source alongside scrcpy. This C interface hides the C++ types
 * (the spoutDX object is held behind an opaque pointer).
 */
struct sc_spout_sender;

// Create a sender, opening a D3D11 device and setting the sender name.
// Returns NULL on failure.
struct sc_spout_sender *
sc_spout_sender_init(const char *name);

// Send a BGRA image (pitch in bytes). Returns false on failure.
bool
sc_spout_sender_send_image(struct sc_spout_sender *sender,
                           const uint8_t *bgra, uint32_t width,
                           uint32_t height, uint32_t pitch);

void
sc_spout_sender_destroy(struct sc_spout_sender *sender);

#ifdef __cplusplus
}
#endif

#endif
