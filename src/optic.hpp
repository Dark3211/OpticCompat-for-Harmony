#pragma once
#include "common.hpp"
#include "audio.hpp"

namespace OpticCompat {
    struct SpriteState {
        float x = 0.0f;
        float y = 0.0f;
        float scale_x = 1.0f;
        float scale_y = 1.0f;
        float opacity = 255.0f;
        float rotation = 0.0f;
        std::size_t current_frame = 0;
    };

    class Animation {
    public:
        enum class Property : std::size_t { PositionX, PositionY, Opacity, Rotation, ScaleX, ScaleY, Count, Invalid };
        struct Curve {
            float y1 = 0.0f;
            float y2 = 0.0f;
            float value(float t) const noexcept;
        };
        struct Transform {
            float x = 0.0f;
            float y = 0.0f;
            float opacity = 0.0f;
            float rotation = 0.0f;
            float scale_x = 0.0f;
            float scale_y = 0.0f;
        };

        explicit Animation(long duration = 0) noexcept;
        long duration() const noexcept { return duration_; }
        void set_property(Property property, Curve curve, float value) noexcept;
        Transform transform() const noexcept { return transform_; }
        void play() noexcept;
        void stop() noexcept { playing_ = false; }
        bool is_playing() const noexcept { return playing_; }
        long time_left() const noexcept;
        void apply(SpriteState &state) const noexcept;
        static Property property_from_string(std::string_view value) noexcept;
        static Curve curve_from_preset(std::string_view value, bool &valid) noexcept;

    private:
        float progress() const noexcept;
        long duration_ = 0;
        bool playing_ = false;
        std::chrono::steady_clock::time_point started_{};
        Transform transform_{};
        std::array<Curve, static_cast<std::size_t>(Property::Count)> curves_{};
    };

    class Sprite {
    public:
        Sprite(std::filesystem::path path, int frame_width, int frame_height,
               std::size_t rows = 1, std::size_t columns = 1,
               std::size_t frames = 1, std::size_t fps = 0);
        ~Sprite();
        Sprite(const Sprite &) = delete;
        Sprite &operator=(const Sprite &) = delete;

        bool load(IDirect3DDevice9 *device);
        void unload() noexcept;
        bool draw(IDirect3DDevice9 *device, const SpriteState &state) const noexcept;
        std::size_t frame_count() const noexcept { return frames_; }
        std::size_t fps() const noexcept { return fps_; }

    private:
        std::filesystem::path path_;
        int frame_width_ = 0;
        int frame_height_ = 0;
        int texture_width_ = 0;
        int texture_height_ = 0;
        std::size_t rows_ = 1;
        std::size_t columns_ = 1;
        std::size_t frames_ = 1;
        std::size_t fps_ = 0;
        IDirect3DTexture9 *texture_ = nullptr;
    };

    struct ActiveAnimation {
        Animation animation;
    };

    struct RenderInstance {
        std::size_t sprite_handle = 0;
        SpriteState state{};
        std::vector<ActiveAnimation> animations;
        std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
        bool fading_out = false;
        long age_ms() const noexcept;
    };

    struct RenderQueue {
        SpriteState initial_state{};
        float rotation = 0.0f;
        std::size_t max_renders = 0;
        long render_duration = 0;
        bool temporal = false;
        Animation fade_in{};
        Animation fade_out{};
        Animation slide{};
        std::queue<std::size_t> pending;
        std::deque<RenderInstance> renders;
    };

    class OpticStore {
    public:
        void reset(std::filesystem::path data_root);
        const std::filesystem::path &data_root() const noexcept { return data_root_; }

        std::size_t create_animation(long duration);
        Animation *animation(std::size_t handle) noexcept;
        std::size_t create_sprite(const std::filesystem::path &path, int width, int height,
                                  std::size_t rows = 1, std::size_t columns = 1,
                                  std::size_t frames = 1, std::size_t fps = 0);
        std::size_t create_render_queue(SpriteState state, float rotation, std::size_t max_renders,
                                        long duration, bool temporal = false);
        RenderQueue *render_queue(std::size_t handle) noexcept;
        void enqueue_sprite(std::size_t sprite_handle, std::size_t queue_handle);
        void render_direct(std::size_t sprite_handle, SpriteState state, long duration,
                           const Animation *fade_in, const Animation *fade_out);
        void clear_render_queue(std::size_t handle);

        std::size_t create_sound(const std::filesystem::path &path);
        std::size_t create_audio_engine();
        void play_sound(std::size_t sound, std::size_t engine, bool no_enqueue);
        void clear_audio_engine(std::size_t engine);
        void set_audio_engine_gain(std::size_t engine, int gain);

        bool valid_sprite(std::size_t handle) const noexcept;
        void on_end_scene(IDirect3DDevice9 *device) noexcept;

    private:
        void process_queue(RenderQueue &queue, IDirect3DDevice9 *device) noexcept;
        std::filesystem::path data_root_;
        std::vector<Animation> animations_;
        std::vector<std::unique_ptr<Sprite>> sprites_;
        std::vector<std::unique_ptr<RenderQueue>> queues_;
        std::vector<std::unique_ptr<Sound>> sounds_;
        std::vector<std::unique_ptr<AudioEngine>> audio_engines_;
    };
}
