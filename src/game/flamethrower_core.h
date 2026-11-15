#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace game::flamethrower {

constexpr uint32_t kBmshMagic = 0x48534D42u;  // "BMSH"
constexpr uint32_t kBmshVersion = 1u;

#pragma pack(push, 1)
struct BmshHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t vertex_count;
    uint32_t index_count;
    float nozzle_pos[3];
    float pilot_pos[3];
    float bounds_min[3];
    float bounds_max[3];
};

struct BmshVertex {
    float x, y, z;
    float nx, ny, nz;
    uint32_t bgra;
};
#pragma pack(pop)

struct BmshMesh {
    std::vector<BmshVertex> vertices;
    std::vector<uint16_t> indices;
    float nozzle_pos[3] = {0.0f, 0.0f, 0.42f};
    float pilot_pos[3] = {0.0f, -0.02f, 0.40f};
    float bounds_min[3] = {};
    float bounds_max[3] = {};
};

bool ParseBmsh(const void* data, size_t size, BmshMesh& out);

struct VictimBurnState {
    uint32_t victim_ea = 0;
    float heat = 0.0f;           // 0..1 pre-ignition buildup
    bool ignited = false;
    float burn_timer = 0.0f;     // remaining burn duration (s)
    float direct_heat = 0.0f;    // recently bathed in live stream (s)
    float agony_cooldown = 0.0f; // time until next clutch/stumble reaction
    float char_factor = 0.0f;    // 0..0.9 visual soot/char
    bool was_dying = false;
    float fuel_soak = 0.0f;      // seconds of jet contact -> how much gel stuck
    float severity = 0.0f;       // 0.5..1 napalm load carried by the body
};

// One gobbet of burning thickened fuel. The jet simulation owns the single
// authoritative stream: the damage queries and the rendered flames both read
// these segments, so the visible jet is exactly what burns. Modelled on real
// napalm behaviour: 40-50 m/s muzzle velocity, heavy drag that stops the front
// around 16 m after ~0.8 s, gel that droops near the nozzle then rises once it
// has burned into gas, and fuel that pools and keeps burning on the floor.
struct JetSegment {
    float pos[3] = {};
    float vel[3] = {};
    float radius = 0.09f;
    float heat = 0.0f;  // 0..1 combustion intensity
    float age = 0.0f;
    float ttl = 1.45f;
    float spawn_heat = 0.0f;  // heat when it left the nozzle, fuel carried
};

struct VictimBurnTick {
    float damage = 0.0f;
    bool agony_flinch = false;
    uint32_t agony_clip = 0x1Eu;  // 0x1E = torso/stagger, 0x1F = face/shield
    bool emit_fire = false;
    bool emit_smoke = false;
    float fire_intensity = 0.0f;
    float char_factor = 0.0f;
};

class CoreSim {
  public:
    static constexpr size_t kMaxVictims = 64;
    static constexpr size_t kMaxJetSegments = 96;   // fuel in flight
    static constexpr size_t kMaxPoolSegments = 64;  // burning fuel on the floor
    static constexpr float kMaxRangeMeters = 16.5f;
    static constexpr float kReloadDurationSec = 2.10f;
    static constexpr float kFullBurnSeconds = 9.40f;   // full-pressure canister life
    static constexpr float kJetExitSpeed = 44.0f;      // m/s at the nozzle
    static constexpr float kJetDragK = 2.62f;          // 1/s, air drag on the gel
    static constexpr float kJetSegmentTtl = 1.45f;
    static constexpr float kJetSpawnRate = 58.0f;      // gobbets per second at full flow

    void Reset();

    bool IsActive() const noexcept { return active_; }
    uint32_t PreviousSlot() const noexcept { return prev_slot_; }
    void SetActive(bool on, uint32_t current_slot);
    uint32_t DeactivateAndRestoreSlot();

    void RefillFuel();
    void Step(float dt, bool trigger_held, bool reload_pressed, bool infinite_ammo);

    // Weapon pose feeding the jet. Called once per frame from the game thread
    // with the muzzle world point, its aim ray and the floor under the player.
    void SetMuzzle(const float pos[3], const float dir[3], float ground_y);

    float FuelPct() const noexcept { return fuel_pct_; }
    float Pressure() const noexcept { return pressure_; }
    bool IsFiring() const noexcept { return firing_; }
    bool IsReloading() const noexcept { return reloading_; }
    float ReloadProgress() const noexcept;
    float EquipProgress() const noexcept { return equip_anim_; }
    uint32_t IgnitionCount() const noexcept { return ignition_count_; }

    // Legacy static cone helper, kept for tooling/tests. The live model is the
    // travelled jet, queried through StreamExposure().
    static float ConeIntensity(const float origin[3], const float forward[3],
                               const float target_pos[3]) noexcept;

    // How hard the live jet is currently bathing a point (0..1). This is the
    // only damage gate: no travel time means no heat at range.
    float StreamExposure(const float target_pos[3], float target_radius) const;

    size_t CopyJetSegments(JetSegment* out, size_t max_segments) const;
    size_t CopyPoolSegments(JetSegment* out, size_t max_segments) const;
    bool HasActiveJet() const noexcept { return jet_count_ != 0 || pool_count_ != 0; }
    float JetFrontDistance() const noexcept;  // furthest burning fuel along the aim ray

    void ExposeEnemy(uint32_t victim_ea, float intensity, float dt);
    VictimBurnTick TickVictim(uint32_t victim_ea, float dt, bool is_dying);
    VictimBurnState GetVictimState(uint32_t victim_ea) const noexcept;
    void ClearVictims();

    // Thread-safe procedural stereo audio render for mixer thread.
    void RenderAudio(float* stereo_out, uint32_t frames, uint32_t sample_rate);

  private:
    VictimBurnState* FindOrAllocVictim(uint32_t victim_ea) noexcept;
    void SyncAudioAtomics() noexcept;
    void StepJet(float dt) noexcept;
    void SpawnJetSegment(float pressure) noexcept;
    void SpawnPoolSegment(const float pos[3], float radius, float heat, float ttl) noexcept;
    float RandJet() noexcept;

    bool active_ = false;
    uint32_t prev_slot_ = 0;
    float equip_anim_ = 0.0f;
    float fuel_pct_ = 100.0f;
    float pressure_ = 0.0f;
    bool firing_ = false;
    bool reloading_ = false;
    float reload_elapsed_ = 0.0f;
    bool reload_refilled_ = false;
    uint32_t ignition_count_ = 0;

    // Jet / muzzle state
    float muzzle_pos_[3] = {};
    float muzzle_dir_[3] = {0.0f, 0.0f, 1.0f};
    float aim_dir_[3] = {0.0f, 0.0f, 1.0f};
    bool muzzle_valid_ = false;
    float ground_y_ = 0.0f;
    bool has_ground_ = false;
    float emit_accum_ = 0.0f;
    float jet_phase_ = 0.0f;
    uint32_t rng_jet_ = 0x9E3779B9u;
    std::array<JetSegment, kMaxJetSegments> jet_{};
    uint32_t jet_count_ = 0;
    std::array<JetSegment, kMaxPoolSegments> pool_{};
    uint32_t pool_count_ = 0;

    std::array<VictimBurnState, kMaxVictims> victims_{};
    uint32_t victim_cursor_ = 0;

    // Lock-free mirror for audio thread + DSP filter state
    std::atomic<bool> aud_active_{false};
    std::atomic<bool> aud_firing_{false};
    std::atomic<bool> aud_reloading_{false};
    std::atomic<float> aud_pressure_{0.0f};
    std::atomic<float> aud_reload_t_{0.0f};
    std::atomic<uint32_t> aud_ignite_seq_{0};

    uint32_t audio_seen_ignite_ = 0;
    float audio_whoosh_env_ = 0.0f;
    uint32_t rng_l_ = 0x13579BDFu;
    uint32_t rng_r_ = 0x2468ACE0u;
    float pink_l_[3] = {};
    float pink_r_[3] = {};
    float lp_l_ = 0.0f;
    float lp_r_ = 0.0f;
    float hp_l_ = 0.0f;
    float hp_r_ = 0.0f;
    float pilot_lp_ = 0.0f;
    float lfo_phase_ = 0.0f;
};

}  // namespace game::flamethrower
