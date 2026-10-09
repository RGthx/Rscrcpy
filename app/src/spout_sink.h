#ifndef SC_SPOUT_SINK_H
#define SC_SPOUT_SINK_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>

#include "trait/frame_sink.h"
#include "util/tick.h"

struct sc_spout_sender;
struct SwsContext;

/**
 * Spout sink: forwards decoded frames to a Spout2 sender.
 *
 * This phase handles the CPU path only: software-decoded YUV420P frames are
 * converted to BGRA with libswscale and sent via spoutDX SendImage().
 * Hardware-decoded frames (D3D11) are not handled yet.
 */
struct sc_spout_sink {
    struct sc_frame_sink frame_sink; // frame sink trait

    struct sc_spout_sender *sender; // Spout sender (NULL until open)

    struct SwsContext *sws; // YUV420P -> BGRA converter (lazily created)
    uint8_t *bgra; // conversion output buffer
    unsigned width; // current frame width
    unsigned height; // current frame height

    // state
    uint64_t frame_count;
    bool frame_shape_logged;
    bool warned; // whether a repeated error has already been logged

    // fps tracking (sampled once per second)
    sc_tick fps_last_tick;
    uint64_t fps_last_count;

    // latency tracking: wall clock elapsed vs stream pts elapsed
    bool latency_init;
    sc_tick latency_start_tick;
    int64_t latency_start_pts;
};

bool
sc_spout_sink_init(struct sc_spout_sink *ss);

void
sc_spout_sink_destroy(struct sc_spout_sink *ss);

#endif
