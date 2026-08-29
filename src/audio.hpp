#pragma once
#include "common.hpp"

namespace OpticCompat {
    struct Sound {
        std::filesystem::path path;
    };

    class AudioEngine {
    public:
        AudioEngine() = default;
        ~AudioEngine();
        AudioEngine(const AudioEngine &) = delete;
        AudioEngine &operator=(const AudioEngine &) = delete;

        void enqueue(std::size_t sound_handle, bool no_enqueue);
        void update(const std::vector<std::unique_ptr<Sound>> &sounds);
        void clear();
        void set_gain(int gain);

    private:
        bool start(std::size_t handle, const Sound &sound);
        void close_current();
        bool current_is_playing() const;

        std::deque<std::size_t> queue_;
        std::wstring alias_;
        int gain_ = 100;
        inline static std::atomic<unsigned long> next_alias_{1};
    };
}
