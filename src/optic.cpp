#include "optic.hpp"

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "gdiplus.lib")

namespace OpticCompat {
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
        if(duration_ > 0) {
            started_ = std::chrono::steady_clock::now();
            playing_ = true;
        }
        else {
            playing_ = false;
        }
    }

    float Animation::progress() const noexcept {
        if(duration_ <= 0) return 1.0f;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_).count();
        return std::clamp(static_cast<float>(elapsed) / static_cast<float>(duration_), 0.0f, 1.0f);
    }

    long Animation::time_left() const noexcept {
        if(!playing_ || duration_ <= 0) return 0;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_).count();
        return std::max<long>(0, duration_ - static_cast<long>(elapsed));
    }

    void Animation::apply(SpriteState &state) const noexcept {
        const float t = progress();
        auto factor = [&](Property p) {
            return curves_[static_cast<std::size_t>(p)].value(t);
        };
        state.x += transform_.x * factor(Property::PositionX);
        state.y += transform_.y * factor(Property::PositionY);
        state.opacity += transform_.opacity * factor(Property::Opacity);
        state.rotation += transform_.rotation * factor(Property::Rotation);
        state.scale_x = std::max(0.0f, state.scale_x + transform_.scale_x * factor(Property::ScaleX));
        state.scale_y = std::max(0.0f, state.scale_y + transform_.scale_y * factor(Property::ScaleY));
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

    Sprite::~Sprite() { unload(); }

    bool Sprite::load(IDirect3DDevice9 *device) {
        if(texture_) return true;
        if(!device) return false;

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

    bool Sprite::draw(IDirect3DDevice9 *device, const SpriteState &state) const noexcept {
        if(!device || !texture_) return false;

        struct Vertex { float x, y, z, rhw; D3DCOLOR color; float u, v; };
        constexpr DWORD fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

        const std::size_t frame = frames_ > 1 ? (state.current_frame % frames_) : 0;
        const std::size_t row = frame / columns_;
        const std::size_t col = frame % columns_;
        const float left = frames_ > 1 ? static_cast<float>(1 + col * frame_width_) : 0.0f;
        const float top = frames_ > 1 ? static_cast<float>(1 + row * frame_height_) : 0.0f;
        const float right = std::min(static_cast<float>(texture_width_), left + frame_width_);
        const float bottom = std::min(static_cast<float>(texture_height_), top + frame_height_);
        const float u0 = left / static_cast<float>(texture_width_);
        const float v0 = top / static_cast<float>(texture_height_);
        const float u1 = right / static_cast<float>(texture_width_);
        const float v1 = bottom / static_cast<float>(texture_height_);

        const float cx = state.x + frame_width_ * 0.5f;
        const float cy = state.y + frame_height_ * 0.5f;
        const float s = std::sin(state.rotation);
        const float c = std::cos(state.rotation);

        auto transform = [&](float local_x, float local_y) {
            float px = state.x + local_x * state.scale_x;
            float py = state.y + local_y * state.scale_y;
            const float dx = px - cx;
            const float dy = py - cy;
            const float rx = cx + dx * c - dy * s;
            const float ry = cy + dx * s + dy * c;
            return std::pair<float, float>{rx - 0.5f, ry - 0.5f};
        };

        const auto tl = transform(0.0f, 0.0f);
        const auto tr = transform(static_cast<float>(frame_width_), 0.0f);
        const auto bl = transform(0.0f, static_cast<float>(frame_height_));
        const auto br = transform(static_cast<float>(frame_width_), static_cast<float>(frame_height_));
        const auto alpha = static_cast<BYTE>(std::clamp(state.opacity, 0.0f, 255.0f));
        const D3DCOLOR color = D3DCOLOR_ARGB(alpha, 255, 255, 255);

        Vertex vertices[4] = {
            {tl.first, tl.second, 0.0f, 1.0f, color, u0, v0},
            {tr.first, tr.second, 0.0f, 1.0f, color, u1, v0},
            {bl.first, bl.second, 0.0f, 1.0f, color, u0, v1},
            {br.first, br.second, 0.0f, 1.0f, color, u1, v1},
        };

        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(fvf);
        device->SetTexture(0, texture_);
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
        device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);

        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, 0);

        const HRESULT hr = device->DrawPrimitiveUP(
            D3DPT_TRIANGLESTRIP,
            2,
            vertices,
            sizeof(Vertex)
        );

        return SUCCEEDED(hr);
    }

    long RenderInstance::age_ms() const noexcept {
        return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - created).count());
    }

    void OpticStore::reset(std::filesystem::path data_root) {
        for(auto &engine : audio_engines_) if(engine) engine->clear();
        queues_.clear();
        animations_.clear();
        sprites_.clear();
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
        sprites_.push_back(std::make_unique<Sprite>(path, width, height, rows, columns, frames, fps));
        return sprites_.size() - 1;
    }

    std::size_t OpticStore::create_render_queue(SpriteState state, float rotation, std::size_t max_renders,
                                                 long duration, bool temporal) {
        auto q = std::make_unique<RenderQueue>();
        q->initial_state = state;
        q->rotation = rotation;
        q->max_renders = max_renders;
        q->render_duration = std::max<long>(0, duration);
        q->temporal = temporal;
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
        audio_engines_[engine]->enqueue(sound, no_enqueue);
        audio_engines_[engine]->update(sounds_);
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
        if(!q.slide.is_playing() && !q.pending.empty() && (q.max_renders == 0 || q.renders.size() < q.max_renders)) {
            RenderInstance render{};
            render.sprite_handle = q.pending.front();
            q.pending.pop();
            render.state = q.initial_state;
            const auto fade_transform = q.fade_in.transform();
            render.state.x -= fade_transform.x;
            render.state.y -= fade_transform.y;
            render.state.opacity -= fade_transform.opacity;
            render.state.rotation -= fade_transform.rotation;
            Animation fade = q.fade_in;
            fade.play();
            render.animations.push_back({fade});
            q.renders.push_back(std::move(render));
            if(q.renders.size() > 1) q.slide.play();
        }

        if(!q.renders.empty() && q.renders.front().age_ms() > q.render_duration) {
            q.renders.pop_front();
        }

        for(std::size_t index = 0; index < q.renders.size(); ++index) {
            auto &render = q.renders[index];
            if(!valid_sprite(render.sprite_handle)) continue;
            auto &sprite = *sprites_[render.sprite_handle];
            if(!sprite.load(device)) continue;
            SpriteState current = render.state;

            if(q.slide.is_playing() && index + 1 != q.renders.size()) {
                q.slide.apply(current);
                if(q.slide.time_left() == 0) q.slide.apply(render.state);
            }

            for(std::size_t a = 0; a < render.animations.size();) {
                auto &anim = render.animations[a].animation;
                anim.apply(current);
                if(anim.time_left() == 0) {
                    anim.apply(render.state);
                    render.animations.erase(render.animations.begin() + static_cast<std::ptrdiff_t>(a));
                }
                else {
                    ++a;
                }
            }

            if(q.render_duration - render.age_ms() < q.fade_out.duration() && !render.fading_out) {
                render.fading_out = true;
                Animation fade = q.fade_out;
                fade.play();
                render.animations.push_back({fade});
            }

            if(sprite.frame_count() > 1 && sprite.fps() > 0) {
                const double seconds = static_cast<double>(render.age_ms()) / 1000.0;
                current.current_frame = static_cast<std::size_t>(std::floor(seconds * sprite.fps())) % sprite.frame_count();
                render.state.current_frame = current.current_frame;
            }
            sprite.draw(device, current);
        }

        if(q.slide.time_left() == 0) q.slide.stop();
    }

    void OpticStore::on_end_scene(IDirect3DDevice9 *device) noexcept {
        if(!device) return;

        static IDirect3DDevice9 *texture_device = nullptr;
        if(texture_device && texture_device != device) {
            for(auto &sprite : sprites_) {
                if(sprite) sprite->unload();
            }
        }
        texture_device = device;

        for(auto &sprite : sprites_) {
            if(sprite) {
                sprite->load(device);
            }
        }

        for(auto &engine : audio_engines_) if(engine) engine->update(sounds_);

        IDirect3DStateBlock9 *state_block = nullptr;
        if(SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &state_block)) && state_block) state_block->Capture();

        for(std::size_t i = 0; i < queues_.size(); ++i) {
            auto &q = queues_[i];
            if(!q) continue;
            if(q->temporal && q->pending.empty() && q->renders.empty()) {
                q.reset();
                continue;
            }
            process_queue(*q, device);
            if(q && q->temporal && q->pending.empty() && q->renders.empty()) q.reset();
        }

        if(state_block) {
            state_block->Apply();
            state_block->Release();
        }
    }
}
