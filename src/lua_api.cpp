#include "lua_api.hpp"
#include "runtime.hpp"
#include <cwctype>
#include <cmath>
#include <array>
#include <atomic>
#include <limits>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace OpticCompat::LuaApi {

    struct NativeDamageEvent {
        std::uint32_t effect_tag_id = 0xFFFFFFFFu;
        std::uint32_t responsible_player_id = 0xFFFFFFFFu;
        std::uint32_t responsible_unit_id = 0xFFFFFFFFu;
        std::uint32_t target_object_id = 0xFFFFFFFFu;
        std::int32_t node_index = -1;
        std::int32_t region_index = -1;
        std::int32_t material_index = -1;
        float multiplier = 1.0f;
        std::uint32_t sequence = 0;
    };

    static constexpr std::uint32_t NATIVE_DAMAGE_RING_CAPACITY = 128;
    static std::array<NativeDamageEvent, NATIVE_DAMAGE_RING_CAPACITY>
        native_damage_ring{};
    static std::atomic<std::uint32_t> native_damage_write{0};
    static std::atomic<std::uint32_t> native_damage_read{0};
    static std::atomic<std::uint32_t> native_damage_sequence{0};
    static std::atomic<bool> native_damage_probe_installed{false};
    static std::atomic<bool> native_damage_probe_attempted{false};
    static std::uintptr_t native_damage_probe_continue = 0;

    static std::uint32_t native_read_u32(
        const std::uint8_t *data,
        std::size_t offset,
        std::uint32_t fallback = 0xFFFFFFFFu
    ) noexcept {
        if(!data) return fallback;
        std::uint32_t value = fallback;
        std::memcpy(&value, data + offset, sizeof(value));
        return value;
    }

    static float native_read_float(
        const std::uint8_t *data,
        std::size_t offset,
        float fallback = 1.0f
    ) noexcept {
        if(!data) return fallback;
        float value = fallback;
        std::memcpy(&value, data + offset, sizeof(value));
        if(!std::isfinite(value)) return fallback;
        return value;
    }

    extern "C" __declspec(noinline) void __cdecl
    opticcompat_record_native_damage(
        const void *damage_data,
        std::uint32_t target_object_id,
        std::int32_t node_index,
        std::int32_t region_index,
        std::int32_t material_index
    ) noexcept {
        const auto *bytes =
            static_cast<const std::uint8_t *>(damage_data);

        NativeDamageEvent event{};
        event.effect_tag_id = native_read_u32(bytes, 0x00);
        event.responsible_player_id = native_read_u32(bytes, 0x08);
        event.responsible_unit_id = native_read_u32(bytes, 0x0C);
        event.target_object_id = target_object_id;
        event.node_index = node_index;
        event.region_index = region_index;
        event.material_index = material_index;

        event.multiplier = native_read_float(bytes, 0x44, 1.0f);
        event.sequence = native_damage_sequence.fetch_add(
            1,
            std::memory_order_relaxed
        ) + 1;

        const std::uint32_t write =
            native_damage_write.load(std::memory_order_relaxed);
        const std::uint32_t next =
            (write + 1u) % NATIVE_DAMAGE_RING_CAPACITY;
        std::uint32_t read =
            native_damage_read.load(std::memory_order_acquire);

        if(next == read) {
            read = (read + 1u) % NATIVE_DAMAGE_RING_CAPACITY;
            native_damage_read.store(read, std::memory_order_release);
        }

        native_damage_ring[write] = event;
        native_damage_write.store(next, std::memory_order_release);
    }

#if defined(_MSC_VER) && defined(_M_IX86)
    __declspec(naked) static void native_damage_probe_stub() {
        __asm {

            pushfd
            pushad

            mov eax, dword ptr [esp + 0BCh]
            mov ecx, dword ptr [esp + 0C0h]
            mov edx, dword ptr [esp + 0C4h]
            mov ebx, dword ptr [esp + 0C8h]
            mov esi, dword ptr [esp + 0CCh]

            push esi
            push ebx
            push edx
            push ecx
            push eax
            call opticcompat_record_native_damage
            add esp, 14h

            popad
            popfd

            mov eax, dword ptr [esp + 09Ch]
            jmp dword ptr [native_damage_probe_continue]
        }
    }
#endif

    static std::uint8_t *find_native_damage_probe_site() noexcept {
#if defined(_WIN32)
        const HMODULE module = GetModuleHandleW(nullptr);
        if(!module) return nullptr;

        auto *base = reinterpret_cast<std::uint8_t *>(module);
        const auto *dos =
            reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        if(dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(
            base + dos->e_lfanew
        );
        if(nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

        static constexpr std::uint8_t pattern[] = {
            0x8B, 0x84, 0x24, 0x9C, 0x00, 0x00, 0x00,
            0x25, 0xFF, 0xFF, 0x00, 0x00
        };

        const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
        for(unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if((section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
                continue;
            }

            const std::size_t virtual_size =
                std::max<std::size_t>(
                    section[i].Misc.VirtualSize,
                    section[i].SizeOfRawData
                );
            if(virtual_size < sizeof(pattern)) continue;

            auto *start = base + section[i].VirtualAddress;
            for(std::size_t offset = 0;
                offset + sizeof(pattern) <= virtual_size;
                ++offset) {
                if(std::memcmp(start + offset, pattern, sizeof(pattern)) == 0) {
                    return start + offset;
                }
            }
        }
#endif
        return nullptr;
    }

    static bool install_native_damage_probe() noexcept {
        if(native_damage_probe_installed.load(std::memory_order_acquire)) {
            return true;
        }

        bool expected = false;
        if(!native_damage_probe_attempted.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel)) {
            return native_damage_probe_installed.load(
                std::memory_order_acquire
            );
        }

#if defined(_MSC_VER) && defined(_M_IX86) && defined(_WIN32)
        std::uint8_t *site = find_native_damage_probe_site();
        if(!site) {
return false;
        }

        const auto stub = reinterpret_cast<std::uintptr_t>(
            &native_damage_probe_stub
        );
        const auto after_jmp =
            reinterpret_cast<std::uintptr_t>(site + 5);
        const std::intptr_t displacement =
            static_cast<std::intptr_t>(stub) -
            static_cast<std::intptr_t>(after_jmp);

        if(displacement < std::numeric_limits<std::int32_t>::min() ||
           displacement > std::numeric_limits<std::int32_t>::max()) {
            return false;
        }

        native_damage_probe_continue =
            reinterpret_cast<std::uintptr_t>(site + 7);

        DWORD old_protect = 0;
        if(!VirtualProtect(
                site,
                7,
                PAGE_EXECUTE_READWRITE,
                &old_protect)) {
            native_damage_probe_continue = 0;
            return false;
        }

        site[0] = 0xE9;
        const std::int32_t rel32 =
            static_cast<std::int32_t>(displacement);
        std::memcpy(site + 1, &rel32, sizeof(rel32));
        site[5] = 0x90;
        site[6] = 0x90;

        DWORD ignored = 0;
        VirtualProtect(site, 7, old_protect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), site, 7);

        native_damage_probe_installed.store(
            true,
            std::memory_order_release
        );
        return true;
#else
return false;
#endif
    }

    static int native_damage_probe_available(lua_State *L) {
        if(lua_gettop(L) != 0) {
            return luaL_error(
                L,
                "invalid number of arguments in native_damage_probe_available"
            );
        }
        lua_pushboolean(L, install_native_damage_probe() ? 1 : 0);
        return 1;
    }

    static int clear_native_damage_events(lua_State *L) {
        if(lua_gettop(L) != 0) {
            return luaL_error(
                L,
                "invalid number of arguments in clear_native_damage_events"
            );
        }
        const auto write =
            native_damage_write.load(std::memory_order_acquire);
        native_damage_read.store(write, std::memory_order_release);
        return 0;
    }

    static int poll_native_damage_event(lua_State *L) {
        if(lua_gettop(L) != 0) {
            return luaL_error(
                L,
                "invalid number of arguments in poll_native_damage_event"
            );
        }

        if(!install_native_damage_probe()) {
            lua_pushnil(L);
            return 1;
        }

        const std::uint32_t read =
            native_damage_read.load(std::memory_order_relaxed);
        const std::uint32_t write =
            native_damage_write.load(std::memory_order_acquire);

        if(read == write) {
            lua_pushnil(L);
            return 1;
        }

        const NativeDamageEvent event = native_damage_ring[read];
        native_damage_read.store(
            (read + 1u) % NATIVE_DAMAGE_RING_CAPACITY,
            std::memory_order_release
        );

        lua_newtable(L);

        lua_pushnumber(L, static_cast<lua_Number>(event.effect_tag_id));
        lua_setfield(L, -2, "effectTagId");
        lua_pushnumber(L, static_cast<lua_Number>(event.responsible_player_id));
        lua_setfield(L, -2, "responsiblePlayerId");
        lua_pushnumber(L, static_cast<lua_Number>(event.responsible_unit_id));
        lua_setfield(L, -2, "responsibleUnitId");
        lua_pushnumber(L, static_cast<lua_Number>(event.target_object_id));
        lua_setfield(L, -2, "targetObjectId");
        lua_pushinteger(L, static_cast<lua_Integer>(event.node_index));
        lua_setfield(L, -2, "nodeIndex");
        lua_pushinteger(L, static_cast<lua_Integer>(event.region_index));
        lua_setfield(L, -2, "regionIndex");
        lua_pushinteger(L, static_cast<lua_Integer>(event.material_index));
        lua_setfield(L, -2, "materialIndex");
        lua_pushnumber(L, static_cast<lua_Number>(event.multiplier));
        lua_setfield(L, -2, "multiplier");
        lua_pushnumber(L, static_cast<lua_Number>(event.sequence));
        lua_setfield(L, -2, "sequence");

        return 1;
    }

    static std::size_t check_handle(lua_State *L, int index, const char *kind) {
        const lua_Integer value = luaL_checkinteger(L, index);
        if(value < 0) luaL_error(L, "invalid %s handle", kind);
        return static_cast<std::size_t>(value);
    }

    static int create_animation(lua_State *L) {
        if(lua_gettop(L) != 1) return luaL_error(L, "invalid number of arguments in create_animation");
        const long duration = static_cast<long>(luaL_checknumber(L, 1));
        const auto handle = Runtime::instance().store().create_animation(duration);
        lua_pushinteger(L, static_cast<lua_Integer>(handle));
        return 1;
    }

    static int set_animation_property(lua_State *L) {
        const int args = lua_gettop(L);
        if(args != 4 && args != 7) return luaL_error(L, "invalid number of arguments in set_animation_property");
        auto &store = Runtime::instance().store();
        auto *animation = store.animation(check_handle(L, 1, "animation"));
        if(!animation) return luaL_error(L, "invalid animation handle");

        Animation::Curve curve{};
        const char *property_name = nullptr;
        float value = 0.0f;
        if(args == 4) {
            bool valid = false;
            curve = Animation::curve_from_preset(luaL_checkstring(L, 2), valid);
            if(!valid) return luaL_error(L, "invalid curve preset in set_animation_property");
            property_name = luaL_checkstring(L, 3);
            value = static_cast<float>(luaL_checknumber(L, 4));
        }
        else {
            (void)luaL_checknumber(L, 2);
            const float y1 = static_cast<float>(luaL_checknumber(L, 3));
            (void)luaL_checknumber(L, 4);
            const float y2 = static_cast<float>(luaL_checknumber(L, 5));
            curve = {y1, y2};
            property_name = luaL_checkstring(L, 6);
            value = static_cast<float>(luaL_checknumber(L, 7));
        }

        const auto property = Animation::property_from_string(property_name ? property_name : "");
        if(property == Animation::Property::Invalid) return luaL_error(L, "invalid render property in set_animation_property");
        if(property == Animation::Property::Rotation) value = degrees_to_radians(value);
        animation->set_property(property, curve, value);
        return 0;
    }

    static bool resolve_existing(lua_State *L, int index, std::filesystem::path &path) {
        const char *relative = luaL_checkstring(L, index);
        if(!Runtime::instance().resolve_data_path(relative ? relative : "", path)) {
            luaL_error(L, "invalid Optic data path");
            return false;
        }
        std::error_code ec;
        if(!std::filesystem::is_regular_file(path, ec) || ec) {
            luaL_error(L, "Optic data file does not exist");
            return false;
        }
        return true;
    }

    static int get_image_content_bounds(lua_State *L) {
        if(lua_gettop(L) != 1) {
            return luaL_error(L, "invalid number of arguments in get_image_content_bounds");
        }

        std::filesystem::path path;
        if(!resolve_existing(L, 1, path)) {
            return 0;
        }

        Gdiplus::GdiplusStartupInput startup_input;
        ULONG_PTR gdiplus_token = 0;
        const auto startup_status =
            Gdiplus::GdiplusStartup(&gdiplus_token, &startup_input, nullptr);

        if(startup_status != Gdiplus::Ok) {
            return luaL_error(L, "GDI+ initialization failed in get_image_content_bounds");
        }

        Gdiplus::Bitmap source(path.c_str(), FALSE);
        if(source.GetLastStatus() != Gdiplus::Ok) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return luaL_error(L, "could not load image in get_image_content_bounds");
        }

        const UINT width = source.GetWidth();
        const UINT height = source.GetHeight();

        if(width == 0 || height == 0) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return luaL_error(L, "image has invalid dimensions in get_image_content_bounds");
        }

        UINT min_x = width;
        UINT min_y = height;
        UINT max_x = 0;
        UINT max_y = 0;
        bool found = false;

        for(UINT y = 0; y < height; ++y) {
            for(UINT x = 0; x < width; ++x) {
                Gdiplus::Color color;
                if(source.GetPixel(static_cast<INT>(x), static_cast<INT>(y), &color) != Gdiplus::Ok) {
                    continue;
                }

                if(color.GetAlpha() == 0) {
                    continue;
                }

                if(!found) {
                    min_x = max_x = x;
                    min_y = max_y = y;
                    found = true;
                }
                else {
                    min_x = std::min(min_x, x);
                    min_y = std::min(min_y, y);
                    max_x = std::max(max_x, x);
                    max_y = std::max(max_y, y);
                }
            }
        }

        if(!found) {
            min_x = 0;
            min_y = 0;
            max_x = width - 1;
            max_y = height - 1;
        }

        Gdiplus::GdiplusShutdown(gdiplus_token);

        lua_pushinteger(L, static_cast<lua_Integer>(width));
        lua_pushinteger(L, static_cast<lua_Integer>(height));
        lua_pushinteger(L, static_cast<lua_Integer>(min_x));
        lua_pushinteger(L, static_cast<lua_Integer>(min_y));
        lua_pushinteger(L, static_cast<lua_Integer>(max_x + 1));
        lua_pushinteger(L, static_cast<lua_Integer>(max_y + 1));
        return 6;
    }

    static int halo_bitmap_alpha_at(const std::uint8_t *data,
                                    int width,
                                    int height,
                                    int format,
                                    int x,
                                    int y) noexcept {
        if(!data || width <= 0 || height <= 0 || x < 0 || y < 0 || x >= width || y >= height) {
            return -1;
        }

        const std::size_t pixel = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                  static_cast<std::size_t>(x);

        switch(format) {
            case 0:
                return data[pixel];
            case 1:
                return 255;
            case 2:
                return static_cast<int>((data[pixel] >> 4) * 17U);
            case 3:
                return data[pixel * 2U + 1U];
            case 6:
                return 255;
            case 8: {
                std::uint16_t value = 0;
                std::memcpy(&value, data + pixel * 2U, sizeof(value));
                return (value & 0x8000U) ? 255 : 0;
            }
            case 9: {
                std::uint16_t value = 0;
                std::memcpy(&value, data + pixel * 2U, sizeof(value));
                return static_cast<int>(((value >> 12) & 0xFU) * 17U);
            }
            case 10:
                return 255;
            case 11:
                return data[pixel * 4U + 3U];

            case 14: {
                const int blocks_w = (width + 3) / 4;
                const int bx = x / 4;
                const int by = y / 4;
                const int px = x & 3;
                const int py = y & 3;
                const auto *block = data + (static_cast<std::size_t>(by) * blocks_w + bx) * 8U;
                std::uint16_t color0 = 0, color1 = 0;
                std::uint32_t indices = 0;
                std::memcpy(&color0, block + 0, 2);
                std::memcpy(&color1, block + 2, 2);
                std::memcpy(&indices, block + 4, 4);
                const unsigned code = (indices >> (2U * static_cast<unsigned>(py * 4 + px))) & 3U;
                return (color0 <= color1 && code == 3U) ? 0 : 255;
            }

            case 15: {
                const int blocks_w = (width + 3) / 4;
                const int bx = x / 4;
                const int by = y / 4;
                const int px = x & 3;
                const int py = y & 3;
                const auto *block = data + (static_cast<std::size_t>(by) * blocks_w + bx) * 16U;
                std::uint64_t alpha_bits = 0;
                std::memcpy(&alpha_bits, block, 8);
                const unsigned shift = 4U * static_cast<unsigned>(py * 4 + px);
                return static_cast<int>(((alpha_bits >> shift) & 0xFU) * 17U);
            }

            case 16: {
                const int blocks_w = (width + 3) / 4;
                const int bx = x / 4;
                const int by = y / 4;
                const int px = x & 3;
                const int py = y & 3;
                const auto *block = data + (static_cast<std::size_t>(by) * blocks_w + bx) * 16U;

                const int a0 = block[0];
                const int a1 = block[1];
                std::uint64_t bits = 0;
                for(int i = 0; i < 6; ++i) {
                    bits |= static_cast<std::uint64_t>(block[2 + i]) << (8U * static_cast<unsigned>(i));
                }
                const unsigned code = static_cast<unsigned>((bits >>
                    (3U * static_cast<unsigned>(py * 4 + px))) & 7U);

                int table[8] = {a0, a1, 0, 0, 0, 0, 0, 0};
                if(a0 > a1) {
                    table[2] = (6 * a0 + 1 * a1) / 7;
                    table[3] = (5 * a0 + 2 * a1) / 7;
                    table[4] = (4 * a0 + 3 * a1) / 7;
                    table[5] = (3 * a0 + 4 * a1) / 7;
                    table[6] = (2 * a0 + 5 * a1) / 7;
                    table[7] = (1 * a0 + 6 * a1) / 7;
                }
                else {
                    table[2] = (4 * a0 + 1 * a1) / 5;
                    table[3] = (3 * a0 + 2 * a1) / 5;
                    table[4] = (2 * a0 + 3 * a1) / 5;
                    table[5] = (1 * a0 + 4 * a1) / 5;
                    table[6] = 0;
                    table[7] = 255;
                }
                return table[code];
            }

            case 17:
                return 255;
            default:
                return -1;
        }
    }

    static int get_halo_bitmap_alpha_bounds(lua_State *L) {
        if(lua_gettop(L) != 8) {
            return luaL_error(L, "invalid number of arguments in get_halo_bitmap_alpha_bounds");
        }

        const auto address_value = static_cast<std::uintptr_t>(luaL_checkinteger(L, 1));
        const int width = static_cast<int>(luaL_checkinteger(L, 2));
        const int height = static_cast<int>(luaL_checkinteger(L, 3));
        const int format = static_cast<int>(luaL_checkinteger(L, 4));
        double left = luaL_checknumber(L, 5);
        double top = luaL_checknumber(L, 6);
        double right = luaL_checknumber(L, 7);
        double bottom = luaL_checknumber(L, 8);

        if(address_value == 0 || width <= 0 || height <= 0 || width > 8192 || height > 8192) {
            return 0;
        }

        const bool normalized =
            left >= -0.01 && top >= -0.01 && right <= 1.01 && bottom <= 1.01;

        int x0, y0, x1, y1;
        if(normalized) {
            x0 = static_cast<int>(std::floor(std::min(left, right) * width));
            x1 = static_cast<int>(std::ceil (std::max(left, right) * width));
            y0 = static_cast<int>(std::floor(std::min(top, bottom) * height));
            y1 = static_cast<int>(std::ceil (std::max(top, bottom) * height));
        }
        else {
            x0 = static_cast<int>(std::floor(std::min(left, right)));
            x1 = static_cast<int>(std::ceil (std::max(left, right)));
            y0 = static_cast<int>(std::floor(std::min(top, bottom)));
            y1 = static_cast<int>(std::ceil (std::max(top, bottom)));
        }

        x0 = std::clamp(x0, 0, width);
        x1 = std::clamp(x1, 0, width);
        y0 = std::clamp(y0, 0, height);
        y1 = std::clamp(y1, 0, height);
        if(x1 <= x0 || y1 <= y0) return 0;

        switch(format) {
            case 0: case 1: case 2: case 3: case 6: case 8: case 9:
            case 10: case 11: case 14: case 15: case 16: case 17:
                break;
            default:
                return 0;
        }

        const auto *data = reinterpret_cast<const std::uint8_t *>(address_value);
        int min_x = x1, min_y = y1, max_x = -1, max_y = -1;
        bool unsupported = false;

        __try {
            for(int y = y0; y < y1; ++y) {
                for(int x = x0; x < x1; ++x) {
                    const int alpha = halo_bitmap_alpha_at(data, width, height, format, x, y);
                    if(alpha < 0) {
                        unsupported = true;
                        break;
                    }

                    if(alpha > 8) {
                        min_x = std::min(min_x, x);
                        min_y = std::min(min_y, y);
                        max_x = std::max(max_x, x);
                        max_y = std::max(max_y, y);
                    }
                }
                if(unsupported) break;
            }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }

        if(unsupported || max_x < min_x || max_y < min_y) {
            return 0;
        }

        lua_pushinteger(L, min_x);
        lua_pushinteger(L, min_y);
        lua_pushinteger(L, max_x + 1);
        lua_pushinteger(L, max_y + 1);
        return 4;
    }

    static int expand_5_to_8(unsigned value) noexcept {
        value &= 0x1FU;
        return static_cast<int>((value << 3U) | (value >> 2U));
    }

    static int expand_6_to_8(unsigned value) noexcept {
        value &= 0x3FU;
        return static_cast<int>((value << 2U) | (value >> 4U));
    }

    static void decode_rgb565(std::uint16_t value,
                              int &r,
                              int &g,
                              int &b) noexcept {
        r = expand_5_to_8((value >> 11U) & 0x1FU);
        g = expand_6_to_8((value >> 5U) & 0x3FU);
        b = expand_5_to_8(value & 0x1FU);
    }

    static int visual_coverage(int alpha,
                               int r,
                               int g,
                               int b) noexcept {
        alpha = std::clamp(alpha, 0, 255);
        const int intensity = std::max({r, g, b});
        return (alpha * intensity + 127) / 255;
    }

    static int dxt_color_coverage(const std::uint8_t *color_block,
                                  int px,
                                  int py,
                                  int alpha,
                                  bool dxt1_transparency) noexcept {
        if(!color_block) return -1;

        std::uint16_t color0 = 0;
        std::uint16_t color1 = 0;
        std::uint32_t indices = 0;

        std::memcpy(&color0, color_block + 0, 2);
        std::memcpy(&color1, color_block + 2, 2);
        std::memcpy(&indices, color_block + 4, 4);

        int r0 = 0, g0 = 0, b0 = 0;
        int r1 = 0, g1 = 0, b1 = 0;

        decode_rgb565(color0, r0, g0, b0);
        decode_rgb565(color1, r1, g1, b1);

        int r[4] = {r0, r1, 0, 0};
        int g[4] = {g0, g1, 0, 0};
        int b[4] = {b0, b1, 0, 0};

        if(!dxt1_transparency || color0 > color1) {
            r[2] = (2 * r0 + r1) / 3;
            g[2] = (2 * g0 + g1) / 3;
            b[2] = (2 * b0 + b1) / 3;

            r[3] = (r0 + 2 * r1) / 3;
            g[3] = (g0 + 2 * g1) / 3;
            b[3] = (b0 + 2 * b1) / 3;
        }
        else {
            r[2] = (r0 + r1) / 2;
            g[2] = (g0 + g1) / 2;
            b[2] = (b0 + b1) / 2;

            r[3] = 0;
            g[3] = 0;
            b[3] = 0;
        }

        const unsigned code =
            (indices >> (2U * static_cast<unsigned>(py * 4 + px))) & 3U;

        if(dxt1_transparency && color0 <= color1 && code == 3U) {
            alpha = 0;
        }

        return visual_coverage(alpha, r[code], g[code], b[code]);
    }

    static int d3d_texture_visual_coverage_at(const std::uint8_t *data,
                                              int pitch,
                                              D3DFORMAT format,
                                              int width,
                                              int height,
                                              int x,
                                              int y) noexcept {
        if(!data || pitch == 0 || width <= 0 || height <= 0 ||
           x < 0 || y < 0 || x >= width || y >= height) {
            return -1;
        }

        const auto *row =
            data + static_cast<std::ptrdiff_t>(y) * pitch;

        switch(format) {
            case D3DFMT_A8:
                return row[x];

            case D3DFMT_L8: {
                const int l = row[x];
                return visual_coverage(255, l, l, l);
            }

            case D3DFMT_A4L4: {
                const std::uint8_t value = row[x];
                const int l = static_cast<int>((value & 0xFU) * 17U);
                const int a = static_cast<int>(((value >> 4U) & 0xFU) * 17U);
                return visual_coverage(a, l, l, l);
            }

            case D3DFMT_A8L8: {
                const int l = row[x * 2 + 0];
                const int a = row[x * 2 + 1];
                return visual_coverage(a, l, l, l);
            }

            case D3DFMT_R5G6B5: {
                std::uint16_t value = 0;
                std::memcpy(&value, row + x * 2, sizeof(value));

                int r = 0, g = 0, b = 0;
                decode_rgb565(value, r, g, b);
                return visual_coverage(255, r, g, b);
            }

            case D3DFMT_X1R5G5B5:
            case D3DFMT_A1R5G5B5: {
                std::uint16_t value = 0;
                std::memcpy(&value, row + x * 2, sizeof(value));

                const int a =
                    format == D3DFMT_A1R5G5B5
                        ? ((value & 0x8000U) ? 255 : 0)
                        : 255;

                const int r =
                    expand_5_to_8((value >> 10U) & 0x1FU);
                const int g =
                    expand_5_to_8((value >> 5U) & 0x1FU);
                const int b =
                    expand_5_to_8(value & 0x1FU);

                return visual_coverage(a, r, g, b);
            }

            case D3DFMT_A4R4G4B4: {
                std::uint16_t value = 0;
                std::memcpy(&value, row + x * 2, sizeof(value));

                const int a =
                    static_cast<int>(((value >> 12U) & 0xFU) * 17U);
                const int r =
                    static_cast<int>(((value >> 8U) & 0xFU) * 17U);
                const int g =
                    static_cast<int>(((value >> 4U) & 0xFU) * 17U);
                const int b =
                    static_cast<int>((value & 0xFU) * 17U);

                return visual_coverage(a, r, g, b);
            }

            case D3DFMT_X8R8G8B8:
            case D3DFMT_A8R8G8B8: {
                const auto *pixel = row + x * 4;

                const int b = pixel[0];
                const int g = pixel[1];
                const int r = pixel[2];
                const int a =
                    format == D3DFMT_A8R8G8B8 ? pixel[3] : 255;

                return visual_coverage(a, r, g, b);
            }

            case D3DFMT_A8B8G8R8: {
                const auto *pixel = row + x * 4;

                const int r = pixel[0];
                const int g = pixel[1];
                const int b = pixel[2];
                const int a = pixel[3];

                return visual_coverage(a, r, g, b);
            }

            case D3DFMT_A2R10G10B10:
            case D3DFMT_A2B10G10R10: {
                std::uint32_t value = 0;
                std::memcpy(&value, row + x * 4, sizeof(value));

                const int a =
                    static_cast<int>(((value >> 30U) & 0x3U) * 85U);

                const unsigned c0 = value & 0x3FFU;
                const unsigned c1 = (value >> 10U) & 0x3FFU;
                const unsigned c2 = (value >> 20U) & 0x3FFU;

                const int v0 = static_cast<int>((c0 * 255U + 511U) / 1023U);
                const int v1 = static_cast<int>((c1 * 255U + 511U) / 1023U);
                const int v2 = static_cast<int>((c2 * 255U + 511U) / 1023U);

                return visual_coverage(a, v0, v1, v2);
            }

            case D3DFMT_DXT1: {
                const int bx = x / 4;
                const int by = y / 4;
                const int px = x & 3;
                const int py = y & 3;

                const auto *block =
                    data +
                    static_cast<std::ptrdiff_t>(by) * pitch +
                    bx * 8;

                return dxt_color_coverage(
                    block, px, py, 255, true
                );
            }

            case D3DFMT_DXT2:
            case D3DFMT_DXT3: {
                const int bx = x / 4;
                const int by = y / 4;
                const int px = x & 3;
                const int py = y & 3;

                const auto *block =
                    data +
                    static_cast<std::ptrdiff_t>(by) * pitch +
                    bx * 16;

                std::uint64_t alpha_bits = 0;
                std::memcpy(&alpha_bits, block, 8);

                const unsigned shift =
                    4U * static_cast<unsigned>(py * 4 + px);

                const int alpha =
                    static_cast<int>(
                        ((alpha_bits >> shift) & 0xFU) * 17U
                    );

                return dxt_color_coverage(
                    block + 8, px, py, alpha, false
                );
            }

            case D3DFMT_DXT4:
            case D3DFMT_DXT5: {
                const int bx = x / 4;
                const int by = y / 4;
                const int px = x & 3;
                const int py = y & 3;

                const auto *block =
                    data +
                    static_cast<std::ptrdiff_t>(by) * pitch +
                    bx * 16;

                const int a0 = block[0];
                const int a1 = block[1];

                std::uint64_t bits = 0;
                for(int i = 0; i < 6; ++i) {
                    bits |=
                        static_cast<std::uint64_t>(block[2 + i])
                        << (8U * static_cast<unsigned>(i));
                }

                const unsigned code =
                    static_cast<unsigned>(
                        (
                            bits >>
                            (3U * static_cast<unsigned>(py * 4 + px))
                        ) & 7U
                    );

                int table[8] = {a0, a1, 0, 0, 0, 0, 0, 0};

                if(a0 > a1) {
                    table[2] = (6 * a0 + 1 * a1) / 7;
                    table[3] = (5 * a0 + 2 * a1) / 7;
                    table[4] = (4 * a0 + 3 * a1) / 7;
                    table[5] = (3 * a0 + 4 * a1) / 7;
                    table[6] = (2 * a0 + 5 * a1) / 7;
                    table[7] = (1 * a0 + 6 * a1) / 7;
                }
                else {
                    table[2] = (4 * a0 + 1 * a1) / 5;
                    table[3] = (3 * a0 + 2 * a1) / 5;
                    table[4] = (2 * a0 + 3 * a1) / 5;
                    table[5] = (1 * a0 + 4 * a1) / 5;
                    table[6] = 0;
                    table[7] = 255;
                }

                return dxt_color_coverage(
                    block + 8, px, py, table[code], false
                );
            }

            default:
                return -1;
        }
    }

    static bool scan_d3d_visual_region(const std::uint8_t *data,
                                       int pitch,
                                       D3DFORMAT format,
                                       int width,
                                       int height,
                                       int x0,
                                       int y0,
                                       int x1,
                                       int y1,
                                       int &min_x,
                                       int &min_y,
                                       int &max_x,
                                       int &max_y) noexcept {
        min_x = x1;
        min_y = y1;
        max_x = -1;
        max_y = -1;

        bool supported = true;

        constexpr int VISIBLE_THRESHOLD = 8;

        for(int y = y0; y < y1; ++y) {
            for(int x = x0; x < x1; ++x) {
                const int coverage =
                    d3d_texture_visual_coverage_at(
                        data, pitch, format, width, height, x, y
                    );

                if(coverage < 0) {
                    supported = false;
                    break;
                }

                if(coverage > VISIBLE_THRESHOLD) {
                    min_x = std::min(min_x, x);
                    min_y = std::min(min_y, y);
                    max_x = std::max(max_x, x);
                    max_y = std::max(max_y, y);
                }
            }

            if(!supported) break;
        }

        return supported && max_x >= min_x && max_y >= min_y;
    }

    static bool compute_texture_clip(const D3DSURFACE_DESC &desc,
                                     double left,
                                     double top,
                                     double right,
                                     double bottom,
                                     int &x0,
                                     int &y0,
                                     int &x1,
                                     int &y1) noexcept {
        if(desc.Width == 0 || desc.Height == 0 ||
           desc.Width > 8192 || desc.Height > 8192) {
            return false;
        }

        const bool normalized =
            left >= -0.01 && top >= -0.01 &&
            right <= 1.01 && bottom <= 1.01;

        if(normalized) {
            x0 = static_cast<int>(
                std::floor(std::min(left, right) * desc.Width)
            );
            x1 = static_cast<int>(
                std::ceil(std::max(left, right) * desc.Width)
            );
            y0 = static_cast<int>(
                std::floor(std::min(top, bottom) * desc.Height)
            );
            y1 = static_cast<int>(
                std::ceil(std::max(top, bottom) * desc.Height)
            );
        }
        else {
            x0 = static_cast<int>(std::floor(std::min(left, right)));
            x1 = static_cast<int>(std::ceil(std::max(left, right)));
            y0 = static_cast<int>(std::floor(std::min(top, bottom)));
            y1 = static_cast<int>(std::ceil(std::max(top, bottom)));
        }

        x0 = std::clamp(x0, 0, static_cast<int>(desc.Width));
        x1 = std::clamp(x1, 0, static_cast<int>(desc.Width));
        y0 = std::clamp(y0, 0, static_cast<int>(desc.Height));
        y1 = std::clamp(y1, 0, static_cast<int>(desc.Height));

        return x1 > x0 && y1 > y0;
    }

    static bool scan_hardware_texture_alpha(IDirect3DTexture9 *texture,
                                            double left,
                                            double top,
                                            double right,
                                            double bottom,
                                            int &texture_width,
                                            int &texture_height,
                                            int &min_x,
                                            int &min_y,
                                            int &max_x,
                                            int &max_y) noexcept {
        if(!texture) return false;

        D3DSURFACE_DESC desc{};
        if(FAILED(texture->GetLevelDesc(0, &desc))) {
            return false;
        }

        texture_width = static_cast<int>(desc.Width);
        texture_height = static_cast<int>(desc.Height);

        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if(!compute_texture_clip(
                desc, left, top, right, bottom, x0, y0, x1, y1
            )) {
            return false;
        }

        D3DLOCKED_RECT locked{};

        if(SUCCEEDED(
                texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY)
            )) {
            const bool ok = scan_d3d_visual_region(
                static_cast<const std::uint8_t *>(locked.pBits),
                locked.Pitch,
                desc.Format,
                texture_width,
                texture_height,
                x0, y0, x1, y1,
                min_x, min_y, max_x, max_y
            );

            texture->UnlockRect(0);

            if(ok) {
                return true;
            }
        }

        IDirect3DDevice9 *device = nullptr;
        IDirect3DSurface9 *source = nullptr;
        IDirect3DSurface9 *render_target = nullptr;
        IDirect3DSurface9 *system_surface = nullptr;

        bool result = false;

        if(FAILED(texture->GetDevice(&device)) || !device) {
            return false;
        }

        if(SUCCEEDED(texture->GetSurfaceLevel(0, &source)) && source) {
            if(SUCCEEDED(device->CreateRenderTarget(
                    desc.Width,
                    desc.Height,
                    D3DFMT_A8R8G8B8,
                    D3DMULTISAMPLE_NONE,
                    0,
                    FALSE,
                    &render_target,
                    nullptr
                )) && render_target) {

                if(SUCCEEDED(device->StretchRect(
                        source,
                        nullptr,
                        render_target,
                        nullptr,
                        D3DTEXF_NONE
                    ))) {

                    if(SUCCEEDED(device->CreateOffscreenPlainSurface(
                            desc.Width,
                            desc.Height,
                            D3DFMT_A8R8G8B8,
                            D3DPOOL_SYSTEMMEM,
                            &system_surface,
                            nullptr
                        )) && system_surface) {

                        if(SUCCEEDED(device->GetRenderTargetData(
                                render_target,
                                system_surface
                            ))) {

                            D3DLOCKED_RECT readback{};

                            if(SUCCEEDED(system_surface->LockRect(
                                    &readback,
                                    nullptr,
                                    D3DLOCK_READONLY
                                ))) {

                                result = scan_d3d_visual_region(
                                    static_cast<const std::uint8_t *>(
                                        readback.pBits
                                    ),
                                    readback.Pitch,
                                    D3DFMT_A8R8G8B8,
                                    texture_width,
                                    texture_height,
                                    x0, y0, x1, y1,
                                    min_x, min_y, max_x, max_y
                                );

                                system_surface->UnlockRect();
                            }
                        }
                    }
                }
            }
        }

        if(system_surface) system_surface->Release();
        if(render_target) render_target->Release();
        if(source) source->Release();
        device->Release();

        return result;
    }

    static int get_halo_texture_alpha_bounds(lua_State *L) {
        if(lua_gettop(L) != 5) {
            return luaL_error(
                L,
                "invalid number of arguments in get_halo_texture_alpha_bounds"
            );
        }

        const auto hardware_value =
            static_cast<std::uintptr_t>(luaL_checkinteger(L, 1));

        const double left = luaL_checknumber(L, 2);
        const double top = luaL_checknumber(L, 3);
        const double right = luaL_checknumber(L, 4);
        const double bottom = luaL_checknumber(L, 5);

        if(hardware_value == 0) {
            return 0;
        }

        auto *base =
            reinterpret_cast<IDirect3DBaseTexture9 *>(hardware_value);

        IDirect3DTexture9 *texture = nullptr;

        __try {
            if(base->GetType() != D3DRTYPE_TEXTURE) {
                return 0;
            }

            texture = reinterpret_cast<IDirect3DTexture9 *>(base);
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }

        int texture_width = 0;
        int texture_height = 0;
        int min_x = 0, min_y = 0, max_x = 0, max_y = 0;
        bool ok = false;

        __try {
            ok = scan_hardware_texture_alpha(
                texture,
                left, top, right, bottom,
                texture_width, texture_height,
                min_x, min_y, max_x, max_y
            );
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }

        if(!ok) {
            return 0;
        }

        lua_pushinteger(L, texture_width);
        lua_pushinteger(L, texture_height);
        lua_pushinteger(L, min_x);
        lua_pushinteger(L, min_y);
        lua_pushinteger(L, max_x + 1);
        lua_pushinteger(L, max_y + 1);
        return 6;
    }

    static std::uint64_t fnv1a_path_hash(std::wstring_view value) noexcept {
        std::uint64_t hash = 1469598103934665603ULL;
        for(const wchar_t ch : value) {
            hash ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(ch));
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    static bool get_png_encoder_clsid(CLSID &clsid) noexcept {
        UINT count = 0;
        UINT bytes = 0;

        if(Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok ||
           count == 0 || bytes == 0) {
            return false;
        }

        std::vector<std::byte> storage(bytes);
        auto *encoders =
            reinterpret_cast<Gdiplus::ImageCodecInfo *>(storage.data());

        if(Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) {
            return false;
        }

        for(UINT i = 0; i < count; ++i) {
            if(encoders[i].MimeType &&
               std::wcscmp(encoders[i].MimeType, L"image/png") == 0) {
                clsid = encoders[i].Clsid;
                return true;
            }
        }

        return false;
    }

    static Gdiplus::Color get_hitmarker_source_color(
        Gdiplus::Bitmap &source,
        bool kill_fallback
    ) noexcept {
        const UINT width = source.GetWidth();
        const UINT height = source.GetHeight();

        std::uint64_t sum_r = 0;
        std::uint64_t sum_g = 0;
        std::uint64_t sum_b = 0;
        std::uint64_t weight_sum = 0;
        BYTE max_alpha = 0;

        for(UINT y = 0; y < height; ++y) {
            for(UINT x = 0; x < width; ++x) {
                Gdiplus::Color color;
                if(source.GetPixel(static_cast<INT>(x),
                                   static_cast<INT>(y),
                                   &color) != Gdiplus::Ok) {
                    continue;
                }

                const BYTE alpha = color.GetAlpha();
                const BYTE r = color.GetRed();
                const BYTE g = color.GetGreen();
                const BYTE b = color.GetBlue();
                const BYTE intensity = std::max({r, g, b});

                if(alpha <= 8 || intensity <= 8) {
                    continue;
                }

                const std::uint64_t weight =
                    static_cast<std::uint64_t>(alpha) *
                    static_cast<std::uint64_t>(intensity);

                sum_r += static_cast<std::uint64_t>(r) * weight;
                sum_g += static_cast<std::uint64_t>(g) * weight;
                sum_b += static_cast<std::uint64_t>(b) * weight;
                weight_sum += weight;
                max_alpha = std::max(max_alpha, alpha);
            }
        }

        if(weight_sum == 0) {
            return kill_fallback
                ? Gdiplus::Color(255, 255, 48, 48)
                : Gdiplus::Color(255, 255, 255, 255);
        }

        const BYTE r = static_cast<BYTE>(
            std::clamp<std::uint64_t>(sum_r / weight_sum, 0, 255)
        );
        const BYTE g = static_cast<BYTE>(
            std::clamp<std::uint64_t>(sum_g / weight_sum, 0, 255)
        );
        const BYTE b = static_cast<BYTE>(
            std::clamp<std::uint64_t>(sum_b / weight_sum, 0, 255)
        );

        const BYTE alpha = std::max<BYTE>(220, max_alpha);

        return Gdiplus::Color(alpha, r, g, b);
    }

    static int create_procedural_hitmarker(lua_State *L) {
        const int argc = lua_gettop(L);
        if(argc != 6 && argc != 9) {
            return luaL_error(
                L,
                "invalid number of arguments in create_procedural_hitmarker"
            );
        }

        std::filesystem::path source_path;
        if(!resolve_existing(L, 1, source_path)) {
            return 0;
        }

        const float reticle_width =
            static_cast<float>(luaL_checknumber(L, 2));
        const float reticle_height =
            static_cast<float>(luaL_checknumber(L, 3));
        const float arm_length =
            static_cast<float>(luaL_checknumber(L, 4));
        const float arm_thickness =
            static_cast<float>(luaL_checknumber(L, 5));
        const float reticle_padding =
            static_cast<float>(luaL_checknumber(L, 6));

        if(reticle_width <= 0.0f || reticle_height <= 0.0f ||
           reticle_width > 1024.0f || reticle_height > 1024.0f ||
           arm_length <= 0.0f || arm_length > 128.0f ||
           arm_thickness <= 0.0f || arm_thickness > 32.0f ||
           reticle_padding < 0.0f || reticle_padding > 128.0f) {
            return luaL_error(
                L,
                "invalid dimensions in create_procedural_hitmarker"
            );
        }

        Gdiplus::GdiplusStartupInput startup_input;
        ULONG_PTR gdiplus_token = 0;

        if(Gdiplus::GdiplusStartup(
                &gdiplus_token,
                &startup_input,
                nullptr
            ) != Gdiplus::Ok) {
            return luaL_error(
                L,
                "GDI+ initialization failed in create_procedural_hitmarker"
            );
        }

        Gdiplus::Bitmap source(source_path.c_str(), FALSE);
        if(source.GetLastStatus() != Gdiplus::Ok) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return luaL_error(
                L,
                "could not load source image in create_procedural_hitmarker"
            );
        }

        std::wstring lower_name = source_path.filename().wstring();
        std::transform(
            lower_name.begin(),
            lower_name.end(),
            lower_name.begin(),
            [](wchar_t ch) {
                return static_cast<wchar_t>(std::towlower(ch));
            }
        );
        const bool kill_fallback =
            lower_name.find(L"kill") != std::wstring::npos;

        Gdiplus::Color arm_color =
            get_hitmarker_source_color(source, kill_fallback);

        int override_r = -1;
        int override_g = -1;
        int override_b = -1;
        if(argc == 9) {
            override_r = std::clamp(static_cast<int>(luaL_checkinteger(L, 7)), 0, 255);
            override_g = std::clamp(static_cast<int>(luaL_checkinteger(L, 8)), 0, 255);
            override_b = std::clamp(static_cast<int>(luaL_checkinteger(L, 9)), 0, 255);
            arm_color = Gdiplus::Color(
                255,
                static_cast<BYTE>(override_r),
                static_cast<BYTE>(override_g),
                static_cast<BYTE>(override_b)
            );
        }

        const float reticle_diameter =
            std::max(reticle_width, reticle_height);

        const float inner_radius =
            reticle_diameter * 0.5f + reticle_padding;
        const float outer_radius =
            inner_radius + arm_length;

        const float safety_margin =
            std::max(2.0f, arm_thickness * 0.75f + 1.0f);

        int canvas_size = static_cast<int>(
            std::ceil((outer_radius + safety_margin) * 2.0f)
        );
        canvas_size = std::clamp(canvas_size, 16, 512);

        Gdiplus::Bitmap generated(
            canvas_size,
            canvas_size,
            PixelFormat32bppARGB
        );

        if(generated.GetLastStatus() != Gdiplus::Ok) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return luaL_error(
                L,
                "could not create procedural hitmarker bitmap"
            );
        }

        Gdiplus::Graphics graphics(&generated);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

        Gdiplus::Pen pen(arm_color, arm_thickness);
        pen.SetStartCap(Gdiplus::LineCapFlat);
        pen.SetEndCap(Gdiplus::LineCapFlat);
        pen.SetLineJoin(Gdiplus::LineJoinMiter);

        const float center =
            static_cast<float>(canvas_size) * 0.5f;

        constexpr float inv_sqrt_2 =
            0.70710678118654752440f;

        const float inner =
            inner_radius * inv_sqrt_2;
        const float outer =
            outer_radius * inv_sqrt_2;

        graphics.DrawLine(
            &pen,
            center - inner, center - inner,
            center - outer, center - outer
        );

        graphics.DrawLine(
            &pen,
            center + inner, center - inner,
            center + outer, center - outer
        );

        graphics.DrawLine(
            &pen,
            center - inner, center + inner,
            center - outer, center + outer
        );

        graphics.DrawLine(
            &pen,
            center + inner, center + inner,
            center + outer, center + outer
        );

        auto &store = Runtime::instance().store();
        std::filesystem::path cache_dir =
            store.data_root() /
            ".opticcompat" /
            "generated_hitmarkers";

        std::error_code ec;
        std::filesystem::create_directories(cache_dir, ec);

        if(ec) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return luaL_error(
                L,
                "could not create procedural hitmarker cache directory"
            );
        }

        std::uint64_t path_hash =
            fnv1a_path_hash(source_path.wstring());

        std::error_code time_ec;
        const auto source_time =
            std::filesystem::last_write_time(
                source_path,
                time_ec
            );

        if(!time_ec) {
            const auto stamp =
                static_cast<std::uint64_t>(
                    source_time.time_since_epoch().count()
                );
            path_hash ^= stamp +
                         0x9E3779B97F4A7C15ULL +
                         (path_hash << 6U) +
                         (path_hash >> 2U);
        }

        const int rw =
            static_cast<int>(std::lround(reticle_width));
        const int rh =
            static_cast<int>(std::lround(reticle_height));
        const int al =
            static_cast<int>(std::lround(arm_length * 10.0f));
        const int at =
            static_cast<int>(std::lround(arm_thickness * 10.0f));
        const int rp =
            static_cast<int>(std::lround(reticle_padding * 10.0f));

        const std::wstring file_name =
            L"hm_" +
            std::to_wstring(path_hash) + L"_" +
            std::to_wstring(rw) + L"x" +
            std::to_wstring(rh) + L"_l" +
            std::to_wstring(al) + L"_t" +
            std::to_wstring(at) + L"_p" +
            std::to_wstring(rp) + L"_rgb" +
            std::to_wstring(override_r) + L"-" +
            std::to_wstring(override_g) + L"-" +
            std::to_wstring(override_b) + L".png";

        const auto generated_path =
            cache_dir / file_name;

        CLSID png_clsid{};
        if(!get_png_encoder_clsid(png_clsid)) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return luaL_error(
                L,
                "PNG encoder not available for procedural hitmarker"
            );
        }

        if(generated.Save(
                generated_path.c_str(),
                &png_clsid,
                nullptr
            ) != Gdiplus::Ok) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return luaL_error(
                L,
                "could not save procedural hitmarker PNG"
            );
        }

        Gdiplus::GdiplusShutdown(gdiplus_token);

        try {
            const auto handle =
                store.create_sprite(
                    generated_path,
                    canvas_size,
                    canvas_size
                );

            lua_pushinteger(
                L,
                static_cast<lua_Integer>(handle)
            );
            lua_pushinteger(L, canvas_size);
            lua_pushinteger(L, canvas_size);
            return 3;
        }
        catch(const std::exception &e) {
            return luaL_error(
                L,
                "create_procedural_hitmarker failed: %s",
                e.what()
            );
        }
    }

    static int create_procedural_hit_flash(lua_State *L) {
        if(lua_gettop(L) != 3) {
            return luaL_error(
                L,
                "invalid number of arguments in create_procedural_hit_flash"
            );
        }

        std::filesystem::path source_path;
        if(!resolve_existing(L, 1, source_path)) {
            return 0;
        }

        const float flash_length =
            static_cast<float>(luaL_checknumber(L, 2));
        const float flash_thickness =
            static_cast<float>(luaL_checknumber(L, 3));

        if(flash_length <= 0.0f || flash_length > 128.0f ||
           flash_thickness <= 0.0f || flash_thickness > 32.0f) {
            return luaL_error(
                L,
                "invalid dimensions in create_procedural_hit_flash"
            );
        }

        Gdiplus::GdiplusStartupInput startup_input;
        ULONG_PTR gdiplus_token = 0;

        if(Gdiplus::GdiplusStartup(
                &gdiplus_token,
                &startup_input,
                nullptr
            ) != Gdiplus::Ok) {
            return luaL_error(
                L,
                "GDI+ initialization failed in create_procedural_hit_flash"
            );
        }

        Gdiplus::Bitmap source(source_path.c_str(), FALSE);
        if(source.GetLastStatus() != Gdiplus::Ok) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return luaL_error(
                L,
                "could not load source image in create_procedural_hit_flash"
            );
        }

        std::wstring lower_name = source_path.filename().wstring();
        std::transform(
            lower_name.begin(),
            lower_name.end(),
            lower_name.begin(),
            [](wchar_t ch) {
                return static_cast<wchar_t>(std::towlower(ch));
            }
        );

        const bool kill_fallback =
            lower_name.find(L"kill") != std::wstring::npos;

        const Gdiplus::Color flash_color =
            get_hitmarker_source_color(source, kill_fallback);

        const float margin =
            std::max(3.0f, flash_thickness * 2.0f);

        int canvas_size = static_cast<int>(
            std::ceil(flash_length + margin * 2.0f)
        );
        canvas_size = std::clamp(canvas_size, 16, 128);

        Gdiplus::Bitmap generated(
            canvas_size,
            canvas_size,
            PixelFormat32bppARGB
        );

        if(generated.GetLastStatus() != Gdiplus::Ok) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return luaL_error(
                L,
                "could not create procedural hit flash bitmap"
            );
        }

        Gdiplus::Graphics graphics(&generated);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

        const float center =
            static_cast<float>(canvas_size) * 0.5f;

        const float half_vertical =
            flash_length * 0.5f;

        const float half_horizontal =
            flash_length * 0.16f;

        Gdiplus::Pen main_pen(
            flash_color,
            flash_thickness
        );
        main_pen.SetStartCap(Gdiplus::LineCapRound);
        main_pen.SetEndCap(Gdiplus::LineCapRound);

        graphics.DrawLine(
            &main_pen,
            center,
            center - half_vertical,
            center,
            center + half_vertical
        );

        Gdiplus::Pen cross_pen(
            flash_color,
            std::max(1.0f, flash_thickness * 0.72f)
        );
        cross_pen.SetStartCap(Gdiplus::LineCapRound);
        cross_pen.SetEndCap(Gdiplus::LineCapRound);

        graphics.DrawLine(
            &cross_pen,
            center - half_horizontal,
            center,
            center + half_horizontal,
            center
        );

        Gdiplus::SolidBrush core_brush(flash_color);
        const float core_size =
            std::max(2.0f, flash_thickness * 1.25f);

        graphics.FillEllipse(
            &core_brush,
            center - core_size * 0.5f,
            center - core_size * 0.5f,
            core_size,
            core_size
        );

        auto &store = Runtime::instance().store();
        std::filesystem::path cache_dir =
            store.data_root() /
            ".opticcompat" /
            "generated_hitmarkers";

        std::error_code ec;
        std::filesystem::create_directories(cache_dir, ec);

        if(ec) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return luaL_error(
                L,
                "could not create hit flash cache directory"
            );
        }

        std::uint64_t path_hash =
            fnv1a_path_hash(source_path.wstring());

        std::error_code time_ec;
        const auto source_time =
            std::filesystem::last_write_time(
                source_path,
                time_ec
            );

        if(!time_ec) {
            const auto stamp =
                static_cast<std::uint64_t>(
                    source_time.time_since_epoch().count()
                );
            path_hash ^= stamp +
                         0x9E3779B97F4A7C15ULL +
                         (path_hash << 6U) +
                         (path_hash >> 2U);
        }

        const int fl =
            static_cast<int>(std::lround(flash_length * 10.0f));
        const int ft =
            static_cast<int>(std::lround(flash_thickness * 10.0f));

        const std::wstring file_name =
            L"hf_" +
            std::to_wstring(path_hash) +
            L"_l" + std::to_wstring(fl) +
            L"_t" + std::to_wstring(ft) +
            L".png";

        const auto generated_path =
            cache_dir / file_name;

        CLSID png_clsid{};
        if(!get_png_encoder_clsid(png_clsid)) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return luaL_error(
                L,
                "PNG encoder not available for procedural hit flash"
            );
        }

        if(generated.Save(
                generated_path.c_str(),
                &png_clsid,
                nullptr
            ) != Gdiplus::Ok) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return luaL_error(
                L,
                "could not save procedural hit flash PNG"
            );
        }

        Gdiplus::GdiplusShutdown(gdiplus_token);

        try {
            const auto handle =
                store.create_sprite(
                    generated_path,
                    canvas_size,
                    canvas_size
                );

            lua_pushinteger(
                L,
                static_cast<lua_Integer>(handle)
            );
            lua_pushinteger(L, canvas_size);
            lua_pushinteger(L, canvas_size);
            return 3;
        }
        catch(const std::exception &e) {
            return luaL_error(
                L,
                "create_procedural_hit_flash failed: %s",
                e.what()
            );
        }
    }

    static float hm_smooth(float t) noexcept {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    static float hm_ease_out(float t) noexcept {
        t = std::clamp(t, 0.0f, 1.0f);
        const float u = 1.0f - t;
        return 1.0f - u * u * u;
    }

    static Gdiplus::Color hm_alpha(const Gdiplus::Color &c, float a) noexcept {
        const BYTE alpha = static_cast<BYTE>(std::clamp(
            static_cast<int>(std::lround(c.GetAlpha() * std::clamp(a, 0.0f, 1.0f))),
            0, 255));
        return Gdiplus::Color(alpha, c.GetRed(), c.GetGreen(), c.GetBlue());
    }

    static void hm_draw_arms(Gdiplus::Graphics &g,
                             float cx,
                             float cy,
                             float radius,
                             float arm_length,
                             float thickness,
                             const Gdiplus::Color &color) noexcept {
        Gdiplus::Pen pen(color, thickness);
        pen.SetStartCap(Gdiplus::LineCapFlat);
        pen.SetEndCap(Gdiplus::LineCapFlat);

        constexpr float k = 0.7071067811865475f;
        const float i = radius * k;
        const float o = (radius + arm_length) * k;

        g.DrawLine(&pen, cx - i, cy - i, cx - o, cy - o);
        g.DrawLine(&pen, cx + i, cy - i, cx + o, cy - o);
        g.DrawLine(&pen, cx - i, cy + i, cx - o, cy + o);
        g.DrawLine(&pen, cx + i, cy + i, cx + o, cy + o);
    }

    static void hm_draw_paired_arms(
        Gdiplus::Graphics &g,
        float cx,
        float cy,
        float radius,
        float arm_length,
        float thickness,
        float pair_half_offset,
        const Gdiplus::Color &color
    ) noexcept {
        Gdiplus::Pen main_pen(color, thickness);
        main_pen.SetStartCap(Gdiplus::LineCapFlat);
        main_pen.SetEndCap(Gdiplus::LineCapFlat);

        constexpr float k = 0.7071067811865475f;

        constexpr float directions[4][2] = {
            {-k, -k},
            { k, -k},
            {-k,  k},
            { k,  k}
        };

        constexpr float secondary_sides[4] = {
            -1.0f,
             1.0f,
             1.0f,
            -1.0f
        };

        const float pair_separation =
            pair_half_offset * 2.0f;

        constexpr float secondary_start_fraction = 0.28f;

        for(int arm = 0; arm < 4; ++arm) {
            const float dx = directions[arm][0];
            const float dy = directions[arm][1];

            const float nx = -dy;
            const float ny =  dx;

            const float inner_x =
                cx + dx * radius;

            const float inner_y =
                cy + dy * radius;

            const float outer_x =
                cx + dx * (radius + arm_length);

            const float outer_y =
                cy + dy * (radius + arm_length);

            g.DrawLine(
                &main_pen,
                inner_x,
                inner_y,
                outer_x,
                outer_y
            );

            const float side =
                secondary_sides[arm];

            const float ox =
                nx * pair_separation * side;

            const float oy =
                ny * pair_separation * side;

            const float secondary_radius =
                radius + arm_length * secondary_start_fraction;

            const Gdiplus::PointF secondary_inner(
                cx + dx * secondary_radius + ox,
                cy + dy * secondary_radius + oy
            );

            const Gdiplus::PointF secondary_outer(
                outer_x + ox,
                outer_y + oy
            );

            Gdiplus::LinearGradientBrush fade_brush(
                secondary_inner,
                secondary_outer,
                hm_alpha(color, 0.0f),
                color
            );

            Gdiplus::Color fade_colors[4] = {
                hm_alpha(color, 0.0f),
                hm_alpha(color, 0.40f),
                hm_alpha(color, 0.70f),
                color
            };

            Gdiplus::REAL fade_positions[4] = {
                0.00f,
                0.34f,
                0.67f,
                1.00f
            };

            fade_brush.SetInterpolationColors(
                fade_colors,
                fade_positions,
                4
            );

            Gdiplus::Pen secondary_pen(
                &fade_brush,
                thickness
            );

            secondary_pen.SetStartCap(
                Gdiplus::LineCapFlat
            );

            secondary_pen.SetEndCap(
                Gdiplus::LineCapFlat
            );

            g.DrawLine(
                &secondary_pen,
                secondary_inner,
                secondary_outer
            );
        }
    }

    static void hm_draw_flash(Gdiplus::Graphics &g,
                              float cx,
                              float cy,
                              float length,
                              float thickness,
                              const Gdiplus::Color &color,
                              float intensity,
                              bool kill) noexcept {
        intensity = std::clamp(intensity, 0.0f, 1.0f);
        if(intensity <= 0.001f) return;

        const auto c = hm_alpha(color, intensity);
        const float half_v = length * (kill ? 0.58f : 0.50f) * (0.6f + intensity * 0.4f);
        const float half_h = length * (kill ? 0.20f : 0.15f) * intensity;

        Gdiplus::Pen vp(c, thickness);
        vp.SetStartCap(Gdiplus::LineCapRound);
        vp.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&vp, cx, cy - half_v, cx, cy + half_v);

        Gdiplus::Pen hp(c, std::max(1.0f, thickness * 0.72f));
        hp.SetStartCap(Gdiplus::LineCapRound);
        hp.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&hp, cx - half_h, cy, cx + half_h, cy);

        Gdiplus::SolidBrush brush(c);
        const float core = std::max(1.5f, thickness * (kill ? 1.55f : 1.25f) * intensity);
        g.FillEllipse(&brush, cx - core * 0.5f, cy - core * 0.5f, core, core);
    }

    static int create_procedural_hitmarker_fx(lua_State *L) {
        const int argc = lua_gettop(L);
        if(argc != 11 && argc != 14) {
            return luaL_error(
                L,
                "invalid number of arguments in create_procedural_hitmarker_fx"
            );
        }

        std::filesystem::path source_path;
        if(!resolve_existing(L, 1, source_path)) {
            return 0;
        }

        const float reticle_w =
            static_cast<float>(luaL_checknumber(L, 2));
        const float reticle_h =
            static_cast<float>(luaL_checknumber(L, 3));
        const float arm_len =
            static_cast<float>(luaL_checknumber(L, 4));
        const float thickness =
            static_cast<float>(luaL_checknumber(L, 5));
        const float padding =
            static_cast<float>(luaL_checknumber(L, 6));

        const int effect_kind =
            static_cast<int>(luaL_checkinteger(L, 7));

        if(effect_kind < 0 || effect_kind > 5) {
            return luaL_error(
                L,
                "invalid hitmarker FX effect kind"
            );
        }

        const bool kill =
            effect_kind == 1;

        const bool critical = effect_kind == 2;

        const int combo =
            std::clamp(
                static_cast<int>(
                    luaL_checkinteger(L, 8)
                ),
                1,
                3
            );

        const float motion =
            static_cast<float>(luaL_checknumber(L, 9));

        const float flash_len =
            static_cast<float>(luaL_checknumber(L, 10));
        const float flash_thickness =
            static_cast<float>(luaL_checknumber(L, 11));

        if(reticle_w <= 0.0f ||
           reticle_h <= 0.0f ||
           reticle_w > 1024.0f ||
           reticle_h > 1024.0f ||
           arm_len <= 0.0f ||
           arm_len > 128.0f ||
           thickness <= 0.0f ||
           thickness > 32.0f ||
           padding < 0.0f ||
           padding > 128.0f ||
           motion < 0.0f ||
           motion > 128.0f ||
           flash_len <= 0.0f ||
           flash_thickness <= 0.0f) {
            return luaL_error(
                L,
                "invalid hitmarker FX dimensions"
            );
        }

        Gdiplus::GdiplusStartupInput startup_input;
        ULONG_PTR gdiplus_token = 0;

        if(Gdiplus::GdiplusStartup(
                &gdiplus_token,
                &startup_input,
                nullptr
            ) != Gdiplus::Ok) {
            return luaL_error(
                L,
                "GDI+ initialization failed in create_procedural_hitmarker_fx"
            );
        }

        Gdiplus::Bitmap source(source_path.c_str(), FALSE);

        if(source.GetLastStatus() != Gdiplus::Ok) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return luaL_error(
                L,
                "could not load hitmarker source image"
            );
        }

        Gdiplus::Color base_color =
            get_hitmarker_source_color(
                source,
                kill || critical
            );

        int override_r = -1;
        int override_g = -1;
        int override_b = -1;
        if(argc == 14) {
            override_r = std::clamp(static_cast<int>(luaL_checkinteger(L, 12)), 0, 255);
            override_g = std::clamp(static_cast<int>(luaL_checkinteger(L, 13)), 0, 255);
            override_b = std::clamp(static_cast<int>(luaL_checkinteger(L, 14)), 0, 255);
            base_color = Gdiplus::Color(
                255,
                static_cast<BYTE>(override_r),
                static_cast<BYTE>(override_g),
                static_cast<BYTE>(override_b)
            );
        }

        const int frames =
            kill ? 18 : 14;
        const int fps = 60;

        const int duration_ms =
            static_cast<int>(
                std::ceil(
                    static_cast<double>(frames) *
                    1000.0 /
                    static_cast<double>(fps)
                )
            );

        const int columns = 4;
        const int rows =
            (frames + columns - 1) /
            columns;

        const float reticle_diameter =
            std::max(
                reticle_w,
                reticle_h
            );

        const float base_radius =
            reticle_diameter * 0.5f +
            padding;

        const float combo_bloom =
            static_cast<float>(
                std::max(
                    0,
                    combo - 1
                )
            ) * 5.0f;

        const float max_motion =
            motion + combo_bloom;

        const float max_radius =
            base_radius +
            max_motion +
            18.0f;

        const float safety_margin =
            arm_len +
            std::max(
                5.0f,
                thickness * 2.75f
            ) +
            4.0f;

        int frame_size =
            static_cast<int>(
                std::ceil(
                    (
                        max_radius +
                        safety_margin
                    ) * 2.0f
                )
            );

        frame_size =
            std::clamp(
                frame_size,
                32,
                384
            );

        Gdiplus::Bitmap sheet(
            frame_size * columns,
            frame_size * rows,
            PixelFormat32bppARGB
        );

        if(sheet.GetLastStatus() != Gdiplus::Ok) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return luaL_error(
                L,
                "could not create hitmarker FX sheet"
            );
        }

        Gdiplus::Graphics graphics(&sheet);

        graphics.SetCompositingMode(
            Gdiplus::CompositingModeSourceCopy
        );

        graphics.Clear(
            Gdiplus::Color(
                0,
                0,
                0,
                0
            )
        );

        graphics.SetCompositingMode(
            Gdiplus::CompositingModeSourceOver
        );

        graphics.SetSmoothingMode(
            Gdiplus::SmoothingModeAntiAlias
        );

        graphics.SetPixelOffsetMode(
            Gdiplus::PixelOffsetModeHighQuality
        );

        for(int frame = 0;
            frame < frames;
            ++frame) {

            const float t =
                frames > 1
                    ? static_cast<float>(frame) /
                      static_cast<float>(frames - 1)
                    : 1.0f;

            const float center_x =
                static_cast<float>(
                    (frame % columns) *
                    frame_size
                ) +
                static_cast<float>(
                    frame_size
                ) * 0.5f;

            const float center_y =
                static_cast<float>(
                    (frame / columns) *
                    frame_size
                ) +
                static_cast<float>(
                    frame_size
                ) * 0.5f;

            if(!kill) {

                constexpr float pop_end = 0.22f;
                constexpr float contract_end = 0.78f;
                constexpr float fade_start = 0.30f;
                constexpr float fade_end = 0.92f;

                float radius = base_radius;
                float motion_progress = 0.0f;

                if(t <= pop_end) {
                    const float p =
                        std::clamp(
                            t / pop_end,
                            0.0f,
                            1.0f
                        );

                    motion_progress =
                        hm_ease_out(p);

                    radius =
                        base_radius +
                        max_motion *
                        motion_progress;
                }
                else if(t <= contract_end) {
                    const float p =
                        hm_smooth(
                            (t - pop_end) /
                            (contract_end - pop_end)
                        );

                    motion_progress =
                        1.0f - p;

                    radius =
                        base_radius +
                        max_motion *
                        motion_progress;
                }
                else {
                    radius =
                        base_radius;
                    motion_progress =
                        0.0f;
                }

                radius =
                    std::max(
                        base_radius,
                        radius
                    );

                float alpha = 1.0f;

                if(t > fade_start) {
                    alpha =
                        1.0f -
                        hm_smooth(
                            (t - fade_start) /
                            (fade_end - fade_start)
                        );
                }

                if(t >= fade_end) {
                    alpha = 0.0f;
                }

                if(combo >= 2) {

                    const float pair_half_offset =
                        std::max(
                            2.05f,
                            thickness * 0.90f
                        );

                    hm_draw_paired_arms(
                        graphics,
                        center_x,
                        center_y,
                        radius,
                        arm_len,
                        thickness,
                        pair_half_offset,
                        hm_alpha(
                            base_color,
                            alpha
                        )
                    );
                }
                else {

                    hm_draw_arms(
                        graphics,
                        center_x,
                        center_y,
                        radius,
                        arm_len,
                        thickness,
                        hm_alpha(
                            base_color,
                            alpha
                        )
                    );
                }

            }
            else {

                constexpr float initial_pop_end =
                    0.14f;

                float expansion = 0.0f;

                if(t <= initial_pop_end) {
                    const float p =
                        t /
                        initial_pop_end;

                    expansion =
                        max_motion *
                        0.30f *
                        p * p;
                }
                else {
                    const float p =
                        hm_ease_out(
                            (t - initial_pop_end) /
                            (1.0f - initial_pop_end)
                        );

                    expansion =
                        max_motion *
                        (
                            0.30f +
                            0.70f * p
                        );
                }

                const float radius =
                    base_radius +
                    expansion;

                const float alpha =
                    t <= 0.10f
                        ? 1.0f
                        : 1.0f -
                          hm_smooth(
                              (t - 0.10f) /
                              0.90f
                          );

                hm_draw_arms(
                    graphics,
                    center_x,
                    center_y,
                    radius,
                    arm_len,
                    thickness,
                    hm_alpha(
                        base_color,
                        alpha
                    )
                );

                const int trail_count =
                    combo >= 3
                        ? 3
                        : 2;

                for(int trail = 1;
                    trail <= trail_count;
                    ++trail) {

                    const float lag =
                        0.055f *
                        static_cast<float>(
                            trail
                        );

                    const float delayed_t =
                        std::max(
                            0.0f,
                            t - lag
                        );

                    float trail_expansion =
                        0.0f;

                    if(delayed_t <= initial_pop_end) {
                        const float p =
                            delayed_t /
                            initial_pop_end;

                        trail_expansion =
                            max_motion *
                            0.30f *
                            p * p;
                    }
                    else {
                        const float p =
                            hm_ease_out(
                                (
                                    delayed_t -
                                    initial_pop_end
                                ) /
                                (
                                    1.0f -
                                    initial_pop_end
                                )
                            );

                        trail_expansion =
                            max_motion *
                            (
                                0.30f +
                                0.70f * p
                            );
                    }

                    const float trail_radius =
                        base_radius +
                        trail_expansion +
                        static_cast<float>(
                            trail
                        ) * 3.0f;

                    const float trail_alpha =
                        alpha *
                        std::max(
                            0.14f,
                            0.46f -
                            static_cast<float>(
                                trail - 1
                            ) * 0.09f
                        );

                    hm_draw_arms(
                        graphics,
                        center_x,
                        center_y,
                        trail_radius,
                        arm_len,
                        thickness,
                        hm_alpha(
                            base_color,
                            trail_alpha
                        )
                    );
                }

            }
        }

        auto &store =
            Runtime::instance().store();

        std::filesystem::path cache_dir =
            store.data_root() /
            ".opticcompat" /
            "generated_hitmarkers";

        std::error_code ec;

        std::filesystem::create_directories(
            cache_dir,
            ec
        );

        if(ec) {
            Gdiplus::GdiplusShutdown(
                gdiplus_token
            );

            return luaL_error(
                L,
                "could not create hitmarker FX cache directory"
            );
        }

        std::uint64_t hash =
            fnv1a_path_hash(
                source_path.wstring()
            );

        std::error_code time_ec;

        const auto source_time =
            std::filesystem::last_write_time(
                source_path,
                time_ec
            );

        if(!time_ec) {
            const auto stamp =
                static_cast<std::uint64_t>(
                    source_time
                        .time_since_epoch()
                        .count()
                );

            hash ^=
                stamp +
                0x9E3779B97F4A7C15ULL +
                (hash << 6U) +
                (hash >> 2U);
        }

        const int rw =
            static_cast<int>(
                std::lround(
                    reticle_w
                )
            );

        const int rh =
            static_cast<int>(
                std::lround(
                    reticle_h
                )
            );

        const int al =
            static_cast<int>(
                std::lround(
                    arm_len * 10.0f
                )
            );

        const int at =
            static_cast<int>(
                std::lround(
                    thickness * 10.0f
                )
            );

        const int bp =
            static_cast<int>(
                std::lround(
                    padding * 10.0f
                )
            );

        const int mo =
            static_cast<int>(
                std::lround(
                    motion * 10.0f
                )
            );

        const std::wstring name =
            L"hfx_mixv8_native_status_" +
            std::to_wstring(hash) +
            L"_e" +
            std::to_wstring(effect_kind) +
            L"_c" +
            std::to_wstring(combo) +
            L"_" +
            std::to_wstring(rw) +
            L"x" +
            std::to_wstring(rh) +
            L"_l" +
            std::to_wstring(al) +
            L"_t" +
            std::to_wstring(at) +
            L"_p" +
            std::to_wstring(bp) +
            L"_m" +
            std::to_wstring(mo) +
            L"_rgb" +
            std::to_wstring(override_r) + L"-" +
            std::to_wstring(override_g) + L"-" +
            std::to_wstring(override_b) +
            L".png";

        const auto generated_path =
            cache_dir /
            name;

        CLSID png{};

        if(!get_png_encoder_clsid(
                png
            ) ||
           sheet.Save(
                generated_path.c_str(),
                &png,
                nullptr
            ) != Gdiplus::Ok) {

            Gdiplus::GdiplusShutdown(
                gdiplus_token
            );

            return luaL_error(
                L,
                "could not save hitmarker FX spritesheet"
            );
        }

        Gdiplus::GdiplusShutdown(
            gdiplus_token
        );

        try {
            const auto handle =
                store.create_sprite(
                    generated_path,
                    frame_size,
                    frame_size,
                    static_cast<std::size_t>(
                        rows
                    ),
                    static_cast<std::size_t>(
                        columns
                    ),
                    static_cast<std::size_t>(
                        frames
                    ),
                    static_cast<std::size_t>(
                        fps
                    )
                );

            lua_pushinteger(
                L,
                static_cast<lua_Integer>(
                    handle
                )
            );

            lua_pushinteger(
                L,
                frame_size
            );

            lua_pushinteger(
                L,
                frame_size
            );

            lua_pushinteger(
                L,
                duration_ms
            );

            return 4;
        }
        catch(const std::exception &e) {
            return luaL_error(
                L,
                "create_procedural_hitmarker_fx failed: %s",
                e.what()
            );
        }
    }

    static int create_damage_number_sprite(lua_State *L) {
        const int argc = lua_gettop(L);
        if(argc != 3 && argc != 6) {
            return luaL_error(
                L,
                "invalid number of arguments in create_damage_number_sprite"
            );
        }

        const int damage_points =
            std::clamp(
                static_cast<int>(std::lround(luaL_checknumber(L, 1))),
                0,
                9999
            );

        bool critical = false;
        if(lua_isboolean(L, 2)) {
            critical = lua_toboolean(L, 2) != 0;
        }
        else {
            critical = luaL_checkinteger(L, 2) != 0;
        }

        const float scale =
            std::clamp(
                static_cast<float>(luaL_checknumber(L, 3)),
                0.50f,
                3.00f
            );

        const int width =
            std::clamp(
                static_cast<int>(std::lround(112.0f * scale)),
                64,
                336
            );

        const int height =
            std::clamp(
                static_cast<int>(std::lround(42.0f * scale)),
                28,
                126
            );

        Gdiplus::GdiplusStartupInput startup_input;
        ULONG_PTR gdiplus_token = 0;

        if(Gdiplus::GdiplusStartup(
                &gdiplus_token,
                &startup_input,
                nullptr
            ) != Gdiplus::Ok) {
            return luaL_error(
                L,
                "GDI+ initialization failed in create_damage_number_sprite"
            );
        }

        Gdiplus::Bitmap bitmap(
            width,
            height,
            PixelFormat32bppARGB
        );

        if(bitmap.GetLastStatus() != Gdiplus::Ok) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return luaL_error(
                L,
                "could not create damage number bitmap"
            );
        }

        Gdiplus::Graphics graphics(&bitmap);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

        const std::wstring text =
            std::to_wstring(damage_points);

        Gdiplus::FontFamily font_family(L"Arial");
        const float font_size =
            std::max(16.0f, 23.0f * scale);

        Gdiplus::Font font(
            &font_family,
            font_size,
            Gdiplus::FontStyleBold,
            Gdiplus::UnitPixel
        );

        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        const Gdiplus::RectF layout(
            0.0f,
            0.0f,
            static_cast<float>(width),
            static_cast<float>(height)
        );

        Gdiplus::SolidBrush shadow(
            Gdiplus::Color(215, 0, 0, 0)
        );

        const float outline =
            std::max(1.0f, 1.35f * scale);

        const Gdiplus::PointF offsets[4] = {
            {-outline, 0.0f},
            { outline, 0.0f},
            {0.0f, -outline},
            {0.0f,  outline}
        };

        for(const auto &offset : offsets) {
            Gdiplus::RectF shadow_layout = layout;
            shadow_layout.X += offset.X;
            shadow_layout.Y += offset.Y;

            graphics.DrawString(
                text.c_str(),
                -1,
                &font,
                shadow_layout,
                &format,
                &shadow
            );
        }

        int color_r = critical ? 255 : 255;
        int color_g = critical ? 72 : 255;
        int color_b = critical ? 72 : 255;

        if(argc == 6) {
            color_r = std::clamp(static_cast<int>(luaL_checkinteger(L, 4)), 0, 255);
            color_g = std::clamp(static_cast<int>(luaL_checkinteger(L, 5)), 0, 255);
            color_b = std::clamp(static_cast<int>(luaL_checkinteger(L, 6)), 0, 255);
        }

        const Gdiplus::Color number_color(
            255,
            static_cast<BYTE>(color_r),
            static_cast<BYTE>(color_g),
            static_cast<BYTE>(color_b)
        );

        Gdiplus::SolidBrush fill(number_color);

        graphics.DrawString(
            text.c_str(),
            -1,
            &font,
            layout,
            &format,
            &fill
        );

        auto &store = Runtime::instance().store();

        std::filesystem::path cache_dir =
            store.data_root() /
            ".opticcompat" /
            "generated_hitmarkers";

        std::error_code ec;
        std::filesystem::create_directories(cache_dir, ec);

        if(ec) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return luaL_error(
                L,
                "could not create damage-number cache directory"
            );
        }

        const int scale_key =
            static_cast<int>(std::lround(scale * 100.0f));

        const std::wstring file_name =
            L"damage_v3_palette_" +
            std::to_wstring(damage_points) +
            L"_c" +
            std::to_wstring(critical ? 1 : 0) +
            L"_s" +
            std::to_wstring(scale_key) +
            L"_rgb" +
            std::to_wstring(color_r) + L"-" +
            std::to_wstring(color_g) + L"-" +
            std::to_wstring(color_b) +
            L".png";

        const auto generated_path =
            cache_dir / file_name;

        CLSID png_clsid{};

        if(!get_png_encoder_clsid(png_clsid) ||
           bitmap.Save(
               generated_path.c_str(),
               &png_clsid,
               nullptr
           ) != Gdiplus::Ok) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return luaL_error(
                L,
                "could not save damage-number PNG"
            );
        }

        Gdiplus::GdiplusShutdown(gdiplus_token);

        try {
            const auto handle =
                store.create_sprite(
                    generated_path,
                    width,
                    height
                );

            lua_pushinteger(
                L,
                static_cast<lua_Integer>(handle)
            );
            lua_pushinteger(L, width);
            lua_pushinteger(L, height);
            return 3;
        }
        catch(const std::exception &e) {
            return luaL_error(
                L,
                "create_damage_number_sprite failed: %s",
                e.what()
            );
        }
    }

    static int create_procedural_headshot_medal(lua_State *L) {
        if(lua_gettop(L) != 1) {
            return luaL_error(
                L,
                "invalid number of arguments in create_procedural_headshot_medal"
            );
        }

        const int size =
            std::clamp(
                static_cast<int>(std::lround(luaL_checknumber(L, 1))),
                32,
                512
            );

        Gdiplus::GdiplusStartupInput startup_input;
        ULONG_PTR gdiplus_token = 0;

        if(Gdiplus::GdiplusStartup(
                &gdiplus_token,
                &startup_input,
                nullptr
            ) != Gdiplus::Ok) {
            return luaL_error(
                L,
                "GDI+ initialization failed in create_procedural_headshot_medal"
            );
        }

        Gdiplus::Bitmap bitmap(
            size,
            size,
            PixelFormat32bppARGB
        );

        if(bitmap.GetLastStatus() != Gdiplus::Ok) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return luaL_error(
                L,
                "could not create HeadShot medal bitmap"
            );
        }

        Gdiplus::Graphics graphics(&bitmap);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

        const float s = static_cast<float>(size);
        const float cx = s * 0.5f;
        const float cy = s * 0.5f;
        const float outer_r = s * 0.365f;
        const float line = std::max(1.5f, s * 0.026f);

        const Gdiplus::Color white(235, 245, 248, 255);
        const Gdiplus::Color gold(255, 255, 194, 62);
        const Gdiplus::Color red(255, 255, 76, 76);
        const Gdiplus::Color dark(205, 7, 12, 18);

        Gdiplus::SolidBrush dark_fill(dark);
        graphics.FillEllipse(
            &dark_fill,
            cx - outer_r,
            cy - outer_r,
            outer_r * 2.0f,
            outer_r * 2.0f
        );

        Gdiplus::Pen outer_pen(gold, line * 1.20f);
        outer_pen.SetStartCap(Gdiplus::LineCapRound);
        outer_pen.SetEndCap(Gdiplus::LineCapRound);

        graphics.DrawEllipse(
            &outer_pen,
            cx - outer_r,
            cy - outer_r,
            outer_r * 2.0f,
            outer_r * 2.0f
        );

        Gdiplus::Pen inner_pen(white, line * 0.72f);
        const float inner_r = outer_r * 0.72f;
        graphics.DrawEllipse(
            &inner_pen,
            cx - inner_r,
            cy - inner_r,
            inner_r * 2.0f,
            inner_r * 2.0f
        );

        const float tick_outer = outer_r * 1.18f;
        const float tick_inner = outer_r * 0.83f;
        Gdiplus::Pen tick_pen(white, line * 0.85f);
        tick_pen.SetStartCap(Gdiplus::LineCapRound);
        tick_pen.SetEndCap(Gdiplus::LineCapRound);

        graphics.DrawLine(&tick_pen, cx, cy - tick_outer, cx, cy - tick_inner);
        graphics.DrawLine(&tick_pen, cx, cy + tick_inner, cx, cy + tick_outer);
        graphics.DrawLine(&tick_pen, cx - tick_outer, cy, cx - tick_inner, cy);
        graphics.DrawLine(&tick_pen, cx + tick_inner, cy, cx + tick_outer, cy);

        const float head_r = s * 0.105f;
        const float head_y = cy - s * 0.075f;
        Gdiplus::SolidBrush head_fill(white);
        graphics.FillEllipse(
            &head_fill,
            cx - head_r,
            head_y - head_r,
            head_r * 2.0f,
            head_r * 2.0f
        );

        Gdiplus::Pen shoulder_pen(white, line * 1.35f);
        shoulder_pen.SetStartCap(Gdiplus::LineCapRound);
        shoulder_pen.SetEndCap(Gdiplus::LineCapRound);

        const float shoulder_w = s * 0.27f;
        const float shoulder_y = cy + s * 0.15f;
        graphics.DrawArc(
            &shoulder_pen,
            cx - shoulder_w,
            shoulder_y - s * 0.14f,
            shoulder_w * 2.0f,
            s * 0.24f,
            200.0f,
            140.0f
        );

        const float crit_r = std::max(2.0f, s * 0.035f);
        Gdiplus::SolidBrush crit_fill(red);
        graphics.FillEllipse(
            &crit_fill,
            cx - crit_r,
            head_y - crit_r,
            crit_r * 2.0f,
            crit_r * 2.0f
        );

        auto &store = Runtime::instance().store();

        std::filesystem::path cache_dir =
            store.data_root() /
            ".opticcompat" /
            "generated_hitmarkers";

        std::error_code ec;
        std::filesystem::create_directories(cache_dir, ec);

        if(ec) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return luaL_error(
                L,
                "could not create HeadShot medal cache directory"
            );
        }

        const auto generated_path =
            cache_dir /
            (L"headshot_medal_v1_" + std::to_wstring(size) + L".png");

        CLSID png_clsid{};

        if(!get_png_encoder_clsid(png_clsid) ||
           bitmap.Save(
               generated_path.c_str(),
               &png_clsid,
               nullptr
           ) != Gdiplus::Ok) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return luaL_error(
                L,
                "could not save HeadShot medal PNG"
            );
        }

        Gdiplus::GdiplusShutdown(gdiplus_token);

        try {
            const auto handle =
                store.create_sprite(
                    generated_path,
                    size,
                    size
                );

            lua_pushinteger(
                L,
                static_cast<lua_Integer>(handle)
            );
            lua_pushinteger(L, size);
            lua_pushinteger(L, size);
            return 3;
        }
        catch(const std::exception &e) {
            return luaL_error(
                L,
                "create_procedural_headshot_medal failed: %s",
                e.what()
            );
        }
    }

    static int create_sprite(lua_State *L) {
        const int args = lua_gettop(L);
        if(args != 3 && args != 7) return luaL_error(L, "invalid number of arguments in create_sprite");
        std::filesystem::path path;
        if(!resolve_existing(L, 1, path)) return 0;
        const int width = static_cast<int>(luaL_checknumber(L, 2));
        const int height = static_cast<int>(luaL_checknumber(L, 3));
        if(width <= 0 || height <= 0) return luaL_error(L, "invalid sprite dimensions");

        std::size_t rows = 1, columns = 1, frames = 1, fps = 0;
        if(args == 7) {
            const lua_Integer r = luaL_checkinteger(L, 4);
            const lua_Integer c = luaL_checkinteger(L, 5);
            const lua_Integer f = luaL_checkinteger(L, 6);
            const lua_Integer rate = luaL_checkinteger(L, 7);
            if(r <= 0 || c <= 0 || f <= 0 || rate < 0) return luaL_error(L, "invalid sprite sheet parameters");
            rows = static_cast<std::size_t>(r);
            columns = static_cast<std::size_t>(c);
            frames = static_cast<std::size_t>(f);
            fps = static_cast<std::size_t>(rate);
        }

        try {
            const auto handle = Runtime::instance().store().create_sprite(path, width, height, rows, columns, frames, fps);
            lua_pushinteger(L, static_cast<lua_Integer>(handle));
            return 1;
        }
        catch(const std::exception &e) {
            return luaL_error(L, "create_sprite failed: %s", e.what());
        }
    }

    static int create_render_queue(lua_State *L) {
        const int args = lua_gettop(L);
        if(args < 6 || args > 9) return luaL_error(L, "invalid number of arguments in create_render_queue");
        SpriteState state{};
        state.x = static_cast<float>(luaL_checknumber(L, 1));
        state.y = static_cast<float>(luaL_checknumber(L, 2));
        state.opacity = static_cast<float>(luaL_checknumber(L, 3));
        state.rotation = static_cast<float>(luaL_checknumber(L, 4));
        const long duration = static_cast<long>(luaL_checknumber(L, 5));
        const int max = static_cast<int>(luaL_checknumber(L, 6));
        if(max < 0) return luaL_error(L, "invalid maximum render count");

        auto &store = Runtime::instance().store();
        const auto handle = store.create_render_queue(state, state.rotation, static_cast<std::size_t>(max), duration, false);
        auto *queue = store.render_queue(handle);
        if(args >= 7) {
            auto *anim = store.animation(check_handle(L, 7, "fade-in animation"));
            if(!anim) return luaL_error(L, "invalid fade-in animation handle");
            queue->fade_in = *anim;
        }
        if(args >= 8) {
            auto *anim = store.animation(check_handle(L, 8, "fade-out animation"));
            if(!anim) return luaL_error(L, "invalid fade-out animation handle");
            queue->fade_out = *anim;
        }
        if(args == 9) {
            auto *anim = store.animation(check_handle(L, 9, "slide animation"));
            if(!anim) return luaL_error(L, "invalid slide animation handle");
            queue->slide = *anim;
        }
        lua_pushinteger(L, static_cast<lua_Integer>(handle));
        return 1;
    }

    static int render_sprite(lua_State *L) {
        const int args = lua_gettop(L);
        auto &store = Runtime::instance().store();
        if(args == 2) {
            try {
                const auto sprite_handle = check_handle(L, 1, "sprite");
                const auto queue_handle = check_handle(L, 2, "render queue");

                store.enqueue_sprite(sprite_handle, queue_handle);
                return 0;
            }
            catch(const std::exception &e) {
                return luaL_error(L, "render_sprite failed: %s", e.what());
            }
        }
        if(args != 6 && args != 7 && args != 8) return luaL_error(L, "invalid number of arguments in render_sprite");

        const auto sprite = check_handle(L, 1, "sprite");
        SpriteState state{};
        state.x = static_cast<float>(luaL_checknumber(L, 2));
        state.y = static_cast<float>(luaL_checknumber(L, 3));
        state.opacity = static_cast<float>(luaL_checknumber(L, 4));
        state.rotation = degrees_to_radians(static_cast<float>(luaL_checknumber(L, 5)));
        const long duration = static_cast<long>(luaL_checknumber(L, 6));
        const Animation *fade_in = nullptr;
        const Animation *fade_out = nullptr;
        if(args >= 7) {
            fade_in = store.animation(check_handle(L, 7, "fade-in animation"));
            if(!fade_in) return luaL_error(L, "invalid fade-in animation handle");
        }
        if(args == 8) {
            fade_out = store.animation(check_handle(L, 8, "fade-out animation"));
            if(!fade_out) return luaL_error(L, "invalid fade-out animation handle");
        }
        try {

            store.render_direct(sprite, state, duration, fade_in, fade_out);
            return 0;
        }
        catch(const std::exception &e) {
            return luaL_error(L, "render_sprite failed: %s", e.what());
        }
    }

    static int clear_render_queue(lua_State *L) {
        if(lua_gettop(L) != 1) return luaL_error(L, "invalid number of arguments in clear_render_queue");
        try {
            Runtime::instance().store().clear_render_queue(check_handle(L, 1, "render queue"));
            return 0;
        }
        catch(const std::exception &e) {
            return luaL_error(L, "clear_render_queue failed: %s", e.what());
        }
    }

    static int create_sound(lua_State *L) {
        if(lua_gettop(L) != 1) return luaL_error(L, "invalid number of arguments in create_sound");
        std::filesystem::path path;
        if(!resolve_existing(L, 1, path)) return 0;
        const auto handle = Runtime::instance().store().create_sound(path);
        lua_pushinteger(L, static_cast<lua_Integer>(handle));
        return 1;
    }

    static int create_audio_engine(lua_State *L) {
        if(lua_gettop(L) != 0) return luaL_error(L, "invalid number of arguments in create_audio_engine");
        const auto handle = Runtime::instance().store().create_audio_engine();
        lua_pushinteger(L, static_cast<lua_Integer>(handle));
        return 1;
    }

    static int play_sound(lua_State *L) {
        const int args = lua_gettop(L);
        if(args != 2 && args != 3) return luaL_error(L, "invalid number of arguments in play_sound");
        const auto sound = check_handle(L, 1, "sound");
        const auto engine = check_handle(L, 2, "audio engine");
        const bool no_enqueue = args == 3 && lua_toboolean(L, 3) != 0;
        try {
            Runtime::instance().store().play_sound(sound, engine, no_enqueue);
            return 1;
        }
        catch(const std::exception &e) {
            return luaL_error(L, "play_sound failed: %s", e.what());
        }
    }

    static int clear_audio_engine(lua_State *L) {
        if(lua_gettop(L) != 1) return luaL_error(L, "invalid number of arguments in clear_audio_engine");
        try {
            Runtime::instance().store().clear_audio_engine(check_handle(L, 1, "audio engine"));
            return 0;
        }
        catch(const std::exception &e) {
            return luaL_error(L, "clear_audio_engine failed: %s", e.what());
        }
    }

    static int set_audio_engine_gain(lua_State *L) {
        if(lua_gettop(L) != 2) return luaL_error(L, "invalid number of arguments in set_audio_engine_gain");
        try {
            Runtime::instance().store().set_audio_engine_gain(check_handle(L, 1, "audio engine"),
                                                               static_cast<int>(luaL_checkinteger(L, 2)));
            return 0;
        }
        catch(const std::exception &e) {
            return luaL_error(L, "set_audio_engine_gain failed: %s", e.what());
        }
    }

    static int set_timestamp(lua_State *L) {
        if(lua_gettop(L) != 0) return luaL_error(L, "invalid number of arguments in set_timestamp");
        lua_pushinteger(L, static_cast<lua_Integer>(Runtime::instance().set_timestamp()));
        return 1;
    }

    static int elapsed_ms(lua_State *L) {
        if(lua_gettop(L) != 1) return luaL_error(L, "invalid number of arguments in get_elapsed_milliseconds");
        auto value = Runtime::instance().elapsed_milliseconds(check_handle(L, 1, "timestamp"));
        if(!value) return luaL_error(L, "invalid timestamp handle");
        lua_pushinteger(L, static_cast<lua_Integer>(*value));
        return 1;
    }

    static int elapsed_seconds(lua_State *L) {
        if(lua_gettop(L) != 1) return luaL_error(L, "invalid number of arguments in get_elapsed_seconds");
        auto value = Runtime::instance().elapsed_seconds(check_handle(L, 1, "timestamp"));
        if(!value) return luaL_error(L, "invalid timestamp handle");
        lua_pushinteger(L, static_cast<lua_Integer>(*value));
        return 1;
    }

    static int now_milliseconds(lua_State *L) {
        if(lua_gettop(L) != 0) {
            return luaL_error(
                L,
                "invalid number of arguments in get_milliseconds"
            );
        }

        const auto value =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count();

        lua_pushinteger(
            L,
            static_cast<lua_Integer>(value)
        );
        return 1;
    }

    int set_callback(lua_State *L) {
        if(lua_gettop(L) != 2) return luaL_error(L, "invalid number of arguments in harmony.set_callback");
        const char *type = luaL_checkstring(L, 1);
        const char *function = luaL_checkstring(L, 2);
        if(!Runtime::instance().set_callback(type ? type : "", function ? function : "")) {
            return luaL_error(L, "OpticCompat could not install callback '%s'", type ? type : "?");
        }
        return 0;
    }

    static void set_function(lua_State *L, const char *name, lua_CFunction function) {
        lua_pushcfunction(L, function);
        lua_setfield(L, -2, name);
    }

    void push_optic_table(lua_State *L) {
        lua_newtable(L);
        set_function(L, "create_animation", create_animation);
        set_function(L, "set_animation_property", set_animation_property);
        set_function(L, "create_sprite", create_sprite);
        set_function(L, "create_procedural_hitmarker", create_procedural_hitmarker);
        set_function(L, "create_procedural_hit_flash", create_procedural_hit_flash);
        set_function(L, "create_procedural_hitmarker_fx", create_procedural_hitmarker_fx);
        set_function(L, "create_damage_number_sprite", create_damage_number_sprite);
        set_function(L, "create_procedural_headshot_medal", create_procedural_headshot_medal);
        set_function(L, "native_damage_probe_available", native_damage_probe_available);
        set_function(L, "poll_native_damage_event", poll_native_damage_event);
        set_function(L, "clear_native_damage_events", clear_native_damage_events);
        set_function(L, "get_image_content_bounds", get_image_content_bounds);
        set_function(L, "get_halo_bitmap_alpha_bounds", get_halo_bitmap_alpha_bounds);
        set_function(L, "get_halo_texture_alpha_bounds", get_halo_texture_alpha_bounds);
        set_function(L, "create_render_queue", create_render_queue);
        set_function(L, "render_sprite", render_sprite);
        set_function(L, "clear_render_queue", clear_render_queue);
        set_function(L, "create_sound", create_sound);
        set_function(L, "create_audio_engine", create_audio_engine);
        set_function(L, "play_sound", play_sound);
        set_function(L, "clear_audio_engine", clear_audio_engine);
        set_function(L, "set_audio_engine_gain", set_audio_engine_gain);
    }

    void push_time_table(lua_State *L) {
        lua_newtable(L);
        set_function(L, "set_timestamp", set_timestamp);
        set_function(L, "get_elapsed_milliseconds", elapsed_ms);
        set_function(L, "get_elapsed_seconds", elapsed_seconds);
        set_function(L, "get_milliseconds", now_milliseconds);
    }
}
