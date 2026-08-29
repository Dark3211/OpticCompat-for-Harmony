#pragma once
#include "common.hpp"

namespace OpticCompat {
    std::filesystem::path halo_root();
    void log_line(const char *format, ...);
}
