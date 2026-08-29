#pragma once
#include "common.hpp"

namespace OpticCompat::LuaApi {
    int set_callback(lua_State *state);
    void push_optic_table(lua_State *state);
    void push_time_table(lua_State *state);
}
