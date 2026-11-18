#include "game/flamethrower.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "app/app.h"
#include "common/menu.h"
#include "common/options.h"
#include "core/input.h"
#include "core/log.h"
#include "d3d/world_overlay.h"
#include "game/ballistics_fx.h"
#include "game/combat_feedback.h"
#include "game/delirium_physics.h"
#include "game/flamethrower_core.h"
#include "game/layout.h"
#include "game/overdrive.h"
#include "game/special_arsenal_core.h"
#include "game/time_stop.h"
#include "game/weapon_hands_skinner.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "sound/weapon_audio.h"
#include "xdk/hle.h"

// Release builds only persist Report/Error lines, so diagnostics that must appear in black.log use
// LOGR rather than LOGI. This variant also keeps a call site to its first few hits.
#define ARSENAL_LOG_FIRST(n, ...)                                                          \
    do {                                                                                   \
        static std::atomic<int> _arsenal_log_c{0};                                         \
        if (_arsenal_log_c.fetch_add(1, std::memory_order_relaxed) < (n)) LOGR(__VA_ARGS__); \
    } while (0)

namespace game::flamethrower {
using namespace kernel;
namespace sa = game::special_arsenal;

namespace {

constexpr uint32_t kWorld = ::layout::kWorld;
constexpr uint32_t kWorldState = 0x5AE0u;
constexpr uint32_t kStatePlaying = 0x1Cu;
constexpr uint32_t kWorldPlayer = ::layout::kWorldPlayer;
constexpr uint32_t kPlayerInventory = 0x278u;
constexpr uint32_t kInvSlots = 0x20u;   // the weapon objects, one a slot
constexpr uint32_t kInvHeld = 0x24u;    // the weapon in hand
constexpr uint32_t kWeaponOwner = 0xF0u;  // a weapon's owner (the player object when it is ours)
constexpr uint32_t kInvRequest = 0x38u;
constexpr uint32_t kInvSlotCount = 0x42u;
constexpr uint32_t kInvSlotHeld = 0x43u;
constexpr uint32_t kCharKind = 0xC4u;
constexpr uint32_t kEnemyKind = 1u;
constexpr uint32_t kCharPos = 0xA0u;
constexpr uint32_t kCharHealth = 0x2F0u;
constexpr uint32_t kCharDying = 0x384u;
constexpr uint32_t kBtexD3DFormat = 0x0AA10621u;
constexpr float kPi = 3.14159265f;
constexpr float kDegToRad = kPi / 180.0f;

struct AssetSpec {
    const char* code;
    const char* name;
    const char* bmsh_rel;
    const char* btex_rel;
    const char* bmsh_fb;
    const char* btex_fb;
};

constexpr AssetSpec kAssetSpecs[sa::kWeaponCount] = {
    {"FLM", "M2 FLAMETHROWER", "weapons/flamethrower.bmsh", "weapons/flamethrower.btex",
     "app/src/main/assets/weapons/flamethrower.bmsh", "app/src/main/assets/weapons/flamethrower.btex"},
    {"M91", "MAUSER 1891-12", "weapons/mauser_1891_12.bmsh", "weapons/mauser_1891_12.btex",
     "app/src/main/assets/weapons/mauser_1891_12.bmsh", "app/src/main/assets/weapons/mauser_1891_12.btex"},
    {"C96", "MAUSER C96 M712", "weapons/mauser_c96.bmsh", "weapons/mauser_c96.btex",
     "app/src/main/assets/weapons/mauser_c96.bmsh", "app/src/main/assets/weapons/mauser_c96.btex"},
    {"MXM", "MAXIM PM M1910", "weapons/maxim_m1910.bmsh", "weapons/maxim_m1910.btex",
     "app/src/main/assets/weapons/maxim_m1910.bmsh", "app/src/main/assets/weapons/maxim_m1910.btex"},
};

struct WeaponAsset {
    bool attempted = false;
    bool ready = false;
    sa::BmshModel model{};
    sa::BtexHeader btex_hdr{};
    std::vector<uint8_t> btex_bytes;
    const uint32_t* swizzled_bgra = nullptr;
    std::vector<d3d::ViewmodelTexturedVertex> base_verts;
    std::vector<d3d::ViewmodelTexturedVertex> work_verts;
    std::vector<std::vector<uint16_t>> part_unique_verts;
};

struct MatTexSlot {
    bool ready = false;
    sa::BtexHeader hdr{};
    std::vector<uint8_t> bytes;
    const uint32_t* swizzled_bgra = nullptr;
};

struct M91HdBundle {
    bool ready = false;
    std::vector<sa::MmatBatch> batches;
    std::array<MatTexSlot, 4> mats{};
};

struct M91HandsBundle {
    bool attempted = false;
    bool ready = false;
    game::hands_skinner::SkinnedHandsAsset skinner;
    MatTexSlot tex;
    std::vector<d3d::ViewmodelTexturedVertex> work_verts;
    uint8_t prev_clip = 0;
    float prev_time = 0.0f;
    uint8_t cur_clip = 0;
    float blend = 1.0f;
};

std::array<WeaponAsset, sa::kWeaponCount> g_assets{};
M91HdBundle g_m91_hd{};
M91HandsBundle g_m91_hands{};
std::atomic<bool> g_m91_hands_ready{false};

sa::ArsenalSim g_arsenal;
CoreSim g_flame_sim;
std::mutex g_state_mutex;

std::atomic<bool> g_want_active{false};
std::atomic<bool> g_trigger_held{false};
std::atomic<bool> g_fire_pressed_q{false};
std::atomic<bool> g_reload_queued{false};
std::atomic<bool> g_refill_queued{false};
std::atomic<bool> g_select_fire_q{false};
std::atomic<bool> g_zoom_cycle_q{false};
std::atomic<bool> g_aim_pad_held{false};
std::atomic<bool> g_aim_touch_held{false};
std::atomic<uint32_t> g_burning_count_frame{0};

std::atomic<bool> g_hud_active{false};
std::atomic<bool> g_hud_firing{false};
std::atomic<bool> g_hud_reloading{false};
std::atomic<float> g_hud_fuel{100.0f};
std::atomic<float> g_hud_reload_t{0.0f};
std::atomic<float> g_hud_pressure{0.0f};
std::atomic<uint32_t> g_hud_burning{0};
std::atomic<uint8_t> g_hud_weapon_id{0};
std::atomic<int32_t> g_hud_mag{100};
std::atomic<int32_t> g_hud_max_mag{100};
std::atomic<int32_t> g_hud_reserve{200};
std::atomic<int32_t> g_hud_squibs{5};
std::atomic<float> g_hud_water_c{28.0f};
std::atomic<bool> g_hud_water_boil{false};
std::atomic<bool> g_hud_full_auto{false};
std::atomic<bool> g_hud_bolt_open{false};
std::atomic<float> g_hud_ads_blend{0.0f};
std::atomic<float> g_hud_fov_scale{1.0f};
std::atomic<float> g_hud_scope_zoom_x{4.0f};
std::atomic<float> g_hud_scope_zero_m{200.0f};
std::atomic<bool> g_hud_scoped_out{false};

// Guest-thread combat & view state
float g_nozzle_world[3] = {};
float g_jet_dir[3] = {0.0f, 0.0f, 1.0f};
bool g_nozzle_valid = false;
std::chrono::steady_clock::time_point g_last_step_tp{};
float g_walk_phase = 0.0f;
float g_prev_eye[3] = {};
bool g_prev_eye_valid = false;
float g_last_cam_aspect = 16.0f / 9.0f;
float g_recoil_pitch_cur = 0.0f;
float g_recoil_yaw_cur = 0.0f;
float g_recoil_roll_cur = 0.0f;
float g_recoil_kick_z = 0.0f;
float g_maxim_wheel_sfx_accum = 0.0f;
uint32_t g_shot_rng_seq = 1u;

// Input-thread edge state
bool g_prev_swap_up = false;
bool g_prev_swap_dn = false;
bool g_prev_fire_trig = false;
bool g_prev_mode_btn = false;
bool g_prev_aim_btn = false;
bool g_swallow_up = false;
bool g_swallow_dn = false;
bool g_swallow_mode = false;

uint32_t PlayerPtr() noexcept {
    if (!mem::IsCommitted(kWorld)) return 0;
    const uint32_t w = R32(kWorld);
    if (!w || !mem::IsCommitted(w + kWorldPlayer + 4u)) return 0;
    return w + kWorldPlayer;
}

bool ValidAddr(uint32_t ea, uint32_t bytes) noexcept {
    return ea >= mem::kUserLow && bytes > 0 && (uint64_t(ea) + bytes) <= 0x100000000ULL &&
           mem::IsCommitted(ea) && mem::IsCommitted(ea + bytes - 1u);
}

uint32_t FBits(float v) noexcept {
    uint32_t u = 0;
    std::memcpy(&u, &v, sizeof(u));
    return u;
}

bool LoadFileBytes(const char* rel_path, const char* fallback_path, std::vector<uint8_t>& out) {
    out.clear();
    if (SDL_IOStream* io = SDL_IOFromFile(rel_path, "rb")) {
        const Sint64 sz = SDL_GetIOSize(io);
        if (sz > 0 && sz < 16 * 1024 * 1024) {
            out.resize(size_t(sz));
            const size_t got = SDL_ReadIO(io, out.data(), size_t(sz));
            SDL_CloseIO(io);
            if (got == size_t(sz)) return true;
        } else {
            SDL_CloseIO(io);
        }
    }
    if (fallback_path && fallback_path[0]) {
        std::ifstream f(fallback_path, std::ios::binary | std::ios::ate);
        if (f) {
            const auto sz = f.tellg();
            if (sz > 0 && sz < 16 * 1024 * 1024) {
                f.seekg(0);
                out.resize(size_t(sz));
                if (f.read(reinterpret_cast<char*>(out.data()), sz)) return true;
            }
        }
    }
    return false;
}

void LoadAssetFromSpecLocked(WeaponAsset& a, const AssetSpec& spec) {
    std::vector<uint8_t> bmsh_bytes;
    if (!LoadFileBytes(spec.bmsh_rel, spec.bmsh_fb, bmsh_bytes) ||
        !sa::ParseBmsh(bmsh_bytes.data(), bmsh_bytes.size(), a.model)) {
        LOGR("[ARSENAL] missing/bad mesh %s", spec.bmsh_rel);
        return;
    }

    if (!LoadFileBytes(spec.btex_rel, spec.btex_fb, a.btex_bytes) ||
        !sa::ParseBtexHeader(a.btex_bytes.data(), a.btex_bytes.size(), a.btex_hdr) ||
        a.btex_hdr.width != 1024u || a.btex_hdr.height != 1024u ||
        a.btex_hdr.d3d_format != kBtexD3DFormat) {
        LOGR("[ARSENAL] missing/bad texture %s (%zu bytes)", spec.btex_rel, a.btex_bytes.size());
        return;
    }
    a.swizzled_bgra = reinterpret_cast<const uint32_t*>(a.btex_bytes.data() + 32u);

    // Build neutral-lit textured vertices; lift legacy dark vertex diffuse (e.g. flamethrower avg~66)
    // so fixed-function TSS0 MODULATE displays the rich PBR BTEX albedo cleanly without crushing dark.
    double luma_sum = 0.0;
    for (const auto& sv : a.model.vertices) {
        const float r = float((sv.bgra >> 16) & 0xFFu);
        const float g = float((sv.bgra >> 8) & 0xFFu);
        const float b = float(sv.bgra & 0xFFu);
        luma_sum += double(r + g + b) / 3.0;
    }
    const float avg_lum = a.model.vertices.empty() ? 255.0f
                                                   : float(luma_sum / double(a.model.vertices.size()));
    const bool lift_diffuse = (avg_lum < 150.0f);

    a.base_verts.resize(a.model.vertices.size());
    for (size_t i = 0; i < a.model.vertices.size(); ++i) {
        const auto& s = a.model.vertices[i];
        uint32_t bgra = s.bgra;
        if (lift_diffuse) {
            auto lift_ch = [&](uint8_t c) -> uint32_t {
                const float v = std::clamp(212.0f + (float(c) - avg_lum) * 0.45f, 168.0f, 255.0f);
                return uint32_t(v + 0.5f);
            };
            const uint32_t aa = (bgra >> 24) & 0xFFu;
            const uint32_t rr = lift_ch(uint8_t((bgra >> 16) & 0xFFu));
            const uint32_t gg = lift_ch(uint8_t((bgra >> 8) & 0xFFu));
            const uint32_t bb = lift_ch(uint8_t(bgra & 0xFFu));
            bgra = (aa << 24) | (rr << 16) | (gg << 8) | bb;
        }
        a.base_verts[i] = {s.x, s.y, s.z, s.nx, s.ny, s.nz, bgra, s.u, s.v};
    }
    a.work_verts = a.base_verts;

    // Cache disjoint vertex index lists per articulated BmshPart once at load time.
    const size_t vc = a.base_verts.size();
    std::vector<uint8_t> seen(vc, 0u);
    a.part_unique_verts.resize(a.model.parts.size());
    for (size_t p = 0; p < a.model.parts.size(); ++p) {
        const sa::BmshPart& pt = a.model.parts[p];
        auto& dst = a.part_unique_verts[p];
        dst.reserve(pt.index_count / 2u + 8u);
        for (uint32_t k = 0; k < pt.index_count; ++k) {
            const uint16_t vi = a.model.indices[pt.first_index + k];
            if (vi < vc && !seen[vi]) {
                seen[vi] = 1u;
                dst.push_back(vi);
            }
        }
    }

    a.ready = true;
    LOGR("[ARSENAL] loaded %s (%zu verts, %zu parts, avg_lum=%.1f)",
         spec.code, vc, a.model.parts.size(), avg_lum);
}

bool LoadBtexSlot(const char* rel_path, const char* fb_path, MatTexSlot& slot) {
    if (!LoadFileBytes(rel_path, fb_path, slot.bytes) ||
        !sa::ParseBtexHeader(slot.bytes.data(), slot.bytes.size(), slot.hdr) ||
        slot.hdr.width < 256u || slot.hdr.width != slot.hdr.height ||
        slot.hdr.d3d_format != kBtexD3DFormat) {
        return false;
    }
    slot.swizzled_bgra = reinterpret_cast<const uint32_t*>(slot.bytes.data() + 32u);
    slot.ready = true;
    return true;
}

void LoadM91ExtrasLocked(WeaponAsset& m91_asset) {
    // Prefer full-resolution per-material HD mesh + 4 dedicated hardware-wrapped BTEX maps.
    std::vector<uint8_t> hd_bmsh, hd_mmat;
    if (LoadFileBytes("weapons/mauser_1891_12_hd.bmsh", "app/src/main/assets/weapons/mauser_1891_12_hd.bmsh", hd_bmsh) &&
        LoadFileBytes("weapons/mauser_1891_12_hd.mmat", "app/src/main/assets/weapons/mauser_1891_12_hd.mmat", hd_mmat)) {
        sa::BmshModel hd_model{};
        std::vector<sa::MmatBatch> batches;
        if (sa::ParseBmsh(hd_bmsh.data(), hd_bmsh.size(), hd_model) &&
            sa::ParseMmat(hd_mmat.data(), hd_mmat.size(), batches)) {
            bool all_tex = true;
            std::array<MatTexSlot, 4> texs{};
            for (int mi = 0; mi < 4; ++mi) {
                char rel[96], fb[128];
                std::snprintf(rel, sizeof(rel), "weapons/mauser_1891_12_m%d.btex", mi);
                std::snprintf(fb, sizeof(fb), "app/src/main/assets/weapons/mauser_1891_12_m%d.btex", mi);
                if (!LoadBtexSlot(rel, fb, texs[mi])) {
                    all_tex = false;
                    break;
                }
            }
            if (all_tex) {
                m91_asset.model = std::move(hd_model);
                m91_asset.base_verts.resize(m91_asset.model.vertices.size());
                for (size_t i = 0; i < m91_asset.model.vertices.size(); ++i) {
                    const auto& s = m91_asset.model.vertices[i];
                    m91_asset.base_verts[i] = {s.x, s.y, s.z, s.nx, s.ny, s.nz, s.bgra, s.u, s.v};
                }
                m91_asset.work_verts = m91_asset.base_verts;
                const size_t vc = m91_asset.base_verts.size();
                std::vector<uint8_t> seen(vc, 0u);
                m91_asset.part_unique_verts.assign(m91_asset.model.parts.size(), {});
                for (size_t p = 0; p < m91_asset.model.parts.size(); ++p) {
                    const sa::BmshPart& pt = m91_asset.model.parts[p];
                    auto& dst = m91_asset.part_unique_verts[p];
                    dst.reserve(pt.index_count / 2u + 8u);
                    for (uint32_t k = 0; k < pt.index_count; ++k) {
                        const uint16_t vi = m91_asset.model.indices[pt.first_index + k];
                        if (vi < vc && !seen[vi]) {
                            seen[vi] = 1u;
                            dst.push_back(vi);
                        }
                    }
                }
                g_m91_hd.batches = std::move(batches);
                g_m91_hd.mats = std::move(texs);
                g_m91_hd.ready = true;
                LOGR("[ARSENAL] M91 HD multi-material active (%zu batches, %zu verts)",
                     g_m91_hd.batches.size(), vc);
            }
        }
    }

    if (!g_m91_hands.attempted) {
        g_m91_hands.attempted = true;
        std::vector<uint8_t> hskn_bytes;
        if (LoadFileBytes("weapons/mauser_1891_12_hands.hskn",
                          "app/src/main/assets/weapons/mauser_1891_12_hands.hskn", hskn_bytes) &&
            g_m91_hands.skinner.Parse(hskn_bytes.data(), hskn_bytes.size()) &&
            LoadBtexSlot("weapons/mauser_1891_12_hands.btex",
                         "app/src/main/assets/weapons/mauser_1891_12_hands.btex", g_m91_hands.tex)) {
            g_m91_hands.ready = true;
            g_m91_hands_ready.store(true, std::memory_order_release);
            LOGR("[ARSENAL] loaded M91 40-bone rigged hands (%u verts, %u clips)",
                 g_m91_hands.skinner.VertexCount(), g_m91_hands.skinner.ClipCount());
        } else {
            LOGR("[ARSENAL] M91 rigged hands unavailable; using BLACK arms fallback");
        }
    }
}

void LoadSingleAssetLocked(size_t idx) {
    WeaponAsset& a = g_assets[idx];
    if (a.attempted) return;
    a.attempted = true;
    LoadAssetFromSpecLocked(a, kAssetSpecs[idx]);
    if (idx == size_t(sa::SpecialWeaponId::Mauser1891) && a.ready) {
        LoadM91ExtrasLocked(a);
    }
}

void BuildPerspectiveD3D8LH(float fov_y_rad, float aspect, float zn, float zf, float out[16]) {
    std::memset(out, 0, 64);
    const float f = 1.0f / std::tan(fov_y_rad * 0.5f);
    out[0] = f / std::max(aspect, 0.4f);
    out[5] = f;
    out[10] = zf / (zf - zn);
    out[11] = 1.0f;
    out[14] = -(zn * zf) / (zf - zn);
}

void TransformPoint(const float m[16], const float in[3], float out[3]) {
    out[0] = in[0] * m[0] + in[1] * m[4] + in[2] * m[8] + m[12];
    out[1] = in[0] * m[1] + in[1] * m[5] + in[2] * m[9] + m[13];
    out[2] = in[0] * m[2] + in[1] * m[6] + in[2] * m[10] + m[14];
}

void ApplyGuestDamage(X86Context& ctx, uint8_t* base, uint32_t victim, uint32_t me,
                      const float pos[3], const float dir[3], float force, uint32_t zone,
                      bool thermal) {
    if (!base || !victim || !me || !pos || !dir) return;
    if (!ValidAddr(victim, 0x390u) || !ValidAddr(me, 0x3A0u)) return;
    const uint32_t v_node = R32(victim + 0x324u);
    const uint32_t v_mesh = R32(victim + 0x320u);
    const uint32_t m_wp = R32(me + 0x29Cu);
    if (!ValidAddr(v_node, 0xCB0u) || !ValidAddr(v_mesh, 0x48u) || !ValidAddr(m_wp, 0x110u)) return;
    const uint32_t t0 = R32(0x002D12D0u);
    if (t0) {
        if (!ValidAddr(t0, 4u)) return;
        const uint32_t t1 = R32(t0);
        if (!ValidAddr(t1, 8u)) return;
        const uint32_t t2 = R32(t1 + 4u);
        if (!ValidAddr(t2, 12u)) return;
        const uint32_t slot0 = R32(t2 + 8u);
        if (!ValidAddr(slot0, 0x94u)) return;
    }

    static uint32_t s_scratch = 0;
    if (!s_scratch) s_scratch = mem::AllocSystem(32, 16);
    if (!s_scratch) return;

    float* hp = reinterpret_cast<float*>(HostPtr(s_scratch));
    float* hd = reinterpret_cast<float*>(HostPtr(s_scratch + 16));
    hp[0] = pos[0]; hp[1] = pos[1]; hp[2] = pos[2];
    hd[0] = dir[0]; hd[1] = dir[1]; hd[2] = dir[2];

    const bool prev_thermal = thermal ? game::ballistics::BeginExternalThermalImpact()
                                      : game::ballistics::InThermalImpact();
    const uint32_t saved_ecx = ctx.ecx;
    ctx.ecx = victim;
    // arg5 ([ebp+0x18]) is a signed int8 impact slot index (0 = default), never a float bit pattern
    CallGuest(ctx, 0x001E74B0u, {s_scratch, s_scratch + 16, FBits(force), zone, 0u, me, 0u});
    ctx.ecx = saved_ecx;
    if (thermal) game::ballistics::EndThermalImpact(prev_thermal);
}

void PublishHudLocked(const sa::WeaponStatus& st, float flame_pressure) {
    g_hud_active.store(g_want_active.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_hud_weapon_id.store(uint8_t(st.id), std::memory_order_relaxed);
    const bool firing_now =
        (st.id == sa::SpecialWeaponId::Flamethrower)
            ? (st.phase == sa::ActionPhase::Firing || st.phase == sa::ActionPhase::Igniting)
            : (st.phase == sa::ActionPhase::Firing);
    g_hud_firing.store(firing_now, std::memory_order_relaxed);
    g_hud_reloading.store(st.reloading, std::memory_order_relaxed);
    g_hud_fuel.store(st.fuel_pct, std::memory_order_relaxed);
    g_hud_reload_t.store(g_flame_sim.ReloadProgress(), std::memory_order_relaxed);
    g_hud_pressure.store(
        (st.id == sa::SpecialWeaponId::Flamethrower) ? flame_pressure : st.pose.trigger_pull,
        std::memory_order_relaxed);
    g_hud_mag.store(st.mag_ammo, std::memory_order_relaxed);
    g_hud_max_mag.store(st.max_mag_ammo, std::memory_order_relaxed);
    g_hud_reserve.store(st.reserve_ammo, std::memory_order_relaxed);
    g_hud_squibs.store(st.ignition_cartridges, std::memory_order_relaxed);
    g_hud_water_c.store(st.water_temp_c, std::memory_order_relaxed);
    g_hud_water_boil.store(st.water_boiling, std::memory_order_relaxed);
    g_hud_full_auto.store(st.full_auto_mode, std::memory_order_relaxed);
    g_hud_bolt_open.store(st.bolt_locked_open, std::memory_order_relaxed);
    g_hud_ads_blend.store(st.ads_blend, std::memory_order_relaxed);
    g_hud_fov_scale.store(st.ads_fov_scale, std::memory_order_relaxed);
    g_hud_scope_zoom_x.store(st.scope_zoom_x, std::memory_order_relaxed);
    g_hud_scope_zero_m.store(st.scope_zero_m, std::memory_order_relaxed);
    g_hud_scoped_out.store(g_want_active.load(std::memory_order_relaxed) &&
                               st.id == sa::SpecialWeaponId::Mauser1891 && st.ads_blend >= 0.86f,
                           std::memory_order_relaxed);
}

inline float SmoothStep(float a, float b, float x) noexcept {
    if (x <= a) return 0.0f;
    if (x >= b) return 1.0f;
    const float t = (x - a) / (b - a);
    return t * t * (3.0f - 2.0f * t);
}

void EvalReloadPose(sa::SpecialWeaponId id, float t, float clip_insert, float top_cover_deg,
                    float& out_x, float& out_y, float& out_z,
                    float& out_pitch, float& out_yaw, float& out_roll) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    if (id == sa::SpecialWeaponId::Flamethrower) {
        const float cant = SmoothStep(0.0f, 0.18f, t) * (1.0f - SmoothStep(0.70f, 0.96f, t));
        const float drop = SmoothStep(0.18f, 0.36f, t) * (1.0f - SmoothStep(0.66f, 0.84f, t));
        const float eject_kick = std::sin(SmoothStep(0.16f, 0.32f, t) * kPi);
        const float slam = std::sin(SmoothStep(0.73f, 0.87f, t) * kPi);
        out_x = -0.024f * cant - 0.022f * drop + 0.008f * slam;
        out_y = 0.018f * cant - 0.155f * drop - 0.025f * eject_kick + 0.030f * slam;
        out_z = -0.045f * drop + 0.025f * slam;
        out_pitch = -0.18f * cant + 0.40f * drop - 0.14f * slam;
        out_yaw = 0.22f * cant + 0.12f * drop - 0.05f * slam;
        out_roll = -0.44f * cant - 0.20f * drop + 0.14f * slam;
    } else if (id == sa::SpecialWeaponId::Maxim1910) {
        const float lid = std::clamp(top_cover_deg / 85.0f, 0.0f, 1.0f);
        out_x = -0.018f * lid;
        out_y = -0.055f * lid;
        out_z = -0.025f * lid;
        out_pitch = 0.20f * lid;
        out_yaw = 0.14f * lid;
        out_roll = -0.18f * lid;
    } else {
        // Mauser 1891-12 / C96 M712: cant left to display stripper-clip loading over breech
        const float w = std::max(clip_insert, std::sin(SmoothStep(0.0f, 0.95f, t) * kPi));
        out_x = -0.028f * w;
        out_y = -0.035f * w;
        out_z = -0.022f * w;
        out_pitch = 0.22f * w;
        out_yaw = 0.26f * w;
        out_roll = -0.42f * w;
    }
}

void TransformPartVertices(WeaponAsset& asset, size_t part_idx,
                           float rx_rad, float ry_rad, float rz_rad,
                           float tx, float ty, float tz, bool visible) {
    const auto& vlist = asset.part_unique_verts[part_idx];
    const sa::BmshPart& pt = asset.model.parts[part_idx];
    const float px = pt.pivot[0], py = pt.pivot[1], pz = pt.pivot[2];
    const float cx = std::cos(rx_rad), sx = std::sin(rx_rad);
    const float cy = std::cos(ry_rad), sy = std::sin(ry_rad);
    const float cz = std::cos(rz_rad), sz = std::sin(rz_rad);
    const bool identity_rot = (std::fabs(rx_rad) < 1e-4f &&
                               std::fabs(ry_rad) < 1e-4f &&
                               std::fabs(rz_rad) < 1e-4f);

    for (uint16_t vi : vlist) {
        const auto& src = asset.base_verts[vi];
        auto& dst = asset.work_verts[vi];
        if (!visible) {
            dst = src;
            dst.x = 0.0f; dst.y = 0.0f; dst.z = 0.0f;
            continue;
        }
        float lx = src.x - px;
        float ly = src.y - py;
        float lz = src.z - pz;
        float nx = src.nx, ny = src.ny, nz = src.nz;
        if (!identity_rot) {
            // Apply Rx, then Ry, then Rz around part pivot
            if (std::fabs(rx_rad) >= 1e-4f) {
                const float y1 = ly * cx - lz * sx;
                const float z1 = ly * sx + lz * cx;
                ly = y1; lz = z1;
                const float ny1 = ny * cx - nz * sx;
                const float nz1 = ny * sx + nz * cx;
                ny = ny1; nz = nz1;
            }
            if (std::fabs(ry_rad) >= 1e-4f) {
                const float x1 = lx * cy + lz * sy;
                const float z1 = -lx * sy + lz * cy;
                lx = x1; lz = z1;
                const float nx1 = nx * cy + nz * sy;
                const float nz1 = -nx * sy + nz * cy;
                nx = nx1; nz = nz1;
            }
            if (std::fabs(rz_rad) >= 1e-4f) {
                const float x1 = lx * cz - ly * sz;
                const float y1 = lx * sz + ly * cz;
                lx = x1; ly = y1;
                const float nx1 = nx * cz - ny * sz;
                const float ny1 = nx * sz + ny * cz;
                nx = nx1; ny = ny1;
            }
        }
        dst = src;
        dst.x = lx + px + tx;
        dst.y = ly + py + ty;
        dst.z = lz + pz + tz;
        dst.nx = nx; dst.ny = ny; dst.nz = nz;
    }
}

void ApplyWeaponPartKinematics(WeaponAsset& asset, const sa::WeaponStatus& st, float walk_phase,
                               float recoil_z) {
    const sa::PartAnimPose& p = st.pose;
    for (size_t i = 0; i < asset.model.parts.size(); ++i) {
        const char* nm = asset.model.parts[i].name;
        float rx = 0.0f, ry = 0.0f, rz = 0.0f;
        float tx = 0.0f, ty = 0.0f, tz = 0.0f;
        bool visible = true;

        if (st.id == sa::SpecialWeaponId::Flamethrower) {
            if (std::strcmp(nm, "trigger") == 0) {
                rx = p.trigger_pull * 0.42f;
            } else if (std::strcmp(nm, "safety_lever") == 0) {
                rx = (1.0f - p.safety_lever) * 0.55f;
            } else if (std::strcmp(nm, "charging_handle") == 0) {
                tz = -0.014f * p.pilot_spark;
            } else if (std::strcmp(nm, "pressure_gauge") == 0) {
                tz = (p.gauge_needle > 0.1f) ? (std::sin(walk_phase * 29.0f) * 0.0015f) : 0.0f;
            } else if (std::strcmp(nm, "tanks") == 0) {
                rx = p.tank_tilt_rad;
                ty = -std::sin(std::min(1.0f, std::fabs(p.tank_tilt_rad) * 2.0f)) * 0.06f;
            }
        } else if (st.id == sa::SpecialWeaponId::Mauser1891) {
            if (std::strcmp(nm, "trigger") == 0) {
                rx = p.trigger_pull * 0.28f;
            } else if (std::strcmp(nm, "bolt_handle") == 0) {
                rz = p.bolt_lift_deg * kDegToRad;
                tz = -p.bolt_retract_m;
            } else if (std::strcmp(nm, "bolt_body") == 0) {
                tz = -p.bolt_retract_m;
            } else if (std::strcmp(nm, "bolt_stop") == 0) {
                ry = p.bolt_stop_deg * kDegToRad;
            } else if (std::strcmp(nm, "safety_lever") == 0) {
                rz = -(1.0f - p.safety_lever) * (45.0f * kDegToRad);
            } else if (std::strcmp(nm, "rear_sight") == 0) {
                rx = p.rear_sight_deg * kDegToRad;
            } else if (std::strcmp(nm, "stripper_clip") == 0) {
                visible = st.reloading && (p.clip_insert > 0.01f || p.clip_eject > 0.01f);
                if (p.clip_eject > 0.01f) {
                    tx = p.clip_eject * 0.075f;
                    ty = p.clip_eject * 0.085f;
                    rz = -p.clip_eject * 1.15f;
                } else {
                    ty = (1.0f - p.clip_insert) * 0.055f;
                }
            } else if (std::strcmp(nm, "clip_rounds") == 0) {
                visible = st.reloading && (p.clip_insert > 0.01f) && (p.clip_rounds_left > 0.1f);
                const float fed = 1.0f - (p.clip_rounds_left / 5.0f);
                ty = (1.0f - p.clip_insert) * 0.055f - fed * 0.056f;
            }
        } else if (st.id == sa::SpecialWeaponId::MauserC96) {
            if (std::strcmp(nm, "body_frame") == 0) {
                tz = -p.barrel_recoil_m;
            } else if (std::strcmp(nm, "bolt") == 0) {
                tz = -(p.barrel_recoil_m + p.bolt_retract_m);
            } else if (std::strcmp(nm, "stripper_clip") == 0) {
                visible = st.reloading && (p.clip_insert > 0.01f);
                ty = -p.clip_insert * 0.060f;
            } else if (std::strcmp(nm, "hammer") == 0) {
                rx = (1.0f - p.hammer_cocked) * (54.0f * kDegToRad);
            } else if (std::strcmp(nm, "trigger") == 0) {
                rx = p.trigger_pull * (18.0f * kDegToRad);
            } else if (std::strcmp(nm, "safety_lever") == 0) {
                rx = -(1.0f - p.safety_lever) * (32.0f * kDegToRad);
            } else if (std::strcmp(nm, "rear_sight") == 0) {
                rx = p.rear_sight_deg * kDegToRad;
            }
        } else if (st.id == sa::SpecialWeaponId::Maxim1910) {
            if (std::strcmp(nm, "wheel_left") == 0 || std::strcmp(nm, "wheel_right") == 0) {
                rx = -p.wheel_angle_rad;
            } else if (std::strcmp(nm, "top_cover") == 0) {
                rx = p.top_cover_deg * kDegToRad;
            } else if (std::strcmp(nm, "spade_grips") == 0) {
                rx = -p.spade_thumb_press * 0.18f;
                tz = recoil_z * 0.65f;
            } else if (std::strcmp(nm, "water_jacket") == 0 || std::strcmp(nm, "receiver") == 0) {
                tz = recoil_z * 1.15f;
            } else if (std::strcmp(nm, "ammo_belt") == 0) {
                tx = -p.belt_shift * 0.032f;
                ty = -0.025f * std::clamp(p.top_cover_deg / 85.0f, 0.0f, 1.0f);
            }
        }
        TransformPartVertices(asset, i, rx, ry, rz, tx, ty, tz, visible);
    }
}

void DispatchArsenalAudioCues(const sa::ArsenalStepEvents& ev) {
    using Cue = sa::AudioCueKind;
    // Every cue below belongs to a MOD-ONLY special weapon (M2 / Mauser 1891-12 / C96 /
    // Maxim M1910). Those weapons do not exist in the original ISO audio bank, so the
    // BWAR pak is their ONLY audio source. We therefore force the pack on here: the
    // "WEAPON AUDIO MODE: ORIGINAL" toggle (mod_audio_pack_enabled) must never be able to
    // silence them. Original-weapon gunfire keeps honouring the flag via hooks.cpp.
    constexpr bool kForce = true;
    for (uint8_t i = 0; i < ev.audio_cue_count; ++i) {
        switch (ev.audio_cues[i]) {
            case Cue::FlameIgniteSquib:
                sound::weapon_audio::TriggerSpecialSlot(129, 0.96f, false, kForce);
                sound::weapon_audio::OnPlayerFire("FLM", false, false, kForce);
                break;
            case Cue::FlameValveShutoff:
                sound::weapon_audio::TriggerSpecialSlot(130, 0.85f, false, kForce);
                break;
            case Cue::FlameTankReload:
                sound::weapon_audio::TriggerSpecialSlot(131, 0.92f, false, kForce);
                break;
            case Cue::Mauser91Shot:
                sound::weapon_audio::OnPlayerFire("M91", false, false, kForce);
                break;
            case Cue::Mauser91BoltCycle:
                sound::weapon_audio::TriggerSpecialSlot(133, 0.92f, false, kForce);
                break;
            case Cue::Mauser91StripperClip:
                sound::weapon_audio::TriggerSpecialSlot(134, 0.92f, false, kForce);
                break;
            case Cue::Mauser91DryFire:
                sound::weapon_audio::TriggerSpecialSlot(135, 0.85f, false, kForce);
                break;
            case Cue::Mauser91ChamberClose:
                sound::weapon_audio::TriggerSpecialSlot(149, 0.90f, false, kForce);
                break;
            case Cue::Mauser91Equip:
                sound::weapon_audio::TriggerSpecialSlot(153, 0.88f, false, kForce);
                break;
            case Cue::Mauser91ScopeZoom:
                sound::weapon_audio::TriggerSpecialSlot(135, 0.56f, false, kForce);
                break;
            case Cue::C96Shot:
                sound::weapon_audio::OnPlayerFire("C96", false, false, kForce);
                sound::weapon_audio::TriggerSpecialSlot(137, 0.48f, false, kForce);
                break;
            case Cue::C96BreechClack:
                sound::weapon_audio::TriggerSpecialSlot(137, 0.78f, false, kForce);
                break;
            case Cue::C96ClipReload:
                sound::weapon_audio::TriggerSpecialSlot(138, 0.92f, false, kForce);
                break;
            case Cue::C96DryFire:
                sound::weapon_audio::TriggerSpecialSlot(139, 0.85f, false, kForce);
                break;
            case Cue::C96BoltLockOpen:
                sound::weapon_audio::TriggerSpecialSlot(140, 0.90f, false, kForce);
                break;
            case Cue::MaximShot:
                sound::weapon_audio::OnPlayerFire("MXM", false, false, kForce);
                sound::weapon_audio::TriggerSpecialSlot(151, 0.65f, false, kForce);
                break;
            case Cue::MaximTopCoverReload:
                sound::weapon_audio::TriggerSpecialSlot(142, 0.94f, false, kForce);
                break;
            case Cue::MaximWaterBoil:
                sound::weapon_audio::TriggerSpecialSlot(143, 0.76f, false, kForce);
                break;
            case Cue::MaximSteamHiss:
                sound::weapon_audio::TriggerSpecialSlot(144, 0.86f, false, kForce);
                break;
            default:
                break;
        }
    }
}

// Our own copy of a genuine player fire packet. The one the guest builds for a stock shot only
// lives as long as that shot (and the level), so passing its address around is what made the
// arsenal's bullets stop reaching the world: once the pointer went stale every shot returned early
// and no physical bullet was ever spawned -- which is exactly why windows, cars and walls ignored
// the special weapons. We therefore mirror the packet into memory we own, refresh the mirror every
// time a real player shot is observed, and hand that stable copy to the spawner.
uint32_t g_guest_fire_pkt = 0;  // 32 bytes we allocated: player fire packet for the physical bullet
uint32_t g_guest_fire_pkt_src = 0;

// Writes a packet naming this weapon. Only the weapon pointer is ever read from a fire packet
// (0x001DB7B0 and every observer of it read exactly +4), so a zeroed packet plus that pointer is a
// faithful stand-in and keeps the spawner off memory the guest may have freed.
bool BuildFirePacket(uint32_t weapon) {
    if (!weapon) return false;
    if (!g_guest_fire_pkt) g_guest_fire_pkt = mem::AllocSystem(32, 16);
    if (!g_guest_fire_pkt || !HostPtr(g_guest_fire_pkt)) return false;
    uint8_t* dst = reinterpret_cast<uint8_t*>(HostPtr(g_guest_fire_pkt));
    std::memset(dst, 0, 32);
    std::memcpy(dst + 4, &weapon, sizeof(weapon));
    return true;
}

// The weapon the player is really holding. Lets the arsenal fire a world bullet even on a run where
// no stock weapon was ever discharged, so the very first special-weapon shot already moves the world.
uint32_t PlayerStockWeapon() noexcept {
    const uint32_t p = PlayerPtr();
    if (!p || !ValidAddr(p + kPlayerInventory, 0x44u)) return 0;
    const uint32_t inv = p + kPlayerInventory;
    const uint32_t held = R32(inv + kInvHeld);
    if (ValidAddr(held, 0x110u) && R32(held + kWeaponOwner) == p) return held;
    const uint32_t slots = R32(inv + kInvSlots);
    for (uint32_t i = 0; i < 2u; ++i) {
        if (!ValidAddr(slots + i * 4u, 4u)) break;
        const uint32_t wpn = R32(slots + i * 4u);
        if (ValidAddr(wpn, 0x110u) && R32(wpn + kWeaponOwner) == p) return wpn;
    }
    return 0;
}

// The packet a physical bullet may be fired with: our mirror, validated against the player's own
// weapon so the spawner can never be handed a packet that no longer means anything.
bool GuestFirePacket(uint32_t& out_pkt, float& out_range, float& out_speed, uint8_t& out_flags) {
    out_pkt = 0;
    out_range = 80.0f;
    out_speed = 160.0f;
    out_flags = 0;

    uint32_t src = 0;
    float range = 0.0f, speed = 0.0f;
    uint8_t flags = 0;
    if (game::time_stop::LastPlayerFirePkt(src, range, speed, flags) && src && ValidAddr(src, 24u) &&
        HostPtr(src)) {
        // Refresh our mirror from the genuine packet so range/speed/flags stay authentic.
        if (BuildFirePacket(R32(src + 4u))) {
            uint8_t* dst = reinterpret_cast<uint8_t*>(HostPtr(g_guest_fire_pkt));
            std::memcpy(dst, reinterpret_cast<const uint8_t*>(HostPtr(src)), 24u);
            std::memset(dst + 24, 0, 8);
            g_guest_fire_pkt_src = src;
        }
        out_range = range;
        out_speed = speed;
        out_flags = flags;
    }

    const uint32_t p = PlayerPtr();
    auto ensure_proj_data = [&](uint32_t pkt) {
        if (!pkt || !ValidAddr(pkt, 0x20u)) return;
        const uint32_t proj_data = R32(pkt + 0x0Cu);
        if (!proj_data || !ValidAddr(proj_data, 0x1Cu)) {
            static uint32_t s_safe_proj_data = 0;
            if (!s_safe_proj_data) {
                s_safe_proj_data = mem::AllocSystem(0x20u, 16u);
                if (s_safe_proj_data && HostPtr(s_safe_proj_data)) {
                    std::memset(HostPtr(s_safe_proj_data), 0, 0x20u);
                    W32(s_safe_proj_data + 0x00u, 1u);
                    G<float>(s_safe_proj_data + 0x14u) = 160.0f;
                }
            }
            if (s_safe_proj_data) W32(pkt + 0x0Cu, s_safe_proj_data);
        } else {
            const float mag_range = G<float>(proj_data + 0x14u);
            if (std::isfinite(mag_range) && mag_range > 10.0f && mag_range < 1000.0f) {
                out_range = mag_range;
            }
        }
    };
    auto packet_ok = [&](uint32_t pkt) {
        if (!p || !pkt || !ValidAddr(pkt, 0x20u)) return false;
        const uint32_t wpn = R32(pkt + 4u);
        if (!wpn || !ValidAddr(wpn, 0x110u) || R32(wpn + kWeaponOwner) != p) return false;
        ensure_proj_data(pkt);
        const uint32_t proj_data = R32(pkt + 0x0Cu);
        return proj_data != 0 && ValidAddr(proj_data, 0x1Cu);
    };
    if (packet_ok(g_guest_fire_pkt)) {
        out_pkt = g_guest_fire_pkt;
        return true;
    }

    // No genuine player packet yet. Clone the first real packet the engine built -- from any
    // weapon -- and replace only the weapon pointer with the player's own gun.
    const uint32_t stock_weapon = PlayerStockWeapon();
    uint8_t tmpl[64];
    if (stock_weapon && game::time_stop::FirePacketTemplate(tmpl, sizeof(tmpl)) &&
        BuildFirePacket(stock_weapon)) {
        uint8_t* dst = reinterpret_cast<uint8_t*>(HostPtr(g_guest_fire_pkt));
        std::memcpy(dst, tmpl, 32);
        std::memcpy(dst + 4, &stock_weapon, sizeof(stock_weapon));
        if (packet_ok(g_guest_fire_pkt)) {
            g_guest_fire_pkt_src = 0;
            ARSENAL_LOG_FIRST(3, "[ARSENAL] world bullet pkt cloned from genuine guest template");
            out_pkt = g_guest_fire_pkt;
            return true;
        }
    }
    // Fallback directly to the stock weapon's live magazine packet (weapon+0xF4):
    // sub_001DBD50 passes weapon+0xF4 directly as `fire_pkt` into 0x001DB7B0; sub_001DB910
    // reads [pkt+0x04] (weapon) and [pkt+0x0C] (projectile/range table -> [ptr+0x14] float).
    if (stock_weapon && BuildFirePacket(stock_weapon)) {
        const uint32_t mag = R32(stock_weapon + 0xF4u);
        if (mag && ValidAddr(mag, 0x24u) && HostPtr(mag) && R32(mag + 0x04u) == stock_weapon) {
            uint8_t* dst = reinterpret_cast<uint8_t*>(HostPtr(g_guest_fire_pkt));
            std::memcpy(dst, reinterpret_cast<const uint8_t*>(HostPtr(mag)), 32);
            if (packet_ok(g_guest_fire_pkt)) {
                g_guest_fire_pkt_src = 0;
                ARSENAL_LOG_FIRST(3, "[ARSENAL] world bullet pkt cloned from player weapon+0xF4");
                out_pkt = g_guest_fire_pkt;
                return true;
            }
        }
    }
    g_guest_fire_pkt_src = 0;
    return false;
}

void SpawnWorldBullet(X86Context& ctx, const float origin[3], const float dir[3],
                      float min_range = 0.0f, float min_speed = 0.0f) {
    // A real guest physical bullet (same spawner the game's own guns use) so cars, glass, props,
    // decals and surface impacts react exactly like normal gunfire.
    if (game::time_stop::IsFrozen()) return;
    uint32_t pkt = 0;
    float range = 80.0f, speed = 160.0f;
    uint8_t flags = 0;
    if (!GuestFirePacket(pkt, range, speed, flags)) {
        ARSENAL_LOG_FIRST(3, "[ARSENAL] world bullet skipped: no player fire packet");
        return;
    }
    range = std::max(range, min_range);
    speed = std::max(speed, min_speed);
    const uint32_t mgr = R32(0x002D12D0u);
    if (!mgr || !ValidAddr(mgr + 0x6E0u, 16u * 0x90u)) {
        ARSENAL_LOG_FIRST(3, "[ARSENAL] world bullet skipped: weapon manager 0x%08X unusable", mgr);
        return;
    }

    static uint32_t s_buf = 0;
    if (!s_buf) s_buf = mem::AllocSystem(32, 16);
    if (!s_buf) return;
    float* m = reinterpret_cast<float*>(HostPtr(s_buf));
    float* d = reinterpret_cast<float*>(HostPtr(s_buf + 16));
    m[0] = origin[0]; m[1] = origin[1]; m[2] = origin[2]; m[3] = 1.0f;
    d[0] = dir[0]; d[1] = dir[1]; d[2] = dir[2]; d[3] = 0.0f;

    const uint32_t saved_ecx = ctx.ecx, saved_edx = ctx.edx;
    ctx.ecx = s_buf;
    ctx.edx = mgr;
    game::ballistics::SetSuppressPhysicalSpawnFx(true);
    CallGuest(ctx, 0x001DB7B0u, {pkt, s_buf + 16, FBits(range), FBits(speed), uint32_t(flags)});
    game::ballistics::SetSuppressPhysicalSpawnFx(false);
    ctx.ecx = saved_ecx;
    ctx.edx = saved_edx;
    ARSENAL_LOG_FIRST(4, "[ARSENAL] world bullet spawned pkt=0x%08X range=%.1f speed=%.1f flags=%u",
                      pkt, range, speed, unsigned(flags));
}

void ExecuteArsenalBulletShot(X86Context& ctx, uint8_t* base,
                              const sa::WeaponStatus& st, const sa::ArsenalStepEvents& ev,
                              const float eye[3], const float right[3], const float up[3],
                              const float fwd[3], const float muzzle[3], float ground_y) {
    // Deterministic radial spread perturbation per shot
    g_shot_rng_seq ^= (g_shot_rng_seq << 13);
    g_shot_rng_seq ^= (g_shot_rng_seq >> 17);
    g_shot_rng_seq ^= (g_shot_rng_seq << 5);
    const float r0 = float(g_shot_rng_seq & 0xFFFFu) / 65535.0f;
    const float r1 = float((g_shot_rng_seq >> 16) & 0xFFFFu) / 65535.0f;
    const float sp_rad = ev.bullet_spread_deg * kDegToRad * std::sqrt(r0);
    const float theta = r1 * 6.283185f;
    const float sx = std::cos(theta) * sp_rad;
    const float sy = std::sin(theta) * sp_rad;

    float ray[3] = {
        fwd[0] + right[0] * sx + up[0] * sy,
        fwd[1] + right[1] * sx + up[1] * sy,
        fwd[2] + right[2] * sx + up[2] * sy,
    };
    const float rl = std::sqrt(ray[0] * ray[0] + ray[1] * ray[1] + ray[2] * ray[2]);
    if (rl > 1e-5f) { ray[0] /= rl; ray[1] /= rl; ray[2] /= rl; }

    const uint32_t me = PlayerPtr();
    uint32_t enemies[64]{};
    const size_t n = game::CopyLivingEnemies(enemies, 64);

    auto find_closest_enemy_hit = [&](float min_t, uint32_t skip_who, float& out_t,
                                      uint32_t& out_who, bool& out_head,
                                      float out_hit[3], float& out_feet_y) {
        out_t = 140.0f;
        out_who = 0;
        out_head = false;
        out_feet_y = ground_y;
        for (size_t i = 0; i < n; ++i) {
            const uint32_t e = enemies[i];
            if (!e || e == me || e == skip_who || !ValidAddr(e, 0x390u)) continue;
            if (R32(e + kCharKind) != kEnemyKind || R32(e + kCharDying) != 0u) continue;
            const float ep[3] = {G<float>(e + kCharPos), G<float>(e + kCharPos + 4u),
                                 G<float>(e + kCharPos + 8u)};
            if (!std::isfinite(ep[0]) || !std::isfinite(ep[1]) || !std::isfinite(ep[2])) continue;

            const float ocx = ep[0] - eye[0];
            const float ocz = ep[2] - eye[2];
            const float a_xz = ray[0] * ray[0] + ray[2] * ray[2];
            const float t = (a_xz > 1e-5f)
                                ? ((ocx * ray[0] + ocz * ray[2]) / a_xz)
                                : ((ep[1] + 1.0f - eye[1]) / (std::fabs(ray[1]) > 1e-4f ? ray[1] : 1.0f));
            if (t <= min_t || t >= out_t) continue;

            const float px = eye[0] + ray[0] * t;
            const float py = eye[1] + ray[1] * t;
            const float pz = eye[2] + ray[2] * t;
            const float dx = px - ep[0];
            const float dz = pz - ep[2];
            const float hdist = std::sqrt(dx * dx + dz * dz);
            const float rel_y = py - ep[1];

            const bool head = (rel_y >= 1.36f && rel_y <= 1.84f && hdist <= 0.28f);
            const bool body = (!head && rel_y >= 0.04f && rel_y <= 1.78f && hdist <= 0.48f);
            if (head || body) {
                out_t = t;
                out_who = e;
                out_head = head;
                out_hit[0] = px; out_hit[1] = py; out_hit[2] = pz;
                out_feet_y = ep[1];
            }
        }
    };

    float best_t = 140.0f;
    uint32_t best_who = 0;
    bool best_head = false;
    float best_hit[3] = {};
    float best_feet_y = ground_y;
    find_closest_enemy_hit(0.35f, 0u, best_t, best_who, best_head, best_hit, best_feet_y);

    const float cal_scale = (st.id == sa::SpecialWeaponId::Mauser1891) ? 1.25f
                            : (st.id == sa::SpecialWeaponId::Maxim1910 ? 1.15f : 0.86f);

    if (st.id == sa::SpecialWeaponId::Mauser1891) {
        game::ballistics::EmitMauserMuzzleShockwave(muzzle, ray, ground_y, 1.0f);
    } else if (st.id == sa::SpecialWeaponId::MauserC96) {
        game::ballistics::EmitMauserMuzzleShockwave(muzzle, ray, ground_y, 0.48f);
    }

    auto apply_enemy_hit = [&](uint32_t who, const float hit_p[3], float feet_y, bool head, float base_dmg) {
        float& hp = G<float>(who + kCharHealth);
        const float dmg = base_dmg * (head ? 2.2f : 1.0f);
        const bool will_kill = !std::isfinite(hp) || (hp <= dmg + 0.5f);
        game::ballistics::OnEnemyBloodHit(who, hit_p, feet_y + 0.024f,
                                          std::clamp(hit_p[1] - feet_y, 0.2f, 1.65f),
                                          ray, dmg, head, will_kill);
        game::combat_feedback::RecordPlayerHit(head, will_kill);
        if (!will_kill) {
            hp -= dmg;
            game::delirium_phys::TriggerExternalAgony(ctx, base, who, head ? 0x1Fu : 0x1Eu);
        } else if (me) {
            hp = 0.001f;
            ApplyGuestDamage(ctx, base, who, me, hit_p, ray,
                             std::max(120.0f, dmg * 1.4f), head ? 0x05u : 0x04u, false);
        }
    };

    if (best_who) {
        game::ballistics::EmitSpecialShotMuzzleAndTrace(muzzle, ray, best_hit, cal_scale, true, false);
        apply_enemy_hit(best_who, best_hit, best_feet_y, best_head, ev.bullet_damage);

        // 7.65x53mm Mauser spitzer over-penetration: pierces through first enemy to strike behind!
        if (st.id == sa::SpecialWeaponId::Mauser1891) {
            float pen_t = 140.0f;
            uint32_t pen_who = 0;
            bool pen_head = false;
            float pen_hit[3] = {};
            float pen_feet_y = ground_y;
            find_closest_enemy_hit(best_t + 0.35f, best_who, pen_t, pen_who, pen_head, pen_hit, pen_feet_y);
            if (pen_who) {
                game::ballistics::EmitSpecialShotMuzzleAndTrace(best_hit, ray, pen_hit, cal_scale * 0.85f, false, false);
                apply_enemy_hit(pen_who, pen_hit, pen_feet_y, pen_head, ev.bullet_damage * 0.65f);
            }
        }

        // The shot has to reach the world behind the body as well: fired past the last man it struck,
        // the guest bullet dents the car, shatters the window and punches the wall exactly like
        // stock gunfire, which is what made "the bullet does nothing" so obvious before.
        const float min_r = (st.id == sa::SpecialWeaponId::Mauser1891) ? 160.0f : 90.0f;
        const float min_v = (st.id == sa::SpecialWeaponId::Mauser1891) ? 240.0f : 180.0f;
        const float past[3] = {best_hit[0] + ray[0] * 0.55f, best_hit[1] + ray[1] * 0.55f,
                               best_hit[2] + ray[2] * 0.55f};
        SpawnWorldBullet(ctx, past, ray, min_r, min_v);
    } else {
        float miss_t = 48.0f;
        bool hit_ground = false;
        if (ray[1] < -0.01f && eye[1] > ground_y) {
            const float tg = (ground_y - eye[1]) / ray[1];
            if (tg > 0.4f && tg < 95.0f) {
                miss_t = tg;
                hit_ground = true;
            }
        }
        const float miss_end[3] = {
            eye[0] + ray[0] * miss_t,
            eye[1] + ray[1] * miss_t,
            eye[2] + ray[2] * miss_t,
        };
        game::ballistics::EmitSpecialShotMuzzleAndTrace(muzzle, ray, miss_end, cal_scale, true,
                                                        hit_ground || miss_t < 47.0f);
        // Real world-interacting bullet (cars, glass, barrels, props, walls) on every round fired.
        const float min_r = (st.id == sa::SpecialWeaponId::Mauser1891) ? 160.0f : 90.0f;
        const float min_v = (st.id == sa::SpecialWeaponId::Mauser1891) ? 240.0f : 180.0f;
        const float start[3] = {eye[0] + ray[0] * 0.30f, eye[1] + ray[1] * 0.30f, eye[2] + ray[2] * 0.30f};
        SpawnWorldBullet(ctx, start, ray, min_r, min_v);
        if (st.id == sa::SpecialWeaponId::Mauser1891) {
            sound::weapon_audio::TriggerWorldImpact(miss_end, hit_ground && ((g_shot_rng_seq & 1u) == 0u), true);
        } else if (st.id == sa::SpecialWeaponId::MauserC96) {
            sound::weapon_audio::TriggerWorldImpact(miss_end, hit_ground && ((g_shot_rng_seq & 3u) == 0u),
                                                    (st.mag_ammo & 1) == 0);
        }
    }
}

void GetBaseOffsets(sa::SpecialWeaponId id, float ads,
                    float& ox, float& oy, float& oz,
                    float& yaw, float& pitch) noexcept {
    float hx = 0.120f, hy = -0.115f, hz = 0.180f, hyaw = -0.042f, hpitch = 0.0f;
    float ax = 0.060f, ay = -0.090f, az = 0.160f, ayaw = -0.015f, apitch = 0.0f;
    switch (id) {
        case sa::SpecialWeaponId::Flamethrower:
            hx = 0.115f; hy = -0.112f; hz = 0.165f; hyaw = -0.040f;
            ax = 0.075f; ay = -0.095f; az = 0.150f; ayaw = -0.020f;
            break;
        case sa::SpecialWeaponId::Mauser1891:
            // Framed boldly like BLACK's native AK-47: close receiver/grip/scope/bolt silhouette
            // canted inward with RightGrip & LeftForegrip both cleanly on screen.
            hx = 0.098f; hy = -0.070f; hz =  0.032f; hyaw = -0.115f; hpitch = 0.015f;
            ax = 0.000f; ay = -0.056f; az = -0.045f; ayaw =  0.000f; apitch = 0.000f;
            break;
        case sa::SpecialWeaponId::MauserC96:
            hx = 0.080f; hy = -0.080f; hz = 0.195f; hyaw = -0.036f;
            ax = 0.000f; ay = -0.076f; az = 0.175f; ayaw =  0.000f;
            break;
        case sa::SpecialWeaponId::Maxim1910:
            hx = 0.110f; hy = -0.175f; hz = 0.490f; hyaw = -0.050f;
            ax = 0.035f; ay = -0.125f; az = 0.440f; ayaw = -0.015f;
            break;
        default: break;
    }
    ox = hx + (ax - hx) * ads;
    oy = hy + (ay - hy) * ads;
    oz = hz + (az - hz) * ads;
    yaw = hyaw + (ayaw - hyaw) * ads;
    pitch = hpitch + (apitch - hpitch) * ads;
}

}  // namespace

void EnsureLoaded() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    for (size_t i = 0; i < sa::kWeaponCount; ++i) {
        LoadSingleAssetLocked(i);
    }
}

bool IsActive() {
    return g_want_active.load(std::memory_order_relaxed);
}

bool IsScopedOut() {
    return g_hud_scoped_out.load(std::memory_order_relaxed);
}

bool ShouldHideBlackArms() {
    return (g_hud_weapon_id.load(std::memory_order_relaxed) == uint8_t(sa::SpecialWeaponId::Mauser1891)) &&
           g_m91_hands_ready.load(std::memory_order_acquire);
}

void SetActive(bool enable) {
    if (enable) EnsureLoaded();
    bool play_m91_equip = false;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        const bool prev = g_want_active.load(std::memory_order_relaxed);
        g_want_active.store(enable, std::memory_order_relaxed);
        if (enable && !prev) {
            g_flame_sim.SetActive(true, 0);
            play_m91_equip = (g_arsenal.ActiveWeapon() == sa::SpecialWeaponId::Mauser1891);
        } else if (!enable && prev) {
            g_flame_sim.DeactivateAndRestoreSlot();
            g_trigger_held.store(false, std::memory_order_relaxed);
            g_fire_pressed_q.store(false, std::memory_order_relaxed);
            g_reload_queued.store(false, std::memory_order_relaxed);
            g_select_fire_q.store(false, std::memory_order_relaxed);
            g_zoom_cycle_q.store(false, std::memory_order_relaxed);
            g_aim_pad_held.store(false, std::memory_order_relaxed);
            g_aim_touch_held.store(false, std::memory_order_relaxed);
            g_hud_scoped_out.store(false, std::memory_order_relaxed);
            g_nozzle_valid = false;
        }
        PublishHudLocked(g_arsenal.GetStatus(), g_flame_sim.Pressure());
    }
    if (play_m91_equip) {
        sound::weapon_audio::TriggerSpecialSlot(153, 0.88f, false, true);
    }
}

void Toggle() {
    SetActive(!IsActive());
}

void ForceDeactivate() {
    if (IsActive()) SetActive(false);
}

void RefillFuel() {
    g_refill_queued.store(true, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_flame_sim.RefillFuel();
    g_arsenal.RefillActive();
    PublishHudLocked(g_arsenal.GetStatus(), g_flame_sim.Pressure());
}

void ResetLevelState() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_want_active.store(false, std::memory_order_relaxed);
    g_trigger_held.store(false, std::memory_order_relaxed);
    g_fire_pressed_q.store(false, std::memory_order_relaxed);
    g_reload_queued.store(false, std::memory_order_relaxed);
    g_select_fire_q.store(false, std::memory_order_relaxed);
    g_zoom_cycle_q.store(false, std::memory_order_relaxed);
    g_aim_pad_held.store(false, std::memory_order_relaxed);
    g_aim_touch_held.store(false, std::memory_order_relaxed);
    g_hud_scoped_out.store(false, std::memory_order_relaxed);
    g_flame_sim.Reset();
    g_arsenal.ResetAll();
    g_nozzle_valid = false;
    g_prev_eye_valid = false;
    g_burning_count_frame.store(0, std::memory_order_relaxed);
    g_hud_burning.store(0, std::memory_order_relaxed);
    PublishHudLocked(g_arsenal.GetStatus(), 0.0f);
}

void SelectSpecialWeapon(uint8_t index) {
    if (index >= sa::kWeaponCount) index = 0;
    EnsureLoaded();
    bool play_m91_equip = false;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        const auto target = static_cast<sa::SpecialWeaponId>(index);
        const bool switched = (g_arsenal.ActiveWeapon() != target);
        g_arsenal.SelectWeapon(target);
        PublishHudLocked(g_arsenal.GetStatus(), g_flame_sim.Pressure());
        play_m91_equip = switched && (target == sa::SpecialWeaponId::Mauser1891);
    }
    if (!IsActive()) {
        SetActive(true);
    } else if (play_m91_equip) {
        sound::weapon_audio::TriggerSpecialSlot(153, 0.88f, false, true);
    }
}

void CycleSpecialWeapon(int dir) {
    EnsureLoaded();
    bool play_m91_equip = false;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        const int cur = int(g_arsenal.ActiveWeapon());
        const int next = ((cur + (dir >= 0 ? 1 : -1)) % int(sa::kWeaponCount) + int(sa::kWeaponCount)) %
                         int(sa::kWeaponCount);
        const auto target = static_cast<sa::SpecialWeaponId>(next);
        g_arsenal.SelectWeapon(target);
        PublishHudLocked(g_arsenal.GetStatus(), g_flame_sim.Pressure());
        play_m91_equip = (target == sa::SpecialWeaponId::Mauser1891);
    }
    if (play_m91_equip && IsActive()) {
        sound::weapon_audio::TriggerSpecialSlot(153, 0.88f, false, true);
    }
}

void ToggleFireMode() {
    if (g_hud_weapon_id.load(std::memory_order_relaxed) == uint8_t(sa::SpecialWeaponId::Mauser1891)) {
        g_zoom_cycle_q.store(true, std::memory_order_relaxed);
    } else {
        g_select_fire_q.store(true, std::memory_order_relaxed);
    }
}

void CycleScopeZoom() {
    g_zoom_cycle_q.store(true, std::memory_order_relaxed);
}

void TriggerManualReload() {
    g_reload_queued.store(true, std::memory_order_relaxed);
}

void SetSpecialAiming(bool aiming) {
    g_aim_touch_held.store(aiming, std::memory_order_relaxed);
}

float GetAdsFovScale() {
    if (!IsActive()) return 1.0f;
    return sa::ResolveZoomMagnification(g_hud_fov_scale.load(std::memory_order_relaxed));
}

const char* GetSpecialWeaponName(uint8_t index) {
    if (index >= sa::kWeaponCount) return kAssetSpecs[0].name;
    return kAssetSpecs[index].name;
}

void FilterGameplayPad(int port, input::Pad& pad) {
    if (port != 0) return;

    const uint32_t world = mem::IsCommitted(kWorld) ? R32(kWorld) : 0u;
    const uint32_t me = PlayerPtr();
    const bool in_play =
        world && ValidAddr(world + kWorldState, 4u) && R32(world + kWorldState) == kStatePlaying &&
        me && ValidAddr(me + kPlayerInventory, 0x50u) && !R32(me + kCharDying) && !app::MenuOpen();

    const bool up_now = (pad.buttons & (1u << 3)) != 0;
    const bool dn_now = (pad.buttons & (1u << 2)) != 0;
    const bool up_edge = up_now && !g_prev_swap_up;
    const bool dn_edge = dn_now && !g_prev_swap_dn;
    g_prev_swap_up = up_now;
    g_prev_swap_dn = dn_now;
    if (!up_now) g_swallow_up = false;
    if (!dn_now) g_swallow_dn = false;

    if (in_play) {
        const uint8_t count = G<uint8_t>(me + kPlayerInventory + kInvSlotCount);
        const uint8_t held = G<uint8_t>(me + kPlayerInventory + kInvSlotHeld);
        if (!IsActive()) {
            if (up_edge && (count <= 1u || held + 1u >= count)) {
                SelectSpecialWeapon(0u);
                g_swallow_up = true;
            } else if (dn_edge && held == 0u) {
                SelectSpecialWeapon(uint8_t(sa::kWeaponCount - 1u));
                g_swallow_dn = true;
            }
        } else {
            const uint8_t wid = g_hud_weapon_id.load(std::memory_order_relaxed);
            if (up_edge) {
                g_swallow_up = true;
                if (wid + 1u < uint8_t(sa::kWeaponCount)) {
                    SelectSpecialWeapon(wid + 1u);
                } else {
                    SetActive(false);
                    if (count >= 2u && held != 0u) {
                        W32(me + kPlayerInventory + kInvRequest, 2u);
                    }
                }
            } else if (dn_edge) {
                g_swallow_dn = true;
                if (wid > 0u) {
                    SelectSpecialWeapon(wid - 1u);
                } else {
                    SetActive(false);
                }
            }
        }
    } else {
        g_swallow_up = false;
        g_swallow_dn = false;
    }

    if (g_swallow_up) pad.buttons &= uint16_t(~(1u << 3));
    if (g_swallow_dn) pad.buttons &= uint16_t(~(1u << 2));

    if (!IsActive() || app::MenuOpen()) {
        g_trigger_held.store(false, std::memory_order_relaxed);
        g_aim_pad_held.store(false, std::memory_order_relaxed);
        g_prev_fire_trig = false;
        g_prev_aim_btn = (pad.buttons & (1u << 7)) != 0;
        g_prev_mode_btn = (pad.buttons & (1u << 0)) != 0;
        g_swallow_mode = false;
        return;
    }

    const bool trig = (pad.analog[7] > 0x20);
    const bool trig_edge = trig && !g_prev_fire_trig;
    g_prev_fire_trig = trig;
    if (trig_edge) g_fire_pressed_q.store(true, std::memory_order_relaxed);

    const bool rel = (pad.analog[0] > 0x20);
    g_trigger_held.store(trig, std::memory_order_relaxed);
    if (rel) g_reload_queued.store(true, std::memory_order_relaxed);

    const bool aim_btn = (pad.buttons & (1u << 7)) != 0;
    const bool aim_edge = aim_btn && !g_prev_aim_btn;
    g_prev_aim_btn = aim_btn;
    // Tap-to-toggle ADS: each tap of the aim button flips the aim state (no need to keep holding it)
    if (aim_edge) {
        g_aim_pad_held.store(!g_aim_pad_held.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    const bool mode_now = (pad.buttons & (1u << 0)) != 0;
    const bool mode_edge = mode_now && !g_prev_mode_btn;
    g_prev_mode_btn = mode_now;
    if (!mode_now) g_swallow_mode = false;
    const uint8_t cur_wid = g_hud_weapon_id.load(std::memory_order_relaxed);
    if (mode_edge && cur_wid == uint8_t(sa::SpecialWeaponId::MauserC96)) {
        g_select_fire_q.store(true, std::memory_order_relaxed);
        g_swallow_mode = true;
    } else if (mode_edge && cur_wid == uint8_t(sa::SpecialWeaponId::Mauser1891) &&
               g_hud_ads_blend.load(std::memory_order_relaxed) >= 0.45f) {
        g_zoom_cycle_q.store(true, std::memory_order_relaxed);
        g_swallow_mode = true;
    }
    if (g_swallow_mode) pad.buttons &= uint16_t(~(1u << 0));

    // Suppress underlying weapon fire, reload & ADS toggle while Special Arsenal owns combat input.
    pad.analog[7] = 0;
    pad.analog[0] = 0;
    pad.buttons &= uint16_t(~(1u << 7));
}

void RenderAndStepFrame(X86Context& ctx, uint8_t* base) {
    if (!IsActive()) return;
    EnsureLoaded();

    game::ballistics::CaptureSceneCamera();
    float eye[3], right[3], up[3], fwd[3], view[16], scene_proj[16];
    if (!game::ballistics::GetSceneCameraVectors(eye, right, up, fwd, view, scene_proj)) return;

    auto now = std::chrono::steady_clock::now();
    float dt = 0.016f;
    if (g_last_step_tp.time_since_epoch().count() != 0) {
        dt = std::clamp(std::chrono::duration<float>(now - g_last_step_tp).count(), 0.001f, 0.05f);
    }
    g_last_step_tp = now;

    float player_speed_mps = 0.0f;
    if (g_prev_eye_valid && dt > 1e-4f) {
        const float dx = eye[0] - g_prev_eye[0];
        const float dz = eye[2] - g_prev_eye[2];
        player_speed_mps = std::sqrt(dx * dx + dz * dz) / dt;
        g_walk_phase += dt * (1.8f + std::min(player_speed_mps, 7.0f) * 1.45f);
    } else {
        g_walk_phase += dt * 1.8f;
    }
    std::memcpy(g_prev_eye, eye, sizeof(g_prev_eye));
    g_prev_eye_valid = true;

    float ground_y = eye[1] - 1.55f;
    if (uint32_t me = PlayerPtr(); me && ValidAddr(me + kCharPos, 12u)) {
        const float fy = G<float>(me + kCharPos + 4u);
        if (std::isfinite(fy)) ground_y = fy + 0.02f;
    }

    const bool trig = g_trigger_held.load(std::memory_order_relaxed) && !app::MenuOpen();
    const bool fire_edge = g_fire_pressed_q.exchange(false, std::memory_order_relaxed) && !app::MenuOpen();
    const bool rel = g_reload_queued.exchange(false, std::memory_order_relaxed);
    const bool refill = g_refill_queued.exchange(false, std::memory_order_relaxed);
    const bool sel_mode = g_select_fire_q.exchange(false, std::memory_order_relaxed);
    const bool zoom_cycle = g_zoom_cycle_q.exchange(false, std::memory_order_relaxed);
    const bool ads_held =
        (g_aim_pad_held.load(std::memory_order_relaxed) || g_aim_touch_held.load(std::memory_order_relaxed)) &&
        !app::MenuOpen();
    const bool inf_ammo = game::Opt().infinite_ammo;

    sa::ArsenalStepEvents ev{};
    sa::WeaponStatus st{};
    float flame_pressure = 0.0f, equip_t = 1.0f, flame_rel_t = 0.0f;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (refill) {
            g_arsenal.RefillActive();
            g_flame_sim.RefillFuel();
        }
        sa::ArsenalInput in{};
        in.dt = dt;
        in.equipped = true;
        in.fire_held = trig;
        in.fire_pressed = fire_edge;
        in.reload_pressed = rel;
        in.ads_held = ads_held;
        in.select_fire_pressed = sel_mode;
        in.zoom_cycle_pressed = zoom_cycle;
        in.player_speed_mps = player_speed_mps;
        in.equip_progress = g_flame_sim.EquipProgress();
        ev = g_arsenal.Step(in);
        st = g_arsenal.GetStatus();

        // Reloading drops the toggled aim so the weapon comes back to the hip like a real reload
        if (st.reloading) g_aim_pad_held.store(false, std::memory_order_relaxed);

        if (inf_ammo) {
            g_arsenal.RefillActive();
            st = g_arsenal.GetStatus();
        }

        const bool flame_jet_on =
            (st.id == sa::SpecialWeaponId::Flamethrower) && ev.fire_flame_tick;
        const bool flame_rel =
            (st.id == sa::SpecialWeaponId::Flamethrower) && st.reloading;
        g_flame_sim.Step(dt, flame_jet_on, flame_rel, true);
        flame_pressure = g_flame_sim.Pressure();
        equip_t = g_flame_sim.EquipProgress();
        flame_rel_t = g_flame_sim.ReloadProgress();
        PublishHudLocked(st, flame_pressure);
    }

    DispatchArsenalAudioCues(ev);

    if (st.id == sa::SpecialWeaponId::Maxim1910 && player_speed_mps > 1.15f) {
        g_maxim_wheel_sfx_accum += dt * player_speed_mps;
        if (g_maxim_wheel_sfx_accum >= 2.4f) {
            g_maxim_wheel_sfx_accum = 0.0f;
            sound::weapon_audio::TriggerSpecialSlot(150, 0.42f, false);
        }
    } else {
        g_maxim_wheel_sfx_accum = 0.0f;
    }

    if (ev.fire_bullet || ev.fire_flame_tick) {
        game::haptics::DispatchWeaponHaptic(st.code, false, false, false);
    }
    if (ev.eject_shell) {
        game::ballistics::OnPlayerShellEject(st.code, false);
    }

    // Damped spring recovery for viewmodel recoil kick
    g_recoil_pitch_cur = std::clamp(g_recoil_pitch_cur * std::exp(-dt * 12.0f) + ev.recoil_pitch_deg * kDegToRad,
                                    -0.32f, 0.32f);
    g_recoil_yaw_cur = std::clamp(g_recoil_yaw_cur * std::exp(-dt * 14.0f) + ev.recoil_yaw_deg * kDegToRad,
                                  -0.18f, 0.18f);
    g_recoil_roll_cur = std::clamp(g_recoil_roll_cur * std::exp(-dt * 14.0f) + ev.recoil_roll_deg * kDegToRad,
                                   -0.25f, 0.25f);
    if (ev.fire_bullet) {
        g_recoil_kick_z = (st.id == sa::SpecialWeaponId::Maxim1910) ? -0.022f : -0.028f;
    } else {
        g_recoil_kick_z *= std::exp(-dt * 16.0f);
    }

    g_hud_burning.store(g_burning_count_frame.exchange(0, std::memory_order_relaxed),
                        std::memory_order_relaxed);

    const float ease_draw = 1.0f - std::pow(1.0f - equip_t, 3.0f);
    const float draw_y = (1.0f - ease_draw) * -0.26f;
    const float draw_pitch = (1.0f - ease_draw) * 0.42f;

    float reload_x = 0.0f, reload_y = 0.0f, reload_z = 0.0f;
    float reload_pitch = 0.0f, reload_yaw = 0.0f, reload_roll = 0.0f;
    if (st.reloading) {
        const float rel_p = (st.id == sa::SpecialWeaponId::Flamethrower)
                                ? flame_rel_t
                                : std::max(st.pose.clip_insert, st.pose.top_cover_deg / 85.0f);
        EvalReloadPose(st.id, rel_p, st.pose.clip_insert, st.pose.top_cover_deg,
                       reload_x, reload_y, reload_z, reload_pitch, reload_yaw, reload_roll);
    }

    const float ads_damp = 1.0f - 0.82f * st.ads_blend;
    const float sway_x = std::sin(g_walk_phase) * 0.0052f * ads_damp;
    const float sway_y = std::cos(g_walk_phase * 2.0f) * 0.0036f * ads_damp;
    const bool flm_firing = (st.id == sa::SpecialWeaponId::Flamethrower) &&
                            (st.phase == sa::ActionPhase::Firing);
    const float jitter_x = flm_firing ? std::sin(g_walk_phase * 29.0f) * 0.0018f * flame_pressure : 0.0f;
    const float jitter_y = flm_firing ? std::cos(g_walk_phase * 37.0f) * 0.0018f * flame_pressure : 0.0f;

    float base_ox = 0.0f, base_oy = 0.0f, base_oz = 0.0f, base_yaw = 0.0f, base_pitch = 0.0f;
    GetBaseOffsets(st.id, st.ads_blend, base_ox, base_oy, base_oz, base_yaw, base_pitch);

    const float yaw = base_yaw + reload_yaw + g_recoil_yaw_cur * ads_damp;
    const float pitch = base_pitch + draw_pitch + reload_pitch - g_recoil_pitch_cur -
                        ((st.id == sa::SpecialWeaponId::Flamethrower) ? flame_pressure * 0.028f : 0.0f);
    const float hip_cant = (st.id == sa::SpecialWeaponId::Mauser1891) ? (-0.045f * (1.0f - st.ads_blend)) : 0.0f;
    const float roll = hip_cant + reload_roll + g_recoil_roll_cur * ads_damp;

    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll), sr = std::sin(roll);

    const float lx[3] = {cy * cr + sy * sp * sr, cp * sr, -sy * cr + cy * sp * sr};
    const float ly[3] = {-cy * sr + sy * sp * cr, cp * cr, sy * sr + cy * sp * cr};
    const float lz[3] = {sy * cp, -sp, cy * cp};

    auto to_world_dir = [&](const float c[3], float out[3]) {
        out[0] = right[0] * c[0] + up[0] * c[1] + fwd[0] * c[2];
        out[1] = right[1] * c[0] + up[1] * c[1] + fwd[1] * c[2];
        out[2] = right[2] * c[0] + up[2] * c[1] + fwd[2] * c[2];
    };

    float world[16] = {};
    float w_x[3], w_y[3], w_z[3];
    to_world_dir(lx, w_x);
    to_world_dir(ly, w_y);
    to_world_dir(lz, w_z);

    const float vm_scale = (st.id == sa::SpecialWeaponId::Mauser1891) ? 1.12f : 1.0f;
    for (int k = 0; k < 3; ++k) {
        w_x[k] *= vm_scale;
        w_y[k] *= vm_scale;
        w_z[k] *= vm_scale;
    }

    const float ox = base_ox + reload_x + sway_x + jitter_x;
    const float oy = base_oy + draw_y + reload_y + sway_y + jitter_y;
    const float oz = base_oz + reload_z + g_recoil_kick_z -
                     ((st.id == sa::SpecialWeaponId::Flamethrower) ? flame_pressure * 0.020f : 0.0f);
    const float org[3] = {
        eye[0] + right[0] * ox + up[0] * oy + fwd[0] * oz,
        eye[1] + right[1] * ox + up[1] * oy + fwd[1] * oz,
        eye[2] + right[2] * ox + up[2] * oy + fwd[2] * oz
    };

    world[0] = w_x[0]; world[1] = w_x[1]; world[2] = w_x[2]; world[3] = 0.0f;
    world[4] = w_y[0]; world[5] = w_y[1]; world[6] = w_y[2]; world[7] = 0.0f;
    world[8] = w_z[0]; world[9] = w_z[1]; world[10] = w_z[2]; world[11] = 0.0f;
    world[12] = org[0]; world[13] = org[1]; world[14] = org[2]; world[15] = 1.0f;

    WeaponAsset& asset = g_assets[size_t(st.id)];
    float pilot_world[3] = {};
    if (asset.ready) {
        TransformPoint(world, asset.model.nozzle_pos, g_nozzle_world);
        TransformPoint(world, asset.model.pilot_pos, pilot_world);
    } else {
        g_nozzle_world[0] = org[0] + fwd[0] * 0.45f;
        g_nozzle_world[1] = org[1] + fwd[1] * 0.45f;
        g_nozzle_world[2] = org[2] + fwd[2] * 0.45f;
        std::memcpy(pilot_world, g_nozzle_world, sizeof(pilot_world));
    }
    g_jet_dir[0] = fwd[0]; g_jet_dir[1] = fwd[1]; g_jet_dir[2] = fwd[2];
    g_nozzle_valid = true;

    if (ev.fire_bullet) {
        ExecuteArsenalBulletShot(ctx, base, st, ev, eye, right, up, fwd, g_nozzle_world, ground_y);
    }
    if (ev.steam_plume && asset.ready) {
        game::ballistics::EmitMaximSteamPlume(pilot_world, std::max(0.4f, st.pose.steam_vent));
    }

    const float aspect = (std::fabs(scene_proj[0]) > 0.05f && std::fabs(scene_proj[5]) > 0.05f)
                             ? std::fabs(scene_proj[5] / scene_proj[0])
                             : g_last_cam_aspect;
    g_last_cam_aspect = aspect;
    // Match BLACK's native viewmodel vertical FOV (~40 deg for imposing Kar98k framing, ~50.6 deg for others)
    const float vm_fov_deg = (st.id == sa::SpecialWeaponId::Mauser1891)
                                 ? std::clamp(float(game::Opt().viewmodel_fov) - 8.5f, 38.0f, 43.0f)
                                 : std::clamp(float(game::Opt().viewmodel_fov), 45.0f, 56.0f);
    float vm_proj[16];
    BuildPerspectiveD3D8LH(vm_fov_deg * kDegToRad, aspect, 0.04f, 12.0f, vm_proj);

    const float flicker = 0.85f + 0.15f * std::sin(g_walk_phase * 23.0f);
    float light_intensity = 0.0f;
    if (st.id == sa::SpecialWeaponId::Flamethrower && !st.reloading) {
        light_intensity = 0.10f * flicker + flame_pressure * 1.25f * flicker;
    } else if (ev.fire_bullet) {
        light_intensity = 1.45f;
    }

    // Scoped sight picture: once the eye is fully behind the optic the overlay in
    // scope_overlay.cpp *is* the view -- like every scoped weapon in the base
    // game, the gun and arms leave the frame instead of floating around under a
    // floating tube. Below that the viewmodel keeps rendering so the raise reads.
    const bool scoped_out = (st.id == sa::SpecialWeaponId::Mauser1891) && st.ads_blend >= 0.86f;

    if (!scoped_out && asset.ready && !asset.work_verts.empty()) {
        ApplyWeaponPartKinematics(asset, st, g_walk_phase, g_recoil_kick_z);
        if (st.id == sa::SpecialWeaponId::Mauser1891 && g_m91_hd.ready) {
            const bool show_clip = st.reloading && (st.pose.clip_insert > 0.01f || st.pose.clip_eject > 0.01f);
            std::vector<d3d::ViewmodelTexturedBatch> draw_batches;
            draw_batches.reserve(g_m91_hd.batches.size());
            for (const auto& b : g_m91_hd.batches) {
                if (b.part_idx < asset.model.parts.size() &&
                    std::strcmp(asset.model.parts[b.part_idx].name, "stripper_clip") == 0 &&
                    !show_clip) {
                    continue;
                }
                const MatTexSlot& tex = g_m91_hd.mats[b.mat_id];
                if (!tex.ready || b.index_count < 3u) continue;
                draw_batches.push_back({
                    asset.model.indices.data() + b.first_index, b.index_count,
                    tex.hdr.cache_key, tex.swizzled_bgra,
                    tex.hdr.width, tex.hdr.height});
            }
            if (!draw_batches.empty()) {
                d3d::DrawViewmodelTexturedBatches(
                    asset.work_verts.data(), uint32_t(asset.work_verts.size()),
                    draw_batches.data(), uint32_t(draw_batches.size()),
                    world, view, vm_proj, pilot_world, light_intensity);
            }
        } else {
            d3d::DrawViewmodelTexturedMesh(
                asset.work_verts.data(), uint32_t(asset.work_verts.size()),
                asset.model.indices.data(), uint32_t(asset.model.indices.size()),
                asset.btex_hdr.cache_key, asset.swizzled_bgra,
                asset.btex_hdr.width, asset.btex_hdr.height,
                world, view, vm_proj, pilot_world, light_intensity);
        }

        if (st.id == sa::SpecialWeaponId::Mauser1891 && g_m91_hands.ready) {
            const uint8_t target_clip = st.pose.hand_clip_id;
            const float target_time = st.pose.hand_clip_time_s;
            if (target_clip != g_m91_hands.cur_clip) {
                g_m91_hands.prev_clip = g_m91_hands.cur_clip;
                g_m91_hands.cur_clip = target_clip;
                g_m91_hands.blend = 0.0f;
            }
            g_m91_hands.blend = std::min(1.0f, g_m91_hands.blend + dt / 0.075f);
            if (g_m91_hands.skinner.SkinVerticesBlended(
                    g_m91_hands.prev_clip, g_m91_hands.prev_time,
                    g_m91_hands.cur_clip, target_time,
                    g_m91_hands.blend, st.pose.trigger_pull, g_m91_hands.work_verts) &&
                !g_m91_hands.work_verts.empty()) {
                d3d::DrawViewmodelTexturedMesh(
                    g_m91_hands.work_verts.data(), uint32_t(g_m91_hands.work_verts.size()),
                    g_m91_hands.skinner.Indices().data(), uint32_t(g_m91_hands.skinner.Indices().size()),
                    g_m91_hands.tex.hdr.cache_key, g_m91_hands.tex.swizzled_bgra,
                    g_m91_hands.tex.hdr.width, g_m91_hands.tex.hdr.height,
                    world, view, vm_proj, pilot_world, light_intensity);
            }
            if (g_m91_hands.blend >= 0.999f) {
                g_m91_hands.prev_time = target_time;
            }
        }
    }

    if (st.id == sa::SpecialWeaponId::Flamethrower && !st.reloading) {
        game::ballistics::EmitPilotFlameGlow(pilot_world, flicker * (0.55f + 0.45f * flame_pressure));
    }

    // Publish M2 napalm stream & floor pools cleanly even when switching momentarily
    g_flame_sim.SetMuzzle(g_nozzle_world, fwd, ground_y);
    {
        static JetSegment s_jet[CoreSim::kMaxJetSegments];
        static JetSegment s_pool[CoreSim::kMaxPoolSegments];
        static game::ballistics::FlameStreamPoint s_fx_jet[CoreSim::kMaxJetSegments];
        static game::ballistics::FlameStreamPoint s_fx_pool[CoreSim::kMaxPoolSegments];
        size_t jn = 0, pn = 0;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            jn = g_flame_sim.CopyJetSegments(s_jet, CoreSim::kMaxJetSegments);
            pn = g_flame_sim.CopyPoolSegments(s_pool, CoreSim::kMaxPoolSegments);
        }
        for (size_t i = 0; i < jn; ++i) {
            s_fx_jet[i] = {{s_jet[i].pos[0], s_jet[i].pos[1], s_jet[i].pos[2]},
                           {s_jet[i].vel[0], s_jet[i].vel[1], s_jet[i].vel[2]},
                           s_jet[i].radius, s_jet[i].heat};
        }
        for (size_t i = 0; i < pn; ++i) {
            s_fx_pool[i] = {{s_pool[i].pos[0], s_pool[i].pos[1], s_pool[i].pos[2]},
                            {0.0f, 0.0f, 0.0f},
                            s_pool[i].radius, s_pool[i].heat};
        }
        game::ballistics::SetFlameStream(s_fx_jet, uint32_t(jn), s_fx_pool, uint32_t(pn), ground_y);
    }
}

void OnActorStep(X86Context& ctx, uint8_t* base, uint32_t who, float dt) {
    if (!who || !ValidAddr(who, 0x390u)) return;
    if (R32(who + kCharKind) != kEnemyKind) return;
    const uint32_t me = PlayerPtr();
    if (!me || who == me) return;

    const float cdt = std::isfinite(dt) ? std::clamp(dt, 0.001f, 0.05f) : 0.016f;
    const bool is_dying = (R32(who + kCharDying) != 0u);
    const float feet[3] = {
        G<float>(who + kCharPos),
        G<float>(who + kCharPos + 4u),
        G<float>(who + kCharPos + 8u)
    };
    if (!std::isfinite(feet[0]) || !std::isfinite(feet[1]) || !std::isfinite(feet[2])) return;

    const float chest[3] = {feet[0], feet[1] + 1.05f, feet[2]};
    const float boots[3] = {feet[0], feet[1] + 0.22f, feet[2]};

    VictimBurnTick tick{};
    bool active_burning = false;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (!is_dying && g_flame_sim.HasActiveJet()) {
            const float chest_ex = g_flame_sim.StreamExposure(chest, 0.42f);
            const float boots_ex = g_flame_sim.StreamExposure(boots, 0.30f);
            const float exposure = std::max(chest_ex, boots_ex);
            if (exposure > 0.02f) {
                g_flame_sim.ExposeEnemy(who, exposure, cdt);
            }
        }
        tick = g_flame_sim.TickVictim(who, cdt, is_dying);
        active_burning = g_flame_sim.GetVictimState(who).ignited;
    }

    if (active_burning && !is_dying) {
        g_burning_count_frame.fetch_add(1, std::memory_order_relaxed);
    }
    if (tick.emit_fire || tick.emit_smoke) {
        game::ballistics::EmitBurnVictimFlames(feet, feet[1] + 0.02f, tick.fire_intensity,
                                               tick.emit_fire, tick.emit_smoke, is_dying);
    }
    if (!is_dying && tick.agony_flinch) {
        game::delirium_phys::TriggerExternalAgony(ctx, base, who, tick.agony_clip);
        const float jitter_ang = float((who ^ uint32_t(tick.char_factor * 100.0f)) & 0xFFu) * 0.0245f;
        G<float>(who + kCharPos) += std::cos(jitter_ang) * 0.95f * cdt;
        G<float>(who + kCharPos + 8u) += std::sin(jitter_ang) * 0.95f * cdt;
    }
    if (!is_dying && tick.damage > 0.001f) {
        float& hp = G<float>(who + kCharHealth);
        if (std::isfinite(hp) && hp > 0.0f) {
            if (hp > tick.damage + 0.5f) {
                hp -= tick.damage;
            } else {
                hp = 0.001f;
                const float aim_pos[3] = {feet[0], feet[1] + 1.05f, feet[2]};
                ApplyGuestDamage(ctx, base, who, me, aim_pos, g_jet_dir, 120.0f, 0x07u, true);
            }
        }
    }
}

void RenderAudioMix(float* out, uint32_t frames, uint32_t sample_rate,
                    uint32_t out_channels, uint32_t ch_left, uint32_t ch_right) {
    if (!out || frames == 0 || out_channels < 2) return;
    if (!IsActive() || g_hud_weapon_id.load(std::memory_order_relaxed) != 0u) return;

    constexpr uint32_t kChunk = 256;
    float tmp[kChunk * 2];
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = std::min(kChunk, frames - done);
        std::memset(tmp, 0, size_t(n) * 2 * sizeof(float));
        g_flame_sim.RenderAudio(tmp, n, sample_rate);
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t dst = (done + i) * out_channels;
            // Layer subtle procedural flame crackle underneath BWAR v3 CC0 studio recordings
            out[dst + ch_left] = std::clamp(out[dst + ch_left] + tmp[i * 2 + 0] * 0.42f, -1.0f, 1.0f);
            out[dst + ch_right] = std::clamp(out[dst + ch_right] + tmp[i * 2 + 1] * 0.42f, -1.0f, 1.0f);
        }
        done += n;
    }
}

HudState GetHudState() {
    HudState s;
    const uint8_t wid = std::min<uint8_t>(g_hud_weapon_id.load(std::memory_order_relaxed),
                                          uint8_t(sa::kWeaponCount - 1u));
    s.active = g_hud_active.load(std::memory_order_relaxed);
    s.firing = g_hud_firing.load(std::memory_order_relaxed);
    s.reloading = g_hud_reloading.load(std::memory_order_relaxed);
    s.fuel_pct = g_hud_fuel.load(std::memory_order_relaxed);
    s.reload_progress = g_hud_reload_t.load(std::memory_order_relaxed);
    s.pressure = g_hud_pressure.load(std::memory_order_relaxed);
    s.active_burning_enemies = g_hud_burning.load(std::memory_order_relaxed);

    s.weapon_id = wid;
    s.code = kAssetSpecs[wid].code;
    s.display_name = kAssetSpecs[wid].name;
    s.mag_ammo = g_hud_mag.load(std::memory_order_relaxed);
    s.max_mag_ammo = g_hud_max_mag.load(std::memory_order_relaxed);
    s.reserve_ammo = g_hud_reserve.load(std::memory_order_relaxed);
    s.ignition_cartridges = g_hud_squibs.load(std::memory_order_relaxed);
    s.water_temp_c = g_hud_water_c.load(std::memory_order_relaxed);
    s.water_boiling = g_hud_water_boil.load(std::memory_order_relaxed);
    s.full_auto_mode = g_hud_full_auto.load(std::memory_order_relaxed);
    s.bolt_locked_open = g_hud_bolt_open.load(std::memory_order_relaxed);
    s.ads_blend = g_hud_ads_blend.load(std::memory_order_relaxed);
    s.scope_zoom_x = g_hud_scope_zoom_x.load(std::memory_order_relaxed);
    s.scope_zero_m = g_hud_scope_zero_m.load(std::memory_order_relaxed);
    return s;
}

}  // namespace game::flamethrower
