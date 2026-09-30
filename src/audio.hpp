#pragma once
#include "common.hpp"

namespace OpticCompat {
    struct Sound {
        std::filesystem::path path;
    };

    class AudioEngine {
    public:
        AudioEngine();
        ~AudioEngine();
        AudioEngine(const AudioEngine &) = delete;
        AudioEngine &operator=(const AudioEngine &) = delete;

        void enqueue(std::size_t sound_handle, const Sound &sound, bool no_enqueue);
        void update(const std::vector<std::unique_ptr<Sound>> &sounds) noexcept;
        bool play_immediate(std::size_t handle, const Sound &sound);
        void clear();
        void set_gain(int gain);

    private:
        struct PendingSound {
            std::size_t handle = 0;
            std::filesystem::path path;
        };

        void worker_main() noexcept;
        bool start(std::size_t handle, const std::filesystem::path &path, int gain);
        bool restart_or_start(const PendingSound &sound, int gain);
        void apply_gain(int gain);
        void close_current();
        bool current_is_playing() const;

        std::mutex mutex_;
        std::condition_variable wake_;
        std::deque<PendingSound> queue_;
        std::optional<PendingSound> immediate_;
        bool clear_requested_ = false;
        bool gain_dirty_ = false;
        bool stopping_ = false;
        int requested_gain_ = 100;
        std::thread worker_;

        std::wstring alias_;
        std::optional<std::size_t> current_handle_;
        inline static std::atomic<unsigned long> next_alias_{1};
    };
}
