#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace game::special_arsenal {

enum class SpecialWeaponId : uint8_t {
    Flamethrower = 0,
    Mauser1891 = 1,
    MauserC96 = 2,
    Maxim1910 = 3,
    Count = 4
};

constexpr size_t kWeaponCount = size_t(SpecialWeaponId::Count);

struct BmshVertexV2 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float nx = 0.0f, ny = 0.0f, nz = 1.0f;
    uint32_t bgra = 0xFFFFFFFFu;
    float u = 0.0f, v = 0.0f;
};

struct BmshPart {
    char name[32] = {};
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    float pivot[3] = {0.0f, 0.0f, 0.0f};
    float rest_offset[3] = {0.0f, 0.0f, 0.0f};
};

struct BmshModel {
    uint32_t version = 0;
    uint32_t flags = 0;
    std::vector<BmshVertexV2> vertices;
    std::vector<uint16_t> indices;
    std::vector<BmshPart> parts;
    float bounds_min[3] = {0.0f, 0.0f, 0.0f};
    float bounds_max[3] = {0.0f, 0.0f, 0.0f};
    float nozzle_pos[3] = {0.0f, 0.0f, 0.5f};
    float pilot_pos[3] = {0.0f, -0.02f, 0.45f};
    float eject_pos[3] = {0.03f, 0.02f, 0.12f};
};

struct BtexHeader {
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t d3d_format = 0;
    uint32_t payload_bytes = 0;
    uint64_t cache_key = 0;
};

struct BtexValidation {
    bool valid = false;
    BtexHeader header{};
    float white_fraction = 1.0f;
    float mean_luma = 0.0f;
    float std_rgb = 0.0f;
};

struct MmatBatch {
    uint32_t part_idx = 0;
    uint32_t mat_id = 0;
    uint32_t first_index = 0;
    uint32_t index_count = 0;
};

bool ParseBmsh(const uint8_t* data, size_t size, BmshModel& out);
bool ParseMmat(const uint8_t* data, size_t size, std::vector<MmatBatch>& out);
bool ParseBtexHeader(const uint8_t* data, size_t size, BtexHeader& out);
BtexValidation ValidateBtex(const uint8_t* data, size_t size);

enum class ActionPhase : uint8_t {
    Idle = 0,
    Igniting,
    Firing,
    BoltWaitRelease,
    BoltLift,
    BoltBack,
    BoltPushCocking,
    BoltLock,
    Reloading
};

enum class AudioCueKind : uint8_t {
    None = 0,
    FlameIgniteSquib,
    FlameValveShutoff,
    FlameTankReload,
    Mauser91Shot,
    Mauser91BoltCycle,
    Mauser91StripperClip,
    Mauser91DryFire,
    Mauser91ChamberClose,
    Mauser91Equip,
    Mauser91ScopeZoom,
    C96Shot,
    C96BreechClack,
    C96ClipReload,
    C96DryFire,
    C96BoltLockOpen,
    MaximShot,
    MaximTopCoverReload,
    MaximWaterBoil,
    MaximSteamHiss
};

struct ArsenalInput {
    float dt = 0.016667f;
    bool equipped = false;
    bool fire_held = false;
    bool fire_pressed = false;
    bool reload_pressed = false;
    bool ads_held = false;
    bool select_fire_pressed = false;
    bool zoom_cycle_pressed = false;
    float player_speed_mps = 0.0f;
    float equip_progress = 1.0f;
};

struct PartAnimPose {
    // Flamethrower
    float trigger_pull = 0.0f;      // 0..1
    float safety_lever = 0.0f;      // 0=safe, 1=fire
    float gauge_needle = 0.0f;      // 0..1 normalized regulator gauge
    float pilot_spark = 0.0f;       // 0..1 electrical squib spark intensity
    float tank_tilt_rad = 0.0f;     // reload tilt

    // Mauser 1891-12
    float bolt_lift_deg = 0.0f;     // 0..68 deg (bolt handle only)
    float bolt_retract_m = 0.0f;    // 0..0.096 m
    float bolt_stop_deg = 0.0f;     // 0..-14 deg latch pivot
    float rear_sight_deg = 0.0f;    // 0..4.5 deg tangent sight leaf elevation on ADS
    float clip_insert = 0.0f;       // 0..1 stripper clip downward insertion
    float clip_eject = 0.0f;        // 0..1 empty stripper clip ejection arc
    float clip_rounds_left = 0.0f;  // 0..5
    uint8_t hand_clip_id = 0u;      // 0=idle/fire, 1=bolt_cycle, 2=reload_stripper_clip
    float hand_clip_time_s = 0.0f;  // synchronized time into Mauser 1891-12 hands animation clip

    // Mauser C96 / M712
    float barrel_recoil_m = 0.0f;   // 0..0.008 m short recoil barrel+upper extension
    float hammer_cocked = 1.0f;     // 0=struck, 1=cocked
    bool bolt_locked_open = false;

    // Maxim PM M1910
    float wheel_angle_rad = 0.0f;   // rotates continuously with forward movement
    float top_cover_deg = 0.0f;     // hinged feed cover on reload
    float spade_thumb_press = 0.0f; // 0..1 thumb trigger depression
    float belt_shift = 0.0f;        // 0..1 belt indexing per shot
    float steam_vent = 0.0f;        // 0..1 visible condenser steam intensity
};

struct ArsenalStepEvents {
    bool fire_bullet = false;
    bool fire_flame_tick = false;
    bool eject_shell = false;
    bool steam_plume = false;
    float bullet_damage = 0.0f;
    float bullet_velocity_mps = 0.0f;
    float bullet_spread_deg = 0.5f;
    float recoil_pitch_deg = 0.0f;
    float recoil_yaw_deg = 0.0f;
    float recoil_roll_deg = 0.0f;
    std::array<AudioCueKind, 4> audio_cues{};
    uint8_t audio_cue_count = 0;

    void PushAudio(AudioCueKind c) {
        if (c != AudioCueKind::None && audio_cue_count < audio_cues.size()) {
            audio_cues[audio_cue_count++] = c;
        }
    }
};

struct WeaponStatus {
    SpecialWeaponId id = SpecialWeaponId::Flamethrower;
    const char* code = "FLM";
    const char* display_name = "M2 FLAMETHROWER";
    int mag_ammo = 100;
    int max_mag_ammo = 100;
    int reserve_ammo = 200;
    int ignition_cartridges = 5;
    float fuel_pct = 100.0f;
    float regulator_psi = 350.0f;
    float nozzle_range_m = 28.0f;
    float water_temp_c = 28.0f;
    float water_level_l = 4.5f;
    bool water_boiling = false;
    bool full_auto_mode = false;
    bool bolt_locked_open = false;
    bool reloading = false;
    float ads_blend = 0.0f;
    float ads_fov_scale = 1.0f;
    float scope_zoom_x = 1.0f;
    float scope_zero_m = 200.0f;
    ActionPhase phase = ActionPhase::Idle;
    PartAnimPose pose{};
};

// Converts normalized FOV divisor ads_fov_scale (<= 1.0f) to optical magnification (>= 1.0f).
float ResolveZoomMagnification(float ads_fov_scale) noexcept;

class ArsenalSim {
public:
    void ResetAll();
    void SelectWeapon(SpecialWeaponId id);
    SpecialWeaponId ActiveWeapon() const noexcept { return active_; }
    ArsenalStepEvents Step(const ArsenalInput& in);
    WeaponStatus GetStatus() const noexcept;
    WeaponStatus GetStatusFor(SpecialWeaponId id) const noexcept;
    void RefillActive();

private:
    SpecialWeaponId active_ = SpecialWeaponId::Flamethrower;
    bool initialized_ = false;
    bool was_firing_flame_ = false;
    ActionPhase phase_ = ActionPhase::Idle;
    float phase_timer_ = 0.0f;
    float reload_timer_ = 0.0f;
    float ads_blend_ = 0.0f;
    float fire_cooldown_ = 0.0f;
    float flame_pressure_ramp_ = 0.0f;
    float boil_emit_timer_ = 0.0f;
    float hammer_strike_timer_ = 0.0f;
    bool prev_fire_held_ = false;
    bool shell_pending_in_cycle_ = false;
    uint32_t shot_seq_ = 0;

    // Weapon persistent ammo & thermal state
    float fuel_pct_ = 100.0f;
    int flame_reserve_pct_ = 200;
    int ignition_cartridges_ = 5;

    int m91_mag_ = 5;
    int m91_reserve_ = 65;
    bool m91_reload_close_cue_sent_ = false;
    float m91_zoom_x_ = 4.0f;
    float m91_zero_m_ = 200.0f;

    int c96_mag_ = 20;
    int c96_reserve_ = 120;
    bool c96_full_auto_ = true;
    bool c96_bolt_locked_open_ = false;
    bool c96_reload_close_cue_sent_ = false;

    int maxim_mag_ = 250;
    int maxim_reserve_ = 500;
    float maxim_water_temp_c_ = 28.0f;
    float maxim_water_level_l_ = 4.5f;
    float maxim_wheel_angle_rad_ = 0.0f;
    float maxim_belt_shift_ = 0.0f;
    float maxim_recoil_kick_ = 0.0f;
    float m91_recoil_kick_ = 0.0f;
    float c96_barrel_kick_ = 0.0f;
    float c96_bolt_kick_ = 0.0f;
};

}  // namespace game::special_arsenal
