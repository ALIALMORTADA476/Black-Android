#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "d3d/world_overlay.h"

namespace vfx {

struct BloodFlipbookTexture {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t cols = 4;
    uint32_t rows = 4;
    float padding_px = 6.0f;
    // Pixel format: SDL_PIXELFORMAT_ARGB8888 (0xAARRGGBB on little-endian ARM64/x86_64)
    std::vector<uint32_t> pixels_argb8888;
    bool released_cpu = false;

    void ReleaseCpuPixels() noexcept {
        if (width > 0 && height > 0 && cols > 0 && rows > 0) {
            std::vector<uint32_t>().swap(pixels_argb8888);
            released_cpu = true;
        }
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return width > 0 && height > 0 && cols > 0 && rows > 0 &&
               (released_cpu ||
                pixels_argb8888.size() == static_cast<size_t>(width) * static_cast<size_t>(height));
    }

    // Samples normal & wet gloss from blood_wet_normal.png in [-1.0, +1.0] for (nx, ny) and [0..1] for gloss
    void SampleWetNormal(float u, float v, float& out_nx, float& out_ny, float& out_gloss) const noexcept;
};

struct BloodCameraFrameData {
    float eye[3]{};
    float right[3]{};
    float up[3]{};
    float forward[3]{};
    float light_dir[3]{-0.35f, -0.85f, -0.38f};  // Main scene or muzzle/explosion light direction
    float light_intensity = 1.0f;
    const float* view = nullptr;  // float[16]
    const float* proj = nullptr;  // float[16]
};

// Optional callback so airborne blood droplets can query surrounding walls/props/floors.
// Return true if segment [start -> end] hits a surface, populating hit_pos and hit_normal.
using WorldTraceFn = bool (*)(const float start[3], const float end[3],
                              float out_hit_pos[3], float out_hit_normal[3], void* user_data);

struct BloodVfxConfig {
    std::string mist_flipbook_path = "vfx/blood_mist_splatter_flipbook.png";
    std::string wall_atlas_path = "vfx/blood_wall_splatter_atlas.png";
    std::string ground_pool_path = "vfx/blood_ground_pool_flipbook.png";
    std::string wet_normal_path = "vfx/blood_wet_normal.png";

    uint32_t mist_cols = 4;
    uint32_t mist_rows = 4;
    uint32_t wall_cols = 4;
    uint32_t wall_rows = 4;
    uint32_t pool_cols = 4;
    uint32_t pool_rows = 4;

    float cell_uv_padding_px = 6.0f;
    bool persistent_decals = true;

    uint32_t max_active_bursts = 64;
    uint32_t max_ballistic_droplets = 256;
    uint32_t max_wall_splatters = 512;
    uint32_t max_ground_pools = 512;

    uint32_t max_textured_quads_per_batch = 3072;
    uint32_t max_untextured_verts_per_batch = 2048;
};

// Texture slot passed to BloodBindTextureFn right before d3d::DrawTexturedWorldOverlay
enum class BloodTextureSlot : uint32_t {
    MistFlipbook = 0,     // vfx/blood_mist_splatter_flipbook.png
    WallSplatter = 1,     // vfx/blood_wall_splatter_atlas.png
    GroundPool = 2,       // vfx/blood_ground_pool_flipbook.png
    WetSpecularNormal = 3 // vfx/blood_wet_normal.png
};

using BloodBindTextureFn = void (*)(BloodTextureSlot slot, void* user_data);

class BloodVfxSystem {
public:
    BloodVfxSystem() = default;
    ~BloodVfxSystem() = default;

    BloodVfxSystem(const BloodVfxSystem&) = delete;
    BloodVfxSystem& operator=(const BloodVfxSystem&) = delete;

    // Loads all 4 PNG blood flipbooks/atlases via SDL_LoadPNG and converts them to ARGB8888 (0xAARRGGBB).
    bool Initialize(const BloodVfxConfig& config = BloodVfxConfig{});
    void Shutdown();
    void ClearAllDecals() noexcept;

    void SetTextureBinder(BloodBindTextureFn fn, void* user_data) noexcept {
        bind_fn_ = fn;
        bind_user_data_ = user_data;
    }

    void SetWorldTracer(WorldTraceFn fn, void* user_data) noexcept {
        trace_fn_ = fn;
        trace_user_data_ = user_data;
    }

    void SetPersistentDecals(bool persistent) noexcept {
        config_.persistent_decals = persistent;
    }

    [[nodiscard]] bool GetPersistentDecals() const noexcept {
        return config_.persistent_decals;
    }

    [[nodiscard]] const BloodFlipbookTexture& GetMistTexture() const noexcept { return mist_tex_; }
    [[nodiscard]] const BloodFlipbookTexture& GetWallAtlasTexture() const noexcept { return wall_tex_; }
    [[nodiscard]] const BloodFlipbookTexture& GetGroundPoolTexture() const noexcept { return pool_tex_; }
    [[nodiscard]] const BloodFlipbookTexture& GetWetNormalTexture() const noexcept { return wet_normal_tex_; }

    // Frees duplicate CPU copies of large 2048x2048 blood flipbooks after GPU upload while retaining wet normal.
    void ReleaseUploadedCpuBuffers() noexcept {
        mist_tex_.ReleaseCpuPixels();
        wall_tex_.ReleaseCpuPixels();
        pool_tex_.ReleaseCpuPixels();
    }

    // 1) High-velocity bullet/shrapnel impact burst:
    //    Spawns directional arterial blood jet, aerosolized blood mist cloud, and ballistic droplets
    //    that fly outward with gravity/drag and automatically stick to walls or form pools on ground_y.
    void SpawnImpactBurst(const float hit_pos[3], const float impact_dir[3],
                          float ground_y, float caliber_intensity = 1.0f,
                          uint32_t droplet_count = 14U);

    // 1b) Physical dripping/oozing arterial droplet from a bleeding wound or corpse.
    //     Falls strictly under gravity onto ground_y and feeds ground pools without airborne mist cards.
    void SpawnWoundDripDroplet(const float wound_pos[3], const float spray_dir[3],
                               float ground_y, float speed = 0.85f, float radius = 0.016f);

    // 2) Directly stick a directional splatter decal onto any wall, prop, or sloped surface.
    //    Automatically aligns drips with world gravity and slides downward while fresh.
    void SpawnSurfaceSplatter(const float surface_pos[3], const float surface_normal[3],
                              const float spray_dir[3], float radius = 0.42f,
                              float drip_speed = 0.08f, float lifetime_sec = 0.0f);

    // 3) Directly spawn or feed an expanding, coagulating blood pool on the ground.
    //    If a pool already exists within merge_radius, it organically grows & refreshes that pool.
    void SpawnOrGrowGroundPool(const float ground_pos[3], const float ground_normal[3],
                               float target_radius = 0.65f, float spread_duration_sec = 4.5f,
                               float lifetime_sec = 0.0f);

    // 4) Immediate-mode helper (stateless): draws an airborne burst at exact normalized age [0..ttl]
    void EmitImmediateBurst(const float hit_pos[3], const float impact_dir[3],
                            float radius, float age, float ttl, float opacity = 1.0f);

    // Advances active blood bursts, ballistic droplets, wall drips, coagulation darkening, and pool spreading.
    void UpdateAndEmit(float dt, float anim_time, float default_ground_y,
                       const BloodCameraFrameData& camera);

    // Flushes all batched blood decals, pools, mist, and wet specular highlights. Zero heap allocations.
    void Flush();

private:
    struct CellUv {
        float u0, v0, u1, v1;
    };

    struct ActiveBurst {
        bool active = false;
        float pos[3]{};
        float vel[3]{};
        float dir[3]{};
        float ground_y = -10000.0f;
        float radius = 0.28f;
        float age = 0.0f;
        float ttl = 0.34f;
        float intensity = 1.0f;
        uint32_t seed = 0;
    };

    struct BallisticDroplet {
        bool active = false;
        float pos[3]{};
        float vel[3]{};
        float ground_y = 0.0f;
        float radius = 0.08f;
        float age = 0.0f;
        float ttl = 1.8f;
        uint32_t seed = 0;
    };

    struct WallSplatterDecal {
        bool active = false;
        float pos[3]{};
        float normal[3]{};
        float tangent_u[3]{};
        float tangent_v[3]{};  // Aligned with downward gravity on walls so drips run down
        float radius_u = 0.35f;
        float radius_v = 0.35f;
        float drip_stretch = 0.0f;
        float drip_speed = 0.06f;
        float age = 0.0f;
        float ttl = 45.0f;
        float wetness = 1.0f;
        uint32_t atlas_frame = 0;
        uint32_t seed = 0;
    };

    struct GroundPoolDecal {
        bool active = false;
        float pos[3]{};
        float normal[3]{0.0f, 1.0f, 0.0f};
        float tangent_u[3]{1.0f, 0.0f, 0.0f};
        float tangent_v[3]{0.0f, 0.0f, 1.0f};
        float start_radius = 0.15f;
        float target_radius = 0.70f;
        float current_radius = 0.15f;
        float spread_duration = 4.5f;
        float age = 0.0f;
        float ttl = 60.0f;
        float wetness = 1.0f;
        uint32_t seed = 0;
    };

    static bool LoadFlipbookPng(const std::string& path, uint32_t cols, uint32_t rows,
                                float padding_px, BloodFlipbookTexture& out_tex,
                                bool decontaminate_edges = true);

    static CellUv ComputeCellUv(const BloodFlipbookTexture& tex, uint32_t frame_index) noexcept;

    static uint32_t PackBgra(uint8_t a, uint8_t r, uint8_t g, uint8_t b) noexcept {
        return (static_cast<uint32_t>(a) << 24) |
               (static_cast<uint32_t>(r) << 16) |
               (static_cast<uint32_t>(g) << 8) |
               static_cast<uint32_t>(b);
    }

    // Physical arterial-to-coagulated venous blood color ramp
    static uint32_t BloodColorRamp(float freshness_01, float lighting_scale, float alpha_01) noexcept;

    float ComputeWetSpecularHighlight(const float world_pos[3], const float normal[3],
                                      float wetness, float uv_phase) const noexcept;

    void PushBillboardQuad(std::vector<d3d::TexturedWorldVertex>& dst,
                           const float center[3], float half_w, float half_h,
                           float roll_rad, const CellUv& uv, uint32_t bgra);

    void PushOrientedQuad(std::vector<d3d::TexturedWorldVertex>& dst,
                          const float center[3], const float axis_u[3], const float axis_v[3],
                          const CellUv& uv, uint32_t bgra);

    void FlushTexturedBatch(const std::vector<d3d::TexturedWorldVertex>& verts,
                            BloodTextureSlot slot, bool additive);

    BloodVfxConfig config_{};
    BloodCameraFrameData camera_{};

    BloodFlipbookTexture mist_tex_{};
    BloodFlipbookTexture wall_tex_{};
    BloodFlipbookTexture pool_tex_{};
    BloodFlipbookTexture wet_normal_tex_{};

    BloodBindTextureFn bind_fn_ = nullptr;
    void* bind_user_data_ = nullptr;

    WorldTraceFn trace_fn_ = nullptr;
    void* trace_user_data_ = nullptr;

    // Pre-allocated fixed-capacity pools (ring-buffer replacement when full, zero heap allocation).
    std::vector<ActiveBurst> bursts_;
    std::vector<BallisticDroplet> droplets_;
    std::vector<WallSplatterDecal> wall_decals_;
    std::vector<GroundPoolDecal> ground_pools_;

    uint32_t next_burst_idx_ = 0;
    uint32_t next_droplet_idx_ = 0;
    uint32_t next_wall_idx_ = 0;
    uint32_t next_pool_idx_ = 0;
    uint32_t spawn_counter_ = 1;

    // Pre-allocated vertex buffers for batched rendering.
    std::vector<d3d::TexturedWorldVertex> pool_alpha_verts_;
    std::vector<d3d::TexturedWorldVertex> wall_alpha_verts_;
    std::vector<d3d::TexturedWorldVertex> mist_alpha_verts_;
    std::vector<d3d::TexturedWorldVertex> wet_specular_add_verts_;
    std::vector<d3d::WorldVertex> untextured_droplets_verts_;
};

}  // namespace vfx
