#include "vfx/blood_vfx_system.h"

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

inline float Dot3(const float a[3], const float b[3]) noexcept {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline void Normalize3(float v[3]) noexcept {
    const float len_sq = Dot3(v, v);
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

// Builds a surface-aligned basis where tangent_v points down the steepest gravity slope
// on walls/sloped surfaces, so blood drips always run physically downward.
inline void BuildGravityAlignedSurfaceBasis(const float normal_in[3],
                                            float out_normal[3],
                                            float out_tangent_u[3],
                                            float out_tangent_v[3]) noexcept {
    out_normal[0] = normal_in[0];
    out_normal[1] = normal_in[1];
    out_normal[2] = normal_in[2];
    Normalize3(out_normal);

    // If surface is nearly horizontal (floor/ceiling), use world X/Z basis
    if (std::abs(out_normal[1]) > 0.92f) {
        const float ref[3] = {1.0f, 0.0f, 0.0f};
        Cross3(out_normal, ref, out_tangent_v);
        Normalize3(out_tangent_v);
        Cross3(out_tangent_v, out_normal, out_tangent_u);
        Normalize3(out_tangent_u);
        return;
    }

    // Project world up (0, 1, 0) onto the surface plane so +tangent_v points UP along the wall
    // (top of quad = +tangent_v, bottom of quad = -tangent_v where drips hang downward).
    const float world_up[3] = {0.0f, 1.0f, 0.0f};
    const float d = Dot3(world_up, out_normal);
    out_tangent_v[0] = world_up[0] - d * out_normal[0];
    out_tangent_v[1] = world_up[1] - d * out_normal[1];
    out_tangent_v[2] = world_up[2] - d * out_normal[2];
    Normalize3(out_tangent_v);

    Cross3(out_tangent_v, out_normal, out_tangent_u);
    Normalize3(out_tangent_u);
}

}  // namespace

void BloodFlipbookTexture::SampleWetNormal(float u, float v,
                                           float& out_nx, float& out_ny, float& out_gloss) const noexcept {
    if (!IsValid() || pixels_argb8888.empty()) {
        out_nx = 0.0f;
        out_ny = 0.0f;
        out_gloss = 0.5f;
        return;
    }
    const float wu = u - std::floor(u);
    const float wv = v - std::floor(v);
    const uint32_t x = static_cast<uint32_t>(Clamp01(wu) * static_cast<float>(width - 1U));
    const uint32_t y = static_cast<uint32_t>(Clamp01(wv) * static_cast<float>(height - 1U));
    const uint32_t px = pixels_argb8888[static_cast<size_t>(y) * width + x];

    const uint8_t a = static_cast<uint8_t>((px >> 24) & 0xFFU);
    const uint8_t r = static_cast<uint8_t>((px >> 16) & 0xFFU);
    const uint8_t g = static_cast<uint8_t>((px >> 8) & 0xFFU);

    out_nx = (static_cast<float>(r) - 128.0f) / 127.0f;
    out_ny = (static_cast<float>(g) - 128.0f) / 127.0f;
    out_gloss = static_cast<float>(a) / 255.0f;
}

bool BloodVfxSystem::Initialize(const BloodVfxConfig& config) {
    config_ = config;

    bursts_.assign(config_.max_active_bursts, ActiveBurst{});
    droplets_.assign(config_.max_ballistic_droplets, BallisticDroplet{});
    wall_decals_.assign(config_.max_wall_splatters, WallSplatterDecal{});
    ground_pools_.assign(config_.max_ground_pools, GroundPoolDecal{});

    const size_t tex_vert_cap = static_cast<size_t>(config_.max_textured_quads_per_batch) * 6U;
    pool_alpha_verts_.reserve(tex_vert_cap / 2U);
    wall_alpha_verts_.reserve(tex_vert_cap / 2U);
    mist_alpha_verts_.reserve(tex_vert_cap);
    wet_specular_add_verts_.reserve(tex_vert_cap / 2U);
    untextured_droplets_verts_.reserve(config_.max_untextured_verts_per_batch);

    const bool ok_mist = LoadFlipbookPng(config_.mist_flipbook_path, config_.mist_cols,
                                         config_.mist_rows, config_.cell_uv_padding_px, mist_tex_, true);
    const bool ok_wall = LoadFlipbookPng(config_.wall_atlas_path, config_.wall_cols,
                                         config_.wall_rows, config_.cell_uv_padding_px, wall_tex_, true);
    const bool ok_pool = LoadFlipbookPng(config_.ground_pool_path, config_.pool_cols,
                                         config_.pool_rows, config_.cell_uv_padding_px, pool_tex_, true);
    LoadFlipbookPng(config_.wet_normal_path, 1U, 1U, 0.0f, wet_normal_tex_, false);

    return ok_mist && ok_wall && ok_pool;
}

void BloodVfxSystem::Shutdown() {
    mist_tex_ = BloodFlipbookTexture{};
    wall_tex_ = BloodFlipbookTexture{};
    pool_tex_ = BloodFlipbookTexture{};
    wet_normal_tex_ = BloodFlipbookTexture{};

    bursts_.clear();
    droplets_.clear();
    wall_decals_.clear();
    ground_pools_.clear();

    pool_alpha_verts_.clear();
    wall_alpha_verts_.clear();
    mist_alpha_verts_.clear();
    wet_specular_add_verts_.clear();
    untextured_droplets_verts_.clear();
}

void BloodVfxSystem::ClearAllDecals() noexcept {
    for (auto& b : bursts_) b.active = false;
    for (auto& d : droplets_) d.active = false;
    for (auto& w : wall_decals_) w.active = false;
    for (auto& p : ground_pools_) p.active = false;
}

bool BloodVfxSystem::LoadFlipbookPng(const std::string& path, uint32_t cols, uint32_t rows,
                                     float padding_px, BloodFlipbookTexture& out_tex,
                                     bool decontaminate_edges) {
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
    out_tex.released_cpu = false;
    out_tex.pixels_argb8888.resize(static_cast<size_t>(out_tex.width) * out_tex.height);

    const uint8_t* src_bytes = static_cast<const uint8_t*>(converted->pixels);
    for (uint32_t y = 0; y < out_tex.height; ++y) {
        const uint8_t* row_ptr = src_bytes + static_cast<size_t>(y) * converted->pitch;
        uint32_t* dst_ptr = out_tex.pixels_argb8888.data() + static_cast<size_t>(y) * out_tex.width;
        std::copy_n(reinterpret_cast<const uint32_t*>(row_ptr), out_tex.width, dst_ptr);
    }

    SDL_DestroySurface(converted);

    if (decontaminate_edges && out_tex.width > 1U && out_tex.height > 1U) {
        const uint32_t w = out_tex.width;
        const uint32_t h = out_tex.height;
        uint32_t* px = out_tex.pixels_argb8888.data();

        // Pass 1: Strip semi-transparent white/pink canvas matte on low-alpha edges while preserving
        // rich opaque deep crimson core & wet specular highlights intact.
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            const uint32_t c = px[i];
            const uint8_t a = static_cast<uint8_t>((c >> 24) & 0xFFU);
            if (a == 0U) {
                px[i] = 0x00460305U;
                continue;
            }
            if (a < 235U) {
                const uint8_t r = static_cast<uint8_t>((c >> 16) & 0xFFU);
                const uint8_t g = static_cast<uint8_t>((c >> 8) & 0xFFU);
                const uint8_t b = static_cast<uint8_t>(c & 0xFFU);
                const float min_gb = static_cast<float>(std::min(g, b));
                const float excess_white = min_gb - 10.0f;
                if (excess_white > 0.5f) {
                    const float af = std::max(0.36f, static_cast<float>(a) / 255.0f);
                    const float cr = std::clamp((static_cast<float>(r) - excess_white * 0.72f) / af, 40.0f, 102.0f);
                    const float cg = std::min(std::max(0.0f, static_cast<float>(g) - excess_white), cr * 0.11f);
                    const float cb = std::min(std::max(0.0f, static_cast<float>(b) - excess_white), cr * 0.13f);
                    px[i] = PackBgra(a,
                                     static_cast<uint8_t>(cr + 0.5f),
                                     static_cast<uint8_t>(cg + 0.5f),
                                     static_cast<uint8_t>(cb + 0.5f));
                }
            }
        }

        // Pass 2: 1-pixel RGB color bleed into adjacent alpha==0 texels so bilinear GPU filtering
        // never pulls [0,0,0] dark borders onto sharp pool or splatter silhouettes.
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const size_t idx = static_cast<size_t>(y) * w + x;
                if ((px[idx] >> 24) != 0U) {
                    continue;
                }
                const uint32_t nb[4] = {
                    (x > 0U) ? px[idx - 1U] : 0U,
                    (x + 1U < w) ? px[idx + 1U] : 0U,
                    (y > 0U) ? px[idx - w] : 0U,
                    (y + 1U < h) ? px[idx + w] : 0U,
                };
                for (uint32_t n : nb) {
                    if ((n >> 24) >= 24U) {
                        px[idx] = (n & 0x00FFFFFFU);
                        break;
                    }
                }
            }
        }
    }

    return out_tex.IsValid();
}

BloodVfxSystem::CellUv BloodVfxSystem::ComputeCellUv(const BloodFlipbookTexture& tex,
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

uint32_t BloodVfxSystem::BloodColorRamp(float freshness_01, float lighting_scale, float alpha_01) noexcept {
    const float f = Clamp01(freshness_01);
    const float l = std::clamp(lighting_scale, 0.75f, 1.25f);
    const float a = Clamp01(alpha_01);

    // Blood PNGs already carry authentic dark crimson hues; trim excess green/blue and
    // deepen slightly for rich dark red arterial/venous blood without ever crushing to black.
    const float r = Clamp01(Lerp(0.56f, 0.76f, f) * l);
    const float g = Clamp01(Lerp(0.24f, 0.36f, f * f) * l);
    const float b = Clamp01(Lerp(0.26f, 0.38f, f * f) * l);

    return PackBgra(static_cast<uint8_t>(a * 255.0f),
                    static_cast<uint8_t>(r * 255.0f),
                    static_cast<uint8_t>(g * 255.0f),
                    static_cast<uint8_t>(b * 255.0f));
}

float BloodVfxSystem::ComputeWetSpecularHighlight(const float world_pos[3], const float normal[3],
                                                  float wetness, float uv_phase) const noexcept {
    if (wetness <= 0.01f) {
        return 0.0f;
    }

    float view_dir[3] = {
        camera_.eye[0] - world_pos[0],
        camera_.eye[1] - world_pos[1],
        camera_.eye[2] - world_pos[2],
    };
    Normalize3(view_dir);

    float light_to_surf[3] = {-camera_.light_dir[0], -camera_.light_dir[1], -camera_.light_dir[2]};
    Normalize3(light_to_surf);

    float half_vec[3] = {
        view_dir[0] + light_to_surf[0],
        view_dir[1] + light_to_surf[1],
        view_dir[2] + light_to_surf[2],
    };
    Normalize3(half_vec);

    float nx = 0.0f, ny = 0.0f, gloss = 0.65f;
    if (wet_normal_tex_.IsValid()) {
        wet_normal_tex_.SampleWetNormal(
            world_pos[0] * 0.85f + uv_phase, world_pos[2] * 0.85f - uv_phase, nx, ny, gloss);
    }

    float pert_n[3] = {
        normal[0] + nx * 0.25f,
        normal[1] + 0.10f,
        normal[2] + ny * 0.25f,
    };
    Normalize3(pert_n);

    const float ndoth = std::max(0.0f, Dot3(pert_n, half_vec));
    const float spec = std::pow(ndoth, 24.0f) * gloss * wetness * camera_.light_intensity;
    return Clamp01(spec);
}

void BloodVfxSystem::PushBillboardQuad(std::vector<d3d::TexturedWorldVertex>& dst,
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

void BloodVfxSystem::PushOrientedQuad(std::vector<d3d::TexturedWorldVertex>& dst,
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

void BloodVfxSystem::SpawnImpactBurst(const float hit_pos[3], const float impact_dir[3],
                                      float ground_y, float caliber_intensity,
                                      uint32_t droplet_count) {
    if (bursts_.empty()) {
        return;
    }

    const float clamped_int = std::clamp(caliber_intensity, 0.35f, 2.5f);
    const uint32_t seed = HashU32(spawn_counter_++ * 2654435761U);

    // 1. Register airborne Flipbook burst (arterial exit jet + atomized mist)
    ActiveBurst& b = bursts_[next_burst_idx_];
    next_burst_idx_ = (next_burst_idx_ + 1U) % static_cast<uint32_t>(bursts_.size());
    b.active = true;
    b.pos[0] = hit_pos[0];
    b.pos[1] = hit_pos[1];
    b.pos[2] = hit_pos[2];
    b.dir[0] = impact_dir[0];
    b.dir[1] = impact_dir[1];
    b.dir[2] = impact_dir[2];
    Normalize3(b.dir);
    const float jet_speed = 0.85f + 0.55f * clamped_int;
    b.vel[0] = b.dir[0] * jet_speed;
    b.vel[1] = b.dir[1] * jet_speed + 0.25f;
    b.vel[2] = b.dir[2] * jet_speed;
    b.ground_y = ground_y;
    b.radius = 0.24f * clamped_int;
    b.age = 0.0f;
    b.ttl = 0.26f + 0.07f * clamped_int;
    b.intensity = clamped_int;
    b.seed = seed;

    // 2. Spawn physical ballistic droplets that arc with gravity & air drag to walls or floor pools
    if (droplets_.empty()) {
        return;
    }
    const uint32_t count = std::min(droplet_count, 32U);
    for (uint32_t i = 0; i < count; ++i) {
        BallisticDroplet& d = droplets_[next_droplet_idx_];
        next_droplet_idx_ = (next_droplet_idx_ + 1U) % static_cast<uint32_t>(droplets_.size());

        const uint32_t dseed = seed + i * 79U;
        const bool entry_splash = (i % 5U == 0U);
        const float dir_sign = entry_splash ? -0.45f : 1.0f;
        const float speed = (entry_splash ? (1.4f + 2.2f * HashFloat01(dseed))
                                          : (2.6f + 4.8f * HashFloat01(dseed))) *
                            std::sqrt(clamped_int);
        const float spread = entry_splash ? 0.58f : 0.42f;

        float dir[3] = {
            b.dir[0] * dir_sign + HashSigned(dseed + 11U) * spread,
            b.dir[1] * dir_sign + (0.08f + 0.52f * HashFloat01(dseed + 23U)) * spread,
            b.dir[2] * dir_sign + HashSigned(dseed + 37U) * spread,
        };
        Normalize3(dir);

        d.active = true;
        d.pos[0] = hit_pos[0] + dir[0] * 0.05f;
        d.pos[1] = hit_pos[1] + dir[1] * 0.05f;
        d.pos[2] = hit_pos[2] + dir[2] * 0.05f;
        d.vel[0] = dir[0] * speed;
        d.vel[1] = dir[1] * speed;
        d.vel[2] = dir[2] * speed;
        d.ground_y = ground_y;
        d.radius = (0.011f + 0.014f * HashFloat01(dseed + 53U)) *
                   std::clamp(clamped_int, 0.75f, 1.45f);
        d.age = 0.0f;
        d.ttl = 1.45f;
        d.seed = dseed;
    }
}

void BloodVfxSystem::SpawnWoundDripDroplet(const float wound_pos[3], const float spray_dir[3],
                                           float ground_y, float speed, float radius) {
    if (droplets_.empty()) {
        return;
    }
    BallisticDroplet& d = droplets_[next_droplet_idx_];
    next_droplet_idx_ = (next_droplet_idx_ + 1U) % static_cast<uint32_t>(droplets_.size());

    const uint32_t dseed = HashU32(spawn_counter_++ * 2246822519U);
    float dir[3] = {spray_dir[0], spray_dir[1], spray_dir[2]};
    Normalize3(dir);

    const float spd = std::clamp(speed, 0.15f, 3.2f) * (0.75f + 0.50f * HashFloat01(dseed + 3U));
    d.active = true;
    d.pos[0] = wound_pos[0] + HashSigned(dseed + 7U) * 0.04f;
    d.pos[1] = wound_pos[1] + HashSigned(dseed + 13U) * 0.03f;
    d.pos[2] = wound_pos[2] + HashSigned(dseed + 19U) * 0.04f;
    d.vel[0] = dir[0] * spd + HashSigned(dseed + 29U) * 0.35f;
    d.vel[1] = dir[1] * spd * 0.45f - 0.15f;
    d.vel[2] = dir[2] * spd + HashSigned(dseed + 41U) * 0.35f;
    d.ground_y = ground_y;
    d.radius = std::clamp(radius, 0.009f, 0.024f);
    d.age = 0.0f;
    d.ttl = 1.25f;
    d.seed = dseed;
}

void BloodVfxSystem::SpawnSurfaceSplatter(const float surface_pos[3], const float surface_normal[3],
                                          const float spray_dir[3], float radius,
                                          float drip_speed, float lifetime_sec) {
    if (wall_decals_.empty()) {
        return;
    }

    // If the surface is actually flat ground (normal.y > 0.78), route exclusively to ground pools
    // so vertical drip strips from blood_wall_splatter_atlas are never stamped flat across floors.
    if (surface_normal[1] > 0.78f) {
        SpawnOrGrowGroundPool(surface_pos, surface_normal, radius * 1.15f, 3.2f, lifetime_sec);
        return;
    }

    uint32_t chosen_wall = UINT32_MAX;
    for (uint32_t i = 0; i < static_cast<uint32_t>(wall_decals_.size()); ++i) {
        const uint32_t idx = (next_wall_idx_ + i) % static_cast<uint32_t>(wall_decals_.size());
        if (!wall_decals_[idx].active) {
            chosen_wall = idx;
            break;
        }
    }
    if (chosen_wall == UINT32_MAX) {
        float best_score = -1.0f;
        for (uint32_t i = 0; i < static_cast<uint32_t>(wall_decals_.size()); ++i) {
            const WallSplatterDecal& cur = wall_decals_[i];
            const float dx = cur.pos[0] - camera_.eye[0];
            const float dy = cur.pos[1] - camera_.eye[1];
            const float dz = cur.pos[2] - camera_.eye[2];
            const float score = (dx * dx + dy * dy + dz * dz) + cur.age * 2.0f;
            if (score > best_score) {
                best_score = score;
                chosen_wall = i;
            }
        }
        if (chosen_wall == UINT32_MAX) {
            chosen_wall = next_wall_idx_;
        }
    }
    next_wall_idx_ = (chosen_wall + 1U) % static_cast<uint32_t>(wall_decals_.size());
    WallSplatterDecal& w = wall_decals_[chosen_wall];

    const uint32_t seed = HashU32(spawn_counter_++ * 1103515245U);
    BuildGravityAlignedSurfaceBasis(surface_normal, w.normal, w.tangent_u, w.tangent_v);

    // Offset slightly along surface normal (3mm..8mm) to prevent Z-fighting with wall geometry
    const float z_bias = 0.005f + 0.004f * HashFloat01(seed + 7U);
    w.active = true;
    w.pos[0] = surface_pos[0] + w.normal[0] * z_bias;
    w.pos[1] = surface_pos[1] + w.normal[1] * z_bias;
    w.pos[2] = surface_pos[2] + w.normal[2] * z_bias;

    // Directional elongation when blood hits wall at a grazing angle
    float spray_n[3] = {spray_dir[0], spray_dir[1], spray_dir[2]};
    Normalize3(spray_n);
    const float grazing = 1.0f - std::abs(Dot3(spray_n, w.normal));
    const float stretch = 1.0f + grazing * 0.55f;

    w.radius_u = std::max(0.08f, radius * (0.85f + 0.30f * HashFloat01(seed + 13U)));
    w.radius_v = w.radius_u * stretch;
    w.drip_stretch = 0.0f;
    w.drip_speed = std::max(0.0f, drip_speed);
    w.age = 0.0f;
    w.ttl = (config_.persistent_decals || lifetime_sec <= 0.0f)
                ? 0.0f
                : std::max(2.0f, lifetime_sec);
    w.wetness = 1.0f;

    const uint32_t total_wall_frames = wall_tex_.cols * wall_tex_.rows;
    w.atlas_frame = total_wall_frames > 0 ? (seed % total_wall_frames) : 0U;
    w.seed = seed;
}

void BloodVfxSystem::SpawnOrGrowGroundPool(const float ground_pos[3], const float ground_normal[3],
                                           float target_radius, float spread_duration_sec,
                                           float lifetime_sec) {
    if (ground_pools_.empty()) {
        return;
    }

    const bool make_permanent = config_.persistent_decals || (lifetime_sec <= 0.0f);

    // Check if an existing active pool is nearby so droplets feed & expand the same puddle organically
    const float merge_r = std::max(0.42f, target_radius * 0.90f);
    const float merge_dist_sq = merge_r * merge_r;
    GroundPoolDecal* nearest_fallback = nullptr;
    float nearest_d2 = 0.95f * 0.95f;
    for (auto& existing : ground_pools_) {
        if (!existing.active) {
            continue;
        }
        const float dx = existing.pos[0] - ground_pos[0];
        const float dy = existing.pos[1] - ground_pos[1];
        const float dz = existing.pos[2] - ground_pos[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= merge_dist_sq) {
            existing.target_radius = std::min(1.95f, std::hypot(existing.target_radius, target_radius * 0.42f));
            existing.wetness = 1.0f;
            existing.ttl = (make_permanent || existing.ttl <= 0.0f)
                               ? 0.0f
                               : std::max(existing.ttl, existing.age + lifetime_sec * 0.5f);
            return;
        }
        if (d2 < nearest_d2) {
            nearest_d2 = d2;
            nearest_fallback = &existing;
        }
    }

    uint32_t chosen_pool = UINT32_MAX;
    for (uint32_t i = 0; i < static_cast<uint32_t>(ground_pools_.size()); ++i) {
        const uint32_t idx = (next_pool_idx_ + i) % static_cast<uint32_t>(ground_pools_.size());
        if (!ground_pools_[idx].active) {
            chosen_pool = idx;
            break;
        }
    }
    if (chosen_pool == UINT32_MAX) {
        if (target_radius < 0.36f && nearest_fallback) {
            nearest_fallback->target_radius =
                std::min(1.95f, std::hypot(nearest_fallback->target_radius, target_radius * 0.38f));
            nearest_fallback->wetness = 1.0f;
            if (make_permanent) {
                nearest_fallback->ttl = 0.0f;
            }
            return;
        }
        float best_score = -1.0f;
        for (uint32_t i = 0; i < static_cast<uint32_t>(ground_pools_.size()); ++i) {
            const GroundPoolDecal& cur = ground_pools_[i];
            const float dx = cur.pos[0] - camera_.eye[0];
            const float dy = cur.pos[1] - camera_.eye[1];
            const float dz = cur.pos[2] - camera_.eye[2];
            const float score = (dx * dx + dy * dy + dz * dz) + cur.age * 1.5f;
            if (score > best_score) {
                best_score = score;
                chosen_pool = i;
            }
        }
        if (chosen_pool == UINT32_MAX) {
            chosen_pool = next_pool_idx_;
        }
    }
    next_pool_idx_ = (chosen_pool + 1U) % static_cast<uint32_t>(ground_pools_.size());
    GroundPoolDecal& p = ground_pools_[chosen_pool];

    const uint32_t seed = HashU32(spawn_counter_++ * 1664525U);
    BuildGravityAlignedSurfaceBasis(ground_normal, p.normal, p.tangent_u, p.tangent_v);

    // Randomize rotation in the ground plane so every pool has a unique organic silhouette
    const float angle = HashFloat01(seed) * kTwoPi;
    const float cs = std::cos(angle), sn = std::sin(angle);
    float u_rot[3]{}, v_rot[3]{};
    for (int i = 0; i < 3; ++i) {
        u_rot[i] = p.tangent_u[i] * cs - p.tangent_v[i] * sn;
        v_rot[i] = p.tangent_u[i] * sn + p.tangent_v[i] * cs;
    }
    for (int i = 0; i < 3; ++i) {
        p.tangent_u[i] = u_rot[i];
        p.tangent_v[i] = v_rot[i];
    }

    const float z_bias = 0.004f + 0.003f * HashFloat01(seed + 19U);
    p.active = true;
    p.pos[0] = ground_pos[0] + p.normal[0] * z_bias;
    p.pos[1] = ground_pos[1] + p.normal[1] * z_bias;
    p.pos[2] = ground_pos[2] + p.normal[2] * z_bias;
    // blood_ground_pool_flipbook.png cell 0..15 already animates splash expansion from small
    // center droplet -> full irregular pool, so start quad radius at ~56% to avoid microscopic start.
    p.start_radius = std::max(0.11f, target_radius * 0.56f);
    p.target_radius = std::max(p.start_radius + 0.04f, target_radius);
    p.current_radius = p.start_radius;
    p.spread_duration = std::max(0.25f, spread_duration_sec);
    p.age = 0.0f;
    p.ttl = make_permanent ? 0.0f : std::max(3.0f, lifetime_sec);
    p.wetness = 1.0f;
    p.seed = seed;
}

void BloodVfxSystem::EmitImmediateBurst(const float hit_pos[3], const float impact_dir[3],
                                        float radius, float age, float ttl, float opacity) {
    if (!mist_tex_.IsValid() || ttl <= 1e-4f || age < 0.0f || age > ttl) {
        return;
    }

    const float t = Clamp01(age / ttl);
    float dir[3] = {impact_dir[0], impact_dir[1], impact_dir[2]};
    Normalize3(dir);

    const float drag_dist = radius * (1.0f - std::exp(-5.0f * t));
    const float gravity_drop = 0.5f * 6.8f * age * age;

    // 1. Directional arterial exit jet + breaking liquid web (ONLY cells 0..6 in mist flipbook!).
    // Cell 0..6 have wound origin flush against LEFT UV edge (u0), spraying rightward to u1.
    if (t < 0.58f) {
        const float jet_t = t / 0.58f;
        const uint32_t jet_frame = std::min(6U, static_cast<uint32_t>(jet_t * 6.99f));

        // Ensure valid perpendicular side vector even when shooting nearly straight along camera view
        float jet_dir[3] = {dir[0], dir[1] - 0.18f * jet_t, dir[2]};
        const float fwd_align = std::abs(Dot3(dir, camera_.forward));
        if (fwd_align > 0.86f) {
            const float blend = (fwd_align - 0.86f) / 0.14f;
            jet_dir[0] += camera_.right[0] * (0.45f * blend);
            jet_dir[1] += (camera_.up[1] - 0.25f) * (0.20f * blend);
            jet_dir[2] += camera_.right[2] * (0.45f * blend);
        }
        Normalize3(jet_dir);

        float side[3]{};
        Cross3(jet_dir, camera_.forward, side);
        if (Dot3(side, side) < 1e-5f) {
            side[0] = camera_.up[0];
            side[1] = camera_.up[1];
            side[2] = camera_.up[2];
        }
        Normalize3(side);

        const float half_len = radius * Lerp(0.62f, 1.48f, std::pow(jet_t, 0.42f));
        const float half_thick = half_len * 0.60f;
        const float axis_u[3] = {jet_dir[0] * half_len,
                                 jet_dir[1] * half_len - 0.12f * jet_t * half_len,
                                 jet_dir[2] * half_len};
        const float axis_v[3] = {side[0] * half_thick, side[1] * half_thick, side[2] * half_thick};

        // Anchor left edge (center - axis_u) right inside the wound origin
        const float jet_center[3] = {
            hit_pos[0] + axis_u[0],
            hit_pos[1] + axis_u[1] - gravity_drop * 0.42f,
            hit_pos[2] + axis_u[2],
        };

        const uint32_t jet_col = BloodColorRamp(1.0f - jet_t * 0.20f, camera_.light_intensity,
                                                (1.0f - std::pow(jet_t, 1.45f) * 0.88f) * opacity);
        PushOrientedQuad(mist_alpha_verts_, jet_center, axis_u, axis_v,
                         ComputeCellUv(mist_tex_, jet_frame), jet_col);
    }

    // 2. Centered aerosolized crimson mist & fine micro-spray (ONLY centered mist cells 10..15!)
    constexpr uint32_t kMistPuffs = 2U;
    for (uint32_t i = 0; i < kMistPuffs; ++i) {
        const float puff_scale = radius * (0.44f + 0.62f * std::pow(t, 0.52f)) * (0.88f + 0.14f * float(i));
        const float pos[3] = {
            hit_pos[0] + dir[0] * drag_dist * (0.35f + 0.35f * static_cast<float>(i)),
            hit_pos[1] + dir[1] * drag_dist * 0.35f - gravity_drop * (0.60f + 0.25f * float(i)),
            hit_pos[2] + dir[2] * drag_dist * (0.35f + 0.35f * static_cast<float>(i)),
        };
        const float fade_in = Clamp01(t * 7.5f);
        const float fade_out = std::pow(Clamp01(1.0f - t), 1.35f);
        const float alpha = fade_in * fade_out * 0.86f * opacity;
        if (alpha <= 0.015f) {
            continue;
        }
        const uint32_t mist_step = std::min(4U, static_cast<uint32_t>(t * 4.99f));
        const uint32_t puff_frame = (i == 0U && t < 0.28f) ? 10U : (11U + ((mist_step + i) % 5U));
        const uint32_t col = BloodColorRamp(1.0f - t * 0.25f, camera_.light_intensity, alpha);

        PushBillboardQuad(mist_alpha_verts_, pos, puff_scale, puff_scale,
                          static_cast<float>(i) * 1.15f + t * 0.35f,
                          ComputeCellUv(mist_tex_, puff_frame), col);
    }
}

void BloodVfxSystem::UpdateAndEmit(float dt, float anim_time, float default_ground_y,
                                   const BloodCameraFrameData& camera) {
    camera_ = camera;
    pool_alpha_verts_.clear();
    wall_alpha_verts_.clear();
    mist_alpha_verts_.clear();
    wet_specular_add_verts_.clear();
    untextured_droplets_verts_.clear();

    const float safe_dt = std::clamp(dt, 0.0f, 0.1f);

    // --- 1. Update & Emit Ground Blood Pools (Spreading + Coagulation + Wet Specular) ---
    if (pool_tex_.IsValid()) {
        const uint32_t pool_frames = pool_tex_.cols * pool_tex_.rows;
        const CellUv full_norm_uv{0.02f, 0.02f, 0.98f, 0.98f};

        for (auto& p : ground_pools_) {
            if (!p.active) {
                continue;
            }
            const bool permanent = config_.persistent_decals || (p.ttl <= 0.0f);
            p.age = permanent ? std::min(p.age + safe_dt, 120.0f) : (p.age + safe_dt);
            if (!permanent && p.age >= p.ttl) {
                p.active = false;
                continue;
            }

            // Smooth surface-tension spreading curve from frame 0 (small splash) -> frame 15 (full pool)
            const float spread_t = Clamp01(p.age / p.spread_duration);
            const float eased_spread = 1.0f - std::pow(1.0f - spread_t, 2.4f);
            p.current_radius = Lerp(p.start_radius, p.target_radius, eased_spread);

            // Freshness cools down as blood coagulates and darkens over 25 seconds
            const float freshness = Clamp01(1.0f - p.age / 25.0f);
            p.wetness = Clamp01(1.0f - p.age / 35.0f);

            // Never fade out when persistent_decals is enabled (or p.ttl <= 0)
            const float life_t = (!permanent && p.ttl > 0.0f) ? (p.age / p.ttl) : 0.0f;
            const float fade = permanent ? 1.0f : (life_t > 0.85f ? Clamp01((1.0f - life_t) / 0.15f) : 1.0f);

            const uint32_t frame = std::min(
                pool_frames - 1U,
                static_cast<uint32_t>(eased_spread * static_cast<float>(pool_frames - 1U) + 0.5f));

            const float axis_u[3] = {
                p.tangent_u[0] * p.current_radius,
                p.tangent_u[1] * p.current_radius,
                p.tangent_u[2] * p.current_radius,
            };
            const float axis_v[3] = {
                p.tangent_v[0] * p.current_radius,
                p.tangent_v[1] * p.current_radius,
                p.tangent_v[2] * p.current_radius,
            };

            const uint32_t pool_bgra = BloodColorRamp(freshness, camera_.light_intensity, 0.96f * fade);
            PushOrientedQuad(pool_alpha_verts_, p.pos, axis_u, axis_v,
                             ComputeCellUv(pool_tex_, frame), pool_bgra);

            // Wet glossy specular reflection overlay on fresh pools
            const float spec = ComputeWetSpecularHighlight(p.pos, p.normal, p.wetness * fade, anim_time * 0.08f);
            if (spec > 0.02f && wet_normal_tex_.IsValid()) {
                const uint8_t spec_a = static_cast<uint8_t>(Clamp01(spec * 0.55f) * 255.0f);
                PushOrientedQuad(wet_specular_add_verts_, p.pos, axis_u, axis_v,
                                 full_norm_uv, PackBgra(spec_a, 255, 195, 185));
            }
        }
    }

    // --- 2. Update & Emit Sticky Wall / Surface Splatters (with Gravity Drips) ---
    if (wall_tex_.IsValid()) {
        const CellUv full_norm_uv{0.02f, 0.02f, 0.98f, 0.98f};

        for (auto& w : wall_decals_) {
            if (!w.active) {
                continue;
            }
            const bool permanent = config_.persistent_decals || (w.ttl <= 0.0f);
            w.age = permanent ? std::min(w.age + safe_dt, 120.0f) : (w.age + safe_dt);
            if (!permanent && w.age >= w.ttl) {
                w.active = false;
                continue;
            }

            // Viscous gravity drip: while blood is fresh (first 5 seconds), the bottom edge slowly creeps down
            if (w.age < 5.0f && std::abs(w.normal[1]) < 0.85f) {
                const float viscosity_decay = std::exp(-w.age * 0.65f);
                const float delta_drip = w.drip_speed * viscosity_decay * safe_dt;
                w.drip_stretch += delta_drip;
                // Shift center downward by half the stretch so the top impact core stays anchored in place!
                w.pos[0] -= w.tangent_v[0] * (delta_drip * 0.5f);
                w.pos[1] -= w.tangent_v[1] * (delta_drip * 0.5f);
                w.pos[2] -= w.tangent_v[2] * (delta_drip * 0.5f);
            }

            const float freshness = Clamp01(1.0f - w.age / 20.0f);
            w.wetness = Clamp01(1.0f - w.age / 28.0f);

            const float life_t = (!permanent && w.ttl > 0.0f) ? (w.age / w.ttl) : 0.0f;
            const float fade = permanent ? 1.0f : (life_t > 0.85f ? Clamp01((1.0f - life_t) / 0.15f) : 1.0f);

            const float eff_rv = w.radius_v + w.drip_stretch * 0.5f;
            const float axis_u[3] = {
                w.tangent_u[0] * w.radius_u,
                w.tangent_u[1] * w.radius_u,
                w.tangent_u[2] * w.radius_u,
            };
            const float axis_v[3] = {
                w.tangent_v[0] * eff_rv,
                w.tangent_v[1] * eff_rv,
                w.tangent_v[2] * eff_rv,
            };

            const uint32_t wall_bgra = BloodColorRamp(freshness, camera_.light_intensity, 0.95f * fade);
            PushOrientedQuad(wall_alpha_verts_, w.pos, axis_u, axis_v,
                             ComputeCellUv(wall_tex_, w.atlas_frame), wall_bgra);

            const float spec = ComputeWetSpecularHighlight(w.pos, w.normal, w.wetness * fade, 0.0f);
            if (spec > 0.02f && wet_normal_tex_.IsValid()) {
                const uint8_t spec_a = static_cast<uint8_t>(Clamp01(spec * 0.48f) * 255.0f);
                PushOrientedQuad(wet_specular_add_verts_, w.pos, axis_u, axis_v,
                                 full_norm_uv, PackBgra(spec_a, 255, 190, 180));
            }
        }
    }

    // --- 3. Update & Emit Ballistic Droplets (Mid-Air Flight -> Surface Stick or Ground Pool) ---
    for (auto& d : droplets_) {
        if (!d.active) {
            continue;
        }
        d.age += safe_dt;
        if (d.age >= d.ttl) {
            d.active = false;
            continue;
        }

        const float prev_pos[3] = {d.pos[0], d.pos[1], d.pos[2]};

        // Air drag + gravity acceleration
        const float drag = std::exp(-1.8f * safe_dt);
        d.vel[0] *= drag;
        d.vel[1] = d.vel[1] * drag - 9.81f * safe_dt;
        d.vel[2] *= drag;

        d.pos[0] += d.vel[0] * safe_dt;
        d.pos[1] += d.vel[1] * safe_dt;
        d.pos[2] += d.vel[2] * safe_dt;

        // Check world raycast collision against walls/props if WorldTraceFn is registered
        if (trace_fn_) {
            float hit_p[3]{};
            float hit_n[3]{};
            if (trace_fn_(prev_pos, d.pos, hit_p, hit_n, trace_user_data_)) {
                SpawnSurfaceSplatter(hit_p, hit_n, d.vel,
                                     std::clamp(d.radius * 12.0f, 0.18f, 0.46f), 0.075f,
                                     config_.persistent_decals ? 0.0f : 55.0f);
                d.active = false;
                continue;
            }
        }

        // Check enemy's real ground plane contact (NEVER hoist droplets up to player ground!)
        const float floor_y = (d.ground_y > -5000.0f && std::isfinite(d.ground_y))
                                  ? d.ground_y
                                  : default_ground_y;
        if (d.pos[1] <= floor_y) {
            const float hit_p[3] = {d.pos[0], floor_y, d.pos[2]};
            const float up_n[3] = {0.0f, 1.0f, 0.0f};
            const float decal_ttl = config_.persistent_decals ? 0.0f : 60.0f;
            // Always feed/stamp organic blood_ground_pool_flipbook.png puddles on floors
            SpawnOrGrowGroundPool(hit_p, up_n, std::clamp(d.radius * 11.5f, 0.16f, 0.42f),
                                  3.0f, decal_ttl);
            // Fast horizontal droplets leave a directional forward splatter trail along ground
            const float horiz_spd = std::hypot(d.vel[0], d.vel[2]);
            if (horiz_spd > 2.8f) {
                const float inv_h = 1.0f / horiz_spd;
                const float streak_p[3] = {
                    hit_p[0] + d.vel[0] * inv_h * 0.16f,
                    floor_y,
                    hit_p[2] + d.vel[2] * inv_h * 0.16f,
                };
                SpawnOrGrowGroundPool(streak_p, up_n, std::clamp(d.radius * 6.0f, 0.10f, 0.22f),
                                      2.4f, decal_ttl);
            }
            d.active = false;
            continue;
        }

        // Render airborne physical velocity-stretched 3D liquid droplet/ligament
        if (untextured_droplets_verts_.size() + 6U <= untextured_droplets_verts_.capacity()) {
            const float spd_sq = Dot3(d.vel, d.vel);
            const float spd = spd_sq > 1e-6f ? std::sqrt(spd_sq) : 0.01f;
            float vel_dir[3] = {d.vel[0] / spd, d.vel[1] / spd, d.vel[2] / spd};
            float view_dir[3] = {
                camera_.eye[0] - d.pos[0],
                camera_.eye[1] - d.pos[1],
                camera_.eye[2] - d.pos[2],
            };
            Normalize3(view_dir);

            float side[3]{};
            Cross3(vel_dir, view_dir, side);
            if (Dot3(side, side) < 1e-5f) {
                Cross3(vel_dir, camera_.up, side);
            }
            Normalize3(side);

            const float half_w = std::clamp(d.radius * 0.65f, 0.007f, 0.024f);
            const float len = std::clamp(d.radius * 2.6f + spd * 0.022f, 0.045f, 0.22f);
            const float hw[3] = {side[0] * half_w, side[1] * half_w, side[2] * half_w};
            const float head[3] = {
                d.pos[0] + vel_dir[0] * (len * 0.30f),
                d.pos[1] + vel_dir[1] * (len * 0.30f),
                d.pos[2] + vel_dir[2] * (len * 0.30f),
            };
            const float tail[3] = {
                d.pos[0] - vel_dir[0] * (len * 0.70f),
                d.pos[1] - vel_dir[1] * (len * 0.70f),
                d.pos[2] - vel_dir[2] * (len * 0.70f),
            };
            const float left_p[3] = {d.pos[0] - hw[0], d.pos[1] - hw[1], d.pos[2] - hw[2]};
            const float right_p[3] = {d.pos[0] + hw[0], d.pos[1] + hw[1], d.pos[2] + hw[2]};

            const float life = Clamp01(d.age / d.ttl);
            const float lscale = std::clamp(camera_.light_intensity, 0.75f, 1.25f);
            const uint32_t col_head = PackBgra(
                static_cast<uint8_t>((1.0f - life * 0.35f) * 245.0f),
                static_cast<uint8_t>(std::min(255.0f, 84.0f * lscale)),
                static_cast<uint8_t>(std::min(255.0f, 3.0f * lscale)),
                static_cast<uint8_t>(std::min(255.0f, 5.0f * lscale)));
            const uint32_t col_side = PackBgra(
                static_cast<uint8_t>((1.0f - life * 0.45f) * 215.0f),
                static_cast<uint8_t>(std::min(255.0f, 70.0f * lscale)), 2U, 4U);
            const uint32_t col_tail = PackBgra(
                static_cast<uint8_t>((1.0f - life * 0.55f) * 70.0f), 56U, 2U, 3U);

            const d3d::WorldVertex v_head{head[0], head[1], head[2], col_head};
            const d3d::WorldVertex v_left{left_p[0], left_p[1], left_p[2], col_side};
            const d3d::WorldVertex v_right{right_p[0], right_p[1], right_p[2], col_side};
            const d3d::WorldVertex v_tail{tail[0], tail[1], tail[2], col_tail};

            untextured_droplets_verts_.push_back(v_head);
            untextured_droplets_verts_.push_back(v_right);
            untextured_droplets_verts_.push_back(v_left);
            untextured_droplets_verts_.push_back(v_left);
            untextured_droplets_verts_.push_back(v_right);
            untextured_droplets_verts_.push_back(v_tail);
        }
    }

    // --- 4. Update & Emit Active Airborne Blood Bursts (with aerodynamic drag + gravity) ---
    for (auto& b : bursts_) {
        if (!b.active) {
            continue;
        }
        b.age += safe_dt;
        if (b.age >= b.ttl) {
            b.active = false;
            continue;
        }
        if (safe_dt > 0.0f) {
            const float bdrag = std::exp(-4.5f * safe_dt);
            b.vel[0] *= bdrag;
            b.vel[1] = b.vel[1] * bdrag - 5.2f * safe_dt;
            b.vel[2] *= bdrag;
            b.pos[0] += b.vel[0] * safe_dt;
            b.pos[1] += b.vel[1] * safe_dt;
            b.pos[2] += b.vel[2] * safe_dt;
            if (b.ground_y > -5000.0f && b.pos[1] < b.ground_y + 0.04f) {
                b.pos[1] = b.ground_y + 0.04f;
                b.vel[1] = std::max(0.0f, b.vel[1]);
            }
        }
        EmitImmediateBurst(b.pos, b.dir, b.radius, b.age, b.ttl, 1.0f);
    }
}

void BloodVfxSystem::FlushTexturedBatch(const std::vector<d3d::TexturedWorldVertex>& verts,
                                        BloodTextureSlot slot, bool additive) {
    if (verts.empty() || !camera_.view || !camera_.proj) {
        return;
    }
    if (bind_fn_) {
        bind_fn_(slot, bind_user_data_);
    }
    d3d::DrawTexturedWorldOverlay(verts.data(), static_cast<uint32_t>(verts.size()),
                                  camera_.view, camera_.proj, additive);
}

void BloodVfxSystem::Flush() {
    if (!camera_.view || !camera_.proj) {
        return;
    }

    // 1. Expanding ground blood pools (alpha-blended)
    FlushTexturedBatch(pool_alpha_verts_, BloodTextureSlot::GroundPool, false);

    // 2. Wall & surface blood splatters + gravity drips (alpha-blended)
    FlushTexturedBatch(wall_alpha_verts_, BloodTextureSlot::WallSplatter, false);

    // 3. Airborne blood impact jets, droplets & fine aerosol mist (alpha-blended)
    FlushTexturedBatch(mist_alpha_verts_, BloodTextureSlot::MistFlipbook, false);

    // Note: wet_normal_tex_ is a tangent-space normal map and is sampled on CPU for gloss
    // modulation above; never draw purple normal-map pixels directly as additive quads.
    (void)wet_specular_add_verts_;

    // 5. Optional untextured micro-droplet accents
    if (!untextured_droplets_verts_.empty()) {
        d3d::DrawWorldOverlay(untextured_droplets_verts_.data(),
                              static_cast<uint32_t>(untextured_droplets_verts_.size()),
                              camera_.view, camera_.proj, false);
    }
}

}  // namespace vfx
