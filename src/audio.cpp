#include "audio.hpp"
#include "log.hpp"

#pragma comment(lib, "winmm.lib")

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

    AudioEngine::~AudioEngine() {
        clear();
    }

    void AudioEngine::enqueue(std::size_t sound_handle, bool no_enqueue) {
        if(no_enqueue) {
            clear();
            queue_.push_front(sound_handle);
        }
        else {
            queue_.push_back(sound_handle);
        }
    }

    bool AudioEngine::start(std::size_t handle, const Sound &sound) {
        close_current();
        alias_ = L"opticcompat_" + std::to_wstring(next_alias_.fetch_add(1));
        const std::wstring open_cmd = L"open " + quote_mci_path(sound.path) + L" type mpegvideo alias " + alias_;
        const MCIERROR open_error = mciSendStringW(open_cmd.c_str(), nullptr, 0, nullptr);
        if(open_error != 0) {
            wchar_t error_text[256]{};
            mciGetErrorStringW(open_error, error_text, static_cast<UINT>(std::size(error_text)));
            log_line("MCI could not open sound handle %zu (error %lu).", handle, static_cast<unsigned long>(open_error));
            alias_.clear();
            return false;
        }

        const int volume = std::clamp(gain_, 0, 100) * 10;
        const std::wstring volume_cmd = L"setaudio " + alias_ + L" volume to " + std::to_wstring(volume);
        mciSendStringW(volume_cmd.c_str(), nullptr, 0, nullptr);

        const std::wstring play_cmd = L"play " + alias_ + L" from 0";
        const MCIERROR play_error = mciSendStringW(play_cmd.c_str(), nullptr, 0, nullptr);
        if(play_error != 0) {
            log_line("MCI could not play sound handle %zu (error %lu).", handle, static_cast<unsigned long>(play_error));
            close_current();
            return false;
        }
        return true;
    }

    bool AudioEngine::current_is_playing() const {
        if(alias_.empty()) return false;
        wchar_t mode[64]{};
        const std::wstring cmd = L"status " + alias_ + L" mode";
        if(mciSendStringW(cmd.c_str(), mode, static_cast<UINT>(std::size(mode)), nullptr) != 0) return false;
        return _wcsicmp(mode, L"playing") == 0 || _wcsicmp(mode, L"paused") == 0;
    }

    void AudioEngine::close_current() {
        if(alias_.empty()) return;
        const std::wstring stop_cmd = L"stop " + alias_;
        const std::wstring close_cmd = L"close " + alias_;
        mciSendStringW(stop_cmd.c_str(), nullptr, 0, nullptr);
        mciSendStringW(close_cmd.c_str(), nullptr, 0, nullptr);
        alias_.clear();
    }

    void AudioEngine::update(const std::vector<std::unique_ptr<Sound>> &sounds) {
        if(!alias_.empty() && !current_is_playing()) close_current();
        while(alias_.empty() && !queue_.empty()) {
            const std::size_t handle = queue_.front();
            queue_.pop_front();
            if(handle >= sounds.size() || !sounds[handle]) continue;
            if(start(handle, *sounds[handle])) break;
        }
    }

    void AudioEngine::clear() {
        queue_.clear();
        close_current();
    }

    void AudioEngine::set_gain(int gain) {
        gain_ = std::clamp(gain, 0, 100);
        if(!alias_.empty()) {
            const std::wstring cmd = L"setaudio " + alias_ + L" volume to " + std::to_wstring(gain_ * 10);
            mciSendStringW(cmd.c_str(), nullptr, 0, nullptr);
        }
    }
}
