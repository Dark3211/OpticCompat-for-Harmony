#pragma once
#include "common.hpp"

namespace OpticCompat {
    class MultiplayerHooks {
    public:
        bool install_event_hook();
        bool install_sound_hook();
        bool event_installed() const noexcept { return event_installed_; }
        bool sound_installed() const noexcept { return sound_installed_; }

        static const char *event_name(std::uint32_t value) noexcept;
        static const char *sound_name(std::uint32_t value) noexcept;

    private:
        bool event_installed_ = false;
        bool sound_installed_ = false;
    };
}
