#include "spout_sink.h"

#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>

#include <libavutil/hwcontext.h>
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

// Create (or reuse) the YUV420P -> BGRA converter for the given frame size.
// Only called from the worker thread.
static bool
sc_spout_sink_prepare(struct sc_spout_sink *ss, const AVFrame *frame) {
    unsigned width = frame->width;
    unsigned height = frame->height;

    if (ss->sws && ss->width == width && ss->height == height) {
        return true;
    }

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

// Perform the actual conversion + send. Only called from the worker thread.
static void
spout_sink_send_frame(struct sc_spout_sink *ss, const AVFrame *frame) {
    if (!ss->sender) {
        return;
    }

    if (frame->format == AV_PIX_FMT_D3D11) {
        // GPU path: hardware-decoded NV12 frames are converted to BGRA by the
        // D3D11 video processor and sent without a CPU round-trip.
        if (!frame->hw_frames_ctx) {
            if (!ss->warned) {
                LOGE("Spout: D3D11 frame without hw_frames_ctx, skipping");
                ss->warned = true;
            }
            return;
        }

        const AVHWFramesContext *frames =
            (const AVHWFramesContext *) frame->hw_frames_ctx->data;

        void *resource = frame->data[0];
        uint32_t slice = (uint32_t) (uintptr_t) frame->data[1];
        uint32_t surf_w = frames->width;
        uint32_t surf_h = frames->height;
        uint32_t crop_left = (uint32_t) frame->crop_left;
        uint32_t crop_top = (uint32_t) frame->crop_top;
        uint32_t w = frame->width;
        uint32_t h = frame->height;

        if (crop_left + w > surf_w || crop_top + h > surf_h) {
            if (!ss->warned) {
                LOGE("Spout: inconsistent D3D11 frame geometry "
                     "(surface %ux%u, crop %u,%u size %ux%u)",
                     surf_w, surf_h, crop_left, crop_top, w, h);
                ss->warned = true;
            }
            return;
        }

        int ycbcr_bt709 = frame->colorspace == AVCOL_SPC_BT709;
        int full_range = frame->color_range == AVCOL_RANGE_JPEG;

        if (!sc_spout_sender_send_d3d11(ss->sender, resource, slice,
                                        surf_w, surf_h, w, h,
                                        crop_left, crop_top,
                                        ycbcr_bt709, full_range)) {
            if (!ss->warned) {
                LOGE("Spout: SendTexture() failed");
                ss->warned = true;
            }
        }

        return;
    }

    if (frame->format != AV_PIX_FMT_YUV420P) {
        // Only software decoding (YUV420P) is handled on the CPU path
        if (!ss->warned) {
            const char *fmt_name = av_get_pix_fmt_name(frame->format);
            LOGW("Spout: unsupported frame format %d (%s), skipping",
                 frame->format, fmt_name ? fmt_name : "?");
            ss->warned = true;
        }
        return;
    }

    if (!sc_spout_sink_prepare(ss, frame)) {
        return;
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
        return;
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
}

// Worker thread: consume the latest frame and send it to Spout.
//
// This keeps the (heavy) GPU work off the decoder thread, so decoding is never
// throttled by the sink and the latency does not accumulate.
static int
run_spout(void *data) {
    struct sc_spout_sink *ss = data;

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        LOG_OOM();
        return -1;
    }

    for (;;) {
        sc_tick wait_start = sc_tick_now();

        sc_mutex_lock(&ss->mutex);
        while (!ss->stopped && !sc_frame_buffer_has_frame(&ss->fb)) {
            sc_cond_wait(&ss->cond, &ss->mutex);
        }

        if (ss->stopped) {
            sc_mutex_unlock(&ss->mutex);
            break;
        }

        assert(sc_frame_buffer_has_frame(&ss->fb));
        sc_frame_buffer_consume(&ss->fb, frame);
        sc_mutex_unlock(&ss->mutex);

        sc_tick send_start = sc_tick_now();
        ss->stat_wait_us += send_start - wait_start;

        spout_sink_send_frame(ss, frame);
        av_frame_unref(frame);

        sc_tick send_end = sc_tick_now();
        sc_tick send_us = send_end - send_start;
        ss->stat_send_us += send_us;
        if (send_us > ss->stat_send_max_us) {
            ss->stat_send_max_us = send_us;
        }

        ++ss->sent_count;

        // Log the send rate plus the drop count once per second: a positive
        // drop count means the worker cannot keep up with the device.
        sc_tick now = sc_tick_now();
        sc_tick elapsed = now - ss->send_fps_last_tick;
        if (elapsed >= SC_TICK_FROM_SEC(1)) {
            uint64_t frames = ss->sent_count - ss->send_fps_last_count;
            double fps = (double) frames * (double) SC_TICK_FREQ
                       / (double) elapsed;
            double avg_send_ms = frames
                               ? (double) ss->stat_send_us / frames / 1000.0
                               : 0.0;
            double avg_wait_ms = frames
                               ? (double) ss->stat_wait_us / frames / 1000.0
                               : 0.0;
            LOGI("Spout sink sent: %.1f fps (sent=%"PRIu64" received=%"PRIu64
                 " dropped=%"PRIu64") send=%.1fms max=%.1fms wait=%.1fms",
                 fps, ss->sent_count, ss->frame_count, ss->dropped,
                 avg_send_ms, (double) ss->stat_send_max_us / 1000.0,
                 avg_wait_ms);
            ss->send_fps_last_tick = now;
            ss->send_fps_last_count = ss->sent_count;
            ss->stat_send_us = 0;
            ss->stat_send_max_us = 0;
            ss->stat_wait_us = 0;
        }
    }

    av_frame_free(&frame);
    LOGD("Spout thread ended");

    return 0;
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
    ss->sent_count = 0;
    ss->dropped = 0;
    ss->frame_shape_logged = false;
    ss->warned = false;
    ss->fps_last_tick = sc_tick_now();
    ss->fps_last_count = 0;
    ss->send_fps_last_tick = sc_tick_now();
    ss->send_fps_last_count = 0;
    ss->latency_init = false;

    ss->sender = sc_spout_sender_init(SPOUT_SENDER_NAME, ss->d3d11_device);
    if (!ss->sender) {
        LOGE("Could not initialize Spout sender");
        return false;
    }

    sc_mutex_lock(&ss->mutex);
    ss->stopped = false;
    sc_mutex_unlock(&ss->mutex);

    if (!sc_thread_create(&ss->thread, run_spout, "scrcpy-spout", ss)) {
        LOGE("Could not start Spout thread");
        sc_spout_sender_destroy(ss->sender);
        ss->sender = NULL;
        return false;
    }

    return true;
}

static void
sc_spout_sink_close(struct sc_frame_sink *sink) {
    struct sc_spout_sink *ss = DOWNCAST(sink);

    sc_mutex_lock(&ss->mutex);
    ss->stopped = true;
    sc_cond_signal(&ss->cond);
    sc_mutex_unlock(&ss->mutex);

    sc_thread_join(&ss->thread, NULL);

    LOGI("Spout sink closed: sent=%"PRIu64" received=%"PRIu64" dropped=%"PRIu64,
         ss->sent_count, ss->frame_count, ss->dropped);

    if (ss->sender) {
        sc_spout_sender_destroy(ss->sender);
        ss->sender = NULL;
    }
}

static enum sc_sink_result
sc_spout_sink_push(struct sc_frame_sink *sink, const AVFrame *frame) {
    struct sc_spout_sink *ss = DOWNCAST(sink);

    uint64_t count = ss->frame_count++;

    sc_tick now = sc_tick_now();

    // End-to-end latency probe: compare the wall-clock elapsed time with the
    // stream pts elapsed time since the first frame. The difference is the
    // accumulated pipeline latency: if it grows, the frames pushed by the
    // decoder are increasingly late (the decoder cannot keep up).
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

    // Lightweight: store the latest frame (dropping any unconsumed previous
    // one) and wake the worker thread. Never blocks the decoder thread.
    sc_mutex_lock(&ss->mutex);

    if (ss->stopped) {
        sc_mutex_unlock(&ss->mutex);
        return SC_SINK_STOPPED;
    }

    if (sc_frame_buffer_has_frame(&ss->fb)) {
        // A previous frame has not been consumed yet: it will be dropped
        ++ss->dropped;
    }

    bool ok = sc_frame_buffer_push(&ss->fb, frame);
    if (ok) {
        sc_cond_signal(&ss->cond);
    }

    sc_mutex_unlock(&ss->mutex);

    return ok ? SC_SINK_OK : SC_SINK_KO;
}

bool
sc_spout_sink_init(struct sc_spout_sink *ss, void *d3d11_device) {
    static const struct sc_frame_sink_ops ops = {
        .open = sc_spout_sink_open,
        .close = sc_spout_sink_close,
        .push = sc_spout_sink_push,
    };

    ss->frame_sink.ops = &ops;
    ss->sender = NULL;
    ss->d3d11_device = d3d11_device;
    ss->sws = NULL;
    ss->bgra = NULL;
    ss->width = 0;
    ss->height = 0;
    ss->frame_count = 0;
    ss->sent_count = 0;
    ss->frame_shape_logged = false;
    ss->warned = false;
    ss->fps_last_tick = 0;
    ss->fps_last_count = 0;
    ss->send_fps_last_tick = 0;
    ss->send_fps_last_count = 0;
    ss->latency_init = false;
    ss->dropped = 0;
    ss->stopped = true;

    if (!sc_mutex_init(&ss->mutex)) {
        return false;
    }

    if (!sc_cond_init(&ss->cond)) {
        sc_mutex_destroy(&ss->mutex);
        return false;
    }

    if (!sc_frame_buffer_init(&ss->fb)) {
        sc_cond_destroy(&ss->cond);
        sc_mutex_destroy(&ss->mutex);
        return false;
    }

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

    sc_frame_buffer_destroy(&ss->fb);
    sc_cond_destroy(&ss->cond);
    sc_mutex_destroy(&ss->mutex);
}
