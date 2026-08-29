#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <d3d9.h>
#include <gdiplus.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
#include "../third_party/lua/lua.h"
#include "../third_party/lua/lauxlib.h"
}

namespace OpticCompat {
    inline constexpr const char *kVersion = "0.1.0";
    inline constexpr float kPi = 3.14159265358979323846f;
    inline float degrees_to_radians(float degrees) noexcept {
        return degrees * (kPi / 180.0f);
    }
}
