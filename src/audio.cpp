#include "audio.hpp"
#include "log.hpp"
#include <objbase.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")

namespace OpticCompat {
    static std::wstring quote_mci_path(const std::filesystem::path &path) {
        std::wstring p = path.wstring();
        std::wstring escaped;
        escaped.reserve(p.size());
        for(wchar_t ch : p) {
            if(ch == L'"') continue;
            escaped.push_back(ch);
        }
        return L"\"" + escaped + L"\"";
    }

    AudioEngine::AudioEngine()
        : worker_(&AudioEngine::worker_main, this) {
    }

    AudioEngine::~AudioEngine() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            queue_.clear();
            immediate_.reset();
            clear_requested_ = true;
        }
        wake_.notify_all();

        if(worker_.joinable()) {
            worker_.join();
        }
    }

    void AudioEngine::enqueue(std::size_t sound_handle,
                              const Sound &sound,
                              bool no_enqueue) {
        PendingSound pending{sound_handle, sound.path};

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(stopping_) return;

            if(no_enqueue) {
                queue_.clear();
                immediate_ = std::move(pending);
            }
            else {
                queue_.push_back(std::move(pending));
            }
        }

        wake_.notify_one();
    }

    bool AudioEngine::start(std::size_t handle,
                            const std::filesystem::path &path,
                            int gain) {
        close_current();

        alias_ =
            L"opticcompat_" +
            std::to_wstring(next_alias_.fetch_add(1));

        const std::wstring open_cmd =
            L"open " +
            quote_mci_path(path) +
            L" type mpegvideo alias " +
            alias_;

        const MCIERROR open_error =
            mciSendStringW(
                open_cmd.c_str(),
                nullptr,
                0,
                nullptr
            );

        if(open_error != 0) {
            log_line(
                "MCI could not open sound handle %zu (error %lu).",
                handle,
                static_cast<unsigned long>(open_error)
            );
            alias_.clear();
            return false;
        }

        apply_gain(gain);

        const std::wstring play_cmd =
            L"play " + alias_ + L" from 0";

        const MCIERROR play_error =
            mciSendStringW(
                play_cmd.c_str(),
                nullptr,
                0,
                nullptr
            );

        if(play_error != 0) {
            log_line(
                "MCI could not play sound handle %zu (error %lu).",
                handle,
                static_cast<unsigned long>(play_error)
            );
            close_current();
            return false;
        }

        current_handle_ = handle;
        return true;
    }

    bool AudioEngine::restart_or_start(
        const PendingSound &sound,
        int gain
    ) {
        if(!alias_.empty() &&
           current_handle_ &&
           *current_handle_ == sound.handle) {
            apply_gain(gain);

            const std::wstring replay =
                L"play " + alias_ + L" from 0";

            if(mciSendStringW(
                    replay.c_str(),
                    nullptr,
                    0,
                    nullptr
                ) == 0) {
                return true;
            }
        }

        return start(
            sound.handle,
            sound.path,
            gain
        );
    }

    bool AudioEngine::current_is_playing() const {
        if(alias_.empty()) return false;

        wchar_t mode[64]{};
        const std::wstring cmd =
            L"status " + alias_ + L" mode";

        if(mciSendStringW(
                cmd.c_str(),
                mode,
                static_cast<UINT>(std::size(mode)),
                nullptr
            ) != 0) {
            return false;
        }

        return
            _wcsicmp(mode, L"playing") == 0 ||
            _wcsicmp(mode, L"paused") == 0;
    }

    void AudioEngine::apply_gain(int gain) {
        if(alias_.empty()) return;

        const int volume =
            std::clamp(gain, 0, 100) * 10;

        const std::wstring cmd =
            L"setaudio " +
            alias_ +
            L" volume to " +
            std::to_wstring(volume);

        mciSendStringW(
            cmd.c_str(),
            nullptr,
            0,
            nullptr
        );
    }

    void AudioEngine::close_current() {
        if(alias_.empty()) {
            current_handle_.reset();
            return;
        }

        const std::wstring stop_cmd =
            L"stop " + alias_;
        const std::wstring close_cmd =
            L"close " + alias_;

        mciSendStringW(
            stop_cmd.c_str(),
            nullptr,
            0,
            nullptr
        );
        mciSendStringW(
            close_cmd.c_str(),
            nullptr,
            0,
            nullptr
        );

        alias_.clear();
        current_handle_.reset();
    }

    void AudioEngine::worker_main() noexcept {
        using namespace std::chrono_literals;

        const HRESULT com_result =
            CoInitializeEx(
                nullptr,
                COINIT_APARTMENTTHREADED |
                COINIT_DISABLE_OLE1DDE
            );

        const bool com_initialized =
            SUCCEEDED(com_result);

        MSG startup_message{};
        PeekMessageW(
            &startup_message,
            nullptr,
            WM_USER,
            WM_USER,
            PM_NOREMOVE
        );

        for(;;) {
            enum class Action {
                None,
                Stop,
                Clear,
                Gain,
                Immediate,
                StartQueued,
                CheckQueued
            };

            Action action = Action::None;
            PendingSound pending{};
            int gain = 100;

            {
                std::unique_lock<std::mutex> lock(mutex_);

                const auto ready = [this]() noexcept {
                    return
                        stopping_ ||
                        clear_requested_ ||
                        gain_dirty_ ||
                        immediate_.has_value() ||
                        (alias_.empty() && !queue_.empty());
                };

                if(!ready()) {
                    if(!alias_.empty()) {
                        wake_.wait_for(lock, 8ms, ready);
                    }
                    else {
                        wake_.wait(lock, ready);
                    }
                }

                if(stopping_) {
                    action = Action::Stop;
                }
                else if(clear_requested_) {
                    clear_requested_ = false;
                    action = Action::Clear;
                }
                else if(gain_dirty_) {
                    gain_dirty_ = false;
                    gain = requested_gain_;
                    action = Action::Gain;
                }
                else if(immediate_) {
                    pending = std::move(*immediate_);
                    immediate_.reset();
                    queue_.clear();
                    gain = requested_gain_;
                    action = Action::Immediate;
                }
                else if(alias_.empty() && !queue_.empty()) {
                    pending = std::move(queue_.front());
                    queue_.pop_front();
                    gain = requested_gain_;
                    action = Action::StartQueued;
                }
                else if(!alias_.empty() && !queue_.empty()) {
                    action = Action::CheckQueued;
                }
            }

            MSG message{};
            while(PeekMessageW(
                    &message,
                    nullptr,
                    0,
                    0,
                    PM_REMOVE
                )) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            if(action == Action::None &&
               !alias_.empty() &&
               !current_is_playing()) {
                close_current();
            }

            switch(action) {
                case Action::Stop:
                    close_current();
                    if(com_initialized) {
                        CoUninitialize();
                    }
                    return;

                case Action::Clear:
                    close_current();
                    break;

                case Action::Gain:
                    apply_gain(gain);
                    break;

                case Action::Immediate:
                    restart_or_start(pending, gain);
                    break;

                case Action::StartQueued:
                    start(
                        pending.handle,
                        pending.path,
                        gain
                    );
                    break;

                case Action::CheckQueued:
                    if(!current_is_playing()) {
                        close_current();
                    }
                    break;

                case Action::None:
                default:
                    break;
            }
        }
    }

    void AudioEngine::update(
        const std::vector<std::unique_ptr<Sound>> &
    ) noexcept {
    }

    bool AudioEngine::play_immediate(
        std::size_t handle,
        const Sound &sound
    ) {
        enqueue(handle, sound, true);
        return true;
    }

    void AudioEngine::clear() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(stopping_) return;
            queue_.clear();
            immediate_.reset();
            clear_requested_ = true;
        }

        wake_.notify_one();
    }

    void AudioEngine::set_gain(int gain) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requested_gain_ =
                std::clamp(gain, 0, 100);
            gain_dirty_ = true;
        }

        wake_.notify_one();
    }
}
