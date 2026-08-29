#include "runtime.hpp"
#include "log.hpp"

namespace OpticCompat {
    Runtime &Runtime::instance() {
        static Runtime runtime;
        return runtime;
    }

    std::string Runtime::read_global_string(lua_State *state, const char *name) const {
        if(!state || !name) return {};
        lua_getglobal(state, name);
        const char *value = lua_tostring(state, -1);
        std::string result = value ? value : "";
        lua_pop(state, 1);
        return result;
    }

    bool Runtime::attach(lua_State *state) {
        if(!state) return false;
        if(state_ == state) return true;
        if(state_ && state_ != state) {
            log_line("Refusing a second simultaneous Lua state.");
            return false;
        }

        std::string script_name = read_global_string(state, "script_name");
        std::string script_type = read_global_string(state, "script_type");
        if(script_name.empty()) script_name = "optic.lua";
        if(script_type.empty()) script_type = "global";

        std::filesystem::path script_file(script_name);
        std::string stem = script_file.stem().string();
        if(stem.empty()) stem = "optic";

        // Chimera stores Lua scripts/data under its profile path, which may be
        // different from the Halo installation directory. In the hardened
        // branch, package.path is set to:
        //
        //   <chimera profile>\lua\modules\?.lua
        //
        // Derive the real Lua root from that value so OpticCompat resolves the
        // exact same data directory used by Chimera's read_file/file_exists.
        std::filesystem::path lua_root;
        lua_getglobal(state, "package");
        if(lua_istable(state, -1)) {
            lua_getfield(state, -1, "path");
            const char *package_path = lua_tostring(state, -1);
            if(package_path && *package_path) {
                std::string paths(package_path);
                std::size_t start = 0;
                while(start <= paths.size()) {
                    const std::size_t end = paths.find(';', start);
                    std::string entry = paths.substr(
                        start,
                        end == std::string::npos ? std::string::npos : end - start
                    );

                    if(!entry.empty()) {
                        std::filesystem::path module_pattern(entry);
                        auto modules_dir = module_pattern.parent_path();
                        auto candidate_lua_root = modules_dir.parent_path();

                        const std::wstring modules_name = modules_dir.filename().wstring();
                        const std::wstring lua_name = candidate_lua_root.filename().wstring();

                        if(_wcsicmp(modules_name.c_str(), L"modules") == 0 &&
                           _wcsicmp(lua_name.c_str(), L"lua") == 0) {
                            lua_root = candidate_lua_root.lexically_normal();
                            break;
                        }
                    }

                    if(end == std::string::npos) break;
                    start = end + 1;
                }
            }
            lua_pop(state, 1);
        }
        lua_pop(state, 1);

        if(lua_root.empty()) {
            // Fallback kept for unusual environments where package.path was
            // modified before Harmony is required.
            lua_root = (halo_root() / L"chimera" / L"lua").lexically_normal();
            log_line("Warning: could not derive Chimera Lua profile from package.path; using fallback: %ls",
                     lua_root.c_str());
        }
        else {
            log_line("Chimera Lua profile resolved from package.path: %ls", lua_root.c_str());
        }

        data_root_ = (lua_root / L"data" /
                      std::filesystem::path(script_type) /
                      std::filesystem::path(stem)).lexically_normal();

        state_ = state;
        callback_event_.clear();
        callback_sound_.clear();
        timestamps_.clear();
        store_.reset(data_root_);
        renderer_.initialize();
        if(!renderer_.ensure_hook()) renderer_.start_retry_worker();

        log_line("OpticCompat %s attached to %s/%s. Data root: %ls",
                 kVersion, script_type.c_str(), script_name.c_str(), data_root_.c_str());
        return true;
    }

    void Runtime::detach(lua_State *state) noexcept {
        if(!state || state_ != state) return;
        callback_event_.clear();
        callback_sound_.clear();
        timestamps_.clear();
        store_.reset({});
        state_ = nullptr;
        log_line("OpticCompat detached from Lua state.");
    }

    bool Runtime::set_callback(std::string_view type, std::string function_name) {
        if(!state_ || function_name.empty()) return false;
        if(type == "multiplayer event") {
            if(!multiplayer_.install_event_hook()) return false;
            callback_event_ = std::move(function_name);
            return true;
        }
        if(type == "multiplayer sound") {
            if(!multiplayer_.install_sound_hook()) return false;
            callback_sound_ = std::move(function_name);
            return true;
        }
        return false;
    }

    bool Runtime::call_lua_callback(const std::string &name, int nargs, int nresults) noexcept {
        if(!state_ || name.empty()) return false;
        if(lua_pcall(state_, nargs, nresults, 0) != LUA_OK) {
            const char *error = lua_tostring(state_, -1);
            log_line("Lua callback '%s' failed: %s", name.c_str(), error ? error : "unknown error");
            lua_pop(state_, 1);
            return false;
        }
        return true;
    }

    bool Runtime::dispatch_multiplayer_sound(std::uint32_t value) noexcept {
        if(!state_ || callback_sound_.empty()) return true;
        const char *name = MultiplayerHooks::sound_name(value);
        if(!name) return true;

        const int base = lua_gettop(state_);
        lua_getglobal(state_, callback_sound_.c_str());
        if(!lua_isfunction(state_, -1)) {
            lua_settop(state_, base);
            return true;
        }
        lua_pushstring(state_, name);
        if(!call_lua_callback(callback_sound_, 1, 1)) {
            lua_settop(state_, base);
            return true;
        }
        const bool allow = lua_toboolean(state_, -1) != 0;
        lua_settop(state_, base);
        return allow;
    }

    void Runtime::dispatch_multiplayer_event(std::uint32_t value, std::uint32_t local_id,
                                             std::uint32_t killer_id, std::uint32_t victim_id) noexcept {
        if(!state_ || callback_event_.empty()) return;
        const char *name = MultiplayerHooks::event_name(value);
        if(!name) return;

        const int base = lua_gettop(state_);
        lua_getglobal(state_, callback_event_.c_str());
        if(!lua_isfunction(state_, -1)) {
            lua_settop(state_, base);
            return;
        }
        lua_pushstring(state_, name);
        lua_pushinteger(state_, static_cast<lua_Integer>(local_id));
        lua_pushinteger(state_, static_cast<lua_Integer>(killer_id));
        lua_pushinteger(state_, static_cast<lua_Integer>(victim_id));
        if(!call_lua_callback(callback_event_, 4, 0)) lua_settop(state_, base);
        else lua_settop(state_, base);
    }

    void Runtime::on_end_scene(IDirect3DDevice9 *device) noexcept {
        if(!state_) return;
        store_.on_end_scene(device);
    }

    std::size_t Runtime::set_timestamp() {
        timestamps_.push_back(std::chrono::steady_clock::now());
        return timestamps_.size() - 1;
    }

    std::optional<long long> Runtime::elapsed_milliseconds(std::size_t handle) const noexcept {
        if(handle >= timestamps_.size()) return std::nullopt;
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - timestamps_[handle]).count();
    }

    std::optional<long long> Runtime::elapsed_seconds(std::size_t handle) const noexcept {
        if(handle >= timestamps_.size()) return std::nullopt;
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - timestamps_[handle]).count();
    }

    bool Runtime::path_starts_with(const std::filesystem::path &child, const std::filesystem::path &base) {
        auto c = child.begin();
        auto b = base.begin();
        for(; b != base.end(); ++b, ++c) {
            if(c == child.end()) return false;
            const std::wstring cw = c->wstring();
            const std::wstring bw = b->wstring();
            if(_wcsicmp(cw.c_str(), bw.c_str()) != 0) return false;
        }
        return true;
    }

    bool Runtime::resolve_data_path(std::string_view relative, std::filesystem::path &out) const {
        if(data_root_.empty() || relative.empty()) return false;
        std::filesystem::path rel{std::string(relative)};
        if(rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) return false;
        std::error_code ec;
        auto base = std::filesystem::absolute(data_root_, ec).lexically_normal();
        if(ec) return false;
        auto candidate = std::filesystem::absolute(base / rel, ec).lexically_normal();
        if(ec || !path_starts_with(candidate, base)) return false;
        out = std::move(candidate);
        return true;
    }
}
