#include "common.hpp"
#include "runtime.hpp"
#include "lua_api.hpp"
#include "log.hpp"

#if defined(_M_IX86)
#pragma comment(linker, "/EXPORT:luaopen_mods_harmony=_luaopen_mods_harmony")
#endif

namespace {
    int opticcompat_guard_gc(lua_State *L) {
        OpticCompat::Runtime::instance().detach(L);
        return 0;
    }

    void install_guard(lua_State *L) {
        lua_getfield(L, LUA_REGISTRYINDEX, "OpticCompat.guard");
        const bool exists = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if(exists) return;

        (void)lua_newuserdatauv(L, 1, 0);
        if(luaL_newmetatable(L, "OpticCompat.Guard")) {
            lua_pushcfunction(L, opticcompat_guard_gc);
            lua_setfield(L, -2, "__gc");
        }
        lua_setmetatable(L, -2);
        lua_setfield(L, LUA_REGISTRYINDEX, "OpticCompat.guard");
    }
}

extern "C" int luaopen_mods_harmony(lua_State *L) {
    if(!OpticCompat::Runtime::instance().attach(L)) {
        return luaL_error(L, "OpticCompat: initialization failed");
    }
    install_guard(L);

    lua_newtable(L);
    lua_pushcfunction(L, OpticCompat::LuaApi::set_callback);
    lua_setfield(L, -2, "set_callback");

    OpticCompat::LuaApi::push_optic_table(L);
    lua_setfield(L, -2, "optic");

    OpticCompat::LuaApi::push_time_table(L);
    lua_setfield(L, -2, "time");

    lua_pushstring(L, OpticCompat::kVersion);
    lua_setfield(L, -2, "_opticcompat_version");
    return 1;
}
