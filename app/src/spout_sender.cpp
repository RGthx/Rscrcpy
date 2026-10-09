#include "spout_sender.h"

#include <new>

#include <windows.h>
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

struct sc_spout_sender *
sc_spout_sender_init(const char *name) {
    struct sc_spout_sender *sender = new (std::nothrow) sc_spout_sender();
    if (!sender) {
        return nullptr;
    }

    // Use the same adapter as the receiver (OBS) so the shared texture can
    // be opened.
    int adapter = log_and_pick_adapter();
    if (adapter >= 0 && !sender->dx.SetAdapter(adapter)) {
        LOGW("Spout: SetAdapter(%d) failed, using default adapter", adapter);
    }

    if (!sender->dx.OpenDirectX11()) {
        LOGE("Spout: OpenDirectX11 failed");
        delete sender;
        return nullptr;
    }

    if (!sender->dx.SetSenderName(name)) {
        LOGE("Spout: SetSenderName(\"%s\") failed", name);
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

void
sc_spout_sender_destroy(struct sc_spout_sender *sender) {
    if (sender) {
        sender->dx.ReleaseSender();
        delete sender;
    }
}

} // extern "C"
