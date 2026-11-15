#include "game/flamethrower.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include "app/app.h"
#include "common/menu.h"
#include "common/options.h"
#include "core/input.h"
#include "d3d/world_overlay.h"
#include "game/ballistics_fx.h"
#include "game/delirium_physics.h"
#include "game/flamethrower_core.h"
#include "game/layout.h"
#include "kernel/kernel.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace game::flamethrower {

using namespace kernel;

namespace {

constexpr uint32_t kWorld = ::layout::kWorld;
constexpr uint32_t kWorldState = 0x5AE0u;
constexpr uint32_t kStatePlaying = 0x1Cu;
constexpr uint32_t kWorldPlayer = ::layout::kWorldPlayer;
constexpr uint32_t kPlayerInventory = 0x278u;
constexpr uint32_t kInvRequest = 0x38u;
constexpr uint32_t kInvSlotCount = 0x42u;
constexpr uint32_t kInvSlotHeld = 0x43u;
constexpr uint32_t kCharKind = 0xC4u;
constexpr uint32_t kEnemyKind = 1u;
constexpr uint32_t kCharPos = 0xA0u;
constexpr uint32_t kCharHealth = 0x2F0u;
constexpr uint32_t kCharDying = 0x384u;
constexpr float kPi = 3.14159265f;

CoreSim g_sim;
std::mutex g_state_mutex;

bool g_mesh_attempted = false;
bool g_mesh_ready = false;
BmshMesh g_raw_mesh;
std::vector<d3d::ViewmodelVertex> g_vm_verts;

std::atomic<bool> g_want_active{false};
std::atomic<bool> g_trigger_held{false};
std::atomic<bool> g_reload_queued{false};
std::atomic<bool> g_refill_queued{false};
std::atomic<uint32_t> g_burning_count_frame{0};

std::atomic<bool> g_hud_active{false};
std::atomic<bool> g_hud_firing{false};
std::atomic<bool> g_hud_reloading{false};
std::atomic<float> g_hud_fuel{100.0f};
std::atomic<float> g_hud_reload_t{0.0f};
std::atomic<float> g_hud_pressure{0.0f};
std::atomic<uint32_t> g_hud_burning{0};

// Last computed world camera/nozzle pose & aim direction (guest thread owned)
float g_nozzle_world[3] = {};
float g_jet_dir[3] = {0.0f, 0.0f, 1.0f};
bool g_nozzle_valid = false;
std::chrono::steady_clock::time_point g_last_step_tp{};
float g_walk_phase = 0.0f;
float g_prev_eye[3] = {};
bool g_prev_eye_valid = false;
float g_last_cam_aspect = 16.0f / 9.0f;

// Weapon switch cycle edge state (input thread owned via FilterGameplayPad)
bool g_prev_swap_up = false;
bool g_prev_swap_dn = false;
bool g_swallow_up = false;
bool g_swallow_dn = false;

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

bool LoadFileBytes(const char* rel_path, std::vector<uint8_t>& out) {
    out.clear();
    if (SDL_IOStream* io = SDL_IOFromFile(rel_path, "rb")) {
        Sint64 sz = SDL_GetIOSize(io);
        if (sz > 0 && sz < 8 * 1024 * 1024) {
            out.resize(size_t(sz));
            size_t got = SDL_ReadIO(io, out.data(), size_t(sz));
            SDL_CloseIO(io);
            if (got == size_t(sz)) return true;
        } else {
            SDL_CloseIO(io);
        }
    }
    const char* fallbacks[] = {
        "app/src/main/assets/weapons/flamethrower.bmsh",
        "weapons/flamethrower.bmsh",
    };
    for (const char* p : fallbacks) {
        std::ifstream f(p, std::ios::binary | std::ios::ate);
        if (!f) continue;
        auto sz = f.tellg();
        if (sz <= 0 || sz > 8 * 1024 * 1024) continue;
        f.seekg(0);
        out.resize(size_t(sz));
        if (f.read(reinterpret_cast<char*>(out.data()), sz)) return true;
    }
    return false;
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

void ApplyLethalBurn(X86Context& ctx, uint8_t* base, uint32_t victim, uint32_t me,
                     const float pos[3], const float dir[3]) {
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
    hp[0] = pos[0]; hp[1] = pos[1] + 1.05f; hp[2] = pos[2];
    hd[0] = dir[0]; hd[1] = dir[1]; hd[2] = dir[2];

    const bool prev_thermal = game::ballistics::BeginExternalThermalImpact();
    const uint32_t saved_ecx = ctx.ecx;
    ctx.ecx = victim;
    // arg5 ([ebp+0x18]) is a signed int8 impact slot index (0 = default), never a float bit pattern
    CallGuest(ctx, 0x001E74B0u, {s_scratch, s_scratch + 16, FBits(120.0f), 0x07u, 0u, me, 0u});
    ctx.ecx = saved_ecx;
    game::ballistics::EndThermalImpact(prev_thermal);
}

void PublishHud() {
    g_hud_active.store(g_sim.IsActive(), std::memory_order_relaxed);
    g_hud_firing.store(g_sim.IsFiring(), std::memory_order_relaxed);
    g_hud_reloading.store(g_sim.IsReloading(), std::memory_order_relaxed);
    g_hud_fuel.store(g_sim.FuelPct(), std::memory_order_relaxed);
    g_hud_reload_t.store(g_sim.ReloadProgress(), std::memory_order_relaxed);
    g_hud_pressure.store(g_sim.Pressure(), std::memory_order_relaxed);
}

}  // namespace

void EnsureLoaded() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_mesh_attempted) return;
    g_mesh_attempted = true;

    std::vector<uint8_t> bytes;
    if (!LoadFileBytes("weapons/flamethrower.bmsh", bytes)) {
        LOGR("[FLAMETHROWER] missing asset weapons/flamethrower.bmsh");
        return;
    }
    if (!ParseBmsh(bytes.data(), bytes.size(), g_raw_mesh)) {
        LOGR("[FLAMETHROWER] failed to parse weapons/flamethrower.bmsh (%zu bytes)", bytes.size());
        return;
    }
    g_vm_verts.resize(g_raw_mesh.vertices.size());
    for (size_t i = 0; i < g_raw_mesh.vertices.size(); i++) {
        const auto& src = g_raw_mesh.vertices[i];
        g_vm_verts[i] = {src.x, src.y, src.z, src.nx, src.ny, src.nz, src.bgra};
    }
    g_mesh_ready = true;
    LOGR("[FLAMETHROWER] loaded weapons/flamethrower.bmsh (%zu verts, %zu tris)",
         g_vm_verts.size(), g_raw_mesh.indices.size() / 3);
}

bool IsActive() {
    return g_want_active.load(std::memory_order_relaxed);
}

void SetActive(bool enable) {
    if (enable) EnsureLoaded();
    std::lock_guard<std::mutex> lock(g_state_mutex);
    const bool prev = g_want_active.load(std::memory_order_relaxed);
    g_want_active.store(enable, std::memory_order_relaxed);
    if (enable && !prev) {
        g_sim.SetActive(true, 0);
    } else if (!enable && prev) {
        g_sim.DeactivateAndRestoreSlot();
        g_trigger_held.store(false, std::memory_order_relaxed);
        g_reload_queued.store(false, std::memory_order_relaxed);
        g_nozzle_valid = false;
    }
    PublishHud();
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
    g_sim.RefillFuel();
    PublishHud();
}

void ResetLevelState() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_want_active.store(false, std::memory_order_relaxed);
    g_trigger_held.store(false, std::memory_order_relaxed);
    g_reload_queued.store(false, std::memory_order_relaxed);
    g_sim.Reset();
    g_nozzle_valid = false;
    g_prev_eye_valid = false;
    g_burning_count_frame.store(0, std::memory_order_relaxed);
    g_hud_burning.store(0, std::memory_order_relaxed);
    PublishHud();
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
                SetActive(true);
                g_swallow_up = true;
            } else if (dn_edge && held == 0u) {
                SetActive(true);
                g_swallow_dn = true;
            }
        } else {
            if (up_edge) {
                SetActive(false);
                g_swallow_up = true;
                if (count >= 2u && held != 0u) {
                    W32(me + kPlayerInventory + kInvRequest, 2u);
                }
            } else if (dn_edge) {
                SetActive(false);
                g_swallow_dn = true;
            }
        }
    } else {
        g_swallow_up = false;
        g_swallow_dn = false;
    }

    if (g_swallow_up) pad.buttons &= uint16_t(~(1u << 3));
    if (g_swallow_dn) pad.buttons &= uint16_t(~(1u << 2));

    if (!IsActive()) {
        g_trigger_held.store(false, std::memory_order_relaxed);
        return;
    }
    if (app::MenuOpen()) {
        g_trigger_held.store(false, std::memory_order_relaxed);
        return;
    }
    const bool trig = (pad.analog[7] > 0x20);
    const bool rel = (pad.analog[0] > 0x20);
    g_trigger_held.store(trig, std::memory_order_relaxed);
    if (rel) g_reload_queued.store(true, std::memory_order_relaxed);

    // Suppress underlying weapon trigger and reload while Flamethrower owns combat input
    pad.analog[7] = 0;
    pad.analog[0] = 0;
}

namespace {

inline float SmoothStep(float a, float b, float x) noexcept {
    if (x <= a) return 0.0f;
    if (x >= b) return 1.0f;
    const float t = (x - a) / (b - a);
    return t * t * (3.0f - 2.0f * t);
}

// 5-stage tactical canister-swap animation across normalized reload progress [0..1]:
//  1) [0.00..0.18] Cant weapon sideways/inward & raise catch
//  2) [0.18..0.36] Sharp unlock kick & drop depleted canister
//  3) [0.36..0.66] Lower heavy chassis cleanly to fit pressurized tank
//  4) [0.66..0.84] Forceful upward slam-lock & forward bolt bump
//  5) [0.84..1.00] Crisp shoulder recovery & ignition readiness
void EvalReloadPose(float t, float& out_x, float& out_y, float& out_z,
                    float& out_pitch, float& out_yaw, float& out_roll) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    const float cant = SmoothStep(0.0f, 0.18f, t) * (1.0f - SmoothStep(0.70f, 0.96f, t));
    const float drop = SmoothStep(0.18f, 0.36f, t) * (1.0f - SmoothStep(0.66f, 0.84f, t));
    const float eject_kick = std::sin(SmoothStep(0.16f, 0.32f, t) * kPi);
    const float slam = std::sin(SmoothStep(0.73f, 0.87f, t) * kPi);
    const float settle = std::sin(SmoothStep(0.84f, 1.00f, t) * kPi);

    out_x = -0.024f * cant - 0.022f * drop + 0.008f * slam;
    out_y = 0.018f * cant - 0.165f * drop - 0.028f * eject_kick + 0.032f * slam - 0.006f * settle;
    out_z = -0.048f * drop + 0.026f * slam - 0.008f * eject_kick;
    out_pitch = -0.18f * cant + 0.42f * drop - 0.12f * eject_kick - 0.14f * slam;
    out_yaw = 0.22f * cant + 0.12f * drop - 0.05f * slam;
    out_roll = -0.44f * cant - 0.20f * drop + 0.14f * slam - 0.04f * settle;
}

}  // namespace

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

    if (g_prev_eye_valid && dt > 1e-4f) {
        float dx = eye[0] - g_prev_eye[0];
        float dz = eye[2] - g_prev_eye[2];
        float spd = std::sqrt(dx * dx + dz * dz) / dt;
        g_walk_phase += dt * (1.8f + std::min(spd, 7.0f) * 1.45f);
    } else {
        g_walk_phase += dt * 1.8f;
    }
    std::memcpy(g_prev_eye, eye, sizeof(g_prev_eye));
    g_prev_eye_valid = true;

    // Floor under the player drives where fuel pools and keeps burning
    float ground_y = eye[1] - 1.55f;
    if (uint32_t me = PlayerPtr(); me && ValidAddr(me + kCharPos, 12u)) {
        const float fy = G<float>(me + kCharPos + 4u);
        if (std::isfinite(fy)) ground_y = fy + 0.02f;
    }

    const bool trig = g_trigger_held.load(std::memory_order_relaxed) && !app::MenuOpen();
    const bool rel = g_reload_queued.exchange(false, std::memory_order_relaxed);
    const bool refill = g_refill_queued.exchange(false, std::memory_order_relaxed);
    const bool inf_ammo = game::Opt().infinite_ammo;

    float equip_t = 1.0f, pressure = 0.0f, reload_t = 0.0f;
    bool firing = false, reloading = false;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (refill) g_sim.RefillFuel();
        g_sim.Step(dt, trig, rel, inf_ammo);
        equip_t = g_sim.EquipProgress();
        pressure = g_sim.Pressure();
        firing = g_sim.IsFiring();
        reloading = g_sim.IsReloading();
        reload_t = g_sim.ReloadProgress();
        PublishHud();
    }
    if (firing) {
        game::haptics::DispatchWeaponHaptic("FLM", false, false, false);
    }

    g_hud_burning.store(g_burning_count_frame.exchange(0, std::memory_order_relaxed),
                        std::memory_order_relaxed);

    // Smooth draw raise + 5-stage mechanical canister-swap reload + figure-8 sway + pressure recoil
    const float ease_draw = 1.0f - std::pow(1.0f - equip_t, 3.0f);
    const float draw_y = (1.0f - ease_draw) * -0.26f;
    const float draw_pitch = (1.0f - ease_draw) * 0.42f;

    float reload_x = 0.0f, reload_y = 0.0f, reload_z = 0.0f;
    float reload_pitch = 0.0f, reload_yaw = 0.0f, reload_roll = 0.0f;
    if (reloading) {
        EvalReloadPose(reload_t, reload_x, reload_y, reload_z, reload_pitch, reload_yaw, reload_roll);
    }

    const float sway_x = std::sin(g_walk_phase) * 0.0055f;
    const float sway_y = std::cos(g_walk_phase * 2.0f) * 0.0038f;
    const float recoil_z = -pressure * 0.020f;
    const float jitter_x = firing ? std::sin(g_walk_phase * 29.0f) * 0.0018f * pressure : 0.0f;
    const float jitter_y = firing ? std::cos(g_walk_phase * 37.0f) * 0.0018f * pressure : 0.0f;

    // Slight inward toe-in & barrel pitch aligning jet naturally toward screen reticle
    const float yaw = -0.055f + reload_yaw;
    const float pitch = draw_pitch + reload_pitch - pressure * 0.028f;
    const float roll = reload_roll;

    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll), sr = std::sin(roll);

    // Local rotated basis expressed in camera space (X=right, Y=up, Z=forward)
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

    const float ox = 0.165f + reload_x + sway_x + jitter_x;
    const float oy = -0.145f + draw_y + reload_y + sway_y + jitter_y;
    const float oz = 0.345f + reload_z + recoil_z;
    const float org[3] = {
        eye[0] + right[0] * ox + up[0] * oy + fwd[0] * oz,
        eye[1] + right[1] * ox + up[1] * oy + fwd[1] * oz,
        eye[2] + right[2] * ox + up[2] * oy + fwd[2] * oz
    };

    world[0] = w_x[0]; world[1] = w_x[1]; world[2] = w_x[2]; world[3] = 0.0f;
    world[4] = w_y[0]; world[5] = w_y[1]; world[6] = w_y[2]; world[7] = 0.0f;
    world[8] = w_z[0]; world[9] = w_z[1]; world[10] = w_z[2]; world[11] = 0.0f;
    world[12] = org[0]; world[13] = org[1]; world[14] = org[2]; world[15] = 1.0f;

    float pilot_world[3] = {};
    TransformPoint(world, g_raw_mesh.nozzle_pos, g_nozzle_world);
    TransformPoint(world, g_raw_mesh.pilot_pos, pilot_world);
    g_jet_dir[0] = fwd[0];
    g_jet_dir[1] = fwd[1];
    g_jet_dir[2] = fwd[2];
    g_nozzle_valid = true;

    const float aspect = (std::fabs(scene_proj[0]) > 0.05f && std::fabs(scene_proj[5]) > 0.05f)
                             ? (scene_proj[5] / scene_proj[0])
                             : g_last_cam_aspect;
    g_last_cam_aspect = aspect;
    const float vm_fov_deg = std::clamp(float(game::Opt().viewmodel_fov), 45.0f, 88.0f);
    float vm_proj[16];
    BuildPerspectiveD3D8LH(vm_fov_deg * (kPi / 180.0f), aspect, 0.04f, 12.0f, vm_proj);

    const float flicker = 0.85f + 0.15f * std::sin(g_walk_phase * 23.0f);
    const float flame_light = reloading ? 0.0f : (0.10f * flicker + pressure * 1.25f * flicker);

    if (g_mesh_ready && !g_vm_verts.empty()) {
        d3d::DrawViewmodelMesh(g_vm_verts.data(), uint32_t(g_vm_verts.size()),
                               g_raw_mesh.indices.data(), uint32_t(g_raw_mesh.indices.size()),
                               world, view, vm_proj, pilot_world, flame_light);
    }

    if (!reloading) {
        // pilot flame stays lit while the weapon is up, like a real igniter head
        game::ballistics::EmitPilotFlameGlow(pilot_world, flicker * (0.55f + 0.45f * pressure));
    }

    // Hand the muzzle pose to the simulation so next frame's fuel leaves the real
    // nozzle, then publish the simulated stream so the renderer draws exactly the
    // segments that are doing the burning (no separate visual approximation).
    g_sim.SetMuzzle(g_nozzle_world, fwd, ground_y);
    {
        static JetSegment s_jet[CoreSim::kMaxJetSegments];
        static JetSegment s_pool[CoreSim::kMaxPoolSegments];
        static game::ballistics::FlameStreamPoint s_fx_jet[CoreSim::kMaxJetSegments];
        static game::ballistics::FlameStreamPoint s_fx_pool[CoreSim::kMaxPoolSegments];
        size_t jn = 0;
        size_t pn = 0;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            jn = g_sim.CopyJetSegments(s_jet, CoreSim::kMaxJetSegments);
            pn = g_sim.CopyPoolSegments(s_pool, CoreSim::kMaxPoolSegments);
        }
        for (size_t i = 0; i < jn; i++) {
            s_fx_jet[i] = {{s_jet[i].pos[0], s_jet[i].pos[1], s_jet[i].pos[2]},
                           {s_jet[i].vel[0], s_jet[i].vel[1], s_jet[i].vel[2]},
                           s_jet[i].radius, s_jet[i].heat};
        }
        for (size_t i = 0; i < pn; i++) {
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

    // Read snapshot of exposure & update burn victim sim
    VictimBurnTick tick{};
    bool active_burning = false;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (!is_dying && g_sim.HasActiveJet()) {
            // Heat only arrives once the simulated fuel has actually travelled
            // there; a body standing in pooled fire keeps cooking from the floor.
            const float chest_ex = g_sim.StreamExposure(chest, 0.42f);
            const float boots_ex = g_sim.StreamExposure(boots, 0.30f);
            const float exposure = std::max(chest_ex, boots_ex);
            if (exposure > 0.02f) {
                g_sim.ExposeEnemy(who, exposure, cdt);
            }
        }
        tick = g_sim.TickVictim(who, cdt, is_dying);
        active_burning = g_sim.GetVictimState(who).ignited;
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
        // Subtle erratic panic stumble while engulfed in flames
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
                ApplyLethalBurn(ctx, base, who, me, feet, g_jet_dir);
            }
        }
    }
}

void RenderAudioMix(float* out, uint32_t frames, uint32_t sample_rate,
                    uint32_t out_channels, uint32_t ch_left, uint32_t ch_right) {
    if (!out || frames == 0 || out_channels < 2) return;
    if (out_channels == 2 && ch_left == 0 && ch_right == 1) {
        g_sim.RenderAudio(out, frames, sample_rate);
        return;
    }
    constexpr uint32_t kChunk = 256;
    float tmp[kChunk * 2];
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = std::min(kChunk, frames - done);
        std::memset(tmp, 0, size_t(n) * 2 * sizeof(float));
        g_sim.RenderAudio(tmp, n, sample_rate);
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t dst = (done + i) * out_channels;
            out[dst + ch_left] = std::clamp(out[dst + ch_left] + tmp[i * 2 + 0], -1.0f, 1.0f);
            out[dst + ch_right] = std::clamp(out[dst + ch_right] + tmp[i * 2 + 1], -1.0f, 1.0f);
        }
        done += n;
    }
}

HudState GetHudState() {
    HudState s;
    s.active = g_hud_active.load(std::memory_order_relaxed);
    s.firing = g_hud_firing.load(std::memory_order_relaxed);
    s.reloading = g_hud_reloading.load(std::memory_order_relaxed);
    s.fuel_pct = g_hud_fuel.load(std::memory_order_relaxed);
    s.reload_progress = g_hud_reload_t.load(std::memory_order_relaxed);
    s.pressure = g_hud_pressure.load(std::memory_order_relaxed);
    s.active_burning_enemies = g_hud_burning.load(std::memory_order_relaxed);
    return s;
}

}  // namespace game::flamethrower
