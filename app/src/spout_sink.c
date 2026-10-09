#include "spout_sink.h"

#include <inttypes.h>
#include <stdlib.h>

#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#include "spout_sender.h"
#include "util/log.h"

/** Downcast frame_sink to sc_spout_sink */
#define DOWNCAST(SINK) container_of(SINK, struct sc_spout_sink, frame_sink)

// Name under which the Spout sender is published (visible in OBS)
#define SPOUT_SENDER_NAME "scrcpy"

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
    ss->warned = false;
    ss->fps_last_tick = sc_tick_now();
    ss->fps_last_count = 0;
    ss->latency_init = false;

    ss->sender = sc_spout_sender_init(SPOUT_SENDER_NAME);
    if (!ss->sender) {
        LOGE("Could not initialize Spout sender");
        return false;
    }

    return true;
}

static void
sc_spout_sink_close(struct sc_frame_sink *sink) {
    struct sc_spout_sink *ss = DOWNCAST(sink);
    LOGI("Spout sink closed: %"PRIu64" frames received", ss->frame_count);

    if (ss->sender) {
        sc_spout_sender_destroy(ss->sender);
        ss->sender = NULL;
    }
}

// Create (or reuse) the YUV420P -> BGRA converter for the given frame size.
static bool
sc_spout_sink_prepare(struct sc_spout_sink *ss, const AVFrame *frame) {
    unsigned width = frame->width;
    unsigned height = frame->height;

    if (ss->sws && ss->width == width && ss->height == height) {
        return true;
    }

    // A new size starts a new stream session: restart latency tracking.
    ss->latency_init = false;

    uint8_t *bgra = realloc(ss->bgra, (size_t) width * height * 4);
    if (!bgra) {
        LOG_OOM();
        return false;
    }
    ss->bgra = bgra;

    ss->sws = sws_getCachedContext(ss->sws, width, height, AV_PIX_FMT_YUV420P,
                                   width, height, AV_PIX_FMT_BGRA, SWS_BILINEAR,
                                   NULL, NULL, NULL);
    if (!ss->sws) {
        LOGE("Could not create swscale context");
        return false;
    }

    int space;
    switch (frame->colorspace) {
        case AVCOL_SPC_BT709:
            space = SWS_CS_ITU709;
            break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            space = SWS_CS_BT2020;
            break;
        default:
            // BT470BG / SMPTE170M (and unspecified) map to BT.601
            space = SWS_CS_ITU601;
            break;
    }

    // YUV source may be limited range; the BGRA destination is full range RGB.
    const int *table = sws_getCoefficients(space);
    int src_range = frame->color_range == AVCOL_RANGE_JPEG; // 1 = full
    sws_setColorspaceDetails(ss->sws, table, src_range, table, 1,
                             0, 1 << 16, 1 << 16);

    ss->width = width;
    ss->height = height;

    return true;
}

static enum sc_sink_result
sc_spout_sink_push(struct sc_frame_sink *sink, const AVFrame *frame) {
    struct sc_spout_sink *ss = DOWNCAST(sink);

    uint64_t count = ss->frame_count++;

    sc_tick now = sc_tick_now();

    // End-to-end latency probe: compare the wall-clock elapsed time with the
    // stream pts elapsed time since the first frame. The difference is the
    // accumulated pipeline latency: if it grows, the sink consumes frames more
    // slowly than the device produces them (no backpressure / no frame drop).
    int64_t pts = frame->pts; // microseconds
    bool has_pts = pts != AV_NOPTS_VALUE;
    if (has_pts && !ss->latency_init) {
        ss->latency_init = true;
        ss->latency_start_tick = now;
        ss->latency_start_pts = pts;
    }

    // Log the instantaneous frame rate once per second to observe fluctuations.
    sc_tick elapsed = now - ss->fps_last_tick;
    if (elapsed >= SC_TICK_FROM_SEC(1)) {
        uint64_t frames = ss->frame_count - ss->fps_last_count;
        double fps = (double) frames * (double) SC_TICK_FREQ / (double) elapsed;
        if (has_pts) {
            double wall_ms = (double) (now - ss->latency_start_tick) / 1000.0;
            double stream_ms = (double) (pts - ss->latency_start_pts) / 1000.0;
            LOGI("Spout sink fps: %.1f (frames=%"PRIu64", total=%"PRIu64"), "
                 "latency=%.0f ms (wall=%.0f stream=%.0f)",
                 fps, frames, ss->frame_count, wall_ms - stream_ms,
                 wall_ms, stream_ms);
        } else {
            LOGI("Spout sink fps: %.1f (frames=%"PRIu64", total=%"PRIu64")",
                 fps, frames, ss->frame_count);
        }
        ss->fps_last_tick = now;
        ss->fps_last_count = ss->frame_count;
    }

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

    if (!ss->sender) {
        // Not opened
        return SC_SINK_OK;
    }

    if (frame->format != AV_PIX_FMT_YUV420P) {
        // Only the CPU path (software decoding) is handled for now
        if (!ss->warned) {
            const char *fmt_name = av_get_pix_fmt_name(frame->format);
            LOGW("Spout: unsupported frame format %d (%s), skipping",
                 frame->format, fmt_name ? fmt_name : "?");
            ss->warned = true;
        }
        return SC_SINK_OK;
    }

    if (!sc_spout_sink_prepare(ss, frame)) {
        return SC_SINK_OK;
    }

    const uint8_t *src[4] = {
        frame->data[0], frame->data[1], frame->data[2], NULL,
    };
    int src_stride[4] = {
        frame->linesize[0], frame->linesize[1], frame->linesize[2], 0,
    };
    uint8_t *dst[4] = { ss->bgra, NULL, NULL, NULL };
    int dst_stride[4] = { (int) (ss->width * 4), 0, 0, 0 };

    // sws_scale() accounts for the source row strides (linesize), which may be
    // larger than the frame width (padding).
    int h = sws_scale(ss->sws, src, src_stride, 0, frame->height,
                      dst, dst_stride);
    if (h <= 0) {
        LOGE("Spout: sws_scale() failed");
        return SC_SINK_OK;
    }

    uint32_t w = ss->width;
    uint32_t pitch = w * 4;
    if (!sc_spout_sender_send_image(ss->sender, ss->bgra, w, ss->height,
                                    pitch)) {
        if (!ss->warned) {
            LOGE("Spout: SendImage() failed");
            ss->warned = true;
        }
    }

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
    ss->sender = NULL;
    ss->sws = NULL;
    ss->bgra = NULL;
    ss->width = 0;
    ss->height = 0;
    ss->frame_count = 0;
    ss->frame_shape_logged = false;
    ss->warned = false;
    ss->fps_last_tick = 0;
    ss->fps_last_count = 0;
    ss->latency_init = false;

    return true;
}

void
sc_spout_sink_destroy(struct sc_spout_sink *ss) {
    if (ss->sender) {
        sc_spout_sender_destroy(ss->sender);
        ss->sender = NULL;
    }
    if (ss->sws) {
        sws_freeContext(ss->sws);
        ss->sws = NULL;
    }
    free(ss->bgra);
    ss->bgra = NULL;
}
