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
        void play(std::chrono::steady_clock::time_point now) noexcept;
        void stop() noexcept { playing_ = false; }
        bool is_playing() const noexcept { return playing_; }
        long time_left() const noexcept;
        long time_left(std::chrono::steady_clock::time_point now) const noexcept;
        void apply(SpriteState &state) const noexcept;
        void apply(SpriteState &state, std::chrono::steady_clock::time_point now) const noexcept;
        static Property property_from_string(std::string_view value) noexcept;
        static Curve curve_from_preset(std::string_view value, bool &valid) noexcept;

    private:
        float progress() const noexcept;
        float progress(std::chrono::steady_clock::time_point now) const noexcept;
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
        Sprite(std::vector<std::byte> pixels,
               int frame_width,
               int frame_height,
               std::size_t rows = 1,
               std::size_t columns = 1,
               std::size_t frames = 1,
               std::size_t fps = 0);
        ~Sprite();
        Sprite(const Sprite &) = delete;
        Sprite &operator=(const Sprite &) = delete;

        bool load(IDirect3DDevice9 *device);
        void unload() noexcept;
        static void prepare_device(IDirect3DDevice9 *device) noexcept;
        bool draw(IDirect3DDevice9 *device,
                  const SpriteState &state,
                  IDirect3DVertexBuffer9 *&vertex_buffer,
                  std::size_t &vertex_cursor,
                  bool &stream_bound,
                  IDirect3DBaseTexture9 *&bound_texture) const noexcept;
        bool matches(const std::filesystem::path &path,
                     int frame_width,
                     int frame_height,
                     std::size_t rows,
                     std::size_t columns,
                     std::size_t frames,
                     std::size_t fps) const noexcept;
        std::size_t frame_count() const noexcept { return frames_; }
        std::size_t fps() const noexcept { return fps_; }

    private:
        std::filesystem::path path_;
        std::vector<std::byte> pixels_;
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
        ~OpticStore();
        void reset(std::filesystem::path data_root);
        const std::filesystem::path &data_root() const noexcept { return data_root_; }

        std::size_t create_animation(long duration);
        Animation *animation(std::size_t handle) noexcept;
        std::size_t create_sprite(const std::filesystem::path &path, int width, int height,
                                  std::size_t rows = 1, std::size_t columns = 1,
                                  std::size_t frames = 1, std::size_t fps = 0);
        std::size_t create_memory_sprite(std::string key,
                                         int width,
                                         int height,
                                         std::vector<std::byte> pixels,
                                         std::size_t rows = 1,
                                         std::size_t columns = 1,
                                         std::size_t frames = 1,
                                         std::size_t fps = 0);
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
        bool capture_render_state(IDirect3DDevice9 *device) noexcept;
        bool ensure_sprite_vertex_buffer(IDirect3DDevice9 *device) noexcept;
        void release_render_state() noexcept;
        std::filesystem::path data_root_;
        std::vector<Animation> animations_;
        std::vector<std::unique_ptr<Sprite>> sprites_;
        std::unordered_map<std::wstring, std::size_t> file_sprites_;
        std::unordered_map<std::string, std::size_t> memory_sprites_;
        std::vector<std::unique_ptr<RenderQueue>> queues_;
        std::vector<std::unique_ptr<Sound>> sounds_;
        std::vector<std::unique_ptr<AudioEngine>> audio_engines_;
        IDirect3DDevice9 *render_device_ = nullptr;
        IDirect3DStateBlock9 *render_state_ = nullptr;
        IDirect3DVertexBuffer9 *sprite_vertex_buffer_ = nullptr;
        std::size_t sprite_vertex_cursor_ = 0;
        bool sprite_vertex_stream_bound_ = false;
        IDirect3DBaseTexture9 *sprite_bound_texture_ = nullptr;
    };
}
