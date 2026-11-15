#include "game/ballistics_fx.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "app/app.h"
#include "common/mod_schema.h"
#include "common/options.h"
#include "d3d/world_overlay.h"
#include "game/flame_plume.h"
#include "game/layout.h"
#include "game/overdrive_rules.h"
#include "game/time_stop.h"
#include "sound/weapon_audio.h"
#include "xdk/hle.h"
#include "xdk/layout.h"
#ifdef __ANDROID__
#include "host.h"
#endif

namespace game {
namespace ballistics {

namespace {

using namespace kernel;
using namespace layout;

inline bool MemMapped(uint32_t va, uint32_t size = 16u) {
  return va >= 0x10000u && size > 0 &&
         (uint64_t(va) + size) <= 0x100000000ULL && mem::IsCommitted(va) &&
         mem::IsCommitted(va + size - 1u);
}

constexpr uint32_t kWeaponManager = 0x2D12D0;
constexpr uint32_t kBulletPoolOffset = 0x6E0;
constexpr uint32_t kBulletSlotSize = 0x90;
constexpr int kBulletPoolSlots = 16;

constexpr uint32_t kExplosiveManager = 0x2D1998;
constexpr uint32_t kExplosiveSlotSize = 0x120;
constexpr uint32_t kPoolABitmap = 0x3700;
constexpr uint32_t kPoolBBitmap = 0x3720;
constexpr int kPoolACount = 32;
constexpr int kPoolBCount = 16;

constexpr size_t kMaxTracers = 96;
constexpr size_t kMaxSmokeNodes = 192;
constexpr size_t kMaxBloodNodes = 256;
constexpr size_t kMaxWoundStreams = 24;
constexpr size_t kMaxBloodPools = 128;
constexpr size_t kMaxMuzzleFlashes = 16;
constexpr size_t kMaxImpactNodes = 96;
constexpr size_t kMaxExplosionFlames = 128;
constexpr size_t kMaxExplosionEmbers = 256;
constexpr size_t kMaxRecentExplosions = 16;
constexpr size_t kMuzzleCacheSize = 24;
constexpr size_t kMaxShellCasings = 64;
constexpr size_t kMaxHazeMotes = 72;

// Table layout matching weapons.cpp
constexpr uint32_t kTableCount = 0x1E8;
constexpr uint32_t kTableEntries = 0x1EC;
constexpr uint32_t kEntrySize = 0x28;

// Base-40 decode table from BLACK weapon IDs
std::string DecodeId(uint64_t code) {
  char out[12];
  int n = 0;
  for (uint64_t v = code; v && n < 11; v /= 40) {
    int d = int(v % 40);
    out[n++] = d == 0    ? ' '
               : d <= 10 ? char('0' + d - 1)
               : d <= 36 ? char('A' + d - 11)
                         : '_';
  }
  while (n > 0 && out[n - 1] == ' ')
    n--;
  return std::string(out, out + n);
}

WeaponClass LookupFirePktWeapon(uint32_t fire_pkt, bool &out_suppressed,
                                bool &out_is_player, bool &out_is_magnum,
                                uint32_t &out_weapon_ptr) {
  out_suppressed = false;
  out_is_player = false;
  out_is_magnum = false;
  out_weapon_ptr = 0;
  if (!fire_pkt || !MemMapped(fire_pkt))
    return WeaponClass::Unknown;

  uint32_t wpn = R32(fire_pkt + 0x04);
  if (!wpn || !MemMapped(wpn))
    return WeaponClass::Unknown;
  out_weapon_ptr = wpn;

  out_suppressed = (G<uint8_t>(wpn + 0x108) != 0);
  uint32_t owner = R32(wpn + 0xF0);
  if (owner && MemMapped(owner)) {
    uint32_t world = R32(kWorld);
    // kWorldPlayer is embedded inline inside *kWorld (not a pointer)
    uint32_t player = (world && MemMapped(world)) ? (world + kWorldPlayer) : 0;
    out_is_player =
        (player != 0 && owner == player) || (G<uint8_t>(owner + 0xC4) == 2);
  }

  int type = int(G<int32_t>(wpn + 0x00));
  uint32_t mgr = R32(kWeaponManager);
  uint32_t table = (mgr && MemMapped(mgr)) ? R32(mgr) : 0;
  if (!table || !MemMapped(table) || type < 0)
    return WeaponClass::Unknown;

  int count = G<uint8_t>(table + kTableCount);
  uint32_t entries = R32(table + kTableEntries);
  if (!entries || !MemMapped(entries) || type >= count || count > 64)
    return WeaponClass::Unknown;

  uint32_t e = entries + uint32_t(type) * kEntrySize;
  std::string key = DecodeId(uint64_t(R32(e)) | (uint64_t(R32(e + 4)) << 32));
  if (key.size() > 4 && key.compare(0, 4, "BG1_") == 0)
    key = key.substr(4);
  if (key == "MGN" || key == "MG5")
    out_is_magnum = true;
  if (key.size() >= 3 && key[2] == '5')
    out_suppressed = true;
  return ClassifyWeaponCode(key.c_str());
}

struct MuzzleEntry {
  uint32_t owner = 0;
  float pos[3] = {};
  uint64_t seq = 0;
};

struct TracerInstance {
  bool active = false;
  bool is_physical = true;
  bool is_player = false;
  bool hit_locked = false;
  bool suppressed = false;
  WeaponClass cls = WeaponClass::Rifle;

  uint32_t guest_slot = 0;
  float visual_origin[3] = {};
  float physics_origin[3] = {};
  float head_pos[3] = {};
  float target_end[3] = {}; // Used by synchronized hitscan tracers
  float dir[3] = {};
  float speed = 180.0f;

  float age = 0.0f;      // Elapsed tracer time
  float hit_age = -1.0f; // Time since impact/completion
  float afterglow_ttl = 0.13f;
  float trail_scale = 1.0f;
  float core_thickness = 2.0f;
  float brightness = 1.0f;
  uint32_t color_override = 0;
  uint32_t group_id = 0;
  float last_smoke_dist = 0.0f;
};

struct SmokeNode {
  bool active = false;
  bool from_launcher = false;
  bool is_muzzle_wisp = false;
  float pos[3] = {};
  float prev_pos[3] = {};
  bool has_prev = false;
  float vel[3] = {};
  float age = 0.0f;
  float ttl = 1.0f;
  float base_radius = 0.025f;
  float opacity = 0.08f;
  float tint = 0.78f;
  float seed = 0.0f;
  uint32_t group_id = 0;
};

struct PlayerMuzzleEmitter {
  float heat = 0.0f;
  float emit_accum = 0.0f;
  bool suppressed = false;
  bool has_prev_wisp = false;
  float prev_wisp_pos[3] = {};
  uint32_t wisp_seq = 0;
};

struct ExplosiveTracker {
  bool active = false;
  float last_pos[3] = {};
  float smoke_accum = 0.0f;
};

struct BloodNode {
  bool active = false;
  bool is_mist = false; // true: soft arterial mist; false: gravity-driven
                        // liquid stream/droplet
  float pos[3] = {};
  float prev_pos[3] = {};
  float vel[3] = {};
  float ground_y = -1000.0f;
  float age = 0.0f;
  float ttl = 0.75f;
  float radius = 0.03f;
  float opacity = 0.94f;
  float r = 0.048f;
  float g = 0.0005f;
  float b = 0.0012f;
};

struct WoundBleedStream {
  bool active = false;
  uint32_t victim_ea = 0;
  float y_offset = 1.15f;
  float dir[3] = {0.0f, 0.0f, 1.0f};
  float ground_y = 0.0f;
  float remaining = 0.0f;
  float emit_accum = 0.0f;
  uint32_t mult = 2;
  bool is_headshot = false;
};

struct BloodPoolNode {
  bool active = false;
  float pos[3] = {};
  float age = 0.0f;
  float ttl = 22.0f;
  float radius = 0.16f;
  float target_radius = 0.42f;
  float seed = 0.0f;
  float opacity = 0.86f;
};

struct MuzzleFlashNode {
  bool active = false;
  float pos[3] = {};
  float dir[3] = {};
  float age = 0.0f;
  float ttl = 0.048f;
  float radius = 0.14f;
  float rot = 0.0f;
  int tile = 4;
};

struct ImpactNode {
  bool active = false;
  bool is_spark = true; // true: additive amber spark; false: concrete/dust puff
  float pos[3] = {};
  float prev_pos[3] = {};
  float vel[3] = {};
  float age = 0.0f;
  float ttl = 0.25f;
  float radius = 0.02f;
  float opacity = 0.85f;
};

struct ExplosionFlameNode {
  bool active = false;
  bool is_shockwave =
      false; // ground-hugging radial ring, drawn as a floor tile
  bool is_air_ring =
      false;             // vertical spherical blast shell, drawn as a billboard
  bool is_flash = false; // instant white-hot ignition flash
  bool is_burn_fire = false; // enemy burn/fire cloud (natural golden orange flame)
  float pos[3] = {};
  float vel[3] = {};
  float age = 0.0f;
  float ttl = 0.45f;
  float start_rad = 0.45f;
  float end_rad = 2.40f;
  float rot = 0.0f;
  float rot_vel = 0.0f;
  int tile = 4;
  float heat = 1.0f;
};

struct ExplosionEmberNode {
  bool active = false;
  float pos[3] = {};
  float prev_pos[3] = {};
  float vel[3] = {};
  float ground_y = -1000.0f;
  float age = 0.0f;
  float ttl = 1.10f;
  float radius = 0.024f;
  float temp = 1.0f;
};

struct RecentExplosion {
  bool valid = false;
  float pos[3] = {};
  std::chrono::steady_clock::time_point tp{};
};

// Screen-space refraction lens that bends the frame around a fresh blast. Kept
// separate from the flame geometry so the copy of the colour target happens
// before any of the explosion's own fire is drawn over it.
struct BlastLensNode {
  bool active = false;
  float pos[3] = {};
  float radius = 3.0f;
  float age = 0.0f;
  float ttl = 0.60f;
  float strength = 1.0f;
};

struct ShellCasingInstance {
  ShellCasingBody body{};
  WeaponClass cls = WeaponClass::Rifle;
  bool red_hull = false;
  float length_m = 0.045f;
  float radius_m = 0.0075f;
  float axis_a[3] = {1.0f, 0.0f, 0.0f};
  float axis_b[3] = {0.0f, 1.0f, 0.0f};
};

struct HazeMoteInstance {
  bool active = false;
  float pos[3] = {};
  float vel[3] = {};
  float age = 0.0f;
  float ttl = 2.6f;
  float radius = 0.34f;
  float opacity = 0.065f;
  float rot = 0.0f;
  float rot_vel = 0.2f;
  int tile = 12;
  bool warm_dust = true;
};

struct CameraState {
  bool valid = false;
  float view[16] = {};
  float proj[16] = {};
  float eye[3] = {};
  float right[3] = {1.0f, 0.0f, 0.0f};
  float up[3] = {0.0f, 1.0f, 0.0f};
  float forward[3] = {0.0f, 0.0f, 1.0f};
  float tan_half_fov_y = 0.70f;
};

std::mutex g_mutex;
std::array<TracerInstance, kMaxTracers> g_tracers{};
size_t g_tracer_cursor = 0;

std::array<SmokeNode, kMaxSmokeNodes> g_smoke{};
size_t g_smoke_cursor = 0;
PlayerMuzzleEmitter g_muzzle_emitter{};

std::array<BloodNode, kMaxBloodNodes> g_blood{};
size_t g_blood_cursor = 0;
std::array<WoundBleedStream, kMaxWoundStreams> g_wound_streams{};
size_t g_wound_cursor = 0;
std::array<BloodPoolNode, kMaxBloodPools> g_blood_pools{};
size_t g_pool_cursor = 0;
std::array<MuzzleFlashNode, kMaxMuzzleFlashes> g_muzzle_flashes{};
size_t g_flash_cursor = 0;
std::array<ImpactNode, kMaxImpactNodes> g_impacts{};
size_t g_impact_cursor = 0;
std::array<ExplosionFlameNode, kMaxExplosionFlames> g_explosion_flames{};
size_t g_exp_flame_cursor = 0;
std::array<ExplosionEmberNode, kMaxExplosionEmbers> g_explosion_embers{};
size_t g_exp_ember_cursor = 0;
std::array<RecentExplosion, kMaxRecentExplosions> g_recent_explosions{};
size_t g_recent_exp_cursor = 0;

// Explosion heat-haze lenses (screen-space refraction, see RenderWorldScene 2a)
constexpr size_t kMaxBlastLenses = 4;
std::array<BlastLensNode, kMaxBlastLenses> g_blast_lenses{};
size_t g_blast_lens_cursor = 0;

std::array<ShellCasingInstance, kMaxShellCasings> g_shells{};
size_t g_shell_cursor = 0;
std::array<HazeMoteInstance, kMaxHazeMotes> g_haze_motes{};
size_t g_haze_cursor = 0;

uint32_t g_fx_rng = 0x5A17B10Du;

// Flamethrower stream snapshot: written every frame from the weapon sim and
// drawn verbatim, so the visible jet and the damaging jet cannot drift apart.
constexpr size_t kMaxFlameStreamPoints = 128;
std::array<FlameStreamPoint, kMaxFlameStreamPoints> g_flame_jet{};
uint32_t g_flame_jet_count = 0;
std::array<FlameStreamPoint, 64> g_flame_pools{};
uint32_t g_flame_pool_count = 0;
float g_flame_stream_ground_y = 0.0f;
std::chrono::steady_clock::time_point g_flame_stream_tp{};
bool g_flame_stream_fresh = false;
float g_flame_ember_accum = 0.0f;
float g_flame_soot_accum = 0.0f;

struct ThermalSlotEntry {
  bool active = false;
  uint32_t slot_ea = 0;
  float dir[3] = {0.0f, 0.0f, 1.0f};
  std::chrono::steady_clock::time_point tp{};
};
constexpr size_t kMaxThermalSlots = 24;
std::array<ThermalSlotEntry, kMaxThermalSlots> g_thermal_slots{};
size_t g_thermal_cursor = 0;
thread_local bool g_in_thermal_impact = false;

std::array<MuzzleEntry, kMuzzleCacheSize> g_muzzles{};
uint64_t g_muzzle_seq = 0;

std::array<ExplosiveTracker, kPoolACount + kPoolBCount> g_explosive_trackers{};

CameraState g_cam{};
std::chrono::steady_clock::time_point g_last_frame_tp{};
bool g_has_frame_tp = false;

uint64_t g_player_shot_seq = 0;
uint64_t g_ai_shot_seq = 0;
uint32_t g_next_group_id = 1;
uint32_t g_last_shot_wpn = 0;
float g_last_shot_time_sec = -10.0f;

std::atomic<uint64_t> g_shots_observed{0};
std::atomic<uint64_t> g_tracers_emitted{0};
std::atomic<uint64_t> g_hitscan_synced{0};
std::atomic<uint64_t> g_smoke_emitted{0};
std::atomic<uint64_t> g_culled_count{0};
std::atomic<uint64_t> g_shells_ejected{0};

std::chrono::steady_clock::time_point g_last_telemetry_log{};

inline float VecLen(const float v[3]) {
  return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

inline float VecDistSq(const float a[3], const float b[3]) {
  float dx = a[0] - b[0];
  float dy = a[1] - b[1];
  float dz = a[2] - b[2];
  return dx * dx + dy * dy + dz * dz;
}

inline bool IsFiniteVec3(const float v[3]) {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

struct Color3 {
  float r, g, b;
};

void Emit3DRibbon(std::vector<d3d::WorldVertex> &out, const float p0[3],
                  const float p1[3], float half_w0, float half_w1,
                  uint32_t col0, uint32_t col1, const CameraState &cam) {
  float dx = p1[0] - p0[0];
  float dy = p1[1] - p0[1];
  float dz = p1[2] - p0[2];
  float len_sq = dx * dx + dy * dy + dz * dz;
  if (len_sq < 1e-6f)
    return;

  float inv_len = 1.0f / std::sqrt(len_sq);
  float fx = dx * inv_len;
  float fy = dy * inv_len;
  float fz = dz * inv_len;

  float mx = 0.5f * (p0[0] + p1[0]);
  float my = 0.5f * (p0[1] + p1[1]);
  float mz = 0.5f * (p0[2] + p1[2]);
  float vx = cam.eye[0] - mx;
  float vy = cam.eye[1] - my;
  float vz = cam.eye[2] - mz;

  // Cross(fwd, to_cam) yields camera-facing sideways perpendicular vector
  float sx = fy * vz - fz * vy;
  float sy = fz * vx - fx * vz;
  float sz = fx * vy - fy * vx;
  float slen_sq = sx * sx + sy * sy + sz * sz;
  if (slen_sq < 1e-8f) {
    sx = cam.right[0];
    sy = cam.right[1];
    sz = cam.right[2];
    slen_sq = sx * sx + sy * sy + sz * sz;
    if (slen_sq < 1e-8f)
      return;
  }
  float inv_s = 1.0f / std::sqrt(slen_sq);
  sx *= inv_s;
  sy *= inv_s;
  sz *= inv_s;

  const d3d::WorldVertex v0a{p0[0] - sx * half_w0, p0[1] - sy * half_w0,
                             p0[2] - sz * half_w0, col0};
  const d3d::WorldVertex v0b{p0[0] + sx * half_w0, p0[1] + sy * half_w0,
                             p0[2] + sz * half_w0, col0};
  const d3d::WorldVertex v1b{p1[0] + sx * half_w1, p1[1] + sy * half_w1,
                             p1[2] + sz * half_w1, col1};
  const d3d::WorldVertex v1a{p1[0] - sx * half_w1, p1[1] - sy * half_w1,
                             p1[2] - sz * half_w1, col1};
  out.push_back(v0a);
  out.push_back(v0b);
  out.push_back(v1b);
  out.push_back(v0a);
  out.push_back(v1b);
  out.push_back(v1a);
}

// Emits a 10-slice two-ring radial disc (smooth Gaussian-like falloff without
// polygon edges)
void Emit3DSoftDisc(std::vector<d3d::WorldVertex> &out, const float center[3],
                    float radius, const Color3 &rgb, float center_alpha,
                    const CameraState &cam) {
  if (radius <= 1e-4f || center_alpha <= 0.004f)
    return;
  const uint32_t col_center =
      d3d::PackColorBgra(rgb.r, rgb.g, rgb.b, center_alpha);
  const uint32_t col_mid =
      d3d::PackColorBgra(rgb.r, rgb.g, rgb.b, center_alpha * 0.48f);
  const uint32_t col_edge = d3d::PackColorBgra(rgb.r, rgb.g, rgb.b, 0.0f);
  const d3d::WorldVertex vc{center[0], center[1], center[2], col_center};

  constexpr int kSlices = 10;
  constexpr float kCos[11] = {1.0f,       0.809017f, 0.309017f,  -0.309017f,
                              -0.809017f, -1.0f,     -0.809017f, -0.309017f,
                              0.309017f,  0.809017f, 1.0f};
  constexpr float kSin[11] = {0.0f,       0.587785f,  0.951057f,  0.951057f,
                              0.587785f,  0.0f,       -0.587785f, -0.951057f,
                              -0.951057f, -0.587785f, 0.0f};

  d3d::WorldVertex inner[11];
  d3d::WorldVertex outer[11];
  const float r_mid = radius * 0.44f;
  for (int i = 0; i <= kSlices; i++) {
    float rx = cam.right[0] * kCos[i] + cam.up[0] * kSin[i];
    float ry = cam.right[1] * kCos[i] + cam.up[1] * kSin[i];
    float rz = cam.right[2] * kCos[i] + cam.up[2] * kSin[i];
    inner[i] = {center[0] + rx * r_mid, center[1] + ry * r_mid,
                center[2] + rz * r_mid, col_mid};
    outer[i] = {center[0] + rx * radius, center[1] + ry * radius,
                center[2] + rz * radius, col_edge};
  }
  for (int i = 0; i < kSlices; i++) {
    out.push_back(vc);
    out.push_back(inner[i]);
    out.push_back(inner[i + 1]);
    out.push_back(inner[i]);
    out.push_back(outer[i]);
    out.push_back(outer[i + 1]);
    out.push_back(inner[i]);
    out.push_back(outer[i + 1]);
    out.push_back(inner[i + 1]);
  }
}

// Emits an organic irregular horizontal blood pool on the ground (XZ plane)
void Emit3DGroundPool(std::vector<d3d::WorldVertex> &out, const float center[3],
                      float radius, float seed, float alpha) {
  if (radius <= 0.01f || alpha <= 0.01f)
    return;
  const uint32_t col_core =
      d3d::PackColorBgra(0.048f, 0.0004f, 0.0010f, alpha * 0.97f);
  const uint32_t col_mid =
      d3d::PackColorBgra(0.032f, 0.0003f, 0.0008f, alpha * 0.86f);
  const uint32_t col_edge = d3d::PackColorBgra(0.020f, 0.0002f, 0.0006f, 0.0f);
  const d3d::WorldVertex vc{center[0], center[1], center[2], col_core};

  constexpr int kSlices = 10;
  constexpr float kCos[11] = {1.0f,       0.809017f, 0.309017f,  -0.309017f,
                              -0.809017f, -1.0f,     -0.809017f, -0.309017f,
                              0.309017f,  0.809017f, 1.0f};
  constexpr float kSin[11] = {0.0f,       0.587785f,  0.951057f,  0.951057f,
                              0.587785f,  0.0f,       -0.587785f, -0.951057f,
                              -0.951057f, -0.587785f, 0.0f};

  d3d::WorldVertex inner[11];
  d3d::WorldVertex outer[11];
  for (int i = 0; i <= kSlices; i++) {
    int idx = (i == kSlices) ? 0 : i;
    float lobe = 1.0f + 0.24f * std::sin(seed + float(idx) * 1.9f) +
                 0.14f * std::cos(seed * 1.7f + float(idx) * 3.1f);
    float r_out = radius * lobe;
    float r_mid = r_out * 0.62f;
    inner[i] = {center[0] + kCos[i] * r_mid, center[1],
                center[2] + kSin[i] * r_mid, col_mid};
    outer[i] = {center[0] + kCos[i] * r_out, center[1],
                center[2] + kSin[i] * r_out, col_edge};
  }
  for (int i = 0; i < kSlices; i++) {
    out.push_back(vc);
    out.push_back(inner[i]);
    out.push_back(inner[i + 1]);
    out.push_back(inner[i]);
    out.push_back(outer[i]);
    out.push_back(outer[i + 1]);
    out.push_back(inner[i]);
    out.push_back(outer[i + 1]);
    out.push_back(inner[i + 1]);
  }
}

// Shared textured quad emitter with generous per-tile safety padding against bilinear bleed.
void EmitOrientedTexturedQuad(std::vector<d3d::TexturedWorldVertex> &out,
                              const float center[3], const float half_u[3],
                              const float half_v[3], int tile_idx,
                              uint32_t bgra) {
  const int col = tile_idx & 3;
  const int row = (tile_idx >> 2) & 3;
  constexpr float kPad = 0.010f;
  const float u0 = float(col) * 0.25f + kPad;
  const float u1 = float(col + 1) * 0.25f - kPad;
  const float v0 = float(row) * 0.25f + kPad;
  const float v1 = float(row + 1) * 0.25f - kPad;

  const d3d::TexturedWorldVertex p0{center[0] - half_u[0] - half_v[0],
                                    center[1] - half_u[1] - half_v[1],
                                    center[2] - half_u[2] - half_v[2],
                                    bgra,
                                    u0,
                                    v1};
  const d3d::TexturedWorldVertex p1{center[0] + half_u[0] - half_v[0],
                                    center[1] + half_u[1] - half_v[1],
                                    center[2] + half_u[2] - half_v[2],
                                    bgra,
                                    u1,
                                    v1};
  const d3d::TexturedWorldVertex p2{center[0] + half_u[0] + half_v[0],
                                    center[1] + half_u[1] + half_v[1],
                                    center[2] + half_u[2] + half_v[2],
                                    bgra,
                                    u1,
                                    v0};
  const d3d::TexturedWorldVertex p3{center[0] - half_u[0] + half_v[0],
                                    center[1] - half_u[1] + half_v[1],
                                    center[2] - half_u[2] + half_v[2],
                                    bgra,
                                    u0,
                                    v0};
  out.push_back(p0);
  out.push_back(p1);
  out.push_back(p2);
  out.push_back(p0);
  out.push_back(p2);
  out.push_back(p3);
}

void EmitTexturedBillboard(std::vector<d3d::TexturedWorldVertex> &out,
                           const float center[3], float radius, float rot_rad,
                           int tile_idx, uint32_t bgra,
                           const CameraState &cam) {
  if (radius <= 1e-4f)
    return;
  const float c = std::cos(rot_rad);
  const float s = std::sin(rot_rad);
  const float half_u[3] = {(cam.right[0] * c + cam.up[0] * s) * radius,
                           (cam.right[1] * c + cam.up[1] * s) * radius,
                           (cam.right[2] * c + cam.up[2] * s) * radius};
  const float half_v[3] = {(-cam.right[0] * s + cam.up[0] * c) * radius,
                           (-cam.right[1] * s + cam.up[1] * c) * radius,
                           (-cam.right[2] * s + cam.up[2] * c) * radius};
  EmitOrientedTexturedQuad(out, center, half_u, half_v, tile_idx, bgra);
}

// Projects flow vector dir onto camera plane so elongated flame tongues point along
// the jet/rising buoyancy while gracefully foreshortening when aimed down-lens.
void EmitStretchedBillboard(std::vector<d3d::TexturedWorldVertex> &out,
                            const float center[3], const float dir[3],
                            float radius, float stretch, float rot_rad,
                            int tile_idx, uint32_t bgra,
                            const CameraState &cam) {
  if (radius <= 1e-4f)
    return;
  const float px = dir[0] * cam.right[0] + dir[1] * cam.right[1] + dir[2] * cam.right[2];
  const float py = dir[0] * cam.up[0] + dir[1] * cam.up[1] + dir[2] * cam.up[2];
  const float proj_len = std::sqrt(px * px + py * py);
  if (stretch <= 1.02f || proj_len < 0.08f) {
    EmitTexturedBillboard(out, center, radius, rot_rad, tile_idx, bgra, cam);
    return;
  }

  const float inv_p = 1.0f / proj_len;
  const float tx = px * inv_p;
  const float ty = py * inv_p;
  // Continuous small flicker sway derived from rot_rad without flipping tongues backwards.
  const float twist = std::sin(rot_rad) * 0.16f;
  const float ct = std::cos(twist);
  const float st = std::sin(twist);
  const float ax = tx * ct - ty * st;
  const float ay = tx * st + ty * ct;
  const float nx = -ay;
  const float ny = ax;

  const float proj_weight = std::clamp(proj_len * 1.15f, 0.0f, 1.0f);
  const float eff_stretch = 1.0f + (stretch - 1.0f) * proj_weight;
  const float half_along = radius * eff_stretch;
  const float half_perp = radius / std::sqrt(std::max(eff_stretch * 0.85f, 1.0f));

  const float half_u[3] = {(cam.right[0] * nx + cam.up[0] * ny) * half_perp,
                           (cam.right[1] * nx + cam.up[1] * ny) * half_perp,
                           (cam.right[2] * nx + cam.up[2] * ny) * half_perp};
  const float half_v[3] = {(cam.right[0] * ax + cam.up[0] * ay) * half_along,
                           (cam.right[1] * ax + cam.up[1] * ay) * half_along,
                           (cam.right[2] * ax + cam.up[2] * ay) * half_along};
  EmitOrientedTexturedQuad(out, center, half_u, half_v, tile_idx, bgra);
}

void EmitTexturedGroundTile(std::vector<d3d::TexturedWorldVertex> &out,
                            const float center[3], float radius, float rot_rad,
                            int tile_idx, uint32_t bgra) {
  if (radius <= 0.01f)
    return;
  const float c = std::cos(rot_rad) * radius;
  const float s = std::sin(rot_rad) * radius;
  const float half_u[3] = {c, 0.0f, s};
  const float half_v[3] = {-s, 0.0f, c};
  EmitOrientedTexturedQuad(out, center, half_u, half_v, tile_idx, bgra);
}

void GetWeaponVisualProfile(WeaponClass cls, bool suppressed, bool is_magnum,
                            float &out_thickness, float &out_trail_scale,
                            float &out_brightness) {
  switch (cls) {
  case WeaponClass::Pistol:
    out_thickness = is_magnum ? 2.4f : 1.75f;
    out_trail_scale = is_magnum ? 1.05f : 0.78f;
    out_brightness = is_magnum ? 1.15f : 0.92f;
    break;
  case WeaponClass::Smg:
    out_thickness = 1.65f;
    out_trail_scale = 0.86f;
    out_brightness = 0.95f;
    break;
  case WeaponClass::Rifle:
    out_thickness = 2.15f;
    out_trail_scale = 1.12f;
    out_brightness = 1.05f;
    break;
  case WeaponClass::Shotgun:
    out_thickness = 1.55f;
    out_trail_scale = 0.72f;
    out_brightness = 0.90f;
    break;
  case WeaponClass::Sniper:
    out_thickness = 2.75f;
    out_trail_scale = 1.65f;
    out_brightness = 1.22f;
    break;
  case WeaponClass::Lmg:
    out_thickness = 2.55f;
    out_trail_scale = 1.32f;
    out_brightness = 1.18f;
    break;
  case WeaponClass::Launcher:
    out_thickness = 3.20f;
    out_trail_scale = 1.40f;
    out_brightness = 1.25f;
    break;
  default:
    out_thickness = 2.00f;
    out_trail_scale = 1.00f;
    out_brightness = 1.00f;
    break;
  }
  if (suppressed) {
    out_thickness *= 0.80f;
    out_trail_scale *= 0.76f;
    out_brightness *= 0.68f;
  }
}

void ResolveTracerColors(uint32_t color_mode, WeaponClass cls, bool suppressed,
                         Color3 &out_core, Color3 &out_body, Color3 &out_halo) {
  // Realistic incandescent base colors (avoiding neon cyan/laser saturation)
  switch (color_mode) {
  case 1: // NATO Amber-Gold
    out_core = {1.00f, 0.98f, 0.86f};
    out_body = {1.00f, 0.78f, 0.28f};
    out_halo = {0.95f, 0.45f, 0.12f};
    break;
  case 2: // Crimson / Red Phosphorus
    out_core = {1.00f, 0.92f, 0.84f};
    out_body = {1.00f, 0.36f, 0.18f};
    out_halo = {0.85f, 0.14f, 0.08f};
    break;
  case 3: // Phosphor Green
    out_core = {0.92f, 1.00f, 0.88f};
    out_body = {0.45f, 0.95f, 0.40f};
    out_halo = {0.16f, 0.65f, 0.22f};
    break;
  case 4: // Tactical White-Steel
    out_core = {1.00f, 1.00f, 1.00f};
    out_body = {0.84f, 0.92f, 1.00f};
    out_halo = {0.55f, 0.70f, 0.88f};
    break;
  case 5: // Electric Cyan (Rail AP)
    out_core = {0.95f, 1.00f, 1.00f};
    out_body = {0.26f, 0.92f, 1.00f};
    out_halo = {0.06f, 0.56f, 1.00f};
    break;
  case 0: // Weapon-Specific Realism
  default:
    if (cls == WeaponClass::Sniper) {
      out_core = {1.00f, 0.99f, 0.94f};
      out_body = {0.98f, 0.88f, 0.62f};
      out_halo = {0.85f, 0.66f, 0.36f};
    } else if (cls == WeaponClass::Lmg || cls == WeaponClass::Launcher) {
      out_core = {1.00f, 0.94f, 0.80f};
      out_body = {1.00f, 0.56f, 0.18f};
      out_halo = {0.92f, 0.26f, 0.08f};
    } else if (cls == WeaponClass::Pistol || cls == WeaponClass::Smg) {
      out_core = {1.00f, 0.97f, 0.88f};
      out_body = {1.00f, 0.82f, 0.44f};
      out_halo = {0.88f, 0.55f, 0.18f};
    } else {
      out_core = {1.00f, 0.97f, 0.86f};
      out_body = {1.00f, 0.72f, 0.26f};
      out_halo = {0.92f, 0.40f, 0.10f};
    }
    break;
  }
  if (suppressed && color_mode == 0) {
    // Suppressed rounds glow warmer orange-amber like subsonic base tracers
    out_core = {1.00f, 0.90f, 0.72f};
    out_body = {0.95f, 0.60f, 0.25f};
    out_halo = {0.75f, 0.34f, 0.10f};
  }
}

void PushSmokeNodeLocked(const float pos[3], const float prev_pos[3],
                         bool has_prev, const float vel[3], float ttl,
                         float radius, float opacity, bool from_launcher,
                         uint32_t group_id, size_t max_cap,
                         bool is_muzzle_wisp = false,
                         float tint_override = -1.0f) {
  if (!IsFiniteVec3(pos) || max_cap == 0)
    return;

  // Locate free slot or advance ring cursor inside active capacity budget
  size_t chosen = kMaxSmokeNodes;
  for (size_t i = 0; i < max_cap; i++) {
    size_t idx = (g_smoke_cursor + i) % max_cap;
    if (!g_smoke[idx].active) {
      chosen = idx;
      break;
    }
  }
  if (chosen == kMaxSmokeNodes) {
    chosen = g_smoke_cursor % max_cap;
  }
  g_smoke_cursor = (chosen + 1) % max_cap;

  const uint64_t emit_id =
      g_smoke_emitted.fetch_add(1, std::memory_order_relaxed);
  SmokeNode &n = g_smoke[chosen];
  n.active = true;
  n.from_launcher = from_launcher;
  n.is_muzzle_wisp = is_muzzle_wisp;
  std::memcpy(n.pos, pos, sizeof(n.pos));
  if (has_prev && IsFiniteVec3(prev_pos)) {
    std::memcpy(n.prev_pos, prev_pos, sizeof(n.prev_pos));
    n.has_prev = true;
  } else {
    std::memcpy(n.prev_pos, pos, sizeof(n.prev_pos));
    n.has_prev = false;
  }
  if (vel)
    std::memcpy(n.vel, vel, sizeof(n.vel));
  else
    std::memset(n.vel, 0, sizeof(n.vel));
  n.age = 0.0f;
  n.ttl = std::clamp(ttl, 0.15f, 4.8f);
  n.base_radius = std::max(radius, 0.008f);
  n.opacity = std::clamp(opacity, 0.01f, tint_override >= 0.0f ? 0.84f : 0.45f);
  n.tint = (tint_override >= 0.0f) ? std::clamp(tint_override, 0.08f, 1.0f)
                                   : (from_launcher ? 0.68f : 0.78f);
  n.seed = float(emit_id & 63u) * 0.73f;
  n.group_id = group_id;
}

void PushHazeMoteLocked(const float pos[3], const float vel[3], float ttl,
                        float radius, float opacity, bool warm_dust,
                        size_t max_cap) {
  if (!IsFiniteVec3(pos) || max_cap == 0)
    return;
  const size_t cap = std::min(max_cap, kMaxHazeMotes);
  size_t chosen = cap;
  for (size_t i = 0; i < cap; i++) {
    size_t idx = (g_haze_cursor + i) % cap;
    if (!g_haze_motes[idx].active) {
      chosen = idx;
      break;
    }
  }
  if (chosen == cap)
    chosen = g_haze_cursor % cap;
  g_haze_cursor = (chosen + 1) % cap;

  g_fx_rng ^= g_fx_rng << 13;
  g_fx_rng ^= g_fx_rng >> 17;
  g_fx_rng ^= g_fx_rng << 5;
  const float r0 = float(g_fx_rng & 0xFFFFu) / 65535.0f;

  HazeMoteInstance &m = g_haze_motes[chosen];
  m.active = true;
  std::memcpy(m.pos, pos, sizeof(m.pos));
  if (vel)
    std::memcpy(m.vel, vel, sizeof(m.vel));
  else
    std::memset(m.vel, 0, sizeof(m.vel));
  m.age = 0.0f;
  m.ttl = std::clamp(ttl, 0.8f, 4.5f);
  m.radius = std::clamp(radius, 0.14f, 1.35f);
  m.opacity = std::clamp(opacity, 0.015f, 0.12f);
  m.rot = r0 * 6.28318f;
  m.rot_vel = (r0 - 0.5f) * 0.35f;
  m.tile = int(chosen & 3u);
  m.warm_dust = warm_dust;
}

bool LookupRecentMuzzleLocked(uint32_t owner, float out_pos[3]) {
  uint64_t best_seq = 0;
  const MuzzleEntry *best = nullptr;
  for (const auto &m : g_muzzles) {
    if (m.owner == owner && m.seq > best_seq) {
      best_seq = m.seq;
      best = &m;
    }
  }
  if (!best || (g_muzzle_seq - best_seq) > 64)
    return false;
  std::memcpy(out_pos, best->pos, sizeof(float) * 3);
  return true;
}

float ReadWorldTimeSec() {
  uint32_t world = R32(kWorld);
  if (!world || !MemMapped(world))
    return 0.0f;
  return G<float>(world + kTimerTime);
}

inline uint32_t PackColor3(const Color3 &c, float alpha) {
  return d3d::PackColorBgra(c.r, c.g, c.b, std::clamp(alpha, 0.0f, 1.0f));
}

void SampleSmokeConfig(const modsettings::EnhancedOptions &enh, bool is_perf,
                       bool &out_enabled, float &out_density, float &out_ttl,
                       size_t &out_cap) {
  out_enabled = enh.smoke_trails_enabled && (enh.smoke_mode > 0);
  if (!out_enabled) {
    out_density = 0.0f;
    out_ttl = 0.0f;
    out_cap = 0;
    return;
  }
  float mode_scale = 1.0f;
  switch (enh.smoke_mode) {
  case 1:
    mode_scale = 0.58f;
    break; // Light Smoke
  case 2:
    mode_scale = 1.00f;
    break; // Medium Smoke
  case 3:
    mode_scale = 1.48f;
    break; // Enhanced Smoke
  default:
    mode_scale = 0.0f;
    break;
  }
  float pct_scale =
      std::clamp(float(enh.smoke_intensity) * 0.01f, 0.10f, 2.00f);
  out_density = mode_scale * pct_scale;
  out_ttl = std::clamp(enh.smoke_duration, 0.20f, 3.00f);
  if (enh.smoke_mode == 1)
    out_ttl *= 0.72f;
  else if (enh.smoke_mode == 3)
    out_ttl *= 1.18f;
  out_cap = is_perf ? 64u : kMaxSmokeNodes;
}

} // namespace

bool IsPerformancePresetActive() {
  return modsettings::Registry().GetEnhanced().ballistics_preset ==
         "performance";
}

void CaptureSceneCamera() {
  uint32_t transforms = SYM(D3D_Device) + XDK_CDevice_Transforms;
  if (!transforms || !MemMapped(transforms))
    return;

  float view[16], proj[16];
  std::memcpy(view, HostPtr(transforms + 0), sizeof(view));
  std::memcpy(proj, HostPtr(transforms + 64), sizeof(proj));
  if (!std::isfinite(view[0]) || !std::isfinite(proj[5]) ||
      !std::isfinite(proj[15]))
    return;

  std::lock_guard<std::mutex> lock(g_mutex);
  std::memcpy(g_cam.view, view, sizeof(view));
  std::memcpy(g_cam.proj, proj, sizeof(proj));
  // Reconstruct camera world eye from row-major orthonormal view matrix: eye =
  // -translation * R^T
  g_cam.eye[0] =
      -(view[12] * view[0] + view[13] * view[1] + view[14] * view[2]);
  g_cam.eye[1] =
      -(view[12] * view[4] + view[13] * view[5] + view[14] * view[6]);
  g_cam.eye[2] =
      -(view[12] * view[8] + view[13] * view[9] + view[14] * view[10]);
  g_cam.right[0] = view[0];
  g_cam.right[1] = view[4];
  g_cam.right[2] = view[8];
  g_cam.up[0] = view[1];
  g_cam.up[1] = view[5];
  g_cam.up[2] = view[9];
  g_cam.forward[0] = view[2];
  g_cam.forward[1] = view[6];
  g_cam.forward[2] = view[10];
  g_cam.tan_half_fov_y =
      (std::fabs(proj[5]) > 0.05f)
          ? std::clamp(1.0f / std::fabs(proj[5]), 0.05f, 2.5f)
          : 0.70f;
  g_cam.valid = true;
}

void NotifyMuzzleMatrix(uint32_t owner_ptr, const float mat4x4[16]) {
  if (!owner_ptr || !mat4x4)
    return;
  if (!IsFiniteVec3(mat4x4 + 12))
    return;

  std::lock_guard<std::mutex> lock(g_mutex);
  g_muzzle_seq++;
  for (auto &m : g_muzzles) {
    if (m.owner == owner_ptr) {
      std::memcpy(m.pos, mat4x4 + 12, sizeof(m.pos));
      m.seq = g_muzzle_seq;
      return;
    }
  }
  MuzzleEntry &slot = g_muzzles[size_t(g_muzzle_seq % kMuzzleCacheSize)];
  slot.owner = owner_ptr;
  std::memcpy(slot.pos, mat4x4 + 12, sizeof(slot.pos));
  slot.seq = g_muzzle_seq;
}

void OnPhysicalBulletSpawn(uint32_t slot_ea, uint32_t fire_pkt,
                           const float origin[3], const float dir[3],
                           float speed, float /*range*/, uint8_t /*flags*/) {
  if (!origin || !dir || !IsFiniteVec3(origin) || !IsFiniteVec3(dir))
    return;

  const auto &enh = modsettings::Registry().GetEnhanced();
  const bool tracers_active = enh.tracers_enabled && enh.tracer_frequency != 0;
  bool smoke_enabled = false;
  float smoke_density = 0.0f, smoke_ttl = 0.0f;
  size_t smoke_cap = 0;
  SampleSmokeConfig(enh, IsPerformancePresetActive(), smoke_enabled,
                    smoke_density, smoke_ttl, smoke_cap);
  if (!tracers_active && !smoke_enabled)
    return;

  bool suppressed = false, is_player = false, is_magnum = false;
  uint32_t wpn_ptr = 0;
  WeaponClass cls =
      LookupFirePktWeapon(fire_pkt, suppressed, is_player, is_magnum, wpn_ptr);

  float world_t = ReadWorldTimeSec();
  std::lock_guard<std::mutex> lock(g_mutex);

  // Group shotgun pellets within same trigger pull (~6ms) into one shot
  // sequence
  bool same_shot_burst = (cls == WeaponClass::Shotgun && wpn_ptr != 0 &&
                          wpn_ptr == g_last_shot_wpn &&
                          std::fabs(world_t - g_last_shot_time_sec) < 0.008f);
  uint64_t seq = g_player_shot_seq;
  uint32_t group_id = g_next_group_id;
  if (!same_shot_burst) {
    seq = g_player_shot_seq++;
    group_id = g_next_group_id++;
    g_last_shot_wpn = wpn_ptr;
    g_last_shot_time_sec = world_t;
    g_shots_observed.fetch_add(1, std::memory_order_relaxed);
  }

  // Determine genuine off-axis barrel origin from sub_001D2860 when available;
  // otherwise for player first-person shots, place origin at weapon barrel in
  // camera space so tracers and muzzle smoke do not collapse onto center
  // crosshair pixel.
  float visual_origin[3] = {origin[0], origin[1], origin[2]};
  uint32_t owner_ptr =
      (wpn_ptr && MemMapped(wpn_ptr)) ? R32(wpn_ptr + 0xF0) : 0;
  float muzzle[3] = {};
  bool have_muzzle = false;
  if (owner_ptr && LookupRecentMuzzleLocked(owner_ptr, muzzle)) {
    float d[3] = {muzzle[0] - origin[0], muzzle[1] - origin[1],
                  muzzle[2] - origin[2]};
    float proj_d = d[0] * dir[0] + d[1] * dir[1] + d[2] * dir[2];
    float perp_sq = std::max(0.0f, (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) -
                                       proj_d * proj_d);
    if (proj_d > 0.10f && proj_d < 2.4f && perp_sq > 0.0020f &&
        perp_sq < 2.25f) {
      std::memcpy(visual_origin, muzzle, sizeof(visual_origin));
      have_muzzle = true;
    }
  }
  if (!have_muzzle && is_player && g_cam.valid) {
    uint32_t world = R32(kWorld);
    uint32_t player = (world && MemMapped(world)) ? (world + kWorldPlayer) : 0;
    float zoom = (player && MemMapped(player + 0x2A4u, 4u))
                     ? G<float>(player + 0x2A4u)
                     : 1.0f;
    float ads_scale = (zoom > 1.2f) ? 0.50f : 1.0f;
    const float r_off = 0.140f * ads_scale;
    const float u_off = -0.095f * ads_scale;
    const float f_off = 0.22f;
    visual_origin[0] = origin[0] + dir[0] * f_off + g_cam.right[0] * r_off +
                       g_cam.up[0] * u_off;
    visual_origin[1] = origin[1] + dir[1] * f_off + g_cam.right[1] * r_off +
                       g_cam.up[1] * u_off;
    visual_origin[2] = origin[2] + dir[2] * f_off + g_cam.right[2] * r_off +
                       g_cam.up[2] * u_off;
  }

  if (smoke_enabled) {
    if (is_player) {
      g_muzzle_emitter.heat =
          std::min(1.0f, g_muzzle_emitter.heat + (suppressed ? 0.14f : 0.22f));
      g_muzzle_emitter.suppressed = suppressed;
    }
    // Restrained initial barrel gas puff right at the muzzle tip
    if (!same_shot_burst) {
      float muzzle_pos[3] = {visual_origin[0] + dir[0] * 0.08f,
                             visual_origin[1] + dir[1] * 0.08f,
                             visual_origin[2] + dir[2] * 0.08f};
      float puff_vel[3] = {
          dir[0] * 0.42f + (is_player ? g_cam.right[0] * 0.04f : 0.0f),
          0.24f + dir[1] * 0.18f,
          dir[2] * 0.42f + (is_player ? g_cam.right[2] * 0.04f : 0.0f)};
      float base_op = is_player ? 0.115f : 0.080f;
      PushSmokeNodeLocked(
          muzzle_pos, visual_origin, false, puff_vel,
          std::clamp(smoke_ttl * 0.55f, 0.28f, 0.92f),
          0.025f * std::min(smoke_density, 1.35f),
          std::clamp(base_op * smoke_density * (suppressed ? 0.65f : 1.0f),
                     0.024f, 0.19f),
          false, group_id, smoke_cap, true, 0.52f);
    }
  }

  // Sharp 1-2 frame tactical muzzle flame (Insurgency / STALKER Realistic FX
  // style)
  if (!same_shot_burst && !suppressed) {
    MuzzleFlashNode &mf = g_muzzle_flashes[g_flash_cursor];
    g_flash_cursor = (g_flash_cursor + 1) % kMaxMuzzleFlashes;
    mf.active = true;
    mf.pos[0] = visual_origin[0] + dir[0] * 0.06f;
    mf.pos[1] = visual_origin[1] + dir[1] * 0.06f;
    mf.pos[2] = visual_origin[2] + dir[2] * 0.06f;
    std::memcpy(mf.dir, dir, sizeof(mf.dir));
    mf.age = 0.0f;
    mf.ttl = 0.052f;
    mf.radius = (cls == WeaponClass::Shotgun || cls == WeaponClass::Sniper ||
                 cls == WeaponClass::Lmg)
                    ? 0.25f
                    : (cls == WeaponClass::Pistol ? 0.13f : 0.18f);
    mf.rot = float((seq * 1103515245u) & 0xFFFFu) * (6.28318f / 65535.0f);
    mf.tile = 4 + int(seq & 3u);
  }

  const uint32_t special_mode =
      is_player ? enh.special_ammo_mode : 0u;
  if (!tracers_active ||
      (!(special_mode != 0u && enh.tracer_frequency != 0) &&
       !ShouldEmitTracer(enh.tracer_frequency, seq, cls, suppressed, is_player)))
    return;

  const size_t max_t = IsPerformancePresetActive() ? 32u : kMaxTracers;
  size_t chosen = max_t;
  for (size_t i = 0; i < max_t; i++) {
    size_t idx = (g_tracer_cursor + i) % max_t;
    if (!g_tracers[idx].active) {
      chosen = idx;
      break;
    }
  }
  if (chosen == max_t)
    chosen = g_tracer_cursor % max_t;
  g_tracer_cursor = (chosen + 1) % max_t;

  TracerInstance &t = g_tracers[chosen];
  t.active = true;
  t.is_physical = true;
  t.is_player = is_player;
  t.hit_locked = false;
  t.suppressed = suppressed;
  t.cls = cls;
  t.guest_slot = slot_ea;
  std::memcpy(t.visual_origin, visual_origin, sizeof(visual_origin));
  std::memcpy(t.physics_origin, origin, sizeof(t.physics_origin));
  std::memcpy(t.head_pos, origin, sizeof(t.head_pos));
  std::memcpy(t.target_end, origin, sizeof(t.target_end));
  std::memcpy(t.dir, dir, sizeof(t.dir));
  t.speed = std::max(speed, 25.0f);
  t.age = 0.0f;
  t.hit_age = -1.0f;
  t.afterglow_ttl =
      (cls == WeaponClass::Sniper || cls == WeaponClass::Lmg) ? 0.16f : 0.12f;
  GetWeaponVisualProfile(cls, suppressed, is_magnum, t.core_thickness,
                         t.trail_scale, t.brightness);
  t.color_override = enh.tracer_color;
  if (special_mode != 0u) {
    const auto prof = game::overdrive_rules::GetSpecialAmmoProfile(
        game::overdrive_rules::SanitizeSpecialAmmo(special_mode));
    if (prof.tracer_color_override != 0u)
      t.color_override = prof.tracer_color_override;
    if (special_mode == 3u) {
      t.core_thickness *= 1.25f;
      t.trail_scale *= 1.18f;
      t.brightness *= 1.20f;
    }
  }
  t.group_id = group_id;
  t.last_smoke_dist = 0.0f;
  g_tracers_emitted.fetch_add(1, std::memory_order_relaxed);
}

void OnPhysicalBulletStep(uint32_t slot_ea, const float prev_pos[3],
                          const float new_pos[3], bool active_after) {
  if (!slot_ea || !prev_pos || !new_pos || !IsFiniteVec3(new_pos))
    return;
  const auto &enh = modsettings::Registry().GetEnhanced();
  bool smoke_enabled = false;
  float smoke_density = 0.0f, smoke_ttl = 0.0f;
  size_t smoke_cap = 0;
  SampleSmokeConfig(enh, IsPerformancePresetActive(), smoke_enabled,
                    smoke_density, smoke_ttl, smoke_cap);

  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto &t : g_tracers) {
    if (!t.active || !t.is_physical || t.guest_slot != slot_ea)
      continue;
    std::memcpy(t.head_pos, new_pos, sizeof(t.head_pos));
    if (!active_after && !t.hit_locked) {
      t.hit_locked = true;
      t.hit_age = 0.0f;
    }
    if (smoke_enabled) {
      float traveled = std::sqrt(VecDistSq(t.physics_origin, new_pos));
      const float step_interval =
          (t.cls == WeaponClass::Sniper || t.cls == WeaponClass::Lmg) ? 4.5f
                                                                      : 7.0f;
      if (traveled >= 2.2f && (traveled - t.last_smoke_dist) >= step_interval) {
        t.last_smoke_dist = traveled;
        float vel[3] = {0.03f, 0.16f, 0.03f};
        float base_op = (t.cls == WeaponClass::Sniper) ? 0.065f : 0.040f;
        PushSmokeNodeLocked(
            new_pos, prev_pos, false, vel, smoke_ttl * 0.65f,
            0.019f * std::min(smoke_density, 1.3f),
            std::clamp(base_op * smoke_density * (t.suppressed ? 0.60f : 1.0f),
                       0.015f, 0.10f),
            false, t.group_id, smoke_cap, false);
      }
    }
    break;
  }
}

void RegisterThermalSlot(uint32_t slot_ea, const float dir[3]) {
  if (!slot_ea || !dir || !IsFiniteVec3(dir))
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto now = std::chrono::steady_clock::now();
  size_t idx = g_thermal_cursor % kMaxThermalSlots;
  for (size_t i = 0; i < kMaxThermalSlots; i++) {
    size_t c = (g_thermal_cursor + i) % kMaxThermalSlots;
    if (!g_thermal_slots[c].active || g_thermal_slots[c].slot_ea == slot_ea ||
        std::chrono::duration<float>(now - g_thermal_slots[c].tp).count() >
            1.2f) {
      idx = c;
      break;
    }
  }
  g_thermal_cursor = (idx + 1) % kMaxThermalSlots;
  ThermalSlotEntry &e = g_thermal_slots[idx];
  e.active = true;
  e.slot_ea = slot_ea;
  std::memcpy(e.dir, dir, sizeof(e.dir));
  e.tp = now;
}

bool InThermalImpact() { return g_in_thermal_impact; }

static bool IsThermalSlotLocked(uint32_t slot_ea, bool consume,
                                float out_dir[3]);

bool BeginThermalImpact(uint32_t slot_ea) {
  const bool prev = g_in_thermal_impact;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_in_thermal_impact = IsThermalSlotLocked(slot_ea, false, nullptr);
  return prev;
}

bool BeginExternalThermalImpact() {
  const bool prev = g_in_thermal_impact;
  g_in_thermal_impact = true;
  return prev;
}

void EndThermalImpact(bool prev) { g_in_thermal_impact = prev; }

static bool IsThermalSlotLocked(uint32_t slot_ea, bool consume,
                                float out_dir[3]) {
  if (!slot_ea)
    return false;
  const auto now = std::chrono::steady_clock::now();
  for (auto &e : g_thermal_slots) {
    if (!e.active || e.slot_ea != slot_ea)
      continue;
    if (std::chrono::duration<float>(now - e.tp).count() > 1.2f) {
      e.active = false;
      continue;
    }
    if (out_dir)
      std::memcpy(out_dir, e.dir, 12);
    if (consume)
      e.active = false;
    return true;
  }
  return false;
}

void OnPhysicalBulletHit(uint32_t slot_ea, const float hit_pos[3]) {
  if (!slot_ea || !hit_pos || !IsFiniteVec3(hit_pos))
    return;
  const auto &enh = modsettings::Registry().GetEnhanced();
  const bool diverse_impacts = enh.diverse_bullet_impacts;
  const bool perf = IsPerformancePresetActive();
  std::lock_guard<std::mutex> lock(g_mutex);
  float thermal_dir[3] = {0.0f, 0.0f, 1.0f};
  if (IsThermalSlotLocked(slot_ea, true, thermal_dir)) {
    auto next_rand = [&]() -> float {
      g_fx_rng ^= g_fx_rng << 13;
      g_fx_rng ^= g_fx_rng >> 17;
      g_fx_rng ^= g_fx_rng << 5;
      return float(g_fx_rng & 0xFFFFu) / 65535.0f;
    };
    const int flame_count = perf ? 1 : 2;
    for (int i = 0; i < flame_count; i++) {
      size_t chosen = kMaxExplosionFlames;
      for (size_t k = 0; k < kMaxExplosionFlames; k++) {
        size_t idx = (g_exp_flame_cursor + k) % kMaxExplosionFlames;
        if (!g_explosion_flames[idx].active) {
          chosen = idx;
          break;
        }
      }
      if (chosen == kMaxExplosionFlames)
        chosen = g_exp_flame_cursor % kMaxExplosionFlames;
      g_exp_flame_cursor = (chosen + 1) % kMaxExplosionFlames;
      ExplosionFlameNode &f = g_explosion_flames[chosen];
      f.active = true;
      f.is_shockwave = false;
      f.pos[0] =
          hit_pos[0] - thermal_dir[0] * 0.08f + (next_rand() - 0.5f) * 0.16f;
      f.pos[1] = hit_pos[1] + (next_rand() - 0.35f) * 0.14f;
      f.pos[2] =
          hit_pos[2] - thermal_dir[2] * 0.08f + (next_rand() - 0.5f) * 0.16f;
      f.vel[0] = -thermal_dir[0] * (0.4f + next_rand() * 0.8f) +
                 (next_rand() - 0.5f) * 0.9f;
      f.vel[1] = 0.8f + next_rand() * 1.6f;
      f.vel[2] = -thermal_dir[2] * (0.4f + next_rand() * 0.8f) +
                 (next_rand() - 0.5f) * 0.9f;
      f.age = 0.0f;
      f.ttl = 0.42f + next_rand() * 0.36f;
      f.start_rad = 0.16f + next_rand() * 0.08f;
      f.end_rad = 0.52f + next_rand() * 0.28f;
      f.rot = next_rand() * 6.28318f;
      f.rot_vel = (next_rand() - 0.5f) * 5.0f;
      f.tile = 4 + (int(chosen + uint32_t(next_rand() * 4.0f)) & 3);
      f.heat = 1.25f;
    }
    const int ember_count = perf ? 2 : 4;
    for (int i = 0; i < ember_count; i++) {
      size_t chosen = kMaxExplosionEmbers;
      for (size_t k = 0; k < kMaxExplosionEmbers; k++) {
        size_t idx = (g_exp_ember_cursor + k) % kMaxExplosionEmbers;
        if (!g_explosion_embers[idx].active) {
          chosen = idx;
          break;
        }
      }
      if (chosen == kMaxExplosionEmbers)
        chosen = g_exp_ember_cursor % kMaxExplosionEmbers;
      g_exp_ember_cursor = (chosen + 1) % kMaxExplosionEmbers;
      ExplosionEmberNode &em = g_explosion_embers[chosen];
      const float spd = 1.8f + next_rand() * 4.2f;
      em.active = true;
      em.pos[0] = hit_pos[0] - thermal_dir[0] * 0.04f;
      em.pos[1] = hit_pos[1];
      em.pos[2] = hit_pos[2] - thermal_dir[2] * 0.04f;
      std::memcpy(em.prev_pos, em.pos, sizeof(em.prev_pos));
      em.vel[0] = (-thermal_dir[0] * 0.65f + (next_rand() - 0.5f) * 1.4f) * spd;
      em.vel[1] = (0.4f + next_rand() * 1.1f) * spd;
      em.vel[2] = (-thermal_dir[2] * 0.65f + (next_rand() - 0.5f) * 1.4f) * spd;
      em.ground_y = hit_pos[1] - 0.9f;
      em.age = 0.0f;
      em.ttl = 0.35f + next_rand() * 0.45f;
      em.radius = 0.011f + next_rand() * 0.010f;
      em.temp = 0.95f + next_rand() * 0.25f;
    }
    const float sv[3] = {-thermal_dir[0] * 0.35f, 1.3f + next_rand() * 0.9f,
                         -thermal_dir[2] * 0.35f};
    const float sp[3] = {hit_pos[0] - thermal_dir[0] * 0.06f,
                         hit_pos[1] + 0.08f,
                         hit_pos[2] - thermal_dir[2] * 0.06f};
    PushSmokeNodeLocked(sp, hit_pos, false, sv, 1.55f + next_rand() * 0.75f,
                        0.22f + next_rand() * 0.14f, 0.44f, true, 0u,
                        perf ? 64u : kMaxSmokeNodes, false, 0.16f);
    return;
  }
  float impact_dir[3] = {0.0f, 1.0f, 0.0f};
  bool found_tracer = false;
  for (auto &t : g_tracers) {
    if (!t.active || !t.is_physical || t.guest_slot != slot_ea)
      continue;
    std::memcpy(t.head_pos, hit_pos, sizeof(t.head_pos));
    t.hit_locked = true;
    if (t.hit_age < 0.0f)
      t.hit_age = 0.0f;
    impact_dir[0] = -t.dir[0];
    impact_dir[1] = -t.dir[1];
    impact_dir[2] = -t.dir[2];
    found_tracer = true;
    break;
  }
  if (diverse_impacts && found_tracer) {
    auto next_rand = [&]() -> float {
      g_fx_rng ^= g_fx_rng << 13;
      g_fx_rng ^= g_fx_rng >> 17;
      g_fx_rng ^= g_fx_rng << 5;
      return float(g_fx_rng & 0xFFFFu) / 65535.0f;
    };
    auto push_imp = [&](bool is_spark, const float vel[3], float ttl, float rad,
                        float op) {
      size_t chosen = kMaxImpactNodes;
      for (size_t i = 0; i < kMaxImpactNodes; i++) {
        size_t idx = (g_impact_cursor + i) % kMaxImpactNodes;
        if (!g_impacts[idx].active) {
          chosen = idx;
          break;
        }
      }
      if (chosen == kMaxImpactNodes)
        chosen = g_impact_cursor % kMaxImpactNodes;
      g_impact_cursor = (chosen + 1) % kMaxImpactNodes;
      ImpactNode &n = g_impacts[chosen];
      n.active = true;
      n.is_spark = is_spark;
      std::memcpy(n.pos, hit_pos, sizeof(n.pos));
      std::memcpy(n.prev_pos, hit_pos, sizeof(n.prev_pos));
      std::memcpy(n.vel, vel, sizeof(n.vel));
      n.age = 0.0f;
      n.ttl = ttl;
      n.radius = rad;
      n.opacity = op;
    };
    for (int i = 0; i < 6; i++) {
      float spd = 3.2f + next_rand() * 5.8f;
      float vel[3] = {
          (impact_dir[0] * 0.75f + (next_rand() - 0.5f) * 1.55f) * spd,
          (impact_dir[1] * 0.45f + 0.38f + next_rand() * 0.90f) * spd,
          (impact_dir[2] * 0.75f + (next_rand() - 0.5f) * 1.55f) * spd};
      push_imp(true, vel, 0.16f + next_rand() * 0.18f, 0.013f, 0.95f);
    }
    for (int i = 0; i < 3; i++) {
      float spd = 0.85f + next_rand() * 1.55f;
      float vel[3] = {
          (impact_dir[0] * 0.65f + (next_rand() - 0.5f) * 0.95f) * spd,
          (0.50f + next_rand() * 0.75f) * spd,
          (impact_dir[2] * 0.65f + (next_rand() - 0.5f) * 0.95f) * spd};
      push_imp(false, vel, 0.32f + next_rand() * 0.26f,
               0.090f + next_rand() * 0.06f, 0.28f);
    }
    if (enh.smoke_trails_enabled && enh.smoke_mode > 0 && (g_fx_rng & 3u) == 0u) {
      const float haze_v[3] = {(next_rand() - 0.5f) * 0.22f,
                               0.16f + next_rand() * 0.18f,
                               (next_rand() - 0.5f) * 0.22f};
      PushHazeMoteLocked(hit_pos, haze_v, 2.2f + next_rand() * 1.4f,
                         0.28f + next_rand() * 0.18f, 0.055f, true,
                         perf ? 28u : kMaxHazeMotes);
    }
  }
}

void OnEnemyBloodHit(uint32_t victim_ea, const float hit_pos[3], float ground_y,
                     float wound_y_offset, const float hit_dir[3],
                     float impact_force, bool is_headshot, bool is_lethal) {
  if (!hit_pos || !IsFiniteVec3(hit_pos))
    return;
  const auto &enh = modsettings::Registry().GetEnhanced();
  const uint32_t mult = std::clamp(enh.blood_particle_multiplier, 1u, 4u);

  float dir[3] = {0.0f, 0.0f, 1.0f};
  if (hit_dir && IsFiniteVec3(hit_dir)) {
    float len_sq = hit_dir[0] * hit_dir[0] + hit_dir[1] * hit_dir[1] +
                   hit_dir[2] * hit_dir[2];
    if (len_sq > 1e-6f) {
      float inv = 1.0f / std::sqrt(len_sq);
      dir[0] = hit_dir[0] * inv;
      dir[1] = hit_dir[1] * inv;
      dir[2] = hit_dir[2] * inv;
    }
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  auto next_rand = [&]() -> float {
    g_fx_rng ^= g_fx_rng << 13;
    g_fx_rng ^= g_fx_rng >> 17;
    g_fx_rng ^= g_fx_rng << 5;
    return float(g_fx_rng & 0xFFFFu) / 65535.0f;
  };
  auto push_blood = [&](bool is_mist, const float pos[3], const float vel[3],
                        float ttl, float rad, float op, float r, float g,
                        float b) {
    size_t chosen = kMaxBloodNodes;
    for (size_t i = 0; i < kMaxBloodNodes; i++) {
      size_t idx = (g_blood_cursor + i) % kMaxBloodNodes;
      if (!g_blood[idx].active) {
        chosen = idx;
        break;
      }
    }
    if (chosen == kMaxBloodNodes)
      chosen = g_blood_cursor % kMaxBloodNodes;
    g_blood_cursor = (chosen + 1) % kMaxBloodNodes;
    BloodNode &bn = g_blood[chosen];
    bn.active = true;
    bn.is_mist = is_mist;
    std::memcpy(bn.pos, pos, sizeof(bn.pos));
    std::memcpy(bn.prev_pos, pos, sizeof(bn.prev_pos));
    std::memcpy(bn.vel, vel, sizeof(bn.vel));
    bn.ground_y = ground_y;
    bn.age = 0.0f;
    bn.ttl = ttl;
    bn.radius = rad;
    bn.opacity = op;
    bn.r = r;
    bn.g = g;
    bn.b = b;
  };

  const float force_scale =
      std::clamp(0.85f + impact_force * 0.25f, 0.85f, 1.85f);
  const int mist_count = 1 + (mult >= 3u ? 1 : 0) + (is_headshot ? 1 : 0);
  const int drop_count =
      int(10 + mult * 7) + (is_headshot ? 10 : 0) + (is_lethal ? 8 : 0);

  // Layer 1: Brief, crisp dark venous splash puff (dissipates quickly, never
  // hovers in layers)
  for (int i = 0; i < mist_count; i++) {
    const bool exit_side = (i % 3 != 0);
    const float side_sign = exit_side ? 1.0f : -0.55f;
    float pos[3] = {hit_pos[0] + dir[0] * (exit_side ? 0.12f : -0.08f) +
                        (next_rand() - 0.5f) * 0.06f,
                    hit_pos[1] + (next_rand() - 0.45f) * 0.07f,
                    hit_pos[2] + dir[2] * (exit_side ? 0.12f : -0.08f) +
                        (next_rand() - 0.5f) * 0.06f};
    float spd = (0.85f + next_rand() * 1.15f) * force_scale;
    float vel[3] = {
        (dir[0] * side_sign + (next_rand() - 0.5f) * 0.75f) * spd,
        (-0.15f + next_rand() * 0.35f + (is_headshot ? 0.20f : 0.0f)) * spd,
        (dir[2] * side_sign + (next_rand() - 0.5f) * 0.75f) * spd};
    float crimson = 0.044f + next_rand() * 0.022f;
    push_blood(true, pos, vel, 0.14f + next_rand() * 0.12f,
               (0.055f + next_rand() * 0.045f) * (is_headshot ? 1.25f : 1.0f),
               0.72f + next_rand() * 0.18f, crimson, 0.0005f, 0.0012f);
  }

  // Layer 2: Thick dark venous liquid jets & clots (arc under gravity onto the
  // floor)
  for (int i = 0; i < drop_count; i++) {
    const bool exit_spray = (i % 4 != 0);
    const float dir_sign = exit_spray ? 1.0f : -0.60f;
    float pos[3] = {hit_pos[0] + dir[0] * (exit_spray ? 0.10f : -0.07f) +
                        (next_rand() - 0.5f) * 0.05f,
                    hit_pos[1] + (next_rand() - 0.5f) * 0.07f,
                    hit_pos[2] + dir[2] * (exit_spray ? 0.10f : -0.07f) +
                        (next_rand() - 0.5f) * 0.05f};
    float spd = (exit_spray ? (2.0f + next_rand() * 3.4f)
                            : (1.0f + next_rand() * 2.0f)) *
                force_scale;
    float vel[3] = {
        (dir[0] * dir_sign + (next_rand() - 0.5f) * 0.85f) * spd,
        (-0.10f + next_rand() * 0.45f + (is_headshot ? 0.28f : 0.0f)) * spd *
            0.48f,
        (dir[2] * dir_sign + (next_rand() - 0.5f) * 0.85f) * spd};
    float dark_red = 0.036f + next_rand() * 0.024f;
    push_blood(false, pos, vel, 0.55f + next_rand() * 0.40f,
               0.018f + next_rand() * 0.016f, 0.94f + next_rand() * 0.05f,
               dark_red, 0.0004f, 0.0010f);
  }

  // Layer 3: Register/refresh continuous pouring wound stream on this soldier
  if (victim_ea != 0) {
    size_t slot = kMaxWoundStreams;
    for (size_t i = 0; i < kMaxWoundStreams; i++) {
      if (g_wound_streams[i].active &&
          g_wound_streams[i].victim_ea == victim_ea) {
        slot = i;
        break;
      }
    }
    if (slot == kMaxWoundStreams) {
      for (size_t i = 0; i < kMaxWoundStreams; i++) {
        size_t idx = (g_wound_cursor + i) % kMaxWoundStreams;
        if (!g_wound_streams[idx].active) {
          slot = idx;
          break;
        }
      }
    }
    if (slot == kMaxWoundStreams)
      slot = g_wound_cursor % kMaxWoundStreams;
    g_wound_cursor = (slot + 1) % kMaxWoundStreams;

    WoundBleedStream &ws = g_wound_streams[slot];
    ws.active = true;
    ws.victim_ea = victim_ea;
    ws.y_offset = is_lethal ? std::min(wound_y_offset, 0.55f) : wound_y_offset;
    std::memcpy(ws.dir, dir, sizeof(ws.dir));
    ws.ground_y = ground_y;
    ws.remaining = is_lethal ? 3.2f : 1.6f;
    ws.emit_accum = 0.03f;
    ws.mult = mult;
    ws.is_headshot = is_headshot;
  }

  // Layer 4: Immediate ground splatter beneath the wounded soldier
  {
    const float pool_ttl = enh.persistent_blood_decals ? 22.0f : 8.0f;
    float p_pos[3] = {hit_pos[0] + dir[0] * (0.18f + next_rand() * 0.25f),
                      ground_y,
                      hit_pos[2] + dir[2] * (0.18f + next_rand() * 0.25f)};
    size_t chosen = kMaxBloodPools;
    for (size_t i = 0; i < kMaxBloodPools; i++) {
      if (g_blood_pools[i].active &&
          VecDistSq(g_blood_pools[i].pos, p_pos) < 0.22f * 0.22f) {
        chosen = i;
        g_blood_pools[i].target_radius =
            std::min(0.82f, g_blood_pools[i].target_radius +
                                (is_lethal ? 0.16f : 0.08f));
        g_blood_pools[i].age = 0.0f;
        g_blood_pools[i].ttl = pool_ttl;
        break;
      }
    }
    if (chosen == kMaxBloodPools) {
      for (size_t i = 0; i < kMaxBloodPools; i++) {
        size_t idx = (g_pool_cursor + i) % kMaxBloodPools;
        if (!g_blood_pools[idx].active) {
          chosen = idx;
          break;
        }
      }
      if (chosen == kMaxBloodPools)
        chosen = g_pool_cursor % kMaxBloodPools;
      g_pool_cursor = (chosen + 1) % kMaxBloodPools;
      BloodPoolNode &pl = g_blood_pools[chosen];
      pl.active = true;
      std::memcpy(pl.pos, p_pos, sizeof(pl.pos));
      pl.age = 0.0f;
      pl.ttl = pool_ttl;
      pl.radius = 0.11f;
      pl.target_radius = (is_lethal ? 0.52f : 0.28f) + float(mult) * 0.045f;
      pl.seed = next_rand() * 6.28f;
      pl.opacity = 0.94f;
    }
  }
}

void OnExplosionDetonate(const float pos[3], float radius, float force) {
  if (!pos || !IsFiniteVec3(pos))
    return;
  if ((pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]) < 0.01f)
    return;

  const auto &enh = modsettings::Registry().GetEnhanced();
  const bool perf = IsPerformancePresetActive();
  const float rad_scale = std::clamp(radius / 5.5f, 0.70f, 2.25f) *
                          std::clamp(0.75f + force * 0.25f, 0.75f, 1.75f);
  const float glow_mul =
      std::clamp(float(enh.explosion_glow_boost) * 0.01f, 0.55f, 2.25f);

  bool smoke_enabled = false;
  float smoke_density = 0.0f, smoke_ttl = 0.0f;
  size_t smoke_cap = 0;
  SampleSmokeConfig(enh, perf, smoke_enabled, smoke_density, smoke_ttl,
                    smoke_cap);

  std::unique_lock<std::mutex> lock(g_mutex);
  const auto now = std::chrono::steady_clock::now();
  for (const auto &rec : g_recent_explosions) {
    if (!rec.valid)
      continue;
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - rec.tp)
            .count();
    if (ms >= 0 && ms < 200 && VecDistSq(rec.pos, pos) < 1.35f * 1.35f) {
      return;
    }
  }
  RecentExplosion &slot = g_recent_explosions[g_recent_exp_cursor];
  g_recent_exp_cursor = (g_recent_exp_cursor + 1) % kMaxRecentExplosions;
  slot.valid = true;
  std::memcpy(slot.pos, pos, sizeof(slot.pos));
  slot.tp = now;

  auto next_rand = [&]() -> float {
    g_fx_rng ^= g_fx_rng << 13;
    g_fx_rng ^= g_fx_rng >> 17;
    g_fx_rng ^= g_fx_rng << 5;
    return float(g_fx_rng & 0xFFFFu) / 65535.0f;
  };
  auto push_flame = [&](bool is_shock, const float p[3], const float v[3],
                        float ttl, float r0, float r1, int tile, float heat,
                        bool air_ring = false, bool flash = false) {
    size_t chosen = kMaxExplosionFlames;
    for (size_t i = 0; i < kMaxExplosionFlames; i++) {
      size_t idx = (g_exp_flame_cursor + i) % kMaxExplosionFlames;
      if (!g_explosion_flames[idx].active) {
        chosen = idx;
        break;
      }
    }
    if (chosen == kMaxExplosionFlames)
      chosen = g_exp_flame_cursor % kMaxExplosionFlames;
    g_exp_flame_cursor = (chosen + 1) % kMaxExplosionFlames;
    ExplosionFlameNode &f = g_explosion_flames[chosen];
    f.active = true;
    f.is_shockwave = is_shock;
    f.is_air_ring = air_ring;
    f.is_flash = flash;
    f.is_burn_fire = false;
    std::memcpy(f.pos, p, sizeof(f.pos));
    std::memcpy(f.vel, v, sizeof(f.vel));
    f.age = 0.0f;
    f.ttl = ttl;
    f.start_rad = r0;
    f.end_rad = r1;
    f.rot = next_rand() * 6.28318f;
    f.rot_vel = (next_rand() - 0.5f) * 4.8f;
    f.tile = tile;
    f.heat = heat;
  };

  // 1. Ignition flash: one very short white-hot pop that sells the detonation
  {
    const float fp[3] = {pos[0], pos[1] + 0.30f * rad_scale, pos[2]};
    const float fv[3] = {0.0f, 0.55f, 0.0f};
    push_flame(false, fp, fv, 0.13f, 0.60f * rad_scale, 7.20f * rad_scale, 5,
               3.1f * glow_mul, false, true);
  }

  // 2. Three-shell fireball: incandescent core, rolling orange body, soot-lit
  // rim
  const int core_count = perf ? 2 : 3;
  for (int i = 0; i < core_count; i++) {
    const float ang = next_rand() * 6.28318f;
    const float p[3] = {pos[0] + std::cos(ang) * 0.18f,
                        pos[1] + 0.20f + next_rand() * 0.22f,
                        pos[2] + std::sin(ang) * 0.18f};
    const float v[3] = {std::cos(ang) * (0.35f + next_rand() * 0.90f),
                        1.70f + next_rand() * 1.70f,
                        std::sin(ang) * (0.35f + next_rand() * 0.90f)};
    push_flame(false, p, v, 0.30f + next_rand() * 0.16f,
               (0.85f + next_rand() * 0.22f) * rad_scale,
               (3.00f + next_rand() * 0.80f) * rad_scale, 4 + (int(i) & 1),
               (1.55f + next_rand() * 0.35f) * glow_mul);
  }
  const int mid_count = perf ? 4 : 6;
  for (int i = 0; i < mid_count; i++) {
    const float ang = next_rand() * 6.28318f;
    const float out_spd = 2.4f + next_rand() * 3.4f;
    const float p[3] = {
        pos[0] + std::cos(ang) * (0.30f + next_rand() * 0.30f) * rad_scale,
        pos[1] + 0.10f + next_rand() * 0.55f,
        pos[2] + std::sin(ang) * (0.30f + next_rand() * 0.30f) * rad_scale};
    const float v[3] = {std::cos(ang) * out_spd, 2.0f + next_rand() * 2.6f,
                        std::sin(ang) * out_spd};
    push_flame(false, p, v, 0.52f + next_rand() * 0.26f,
               (0.62f + next_rand() * 0.24f) * rad_scale,
               (3.40f + next_rand() * 1.10f) * rad_scale,
               4 + (int(next_rand() * 4.0f) & 3),
               (1.05f + next_rand() * 0.30f) * glow_mul);
  }
  const int outer_count = perf ? 6 : 10;
  for (int i = 0; i < outer_count; i++) {
    const float ang = next_rand() * 6.28318f;
    const float out_spd = 3.8f + next_rand() * 5.4f;
    const float p[3] = {
        pos[0] + std::cos(ang) * (0.40f + next_rand() * 0.35f) * rad_scale,
        pos[1] + 0.08f + next_rand() * 0.62f,
        pos[2] + std::sin(ang) * (0.40f + next_rand() * 0.35f) * rad_scale};
    const float v[3] = {std::cos(ang) * out_spd, 2.4f + next_rand() * 4.0f,
                        std::sin(ang) * out_spd};
    push_flame(false, p, v, 0.74f + next_rand() * 0.46f,
               (0.45f + next_rand() * 0.26f) * rad_scale,
               (4.20f + next_rand() * 1.80f) * rad_scale,
               4 + (int(next_rand() * 4.0f) & 3),
               (0.78f + next_rand() * 0.26f) * glow_mul);
  }

  // 2. Radial ground shockwave & dust blast rings
  for (int i = 0; i < 2; i++) {
    const float p[3] = {pos[0], pos[1] - 0.10f, pos[2]};
    const float v[3] = {0.0f, 0.05f, 0.0f};
    push_flame(true, p, v, 0.30f + float(i) * 0.12f, 0.75f * rad_scale,
               (4.4f + float(i) * 1.4f) * rad_scale, 14 + (i & 1),
               0.85f * glow_mul);
  }

  // 3. Flying incandescent sparks & hot glowing embers (arcing &
  // floor-bouncing)
  const int ember_count = perf ? 20 : 38;
  for (int i = 0; i < ember_count; i++) {
    size_t chosen = kMaxExplosionEmbers;
    for (size_t k = 0; k < kMaxExplosionEmbers; k++) {
      size_t idx = (g_exp_ember_cursor + k) % kMaxExplosionEmbers;
      if (!g_explosion_embers[idx].active) {
        chosen = idx;
        break;
      }
    }
    if (chosen == kMaxExplosionEmbers)
      chosen = g_exp_ember_cursor % kMaxExplosionEmbers;
    g_exp_ember_cursor = (chosen + 1) % kMaxExplosionEmbers;
    ExplosionEmberNode &em = g_explosion_embers[chosen];
    em.active = true;
    const float az = next_rand() * 6.28318f;
    const float elev = -0.10f + next_rand() * 1.15f;
    const float spd = (5.8f + next_rand() * 13.5f) *
                      std::clamp(0.85f + rad_scale * 0.25f, 0.85f, 1.55f);
    const float ce = std::cos(elev);
    em.pos[0] = pos[0] + std::cos(az) * 0.22f;
    em.pos[1] = pos[1] + 0.12f + next_rand() * 0.30f;
    em.pos[2] = pos[2] + std::sin(az) * 0.22f;
    std::memcpy(em.prev_pos, em.pos, sizeof(em.prev_pos));
    em.vel[0] = std::cos(az) * ce * spd;
    em.vel[1] = std::max(1.8f, std::sin(elev) * spd + 1.5f);
    em.vel[2] = std::sin(az) * ce * spd;
    em.ground_y = pos[1] - 0.35f;
    em.age = 0.0f;
    em.ttl = 0.55f + next_rand() * 0.95f;
    em.radius = 0.016f + next_rand() * 0.018f;
    em.temp = 0.78f + next_rand() * 0.42f;
  }

  // 4. Heavy billowing dark soot & warm-underlit smoke column
  const size_t plume_cap = std::max<size_t>(smoke_cap, 48u);
  const int plume_count = perf ? 10 : 18;
  const float base_smoke_op =
      smoke_enabled
          ? std::clamp(0.62f * std::max(smoke_density, 0.85f), 0.48f, 0.82f)
          : 0.56f;
  const float base_smoke_ttl =
      smoke_enabled ? std::clamp(smoke_ttl * 1.85f, 2.2f, 4.6f) : 2.8f;
  for (int i = 0; i < plume_count; i++) {
    const float ang = next_rand() * 6.28318f;
    const float off_r = (0.20f + next_rand() * 0.85f) * rad_scale;
    const float p[3] = {pos[0] + std::cos(ang) * off_r,
                        pos[1] + 0.15f + next_rand() * 0.75f * rad_scale,
                        pos[2] + std::sin(ang) * off_r};
    const float out_spd =
        (i < 6) ? (2.2f + next_rand() * 2.8f) : (0.7f + next_rand() * 1.9f);
    const float v[3] = {std::cos(ang) * out_spd,
                        (i < 6) ? (0.45f + next_rand() * 1.15f)
                                : (1.85f + next_rand() * 2.95f),
                        std::sin(ang) * out_spd};
    const float dark_tint = 0.16f + next_rand() * 0.18f;
    PushSmokeNodeLocked(p, pos, false, v,
                        base_smoke_ttl * (0.72f + next_rand() * 0.55f),
                        (0.28f + next_rand() * 0.26f) * rad_scale,
                        base_smoke_op * (0.80f + next_rand() * 0.25f), true, 0u,
                        plume_cap, false, dark_tint);
  }

  // 5. Lingering F.E.A.R.-style illuminated battle haze motes drifting lazily through aftermath
  const int haze_count = perf ? 3 : 6;
  for (int i = 0; i < haze_count; i++) {
    const float ang = next_rand() * 6.28318f;
    const float off = (0.35f + next_rand() * 1.65f) * rad_scale;
    const float hp[3] = {pos[0] + std::cos(ang) * off,
                         pos[1] + 0.35f + next_rand() * 1.15f,
                         pos[2] + std::sin(ang) * off};
    const float hv[3] = {std::cos(ang) * (0.18f + next_rand() * 0.30f),
                         0.14f + next_rand() * 0.24f,
                         std::sin(ang) * (0.18f + next_rand() * 0.30f)};
    PushHazeMoteLocked(hp, hv, 2.8f + next_rand() * 1.5f,
                       (0.52f + next_rand() * 0.42f) * std::min(rad_scale, 1.5f),
                       0.072f, true, perf ? 28u : kMaxHazeMotes);
  }
  lock.unlock();
  game::haptics::DispatchExplosionHaptic(pos);
}

void OnPlayerShellEject(const char *weapon_code, bool alt_fire) {
  const auto &enh = modsettings::Registry().GetEnhanced();
  if (!enh.shell_casings || !ShouldEjectShellCasing(weapon_code, alt_fire))
    return;

  char norm[8] = {};
  if (weapon_code) {
    size_t i = 0;
    while (weapon_code[i] && i < 7) {
      char c = weapon_code[i];
      if (c >= 'a' && c <= 'z')
        c = char(c - 'a' + 'A');
      norm[i++] = c;
    }
  }
  const WeaponClass cls = ClassifyWeaponCode(norm);
  const ShellCasingProfile prof = GetShellCasingProfile(cls);
  if (!prof.ejects)
    return;

  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_cam.valid)
    return;

  auto next_rand = [&]() -> float {
    g_fx_rng ^= g_fx_rng << 13;
    g_fx_rng ^= g_fx_rng >> 17;
    g_fx_rng ^= g_fx_rng << 5;
    return float(g_fx_rng & 0xFFFFu) / 65535.0f;
  };

  uint32_t world = R32(kWorld);
  uint32_t player = (world && MemMapped(world)) ? (world + kWorldPlayer) : 0;
  float ground_y = g_cam.eye[1] - 1.54f;
  if (player && MemMapped(player + 0xA8u, 4u)) {
    const float py = G<float>(player + 0xA4u);
    if (std::isfinite(py) && std::fabs(py - g_cam.eye[1]) < 3.0f) {
      ground_y = py + 0.022f;
    }
  }

  float ads_scale = 1.0f;
  if (player && MemMapped(player + 0x2A4u, 4u)) {
    const float z = G<float>(player + 0x2A4u);
    if (std::isfinite(z) && z > 1.2f)
      ads_scale = 0.62f;
  }

  const size_t cap = IsPerformancePresetActive() ? 28u : kMaxShellCasings;
  size_t chosen = cap;
  for (size_t i = 0; i < cap; i++) {
    size_t idx = (g_shell_cursor + i) % cap;
    if (!g_shells[idx].body.active) {
      chosen = idx;
      break;
    }
  }
  if (chosen == cap)
    chosen = g_shell_cursor % cap;
  g_shell_cursor = (chosen + 1) % cap;

  ShellCasingInstance &sc = g_shells[chosen];
  sc.cls = cls;
  sc.red_hull = prof.red_shotgun_hull;
  sc.length_m = prof.length_m;
  sc.radius_m = prof.radius_m;

  ShellCasingBody &b = sc.body;
  b.active = true;
  b.age = 0.0f;
  b.ttl = IsPerformancePresetActive() ? 2.2f : 3.4f;
  b.bounces = 0;
  b.ground_y = ground_y;
  b.spin_rad = next_rand() * 6.28318f;
  b.spin_vel = 18.0f + next_rand() * 16.0f;

  const float fwd_off = 0.26f;
  const float right_off = 0.145f * ads_scale;
  const float up_off = -0.068f * ads_scale;
  for (int k = 0; k < 3; k++) {
    b.pos[k] = g_cam.eye[k] + g_cam.forward[k] * fwd_off +
               g_cam.right[k] * right_off + g_cam.up[k] * up_off;
    sc.axis_a[k] = g_cam.right[k] * 0.72f + g_cam.forward[k] * 0.69f;
    sc.axis_b[k] = g_cam.up[k] * 0.85f - g_cam.forward[k] * 0.52f;
  }

  const float vr = prof.eject_speed_right * (0.85f + next_rand() * 0.34f);
  const float vu = prof.eject_speed_up * (0.88f + next_rand() * 0.32f);
  const float vb = prof.eject_speed_back * (0.65f + next_rand() * 0.65f);
  for (int k = 0; k < 3; k++) {
    b.vel[k] =
        g_cam.right[k] * vr + g_cam.up[k] * vu - g_cam.forward[k] * vb;
  }
  g_shells_ejected.fetch_add(1, std::memory_order_relaxed);
}

bool GetSceneCameraVectors(float eye[3], float right[3], float up[3],
                           float forward[3], float view[16], float proj[16]) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_cam.valid)
    return false;
  if (eye)
    std::memcpy(eye, g_cam.eye, sizeof(g_cam.eye));
  if (right)
    std::memcpy(right, g_cam.right, sizeof(g_cam.right));
  if (up)
    std::memcpy(up, g_cam.up, sizeof(g_cam.up));
  if (forward)
    std::memcpy(forward, g_cam.forward, sizeof(g_cam.forward));
  if (view)
    std::memcpy(view, g_cam.view, sizeof(g_cam.view));
  if (proj)
    std::memcpy(proj, g_cam.proj, sizeof(g_cam.proj));
  return true;
}

void SetFlameStream(const FlameStreamPoint *jet, uint32_t jet_count,
                    const FlameStreamPoint *pools, uint32_t pool_count,
                    float ground_y) {
  auto now = std::chrono::steady_clock::now();
  float dt = 0.016f;
  if (g_flame_stream_fresh) {
    dt = std::chrono::duration<float>(now - g_flame_stream_tp).count();
    dt = std::clamp(dt, 0.0f, 0.08f);
  }
  g_flame_stream_tp = now;
  g_flame_stream_fresh = true;
  g_flame_stream_ground_y = ground_y;

  std::lock_guard<std::mutex> lock(g_mutex);
  g_flame_jet_count = 0;
  g_flame_pool_count = 0;
  if (jet && jet_count) {
    g_flame_jet_count = std::min(jet_count, uint32_t(kMaxFlameStreamPoints));
    std::memcpy(g_flame_jet.data(), jet,
                size_t(g_flame_jet_count) * sizeof(FlameStreamPoint));
  }
  if (pools && pool_count) {
    g_flame_pool_count = std::min(pool_count, uint32_t(g_flame_pools.size()));
    std::memcpy(g_flame_pools.data(), pools,
                size_t(g_flame_pool_count) * sizeof(FlameStreamPoint));
  }
  if (g_flame_jet_count == 0 && g_flame_pool_count == 0)
    return;

  auto next_rand = [&]() -> float {
    g_fx_rng ^= g_fx_rng << 13;
    g_fx_rng ^= g_fx_rng >> 17;
    g_fx_rng ^= g_fx_rng << 5;
    return float(g_fx_rng & 0xFFFFu) / 65535.0f;
  };
  const bool perf = IsPerformancePresetActive();

  // 1. Burning gel drips out of the stream and arcs down onto the floor
  if (g_flame_jet_count > 1) {
    g_flame_ember_accum += dt * (perf ? 9.0f : 22.0f);
    int budget = perf ? 4 : 12;
    while (g_flame_ember_accum >= 1.0f && budget-- > 0) {
      g_flame_ember_accum -= 1.0f;
      const FlameStreamPoint &src =
          g_flame_jet[uint32_t(next_rand() * float(g_flame_jet_count)) %
                      g_flame_jet_count];
      if (!IsFiniteVec3(src.pos))
        continue;
      size_t chosen = kMaxExplosionEmbers;
      for (size_t k = 0; k < kMaxExplosionEmbers; k++) {
        size_t idx = (g_exp_ember_cursor + k) % kMaxExplosionEmbers;
        if (!g_explosion_embers[idx].active) {
          chosen = idx;
          break;
        }
      }
      if (chosen == kMaxExplosionEmbers)
        chosen = g_exp_ember_cursor % kMaxExplosionEmbers;
      g_exp_ember_cursor = (chosen + 1) % kMaxExplosionEmbers;
      ExplosionEmberNode &em = g_explosion_embers[chosen];
      em.active = true;
      em.pos[0] = src.pos[0];
      em.pos[1] = src.pos[1];
      em.pos[2] = src.pos[2];
      std::memcpy(em.prev_pos, em.pos, sizeof(em.prev_pos));
      em.vel[0] = src.vel[0] * 0.28f + (next_rand() - 0.5f) * 2.6f;
      em.vel[1] = src.vel[1] * 0.28f + 0.4f + next_rand() * 1.4f;
      em.vel[2] = src.vel[2] * 0.28f + (next_rand() - 0.5f) * 2.6f;
      em.ground_y = g_flame_stream_ground_y;
      em.age = 0.0f;
      em.ttl = 0.34f + next_rand() * 0.55f;
      em.radius = 0.010f + next_rand() * 0.012f;
      em.temp = 0.85f + next_rand() * 0.30f;
    }
  }

  // 2. Greasy soot boiling off the far end of the jet and off the floor pools
  const float soot_rate =
      (g_flame_jet_count > 1 ? 7.0f : 0.0f) + float(g_flame_pool_count) * 1.6f;
  g_flame_soot_accum += dt * soot_rate;
  int soot_budget = perf ? 3 : 8;
  while (g_flame_soot_accum >= 1.0f && soot_budget-- > 0) {
    g_flame_soot_accum -= 1.0f;
    float src[3] = {};
    if (g_flame_pool_count && (g_flame_jet_count <= 1 || next_rand() < 0.45f)) {
      const FlameStreamPoint &p =
          g_flame_pools[uint32_t(next_rand() * float(g_flame_pool_count)) %
                        g_flame_pool_count];
      src[0] = p.pos[0];
      src[1] = p.pos[1] + 0.10f;
      src[2] = p.pos[2];
    } else {
      // downstream quarter of the chain, where combustion is dirtiest
      const uint32_t lo = g_flame_jet_count / 2u;
      const uint32_t span = std::max(1u, g_flame_jet_count - lo);
      const FlameStreamPoint &p =
          g_flame_jet[lo + (uint32_t(next_rand() * float(span)) % span)];
      src[0] = p.pos[0];
      src[1] = p.pos[1] + 0.14f;
      src[2] = p.pos[2];
    }
    if (!IsFiniteVec3(src))
      continue;
    const float sv[3] = {(next_rand() - 0.5f) * 0.55f,
                         1.2f + next_rand() * 1.1f,
                         (next_rand() - 0.5f) * 0.55f};
    PushSmokeNodeLocked(src, src, false, sv, 1.5f + next_rand() * 0.9f,
                        0.20f + next_rand() * 0.14f, 0.44f, true, 0u,
                        perf ? 72u : kMaxSmokeNodes, false, 0.13f);
  }
}

void EmitPilotFlameGlow(const float pilot_world[3], float intensity) {
  if (!pilot_world || intensity <= 0.02f || !IsFiniteVec3(pilot_world))
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  size_t chosen = g_flash_cursor % kMaxMuzzleFlashes;
  g_flash_cursor = (chosen + 1) % kMaxMuzzleFlashes;
  MuzzleFlashNode &mf = g_muzzle_flashes[chosen];
  mf.active = true;
  std::memcpy(mf.pos, pilot_world, sizeof(mf.pos));
  mf.dir[0] = 0.0f;
  mf.dir[1] = 1.0f;
  mf.dir[2] = 0.0f;
  mf.age = 0.0f;
  mf.ttl = 0.018f;
  mf.radius = 0.011f * std::clamp(intensity, 0.4f, 1.15f);
  mf.rot = float(g_flash_cursor) * 1.37f;
  mf.tile = 4 + (int(g_flash_cursor) & 3);
}

void EmitBurnVictimFlames(const float world_pos[3], float ground_y,
                          float fire_intensity, bool emit_fire, bool emit_smoke,
                          bool is_dying) {
  if (!world_pos || !IsFiniteVec3(world_pos))
    return;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto next_rand = [&]() -> float {
    g_fx_rng ^= g_fx_rng << 13;
    g_fx_rng ^= g_fx_rng >> 17;
    g_fx_rng ^= g_fx_rng << 5;
    return float(g_fx_rng & 0xFFFFu) / 65535.0f;
  };

  const float base_y = is_dying ? (ground_y + 0.24f) : (world_pos[1] + 0.58f);
  const float h_span = is_dying ? 0.22f : 1.05f;

  if (emit_fire) {
    size_t chosen = kMaxExplosionFlames;
    for (size_t k = 0; k < kMaxExplosionFlames; k++) {
      size_t idx = (g_exp_flame_cursor + k) % kMaxExplosionFlames;
      if (!g_explosion_flames[idx].active) {
        chosen = idx;
        break;
      }
    }
    if (chosen == kMaxExplosionFlames)
      chosen = g_exp_flame_cursor % kMaxExplosionFlames;
    g_exp_flame_cursor = (chosen + 1) % kMaxExplosionFlames;
    ExplosionFlameNode &f = g_explosion_flames[chosen];
    f.active = true;
    f.is_shockwave = false;
    f.is_air_ring = false;
    f.is_flash = false;
    f.is_burn_fire = true;
    f.pos[0] = world_pos[0] + (next_rand() - 0.5f) * 0.36f;
    f.pos[1] = base_y + next_rand() * h_span;
    f.pos[2] = world_pos[2] + (next_rand() - 0.5f) * 0.36f;
    f.vel[0] = (next_rand() - 0.5f) * 0.55f;
    f.vel[1] = 1.4f + next_rand() * 1.4f;
    f.vel[2] = (next_rand() - 0.5f) * 0.55f;
    f.age = 0.0f;
    f.ttl = 0.30f + next_rand() * 0.22f;
    f.start_rad = 0.14f + next_rand() * 0.10f;
    f.end_rad = (0.36f + next_rand() * 0.22f) *
                std::clamp(fire_intensity, 0.45f, 1.15f);
    f.rot = next_rand() * 6.28318f;
    f.rot_vel = (next_rand() - 0.5f) * 5.0f;
    f.tile = 4 + (int(chosen) & 3);
    f.heat = std::clamp(0.85f * fire_intensity + 0.2f, 0.4f, 1.2f);

    // Occasional floating burning cinder from clothing
    if ((g_exp_ember_cursor % 3u) == 0u) {
      size_t eidx = g_exp_ember_cursor % kMaxExplosionEmbers;
      g_exp_ember_cursor = (eidx + 1) % kMaxExplosionEmbers;
      ExplosionEmberNode &em = g_explosion_embers[eidx];
      em.active = true;
      std::memcpy(em.pos, f.pos, sizeof(em.pos));
      std::memcpy(em.prev_pos, em.pos, sizeof(em.prev_pos));
      em.vel[0] = (next_rand() - 0.5f) * 1.8f;
      em.vel[1] = 2.4f + next_rand() * 2.2f;
      em.vel[2] = (next_rand() - 0.5f) * 1.8f;
      em.ground_y = ground_y;
      em.age = 0.0f;
      em.ttl = 0.45f + next_rand() * 0.45f;
      em.radius = 0.011f;
      em.temp = 0.85f;
    }
  }

  if (emit_smoke && (g_smoke_cursor % 2u) == 0u) {
    const float sp[3] = {world_pos[0] + (next_rand() - 0.5f) * 0.30f,
                         base_y + h_span * 0.72f,
                         world_pos[2] + (next_rand() - 0.5f) * 0.30f};
    const float sv[3] = {(next_rand() - 0.5f) * 0.35f,
                         1.35f + next_rand() * 0.85f,
                         (next_rand() - 0.5f) * 0.35f};
    PushSmokeNodeLocked(
        sp, sp, false, sv, 1.4f + next_rand() * 0.7f,
        0.18f + next_rand() * 0.10f, is_dying ? 0.44f : 0.38f, true, 0u,
        IsPerformancePresetActive() ? 64u : kMaxSmokeNodes, false, 0.12f);
  }
}

void OnHitscanShot(const float start[3], const float end[3],
                   uint32_t caller_ret_addr, bool is_player) {
  // 0x001E286D is sub_001E26E0 finishing a physical player bullet slot (already
  // tracked above!)
  if (caller_ret_addr == 0x1E286Du)
    return;
  if (!start || !end || !IsFiniteVec3(start) || !IsFiniteVec3(end))
    return;

  float dx = end[0] - start[0];
  float dy = end[1] - start[1];
  float dz = end[2] - start[2];
  float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (dist < 0.15f || dist > 600.0f)
    return;

  const auto &enh = modsettings::Registry().GetEnhanced();
  const bool tracers_active = enh.tracers_enabled && enh.tracer_frequency != 0;
  bool smoke_enabled = false;
  float smoke_density = 0.0f, smoke_ttl = 0.0f;
  size_t smoke_cap = 0;
  SampleSmokeConfig(enh, IsPerformancePresetActive(), smoke_enabled,
                    smoke_density, smoke_ttl, smoke_cap);
  if (!tracers_active && !smoke_enabled)
    return;

  float inv = 1.0f / dist;
  float dir[3] = {dx * inv, dy * inv, dz * inv};

  std::lock_guard<std::mutex> lock(g_mutex);
  uint64_t seq = g_ai_shot_seq++;
  uint32_t group_id = g_next_group_id++;
  g_shots_observed.fetch_add(1, std::memory_order_relaxed);

  if (smoke_enabled) {
    if (is_player) {
      g_muzzle_emitter.heat = std::min(1.0f, g_muzzle_emitter.heat + 0.20f);
      g_muzzle_emitter.suppressed = false;
    }
    // Subtle wispy vapor along hitscan/AI path (avoid blocky mid-air polygons)
    if (dist > 3.5f) {
      float frac = 0.35f;
      float p[3] = {start[0] + dx * frac, start[1] + dy * frac,
                    start[2] + dz * frac};
      float vel[3] = {0.02f, 0.14f, 0.02f};
      PushSmokeNodeLocked(p, p, false, vel, smoke_ttl * 0.55f,
                          0.018f * std::min(smoke_density, 1.3f),
                          std::clamp(0.038f * smoke_density, 0.014f, 0.085f),
                          false, group_id, smoke_cap, false);
    }
  }

  const uint32_t special_mode =
      is_player ? enh.special_ammo_mode : 0u;
  if (!tracers_active ||
      (!(special_mode != 0u && enh.tracer_frequency != 0) &&
       !ShouldEmitTracer(enh.tracer_frequency, seq, WeaponClass::Rifle, false,
                         is_player)))
    return;

  const size_t max_t = IsPerformancePresetActive() ? 32u : kMaxTracers;
  size_t chosen = max_t;
  for (size_t i = 0; i < max_t; i++) {
    size_t idx = (g_tracer_cursor + i) % max_t;
    if (!g_tracers[idx].active) {
      chosen = idx;
      break;
    }
  }
  if (chosen == max_t)
    chosen = g_tracer_cursor % max_t;
  g_tracer_cursor = (chosen + 1) % max_t;

  TracerInstance &t = g_tracers[chosen];
  t.active = true;
  t.is_physical = false; // Strictly marked synchronized hitscan visual tracer
  t.is_player = is_player;
  t.hit_locked = false;
  t.suppressed = false;
  t.cls = WeaponClass::Rifle;
  t.guest_slot = 0;
  std::memcpy(t.visual_origin, start, sizeof(t.visual_origin));
  std::memcpy(t.physics_origin, start, sizeof(t.physics_origin));
  std::memcpy(t.head_pos, start, sizeof(t.head_pos));
  std::memcpy(t.target_end, end, sizeof(t.target_end));
  std::memcpy(t.dir, dir, sizeof(t.dir));
  // Synchronized high-velocity flight (fast enough to feel crisp without
  // altering real hitscan hit timing)
  t.speed = std::max(260.0f, dist / 0.095f);
  t.age = 0.0f;
  t.hit_age = -1.0f;
  t.afterglow_ttl = 0.11f;
  GetWeaponVisualProfile(WeaponClass::Rifle, false, false, t.core_thickness,
                         t.trail_scale, t.brightness);
  t.brightness *= 0.90f;
  t.color_override = enh.tracer_color;
  if (special_mode != 0u) {
    const auto prof = game::overdrive_rules::GetSpecialAmmoProfile(
        game::overdrive_rules::SanitizeSpecialAmmo(special_mode));
    if (prof.tracer_color_override != 0u)
      t.color_override = prof.tracer_color_override;
    if (special_mode == 3u) {
      t.core_thickness *= 1.25f;
      t.trail_scale *= 1.18f;
      t.brightness *= 1.20f;
    }
  }
  t.group_id = group_id;
  t.last_smoke_dist = 0.0f;
  g_tracers_emitted.fetch_add(1, std::memory_order_relaxed);
  g_hitscan_synced.fetch_add(1, std::memory_order_relaxed);
}

void RenderWorldScene() {
  if (!time_stop::IsActiveGameplay())
    return;

  const auto &enh = modsettings::Registry().GetEnhanced();
  const bool tracers_active = enh.tracers_enabled && enh.tracer_frequency != 0;
  bool smoke_enabled = false;
  float smoke_density = 0.0f, smoke_ttl = 0.0f;
  size_t smoke_cap = 0;
  const bool perf_mode = IsPerformancePresetActive();
  SampleSmokeConfig(enh, perf_mode, smoke_enabled, smoke_density, smoke_ttl,
                    smoke_cap);

  // Napalm stream published by the weapon sim; fades out once the gun is stowed
  float stream_fade = 0.0f;
  if (g_flame_stream_fresh) {
    const float stream_age =
        std::chrono::duration<float>(std::chrono::steady_clock::now() -
                                     g_flame_stream_tp)
            .count();
    stream_fade = std::clamp(1.0f - stream_age / 0.22f, 0.0f, 1.0f);
  }
  {
    bool any_fx = tracers_active || smoke_enabled;
    if (!any_fx) {
      std::lock_guard<std::mutex> lock(g_mutex);
      for (const auto &f : g_explosion_flames)
        if (f.active) {
          any_fx = true;
          break;
        }
      if (!any_fx)
        for (const auto &em : g_explosion_embers)
          if (em.active) {
            any_fx = true;
            break;
          }
      if (!any_fx)
        for (const auto &bn : g_blood)
          if (bn.active) {
            any_fx = true;
            break;
          }
      if (!any_fx)
        for (const auto &pl : g_blood_pools)
          if (pl.active) {
            any_fx = true;
            break;
          }
      if (!any_fx)
        for (const auto &mf : g_muzzle_flashes)
          if (mf.active) {
            any_fx = true;
            break;
          }
      if (!any_fx)
        for (const auto &sc : g_shells)
          if (sc.body.active) {
            any_fx = true;
            break;
          }
      if (!any_fx)
        for (const auto &hm : g_haze_motes)
          if (hm.active) {
            any_fx = true;
            break;
          }
      if (stream_fade <= 0.002f) {
        // stowed weapon: drop the stale stream so the pass can idle out
        g_flame_jet_count = 0;
        g_flame_pool_count = 0;
      }
      if (!any_fx && (g_flame_jet_count || g_flame_pool_count))
        any_fx = true;
    }
    if (!any_fx)
      return;
  }

  auto now = std::chrono::steady_clock::now();
  float real_dt = 0.016f;
  if (g_has_frame_tp) {
    real_dt = std::chrono::duration<float>(now - g_last_frame_tp).count();
    real_dt = std::clamp(real_dt, 0.0f, 0.05f);
  } else {
    g_has_frame_tp = true;
  }
  g_last_frame_tp = now;

  // Respect Time Stop world freeze cleanly: frozen world bullets & smoke freeze
  // their decay
  const bool world_frozen = time_stop::IsFrozen();
  const float world_dt = world_frozen ? 0.0f : real_dt;

  static std::vector<d3d::WorldVertex> smoke_verts;
  static std::vector<d3d::WorldVertex> tracer_verts;
  static std::vector<d3d::TexturedWorldVertex> smoke_tex_verts;
  static std::vector<d3d::TexturedWorldVertex> fire_tex_verts;
  smoke_verts.clear();
  tracer_verts.clear();
  smoke_tex_verts.clear();
  fire_tex_verts.clear();

  float view_copy[16];
  float proj_copy[16];
  uint32_t active_smoke_cnt = 0;
  uint32_t active_tracer_cnt = 0;
  uint32_t culled_this_frame = 0;
  struct PendingClink {
    float pan = 0.35f;
    bool shotgun = false;
  };
  PendingClink pending_clinks[8]{};
  size_t pending_clink_count = 0;

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_cam.valid)
      return;
    std::memcpy(view_copy, g_cam.view, sizeof(view_copy));
    std::memcpy(proj_copy, g_cam.proj, sizeof(proj_copy));

    // Sample active explosive rockets/grenades for launcher smoke exhaust
    if (smoke_enabled && !world_frozen) {
      uint32_t exp_mgr = R32(kExplosiveManager);
      if (exp_mgr && MemMapped(exp_mgr)) {
        for (int i = 0; i < kPoolBCount; i++) {
          uint8_t active = G<uint8_t>(exp_mgr + kPoolBBitmap + uint32_t(i));
          ExplosiveTracker &trk = g_explosive_trackers[kPoolACount + i];
          if (!active) {
            trk.active = false;
            continue;
          }
          uint32_t slot_ea =
              exp_mgr + 0x2400u + uint32_t(i) * kExplosiveSlotSize;
          float pos[3] = {G<float>(slot_ea + 0xA0), G<float>(slot_ea + 0xA4),
                          G<float>(slot_ea + 0xA8)};
          if (!IsFiniteVec3(pos))
            continue;
          if (!trk.active) {
            trk.active = true;
            std::memcpy(trk.last_pos, pos, sizeof(pos));
            trk.smoke_accum = 0.0f;
          } else {
            float moved = std::sqrt(VecDistSq(trk.last_pos, pos));
            if (moved > 0.35f && moved < 80.0f) {
              float vel[3] = {0.10f, 0.55f, 0.10f};
              PushSmokeNodeLocked(
                  pos, trk.last_pos, true, vel, smoke_ttl * 1.45f,
                  0.16f * std::min(smoke_density, 1.5f),
                  std::clamp(0.24f * smoke_density, 0.06f, 0.40f), true, 0u,
                  smoke_cap);
              std::memcpy(trk.last_pos, pos, sizeof(pos));
            }
          }
        }
      }
    }

    // 0. Continuous delicate muzzle smoke stream while barrel is firing /
    // cooling down
    if (smoke_enabled && !world_frozen && g_muzzle_emitter.heat > 0.03f) {
      g_muzzle_emitter.heat =
          std::max(0.0f, g_muzzle_emitter.heat - world_dt * 0.55f);
      g_muzzle_emitter.emit_accum += world_dt;
      if (g_muzzle_emitter.emit_accum >= 0.044f) {
        g_muzzle_emitter.emit_accum = 0.0f;
        uint32_t world = R32(kWorld);
        uint32_t player =
            (world && MemMapped(world)) ? (world + kWorldPlayer) : 0;
        float muzzle_pos[3] = {};
        bool have_barrel = false;
        if (player && LookupRecentMuzzleLocked(player, muzzle_pos)) {
          float d[3] = {muzzle_pos[0] - g_cam.eye[0],
                        muzzle_pos[1] - g_cam.eye[1],
                        muzzle_pos[2] - g_cam.eye[2]};
          float fwd_d = d[0] * g_cam.forward[0] + d[1] * g_cam.forward[1] +
                        d[2] * g_cam.forward[2];
          if (fwd_d > 0.12f && fwd_d < 1.35f) {
            have_barrel = true;
          }
        }
        if (!have_barrel) {
          float zoom = (player && MemMapped(player + 0x2A4u, 4u))
                           ? G<float>(player + 0x2A4u)
                           : 1.0f;
          float ads_scale = (zoom > 1.2f) ? 0.45f : 1.0f;
          const float f_off = 0.34f;
          const float r_off = 0.132f * ads_scale;
          const float u_off = -0.090f * ads_scale;
          muzzle_pos[0] = g_cam.eye[0] + g_cam.forward[0] * f_off +
                          g_cam.right[0] * r_off + g_cam.up[0] * u_off;
          muzzle_pos[1] = g_cam.eye[1] + g_cam.forward[1] * f_off +
                          g_cam.right[1] * r_off + g_cam.up[1] * u_off;
          muzzle_pos[2] = g_cam.eye[2] + g_cam.forward[2] * f_off +
                          g_cam.right[2] * r_off + g_cam.up[2] * u_off;
        }

        float phase = float(g_muzzle_emitter.wisp_seq++) * 1.13f;
        float curl_x = std::sin(phase) * 0.035f;
        float curl_z = std::cos(phase * 0.8f) * 0.035f;
        float wisp_vel[3] = {
            g_cam.forward[0] * 0.14f + g_cam.right[0] * 0.025f + curl_x,
            0.24f + 0.14f * g_muzzle_emitter.heat,
            g_cam.forward[2] * 0.14f + g_cam.right[2] * 0.025f + curl_z};
        bool link_prev =
            g_muzzle_emitter.has_prev_wisp &&
            VecDistSq(muzzle_pos, g_muzzle_emitter.prev_wisp_pos) < 0.035f;
        float wisp_op = std::clamp(
            0.062f * (0.35f + 0.65f * g_muzzle_emitter.heat) * smoke_density *
                (g_muzzle_emitter.suppressed ? 0.65f : 1.0f),
            0.015f, 0.11f);
        float wisp_rad = 0.0125f * std::clamp(smoke_density, 0.55f, 1.25f);
        float wisp_ttl = std::clamp(
            smoke_ttl * (0.60f + 0.35f * g_muzzle_emitter.heat), 0.32f, 1.30f);
        PushSmokeNodeLocked(muzzle_pos, g_muzzle_emitter.prev_wisp_pos,
                            link_prev, wisp_vel, wisp_ttl, wisp_rad, wisp_op,
                            false, 0u, smoke_cap, true);
        std::memcpy(g_muzzle_emitter.prev_wisp_pos, muzzle_pos,
                    sizeof(muzzle_pos));
        g_muzzle_emitter.has_prev_wisp = true;
      }
    } else if (!world_frozen && g_muzzle_emitter.heat <= 0.03f) {
      g_muzzle_emitter.has_prev_wisp = false;
    }

    // 1. Update & Emit 3D Alpha-Blended Smoke Geometry (depth-tested by Vulkan
    // backend)
    if (smoke_enabled) {
      for (size_t i = 0; i < smoke_cap; i++) {
        SmokeNode &n = g_smoke[i];
        if (!n.active)
          continue;

        n.age += world_dt;
        if (n.age >= n.ttl) {
          n.active = false;
          continue;
        }
        active_smoke_cnt++;

        if (world_dt > 0.0f) {
          const float curl = n.is_muzzle_wisp ? 0.055f : 0.015f;
          const float cx = std::sin(n.age * 4.8f + n.seed) * curl;
          const float cz = std::cos(n.age * 4.1f + n.seed * 1.4f) * curl;
          n.pos[0] += (n.vel[0] + cx) * world_dt;
          n.pos[1] += n.vel[1] * world_dt;
          n.pos[2] += (n.vel[2] + cz) * world_dt;
          if (n.is_muzzle_wisp) {
            n.prev_pos[0] += (n.vel[0] * 0.65f + cx * 0.5f) * world_dt;
            n.prev_pos[1] += n.vel[1] * 0.72f * world_dt;
            n.prev_pos[2] += (n.vel[2] * 0.65f + cz * 0.5f) * world_dt;
          }
          float drag_rate = n.is_muzzle_wisp ? 3.8f : 1.8f;
          float drag = std::max(0.0f, 1.0f - world_dt * drag_rate);
          n.vel[0] *= drag;
          n.vel[2] *= drag;
          n.vel[1] += world_dt * (n.is_muzzle_wisp ? 0.14f : 0.05f);
        }

        float life = n.age / n.ttl;
        float fade_in = std::min(1.0f, n.age * 16.0f);
        float fade =
            fade_in * std::pow(std::clamp(1.0f - life, 0.0f, 1.0f), 1.75f);
        float alpha = n.opacity * fade;
        if (alpha < 0.006f)
          continue;

        float dz = (n.pos[0] - g_cam.eye[0]) * g_cam.forward[0] +
                   (n.pos[1] - g_cam.eye[1]) * g_cam.forward[1] +
                   (n.pos[2] - g_cam.eye[2]) * g_cam.forward[2];
        if (dz <= 0.10f) {
          culled_this_frame++;
          continue;
        }

        float expand =
            n.from_launcher ? 3.4f : (n.is_muzzle_wisp ? 2.15f : 1.75f);
        float world_radius = n.base_radius * (1.0f + life * expand);
        float warm = n.is_muzzle_wisp
                         ? std::max(0.0f, 1.0f - life * 4.5f) * 0.08f
                         : 0.0f;
        Color3 smoke_rgb{std::min(1.0f, n.tint * 0.88f + warm),
                         std::min(1.0f, n.tint * 0.89f + warm * 0.5f),
                         std::min(1.0f, n.tint * 0.94f)};

        if (n.has_prev && (n.from_launcher || n.is_muzzle_wisp)) {
          float rib_w = n.is_muzzle_wisp ? (world_radius * 0.34f)
                                         : (world_radius * 0.48f);
          Emit3DRibbon(smoke_verts, n.prev_pos, n.pos, rib_w * 0.70f, rib_w,
                       PackColor3(smoke_rgb, alpha * 0.22f),
                       PackColor3(smoke_rgb, alpha * 0.42f), g_cam);
        }

        EmitTexturedBillboard(smoke_tex_verts, n.pos, world_radius * 1.45f,
                              n.seed + n.age * 0.45f,
                              int(std::fabs(n.seed) * 7.0f + n.age * 3.0f) & 3,
                              PackColor3(smoke_rgb, alpha * 0.78f), g_cam);
      }
    }

    // 2. Update & Emit 3D Additive-Blended Tracer Geometry (depth-tested by
    // Vulkan backend)
    const float user_len_mul =
        std::clamp(float(enh.tracer_trail_length) * 0.01f, 0.25f, 2.50f);
    const float user_bright_mul =
        std::clamp(float(enh.tracer_brightness) * 0.01f, 0.20f, 2.00f);

    if (tracers_active) {
      const size_t max_t = perf_mode ? 32u : kMaxTracers;
      for (size_t i = 0; i < max_t; i++) {
        TracerInstance &t = g_tracers[i];
        if (!t.active)
          continue;

        float dt_step = (world_frozen && !t.is_player) ? 0.0f : real_dt;
        t.age += dt_step;

        if (!t.is_physical) {
          float dist_total =
              std::sqrt(VecDistSq(t.visual_origin, t.target_end));
          float traveled = t.speed * t.age;
          if (traveled >= dist_total) {
            std::memcpy(t.head_pos, t.target_end, sizeof(t.head_pos));
            t.hit_locked = true;
            if (t.hit_age < 0.0f)
              t.hit_age = 0.0f;
          } else {
            float frac = (dist_total > 1e-4f) ? (traveled / dist_total) : 1.0f;
            t.head_pos[0] = t.visual_origin[0] +
                            (t.target_end[0] - t.visual_origin[0]) * frac;
            t.head_pos[1] = t.visual_origin[1] +
                            (t.target_end[1] - t.visual_origin[1]) * frac;
            t.head_pos[2] = t.visual_origin[2] +
                            (t.target_end[2] - t.visual_origin[2]) * frac;
          }
        } else if (t.guest_slot && !t.hit_locked) {
          if (MemMapped(t.guest_slot) && G<uint8_t>(t.guest_slot + 0x88) != 0) {
            float cur[3] = {G<float>(t.guest_slot + 0x50),
                            G<float>(t.guest_slot + 0x54),
                            G<float>(t.guest_slot + 0x58)};
            if (IsFiniteVec3(cur))
              std::memcpy(t.head_pos, cur, sizeof(t.head_pos));
          } else {
            t.hit_locked = true;
            if (t.hit_age < 0.0f)
              t.hit_age = 0.0f;
          }
        }

        if (t.hit_locked && !world_frozen) {
          if (t.hit_age < 0.0f)
            t.hit_age = 0.0f;
          t.hit_age += real_dt;
          if (t.hit_age >= t.afterglow_ttl) {
            t.active = false;
            continue;
          }
        } else if (t.age > 2.2f && !world_frozen) {
          t.active = false;
          continue;
        }
        active_tracer_cnt++;

        float dist_from_origin =
            std::sqrt(VecDistSq(t.physics_origin, t.head_pos));
        if (dist_from_origin < 0.14f)
          continue;

        float after_fade =
            (t.hit_locked && t.hit_age >= 0.0f)
                ? std::clamp(1.0f - (t.hit_age / t.afterglow_ttl), 0.0f, 1.0f)
                : 1.0f;
        float desired_len =
            std::clamp(8.5f * t.trail_scale * user_len_mul, 1.5f, 34.0f);
        float shrink_tail =
            (t.hit_locked && t.hit_age >= 0.0f)
                ? std::max(desired_len * (1.0f - 0.55f * (1.0f - after_fade)),
                           0.8f)
                : desired_len;
        float tail_dist = std::max(0.0f, dist_from_origin - shrink_tail);

        // Smooth barrel convergence: start at visual barrel muzzle, blend onto
        // physics trajectory by 3.5m
        float blend_o = std::clamp(tail_dist / 3.5f, 0.0f, 1.0f);
        float ray_o[3] = {t.visual_origin[0] * (1.0f - blend_o) +
                              t.physics_origin[0] * blend_o,
                          t.visual_origin[1] * (1.0f - blend_o) +
                              t.physics_origin[1] * blend_o,
                          t.visual_origin[2] * (1.0f - blend_o) +
                              t.physics_origin[2] * blend_o};
        float tail_pos[3] = {ray_o[0] + t.dir[0] * tail_dist,
                             ray_o[1] + t.dir[1] * tail_dist,
                             ray_o[2] + t.dir[2] * tail_dist};
        // Keep tail strictly behind head along firing vector for very short
        // initial hops
        float along = (t.head_pos[0] - tail_pos[0]) * t.dir[0] +
                      (t.head_pos[1] - tail_pos[1]) * t.dir[1] +
                      (t.head_pos[2] - tail_pos[2]) * t.dir[2];
        if (along <= 0.06f) {
          tail_pos[0] = t.head_pos[0] -
                        t.dir[0] * std::min(dist_from_origin * 0.65f, 0.35f);
          tail_pos[1] = t.head_pos[1] -
                        t.dir[1] * std::min(dist_from_origin * 0.65f, 0.35f);
          tail_pos[2] = t.head_pos[2] -
                        t.dir[2] * std::min(dist_from_origin * 0.65f, 0.35f);
        }

        float dz_head = (t.head_pos[0] - g_cam.eye[0]) * g_cam.forward[0] +
                        (t.head_pos[1] - g_cam.eye[1]) * g_cam.forward[1] +
                        (t.head_pos[2] - g_cam.eye[2]) * g_cam.forward[2];
        float dz_tail = (tail_pos[0] - g_cam.eye[0]) * g_cam.forward[0] +
                        (tail_pos[1] - g_cam.eye[1]) * g_cam.forward[1] +
                        (tail_pos[2] - g_cam.eye[2]) * g_cam.forward[2];
        if (dz_head <= 0.08f && dz_tail <= 0.08f) {
          culled_this_frame++;
          continue;
        }

        float dist_cam =
            std::max(0.5f, std::sqrt(VecDistSq(g_cam.eye, t.head_pos)));
        // Perspective-calibrated world thickness: sharp near camera, crisp
        // across 150m
        float px_world = dist_cam * g_cam.tan_half_fov_y * 0.0024f;
        float hw_core =
            std::clamp(std::max(0.0055f, px_world * 0.90f) * t.core_thickness *
                           (0.75f + 0.25f * user_bright_mul),
                       0.004f, dist_cam * 0.0085f);
        float hw_halo = hw_core * 2.75f;

        float dist_att =
            std::clamp(14.0f / std::max(dist_cam, 2.0f), 0.45f, 1.30f);
        float alpha = std::clamp(after_fade * t.brightness * user_bright_mul *
                                     (0.72f + 0.28f * dist_att),
                                 0.14f, 1.0f);

        Color3 col_core, col_body, col_halo;
        ResolveTracerColors(t.color_override, t.cls, t.suppressed, col_core,
                            col_body, col_halo);

        // Layer 1: Soft warm outer atmospheric halo
        Emit3DRibbon(tracer_verts, tail_pos, t.head_pos, hw_halo * 0.15f,
                     hw_halo, PackColor3(col_halo, alpha * 0.04f),
                     PackColor3(col_halo, alpha * 0.28f), g_cam);
        // Layer 2: Saturated ballistic tracer body
        Emit3DRibbon(tracer_verts, tail_pos, t.head_pos, hw_core * 0.20f,
                     hw_core * 1.45f, PackColor3(col_body, alpha * 0.10f),
                     PackColor3(col_body, alpha * 0.74f), g_cam);
        // Layer 3: Sharp white-gold incandescent pyrotechnic core
        Emit3DRibbon(tracer_verts, tail_pos, t.head_pos, hw_core * 0.12f,
                     hw_core * 0.72f, PackColor3(col_core, alpha * 0.18f),
                     PackColor3(col_core, alpha * 0.96f), g_cam);

        if (!t.hit_locked || t.hit_age < 0.045f) {
          Emit3DSoftDisc(tracer_verts, t.head_pos, hw_core * 1.65f, col_body,
                         alpha * 0.55f, g_cam);
          Emit3DSoftDisc(tracer_verts, t.head_pos, hw_core * 0.85f, col_core,
                         alpha * 0.95f, g_cam);
        }
      }
    }
    // 2b. Explosions: Crimson-Orange Fireballs, Radial Shockwave & Bouncing
    // Glowing Embers
    {
      for (size_t i = 0; i < kMaxExplosionFlames; i++) {
        ExplosionFlameNode &f = g_explosion_flames[i];
        if (!f.active)
          continue;
        if (world_dt > 0.0f) {
          f.age += world_dt;
          if (f.age >= f.ttl) {
            f.active = false;
            continue;
          }
          const float damp = std::exp(-2.3f * world_dt);
          f.vel[0] *= damp;
          f.vel[2] *= damp;
          f.vel[1] =
              f.vel[1] * damp + (f.is_shockwave ? 0.0f : 2.8f * world_dt);
          f.pos[0] += f.vel[0] * world_dt;
          f.pos[1] += f.vel[1] * world_dt;
          f.pos[2] += f.vel[2] * world_dt;
          f.rot += f.rot_vel * world_dt;
        }
        const float u = std::clamp(f.age / std::max(f.ttl, 0.01f), 0.0f, 1.0f);
        const float grow = 1.0f - std::pow(1.0f - u, 2.5f);
        const float rad = f.start_rad + (f.end_rad - f.start_rad) * grow;
        const float dz = (f.pos[0] - g_cam.eye[0]) * g_cam.forward[0] +
                         (f.pos[1] - g_cam.eye[1]) * g_cam.forward[1] +
                         (f.pos[2] - g_cam.eye[2]) * g_cam.forward[2];
        if (dz <= -rad)
          continue;

        if (f.is_shockwave) {
          const float wave_fade =
              std::clamp((1.0f - u) * (1.0f - u), 0.0f, 1.0f);
          EmitTexturedGroundTile(
              smoke_tex_verts, f.pos, rad, f.rot, f.tile,
              d3d::PackColorBgra(0.46f, 0.36f, 0.28f, wave_fade * 0.62f));
          if (u < 0.42f) {
            const float flash_a =
                std::clamp((1.0f - u / 0.42f) * 0.52f * f.heat, 0.0f, 1.0f);
            EmitTexturedGroundTile(
                fire_tex_verts, f.pos, rad * 0.74f, -f.rot, f.tile,
                d3d::PackColorBgra(1.0f, 0.30f, 0.05f, flash_a));
          }
        } else {
          const float cool = std::clamp(1.0f - u, 0.0f, 1.0f);
          if (f.is_burn_fire) {
            // Burning target / lingering torch fire: bright natural orange-gold flame
            // instead of explosion's blood-red crimson char volume.
            const float body_r = std::clamp(0.92f + 0.08f * cool, 0.0f, 1.0f);
            const float body_g = std::clamp(0.42f + 0.40f * cool, 0.0f, 1.0f);
            const float body_b = std::clamp(0.06f + 0.20f * cool * cool, 0.0f, 1.0f);
            const float body_a =
                std::clamp(std::pow(cool, 1.20f) * 0.52f, 0.0f, 0.68f);
            EmitTexturedBillboard(
                smoke_tex_verts, f.pos, rad, f.rot, f.tile,
                d3d::PackColorBgra(body_r, body_g, body_b, body_a), g_cam);

            const float glow_a =
                std::clamp(std::pow(cool, 1.35f) * 0.85f * f.heat, 0.0f, 1.0f);
            if (glow_a > 0.02f) {
              EmitTexturedBillboard(
                  fire_tex_verts, f.pos, rad * 0.92f, -f.rot * 0.7f, f.tile,
                  d3d::PackColorBgra(1.0f, 0.56f + 0.34f * cool,
                                     0.12f + 0.24f * cool * cool, glow_a),
                  g_cam);
            }
          } else {
            // Explosion fireball + charred soot cloud
            const float body_r =
                (u < 0.55f) ? (0.95f - 0.35f * (u / 0.55f))
                            : std::max(0.18f, 0.60f - 0.92f * (u - 0.55f));
            const float body_g = (u < 0.25f) ? (0.34f - 0.22f * (u / 0.25f))
                                             : std::max(0.04f, 0.12f * cool);
            const float body_b = 0.025f;
            const float body_a =
                std::clamp(std::pow(cool, 1.15f) * 0.84f, 0.0f, 0.90f);
            EmitTexturedBillboard(
                smoke_tex_verts, f.pos, rad, f.rot, f.tile,
                d3d::PackColorBgra(body_r, body_g, body_b, body_a), g_cam);

            const float glow_a =
                std::clamp(std::pow(cool, 1.55f) * 0.74f * f.heat, 0.0f, 1.0f);
            if (glow_a > 0.02f) {
              EmitTexturedBillboard(
                  fire_tex_verts, f.pos, rad * 0.86f, -f.rot * 0.7f, f.tile,
                  d3d::PackColorBgra(1.0f, 0.28f + 0.44f * cool * cool, 0.04f,
                                     glow_a),
                  g_cam);
            }
          }
        }
      }

      for (size_t i = 0; i < kMaxExplosionEmbers; i++) {
        ExplosionEmberNode &em = g_explosion_embers[i];
        if (!em.active)
          continue;
        if (world_dt > 0.0f) {
          em.age += world_dt;
          if (em.age >= em.ttl) {
            em.active = false;
            continue;
          }
          std::memcpy(em.prev_pos, em.pos, sizeof(em.prev_pos));
          const float drag = std::exp(-0.85f * world_dt);
          em.vel[0] *= drag;
          em.vel[2] *= drag;
          em.vel[1] = em.vel[1] * drag - 13.5f * world_dt;
          em.pos[0] += em.vel[0] * world_dt;
          em.pos[1] += em.vel[1] * world_dt;
          em.pos[2] += em.vel[2] * world_dt;
          if (em.pos[1] < em.ground_y) {
            em.pos[1] = em.ground_y;
            em.vel[1] = std::fabs(em.vel[1]) * 0.44f;
            em.vel[0] *= 0.72f;
            em.vel[2] *= 0.72f;
          }
        }
        const float dz = (em.pos[0] - g_cam.eye[0]) * g_cam.forward[0] +
                         (em.pos[1] - g_cam.eye[1]) * g_cam.forward[1] +
                         (em.pos[2] - g_cam.eye[2]) * g_cam.forward[2];
        if (dz <= 0.08f)
          continue;

        const float u =
            std::clamp(em.age / std::max(em.ttl, 0.01f), 0.0f, 1.0f);
        const float cool = std::clamp((1.0f - u) * em.temp, 0.0f, 1.0f);
        const float alpha =
            std::clamp(std::pow(1.0f - u, 0.75f) * 0.96f, 0.0f, 1.0f);
        const float streak_t = 0.032f;
        const float tail[3] = {
            em.pos[0] - std::clamp(em.vel[0] * streak_t, -0.45f, 0.45f),
            em.pos[1] - std::clamp(em.vel[1] * streak_t, -0.45f, 0.45f),
            em.pos[2] - std::clamp(em.vel[2] * streak_t, -0.45f, 0.45f)};
        const Color3 c_core{
            1.0f, std::clamp(0.46f + 0.46f * cool * cool, 0.0f, 1.0f),
            std::clamp(0.08f + 0.36f * cool * cool * cool, 0.0f, 1.0f)};
        const Color3 c_halo{1.0f, std::clamp(0.32f + 0.34f * cool, 0.0f, 1.0f),
                            0.04f};
        Emit3DRibbon(tracer_verts, tail, em.pos, em.radius * 0.22f, em.radius,
                     PackColor3(c_halo, alpha * 0.24f),
                     PackColor3(c_core, alpha), g_cam);
        Emit3DSoftDisc(tracer_verts, em.pos, em.radius * 1.45f, c_core,
                       alpha * 0.88f, g_cam);
        if (u < 0.72f) {
          EmitTexturedBillboard(
              fire_tex_verts, em.pos, em.radius * 3.8f, float(i) * 0.73f,
              12 + (int(i) & 1),
              d3d::PackColorBgra(1.0f,
                                 std::clamp(0.14f + 0.38f * cool, 0.0f, 1.0f),
                                 0.02f, alpha * 0.65f),
              g_cam);
        }
      }
    }
    // 2c. Flamethrower napalm jet: build dense swirling volumetric billows &
    // soot/smoke straight from the weapon simulation segments (no line
    // ribbons).
    if (stream_fade > 0.002f &&
        (g_flame_jet_count > 0 || g_flame_pool_count > 0)) {
      static float s_flame_anim_time = 0.0f;
      s_flame_anim_time += world_dt;

      auto emit_plume_sprites = [&](const FlamePlumeSprite *sprites, size_t n) {
        for (size_t i = 0; i < n; i++) {
          const FlamePlumeSprite &s = sprites[i];
          const Color3 col{s.r, s.g, s.b};
          switch (s.kind) {
          case FlameSpriteKind::CoreDisc:
            Emit3DSoftDisc(tracer_verts, s.pos, s.radius, col, s.alpha, g_cam);
            break;
          case FlameSpriteKind::FireBillboard:
            EmitStretchedBillboard(
                fire_tex_verts, s.pos, s.dir, s.radius, s.stretch, s.rotation,
                s.tile, d3d::PackColorBgra(s.r, s.g, s.b, s.alpha), g_cam);
            break;
          case FlameSpriteKind::SmokeBillboard:
            EmitStretchedBillboard(
                smoke_tex_verts, s.pos, s.dir, s.radius, s.stretch, s.rotation,
                s.tile, d3d::PackColorBgra(s.r, s.g, s.b, s.alpha), g_cam);
            break;
          case FlameSpriteKind::FireGround:
            EmitTexturedGroundTile(fire_tex_verts, s.pos, s.radius, s.rotation,
                                   s.tile,
                                   d3d::PackColorBgra(s.r, s.g, s.b, s.alpha));
            break;
          case FlameSpriteKind::SmokeGround:
            EmitTexturedGroundTile(smoke_tex_verts, s.pos, s.radius, s.rotation,
                                   s.tile,
                                   d3d::PackColorBgra(s.r, s.g, s.b, s.alpha));
            break;
          }
        }
      };

      if (g_flame_jet_count > 0) {
        float jpos[kMaxFlameStreamPoints][3];
        float jrad[kMaxFlameStreamPoints];
        float jheat[kMaxFlameStreamPoints];
        const uint32_t jn =
            std::min(g_flame_jet_count, uint32_t(kMaxFlameStreamPoints));
        for (uint32_t i = 0; i < jn; i++) {
          jpos[i][0] = g_flame_jet[i].pos[0];
          jpos[i][1] = g_flame_jet[i].pos[1];
          jpos[i][2] = g_flame_jet[i].pos[2];
          jrad[i] = g_flame_jet[i].radius;
          jheat[i] = g_flame_jet[i].heat;
        }
        std::array<FlamePlumeSprite, 1024> jet_sprites;
        const size_t n_jet = BuildFlamethrowerJetSprites(
            jpos, jrad, jheat, jn, stream_fade, s_flame_anim_time, perf_mode,
            jet_sprites.data(), jet_sprites.size());
        emit_plume_sprites(jet_sprites.data(), n_jet);
      }

      if (g_flame_pool_count > 0) {
        float ppos[64][3];
        float prad[64];
        float pheat[64];
        const uint32_t pn = std::min(g_flame_pool_count, 64u);
        for (uint32_t i = 0; i < pn; i++) {
          ppos[i][0] = g_flame_pools[i].pos[0];
          ppos[i][1] = g_flame_pools[i].pos[1];
          ppos[i][2] = g_flame_pools[i].pos[2];
          prad[i] = g_flame_pools[i].radius;
          pheat[i] = g_flame_pools[i].heat;
        }
        std::array<FlamePlumeSprite, 256> pool_sprites;
        const size_t n_pool = BuildFlamethrowerPoolSprites(
            ppos, prad, pheat, pn, stream_fade, s_flame_anim_time,
            pool_sprites.data(), pool_sprites.size());
        emit_plume_sprites(pool_sprites.data(), n_pool);
      }
    }
    // 3. Continuous Pouring Wound Streams + Liquid Blood + Ground Pools
    // (depth-tested)
    {
      auto next_rand = [&]() -> float {
        g_fx_rng ^= g_fx_rng << 13;
        g_fx_rng ^= g_fx_rng >> 17;
        g_fx_rng ^= g_fx_rng << 5;
        return float(g_fx_rng & 0xFFFFu) / 65535.0f;
      };
      const bool keep_pools =
          modsettings::Registry().GetEnhanced().persistent_blood_decals;
      const float pool_ttl = keep_pools ? 22.0f : 8.0f;

      // 3a. Emit pouring liquid streams from active wounded soldiers
      for (size_t i = 0; i < kMaxWoundStreams; i++) {
        WoundBleedStream &ws = g_wound_streams[i];
        if (!ws.active)
          continue;
        if (world_dt > 0.0f) {
          ws.remaining -= world_dt;
          if (ws.remaining <= 0.0f || !MemMapped(ws.victim_ea, 0x390u)) {
            ws.active = false;
            continue;
          }
          ws.emit_accum += world_dt;
          if (ws.emit_accum >= 0.036f) {
            ws.emit_accum = 0.0f;
            const float fx = G<float>(ws.victim_ea + 0xA0u);
            const float fy = G<float>(ws.victim_ea + 0xA4u);
            const float fz = G<float>(ws.victim_ea + 0xA8u);
            if (std::isfinite(fx) && std::isfinite(fy) && std::isfinite(fz)) {
              ws.ground_y = fy + 0.025f;
              const bool dying = (R32(ws.victim_ea + 0x384u) != 0u);
              const float cur_y_off =
                  dying ? std::min(ws.y_offset, 0.24f) : ws.y_offset;
              const int count = 1 + int(ws.mult > 2 ? 2 : 1);
              for (int k = 0; k < count; k++) {
                size_t chosen = kMaxBloodNodes;
                for (size_t b = 0; b < kMaxBloodNodes; b++) {
                  size_t idx = (g_blood_cursor + b) % kMaxBloodNodes;
                  if (!g_blood[idx].active) {
                    chosen = idx;
                    break;
                  }
                }
                if (chosen == kMaxBloodNodes)
                  chosen = g_blood_cursor % kMaxBloodNodes;
                g_blood_cursor = (chosen + 1) % kMaxBloodNodes;
                BloodNode &bn = g_blood[chosen];
                bn.active = true;
                bn.is_mist = false;
                bn.pos[0] =
                    fx + ws.dir[0] * 0.08f + (next_rand() - 0.5f) * 0.08f;
                bn.pos[1] = fy + cur_y_off + (next_rand() - 0.5f) * 0.05f;
                bn.pos[2] =
                    fz + ws.dir[2] * 0.08f + (next_rand() - 0.5f) * 0.08f;
                std::memcpy(bn.prev_pos, bn.pos, sizeof(bn.prev_pos));
                const float out_spd = 0.25f + next_rand() * 0.75f;
                bn.vel[0] = ws.dir[0] * out_spd + (next_rand() - 0.5f) * 0.45f;
                bn.vel[1] = -0.35f - next_rand() * 0.55f;
                bn.vel[2] = ws.dir[2] * out_spd + (next_rand() - 0.5f) * 0.45f;
                bn.ground_y = ws.ground_y;
                bn.age = 0.0f;
                bn.ttl = 0.65f + next_rand() * 0.35f;
                bn.radius = 0.024f + next_rand() * 0.018f;
                bn.opacity = 0.95f;
                bn.r = 0.038f + next_rand() * 0.024f;
                bn.g = 0.0004f;
                bn.b = 0.0010f;
              }
            }
          }
        }
      }

      // 3b. Update & Emit 3D Arterial Mist & Pouring Liquid Streams; splash
      // onto floor
      for (size_t i = 0; i < kMaxBloodNodes; i++) {
        BloodNode &bn = g_blood[i];
        if (!bn.active)
          continue;
        bn.age += world_dt;
        if (bn.age >= bn.ttl) {
          bn.active = false;
          continue;
        }
        if (world_dt > 0.0f) {
          std::memcpy(bn.prev_pos, bn.pos, sizeof(bn.prev_pos));
          bn.pos[0] += bn.vel[0] * world_dt;
          bn.pos[1] += bn.vel[1] * world_dt;
          bn.pos[2] += bn.vel[2] * world_dt;
          if (bn.is_mist) {
            const float drag = std::max(0.0f, 1.0f - world_dt * 4.5f);
            bn.vel[0] *= drag;
            bn.vel[1] = bn.vel[1] * drag - 3.8f * world_dt;
            bn.vel[2] *= drag;
            if (bn.pos[1] <= bn.ground_y) {
              bn.active = false;
              continue;
            }
          } else {
            const float drag = std::max(0.0f, 1.0f - world_dt * 1.1f);
            bn.vel[0] *= drag;
            bn.vel[1] -= 12.5f * world_dt;
            bn.vel[2] *= drag;
            if (bn.pos[1] <= bn.ground_y) {
              // Droplet hits the floor: spawn/grow organic blood pool
              float hit_floor[3] = {bn.pos[0], bn.ground_y, bn.pos[2]};
              size_t chosen = kMaxBloodPools;
              for (size_t p = 0; p < kMaxBloodPools; p++) {
                if (g_blood_pools[p].active &&
                    VecDistSq(g_blood_pools[p].pos, hit_floor) <
                        0.24f * 0.24f) {
                  chosen = p;
                  g_blood_pools[p].target_radius =
                      std::min(0.82f, g_blood_pools[p].target_radius + 0.032f);
                  g_blood_pools[p].age = std::min(g_blood_pools[p].age, 0.5f);
                  break;
                }
              }
              if (chosen == kMaxBloodPools) {
                for (size_t p = 0; p < kMaxBloodPools; p++) {
                  size_t idx = (g_pool_cursor + p) % kMaxBloodPools;
                  if (!g_blood_pools[idx].active) {
                    chosen = idx;
                    break;
                  }
                }
                if (chosen == kMaxBloodPools)
                  chosen = g_pool_cursor % kMaxBloodPools;
                g_pool_cursor = (chosen + 1) % kMaxBloodPools;
                BloodPoolNode &pl = g_blood_pools[chosen];
                pl.active = true;
                std::memcpy(pl.pos, hit_floor, sizeof(pl.pos));
                pl.age = 0.0f;
                pl.ttl = pool_ttl;
                pl.radius = 0.07f;
                pl.target_radius = 0.16f + next_rand() * 0.14f;
                pl.seed = next_rand() * 6.28f;
                pl.opacity = 0.88f;
              }
              bn.active = false;
              continue;
            }
          }
        }
        float dz = (bn.pos[0] - g_cam.eye[0]) * g_cam.forward[0] +
                   (bn.pos[1] - g_cam.eye[1]) * g_cam.forward[1] +
                   (bn.pos[2] - g_cam.eye[2]) * g_cam.forward[2];
        if (dz <= 0.12f)
          continue;

        const float life = std::clamp(bn.age / bn.ttl, 0.0f, 1.0f);
        if (bn.is_mist) {
          const float fade =
              std::min(1.0f, bn.age * 28.0f) * std::pow(1.0f - life, 1.85f);
          const float alpha = bn.opacity * fade;
          if (alpha > 0.01f) {
            const float r = bn.radius * (0.80f + life * 0.85f);
            EmitTexturedBillboard(
                smoke_tex_verts, bn.pos, r, bn.age * 1.1f + float(i) * 1.1f,
                8 + (int(i) & 3),
                d3d::PackColorBgra(bn.r, bn.g, bn.b, alpha * 0.85f), g_cam);
          }
        } else {
          const float fade = std::pow(1.0f - life, 0.85f);
          const float alpha = bn.opacity * fade;
          if (alpha > 0.02f) {
            float tail_p[3] = {bn.pos[0] - bn.vel[0] * 0.055f,
                               bn.pos[1] - bn.vel[1] * 0.055f,
                               bn.pos[2] - bn.vel[2] * 0.055f};
            const uint32_t c_tail =
                d3d::PackColorBgra(bn.r * 0.65f, bn.g, bn.b, alpha * 0.35f);
            const uint32_t c_head =
                d3d::PackColorBgra(bn.r, bn.g, bn.b, alpha * 0.96f);
            Emit3DRibbon(smoke_verts, tail_p, bn.pos, bn.radius * 0.30f,
                         bn.radius * 0.85f, c_tail, c_head, g_cam);
            Emit3DSoftDisc(smoke_verts, bn.pos, bn.radius * 0.80f,
                           Color3{bn.r, bn.g, bn.b}, alpha * 0.90f, g_cam);
          }
        }
      }

      // 3c. Update & Emit Horizontal Ground Blood Pools
      for (size_t i = 0; i < kMaxBloodPools; i++) {
        BloodPoolNode &pl = g_blood_pools[i];
        if (!pl.active)
          continue;
        pl.age += world_dt;
        if (pl.age >= pl.ttl) {
          pl.active = false;
          continue;
        }
        if (world_dt > 0.0f && pl.radius < pl.target_radius) {
          pl.radius = std::min(pl.target_radius, pl.radius + world_dt * 0.38f);
        }
        float dz = (pl.pos[0] - g_cam.eye[0]) * g_cam.forward[0] +
                   (pl.pos[1] - g_cam.eye[1]) * g_cam.forward[1] +
                   (pl.pos[2] - g_cam.eye[2]) * g_cam.forward[2];
        if (dz <= -1.0f)
          continue;

        const float life = std::clamp(pl.age / pl.ttl, 0.0f, 1.0f);
        const float fade = (life > 0.72f)
                               ? std::clamp((1.0f - life) / 0.28f, 0.0f, 1.0f)
                               : 1.0f;
        EmitTexturedGroundTile(smoke_tex_verts, pl.pos, pl.radius * 1.25f,
                               pl.seed, 8 + (int(pl.seed * 10.0f) & 3),
                               d3d::PackColorBgra(0.068f, 0.0005f, 0.0012f,
                                                  pl.opacity * fade * 0.97f));
      }
    }

    // 3d. Update & Emit Sharp 2-frame Textured Muzzle Flashes
    for (size_t i = 0; i < kMaxMuzzleFlashes; i++) {
      MuzzleFlashNode &mf = g_muzzle_flashes[i];
      if (!mf.active)
        continue;
      mf.age += real_dt;
      if (mf.age >= mf.ttl) {
        mf.active = false;
        continue;
      }
      const float life = std::clamp(mf.age / mf.ttl, 0.0f, 1.0f);
      const float fade = 1.0f - life * life;
      const float r = mf.radius * (0.85f + life * 0.45f);
      EmitTexturedBillboard(
          fire_tex_verts, mf.pos, r, mf.rot, mf.tile,
          d3d::PackColorBgra(1.0f, 0.82f, 0.52f, fade * 0.96f), g_cam);
    }

    // 4. Update & Emit 3D Diverse Impact Sparks & Concrete Dust
    for (size_t i = 0; i < kMaxImpactNodes; i++) {
      ImpactNode &imp = g_impacts[i];
      if (!imp.active)
        continue;
      imp.age += world_dt;
      if (imp.age >= imp.ttl) {
        imp.active = false;
        continue;
      }
      if (world_dt > 0.0f) {
        std::memcpy(imp.prev_pos, imp.pos, sizeof(imp.prev_pos));
        imp.pos[0] += imp.vel[0] * world_dt;
        imp.pos[1] += imp.vel[1] * world_dt;
        imp.pos[2] += imp.vel[2] * world_dt;
        if (imp.is_spark) {
          imp.vel[1] -= 9.5f * world_dt;
        } else {
          const float drag = std::max(0.0f, 1.0f - world_dt * 3.2f);
          imp.vel[0] *= drag;
          imp.vel[1] = imp.vel[1] * drag + 0.18f * world_dt;
          imp.vel[2] *= drag;
        }
      }
      float dz = (imp.pos[0] - g_cam.eye[0]) * g_cam.forward[0] +
                 (imp.pos[1] - g_cam.eye[1]) * g_cam.forward[1] +
                 (imp.pos[2] - g_cam.eye[2]) * g_cam.forward[2];
      if (dz <= 0.12f)
        continue;

      const float life = std::clamp(imp.age / imp.ttl, 0.0f, 1.0f);
      const float fade = 1.0f - life;
      if (imp.is_spark) {
        float tail_p[3] = {imp.pos[0] - imp.vel[0] * 0.025f,
                           imp.pos[1] - imp.vel[1] * 0.025f,
                           imp.pos[2] - imp.vel[2] * 0.025f};
        const uint32_t c_tail =
            d3d::PackColorBgra(1.0f, 0.55f, 0.15f, fade * 0.15f);
        const uint32_t c_head =
            d3d::PackColorBgra(1.0f, 0.88f, 0.55f, fade * imp.opacity);
        Emit3DRibbon(tracer_verts, tail_p, imp.pos, imp.radius * 0.3f,
                     imp.radius, c_tail, c_head, g_cam);
      } else {
        const float r = imp.radius * (0.8f + life * 1.8f);
        Emit3DSoftDisc(smoke_verts, imp.pos, r, Color3{0.72f, 0.70f, 0.66f},
                       imp.opacity * fade * fade, g_cam);
      }
    }

    // 5. Physical 3D Ejecting Brass Shell Casings & Crimson Shotgun Hulls
    if (enh.shell_casings) {
      for (size_t i = 0; i < kMaxShellCasings; i++) {
        ShellCasingInstance &sc = g_shells[i];
        if (!sc.body.active)
          continue;
        if (world_dt > 0.0f) {
          if (StepShellCasingPhysics(sc.body, world_dt)) {
            if (pending_clink_count < 8u) {
              const float rel[3] = {sc.body.pos[0] - g_cam.eye[0],
                                    sc.body.pos[1] - g_cam.eye[1],
                                    sc.body.pos[2] - g_cam.eye[2]};
              const float pr = rel[0] * g_cam.right[0] + rel[1] * g_cam.right[1] +
                               rel[2] * g_cam.right[2];
              const float dist = std::max(0.35f, VecLen(rel));
              pending_clinks[pending_clink_count++] = {
                  std::clamp(pr / dist, -1.0f, 1.0f), sc.red_hull};
            }
          }
          if (!sc.body.active)
            continue;
        }
        const float dz = (sc.body.pos[0] - g_cam.eye[0]) * g_cam.forward[0] +
                         (sc.body.pos[1] - g_cam.eye[1]) * g_cam.forward[1] +
                         (sc.body.pos[2] - g_cam.eye[2]) * g_cam.forward[2];
        if (dz <= 0.06f)
          continue;

        const float life =
            std::clamp(sc.body.age / std::max(sc.body.ttl, 0.01f), 0.0f, 1.0f);
        const float fade =
            (life > 0.78f) ? std::clamp((1.0f - life) / 0.22f, 0.0f, 1.0f)
                           : 1.0f;
        const float cs = std::cos(sc.body.spin_rad);
        const float sn = std::sin(sc.body.spin_rad);
        float axis[3] = {sc.axis_a[0] * cs + sc.axis_b[0] * sn,
                         sc.axis_a[1] * cs + sc.axis_b[1] * sn,
                         sc.axis_a[2] * cs + sc.axis_b[2] * sn};
        const float al = VecLen(axis);
        if (al > 1e-4f) {
          axis[0] /= al;
          axis[1] /= al;
          axis[2] /= al;
        }
        const float half_l = sc.length_m * 0.5f;
        const float p0[3] = {sc.body.pos[0] - axis[0] * half_l,
                             sc.body.pos[1] - axis[1] * half_l,
                             sc.body.pos[2] - axis[2] * half_l};
        const float p1[3] = {sc.body.pos[0] + axis[0] * half_l,
                             sc.body.pos[1] + axis[1] * half_l,
                             sc.body.pos[2] + axis[2] * half_l};
        const uint32_t col_base =
            d3d::PackColorBgra(0.92f, 0.74f, 0.34f, 0.94f * fade);
        const uint32_t col_tip =
            sc.red_hull ? d3d::PackColorBgra(0.84f, 0.14f, 0.10f, 0.95f * fade)
                        : d3d::PackColorBgra(1.00f, 0.88f, 0.52f, 0.94f * fade);
        Emit3DRibbon(smoke_verts, p0, p1, sc.radius_m, sc.radius_m * 0.95f,
                     col_base, col_tip, g_cam);
      }
    }

    // 6. Lingering Firefight Cordite & Plaster Dust Haze Motes
    for (size_t i = 0; i < kMaxHazeMotes; i++) {
      HazeMoteInstance &hm = g_haze_motes[i];
      if (!hm.active)
        continue;
      if (world_dt > 0.0f) {
        hm.age += world_dt;
        if (hm.age >= hm.ttl) {
          hm.active = false;
          continue;
        }
        const float drag = std::exp(-0.55f * world_dt);
        hm.vel[0] *= drag;
        hm.vel[1] = hm.vel[1] * drag + 0.04f * world_dt;
        hm.vel[2] *= drag;
        hm.pos[0] += hm.vel[0] * world_dt;
        hm.pos[1] += hm.vel[1] * world_dt;
        hm.pos[2] += hm.vel[2] * world_dt;
        hm.rot += hm.rot_vel * world_dt;
      }
      const float dz = (hm.pos[0] - g_cam.eye[0]) * g_cam.forward[0] +
                       (hm.pos[1] - g_cam.eye[1]) * g_cam.forward[1] +
                       (hm.pos[2] - g_cam.eye[2]) * g_cam.forward[2];
      if (dz <= 0.25f)
        continue;

      const float u = std::clamp(hm.age / hm.ttl, 0.0f, 1.0f);
      const float fade = std::min(1.0f, hm.age * 3.0f) * std::pow(1.0f - u, 1.45f);
      const float alpha = hm.opacity * fade;
      if (alpha <= 0.008f)
        continue;
      const float r = hm.radius * (0.85f + u * 0.75f);
      EmitTexturedBillboard(
          smoke_tex_verts, hm.pos, r, hm.rot, hm.tile,
          d3d::PackColorBgra(0.76f, 0.72f, 0.65f, alpha), g_cam);
    }
  }

  for (size_t c = 0; c < pending_clink_count; c++) {
    sound::weapon_audio::TriggerShellClink(pending_clinks[c].pan,
                                           pending_clinks[c].shotgun);
  }

  if (!smoke_verts.empty()) {
    d3d::DrawWorldOverlay(smoke_verts.data(), uint32_t(smoke_verts.size()),
                          view_copy, proj_copy, false);
  }
  if (!smoke_tex_verts.empty()) {
    d3d::DrawTexturedWorldOverlay(smoke_tex_verts.data(),
                                  uint32_t(smoke_tex_verts.size()), view_copy,
                                  proj_copy, false);
  }
  if (!tracer_verts.empty()) {
    d3d::DrawWorldOverlay(tracer_verts.data(), uint32_t(tracer_verts.size()),
                          view_copy, proj_copy, true);
  }
  if (!fire_tex_verts.empty()) {
    d3d::DrawTexturedWorldOverlay(fire_tex_verts.data(),
                                  uint32_t(fire_tex_verts.size()), view_copy,
                                  proj_copy, true);
  }

  if (culled_this_frame > 0)
    g_culled_count.fetch_add(culled_this_frame, std::memory_order_relaxed);

  // Periodic 3.0s logcat telemetry for runtime diagnostics & Android
  // performance verification
  if (g_last_telemetry_log.time_since_epoch().count() == 0 ||
      std::chrono::duration<float>(now - g_last_telemetry_log).count() >=
          3.0f) {
    g_last_telemetry_log = now;
    if (active_tracer_cnt > 0 || active_smoke_cnt > 0 ||
        g_tracers_emitted.load() > 0 || g_smoke_emitted.load() > 0) {
      LOGR("[BALLISTICS_FX] tracers_on=%d freq=%u smoke_on=%d smoke_mode=%u "
           "active_t=%u active_s=%u emitted_t=%llu synced_hs=%llu "
           "emitted_s=%llu culled=%llu",
           tracers_active ? 1 : 0, enh.tracer_frequency, smoke_enabled ? 1 : 0,
           enh.smoke_mode, active_tracer_cnt, active_smoke_cnt,
           static_cast<unsigned long long>(g_tracers_emitted.load()),
           static_cast<unsigned long long>(g_hitscan_synced.load()),
           static_cast<unsigned long long>(g_smoke_emitted.load()),
           static_cast<unsigned long long>(g_culled_count.load()));
    }
  }
}

void ResetAll() {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto &t : g_tracers)
    t.active = false;
  for (auto &s : g_smoke)
    s.active = false;
  for (auto &bn : g_blood)
    bn.active = false;
  for (auto &ws : g_wound_streams)
    ws.active = false;
  for (auto &pl : g_blood_pools)
    pl.active = false;
  for (auto &mf : g_muzzle_flashes)
    mf.active = false;
  for (auto &imp : g_impacts)
    imp.active = false;
  for (auto &f : g_explosion_flames)
    f.active = false;
  for (auto &em : g_explosion_embers)
    em.active = false;
  for (auto &sc : g_shells)
    sc.body.active = false;
  for (auto &hm : g_haze_motes)
    hm.active = false;
  for (auto &rec : g_recent_explosions)
    rec.valid = false;
  for (auto &e : g_explosive_trackers)
    e.active = false;
  for (auto &m : g_muzzles)
    m.owner = 0;
  g_flame_jet_count = 0;
  g_flame_pool_count = 0;
  g_flame_stream_fresh = false;
  g_flame_ember_accum = 0.0f;
  g_flame_soot_accum = 0.0f;
  g_tracer_cursor = 0;
  g_smoke_cursor = 0;
  g_exp_flame_cursor = 0;
  g_exp_ember_cursor = 0;
  g_recent_exp_cursor = 0;
  g_muzzle_emitter = PlayerMuzzleEmitter{};
  g_cam.valid = false;
}

TelemetrySnapshot GetTelemetry() {
  const auto &enh = modsettings::Registry().GetEnhanced();
  const bool perf = IsPerformancePresetActive();
  TelemetrySnapshot snap;
  snap.tracers_enabled = enh.tracers_enabled && enh.tracer_frequency != 0;
  snap.smoke_enabled = enh.smoke_trails_enabled && enh.smoke_mode > 0;
  snap.max_tracers_cap = perf ? 32u : uint32_t(kMaxTracers);
  snap.max_smoke_cap = perf ? 64u : uint32_t(kMaxSmokeNodes);
  snap.total_shots_observed = g_shots_observed.load(std::memory_order_relaxed);
  snap.total_tracers_emitted =
      g_tracers_emitted.load(std::memory_order_relaxed);
  snap.total_hitscan_synced = g_hitscan_synced.load(std::memory_order_relaxed);
  snap.total_smoke_emitted = g_smoke_emitted.load(std::memory_order_relaxed);
  snap.total_culled = g_culled_count.load(std::memory_order_relaxed);
  snap.total_shells_ejected = g_shells_ejected.load(std::memory_order_relaxed);

  std::lock_guard<std::mutex> lock(g_mutex);
  for (const auto &t : g_tracers)
    if (t.active)
      snap.active_tracers++;
  for (const auto &s : g_smoke)
    if (s.active)
      snap.active_smoke_nodes++;
  for (const auto &sc : g_shells)
    if (sc.body.active)
      snap.active_shells++;
  return snap;
}

} // namespace ballistics

namespace haptics {
namespace {

std::mutex g_haptic_mutex;
HapticDispatcher g_haptic_dispatcher;

uint64_t NowHapticMs() {
  using namespace std::chrono;
  return uint64_t(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
          .count());
}

void EmitHapticPulse(const WeaponHapticPulse &pulse) {
  const bool enabled = modsettings::Registry().GetEnhanced().phone_vibration;
  WeaponHapticPulse emit{};
  {
    std::lock_guard<std::mutex> lock(g_haptic_mutex);
    if (!g_haptic_dispatcher.TrySchedule(NowHapticMs(), pulse, enabled,
                                         &emit)) {
      return;
    }
  }
#ifdef __ANDROID__
  host::VibratePhone(emit.duration_ms, emit.amplitude);
#else
  (void)emit;
#endif
}

}  // namespace

void DispatchWeaponHaptic(const char *weapon_code, bool suppressed,
                          bool alt_fire, bool rapid_fire_cheat) {
  EmitHapticPulse(ComputeWeaponHapticPulse(weapon_code, suppressed, alt_fire,
                                           rapid_fire_cheat));
}

void DispatchExplosionHaptic(const float world_pos[3]) {
  if (!world_pos)
    return;
  float eye[3] = {};
  if (!ballistics::GetSceneCameraVectors(eye, nullptr, nullptr, nullptr,
                                         nullptr, nullptr)) {
    return;
  }
  const float dx = world_pos[0] - eye[0];
  const float dy = world_pos[1] - eye[1];
  const float dz = world_pos[2] - eye[2];
  const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
  EmitHapticPulse(ComputeExplosionHapticPulse(dist));
}

}  // namespace haptics
} // namespace game

// ---- Weak Guest X86 Hooks wired cleanly without needing XBE recompile
// ------------------------- Note: sub_001DB7B0 & sub_001E26E0 notify us from
// time_stop.cpp; here we hook sub_001E2630 (physical bullet impact) and
// sub_00168E20 (AI/native tracer streak).

static inline bool HookAddrMapped(uint32_t va, uint32_t size = 16u) {
  return va >= 0x10000u && size > 0 &&
         (uint64_t(va) + size) <= 0x100000000ULL && ::mem::IsCommitted(va) &&
         ::mem::IsCommitted(va + size - 1u);
}

X86_EXTERN_FUNC(__imp__sub_001E2630);
X86_FUNC(sub_001E2630) {
  // sub_001E2630 takes esi = bullet slot ptr
  uint32_t slot_ea = ctx.esi;
  const bool prev_thermal = game::ballistics::BeginThermalImpact(slot_ea);
  __imp__sub_001E2630(ctx, base);
  game::ballistics::EndThermalImpact(prev_thermal);
  if (HookAddrMapped(slot_ea, 0x60u)) {
    float hit_pos[3] = {kernel::G<float>(slot_ea + 0x50),
                        kernel::G<float>(slot_ea + 0x54),
                        kernel::G<float>(slot_ea + 0x58)};
    game::ballistics::OnPhysicalBulletHit(slot_ea, hit_pos);
  }
}

X86_EXTERN_FUNC(__imp__sub_00168E20);
X86_FUNC(sub_00168E20) {
  // Args pushed right-to-left: [esp+4]=start*, [esp+8]=end*, [esp+0]=ret_addr;
  // ret 0x08
  uint32_t ret_addr = kernel::R32(ctx.esp);
  uint32_t start_ea = kernel::R32(ctx.esp + 4);
  uint32_t end_ea = kernel::R32(ctx.esp + 8);
  if (HookAddrMapped(start_ea, 12u) && HookAddrMapped(end_ea, 12u)) {
    float start[3] = {kernel::G<float>(start_ea),
                      kernel::G<float>(start_ea + 4),
                      kernel::G<float>(start_ea + 8)};
    float end[3] = {kernel::G<float>(end_ea), kernel::G<float>(end_ea + 4),
                    kernel::G<float>(end_ea + 8)};
    bool is_player = (ret_addr == 0x1E286Du || ret_addr == 0x1DBAC3u);
    game::ballistics::OnHitscanShot(start, end, ret_addr, is_player);
  }
  __imp__sub_00168E20(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_001E90B0);
X86_FUNC(sub_001E90B0) {
  // Explosion caller sites (0x1768BE, 0x176B89, 0x1EB1DB) pass [esp_entry+0x0C]
  // = frame matrix ptr (+0x30 = world XYZ).
  uint32_t frame_ea =
      HookAddrMapped(ctx.esp + 0x0Cu, 4u) ? kernel::R32(ctx.esp + 0x0Cu) : 0u;
  float pos[3] = {};
  bool have_pos = false;
  if (frame_ea && HookAddrMapped(frame_ea + 0x30u, 12u)) {
    pos[0] = kernel::G<float>(frame_ea + 0x30u);
    pos[1] = kernel::G<float>(frame_ea + 0x34u);
    pos[2] = kernel::G<float>(frame_ea + 0x38u);
    have_pos = std::isfinite(pos[0]) && std::isfinite(pos[1]) &&
               std::isfinite(pos[2]) && std::fabs(pos[0]) < 15000.0f &&
               std::fabs(pos[1]) < 15000.0f && std::fabs(pos[2]) < 15000.0f &&
               (pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]) > 0.01f;
  }
  __imp__sub_001E90B0(ctx, base);
  if (have_pos) {
    game::ballistics::OnExplosionDetonate(pos, 6.5f, 1.0f);
  }
}
