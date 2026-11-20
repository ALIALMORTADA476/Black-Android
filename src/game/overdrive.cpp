#include "game/overdrive.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <imgui.h>

#include "app/app.h"
#include "common/mod_schema.h"
#include "common/options.h"
#include "core/log.h"
#include "game/ballistics_fx.h"
#include "game/delirium_physics.h"
#include "game/layout.h"
#include "game/overdrive_rules.h"
#include "game/time_stop.h"
#include "sound/weapon_audio.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;

namespace app {
bool MenuOpen();
void Save();
std::string Ui(std::string_view text, bool rtl_shape = true);
}

namespace game {
bool HudShown();
}

namespace game::overdrive {

namespace {

constexpr uint32_t kCharPos = 0xA0u;
constexpr uint32_t kCharKind = 0xC4u;
constexpr uint32_t kPlayerForward = 0xF0u;
constexpr uint32_t kPlayerEye = 0x100u;
constexpr uint32_t kPlayerWeapon = 0x29Cu;
constexpr uint32_t kCharHealth = 0x2F0u;
constexpr uint32_t kCharDying = 0x384u;
constexpr uint32_t kWeaponMag = 0xF4u;
constexpr uint32_t kMagData = 8u;
constexpr uint32_t kDataClip = 0x60u;
constexpr uint32_t kAmmoTotal = 0x14u;
constexpr uint32_t kAmmoClip = 0x18u;
constexpr uint32_t kEnemyKind = 1u;
constexpr uint32_t kPlayerKind = 2u;

uint32_t FBits(float v) noexcept {
    uint32_t u = 0;
    std::memcpy(&u, &v, sizeof(u));
    return u;
}

bool ValidAddr(uint32_t ea, uint32_t bytes) noexcept {
    return ea >= mem::kUserLow && bytes > 0 &&
           (uint64_t(ea) + bytes) <= 0x100000000ULL &&
           mem::IsCommitted(ea) && mem::IsCommitted(ea + bytes - 1u);
}

uint32_t PlayerPtr() noexcept {
    if (!mem::IsCommitted(kWorld)) return 0;
    uint32_t w = R32(kWorld);
    if (!w || !ValidAddr(w + kWorldPlayer, 0x3A0u)) return 0;
    return w + kWorldPlayer;
}

float PlayerMaxHp() noexcept {
    // 1200 normal, 750 black ops (same as cheats.cpp)
    return 1200.0f;
}

struct BurningEnemy {
    uint32_t who = 0;
    float remaining_sec = 0.0f;
    float next_tick_fx = 0.0f;
    float dps = 75.0f;
    uint32_t agony_cd = 0;
};

constexpr size_t kMaxBurnSlots = 24;

std::mutex g_od_mutex;
overdrive_rules::AdrenalineEngine g_adrenaline;
overdrive_rules::KineticDashController g_dash;
overdrive_rules::KillCamSlowmoController g_killcam;
std::array<BurningEnemy, kMaxBurnSlots> g_burning{};

bool g_pending_dash = false;
bool g_pending_overdrive = false;
float g_stick_lx = 0.0f;
float g_stick_ly = 0.0f;
float g_dash_dir[3] = {0.0f, 0.0f, 1.0f};
bool g_shockwave_pending_exec = false;

std::chrono::steady_clock::time_point g_last_player_tp{};
std::chrono::steady_clock::time_point g_last_he_blast_tp{};
std::chrono::steady_clock::time_point g_siphon_banner_tp{};
float g_siphon_hp_banner = 0.0f;
uint16_t g_siphon_ammo_banner = 0;

// Lock-free snapshots published per frame for shaders/hooks/HUD
std::atomic<float> g_pub_fire_rate_mul{1.0f};
std::atomic<float> g_pub_fov_boost_deg{0.0f};
std::atomic<float> g_pub_world_time_scale{1.0f};
std::atomic<float> g_pub_edge_energy{0.0f};
std::atomic<float> g_pub_charge{0.0f};
std::atomic<float> g_pub_od_sec{0.0f};
std::atomic<bool> g_pub_od_active{false};
std::atomic<uint32_t> g_pub_streak{0};
std::atomic<float> g_pub_dash_cd{0.0f};
std::atomic<bool> g_pub_dashing{false};

thread_local bool g_in_splash_or_burn = false;

void CallGuestDamage(X86Context& ctx, uint8_t* base, uint32_t victim, uint32_t me,
                     const float pos[3], const float dir[3], float force, uint32_t zone = 0x05u) noexcept {
    static uint32_t s_scratch = 0;
    if (!s_scratch) s_scratch = mem::AllocSystem(32, 16);
    if (!s_scratch || !base || !victim || !me || !pos || !dir) return;
    if (!ValidAddr(victim, 0x390u) || !ValidAddr(me, 0x3A0u)) return;

    const uint32_t v_node = R32(victim + 0x324u);
    const uint32_t v_mesh = R32(victim + 0x320u);
    const uint32_t m_wp = R32(me + 0x29Cu);
    const uint32_t m_node = R32(me + 0x324u);
    if (!ValidAddr(v_node, 0xCB0u) || !ValidAddr(v_mesh, 0x48u) || !ValidAddr(m_wp, 0x110u)) return;
    if (m_node && !ValidAddr(m_node, 0x60u)) return;

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

    float* hp = reinterpret_cast<float*>(HostPtr(s_scratch));
    float* hd = reinterpret_cast<float*>(HostPtr(s_scratch + 16));
    hp[0] = pos[0];
    hp[1] = pos[1] + 1.05f;
    hp[2] = pos[2];
    hd[0] = dir[0];
    hd[1] = dir[1];
    hd[2] = dir[2];

    const bool prev_splash = g_in_splash_or_burn;
    const bool prev_thermal = game::ballistics::BeginExternalThermalImpact();
    g_in_splash_or_burn = true;

    const uint32_t saved_ecx = ctx.ecx;
    ctx.ecx = victim;
    // arg5 ([ebp+0x18]) is a signed int8 impact slot index (0 = default), never a float bit pattern
    CallGuest(ctx, 0x001E74B0u, {s_scratch, s_scratch + 16, FBits(force), zone, 0u, me, 0u});
    ctx.ecx = saved_ecx;

    g_in_splash_or_burn = prev_splash;
    game::ballistics::EndThermalImpact(prev_thermal);
}

void ExecuteRadialBlast(X86Context& ctx, uint8_t* base, uint32_t me,
                        const float center[3], float radius, float base_damage,
                        float launch_force, uint32_t skip_victim = 0) noexcept {
    if (!me || !center || radius <= 0.5f) return;

    game::ballistics::OnExplosionDetonate(center, radius, 1.35f);
    game::haptics::DispatchExplosionHaptic(center);

    uint32_t enemies[64];
    const size_t n = game::CopyLivingEnemies(enemies, 64);
    const float rad2 = radius * radius;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t e = enemies[i];
        if (!e || e == me || e == skip_victim || !ValidAddr(e, 0x390u)) continue;
        if (R32(e + kCharKind) != kEnemyKind || R32(e + kCharDying) != 0u) continue;

        float ep[3] = {G<float>(e + kCharPos), G<float>(e + kCharPos + 4), G<float>(e + kCharPos + 8)};
        float dx = ep[0] - center[0];
        float dy = (ep[1] + 0.9f) - center[1];
        float dz = ep[2] - center[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > rad2) continue;

        const float dist = std::max(0.4f, std::sqrt(d2));
        const float falloff = std::clamp(1.0f - dist / (radius * 1.15f), 0.25f, 1.0f);
        float dir[3] = {dx / dist, std::max(0.28f, (dy + 0.8f) / dist), dz / dist};
        const float dlen = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (dlen > 1e-4f) {
            dir[0] /= dlen;
            dir[1] /= dlen;
            dir[2] /= dlen;
        }

        const float dmg = base_damage * falloff;
        float& hp = G<float>(e + kCharHealth);
        if (std::isfinite(hp) && hp > 0.0f) {
            if (hp > dmg + 1.0f) {
                hp -= dmg;
                // Knockback displacement + delirium flinch without instant death
                G<float>(e + kCharPos) += dir[0] * (0.45f * falloff);
                G<float>(e + kCharPos + 8) += dir[2] * (0.45f * falloff);
                game::delirium_phys::TriggerExternalAgony(ctx, base, e, 0u);
            } else {
                hp = 0.01f;
                CallGuestDamage(ctx, base, e, me, ep, dir, launch_force * falloff, 0x05u);
            }
        }
    }
}

void PublishLocked(const overdrive_rules::OverdriveStepOutput& od,
                   const overdrive_rules::DashStepOutput& dash) noexcept {
    g_pub_fire_rate_mul.store(od.fire_rate_multiplier, std::memory_order_relaxed);
    g_pub_fov_boost_deg.store(dash.fov_boost_deg, std::memory_order_relaxed);
    g_pub_world_time_scale.store(od.world_time_scale, std::memory_order_relaxed);
    g_pub_edge_energy.store(od.edge_energy, std::memory_order_relaxed);
    g_pub_charge.store(od.charge_pct, std::memory_order_relaxed);
    g_pub_od_sec.store(od.overdrive_sec, std::memory_order_relaxed);
    g_pub_od_active.store(od.overdrive_active, std::memory_order_relaxed);
    g_pub_streak.store(od.streak_count, std::memory_order_relaxed);
    g_pub_dash_cd.store(dash.cooldown_remaining, std::memory_order_relaxed);
    g_pub_dashing.store(dash.dashing, std::memory_order_relaxed);
}

}  // namespace

bool NeedsEnemyTracking() noexcept {
    const auto& enh = modsettings::Registry().GetEnhanced();
    return enh.thermal_visor_enabled || enh.kinetic_dash_shockwave ||
           enh.special_ammo_mode != 0u || enh.overdrive_enabled;
}

bool InSplashOrBurn() noexcept {
    return g_in_splash_or_burn;
}

void TriggerKineticDash() {
    if (!time_stop::IsActiveGameplay() || app::MenuOpen()) return;
    if (!modsettings::Registry().GetEnhanced().kinetic_dash_enabled) return;
    std::lock_guard<std::mutex> lock(g_od_mutex);
    g_pending_dash = true;
}

void TriggerOverdriveSurge() {
    // Adrenaline Overdrive Surge & meter removed per user preference
}

void CycleSpecialAmmoMode() {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    const auto cur = overdrive_rules::SanitizeSpecialAmmo(enh.special_ammo_mode);
    const auto next = overdrive_rules::CycleSpecialAmmo(cur);
    reg.SetUint(modsettings::SettingId::SpecialAmmoMode, uint32_t(next), app::Get(), game::Opt());
    sound::weapon_audio::TriggerSpecialAmmoCue(uint8_t(next));
    LOGR("[OVERDRIVE] Special Ammo -> %s (%u)",
         overdrive_rules::GetSpecialAmmoProfile(next).short_code, uint32_t(next));
}

void ToggleThermalVisor() {
    auto& reg = modsettings::Registry();
    const bool next = !reg.GetEnhanced().thermal_visor_enabled;
    reg.SetBool(modsettings::SettingId::ThermalVisorEnabled, next, app::Get(), game::Opt());
    sound::weapon_audio::TriggerSpecialAmmoCue(next ? 3u : 0u);
    LOGR("[OVERDRIVE] Thermal Threat Visor -> %s", next ? "ON" : "OFF");
}

void SetTouchStickInput(float lx, float ly) {
    std::lock_guard<std::mutex> lock(g_od_mutex);
    g_stick_lx = std::clamp(lx, -1.0f, 1.0f);
    g_stick_ly = std::clamp(ly, -1.0f, 1.0f);
}

void ResetLevelState() {
    std::lock_guard<std::mutex> lock(g_od_mutex);
    g_adrenaline.Reset();
    g_dash.Reset();
    for (auto& b : g_burning) b = BurningEnemy{};
    g_pending_dash = false;
    g_pending_overdrive = false;
    g_shockwave_pending_exec = false;
    g_siphon_hp_banner = 0.0f;
    g_siphon_ammo_banner = 0;
    PublishLocked({}, {});
}

void OnPlayerFramePreUpdate(X86Context& ctx, uint8_t* base, uint32_t player_ea) {
    if (!player_ea || !ValidAddr(player_ea, 0x3A0u)) return;
    if (R32(player_ea + kCharDying) != 0u) return;

    const auto now = std::chrono::steady_clock::now();
    float dt = 1.0f / 60.0f;
    if (g_last_player_tp.time_since_epoch().count() != 0) {
        dt = std::clamp(std::chrono::duration<float>(now - g_last_player_tp).count(), 0.001f, 0.05f);
    }
    g_last_player_tp = now;

    const auto& enh = modsettings::Registry().GetEnhanced();
    bool started_dash = false;
    float move_delta[3] = {};

    {
        std::lock_guard<std::mutex> lock(g_od_mutex);
        overdrive_rules::OverdriveStepOutput od{};

        if (g_pending_dash) {
            g_pending_dash = false;
            if (g_dash.TryTrigger(enh.kinetic_dash_enabled, false, false)) {
                started_dash = true;
                float cam_fwd[3] = {G<float>(player_ea + kPlayerForward),
                                    G<float>(player_ea + kPlayerForward + 4u),
                                    G<float>(player_ea + kPlayerForward + 8u)};
                float cam_right[3] = {cam_fwd[2], 0.0f, -cam_fwd[0]};
                if (!overdrive_rules::ComputeDashMoveVector(cam_fwd, cam_right, g_stick_lx, g_stick_ly, g_dash_dir)) {
                    g_dash_dir[0] = cam_fwd[0];
                    g_dash_dir[1] = 0.0f;
                    g_dash_dir[2] = cam_fwd[2];
                }
            }
        }

        auto dash = g_dash.Step(dt);
        const float kc_scale = g_killcam.Step(dt, enh.killcam_slowmo_enabled);
        g_pub_world_time_scale.store(kc_scale, std::memory_order_relaxed);
        g_shockwave_pending_exec = false;
        if (dash.dashing && dash.speed_mps > 0.1f) {
            move_delta[0] = g_dash_dir[0] * dash.speed_mps * dt;
            move_delta[2] = g_dash_dir[2] * dash.speed_mps * dt;
        }
        PublishLocked(od, dash);
    }

    if (move_delta[0] != 0.0f || move_delta[2] != 0.0f) {
        G<float>(player_ea + kCharPos) += move_delta[0];
        G<float>(player_ea + kCharPos + 8u) += move_delta[2];
    }

    if (started_dash) {
        sound::weapon_audio::TriggerKineticDash(false);
        game::haptics::DispatchWeaponHaptic("SHG", false, false, false);
    }
}

void OnEnemyFrameUpdate(X86Context& ctx, uint8_t* base, uint32_t who, float dt) {
    if (!who || !ValidAddr(who, 0x390u) || dt <= 0.0f) return;
    const bool is_dying = (R32(who + kCharDying) != 0u);
    uint32_t me = PlayerPtr();

    bool burning = false;
    bool do_flame_fx = false;
    bool do_agony = false;
    float dmg = 0.0f;
    float heat = 0.0f;

    {
        std::lock_guard<std::mutex> lock(g_od_mutex);
        for (auto& b : g_burning) {
            if (b.who != who) continue;
            if (is_dying) {
                b.remaining_sec = std::min(b.remaining_sec, 0.9f);
            }
            if (b.remaining_sec <= 0.0f) {
                b.who = 0;
                break;
            }
            burning = true;
            b.remaining_sec -= dt;
            heat = std::clamp(b.remaining_sec / 2.5f, 0.25f, 1.0f);
            b.next_tick_fx -= dt;
            if (b.next_tick_fx <= 0.0f) {
                b.next_tick_fx = 0.065f;
                do_flame_fx = true;
            }
            if (!is_dying) {
                dmg = b.dps * dt;
                if (b.agony_cd == 0) {
                    b.agony_cd = 28;
                    do_agony = true;
                } else {
                    b.agony_cd--;
                }
            }
            break;
        }
    }

    if (!burning) return;

    float feet[3] = {G<float>(who + kCharPos), G<float>(who + kCharPos + 4u), G<float>(who + kCharPos + 8u)};
    if (do_flame_fx) {
        game::ballistics::EmitBurnVictimFlames(feet, feet[1] + 0.02f, heat, true, true, is_dying);
    }
    if (do_agony && !is_dying) {
        game::delirium_phys::TriggerExternalAgony(ctx, base, who, 0u);
    }
    if (!is_dying && dmg > 0.01f && me) {
        float& hp = G<float>(who + kCharHealth);
        if (std::isfinite(hp) && hp > 0.0f) {
            if (hp > dmg + 1.0f) {
                hp -= dmg;
            } else {
                hp = 0.01f;
                const float dir[3] = {0.0f, 0.35f, 1.0f};
                CallGuestDamage(ctx, base, who, me, feet, dir, 95.0f, 0x05u);
            }
        }
    }
}

void ModifyPlayerBulletHit(uint32_t victim_ea, bool is_headshot, float& raw_force, float& hp_scale) {
    if (!victim_ea) return;
    const auto& enh = modsettings::Registry().GetEnhanced();
    const auto ammo = overdrive_rules::SanitizeSpecialAmmo(enh.special_ammo_mode);
    if (ammo == overdrive_rules::SpecialAmmoType::Standard) return;

    const auto prof = overdrive_rules::GetSpecialAmmoProfile(ammo);
    raw_force *= prof.force_scale;
    hp_scale *= prof.damage_scale * (is_headshot ? prof.headshot_scale : 1.0f);
}

void OnPlayerBulletPostHit(X86Context& ctx, uint8_t* base, uint32_t victim_ea, uint32_t me_ea,
                           const float hit_pos[3], const float hit_dir[3],
                           bool is_headshot, bool is_lethal) {
    if (!me_ea || !victim_ea || !ValidAddr(me_ea, 0x3A0u)) return;
    if (g_in_splash_or_burn) return;

    const auto& enh = modsettings::Registry().GetEnhanced();
    if (is_lethal && enh.killcam_slowmo_enabled && hit_pos) {
        const float dx = hit_pos[0] - G<float>(me_ea + kCharPos);
        const float dz = hit_pos[2] - G<float>(me_ea + kCharPos + 8u);
        const float d = std::sqrt(dx * dx + dz * dz);
        if (is_headshot || d > 24.0f) {
            std::lock_guard<std::mutex> lock(g_od_mutex);
            g_killcam.Trigger(true, is_headshot ? 1.15f : 0.85f);
        }
    }
    const auto ammo = overdrive_rules::SanitizeSpecialAmmo(enh.special_ammo_mode);
    const auto prof = overdrive_rules::GetSpecialAmmoProfile(ammo);

    // 1) Thermite Incendiary ignition
    if (prof.ignite_target && ValidAddr(victim_ea, 0x390u)) {
        bool already = false;
        std::lock_guard<std::mutex> lock(g_od_mutex);
        BurningEnemy* empty_slot = nullptr;
        for (auto& b : g_burning) {
            if (b.who == victim_ea) {
                b.remaining_sec = 3.6f;
                already = true;
                break;
            }
            if (!empty_slot && b.who == 0) empty_slot = &b;
        }
        if (!already && empty_slot) {
            empty_slot->who = victim_ea;
            empty_slot->remaining_sec = 3.6f;
            empty_slot->next_tick_fx = 0.0f;
            empty_slot->dps = 85.0f;
            empty_slot->agony_cd = 0;
        }
        if (hit_pos) {
            game::ballistics::EmitBurnVictimFlames(hit_pos, hit_pos[1] - 0.8f, 1.0f, true, true, is_lethal);
        }
    }

    // 3) Explosive HE Frag detonation (rate-limited per burst so rapid-fire/shotguns stay crisp)
    if (prof.detonate_on_hit && hit_pos) {
        bool allow = false;
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(g_od_mutex);
            if (g_last_he_blast_tp.time_since_epoch().count() == 0 ||
                std::chrono::duration_cast<std::chrono::milliseconds>(now - g_last_he_blast_tp).count() >= 55) {
                g_last_he_blast_tp = now;
                allow = true;
            }
        }
        if (allow) {
            ExecuteRadialBlast(ctx, base, me_ea, hit_pos, 4.4f, 110.0f, 195.0f, victim_ea);
        }
    }
}

float GetWeaponWaitMultiplier() noexcept {
    return 1.0f;
}

float GetDashFovBoostDeg() noexcept {
    if (!modsettings::Registry().GetEnhanced().kinetic_dash_enabled) return 0.0f;
    return g_pub_fov_boost_deg.load(std::memory_order_relaxed);
}

float GetWorldTimeScaleMultiplier() noexcept {
    if (!modsettings::Registry().GetEnhanced().killcam_slowmo_enabled) return 1.0f;
    return g_pub_world_time_scale.load(std::memory_order_relaxed);
}

void GetShaderPostParams(float& out_thermal_mix, float& out_overdrive_edge) noexcept {
    const auto& enh = modsettings::Registry().GetEnhanced();
    const auto p = overdrive_rules::ComputeVisorShaderParams(
        enh.thermal_visor_enabled,
        time_stop::IsActiveGameplay(),
        app::MenuOpen(),
        0.0f);
    out_thermal_mix = p.thermal_mix;
    out_overdrive_edge = 0.0f;
}

HudSnapshot GetHudSnapshot() noexcept {
    const auto& enh = modsettings::Registry().GetEnhanced();
    const auto ammo = overdrive_rules::SanitizeSpecialAmmo(enh.special_ammo_mode);
    const auto prof = overdrive_rules::GetSpecialAmmoProfile(ammo);
    HudSnapshot s;
    s.enabled = enh.overdrive_enabled;
    s.overdrive_active = g_pub_od_active.load(std::memory_order_relaxed);
    s.charge_pct = g_pub_charge.load(std::memory_order_relaxed);
    s.overdrive_sec = g_pub_od_sec.load(std::memory_order_relaxed);
    s.streak = g_pub_streak.load(std::memory_order_relaxed);
    s.dash_cooldown = g_pub_dash_cd.load(std::memory_order_relaxed);
    s.dashing = g_pub_dashing.load(std::memory_order_relaxed);
    s.ammo_mode = uint32_t(ammo);
    s.ammo_short = prof.short_code;
    s.thermal_on = enh.thermal_visor_enabled;
    return s;
}

void DrawHudOverlay(float screen_w, float screen_h) {
    if (screen_w <= 1.0f || screen_h <= 1.0f) return;
    if (!time_stop::IsActiveGameplay() || game::g_hide_hud || app::MenuOpen() || !game::HudShown()) {
        return;
    }
    const auto& enh = modsettings::Registry().GetEnhanced();
    if (enh.hide_hud_on_screenshots) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;
    const float sc = std::clamp(screen_h / 720.0f, 0.85f, 2.20f);

    // 1. Peripheral speed streaks while Kinetic Dashing (strictly outside central 40% zone)
    if (enh.kinetic_dash_enabled && g_pub_dashing.load(std::memory_order_relaxed)) {
        const float cx = screen_w * 0.5f;
        const float cy = screen_h * 0.5f;
        for (int i = 0; i < 20; ++i) {
            const float a = float(i) * (6.2831853f / 20.0f) + 0.14f;
            const float ca = std::cos(a), sa = std::sin(a);
            const float r0 = std::min(screen_w, screen_h) * 0.43f;
            const float r1 = std::max(screen_w, screen_h) * 0.65f;
            dl->AddLine(ImVec2(cx + ca * r0, cy + sa * r0),
                        ImVec2(cx + ca * r1, cy + sa * r1),
                        IM_COL32(130, 235, 255, 115), 2.2f * sc);
        }
    }

    // 2. Predator FLIR Thermal Target Brackets over living enemies
    if (enh.thermal_visor_enabled) {
        uint32_t enemies[64];
        const size_t n = game::CopyLivingEnemies(enemies, 64);
        float player_pos[3] = {};
        if (uint32_t me = PlayerPtr()) {
            player_pos[0] = G<float>(me + kCharPos);
            player_pos[1] = G<float>(me + kCharPos + 4u);
            player_pos[2] = G<float>(me + kCharPos + 8u);
        }
        for (size_t i = 0; i < n; ++i) {
            const uint32_t e = enemies[i];
            if (!e || !ValidAddr(e, 0x390u) || R32(e + kCharDying) != 0u) continue;
            const float feet[3] = {G<float>(e + kCharPos), G<float>(e + kCharPos + 4u), G<float>(e + kCharPos + 8u)};
            const float head[3] = {feet[0], feet[1] + 1.75f, feet[2]};
            float n_feet[2] = {}, n_head[2] = {};
            if (!game::ProjectWorldPoint(feet, n_feet) || !game::ProjectWorldPoint(head, n_head)) continue;

            const float fx = n_feet[0] * screen_w, fy = n_feet[1] * screen_h;
            const float hx = n_head[0] * screen_w, hy = n_head[1] * screen_h;
            const float hbox = std::max(24.0f * sc, fy - hy);
            const float wbox = hbox * 0.46f;
            const float x0 = hx - wbox * 0.5f, x1 = hx + wbox * 0.5f;
            const float y0 = hy, y1 = hy + hbox;
            const float tick = std::max(7.0f * sc, wbox * 0.28f);

            const ImU32 col_hot = IM_COL32(255, 190, 60, 225);
            const ImU32 col_core = IM_COL32(255, 75, 40, 235);
            // Corner brackets
            dl->AddLine(ImVec2(x0, y0), ImVec2(x0 + tick, y0), col_hot, 2.0f * sc);
            dl->AddLine(ImVec2(x0, y0), ImVec2(x0, y0 + tick), col_hot, 2.0f * sc);
            dl->AddLine(ImVec2(x1, y0), ImVec2(x1 - tick, y0), col_hot, 2.0f * sc);
            dl->AddLine(ImVec2(x1, y0), ImVec2(x1, y0 + tick), col_hot, 2.0f * sc);
            dl->AddLine(ImVec2(x0, y1), ImVec2(x0 + tick, y1), col_hot, 2.0f * sc);
            dl->AddLine(ImVec2(x0, y1), ImVec2(x0, y1 - tick), col_hot, 2.0f * sc);
            dl->AddLine(ImVec2(x1, y1), ImVec2(x1 - tick, y1), col_hot, 2.0f * sc);
            dl->AddLine(ImVec2(x1, y1), ImVec2(x1, y1 - tick), col_hot, 2.0f * sc);

            // Weakpoint head diamond reticle + distance meter
            const float hy_head = y0 + hbox * 0.14f;
            const float dr = std::clamp(hbox * 0.08f, 4.0f * sc, 10.0f * sc);
            dl->AddQuad(ImVec2(hx, hy_head - dr), ImVec2(hx + dr, hy_head),
                        ImVec2(hx, hy_head + dr), ImVec2(hx - dr, hy_head), col_core, 1.8f * sc);

            const float dx = feet[0] - player_pos[0], dy = feet[1] - player_pos[1], dz = feet[2] - player_pos[2];
            const float dist_m = std::sqrt(dx * dx + dy * dy + dz * dz);
            char dist_txt[20];
            std::snprintf(dist_txt, sizeof(dist_txt), "%.0fm", dist_m);
            dl->AddText(ImGui::GetFont(), 13.0f * sc, ImVec2(x0, y1 + 2.0f * sc), col_hot, dist_txt);
        }
    }
}

}  // namespace game::overdrive
