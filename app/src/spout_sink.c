#include "spout_sink.h"

#include <inttypes.h>

#include <libavutil/pixdesc.h>

#include "util/log.h"

/** Downcast frame_sink to sc_spout_sink */
#define DOWNCAST(SINK) container_of(SINK, struct sc_spout_sink, frame_sink)

// Log the first N frames in detail, then one summary line every N-th frame
#define SPOUT_LOG_FIRST_FRAMES 3
#define SPOUT_LOG_PERIOD 300

static void
log_frame_shape(const AVFrame *frame) {
    const char *fmt_name = av_get_pix_fmt_name(frame->format);
    const char *space_name = av_color_space_name(frame->colorspace);
    const char *range_name = av_color_range_name(frame->color_range);
    const char *prim_name = av_color_primaries_name(frame->color_primaries);
    const char *trc_name = av_color_transfer_name(frame->color_trc);

    // For D3D11VA frames, data[0] is a ID3D11Resource* (texture array) and
    // data[1] is the slice index; for software frames, data[0] is the first
    // plane pointer and linesize[0] the row stride.
    // The decoded size is width/height minus the crop margins.
    LOGI("Spout sink frame shape: format=%d (%s) size=%dx%d "
         "crop(l,r,t,b)=%zu,%zu,%zu,%zu data[0]=%p data[1]=%p "
         "linesize[0]=%d hw_frames_ctx=%s "
         "colorspace=%s range=%s primaries=%s trc=%s",
         frame->format, fmt_name ? fmt_name : "?",
         frame->width, frame->height,
         frame->crop_left, frame->crop_right,
         frame->crop_top, frame->crop_bottom,
         (void *) frame->data[0],
         (void *) frame->data[1],
         frame->linesize[0],
         frame->hw_frames_ctx ? "yes" : "no",
         space_name ? space_name : "?",
         range_name ? range_name : "?",
         prim_name ? prim_name : "?",
         trc_name ? trc_name : "?");
}

static bool
sc_spout_sink_open(struct sc_frame_sink *sink, const AVCodecContext *ctx,
                   const struct sc_stream_session *session) {
    struct sc_spout_sink *ss = DOWNCAST(sink);
    (void) session;

    const char *fmt_name = av_get_pix_fmt_name(ctx->pix_fmt);
    const char *space_name = av_color_space_name(ctx->colorspace);
    const char *range_name = av_color_range_name(ctx->color_range);

    LOGI("Spout sink open: codec=%s pix_fmt=%d (%s) size=%dx%d "
         "hw_device_ctx=%s hw_frames_ctx=%s colorspace=%s range=%s",
         ctx->codec ? ctx->codec->name : "?",
         ctx->pix_fmt, fmt_name ? fmt_name : "?",
         ctx->width, ctx->height,
         ctx->hw_device_ctx ? "yes" : "no",
         ctx->hw_frames_ctx ? "yes" : "no",
         space_name ? space_name : "?",
         range_name ? range_name : "?");

    ss->frame_count = 0;
    ss->frame_shape_logged = false;

    return true;
}

static void
sc_spout_sink_close(struct sc_frame_sink *sink) {
    struct sc_spout_sink *ss = DOWNCAST(sink);
    LOGI("Spout sink closed: %"PRIu64" frames received", ss->frame_count);
}

static enum sc_sink_result
sc_spout_sink_push(struct sc_frame_sink *sink, const AVFrame *frame) {
    struct sc_spout_sink *ss = DOWNCAST(sink);

    uint64_t count = ss->frame_count++;

    if (!ss->frame_shape_logged || count < SPOUT_LOG_FIRST_FRAMES
            || count % SPOUT_LOG_PERIOD == 0) {
        if (!ss->frame_shape_logged) {
            // First frame: full shape, once per session (also covers a
            // resolution change, which produces a new session)
            log_frame_shape(frame);
            ss->frame_shape_logged = true;
        } else {
            LOGI("Spout sink frame #%"PRIu64 ": format=%d size=%dx%d "
                 "hw_frames_ctx=%s",
                 count, frame->format, frame->width, frame->height,
                 frame->hw_frames_ctx ? "yes" : "no");
        }
    }

    // Phase 1: no processing, accept everything
    return SC_SINK_OK;
}

bool
sc_spout_sink_init(struct sc_spout_sink *ss) {
    static const struct sc_frame_sink_ops ops = {
        .open = sc_spout_sink_open,
        .close = sc_spout_sink_close,
        .push = sc_spout_sink_push,
    };

    ss->frame_sink.ops = &ops;
    ss->frame_count = 0;
    ss->frame_shape_logged = false;

    return true;
}

void
sc_spout_sink_destroy(struct sc_spout_sink *ss) {
    // Nothing to clean up for now
    (void) ss;
}
