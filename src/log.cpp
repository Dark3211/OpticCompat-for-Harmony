#include "log.hpp"

namespace OpticCompat {
    std::filesystem::path halo_root() {
        std::array<wchar_t, 32768> buffer{};
        const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if(len == 0 || len >= buffer.size()) {
            return std::filesystem::current_path();
        }
        return std::filesystem::path(buffer.data()).parent_path();
    }

    void log_line(const char *, ...) {
    }
}
