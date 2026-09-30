#include "optic.hpp"

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "gdiplus.lib")

namespace OpticCompat {
    namespace {
        struct SpriteVertex {
            float x;
            float y;
            float z;
            float rhw;
            D3DCOLOR color;
            float u;
            float v;
        };

        constexpr UINT kSpriteVertexCapacity = 1024;
        constexpr DWORD kSpriteFvf =
            D3DFVF_XYZRHW |
            D3DFVF_DIFFUSE |
            D3DFVF_TEX1;
    }

    OpticStore::~OpticStore() {
        release_render_state();
    }

    float Animation::Curve::value(float t) const noexcept {
        t = std::clamp(t, 0.0f, 1.0f);
        const float one_minus = 1.0f - t;
        return (2.0f * one_minus * t * y1) + (t * t * y2);
    }

    Animation::Animation(long duration) noexcept : duration_(std::max<long>(0, duration)) {}

    void Animation::set_property(Property property, Curve curve, float value) noexcept {
        const auto index = static_cast<std::size_t>(property);
        if(property == Property::Invalid || index >= curves_.size()) return;
        curves_[index] = curve;
        switch(property) {
            case Property::PositionX: transform_.x = value; break;
            case Property::PositionY: transform_.y = value; break;
            case Property::Opacity: transform_.opacity = value; break;
            case Property::Rotation: transform_.rotation = value; break;
            case Property::ScaleX: transform_.scale_x = value; break;
            case Property::ScaleY: transform_.scale_y = value; break;
            default: break;
        }
    }

    void Animation::play() noexcept {
        play(std::chrono::steady_clock::now());
    }

    void Animation::play(
        std::chrono::steady_clock::time_point now
    ) noexcept {
        if(duration_ > 0) {
            started_ = now;
            playing_ = true;
        }
        else {
            playing_ = false;
        }
    }

    float Animation::progress() const noexcept {
        return progress(std::chrono::steady_clock::now());
    }

    float Animation::progress(
        std::chrono::steady_clock::time_point now
    ) const noexcept {
        if(duration_ <= 0) return 1.0f;

        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - started_
            ).count();

        return std::clamp(
            static_cast<float>(elapsed) /
                static_cast<float>(duration_),
            0.0f,
            1.0f
        );
    }

    long Animation::time_left() const noexcept {
        return time_left(std::chrono::steady_clock::now());
    }

    long Animation::time_left(
        std::chrono::steady_clock::time_point now
    ) const noexcept {
        if(!playing_ || duration_ <= 0) return 0;

        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - started_
            ).count();

        return std::clamp<long>(
            duration_ - static_cast<long>(elapsed),
            0,
            duration_
        );
    }

    void Animation::apply(SpriteState &state) const noexcept {
        apply(state, std::chrono::steady_clock::now());
    }

    void Animation::apply(
        SpriteState &state,
        std::chrono::steady_clock::time_point now
    ) const noexcept {
        const float t = progress(now);
        auto factor = [&](Property p) {
            return curves_[static_cast<std::size_t>(p)].value(t);
        };
        state.x += transform_.x * factor(Property::PositionX);
        state.y += transform_.y * factor(Property::PositionY);
        state.opacity += transform_.opacity * factor(Property::Opacity);
        state.rotation += transform_.rotation * factor(Property::Rotation);
        state.scale_x = std::max(
            0.0f,
            state.scale_x +
                transform_.scale_x * factor(Property::ScaleX)
        );
        state.scale_y = std::max(
            0.0f,
            state.scale_y +
                transform_.scale_y * factor(Property::ScaleY)
        );
    }

    Animation::Property Animation::property_from_string(std::string_view value) noexcept {
        if(value == "position x") return Property::PositionX;
        if(value == "position y") return Property::PositionY;
        if(value == "opacity") return Property::Opacity;
        if(value == "rotation") return Property::Rotation;
        if(value == "scale x") return Property::ScaleX;
        if(value == "scale y") return Property::ScaleY;
        return Property::Invalid;
    }

    Animation::Curve Animation::curve_from_preset(std::string_view value, bool &valid) noexcept {
        valid = true;
        if(value == "ease in") return {0.0f, 1.0f};
        if(value == "ease out") return {0.0f, 1.0f};
        if(value == "ease in out") return {0.0f, 1.0f};
        if(value == "linear") return {0.0f, 1.0f};
        valid = false;
        return {};
    }

    Sprite::Sprite(std::filesystem::path path, int frame_width, int frame_height,
                   std::size_t rows, std::size_t columns, std::size_t frames, std::size_t fps)
        : path_(std::move(path)), frame_width_(frame_width), frame_height_(frame_height),
          texture_width_(frame_width * static_cast<int>(std::max<std::size_t>(1, columns))),
          texture_height_(frame_height * static_cast<int>(std::max<std::size_t>(1, rows))),
          rows_(std::max<std::size_t>(1, rows)), columns_(std::max<std::size_t>(1, columns)),
          frames_(std::max<std::size_t>(1, frames)), fps_(fps) {
        if(frame_width_ <= 0 || frame_height_ <= 0) throw std::runtime_error("invalid sprite dimensions");
        if(frames_ > rows_ * columns_) throw std::runtime_error("sprite frame count exceeds sheet capacity");
    }

    Sprite::Sprite(std::vector<std::byte> pixels,
                   int frame_width,
                   int frame_height,
                   std::size_t rows,
                   std::size_t columns,
                   std::size_t frames,
                   std::size_t fps)
        : pixels_(std::move(pixels)),
          frame_width_(frame_width),
          frame_height_(frame_height),
          texture_width_(frame_width * static_cast<int>(std::max<std::size_t>(1, columns))),
          texture_height_(frame_height * static_cast<int>(std::max<std::size_t>(1, rows))),
          rows_(std::max<std::size_t>(1, rows)),
          columns_(std::max<std::size_t>(1, columns)),
          frames_(std::max<std::size_t>(1, frames)),
          fps_(fps) {
        if(frame_width_ <= 0 || frame_height_ <= 0) {
            throw std::runtime_error("invalid in-memory sprite dimensions");
        }
        if(frames_ > rows_ * columns_) {
            throw std::runtime_error("in-memory sprite frame count exceeds sheet capacity");
        }

        const std::size_t expected =
            static_cast<std::size_t>(texture_width_) *
            static_cast<std::size_t>(texture_height_) *
            4u;

        if(pixels_.size() != expected) {
            throw std::runtime_error("invalid in-memory sprite pixels");
        }
    }

    Sprite::~Sprite() { unload(); }

    bool Sprite::load(IDirect3DDevice9 *device) {
        if(texture_) return true;
        if(!device) return false;

        if(!pixels_.empty()) {
            IDirect3DTexture9 *texture = nullptr;
            HRESULT hr = device->CreateTexture(
                texture_width_,
                texture_height_,
                1,
                0,
                D3DFMT_A8R8G8B8,
                D3DPOOL_MANAGED,
                &texture,
                nullptr
            );
            if(FAILED(hr) || !texture) return false;

            D3DLOCKED_RECT locked{};
            hr = texture->LockRect(0, &locked, nullptr, 0);
            if(FAILED(hr)) {
                texture->Release();
                return false;
            }

            const auto row_bytes =
                static_cast<std::size_t>(texture_width_) * 4u;

            for(int y = 0; y < texture_height_; ++y) {
                const auto *src =
                    pixels_.data() +
                    static_cast<std::size_t>(y) * row_bytes;
                auto *dst =
                    static_cast<std::byte *>(locked.pBits) +
                    static_cast<std::ptrdiff_t>(y) * locked.Pitch;
                std::memcpy(dst, src, row_bytes);
            }

            texture->UnlockRect(0);
            texture_ = texture;
            return true;
        }

        Gdiplus::Bitmap source(path_.c_str(), FALSE);
        if(source.GetLastStatus() != Gdiplus::Ok) {
            return false;
        }

        Gdiplus::Bitmap scaled(texture_width_, texture_height_, PixelFormat32bppARGB);
        if(scaled.GetLastStatus() != Gdiplus::Ok) {
            return false;
        }
        {
            Gdiplus::Graphics graphics(&scaled);
            graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
            graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
            graphics.DrawImage(&source, Gdiplus::Rect(0, 0, texture_width_, texture_height_));
        }

        IDirect3DTexture9 *texture = nullptr;
        HRESULT hr = device->CreateTexture(texture_width_, texture_height_, 1, 0, D3DFMT_A8R8G8B8,
                                           D3DPOOL_MANAGED, &texture, nullptr);
        if(FAILED(hr) || !texture) {
            return false;
        }

        Gdiplus::Rect rect(0, 0, texture_width_, texture_height_);
        Gdiplus::BitmapData bitmap_data{};
        const auto lock_status =
            scaled.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bitmap_data);
        if(lock_status != Gdiplus::Ok) {
            texture->Release();
            return false;
        }

        D3DLOCKED_RECT locked{};
        hr = texture->LockRect(0, &locked, nullptr, 0);
        if(FAILED(hr)) {
            scaled.UnlockBits(&bitmap_data);
            texture->Release();
            return false;
        }

        auto *source_base = static_cast<const std::byte *>(bitmap_data.Scan0);
        auto *dest_base = static_cast<std::byte *>(locked.pBits);
        for(int y = 0; y < texture_height_; ++y) {
            const auto *src = source_base + static_cast<std::ptrdiff_t>(y) * bitmap_data.Stride;
            auto *dst = dest_base + static_cast<std::ptrdiff_t>(y) * locked.Pitch;
            std::memcpy(dst, src, static_cast<std::size_t>(texture_width_) * 4);
        }
        texture->UnlockRect(0);
        scaled.UnlockBits(&bitmap_data);
        texture_ = texture;
        return true;
    }

    void Sprite::unload() noexcept {
        if(texture_) {
            texture_->Release();
            texture_ = nullptr;
        }
    }

    void Sprite::prepare_device(IDirect3DDevice9 *device) noexcept {
        if(!device) return;

        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(kSpriteFvf);
        device->SetTexture(0, nullptr);
        device->SetTexture(1, nullptr);

        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(
            D3DRS_COLORWRITEENABLE,
            D3DCOLORWRITEENABLE_RED |
            D3DCOLORWRITEENABLE_GREEN |
            D3DCOLORWRITEENABLE_BLUE |
            D3DCOLORWRITEENABLE_ALPHA
        );

        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        device->SetTextureStageState(
            0,
            D3DTSS_TEXTURETRANSFORMFLAGS,
            D3DTTFF_DISABLE
        );
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 0);
    }

    bool Sprite::matches(const std::filesystem::path &path,
                         int frame_width,
                         int frame_height,
                         std::size_t rows,
                         std::size_t columns,
                         std::size_t frames,
                         std::size_t fps) const noexcept {
        return path_ == path &&
               frame_width_ == frame_width &&
               frame_height_ == frame_height &&
               rows_ == std::max<std::size_t>(1, rows) &&
               columns_ == std::max<std::size_t>(1, columns) &&
               frames_ == std::max<std::size_t>(1, frames) &&
               fps_ == fps;
    }

    bool Sprite::draw(
        IDirect3DDevice9 *device,
        const SpriteState &state,
        IDirect3DVertexBuffer9 *&vertex_buffer,
        std::size_t &vertex_cursor,
        bool &stream_bound,
        IDirect3DBaseTexture9 *&bound_texture
    ) const noexcept {
        if(!device || !texture_) return false;

        const std::size_t frame =
            frames_ > 1 ? (state.current_frame % frames_) : 0;
        const std::size_t row = frame / columns_;
        const std::size_t col = frame % columns_;
        const float left =
            frames_ > 1
                ? static_cast<float>(1 + col * frame_width_)
                : 0.0f;
        const float top =
            frames_ > 1
                ? static_cast<float>(1 + row * frame_height_)
                : 0.0f;
        const float right =
            std::min(
                static_cast<float>(texture_width_),
                left + frame_width_
            );
        const float bottom =
            std::min(
                static_cast<float>(texture_height_),
                top + frame_height_
            );
        const float u0 = left / static_cast<float>(texture_width_);
        const float v0 = top / static_cast<float>(texture_height_);
        const float u1 = right / static_cast<float>(texture_width_);
        const float v1 = bottom / static_cast<float>(texture_height_);

        std::pair<float, float> tl;
        std::pair<float, float> tr;
        std::pair<float, float> bl;
        std::pair<float, float> br;

        if(state.rotation == 0.0f) {
            const float x0 = state.x - 0.5f;
            const float y0 = state.y - 0.5f;
            const float x1 =
                state.x +
                static_cast<float>(frame_width_) * state.scale_x -
                0.5f;
            const float y1 =
                state.y +
                static_cast<float>(frame_height_) * state.scale_y -
                0.5f;

            tl = {x0, y0};
            tr = {x1, y0};
            bl = {x0, y1};
            br = {x1, y1};
        }
        else {
            const float cx =
                state.x +
                static_cast<float>(frame_width_) * 0.5f;
            const float cy =
                state.y +
                static_cast<float>(frame_height_) * 0.5f;
            const float sin_rotation = std::sin(state.rotation);
            const float cos_rotation = std::cos(state.rotation);

            auto transform = [&](float local_x, float local_y) {
                const float px =
                    state.x + local_x * state.scale_x;
                const float py =
                    state.y + local_y * state.scale_y;
                const float dx = px - cx;
                const float dy = py - cy;
                const float rx =
                    cx +
                    dx * cos_rotation -
                    dy * sin_rotation;
                const float ry =
                    cy +
                    dx * sin_rotation +
                    dy * cos_rotation;
                return std::pair<float, float>{
                    rx - 0.5f,
                    ry - 0.5f
                };
            };

            tl = transform(0.0f, 0.0f);
            tr = transform(
                static_cast<float>(frame_width_),
                0.0f
            );
            bl = transform(
                0.0f,
                static_cast<float>(frame_height_)
            );
            br = transform(
                static_cast<float>(frame_width_),
                static_cast<float>(frame_height_)
            );
        }

        const auto alpha =
            static_cast<BYTE>(
                std::clamp(
                    state.opacity,
                    0.0f,
                    255.0f
                )
            );
        const D3DCOLOR color =
            D3DCOLOR_ARGB(alpha, 255, 255, 255);

        const SpriteVertex vertices[4] = {
            {tl.first, tl.second, 0.0f, 1.0f, color, u0, v0},
            {tr.first, tr.second, 0.0f, 1.0f, color, u1, v0},
            {bl.first, bl.second, 0.0f, 1.0f, color, u0, v1},
            {br.first, br.second, 0.0f, 1.0f, color, u1, v1},
        };

        if(bound_texture != texture_) {
            if(FAILED(device->SetTexture(0, texture_))) {
                return false;
            }
            bound_texture = texture_;
        }

        if(vertex_buffer) {
            if(vertex_cursor + 4 > kSpriteVertexCapacity) {
                vertex_cursor = 0;
            }

            void *mapped = nullptr;
            const UINT offset =
                static_cast<UINT>(
                    vertex_cursor * sizeof(SpriteVertex)
                );
            const DWORD lock_flags =
                vertex_cursor == 0
                    ? D3DLOCK_DISCARD
                    : D3DLOCK_NOOVERWRITE;

            if(SUCCEEDED(
                    vertex_buffer->Lock(
                        offset,
                        sizeof(vertices),
                        &mapped,
                        lock_flags
                    )
                ) &&
               mapped) {
                std::memcpy(
                    mapped,
                    vertices,
                    sizeof(vertices)
                );
                vertex_buffer->Unlock();

                if(!stream_bound) {
                    if(SUCCEEDED(
                            device->SetStreamSource(
                                0,
                                vertex_buffer,
                                0,
                                sizeof(SpriteVertex)
                            )
                        )) {
                        stream_bound = true;
                    }
                }

                if(stream_bound) {
                    const HRESULT hr =
                        device->DrawPrimitive(
                            D3DPT_TRIANGLESTRIP,
                            static_cast<UINT>(vertex_cursor),
                            2
                        );

                    if(SUCCEEDED(hr)) {
                        vertex_cursor += 4;
                        return true;
                    }
                }
            }
            else {
                vertex_buffer->Release();
                vertex_buffer = nullptr;
            }

            stream_bound = false;
        }

        const HRESULT hr =
            device->DrawPrimitiveUP(
                D3DPT_TRIANGLESTRIP,
                2,
                vertices,
                sizeof(SpriteVertex)
            );

        stream_bound = false;
        return SUCCEEDED(hr);
    }

    long RenderInstance::age_ms() const noexcept {
        return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - created).count());
    }

    void OpticStore::reset(std::filesystem::path data_root) {
        release_render_state();
        for(auto &engine : audio_engines_) if(engine) engine->clear();
        queues_.clear();
        animations_.clear();
        sprites_.clear();
        file_sprites_.clear();
        memory_sprites_.clear();
        sounds_.clear();
        audio_engines_.clear();
        data_root_ = std::move(data_root);
    }

    std::size_t OpticStore::create_animation(long duration) {
        animations_.emplace_back(duration);
        return animations_.size() - 1;
    }

    Animation *OpticStore::animation(std::size_t handle) noexcept {
        return handle < animations_.size() ? &animations_[handle] : nullptr;
    }

    std::size_t OpticStore::create_sprite(const std::filesystem::path &path, int width, int height,
                                          std::size_t rows, std::size_t columns, std::size_t frames, std::size_t fps) {
        const auto normalized_rows = std::max<std::size_t>(1, rows);
        const auto normalized_columns = std::max<std::size_t>(1, columns);
        const auto normalized_frames = std::max<std::size_t>(1, frames);

        std::wstring key = path.native();
        key.push_back(L'\x1f');
        key += std::to_wstring(width);
        key.push_back(L'\x1f');
        key += std::to_wstring(height);
        key.push_back(L'\x1f');
        key += std::to_wstring(normalized_rows);
        key.push_back(L'\x1f');
        key += std::to_wstring(normalized_columns);
        key.push_back(L'\x1f');
        key += std::to_wstring(normalized_frames);
        key.push_back(L'\x1f');
        key += std::to_wstring(fps);

        const auto cached = file_sprites_.find(key);
        if(cached != file_sprites_.end() &&
           valid_sprite(cached->second) &&
           sprites_[cached->second]->matches(
               path,
               width,
               height,
               normalized_rows,
               normalized_columns,
               normalized_frames,
               fps)) {
            return cached->second;
        }

        sprites_.push_back(std::make_unique<Sprite>(
            path,
            width,
            height,
            normalized_rows,
            normalized_columns,
            normalized_frames,
            fps
        ));

        const auto handle = sprites_.size() - 1;
        file_sprites_[std::move(key)] = handle;
        return handle;
    }

    std::size_t OpticStore::create_memory_sprite(
        std::string key,
        int width,
        int height,
        std::vector<std::byte> pixels,
        std::size_t rows,
        std::size_t columns,
        std::size_t frames,
        std::size_t fps
    ) {
        const auto cached = memory_sprites_.find(key);
        if(cached != memory_sprites_.end() &&
           valid_sprite(cached->second)) {
            return cached->second;
        }

        sprites_.push_back(std::make_unique<Sprite>(
            std::move(pixels),
            width,
            height,
            rows,
            columns,
            frames,
            fps
        ));

        const auto handle = sprites_.size() - 1;
        memory_sprites_[std::move(key)] = handle;
        return handle;
    }

    std::size_t OpticStore::create_render_queue(SpriteState state, float rotation, std::size_t max_renders,
                                                 long duration, bool temporal) {
        auto q = std::make_unique<RenderQueue>();
        q->initial_state = state;
        q->rotation = rotation;
        q->max_renders = max_renders;
        q->render_duration = std::max<long>(0, duration);
        q->temporal = temporal;

        if(temporal) {
            for(std::size_t i = 0; i < queues_.size(); ++i) {
                if(!queues_[i]) {
                    queues_[i] = std::move(q);
                    return i;
                }
            }
        }

        queues_.push_back(std::move(q));
        return queues_.size() - 1;
    }

    RenderQueue *OpticStore::render_queue(std::size_t handle) noexcept {
        return handle < queues_.size() && queues_[handle] ? queues_[handle].get() : nullptr;
    }

    bool OpticStore::valid_sprite(std::size_t handle) const noexcept {
        return handle < sprites_.size() && sprites_[handle] != nullptr;
    }

    void OpticStore::enqueue_sprite(std::size_t sprite_handle, std::size_t queue_handle) {
        if(!valid_sprite(sprite_handle)) throw std::runtime_error("invalid sprite handle");
        auto *q = render_queue(queue_handle);
        if(!q) throw std::runtime_error("invalid render queue handle");
        q->pending.push(sprite_handle);
    }

    void OpticStore::render_direct(std::size_t sprite_handle, SpriteState state, long duration,
                                   const Animation *fade_in, const Animation *fade_out) {
        if(!valid_sprite(sprite_handle)) throw std::runtime_error("invalid sprite handle");
        const auto handle = create_render_queue(state, 0.0f, 0, duration, true);
        auto *q = render_queue(handle);
        if(fade_in) q->fade_in = *fade_in;
        if(fade_out) q->fade_out = *fade_out;
        q->pending.push(sprite_handle);
    }

    void OpticStore::clear_render_queue(std::size_t handle) {
        auto *q = render_queue(handle);
        if(!q) throw std::runtime_error("invalid render queue handle");
        std::queue<std::size_t> empty;
        std::swap(q->pending, empty);
        q->renders.clear();
    }

    std::size_t OpticStore::create_sound(const std::filesystem::path &path) {
        sounds_.push_back(std::make_unique<Sound>(Sound{path}));
        return sounds_.size() - 1;
    }

    std::size_t OpticStore::create_audio_engine() {
        audio_engines_.push_back(std::make_unique<AudioEngine>());
        return audio_engines_.size() - 1;
    }

    void OpticStore::play_sound(std::size_t sound, std::size_t engine, bool no_enqueue) {
        if(sound >= sounds_.size() || !sounds_[sound]) throw std::runtime_error("invalid sound handle");
        if(engine >= audio_engines_.size() || !audio_engines_[engine]) throw std::runtime_error("invalid audio engine handle");

        if(no_enqueue) {
            if(!audio_engines_[engine]->play_immediate(
                    sound,
                    *sounds_[sound])) {
                throw std::runtime_error("could not play sound");
            }
            return;
        }

        audio_engines_[engine]->enqueue(
            sound,
            *sounds_[sound],
            false
        );
    }

    void OpticStore::clear_audio_engine(std::size_t engine) {
        if(engine >= audio_engines_.size() || !audio_engines_[engine]) throw std::runtime_error("invalid audio engine handle");
        audio_engines_[engine]->clear();
    }

    void OpticStore::set_audio_engine_gain(std::size_t engine, int gain) {
        if(engine >= audio_engines_.size() || !audio_engines_[engine]) throw std::runtime_error("invalid audio engine handle");
        audio_engines_[engine]->set_gain(gain);
    }

    void OpticStore::process_queue(RenderQueue &q, IDirect3DDevice9 *device) noexcept {
        const auto now = std::chrono::steady_clock::now();
        const auto age_ms = [&now](const RenderInstance &render) noexcept {
            return static_cast<long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - render.created
                ).count()
            );
        };

        if(!q.slide.is_playing() && !q.pending.empty() && (q.max_renders == 0 || q.renders.size() < q.max_renders)) {
            RenderInstance render{};
            render.created = now;
            render.sprite_handle = q.pending.front();
            q.pending.pop();
            render.state = q.initial_state;
            const auto fade_transform = q.fade_in.transform();
            render.state.x -= fade_transform.x;
            render.state.y -= fade_transform.y;
            render.state.opacity -= fade_transform.opacity;
            render.state.rotation -= fade_transform.rotation;
            Animation fade = q.fade_in;
            fade.play(now);
            render.animations.push_back({fade});
            q.renders.push_back(std::move(render));
            if(q.renders.size() > 1) q.slide.play(now);
        }

        if(!q.renders.empty() && age_ms(q.renders.front()) > q.render_duration) {
            q.renders.pop_front();
        }

        for(std::size_t index = 0; index < q.renders.size(); ++index) {
            auto &render = q.renders[index];
            if(!valid_sprite(render.sprite_handle)) continue;
            auto &sprite = *sprites_[render.sprite_handle];
            if(!sprite.load(device)) continue;
            SpriteState current = render.state;

            if(q.slide.is_playing() && index + 1 != q.renders.size()) {
                q.slide.apply(current, now);
                if(q.slide.time_left(now) == 0) {
                    q.slide.apply(render.state, now);
                }
            }

            for(std::size_t a = 0; a < render.animations.size();) {
                auto &anim = render.animations[a].animation;
                anim.apply(current, now);
                if(anim.time_left(now) == 0) {
                    anim.apply(render.state, now);
                    render.animations.erase(render.animations.begin() + static_cast<std::ptrdiff_t>(a));
                }
                else {
                    ++a;
                }
            }

            const long render_age = age_ms(render);

            if(q.render_duration - render_age < q.fade_out.duration() && !render.fading_out) {
                render.fading_out = true;
                Animation fade = q.fade_out;
                fade.play(now);
                render.animations.push_back({fade});
            }

            if(sprite.frame_count() > 1 && sprite.fps() > 0) {
                const double seconds = static_cast<double>(render_age) / 1000.0;
                current.current_frame = static_cast<std::size_t>(std::floor(seconds * sprite.fps())) % sprite.frame_count();
                render.state.current_frame = current.current_frame;
            }
            sprite.draw(
                device,
                current,
                sprite_vertex_buffer_,
                sprite_vertex_cursor_,
                sprite_vertex_stream_bound_,
                sprite_bound_texture_
            );
        }

        if(q.slide.time_left(now) == 0) q.slide.stop();
    }

    void OpticStore::release_render_state() noexcept {
        if(render_state_) {
            render_state_->Release();
            render_state_ = nullptr;
        }
        if(sprite_vertex_buffer_) {
            sprite_vertex_buffer_->Release();
            sprite_vertex_buffer_ = nullptr;
        }
        sprite_vertex_cursor_ = 0;
        sprite_vertex_stream_bound_ = false;
        sprite_bound_texture_ = nullptr;
        render_device_ = nullptr;
    }

    bool OpticStore::ensure_sprite_vertex_buffer(
        IDirect3DDevice9 *device
    ) noexcept {
        if(sprite_vertex_buffer_) return true;
        if(!device) return false;

        IDirect3DVertexBuffer9 *buffer = nullptr;
        const HRESULT hr =
            device->CreateVertexBuffer(
                static_cast<UINT>(
                    sizeof(SpriteVertex) *
                    kSpriteVertexCapacity
                ),
                D3DUSAGE_DYNAMIC |
                D3DUSAGE_WRITEONLY,
                kSpriteFvf,
                D3DPOOL_DEFAULT,
                &buffer,
                nullptr
            );

        if(FAILED(hr) || !buffer) {
            return false;
        }

        sprite_vertex_buffer_ = buffer;
        sprite_vertex_cursor_ = 0;
        sprite_vertex_stream_bound_ = false;
        return true;
    }

    bool OpticStore::capture_render_state(IDirect3DDevice9 *device) noexcept {
        if(!device) return false;

        if(render_device_ != device) {
            release_render_state();
            render_device_ = device;
        }

        if(!render_state_) {
            if(FAILED(device->CreateStateBlock(
                    D3DSBT_ALL,
                    &render_state_)) ||
               !render_state_) {
                render_device_ = nullptr;
                return false;
            }
            return true;
        }

        if(FAILED(render_state_->Capture())) {
            release_render_state();
            return false;
        }
        return true;
    }

    void OpticStore::on_end_scene(IDirect3DDevice9 *device) noexcept {
        if(!device) return;

        if(render_device_ && render_device_ != device) {
            for(auto &sprite : sprites_) {
                if(sprite) sprite->unload();
            }
            release_render_state();
        }

        bool has_render_work = false;
        for(auto &queue : queues_) {
            if(!queue) continue;

            if(queue->temporal &&
               queue->pending.empty() &&
               queue->renders.empty()) {
                queue.reset();
                continue;
            }

            if(!queue->pending.empty() || !queue->renders.empty()) {
                has_render_work = true;
            }
        }

        if(!has_render_work) return;
        if(!capture_render_state(device)) return;

        Sprite::prepare_device(device);

        ensure_sprite_vertex_buffer(device);
        sprite_vertex_cursor_ = 0;
        sprite_vertex_stream_bound_ = false;
        sprite_bound_texture_ = nullptr;

        for(auto &queue : queues_) {
            if(!queue) continue;
            process_queue(*queue, device);
            if(queue &&
               queue->temporal &&
               queue->pending.empty() &&
               queue->renders.empty()) {
                queue.reset();
            }
        }

        if(render_state_ && FAILED(render_state_->Apply())) {
            release_render_state();
        }
    }

}
