#ifndef SC_SPOUT_SINK_H
#define SC_SPOUT_SINK_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>
#include <libavcodec/avcodec.h>

#include "frame_buffer.h"
#include "trait/frame_sink.h"
#include "util/thread.h"
#include "util/tick.h"

struct sc_spout_sender;
struct SwsContext;

/**
 * Spout sink: forwards decoded frames to a Spout2 sender.
 *
 * Two paths are supported:
 *  - software decoding: YUV420P frames are converted to BGRA with libswscale
 *    and sent via spoutDX SendImage() (CPU);
 *  - hardware decoding: D3D11 frames are converted to BGRA on the GPU with an
 *    ID3D11VideoProcessor and sent via spoutDX SendTexture() (zero-copy).
 *
 * The conversion and the send are expensive (GPU copies + VideoProcessorBlt +
 * SendTexture, which flushes the shared immediate context). They must not run
 * in the decoder thread: doing so throttles decoding and, since the decoder
 * queue is unbounded, makes the latency grow without bound.
 *
 * Therefore push() only stores the latest frame (a single-slot buffer: any
 * unconsumed previous frame is dropped, keeping latency constant) and wakes a
 * dedicated worker thread which performs the actual work.
 */
struct sc_spout_sink {
    struct sc_frame_sink frame_sink; // frame sink trait

    struct sc_spout_sender *sender; // Spout sender (NULL until open)

    // ID3D11Device used by the renderer (and the D3D11VA decoder), reused by
    // the Spout sender for the GPU path. NULL if unavailable.
    void *d3d11_device;

    // Worker thread and its single-slot "latest frame" queue
    sc_mutex mutex;
    sc_cond cond;
    sc_thread thread;
    struct sc_frame_buffer fb;
    bool stopped;
    uint64_t dropped; // frames overwritten before being sent

    // Worker-thread-only conversion state (only accessed by run_spout())
    struct SwsContext *sws; // YUV420P -> BGRA converter (lazily created)
    uint8_t *bgra; // conversion output buffer
    unsigned width; // current frame width
    unsigned height; // current frame height

    // state
    uint64_t frame_count; // frames received in push()
    uint64_t sent_count; // frames actually sent by the worker
    bool frame_shape_logged;
    bool warned; // whether a repeated error has already been logged

    // fps tracking (sampled once per second)
    sc_tick fps_last_tick;
    uint64_t fps_last_count;

    // send fps tracking (worker thread, sampled once per second)
    sc_tick send_fps_last_tick;
    uint64_t send_fps_last_count;

    // worker timing stats, reset every second (worker thread only)
    sc_tick stat_send_us; // cumulative time spent in spout_sink_send_frame()
    sc_tick stat_send_max_us; // max single send time
    sc_tick stat_wait_us; // cumulative queue wait time

    // latency tracking: wall clock elapsed vs stream pts elapsed
    bool latency_init;
    sc_tick latency_start_tick;
    int64_t latency_start_pts;
};

// d3d11_device is an optional ID3D11Device* (as void*) reused by the Spout
// sender for the zero-copy GPU path (may be NULL).
bool
sc_spout_sink_init(struct sc_spout_sink *ss, void *d3d11_device);

void
sc_spout_sink_destroy(struct sc_spout_sink *ss);

#endif
