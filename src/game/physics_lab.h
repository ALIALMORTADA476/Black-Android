#pragma once
#include <atomic>
#include <cstdint>

namespace modsettings {
struct EnhancedOptions;
}

namespace game::physics_lab {

// Validated runtime multipliers synchronized lock-free from EnhancedOptions
// Classic profile preserves exact 2006 BLACK behavior (all multipliers == 1.0f).
struct RuntimeParams {
    std::atomic<float> explosion_force{1.0f};        // 0.25 .. 10.00
    std::atomic<float> impact_force{1.0f};           // 0.25 .. 10.00
    std::atomic<float> gravity_scale{1.0f};          // 0.05 .. 5.00
    std::atomic<float> debris_intensity{1.0f};       // 0.25 .. 6.00
    std::atomic<float> character_reaction{1.0f};     // 0.25 .. 8.00
    std::atomic<float> weapon_recoil{1.0f};          // 0.00 .. 4.00
    std::atomic<float> explosion_radius{1.0f};       // 0.50 .. 5.00
    std::atomic<float> ragdoll_launch{1.0f};         // 0.25 .. 10.00
    std::atomic<float> structural_destruction{1.0f}; // 0.25 .. 10.00
    std::atomic<float> prop_bounciness{1.0f};        // 0.25 .. 6.00
    std::atomic<float> camera_shockwave{1.0f};       // 0.00 .. 4.00
    std::atomic<float> physics_time_scale{1.0f};     // 0.20 .. 2.00
    std::atomic<bool> delirium_enabled{true};
    std::atomic<float> delirium_intensity{1.35f};    // 0.25 .. 3.00
    std::atomic<bool> delirium_blind_fire{true};
    std::atomic<float> delirium_agony_duration{1.10f}; // 0.00 .. 2.50s
};

struct TelemetrySnapshot {
    uint64_t blast_hits = 0;
    uint64_t impact_hits = 0;
    uint64_t gravity_steps = 0;
    uint64_t debris_steps = 0;
    uint64_t ragdoll_steps = 0;
    uint64_t recoil_kicks = 0;
    float active_gravity_y = -25.0f;
};

RuntimeParams& Params();

// Synchronizes runtime physics multipliers from mod_schema::EnhancedOptions (call after load / UI change / preset).
void SyncFromEnhanced(const modsettings::EnhancedOptions& enh);

// Reads live hit counters and active world gravity for console/log verification.
TelemetrySnapshot GetTelemetry();

}  // namespace game::physics_lab

