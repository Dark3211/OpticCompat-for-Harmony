#include "log.hpp"
#include <cstdarg>

namespace OpticCompat {
    static std::mutex g_log_mutex;

    std::filesystem::path halo_root() {
        std::array<wchar_t, 32768> buffer{};
        const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if(len == 0 || len >= buffer.size()) {
            return std::filesystem::current_path();
        }
        return std::filesystem::path(buffer.data()).parent_path();
    }

    void log_line(const char *format, ...) {
        char message[2048]{};
        va_list args;
        va_start(args, format);
        vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
        va_end(args);

        std::lock_guard<std::mutex> lock(g_log_mutex);
        std::error_code ec;
        const auto mods = halo_root() / L"mods";
        std::filesystem::create_directories(mods, ec);
        const auto path = mods / L"opticcompat.log";
        std::ofstream out(path, std::ios::app | std::ios::binary);
        if(!out) {
            return;
        }

        SYSTEMTIME st{};
        GetLocalTime(&st);
        char stamp[64]{};
        snprintf(stamp, sizeof(stamp), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        out << stamp << message << "\r\n";
    }
}
