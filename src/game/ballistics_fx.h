#pragma once

#include <cstdint>

namespace game {
namespace ballistics {

enum class WeaponClass : uint8_t {
    Pistol = 0,
    Smg = 1,
    Rifle = 2,
    Shotgun = 3,
    Sniper = 4,
    Lmg = 5,
    Launcher = 6,
    Unknown = 7
};

struct TelemetrySnapshot {
    bool tracers_enabled = true;
    bool smoke_enabled = true;
    uint32_t active_tracers = 0;
    uint32_t active_smoke_nodes = 0;
    uint32_t max_tracers_cap = 96;
    uint32_t max_smoke_cap = 192;
    uint64_t total_shots_observed = 0;
    uint64_t total_tracers_emitted = 0;
    uint64_t total_hitscan_synced = 0;
    uint64_t total_smoke_emitted = 0;
    uint64_t total_culled = 0;
    uint32_t active_shells = 0;
    uint64_t total_shells_ejected = 0;
};

struct ShellCasingProfile {
    bool ejects = false;
    float length_m = 0.045f;
    float radius_m = 0.008f;
    float eject_speed_right = 2.6f;
    float eject_speed_up = 1.9f;
    float eject_speed_back = 0.7f;
    bool red_shotgun_hull = false;
};

struct ShellCasingBody {
    bool active = false;
    float pos[3] = {};
    float vel[3] = {};
    float spin_rad = 0.0f;
    float spin_vel = 0.0f;
    float ground_y = 0.0f;
    float age = 0.0f;
    float ttl = 3.2f;
    uint8_t bounces = 0;
};

// Pure testable classification, shell casing ballistics, & frequency rules
WeaponClass ClassifyWeaponCode(const char* code3);
bool ShouldEmitTracer(uint32_t freq_mode, uint64_t shot_seq, WeaponClass cls, bool suppressed, bool is_player);
bool ShouldEjectShellCasing(const char* code3, bool alt_fire = false);
ShellCasingProfile GetShellCasingProfile(WeaponClass cls);
bool StepShellCasingPhysics(ShellCasingBody& body, float dt);
bool IsPerformancePresetActive();

// Game camera & muzzle hooks (called on game thread inside sub_00176F60 & sub_001D2860)
void CaptureSceneCamera();
void NotifyMuzzleMatrix(uint32_t owner_ptr, const float mat4x4[16]);
void OnPlayerShellEject(const char* weapon_code, bool alt_fire = false);

// Physical bullet lifecycle hooks (read-only observers of guest pool [0x002D12D0]+0x6E0)
void OnPhysicalBulletSpawn(uint32_t slot_ea, uint32_t fire_pkt, const float origin[3], const float dir[3],
                           float speed, float range, uint8_t flags);
void OnPhysicalBulletStep(uint32_t slot_ea, const float prev_pos[3], const float new_pos[3], bool active_after);
void OnPhysicalBulletHit(uint32_t slot_ea, const float hit_pos[3]);

// Synchronized hitscan / native tracer observer (from sub_00168E20 callers)
void OnHitscanShot(const float start[3], const float end[3], uint32_t caller_ret_addr, bool is_player);

// Enemy blood splatter, pouring stream & ground pool observer (from sub_001E74B0)
void OnEnemyBloodHit(uint32_t victim_ea, const float hit_pos[3], float ground_y,
                     float wound_y_offset, const float hit_dir[3],
                     float impact_force, bool is_headshot, bool is_lethal);

// Explosion fireball, glowing sparks/embers, shockwave dust & heavy smoke (from sub_001E90B0 / sub_001E5410)
void OnExplosionDetonate(const float pos[3], float radius, float force);

// Flamethrower volumetric jet, pilot flame bead, and burning enemy fire/smoke/embers
bool GetSceneCameraVectors(float eye[3], float right[3], float up[3], float forward[3], float view[16], float proj[16]);

// One simulated gobbet of the napalm stream, handed over by the weapon sim so
// the drawn flame is literally the flame that burns.
struct FlameStreamPoint {
    float pos[3];
    float vel[3];
    float radius;
    float heat;
};

// Publish the current stream (jet in flight + fuel pooled on the floor) for this
// frame. Called once per frame from the flamethrower while equipped.
void SetFlameStream(const FlameStreamPoint* jet, uint32_t jet_count,
                    const FlameStreamPoint* pools, uint32_t pool_count, float ground_y);
void EmitPilotFlameGlow(const float pilot_world[3], float intensity);
void EmitBurnVictimFlames(const float world_pos[3], float ground_y, float fire_intensity,
                          bool emit_fire, bool emit_smoke, bool is_dying);
void EmitWoundDrip(const float pos[3], float ground_y);
void EmitExplosionDismemberment(const float victim_pos[3], const float blast_pos[3],
                                float blast_radius, uint8_t limbs_mask);
void EmitSpecialShotMuzzleAndTrace(const float muzzle_pos[3], const float dir[3],
                                   const float hit_end[3], float caliber_scale,
                                   bool spawn_smoke, bool spark_impact);
void EmitMauserMuzzleShockwave(const float muzzle_pos[3], const float dir[3],
                               float ground_y, float intensity = 1.0f);
void EmitMaximSteamPlume(const float jacket_pos[3], float steam_intensity);
// While true, guest physical bullets spawned by Special Arsenal weapons skip the duplicate tracer/smoke FX.
void SetSuppressPhysicalSpawnFx(bool suppress);
void RegisterThermalSlot(uint32_t slot_ea, const float dir[3]);
bool InThermalImpact();
bool BeginThermalImpact(uint32_t slot_ea);
bool BeginExternalThermalImpact();
void EndThermalImpact(bool prev);

// Frame update + true 3D Z-buffered world render (called inside sub_00176F60 after kAlphaDrawList)
void RenderWorldScene();

// Reset buffers on map load / mode reset
void ResetAll();

TelemetrySnapshot GetTelemetry();

}  // namespace ballistics

namespace haptics {

struct WeaponHapticPulse {
  uint32_t duration_ms = 0;
  uint8_t amplitude = 0;
  uint32_t cooldown_ms = 0;
  uint8_t priority = 0;  // 1=light tap, 2=medium punch, 3=heavy ordnance, 4=explosion
  bool triggered = false;
};

WeaponHapticPulse ComputeWeaponHapticPulse(const char* weapon_code,
                                           bool suppressed,
                                           bool alt_fire = false,
                                           bool rapid_fire_cheat = false);

WeaponHapticPulse ComputeExplosionHapticPulse(float distance_m);

class HapticDispatcher {
 public:
  void Reset();
  bool TrySchedule(uint64_t now_ms, const WeaponHapticPulse& candidate,
                   bool enabled, WeaponHapticPulse* out_emit);
  uint64_t last_emit_ms() const { return last_emit_ms_; }

 private:
  uint64_t last_emit_ms_ = 0;
  uint64_t busy_until_ms_ = 0;
  uint32_t last_cooldown_ms_ = 0;
  uint8_t last_priority_ = 0;
};

void DispatchWeaponHaptic(const char* weapon_code, bool suppressed,
                          bool alt_fire = false, bool rapid_fire_cheat = false);
void DispatchExplosionHaptic(const float world_pos[3]);

}  // namespace haptics
}  // namespace game
