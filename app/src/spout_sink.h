#ifndef SC_SPOUT_SINK_H
#define SC_SPOUT_SINK_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>

#include "trait/frame_sink.h"

/**
 * Spout sink (phase 1: frame shape validation only).
 *
 * Minimal frame_sink skeleton: it only logs the frame properties needed to
 * validate the assumptions of the Spout integration (pixel format, data
 * pointers, hw context, colorimetry) before implementing the actual
 * GPU copy / SpoutDX sending.
 */
struct sc_spout_sink {
    struct sc_frame_sink frame_sink; // frame sink trait

    // logging state
    uint64_t frame_count;
    bool frame_shape_logged;
};

bool
sc_spout_sink_init(struct sc_spout_sink *ss);

void
sc_spout_sink_destroy(struct sc_spout_sink *ss);

#endif
