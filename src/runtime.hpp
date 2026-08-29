#pragma once
#include "common.hpp"
#include "optic.hpp"
#include "renderer.hpp"
#include "multiplayer.hpp"

namespace OpticCompat {
    class Runtime {
    public:
        static Runtime &instance();

        bool attach(lua_State *state);
        void detach(lua_State *state) noexcept;
        lua_State *state() const noexcept { return state_; }
        OpticStore &store() noexcept { return store_; }
        Renderer &renderer() noexcept { return renderer_; }
        MultiplayerHooks &multiplayer() noexcept { return multiplayer_; }

        bool set_callback(std::string_view type, std::string function_name);
        bool dispatch_multiplayer_sound(std::uint32_t value) noexcept;
        void dispatch_multiplayer_event(std::uint32_t value, std::uint32_t local_id,
                                        std::uint32_t killer_id, std::uint32_t victim_id) noexcept;
        void on_end_scene(IDirect3DDevice9 *device) noexcept;

        std::size_t set_timestamp();
        std::optional<long long> elapsed_milliseconds(std::size_t handle) const noexcept;
        std::optional<long long> elapsed_seconds(std::size_t handle) const noexcept;

        bool resolve_data_path(std::string_view relative, std::filesystem::path &out) const;
        const std::filesystem::path &data_root() const noexcept { return data_root_; }

    private:
        Runtime() = default;
        std::string read_global_string(lua_State *state, const char *name) const;
        static bool path_starts_with(const std::filesystem::path &child, const std::filesystem::path &base);
        bool call_lua_callback(const std::string &name, int nargs, int nresults) noexcept;

        lua_State *state_ = nullptr;
        std::string callback_event_;
        std::string callback_sound_;
        std::filesystem::path data_root_;
        std::vector<std::chrono::steady_clock::time_point> timestamps_;
        OpticStore store_;
        Renderer renderer_;
        MultiplayerHooks multiplayer_;
    };
}
