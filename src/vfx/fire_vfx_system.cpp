#include "vfx/fire_vfx_system.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace vfx {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

inline float Clamp01(float v) noexcept {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

inline float Lerp(float a, float b, float t) noexcept {
    return a + (b - a) * t;
}

inline uint32_t HashU32(uint32_t x) noexcept {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

inline float HashFloat01(uint32_t seed) noexcept {
    return static_cast<float>(HashU32(seed) & 0x00FFFFFFU) / static_cast<float>(0x01000000U);
}

inline float HashSigned(uint32_t seed) noexcept {
    return HashFloat01(seed) * 2.0f - 1.0f;
}

inline void Normalize3(float v[3]) noexcept {
    const float len_sq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (len_sq > 1e-8f) {
        const float inv = 1.0f / std::sqrt(len_sq);
        v[0] *= inv;
        v[1] *= inv;
        v[2] *= inv;
    } else {
        v[0] = 0.0f;
        v[1] = 1.0f;
        v[2] = 0.0f;
    }
}

inline void Cross3(const float a[3], const float b[3], float out[3]) noexcept {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

inline uint32_t SpatialSeed(const float p[3]) noexcept {
    const int32_t ix = static_cast<int32_t>(std::floor(p[0] * 10.0f));
    const int32_t iy = static_cast<int32_t>(std::floor(p[1] * 10.0f));
    const int32_t iz = static_cast<int32_t>(std::floor(p[2] * 10.0f));
    uint32_t h = static_cast<uint32_t>(ix) * 73856093U;
    h ^= static_cast<uint32_t>(iy) * 19349663U;
    h ^= static_cast<uint32_t>(iz) * 83492791U;
    return HashU32(h);
}

}  // namespace

void FlipbookTexture::SampleNormalDistortion(float u, float v,
                                             float& out_nx, float& out_ny, float& out_alpha) const noexcept {
    if (!IsValid() || pixels_argb8888.empty()) {
        out_nx = 0.0f;
        out_ny = 0.0f;
        out_alpha = 0.0f;
        return;
    }
    const float cu = Clamp01(u) * static_cast<float>(width - 1U);
    const float cv = Clamp01(v) * static_cast<float>(height - 1U);
    const uint32_t x = static_cast<uint32_t>(cu);
    const uint32_t y = static_cast<uint32_t>(cv);
    const uint32_t px = pixels_argb8888[static_cast<size_t>(y) * width + x];

    const uint8_t a = static_cast<uint8_t>((px >> 24) & 0xFFU);
    const uint8_t r = static_cast<uint8_t>((px >> 16) & 0xFFU);
    const uint8_t g = static_cast<uint8_t>((px >> 8) & 0xFFU);

    out_nx = (static_cast<float>(r) - 128.0f) / 127.0f;
    out_ny = (static_cast<float>(g) - 128.0f) / 127.0f;
    out_alpha = static_cast<float>(a) / 255.0f;
}

bool FireVfxSystem::Initialize(const FireVfxConfig& config) {
    config_ = config;

    const size_t tex_vert_cap = static_cast<size_t>(config_.max_textured_quads_per_batch) * 6U;
    smoke_alpha_verts_.reserve(tex_vert_cap);
    shockwave_alpha_verts_.reserve(tex_vert_cap / 2U);
    shockwave_add_verts_.reserve(tex_vert_cap / 2U);
    fire_loop_add_verts_.reserve(tex_vert_cap);
    explosion_add_verts_.reserve(tex_vert_cap);
    embers_add_verts_.reserve(tex_vert_cap);
    untextured_add_verts_.reserve(config_.max_untextured_verts_per_batch);

    const bool ok_fire = LoadFlipbookPng(config_.fire_loop_path, config_.fire_loop_cols,
                                         config_.fire_loop_rows, config_.cell_uv_padding_px,
                                         fire_loop_tex_);
    const bool ok_expl = LoadFlipbookPng(config_.explosion_fire_path, config_.explosion_fire_cols,
                                         config_.explosion_fire_rows, config_.cell_uv_padding_px,
                                         explosion_fire_tex_);
    LoadFlipbookPng(config_.explosion_smoke_path, config_.explosion_smoke_cols,
                    config_.explosion_smoke_rows, config_.cell_uv_padding_px,
                    explosion_smoke_tex_);
    LoadFlipbookPng(config_.embers_sparks_path, config_.embers_sparks_cols,
                    config_.embers_sparks_rows, config_.cell_uv_padding_px,
                    embers_sparks_tex_);
    LoadFlipbookPng(config_.shockwave_normal_path, 1U, 1U, 0.0f, shockwave_normal_tex_);

    return ok_fire && ok_expl;
}

void FireVfxSystem::Shutdown() {
    fire_loop_tex_ = FlipbookTexture{};
    explosion_fire_tex_ = FlipbookTexture{};
    explosion_smoke_tex_ = FlipbookTexture{};
    embers_sparks_tex_ = FlipbookTexture{};
    shockwave_normal_tex_ = FlipbookTexture{};

    smoke_alpha_verts_.clear();
    shockwave_alpha_verts_.clear();
    shockwave_add_verts_.clear();
    fire_loop_add_verts_.clear();
    explosion_add_verts_.clear();
    embers_add_verts_.clear();
    untextured_add_verts_.clear();
}

bool FireVfxSystem::LoadFlipbookPng(const std::string& path, uint32_t cols, uint32_t rows,
                                    float padding_px, FlipbookTexture& out_tex) {
    SDL_Surface* raw = SDL_LoadPNG(path.c_str());
    if (!raw) {
        return false;
    }

    SDL_Surface* converted = raw;
    if (raw->format != SDL_PIXELFORMAT_ARGB8888) {
        converted = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_ARGB8888);
        SDL_DestroySurface(raw);
        if (!converted) {
            return false;
        }
    }

    out_tex.width = static_cast<uint32_t>(converted->w);
    out_tex.height = static_cast<uint32_t>(converted->h);
    out_tex.cols = std::max(1U, cols);
    out_tex.rows = std::max(1U, rows);
    out_tex.padding_px = padding_px;
    out_tex.pixels_argb8888.resize(static_cast<size_t>(out_tex.width) * out_tex.height);

    const uint8_t* src_bytes = static_cast<const uint8_t*>(converted->pixels);
    for (uint32_t y = 0; y < out_tex.height; ++y) {
        const uint8_t* row_ptr = src_bytes + static_cast<size_t>(y) * converted->pitch;
        uint32_t* dst_ptr = out_tex.pixels_argb8888.data() + static_cast<size_t>(y) * out_tex.width;
        std::copy_n(reinterpret_cast<const uint32_t*>(row_ptr), out_tex.width, dst_ptr);
    }

    SDL_DestroySurface(converted);
    return out_tex.IsValid();
}

FireVfxSystem::CellUv FireVfxSystem::ComputeCellUv(const FlipbookTexture& tex,
                                                   uint32_t frame_index) noexcept {
    const uint32_t total = tex.cols * tex.rows;
    const uint32_t idx = total > 0 ? (frame_index % total) : 0;
    const uint32_t c = idx % tex.cols;
    const uint32_t r = idx / tex.cols;

    const float cell_w = 1.0f / static_cast<float>(tex.cols);
    const float cell_h = 1.0f / static_cast<float>(tex.rows);

    const float pad_u = tex.width > 0 ? (tex.padding_px + 0.5f) / static_cast<float>(tex.width) : 0.002f;
    const float pad_v = tex.height > 0 ? (tex.padding_px + 0.5f) / static_cast<float>(tex.height) : 0.002f;

    CellUv uv{};
    uv.u0 = static_cast<float>(c) * cell_w + pad_u;
    uv.v0 = static_cast<float>(r) * cell_h + pad_v;
    uv.u1 = static_cast<float>(c + 1U) * cell_w - pad_u;
    uv.v1 = static_cast<float>(r + 1U) * cell_h - pad_v;
    return uv;
}

uint32_t FireVfxSystem::BlackbodyColor(float heat_01, float alpha_01) noexcept {
    const float h = Clamp01(heat_01);
    const float a = Clamp01(alpha_01);

    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    if (h > 0.78f) {
        const float k = (h - 0.78f) / 0.22f;
        r = 1.0f;
        g = Lerp(0.75f, 0.98f, k);
        b = Lerp(0.35f, 0.92f, k);
    } else if (h > 0.42f) {
        const float k = (h - 0.42f) / 0.36f;
        r = 1.0f;
        g = Lerp(0.28f, 0.75f, k);
        b = Lerp(0.05f, 0.35f, k);
    } else {
        const float k = h / 0.42f;
        r = Lerp(0.35f, 1.0f, k);
        g = Lerp(0.05f, 0.28f, k);
        b = Lerp(0.02f, 0.05f, k);
    }

    return PackBgra(static_cast<uint8_t>(a * 255.0f),
                    static_cast<uint8_t>(r * 255.0f),
                    static_cast<uint8_t>(g * 255.0f),
                    static_cast<uint8_t>(b * 255.0f));
}

bool FireVfxSystem::ProjectWorldToScreenUv(const float world_pos[3],
                                           float out_uv[2], float& out_view_z) const noexcept {
    if (!camera_.view || !camera_.proj) {
        return false;
    }
    const float x = world_pos[0], y = world_pos[1], z = world_pos[2];
    const float vx = x * camera_.view[0] + y * camera_.view[4] + z * camera_.view[8] + camera_.view[12];
    const float vy = x * camera_.view[1] + y * camera_.view[5] + z * camera_.view[9] + camera_.view[13];
    const float vz = x * camera_.view[2] + y * camera_.view[6] + z * camera_.view[10] + camera_.view[14];
    const float vw = x * camera_.view[3] + y * camera_.view[7] + z * camera_.view[11] + camera_.view[15];

    const float cx = vx * camera_.proj[0] + vy * camera_.proj[4] + vz * camera_.proj[8] + vw * camera_.proj[12];
    const float cy = vx * camera_.proj[1] + vy * camera_.proj[5] + vz * camera_.proj[9] + vw * camera_.proj[13];
    const float cw = vx * camera_.proj[3] + vy * camera_.proj[7] + vz * camera_.proj[11] + vw * camera_.proj[15];

    if (cw <= 0.05f) {
        return false;
    }
    const float ndc_x = cx / cw;
    const float ndc_y = cy / cw;
    out_uv[0] = ndc_x * 0.5f + 0.5f;
    out_uv[1] = 0.5f - ndc_y * 0.5f;
    out_view_z = cw;
    return true;
}

void FireVfxSystem::BeginFrame(const CameraFrameData& camera) noexcept {
    camera_ = camera;
    screen_distortion_ = ShockwaveScreenDistortion{};

    smoke_alpha_verts_.clear();
    shockwave_alpha_verts_.clear();
    shockwave_add_verts_.clear();
    fire_loop_add_verts_.clear();
    explosion_add_verts_.clear();
    embers_add_verts_.clear();
    untextured_add_verts_.clear();
}

void FireVfxSystem::PushBillboardQuad(std::vector<d3d::TexturedWorldVertex>& dst,
                                      const float center[3], float half_w, float half_h,
                                      float roll_rad, const CellUv& uv, uint32_t bgra) {
    const float cs = std::cos(roll_rad);
    const float sn = std::sin(roll_rad);

    float axis_u[3]{};
    float axis_v[3]{};
    for (int i = 0; i < 3; ++i) {
        axis_u[i] = (camera_.right[i] * cs - camera_.up[i] * sn) * half_w;
        axis_v[i] = (camera_.right[i] * sn + camera_.up[i] * cs) * half_h;
    }
    PushOrientedQuad(dst, center, axis_u, axis_v, uv, bgra);
}

void FireVfxSystem::PushOrientedQuad(std::vector<d3d::TexturedWorldVertex>& dst,
                                     const float center[3], const float axis_u[3], const float axis_v[3],
                                     const CellUv& uv, uint32_t bgra) {
    if (dst.size() + 6U > dst.capacity()) {
        return;
    }

    const d3d::TexturedWorldVertex v0{
        center[0] - axis_u[0] + axis_v[0], center[1] - axis_u[1] + axis_v[1],
        center[2] - axis_u[2] + axis_v[2], bgra, uv.u0, uv.v0};
    const d3d::TexturedWorldVertex v1{
        center[0] + axis_u[0] + axis_v[0], center[1] + axis_u[1] + axis_v[1],
        center[2] + axis_u[2] + axis_v[2], bgra, uv.u1, uv.v0};
    const d3d::TexturedWorldVertex v2{
        center[0] + axis_u[0] - axis_v[0], center[1] + axis_u[1] - axis_v[1],
        center[2] + axis_u[2] - axis_v[2], bgra, uv.u1, uv.v1};
    const d3d::TexturedWorldVertex v3{
        center[0] - axis_u[0] - axis_v[0], center[1] - axis_u[1] - axis_v[1],
        center[2] - axis_u[2] - axis_v[2], bgra, uv.u0, uv.v1};

    dst.push_back(v0);
    dst.push_back(v1);
    dst.push_back(v2);
    dst.push_back(v0);
    dst.push_back(v2);
    dst.push_back(v3);
}

void FireVfxSystem::EmitRefractiveShockwaveShell(const float center_pos[3], float shell_radius,
                                                 float normalized_age, float glow_mul, uint32_t seed) {
    // shockwave_normal.png is used for real screen-space refraction in Vulkan and
    // normal-driven ripple perturbation on the subtle ground pressure wave ring; never
    // draw raw purple normal-map pixels directly as world billboards.
    const float fade = std::pow(1.0f - normalized_age, 1.85f) * Clamp01(glow_mul);
    if (fade <= 0.02f || normalized_age > 0.65f) {
        (void)seed;
        return;
    }

    constexpr uint32_t kRingSegments = 32U;
    if (untextured_add_verts_.size() + kRingSegments * 6U > untextured_add_verts_.capacity()) {
        return;
    }

    const float step = kTwoPi / static_cast<float>(kRingSegments);
    const float ring_thick = shell_radius * Lerp(0.14f, 0.05f, normalized_age);

    for (uint32_t i = 0; i < kRingSegments; ++i) {
        const float a0 = static_cast<float>(i) * step;
        const float a1 = static_cast<float>(i + 1U) * step;

        float nx0 = 0.0f, ny0 = 0.0f, na0 = 1.0f;
        float nx1 = 0.0f, ny1 = 0.0f, na1 = 1.0f;
        if (shockwave_normal_tex_.IsValid()) {
            shockwave_normal_tex_.SampleNormalDistortion(
                0.5f + 0.36f * std::cos(a0), 0.5f + 0.36f * std::sin(a0), nx0, ny0, na0);
            shockwave_normal_tex_.SampleNormalDistortion(
                0.5f + 0.36f * std::cos(a1), 0.5f + 0.36f * std::sin(a1), nx1, ny1, na1);
        }

        const float warp0 = 1.0f + nx0 * 0.06f;
        const float warp1 = 1.0f + nx1 * 0.06f;
        const float r_out0 = shell_radius * warp0;
        const float r_in0 = std::max(0.05f, r_out0 - ring_thick);
        const float r_out1 = shell_radius * warp1;
        const float r_in1 = std::max(0.05f, r_out1 - ring_thick);

        const float c0 = std::cos(a0), s0 = std::sin(a0);
        const float c1 = std::cos(a1), s1 = std::sin(a1);
        const float y0 = center_pos[1] + 0.06f + ny0 * 0.04f * shell_radius;
        const float y1 = center_pos[1] + 0.06f + ny1 * 0.04f * shell_radius;

        const uint8_t rim_a = static_cast<uint8_t>(Clamp01(fade * 0.22f * (0.5f + 0.5f * na0)) * 255.0f);
        const uint32_t inner_col = PackBgra(0, 235, 200, 165);
        const uint32_t outer_col = PackBgra(rim_a, 245, 225, 195);

        untextured_add_verts_.push_back({center_pos[0] + c0 * r_in0, y0, center_pos[2] + s0 * r_in0, inner_col});
        untextured_add_verts_.push_back({center_pos[0] + c0 * r_out0, y0, center_pos[2] + s0 * r_out0, outer_col});
        untextured_add_verts_.push_back({center_pos[0] + c1 * r_out1, y1, center_pos[2] + s1 * r_out1, outer_col});
        untextured_add_verts_.push_back({center_pos[0] + c0 * r_in0, y0, center_pos[2] + s0 * r_in0, inner_col});
        untextured_add_verts_.push_back({center_pos[0] + c1 * r_out1, y1, center_pos[2] + s1 * r_out1, outer_col});
        untextured_add_verts_.push_back({center_pos[0] + c1 * r_in1, y1, center_pos[2] + s1 * r_in1, inner_col});
    }
    (void)seed;
}

void FireVfxSystem::EmitHeatHazeShimmer(const float center_pos[3], float radius,
                                        float age, float strength, uint32_t seed) {
    (void)age;
    (void)seed;
    if (strength <= 0.02f) {
        return;
    }

    float screen_uv[2]{0.5f, 0.5f};
    float view_z = 10.0f;
    const float shimmer_center[3] = {
        center_pos[0],
        center_pos[1] + radius * 0.55f,
        center_pos[2],
    };
    if (!ProjectWorldToScreenUv(shimmer_center, screen_uv, view_z)) {
        return;
    }
    if (screen_uv[0] < -0.25f || screen_uv[0] > 1.25f ||
        screen_uv[1] < -0.25f || screen_uv[1] > 1.25f) {
        return;
    }
    const float dist_atten = Clamp01(16.0f / std::max(1.0f, view_z));
    const float haze = std::clamp(0.014f * Clamp01(strength) * dist_atten, 0.0f, 0.018f);
    if (haze <= screen_distortion_.heat_haze_strength) {
        return;
    }
    screen_distortion_.heat_haze_strength = haze;
    if (!screen_distortion_.active) {
        screen_distortion_.active = true;
        screen_distortion_.screen_uv[0] = screen_uv[0];
        screen_distortion_.screen_uv[1] = screen_uv[1];
        screen_distortion_.ring_radius_uv = 0.0f;
        screen_distortion_.ring_thickness_uv = 0.05f;
        screen_distortion_.refraction_strength = 0.0f;
        screen_distortion_.chromatic_aberration = 0.0f;
        screen_distortion_.radial_blur_strength = 0.0f;
    }
}

void FireVfxSystem::PushUntexturedRing(const float center[3], float inner_r, float outer_r,
                                       uint32_t segments, uint32_t inner_bgra, uint32_t outer_bgra) {
    if (untextured_add_verts_.size() + static_cast<size_t>(segments) * 6U > untextured_add_verts_.capacity()) {
        return;
    }
    const float step = kTwoPi / static_cast<float>(segments);
    for (uint32_t i = 0; i < segments; ++i) {
        const float a0 = static_cast<float>(i) * step;
        const float a1 = static_cast<float>(i + 1U) * step;
        const float c0 = std::cos(a0), s0 = std::sin(a0);
        const float c1 = std::cos(a1), s1 = std::sin(a1);

        const d3d::WorldVertex i0{center[0] + c0 * inner_r, center[1], center[2] + s0 * inner_r, inner_bgra};
        const d3d::WorldVertex o0{center[0] + c0 * outer_r, center[1], center[2] + s0 * outer_r, outer_bgra};
        const d3d::WorldVertex o1{center[0] + c1 * outer_r, center[1], center[2] + s1 * outer_r, outer_bgra};
        const d3d::WorldVertex i1{center[0] + c1 * inner_r, center[1], center[2] + s1 * inner_r, inner_bgra};

        untextured_add_verts_.push_back(i0);
        untextured_add_verts_.push_back(o0);
        untextured_add_verts_.push_back(o1);
        untextured_add_verts_.push_back(i0);
        untextured_add_verts_.push_back(o1);
        untextured_add_verts_.push_back(i1);
    }
}

void FireVfxSystem::EmitBurningFire(const float world_pos[3], float ground_y, float intensity,
                                    float dt, float anim_time) {
    (void)dt;
    if (intensity <= 0.01f || !fire_loop_tex_.IsValid()) {
        return;
    }

    const float clamped_int = std::clamp(intensity, 0.1f, 2.5f);
    const uint32_t seed = SpatialSeed(world_pos);
    const uint32_t total_frames = fire_loop_tex_.cols * fire_loop_tex_.rows;
    const float base_y = std::max(world_pos[1], ground_y);

    // Volumetric rising dark smoke plume (explosion_smoke_flipbook.png)
    const FlipbookTexture& smoke_tex =
        explosion_smoke_tex_.IsValid() ? explosion_smoke_tex_ : fire_loop_tex_;
    const uint32_t smoke_frames = smoke_tex.cols * smoke_tex.rows;
    for (uint32_t i = 0; i < 3U; ++i) {
        const float phase = std::fmod(anim_time * 0.58f + HashFloat01(seed + i * 17U), 1.0f);
        const float pos[3] = {
            world_pos[0] + HashSigned(seed + i * 31U) * 0.24f * clamped_int + std::sin(anim_time * 1.6f + i) * 0.12f,
            base_y + (0.55f + phase * 1.75f) * clamped_int,
            world_pos[2] + HashSigned(seed + i * 43U) * 0.24f * clamped_int,
        };
        const float size = (0.48f + phase * 0.68f) * clamped_int;
        const float alpha = std::sin(phase * kPi) * 0.40f * Clamp01(clamped_int);
        const uint8_t shade = static_cast<uint8_t>(Lerp(185.0f, 138.0f, phase));
        const uint32_t frame = static_cast<uint32_t>(phase * static_cast<float>(smoke_frames));
        const uint32_t bgra = PackBgra(static_cast<uint8_t>(alpha * 255.0f),
                                       shade, static_cast<uint8_t>(shade - 8), static_cast<uint8_t>(shade - 14));
        PushBillboardQuad(smoke_alpha_verts_, pos, size, size, HashSigned(seed + i) * 0.45f,
                          ComputeCellUv(smoke_tex, frame), bgra);
    }

    // Screen-space convective heat shimmer above the fire
    EmitHeatHazeShimmer(world_pos, 0.85f * clamped_int, anim_time, 0.85f * clamped_int, seed);

    // Rich photographic fire tongues (fire_loop_flipbook.png 2048x2048) rendered with 2x gain
    constexpr uint32_t kFlameLayers = 6U;
    for (uint32_t i = 0; i < kFlameLayers; ++i) {
        const float offset = HashFloat01(seed + i * 53U);
        const float speed = 22.0f + static_cast<float>(i) * 3.5f;
        const uint32_t frame = static_cast<uint32_t>(anim_time * speed + offset * total_frames) % total_frames;

        const float scale_w = (0.55f + 0.12f * static_cast<float>(i)) * clamped_int;
        const float scale_h = scale_w * 1.55f;
        const float pos[3] = {
            world_pos[0] + HashSigned(seed + i * 71U) * 0.22f * clamped_int,
            base_y + scale_h * 0.72f + static_cast<float>(i) * 0.08f * clamped_int,
            world_pos[2] + HashSigned(seed + i * 89U) * 0.22f * clamped_int,
        };
        const float flicker = 0.88f + 0.12f * std::sin(anim_time * 16.0f + static_cast<float>(i) * 2.1f);
        const uint8_t a_b = static_cast<uint8_t>(Clamp01(0.96f * flicker * Clamp01(clamped_int)) * 255.0f);
        const uint8_t g_c = static_cast<uint8_t>(Lerp(255.0f, 224.0f, static_cast<float>(i) / 5.0f));
        const uint8_t b_c = static_cast<uint8_t>(Lerp(240.0f, 178.0f, static_cast<float>(i) / 5.0f));
        const uint32_t bgra = PackBgra(a_b, 255, g_c, b_c);

        PushBillboardQuad(fire_loop_add_verts_, pos, scale_w, scale_h,
                          HashSigned(seed + i * 19U) * 0.18f, ComputeCellUv(fire_loop_tex_, frame), bgra);
    }

    // Incandescent sparks rising cleanly above active fire (embers_sparks_flipbook.png at 4x gain)
    if (embers_sparks_tex_.IsValid()) {
        const uint32_t ember_frames = embers_sparks_tex_.cols * embers_sparks_tex_.rows;
        for (uint32_t i = 0; i < 4U; ++i) {
            const float phase = std::fmod(anim_time * (0.90f + 0.20f * static_cast<float>(i & 1U)) +
                                              HashFloat01(seed + i * 131U), 1.0f);
            const float pos[3] = {
                world_pos[0] + HashSigned(seed + i * 137U) * 0.34f * clamped_int + std::sin(anim_time * 3.1f + i) * 0.14f,
                base_y + (0.45f + phase * 1.95f) * clamped_int,
                world_pos[2] + HashSigned(seed + i * 139U) * 0.34f * clamped_int,
            };
            const float sz = (0.34f + 0.18f * phase) * clamped_int;
            const uint8_t ea = static_cast<uint8_t>(std::sin(phase * kPi) * 0.92f * 255.0f);
            const uint32_t frame = (static_cast<uint32_t>(phase * static_cast<float>(ember_frames)) + i * 3U) % ember_frames;
            PushBillboardQuad(embers_add_verts_, pos, sz, sz, phase * 1.4f,
                              ComputeCellUv(embers_sparks_tex_, frame), PackBgra(ea, 255, 240, 200));
        }
    }
}

void FireVfxSystem::EmitExplosion(const float center_pos[3], float radius, float force,
                                  float age, float ttl, float glow_mul) {
    if (ttl <= 1e-4f || age < 0.0f || age > ttl || !explosion_fire_tex_.IsValid()) {
        return;
    }

    const float t = Clamp01(age / ttl);
    const uint32_t seed = SpatialSeed(center_pos);
    const float eff_radius = std::max(0.5f, radius);
    const float eff_force = std::max(0.5f, force);
    const float gm = std::clamp(glow_mul, 0.5f, 2.5f);

    // --- Layer 0: Crisp Strong Screen-Space Refractive Shockwave + Heat Haze ---
    {
        float screen_uv[2]{0.5f, 0.5f};
        float view_z = 10.0f;
        if (ProjectWorldToScreenUv(center_pos, screen_uv, view_z)) {
            const float dist_atten = Clamp01(22.0f / std::max(1.0f, view_z));
            const float haze_val = 0.017f * std::pow(1.0f - t, 1.15f) * dist_atten;

            if (t < 0.42f) {
                const float sw_t = Clamp01(t / 0.42f);
                const float shell_r = eff_radius * Lerp(0.35f, 3.10f, std::pow(sw_t, 0.46f));
                EmitRefractiveShockwaveShell(center_pos, shell_r, sw_t, gm, seed);

                const float ground_r = shell_r * 1.08f;
                const float ring_alpha = std::pow(1.0f - sw_t, 1.65f) * 0.42f * Clamp01(gm);
                const float ring_center[3] = {center_pos[0], center_pos[1] + 0.06f, center_pos[2]};
                PushUntexturedRing(ring_center, ground_r * 0.84f, ground_r, 32U,
                                   PackBgra(0, 245, 220, 180),
                                   PackBgra(static_cast<uint8_t>(ring_alpha * 255.0f), 255, 238, 205));

                const float env = std::sin(sw_t * kPi) * (1.0f - sw_t * 0.45f) * dist_atten;
                const float strength = 0.048f * env * std::clamp(eff_force * 0.35f, 0.85f, 1.65f);
                if (strength > screen_distortion_.refraction_strength) {
                    screen_distortion_.active = true;
                    screen_distortion_.screen_uv[0] = screen_uv[0];
                    screen_distortion_.screen_uv[1] = screen_uv[1];
                    screen_distortion_.ring_radius_uv = Lerp(0.035f, 0.82f, std::pow(sw_t, 0.52f)) * dist_atten;
                    screen_distortion_.ring_thickness_uv = Lerp(0.085f, 0.042f, sw_t);
                    screen_distortion_.refraction_strength = strength;
                    screen_distortion_.chromatic_aberration = 0.0055f * env;
                    screen_distortion_.radial_blur_strength = 0.0f;
                    screen_distortion_.heat_haze_strength = std::max(screen_distortion_.heat_haze_strength, haze_val);

                    const float shake_amp = 0.032f * std::pow(1.0f - sw_t, 1.8f) * dist_atten;
                    screen_distortion_.camera_shake_pitch = std::sin(age * 95.0f) * shake_amp;
                    screen_distortion_.camera_shake_yaw = std::cos(age * 83.0f) * shake_amp * 0.75f;
                    screen_distortion_.camera_shake_roll = std::sin(age * 112.0f) * shake_amp * 0.5f;
                }
            } else if (haze_val > screen_distortion_.heat_haze_strength) {
                screen_distortion_.heat_haze_strength = haze_val;
                if (!screen_distortion_.active) {
                    screen_distortion_.active = true;
                    screen_distortion_.screen_uv[0] = screen_uv[0];
                    screen_distortion_.screen_uv[1] = screen_uv[1];
                    screen_distortion_.ring_radius_uv = 0.0f;
                    screen_distortion_.ring_thickness_uv = 0.06f;
                    screen_distortion_.refraction_strength = 0.0f;
                    screen_distortion_.chromatic_aberration = 0.0f;
                    screen_distortion_.radial_blur_strength = 0.0f;
                }
            }
        }
    }

    // --- Layer 1: Dense Volumetric Smoke Column (explosion_smoke_flipbook.png 2048x2048) ---
    const FlipbookTexture& smoke_tex =
        explosion_smoke_tex_.IsValid() ? explosion_smoke_tex_ : explosion_fire_tex_;
    const uint32_t smoke_frames = smoke_tex.cols * smoke_tex.rows;

    if (t > 0.04f) {
        const float smoke_t = Clamp01((t - 0.04f) / 0.96f);
        const uint32_t smoke_frame = std::min(
            smoke_frames - 1U, static_cast<uint32_t>(smoke_t * static_cast<float>(smoke_frames)));
        const float smoke_alpha = std::sin(std::pow(smoke_t, 0.50f) * kPi) * 0.90f;
        const float warm_bounce = Clamp01(1.0f - t * 1.65f);

        constexpr uint32_t kSmokePuffs = 10U;
        for (uint32_t i = 0; i < kSmokePuffs; ++i) {
            const float ang = HashFloat01(seed + i * 29U) * kTwoPi;
            const float spread = (0.12f + 0.58f * HashFloat01(seed + i * 37U)) * eff_radius;
            const float rise = (0.22f + 1.40f * smoke_t + 0.38f * HashFloat01(seed + i * 41U)) * eff_radius;
            const float pos[3] = {
                center_pos[0] + std::cos(ang) * spread * (0.35f + 0.65f * smoke_t),
                center_pos[1] + rise,
                center_pos[2] + std::sin(ang) * spread * (0.35f + 0.65f * smoke_t),
            };
            const float puff_size = eff_radius * (0.55f + 0.95f * smoke_t);
            const uint8_t r_s = static_cast<uint8_t>(Lerp(148.0f, 232.0f, warm_bounce) - HashFloat01(seed + i * 11U) * 24.0f);
            const uint8_t g_s = static_cast<uint8_t>(Lerp(140.0f, 198.0f, warm_bounce) - HashFloat01(seed + i * 11U) * 24.0f);
            const uint8_t b_s = static_cast<uint8_t>(Lerp(134.0f, 168.0f, warm_bounce) - HashFloat01(seed + i * 11U) * 24.0f);
            const uint32_t bgra = PackBgra(static_cast<uint8_t>(Clamp01(smoke_alpha) * 255.0f), r_s, g_s, b_s);
            PushBillboardQuad(smoke_alpha_verts_, pos, puff_size, puff_size,
                              HashSigned(seed + i * 13U) * 1.1f + smoke_t * 0.35f,
                              ComputeCellUv(smoke_tex, smoke_frame + (i & 1U)), bgra);
        }
    }

    // --- Layer 2: Blazing Detonation Core + Rolling Fireball (explosion_fire_flipbook.png at 2x gain) ---
    const uint32_t fire_frames = explosion_fire_tex_.cols * explosion_fire_tex_.rows;
    if (t < 0.78f) {
        const float fire_t = Clamp01(t / 0.78f);
        const uint32_t fire_frame = std::min(
            fire_frames - 1U, static_cast<uint32_t>(fire_t * static_cast<float>(fire_frames)));
        const float expand = std::pow(fire_t, 0.34f);
        const float fire_alpha = std::pow(1.0f - fire_t, 1.25f) * Clamp01(gm);
        const uint8_t g_tint = static_cast<uint8_t>(Lerp(255.0f, 212.0f, fire_t));
        const uint8_t b_tint = static_cast<uint8_t>(Lerp(248.0f, 156.0f, fire_t));
        const uint32_t fire_bgra = PackBgra(static_cast<uint8_t>(fire_alpha * 255.0f), 255, g_tint, b_tint);

        constexpr uint32_t kFireBillboards = 9U;
        for (uint32_t i = 0; i < kFireBillboards; ++i) {
            const float ang = HashFloat01(seed + i * 59U) * kTwoPi;
            const float radial = (i == 0U) ? 0.0f : (0.14f + 0.28f * HashFloat01(seed + i * 61U)) * eff_radius * expand;
            const float pos[3] = {
                center_pos[0] + std::cos(ang) * radial,
                center_pos[1] + eff_radius * (0.24f + 0.78f * expand) + static_cast<float>(i) * 0.14f,
                center_pos[2] + std::sin(ang) * radial,
            };
            const float sz = eff_radius * ((i == 0U) ? (0.82f + 0.88f * expand) : (0.56f + 0.72f * expand));
            PushBillboardQuad(explosion_add_verts_, pos, sz, sz,
                              HashSigned(seed + i * 67U) * 0.70f,
                              ComputeCellUv(explosion_fire_tex_, fire_frame + (i & 1U)),
                              fire_bgra);
        }
    }

    // Roaring base/crater fire tongues from fire_loop_flipbook.png (2x gain)
    if (fire_loop_tex_.IsValid() && t < 0.90f) {
        const float base_t = Clamp01(t / 0.90f);
        const float base_alpha = std::sin(std::min(1.0f, base_t * 3.5f) * (kPi * 0.5f)) *
                                 (1.0f - base_t) * Clamp01(0.95f * gm);
        const uint32_t fl_frames = fire_loop_tex_.cols * fire_loop_tex_.rows;
        const uint8_t b_a = static_cast<uint8_t>(base_alpha * 255.0f);
        for (uint32_t i = 0; i < 4U; ++i) {
            const float ang = HashFloat01(seed + i * 149U) * kTwoPi;
            const float dist = (0.14f + 0.24f * static_cast<float>(i)) * eff_radius;
            const float sw = eff_radius * 0.45f;
            const float sh = sw * 1.55f;
            const float pos[3] = {
                center_pos[0] + std::cos(ang) * dist,
                center_pos[1] + sh * 0.62f,
                center_pos[2] + std::sin(ang) * dist,
            };
            const uint32_t fl_frame = static_cast<uint32_t>(age * 24.0f + i * 11U) % fl_frames;
            PushBillboardQuad(fire_loop_add_verts_, pos, sw, sh,
                              HashSigned(seed + i * 151U) * 0.20f,
                              ComputeCellUv(fire_loop_tex_, fl_frame),
                              PackBgra(b_a, 255, 240, 200));
        }
    }

    // --- Layer 3: Intense Flying Sparks & Incandescent Embers (embers_sparks_flipbook.png at 4x gain) ---
    const FlipbookTexture& ember_tex =
        embers_sparks_tex_.IsValid() ? embers_sparks_tex_ : explosion_fire_tex_;
    const uint32_t ember_frames = ember_tex.cols * ember_tex.rows;

    if (t > 0.01f && t < 0.98f) {
        const float ember_t = Clamp01((t - 0.01f) / 0.97f);
        const float drag = 3.8f;
        const float drag_factor = (1.0f - std::exp(-drag * ember_t)) / drag;
        const float ember_fade = std::sin(std::pow(ember_t, 0.55f) * kPi) * Clamp01(gm);
        const uint32_t ember_bgra = PackBgra(
            static_cast<uint8_t>(ember_fade * 255.0f),
            255,
            static_cast<uint8_t>(Lerp(255.0f, 212.0f, ember_t)),
            static_cast<uint8_t>(Lerp(235.0f, 160.0f, ember_t)));

        constexpr uint32_t kEmberClusters = 24U;
        for (uint32_t i = 0; i < kEmberClusters; ++i) {
            const float azimuth = HashFloat01(seed + i * 97U) * kTwoPi;
            const float elev = 0.16f + 0.78f * HashFloat01(seed + i * 101U);
            const float speed = eff_radius * (2.4f + 2.2f * HashFloat01(seed + i * 103U)) *
                                std::clamp(eff_force * 0.25f, 0.80f, 2.3f);

            const float vx = std::cos(azimuth) * std::cos(elev) * speed;
            const float vy = std::sin(elev) * speed * 1.50f;
            const float vz = std::sin(azimuth) * std::cos(elev) * speed;

            const float turb_x = std::sin(age * 4.5f + static_cast<float>(i) * 1.7f) * eff_radius * 0.12f * ember_t;
            const float turb_z = std::cos(age * 4.1f + static_cast<float>(i) * 2.3f) * eff_radius * 0.12f * ember_t;
            const float gravity_drop = 0.5f * 4.2f * ember_t * ember_t * eff_radius * 0.26f;

            const float pos[3] = {
                center_pos[0] + vx * drag_factor + turb_x,
                center_pos[1] + vy * drag_factor - gravity_drop,
                center_pos[2] + vz * drag_factor + turb_z,
            };

            const float cluster_sz = eff_radius * (0.26f + 0.18f * HashFloat01(seed + i * 107U)) *
                                     (1.0f - 0.15f * ember_t);
            const uint32_t frame = std::min(
                ember_frames - 1U,
                static_cast<uint32_t>(ember_t * static_cast<float>(ember_frames) + (i & 3U)));

            PushBillboardQuad(embers_add_verts_, pos, cluster_sz, cluster_sz,
                              azimuth + ember_t * 1.4f, ComputeCellUv(ember_tex, frame),
                              ember_bgra);
        }
    }
}

void FireVfxSystem::EmitFlameStream(const FlameStreamPoint* points, uint32_t point_count,
                                    float ground_y, float stream_fade, float anim_time) {
    if (!points || point_count == 0 || stream_fade <= 0.01f || !fire_loop_tex_.IsValid()) {
        return;
    }

    const uint32_t total_frames = fire_loop_tex_.cols * fire_loop_tex_.rows;
    const float fade = Clamp01(stream_fade);

    for (uint32_t i = 0; i < point_count; ++i) {
        const FlameStreamPoint& pt = points[i];
        const float heat = Clamp01(pt.heat);
        const float rad = std::max(0.05f, pt.radius);

        // Cool tail transitions into alpha-blended dark smoke
        if (heat < 0.48f) {
            const FlipbookTexture& smoke_tex =
                explosion_smoke_tex_.IsValid() ? explosion_smoke_tex_ : fire_loop_tex_;
            const uint32_t smoke_frames = smoke_tex.cols * smoke_tex.rows;
            const uint32_t frame = static_cast<uint32_t>(anim_time * 18.0f + i * 5U) % smoke_frames;
            const float alpha = (1.0f - heat / 0.48f) * 0.54f * fade;
            const uint32_t bgra = PackBgra(static_cast<uint8_t>(alpha * 255.0f), 168, 156, 148);
            PushBillboardQuad(smoke_alpha_verts_, pt.pos, rad * 1.35f, rad * 1.35f,
                              static_cast<float>(i) * 0.7f, ComputeCellUv(smoke_tex, frame), bgra);
        }

        // Velocity-aligned stretched fireball quad along the flamethrower stream
        float dir[3] = {pt.vel[0], pt.vel[1], pt.vel[2]};
        const float speed = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        const uint32_t frame = static_cast<uint32_t>(anim_time * 28.0f + i * 3U) % total_frames;
        const uint8_t f_alpha = static_cast<uint8_t>(Clamp01(0.92f * fade * (0.40f + 0.60f * heat)) * 255.0f);
        const uint32_t fire_bgra = PackBgra(
            f_alpha, 255,
            static_cast<uint8_t>(Lerp(196.0f, 248.0f, heat)),
            static_cast<uint8_t>(Lerp(132.0f, 226.0f, heat)));

        if (speed > 0.5f) {
            Normalize3(dir);
            float side[3]{};
            Cross3(dir, camera_.forward, side);
            Normalize3(side);

            const float stretch = std::clamp(1.0f + speed * 0.06f, 1.0f, 2.6f);
            const float axis_u[3] = {side[0] * rad, side[1] * rad, side[2] * rad};
            const float axis_v[3] = {dir[0] * rad * stretch, dir[1] * rad * stretch, dir[2] * rad * stretch};
            PushOrientedQuad(fire_loop_add_verts_, pt.pos, axis_u, axis_v,
                             ComputeCellUv(fire_loop_tex_, frame), fire_bgra);
        } else {
            PushBillboardQuad(fire_loop_add_verts_, pt.pos, rad, rad,
                              static_cast<float>(i) * 0.4f, ComputeCellUv(fire_loop_tex_, frame), fire_bgra);
        }

        // Burning fuel pool on ground_y when stream touches or skims the floor
        if (pt.pos[1] <= ground_y + rad * 1.4f && heat > 0.25f) {
            const float pool_pos[3] = {pt.pos[0], ground_y + 0.04f, pt.pos[2]};
            const float pool_r = rad * 1.25f;
            const float axis_u[3] = {pool_r, 0.0f, 0.0f};
            const float axis_v[3] = {0.0f, 0.0f, pool_r};
            const uint32_t pool_bgra = PackBgra(
                static_cast<uint8_t>(Clamp01(0.75f * fade) * 255.0f), 255, 224, 178);
            PushOrientedQuad(fire_loop_add_verts_, pool_pos, axis_u, axis_v,
                             ComputeCellUv(fire_loop_tex_, frame + 7U), pool_bgra);
        }
    }
}

void FireVfxSystem::FlushTexturedBatch(const std::vector<d3d::TexturedWorldVertex>& verts,
                                       FlipbookSlot slot, bool additive, int color_gain_x) {
    if (verts.empty() || !camera_.view || !camera_.proj) {
        return;
    }
    if (bind_fn_) {
        bind_fn_(slot, bind_user_data_);
    }
    d3d::DrawTexturedWorldOverlay(verts.data(), static_cast<uint32_t>(verts.size()),
                                  camera_.view, camera_.proj, additive, color_gain_x);
}

void FireVfxSystem::Flush() {
    if (!camera_.view || !camera_.proj) {
        return;
    }

    // 1. Alpha-blended dark billowing smoke first so all fire, embers, and shockwaves glow against it
    const FlipbookSlot smoke_slot =
        explosion_smoke_tex_.IsValid() ? FlipbookSlot::ExplosionSmoke : FlipbookSlot::ExplosionFire;
    FlushTexturedBatch(smoke_alpha_verts_, smoke_slot, false, 1);

    // 2. Additive fire loops & flamethrower stream (fire_loop_flipbook.png) with 2x incandescent gain
    FlushTexturedBatch(fire_loop_add_verts_, FlipbookSlot::FireLoop, true, 2);

    // 3. Additive core explosion fireball (explosion_fire_flipbook.png) with 2x gain
    FlushTexturedBatch(explosion_add_verts_, FlipbookSlot::ExplosionFire, true, 2);

    // 4. Additive high-drag glowing ember clusters & sparks (embers_sparks_flipbook.png) with 4x spark brilliance
    const FlipbookSlot ember_slot =
        embers_sparks_tex_.IsValid() ? FlipbookSlot::EmbersSparks : FlipbookSlot::ExplosionFire;
    FlushTexturedBatch(embers_add_verts_, ember_slot, true, 4);

    // 5. Untextured subtle ground Mach pressure ring (shockwave_normal.png refraction runs in Vulkan shader)
    if (!untextured_add_verts_.empty()) {
        d3d::DrawWorldOverlay(untextured_add_verts_.data(),
                              static_cast<uint32_t>(untextured_add_verts_.size()),
                              camera_.view, camera_.proj, true);
    }
}

}  // namespace vfx