#pragma once
#include "common.hpp"

namespace OpticCompat {
    class Renderer {
    public:
        bool initialize();
        bool ensure_hook();
        bool hooked() const noexcept { return hooked_.load(); }
        void start_retry_worker();

    private:
        static HRESULT WINAPI present_hook(IDirect3DDevice9 *device,
                                            const RECT *source_rect,
                                            const RECT *dest_rect,
                                            HWND dest_window_override,
                                            const RGNDATA *dirty_region);
        static DWORD WINAPI retry_thread(LPVOID self);
        std::atomic<bool> hooked_{false};
        std::atomic<bool> retry_started_{false};
        IDirect3DDevice9 **device_storage_ = nullptr;
        ULONG_PTR gdiplus_token_ = 0;
    };
}
