#include "renderer.hpp"
#include "memory.hpp"
#include "runtime.hpp"

namespace OpticCompat {
    using PresentFn = HRESULT (WINAPI *)(IDirect3DDevice9 *,
                                             const RECT *,
                                             const RECT *,
                                             HWND,
                                             const RGNDATA *);
    static PresentFn g_original_present = nullptr;

    using ChimeraEndSceneDispatchFn = void (__cdecl *)(IDirect3DDevice9 *);
    static ChimeraEndSceneDispatchFn g_chimera_end_scene_dispatch = nullptr;
    static std::atomic<bool> g_chimera_end_scene_bridge_installed{false};

    static std::byte *find_chimera_end_scene_wrapper() noexcept {

        HMODULE chimera_module = GetModuleHandleW(L"strings.dll");

        if(!chimera_module) {
            chimera_module = GetModuleHandleW(L"chimera.dll");
        }

        if(!chimera_module) {
            return nullptr;
        }
        auto *chimera_base = reinterpret_cast<std::byte *>(chimera_module);
        auto *chimera_dos = reinterpret_cast<IMAGE_DOS_HEADER *>(chimera_base);
        if(!chimera_dos || chimera_dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

        auto *chimera_nt = reinterpret_cast<IMAGE_NT_HEADERS *>(chimera_base + chimera_dos->e_lfanew);
        if(!chimera_nt || chimera_nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

        const auto chimera_begin = reinterpret_cast<std::uintptr_t>(chimera_base);
        const auto chimera_end = chimera_begin + static_cast<std::uintptr_t>(chimera_nt->OptionalHeader.SizeOfImage);

        const auto target_in_chimera = [chimera_begin, chimera_end](const unsigned char *target) noexcept {
            const auto value = reinterpret_cast<std::uintptr_t>(target);
            return value >= chimera_begin && value < chimera_end;
        };

        HMODULE halo_module = GetModuleHandleW(nullptr);
        if(!halo_module) return nullptr;

        auto *halo_base = reinterpret_cast<std::byte *>(halo_module);
        auto *halo_dos = reinterpret_cast<IMAGE_DOS_HEADER *>(halo_base);
        if(!halo_dos || halo_dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

        auto *halo_nt = reinterpret_cast<IMAGE_NT_HEADERS *>(halo_base + halo_dos->e_lfanew);
        if(!halo_nt || halo_nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

        std::byte *resolved_wrapper = nullptr;
        std::size_t matches = 0;
        auto *section = IMAGE_FIRST_SECTION(halo_nt);

        for(unsigned i = 0; i < halo_nt->FileHeader.NumberOfSections; ++i, ++section) {
            if((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;

            auto *section_start = halo_base + section->VirtualAddress;
            const std::size_t section_size = static_cast<std::size_t>(section->Misc.VirtualSize);
            if(section_size < 10) continue;

            for(std::size_t offset = 0; offset + 10 <= section_size; ++offset) {
                auto *site = reinterpret_cast<unsigned char *>(section_start + offset);

                if(site[0] != 0xE9 ||
                   site[5] != 0x90 ||
                   site[6] != 0x85 ||
                   site[7] != 0xC0 ||
                   site[8] != 0x7C ||
                   site[9] != 0x0C) {
                    continue;
                }

                std::int32_t trampoline_rel = 0;
                std::memcpy(&trampoline_rel, site + 1, sizeof(trampoline_rel));
                auto *trampoline = site + 5 + trampoline_rel;

                MEMORY_BASIC_INFORMATION trampoline_info {};
                if(VirtualQuery(trampoline, &trampoline_info, sizeof(trampoline_info)) != sizeof(trampoline_info)) {
                    continue;
                }

                const DWORD trampoline_protection = trampoline_info.Protect & 0xFFU;
                const bool trampoline_executable =
                    trampoline_protection == PAGE_EXECUTE ||
                    trampoline_protection == PAGE_EXECUTE_READ ||
                    trampoline_protection == PAGE_EXECUTE_READWRITE ||
                    trampoline_protection == PAGE_EXECUTE_WRITECOPY;

                if(trampoline_info.State != MEM_COMMIT ||
                   !trampoline_executable ||
                   (trampoline_info.Protect & PAGE_GUARD) != 0 ||
                   (trampoline_info.Protect & PAGE_NOACCESS) != 0) {
                    continue;
                }

                const auto region_begin = reinterpret_cast<std::uintptr_t>(trampoline_info.BaseAddress);
                const auto region_end = region_begin + static_cast<std::uintptr_t>(trampoline_info.RegionSize);
                const auto trampoline_address = reinterpret_cast<std::uintptr_t>(trampoline);
                if(trampoline_address > region_end || region_end - trampoline_address < 21) continue;

                if(trampoline[0] != 0xE8 ||
                   trampoline[5] != 0xFF ||
                   trampoline[6] != 0x92 ||
                   trampoline[7] != 0xA8 ||
                   trampoline[8] != 0x00 ||
                   trampoline[9] != 0x00 ||
                   trampoline[10] != 0x00 ||
                   trampoline[11] != 0xE8 ||
                   trampoline[16] != 0xE9) {
                    continue;
                }

                std::int32_t before_rel = 0;
                std::int32_t after_rel = 0;
                std::int32_t back_rel = 0;
                std::memcpy(&before_rel, trampoline + 1, sizeof(before_rel));
                std::memcpy(&after_rel, trampoline + 12, sizeof(after_rel));
                std::memcpy(&back_rel, trampoline + 17, sizeof(back_rel));

                auto *before_target = trampoline + 5 + before_rel;
                auto *after_target = trampoline + 16 + after_rel;
                auto *back_target = trampoline + 21 + back_rel;

                if(!target_in_chimera(before_target) || !target_in_chimera(after_target)) {
                    continue;
                }

                if(back_target != site + 6) {
                    continue;
                }

                resolved_wrapper = reinterpret_cast<std::byte *>(before_target);
                ++matches;
            }
        }

        if(matches == 1 && resolved_wrapper) {
            return resolved_wrapper;
        }
        return nullptr;
    }

    extern "C" void __cdecl opticcompat_chimera_end_scene_bridge(IDirect3DDevice9 *device) {

        if(g_chimera_end_scene_dispatch) {
            g_chimera_end_scene_dispatch(device);
        }
        __try {
            Runtime::instance().on_end_scene(device);
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    static bool install_chimera_end_scene_bridge() noexcept {
        if(g_chimera_end_scene_bridge_installed.load()) return true;

        auto *wrapper = find_chimera_end_scene_wrapper();
        if(!wrapper) return false;

        auto *call = reinterpret_cast<unsigned char *>(wrapper) + 6;
        if(call[0] != 0xE8) return false;

        std::int32_t old_rel = 0;
        std::memcpy(&old_rel, call + 1, sizeof(old_rel));
        auto *old_target = call + 5 + old_rel;

        if(old_target == reinterpret_cast<unsigned char *>(&opticcompat_chimera_end_scene_bridge)) {
            g_chimera_end_scene_bridge_installed.store(true);
            return true;
        }

        g_chimera_end_scene_dispatch =
            reinterpret_cast<ChimeraEndSceneDispatchFn>(old_target);

        const auto delta =
            reinterpret_cast<std::intptr_t>(&opticcompat_chimera_end_scene_bridge) -
            reinterpret_cast<std::intptr_t>(call + 5);

        const auto new_rel = static_cast<std::int32_t>(delta);
        if(static_cast<std::intptr_t>(new_rel) != delta) {
            g_chimera_end_scene_dispatch = nullptr;
            return false;
        }

        DWORD old_protect = 0;
        if(!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old_protect)) {
            g_chimera_end_scene_dispatch = nullptr;
            return false;
        }

        std::memcpy(call + 1, &new_rel, sizeof(new_rel));
        FlushInstructionCache(GetCurrentProcess(), call, 5);

        DWORD ignored = 0;
        VirtualProtect(call, 5, old_protect, &ignored);

        g_chimera_end_scene_bridge_installed.store(true);
        return true;
    }

    bool Renderer::initialize() {
        if(gdiplus_token_ != 0) return true;
        Gdiplus::GdiplusStartupInput input;
        if(Gdiplus::GdiplusStartup(&gdiplus_token_, &input, nullptr) != Gdiplus::Ok) {
            gdiplus_token_ = 0;
            return false;
        }
        return true;
    }

    bool Renderer::ensure_hook() {
        if(hooked_.load()) return true;
        if(!initialize()) return false;

        if(install_chimera_end_scene_bridge()) {
            hooked_.store(true);
            return true;
        }

        return false;
    }

    HRESULT WINAPI Renderer::present_hook(IDirect3DDevice9 *device,
                                             const RECT *source_rect,
                                             const RECT *dest_rect,
                                             HWND dest_window_override,
                                             const RGNDATA *dirty_region) {
        __try {
            if(device) {
                const HRESULT begin_result = device->BeginScene();

                if(SUCCEEDED(begin_result)) {
                    Runtime::instance().on_end_scene(device);
                    device->EndScene();
                }
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
        }

        return g_original_present
            ? g_original_present(
                device,
                source_rect,
                dest_rect,
                dest_window_override,
                dirty_region
            )
            : D3D_OK;
    }

    DWORD WINAPI Renderer::retry_thread(LPVOID self_ptr) {
        auto *self = static_cast<Renderer *>(self_ptr);
        for(int i = 0; i < 120 && self && !self->hooked(); ++i) {
            if(self->ensure_hook()) break;
            Sleep(250);
        }
        return 0;
    }

    void Renderer::start_retry_worker() {
        bool expected = false;
        if(!retry_started_.compare_exchange_strong(expected, true)) return;
        HANDLE thread = CreateThread(nullptr, 0, &Renderer::retry_thread, this, 0, nullptr);
        if(thread) CloseHandle(thread);
    }
}
