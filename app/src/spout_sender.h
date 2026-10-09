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

// Create a sender.
//
// d3d11_device is an optional ID3D11Device* (as void*). When non-NULL, the
// sender reuses that device (typically SDL's renderer device, shared with the
// D3D11VA decoder) so that shared textures can be opened by a receiver on the
// same adapter, and SendTexture() can run without a cross-device copy. When
// NULL, the sender creates and owns its own device (CPU path).
//
// Returns NULL on failure.
struct sc_spout_sender *
sc_spout_sender_init(const char *name, void *d3d11_device);

// Send a BGRA image (pitch in bytes). Returns false on failure.
bool
sc_spout_sender_send_image(struct sc_spout_sender *sender,
                           const uint8_t *bgra, uint32_t width,
                           uint32_t height, uint32_t pitch);

// Send a hardware-decoded D3D11 frame (zero-copy GPU path).
//
// resource is the decoded ID3D11Resource* (AVFrame.data[0]) and slice its
// subresource index (AVFrame.data[1]). The decoded surface has alignment
// padding, so its dimensions (surface_width/surface_height) may be larger than
// the visible frame (width/height); crop_left/crop_top locate the visible
// region inside the surface.
//
// ycbcr_bt709 selects the input YCbCr matrix (0 = BT.601, 1 = BT.709, the only
// two matrices representable by the D3D11 video processor). full_range selects
// the input nominal range (0 = studio/limited 16-235, 1 = full 0-255).
//
// Converts NV12 -> BGRA on the GPU (ID3D11VideoProcessor) and sends the
// result. Returns false on failure.
bool
sc_spout_sender_send_d3d11(struct sc_spout_sender *sender,
                           void *resource, uint32_t slice,
                           uint32_t surface_width, uint32_t surface_height,
                           uint32_t width, uint32_t height,
                           uint32_t crop_left, uint32_t crop_top,
                           int ycbcr_bt709, int full_range);

void
sc_spout_sender_destroy(struct sc_spout_sender *sender);

#ifdef __cplusplus
}
#endif

#endif
