#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "d3d/world_overlay.h"

namespace vfx {

struct FlipbookTexture {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t cols = 8;
  uint32_t rows = 8;
  float padding_px = 6.0f;
  // Pixel format: SDL_PIXELFORMAT_ARGB8888 (0xAARRGGBB on little-endian
  // ARM64/x86_64)
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

  // Bilinear sample of the normal/distortion map in [-1.0, +1.0] range for (nx,
  // ny, mask_alpha)
  void SampleNormalDistortion(float u, float v, float &out_nx, float &out_ny,
                              float &out_alpha) const noexcept;
};

struct CameraFrameData {
  float eye[3]{};
  float right[3]{};
  float up[3]{};
  float forward[3]{};
  const float *view = nullptr; // float[16]
  const float *proj = nullptr; // float[16]
};

struct FlameStreamPoint {
  float pos[3]{};
  float vel[3]{};
  float radius = 0.25f;
  float heat = 1.0f; // [0.0 = cool smoke/embers, 1.0 = white-hot fuel core]
};

// Post-process / screen-space refraction descriptor automatically computed for
// active explosions. Bind directly to your Vulkan / OpenGL ES / D3D
// post-process pass or use the built-in 3D overlay waves.
struct ShockwaveScreenDistortion {
  bool active = false;
  float screen_uv[2]{
      0.5f, 0.5f}; // Explosion center projected to normalized screen UV [0..1]
  float ring_radius_uv =
      0.0f; // Current expanding shockwave radius in screen UV space
  float ring_thickness_uv = 0.08f; // Width of the refractive compression wave
  float refraction_strength =
      0.0f; // UV offset strength (bend power on wires/walls)
  float chromatic_aberration =
      0.0f; // RGB channel split intensity on shockwave rim
  float radial_blur_strength =
      0.0f; // Radial zoom blur intensity from blast wave suppression
  float heat_haze_strength =
      0.0f; // Rising thermal shimmer above the fireball/smoke column
  float camera_shake_pitch = 0.0f; // Suggested camera pitch impulse (radians)
  float camera_shake_yaw = 0.0f;   // Suggested camera yaw impulse (radians)
  float camera_shake_roll = 0.0f;  // Suggested camera roll impulse (radians)
};

struct FireVfxConfig {
  std::string fire_loop_path = "vfx/fire_loop_flipbook.png";
  std::string explosion_fire_path = "vfx/explosion_fire_flipbook.png";
  std::string explosion_smoke_path = "vfx/explosion_smoke_flipbook.png";
  std::string embers_sparks_path = "vfx/embers_sparks_flipbook.png";
  std::string shockwave_normal_path = "vfx/shockwave_normal.png";

  uint32_t fire_loop_cols = 8;
  uint32_t fire_loop_rows = 8;
  uint32_t explosion_fire_cols = 8;
  uint32_t explosion_fire_rows = 8;
  uint32_t explosion_smoke_cols = 8;
  uint32_t explosion_smoke_rows = 8;
  uint32_t embers_sparks_cols = 4;
  uint32_t embers_sparks_rows = 4;

  float cell_uv_padding_px = 6.0f;
  uint32_t max_textured_quads_per_batch = 2560;
  uint32_t max_untextured_verts_per_batch = 4096;
};

// Texture slot passed to BindTextureFn right before
// d3d::DrawTexturedWorldOverlay
enum class FlipbookSlot : uint32_t {
  FireLoop = 0,
  ExplosionFire = 1,
  ExplosionSmoke = 2,
  EmbersSparks = 3,
  ShockwaveDistortion = 4,
};

using BindTextureFn = void (*)(FlipbookSlot slot, void *user_data);

class FireVfxSystem {
public:
  FireVfxSystem() = default;
  ~FireVfxSystem() = default;

  FireVfxSystem(const FireVfxSystem &) = delete;
  FireVfxSystem &operator=(const FireVfxSystem &) = delete;

  // Loads all 5 PNG flipbooks/maps via SDL_LoadPNG and converts them to
  // ARGB8888 (0xAARRGGBB).
  bool Initialize(const FireVfxConfig &config = FireVfxConfig{});
  void Shutdown();

  void SetTextureBinder(BindTextureFn fn, void *user_data) noexcept {
    bind_fn_ = fn;
    bind_user_data_ = user_data;
  }

  [[nodiscard]] const FlipbookTexture &GetFireLoopTexture() const noexcept {
    return fire_loop_tex_;
  }
  [[nodiscard]] const FlipbookTexture &
  GetExplosionFireTexture() const noexcept {
    return explosion_fire_tex_;
  }
  [[nodiscard]] const FlipbookTexture &
  GetExplosionSmokeTexture() const noexcept {
    return explosion_smoke_tex_;
  }
  [[nodiscard]] const FlipbookTexture &GetEmbersSparksTexture() const noexcept {
    return embers_sparks_tex_;
  }
  [[nodiscard]] const FlipbookTexture &
  GetShockwaveNormalTexture() const noexcept {
    return shockwave_normal_tex_;
  }

    // Frees duplicate CPU copies of large 2048x2048 flipbooks after GPU upload while retaining dimensions.
    void ReleaseUploadedCpuBuffers() noexcept {
        fire_loop_tex_.ReleaseCpuPixels();
        explosion_fire_tex_.ReleaseCpuPixels();
        explosion_smoke_tex_.ReleaseCpuPixels();
        embers_sparks_tex_.ReleaseCpuPixels();
    }

  // Returns the strongest active screen-space shockwave & thermal distortion
  // parameters for this frame.
  [[nodiscard]] const ShockwaveScreenDistortion &
  GetScreenDistortion() const noexcept {
    return screen_distortion_;
  }

  // Call once at the start of VFX rendering for the current frame.
  void BeginFrame(const CameraFrameData &camera) noexcept;

  // 1) Continuous / burning fire (ground fire or burning props/targets) +
  // rising heat shimmer.
  void EmitBurningFire(const float world_pos[3], float ground_y,
                       float intensity, float dt, float anim_time);

  // 2) Full Squad-style explosion:
  //    - Refractive 3D Shockwave Dome & Mach Condensation Halo (warped by
  //    shockwave_normal.png)
  //    - Chromatic RGB Split Pressure Wave + Radial Blur + Camera Shake
  //    - Rising Heat Haze Shimmer Column
  //    - Core Flash & Expanding Fireball (explosion_fire_flipbook.png)
  //    - High-Drag Floating Embers & Ballistic Sparks
  //    (embers_sparks_flipbook.png)
  //    - Dark Billowing Smoke Plume & Ground Dust Wave
  //    (explosion_smoke_flipbook.png)
  void EmitExplosion(const float center_pos[3], float radius, float force,
                     float age, float ttl, float glow_mul);

  // 3) Flamethrower stream + ground-hugging burning fuel pools + heat
  // distortion along stream.
  void EmitFlameStream(const FlameStreamPoint *points, uint32_t point_count,
                       float ground_y, float stream_fade, float anim_time);

  // Flushes all batched geometry in optimal depth/blend order with zero heap
  // allocations.
  void Flush();

private:
  struct CellUv {
    float u0, v0, u1, v1;
  };

  static bool LoadFlipbookPng(const std::string &path, uint32_t cols,
                              uint32_t rows, float padding_px,
                              FlipbookTexture &out_tex);

  static CellUv ComputeCellUv(const FlipbookTexture &tex,
                              uint32_t frame_index) noexcept;

  static uint32_t PackBgra(uint8_t a, uint8_t r, uint8_t g,
                           uint8_t b) noexcept {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
  }

  static uint32_t BlackbodyColor(float heat_01, float alpha_01) noexcept;

  bool ProjectWorldToScreenUv(const float world_pos[3], float out_uv[2],
                              float &out_view_z) const noexcept;

  void PushBillboardQuad(std::vector<d3d::TexturedWorldVertex> &dst,
                         const float center[3], float half_w, float half_h,
                         float roll_rad, const CellUv &uv, uint32_t bgra);

  void PushOrientedQuad(std::vector<d3d::TexturedWorldVertex> &dst,
                        const float center[3], const float axis_u[3],
                        const float axis_v[3], const CellUv &uv, uint32_t bgra);

  void EmitRefractiveShockwaveShell(const float center_pos[3],
                                    float shell_radius, float normalized_age,
                                    float glow_mul, uint32_t seed);

  void EmitHeatHazeShimmer(const float center_pos[3], float radius, float age,
                           float strength, uint32_t seed);

  void PushUntexturedRing(const float center[3], float inner_r, float outer_r,
                          uint32_t segments, uint32_t inner_bgra,
                          uint32_t outer_bgra);

  void FlushTexturedBatch(const std::vector<d3d::TexturedWorldVertex> &verts,
                          FlipbookSlot slot, bool additive,
                          int color_gain_x = 1);

  FireVfxConfig config_{};
  CameraFrameData camera_{};
  ShockwaveScreenDistortion screen_distortion_{};

  FlipbookTexture fire_loop_tex_{};
  FlipbookTexture explosion_fire_tex_{};
  FlipbookTexture explosion_smoke_tex_{};
  FlipbookTexture embers_sparks_tex_{};
  FlipbookTexture shockwave_normal_tex_{};

  BindTextureFn bind_fn_ = nullptr;
  void *bind_user_data_ = nullptr;

  // Pre-allocated vertex buffers (zero new/malloc during gameplay).
  std::vector<d3d::TexturedWorldVertex> smoke_alpha_verts_;
  std::vector<d3d::TexturedWorldVertex> shockwave_alpha_verts_;
  std::vector<d3d::TexturedWorldVertex> shockwave_add_verts_;
  std::vector<d3d::TexturedWorldVertex> fire_loop_add_verts_;
  std::vector<d3d::TexturedWorldVertex> explosion_add_verts_;
  std::vector<d3d::TexturedWorldVertex> embers_add_verts_;
  std::vector<d3d::WorldVertex> untextured_add_verts_;
};

} // namespace vfx
