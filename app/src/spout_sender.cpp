#include "spout_sender.h"

#include <new>

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <SDL3/SDL_log.h>

#include "SpoutDX.h"

// util/log.h pulls in C-only headers (options.h) which do not compile as C++,
// so use SDL logging directly here.
#define LOGI(...) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__)
#define LOGW(...) SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__)
#define LOGE(...) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, __VA_ARGS__)

extern "C" {

struct sc_spout_sender {
    spoutDX dx;

    // Borrowed device/context (either the application device passed to
    // sc_spout_sender_init(), or the one owned by dx). Never released here.
    ID3D11Device *device;
    ID3D11DeviceContext *ctx;

    // GPU conversion pipeline (NV12 -> BGRA), lazily created on the first
    // hardware frame and recreated on a size change.
    ID3D11VideoDevice *video_device; // owned (QueryInterface)
    ID3D11VideoContext *video_ctx; // owned (QueryInterface)

    ID3D11VideoProcessorEnumerator *vp_enum; // owned
    ID3D11VideoProcessor *vp; // owned

    ID3D11Texture2D *nv12_tex; // owned, input surface size
    ID3D11VideoProcessorInputView *in_view; // owned

    ID3D11Texture2D *bgra_tex; // owned, frame size
    ID3D11VideoProcessorOutputView *out_view; // owned

    uint32_t surf_w;
    uint32_t surf_h;
    uint32_t frame_w;
    uint32_t frame_h;

    bool gpu_failed; // whether a repeated GPU error has been logged
};

// Enumerate the DXGI adapters (logging them) and return the index of the
// preferred one: the dedicated GPU OBS renders on.
//
// Spout shares textures with a legacy handle (D3D11_RESOURCE_MISC_SHARED +
// GetSharedHandle()), which can only be opened by a device on the *same*
// adapter. On Optimus laptops a plain D3D11CreateDevice() may land on the
// integrated GPU while OBS renders on the discrete GPU, in which case OBS
// sees the sender but fails to open the shared texture (E_INVALIDARG).
static int
log_and_pick_adapter(void) {
    IDXGIFactory1 *factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **) &factory);
    if (FAILED(hr)) {
        LOGW("Spout: CreateDXGIFactory1 failed (0x%08lx)", (unsigned long) hr);
        return -1;
    }

    int best_index = -1;
    SIZE_T best_vram = 0;

    IDXGIAdapter1 *adapter = nullptr;
    for (UINT i = 0;
            factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            char name[256];
            int n = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                        name, sizeof(name), nullptr, nullptr);
            if (n <= 0) {
                name[0] = '\0';
            }
            bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            LOGI("Spout: DXGI adapter %u: \"%s\" vendor=0x%04x device=0x%04x "
                 "vram=%lluMB software=%d",
                 i, name, desc.VendorId, desc.DeviceId,
                 (unsigned long long) (desc.DedicatedVideoMemory
                                       / (1024 * 1024)),
                 software ? 1 : 0);
            if (!software && desc.DedicatedVideoMemory > best_vram) {
                best_vram = desc.DedicatedVideoMemory;
                best_index = (int) i;
            }
        }
        adapter->Release();
    }

    factory->Release();

    if (best_index >= 0) {
        LOGI("Spout: selected DXGI adapter %d (dedicated, %lluMB)",
             best_index, (unsigned long long) (best_vram / (1024 * 1024)));
    } else {
        LOGW("Spout: no hardware DXGI adapter found, using default");
    }
    return best_index;
}

// Best-effort: log which DXGI adapter a device belongs to.
static void
log_device_adapter(ID3D11Device *device) {
    IDXGIDevice *dxgi_device = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice),
                                      (void **) &dxgi_device))) {
        return;
    }

    IDXGIAdapter *adapter = nullptr;
    if (SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            char name[256];
            int n = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                        name, sizeof(name), nullptr, nullptr);
            if (n <= 0) {
                name[0] = '\0';
            }
            LOGI("Spout: using existing D3D11 device on adapter \"%s\"",
                 name);
        }
        adapter->Release();
    }
    dxgi_device->Release();
}

static void
release_gpu_pipeline(struct sc_spout_sender *sender) {
    if (sender->out_view) {
        sender->out_view->Release();
        sender->out_view = nullptr;
    }
    if (sender->bgra_tex) {
        sender->bgra_tex->Release();
        sender->bgra_tex = nullptr;
    }
    if (sender->in_view) {
        sender->in_view->Release();
        sender->in_view = nullptr;
    }
    if (sender->nv12_tex) {
        sender->nv12_tex->Release();
        sender->nv12_tex = nullptr;
    }
    if (sender->vp) {
        sender->vp->Release();
        sender->vp = nullptr;
    }
    if (sender->vp_enum) {
        sender->vp_enum->Release();
        sender->vp_enum = nullptr;
    }
    sender->surf_w = 0;
    sender->surf_h = 0;
    sender->frame_w = 0;
    sender->frame_h = 0;
}

// Create the NV12 -> BGRA conversion pipeline for the given sizes.
static bool
create_gpu_pipeline(struct sc_spout_sender *sender, uint32_t surf_w,
                    uint32_t surf_h, uint32_t frame_w, uint32_t frame_h) {
    release_gpu_pipeline(sender);

    if (!sender->video_device) {
        HRESULT hr = sender->device->QueryInterface(
            __uuidof(ID3D11VideoDevice), (void **) &sender->video_device);
        if (FAILED(hr) || !sender->video_device) {
            LOGE("Spout: ID3D11VideoDevice unavailable (0x%08lx)",
                 (unsigned long) hr);
            return false;
        }
        hr = sender->ctx->QueryInterface(__uuidof(ID3D11VideoContext),
                                         (void **) &sender->video_ctx);
        if (FAILED(hr) || !sender->video_ctx) {
            LOGE("Spout: ID3D11VideoContext unavailable (0x%08lx)",
                 (unsigned long) hr);
            release_gpu_pipeline(sender);
            return false;
        }
    }

    // Intermediate NV12 texture matching the decoded surface. The visible
    // region may be a sub-rectangle (crop), handled by the processor stream
    // rect, so the texture is sized to the full surface.
    D3D11_TEXTURE2D_DESC nd = {};
    nd.Width = surf_w;
    nd.Height = surf_h;
    nd.MipLevels = 1;
    nd.ArraySize = 1;
    nd.Format = DXGI_FORMAT_NV12;
    nd.SampleDesc.Count = 1;
    nd.Usage = D3D11_USAGE_DEFAULT;
    nd.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = sender->device->CreateTexture2D(&nd, nullptr,
                                                 &sender->nv12_tex);
    if (FAILED(hr)) {
        LOGE("Spout: could not create NV12 texture %ux%u (0x%08lx)",
             surf_w, surf_h, (unsigned long) hr);
        release_gpu_pipeline(sender);
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = surf_w;
    cd.InputHeight = surf_h;
    cd.OutputWidth = frame_w;
    cd.OutputHeight = frame_h;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    hr = sender->video_device->CreateVideoProcessorEnumerator(&cd, &sender->vp_enum);
    if (FAILED(hr)) {
        LOGE("Spout: CreateVideoProcessorEnumerator failed (0x%08lx)",
             (unsigned long) hr);
        release_gpu_pipeline(sender);
        return false;
    }

    hr = sender->video_device->CreateVideoProcessor(sender->vp_enum, 0,
                                                    &sender->vp);
    if (FAILED(hr)) {
        LOGE("Spout: CreateVideoProcessor failed (0x%08lx)", (unsigned long) hr);
        release_gpu_pipeline(sender);
        return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
    ivd.FourCC = 0;
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.ArraySlice = 0;
    ivd.Texture2D.MipSlice = 0;
    hr = sender->video_device->CreateVideoProcessorInputView(
        sender->nv12_tex, sender->vp_enum, &ivd, &sender->in_view);
    if (FAILED(hr)) {
        LOGE("Spout: CreateVideoProcessorInputView failed (0x%08lx)",
             (unsigned long) hr);
        release_gpu_pipeline(sender);
        return false;
    }

    D3D11_TEXTURE2D_DESC bd = {};
    bd.Width = frame_w;
    bd.Height = frame_h;
    bd.MipLevels = 1;
    bd.ArraySize = 1;
    bd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bd.SampleDesc.Count = 1;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = sender->device->CreateTexture2D(&bd, nullptr, &sender->bgra_tex);
    if (FAILED(hr)) {
        LOGE("Spout: could not create BGRA texture %ux%u (0x%08lx)",
             frame_w, frame_h, (unsigned long) hr);
        release_gpu_pipeline(sender);
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovd.Texture2D.MipSlice = 0;
    hr = sender->video_device->CreateVideoProcessorOutputView(
        sender->bgra_tex, sender->vp_enum, &ovd, &sender->out_view);
    if (FAILED(hr)) {
        LOGE("Spout: CreateVideoProcessorOutputView failed (0x%08lx)",
             (unsigned long) hr);
        release_gpu_pipeline(sender);
        return false;
    }

    sender->surf_w = surf_w;
    sender->surf_h = surf_h;
    sender->frame_w = frame_w;
    sender->frame_h = frame_h;
    LOGI("Spout: D3D11 video processor ready: surface %ux%u -> frame %ux%u",
         surf_w, surf_h, frame_w, frame_h);
    return true;
}

struct sc_spout_sender *
sc_spout_sender_init(const char *name, void *d3d11_device) {
    struct sc_spout_sender *sender = new (std::nothrow) sc_spout_sender();
    if (!sender) {
        return nullptr;
    }

    sender->device = nullptr;
    sender->ctx = nullptr;
    sender->video_device = nullptr;
    sender->video_ctx = nullptr;
    sender->vp_enum = nullptr;
    sender->vp = nullptr;
    sender->nv12_tex = nullptr;
    sender->in_view = nullptr;
    sender->bgra_tex = nullptr;
    sender->out_view = nullptr;
    sender->surf_w = 0;
    sender->surf_h = 0;
    sender->frame_w = 0;
    sender->frame_h = 0;
    sender->gpu_failed = false;

    ID3D11Device *device = (ID3D11Device *) d3d11_device;
    if (device) {
        // Reuse the application device (SDL renderer device, shared with the
        // D3D11VA decoder) so that shared textures can be opened by a receiver
        // on the same adapter.
        log_device_adapter(device);
        if (!sender->dx.OpenDirectX11(device)) {
            LOGE("Spout: OpenDirectX11(existing device) failed");
            delete sender;
            return nullptr;
        }
        sender->device = device;
    } else {
        // No device provided (CPU path): pick the adapter OBS renders on and
        // let Spout create its own device.
        int adapter = log_and_pick_adapter();
        if (adapter >= 0 && !sender->dx.SetAdapter(adapter)) {
            LOGW("Spout: SetAdapter(%d) failed, using default adapter", adapter);
        }

        if (!sender->dx.OpenDirectX11()) {
            LOGE("Spout: OpenDirectX11 failed");
            delete sender;
            return nullptr;
        }
        sender->device = sender->dx.GetDX11Device();
    }

    // The immediate context is either the one returned by GetImmediateContext()
    // for an application device, or the class device context. Both are owned by
    // the spoutDX instance (see CloseDirectX11()), so it is only borrowed here.
    sender->ctx = sender->dx.GetDX11Context();

    if (!sender->dx.SetSenderName(name)) {
        LOGE("Spout: SetSenderName(\"%s\") failed", name);
        sender->dx.CloseDirectX11();
        delete sender;
        return nullptr;
    }

    char adapter_name[256] = {0};
    int device_adapter = sender->dx.GetAdapter();
    if (sender->dx.GetAdapterName(device_adapter, adapter_name,
                                  sizeof(adapter_name))) {
        LOGI("Spout: sender \"%s\" on adapter %d [%s]",
             name, device_adapter, adapter_name);
    } else {
        LOGI("Spout: sender \"%s\" on adapter %d", name, device_adapter);
    }

    return sender;
}

bool
sc_spout_sender_send_image(struct sc_spout_sender *sender,
                           const uint8_t *bgra, uint32_t width,
                           uint32_t height, uint32_t pitch) {
    return sender->dx.SendImage(bgra, width, height, pitch);
}

bool
sc_spout_sender_send_d3d11(struct sc_spout_sender *sender,
                           void *resource, uint32_t slice,
                           uint32_t surface_width, uint32_t surface_height,
                           uint32_t width, uint32_t height,
                           uint32_t crop_left, uint32_t crop_top,
                           int ycbcr_bt709, int full_range) {
    if (!sender->device || !sender->ctx || !resource) {
        return false;
    }

    if (sender->gpu_failed) {
        // A deterministic GPU error was already reported: do not retry (and do
        // not spam the log) for every subsequent frame.
        return false;
    }

    if (!sender->nv12_tex
            || sender->surf_w != surface_width
            || sender->surf_h != surface_height
            || sender->frame_w != width
            || sender->frame_h != height) {
        if (!create_gpu_pipeline(sender, surface_width, surface_height,
                                 width, height)) {
            return false;
        }
    }

    ID3D11Resource *src = (ID3D11Resource *) resource;

    // Copies the decoded surface slice into our own NV12 texture. Both are the
    // same size, so copy the whole resource (pBox = NULL): the alignment
    // padding is discarded later by the processor source rect.
    sender->ctx->CopySubresourceRegion(sender->nv12_tex, 0, 0, 0, 0, src,
                                       slice, nullptr);

    // The decoded surface has alignment padding, so crop the stream to the
    // visible region and scale it to the frame size.
    RECT src_rect = {
        (LONG) crop_left,
        (LONG) crop_top,
        (LONG) (crop_left + width),
        (LONG) (crop_top + height),
    };
    sender->video_ctx->VideoProcessorSetStreamSourceRect(sender->vp, 0, TRUE,
                                                         &src_rect);

    RECT dst_rect = { 0, 0, (LONG) width, (LONG) height };
    sender->video_ctx->VideoProcessorSetStreamDestRect(sender->vp, 0, TRUE,
                                                       &dst_rect);

    // Input: NV12, BT.601/BT.709 matrix, studio (16-235) or full (0-255) range.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs = {};
    in_cs.Usage = 0; // playback
    in_cs.YCbCr_Matrix = ycbcr_bt709 ? 1 : 0;
    in_cs.Nominal_Range = full_range ? 1 /* 0-255 */ : 2 /* 16-235 */;
    sender->video_ctx->VideoProcessorSetStreamColorSpace(sender->vp, 0, &in_cs);

    // Output: full-range RGB (RGB_Range = 0), matching the phase 2 CPU path.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs = {};
    out_cs.Usage = 0; // playback
    out_cs.RGB_Range = 0; // 0-255 (full range)
    sender->video_ctx->VideoProcessorSetOutputColorSpace(sender->vp, &out_cs);

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.PastFrames = 0;
    stream.FutureFrames = 0;
    stream.pInputSurface = sender->in_view;

    HRESULT hr = sender->video_ctx->VideoProcessorBlt(sender->vp,
                                                      sender->out_view, 0, 1,
                                                      &stream);
    if (FAILED(hr)) {
        if (!sender->gpu_failed) {
            LOGE("Spout: VideoProcessorBlt failed (0x%08lx)",
                 (unsigned long) hr);
            sender->gpu_failed = true;
        }
        return false;
    }

    // Spout copies the BGRA texture into its shared texture (same device) and
    // signals a new frame.
    if (!sender->dx.SendTexture(sender->bgra_tex)) {
        if (!sender->gpu_failed) {
            LOGE("Spout: SendTexture failed");
            sender->gpu_failed = true;
        }
        return false;
    }

    return true;
}

void
sc_spout_sender_destroy(struct sc_spout_sender *sender) {
    if (sender) {
        release_gpu_pipeline(sender);
        if (sender->video_ctx) {
            sender->video_ctx->Release();
            sender->video_ctx = nullptr;
        }
        if (sender->video_device) {
            sender->video_device->Release();
            sender->video_device = nullptr;
        }
        sender->dx.ReleaseSender();
        sender->dx.CloseDirectX11();
        delete sender;
    }
}

} // extern "C"
