#include "game/time_stop.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "app/app.h"
#include "common/menu.h"
#include "common/mod_schema.h"
#include "common/options.h"
#include "common/time_stop_state.h"
#include "common/translations.h"
#include "core/log.h"
#include "game/ballistics_fx.h"
#include "game/flamethrower.h"
#include "game/layout.h"
#include "imgui.h"
#include "xdk/hle.h"

using namespace kernel;

namespace game::time_stop {
namespace {

constexpr uint32_t kWorldStateOffset = 0x5AE0;
constexpr uint32_t kWorldClockOffset = 0x20;
constexpr uint32_t kWorldGameplayState = 0x1C;

constexpr uint32_t kWeaponMgrPtr = 0x002D12D0;
constexpr uint32_t kBulletPoolOffset = 0x6E0;
constexpr uint32_t kBulletSlotCount = 16;
constexpr uint32_t kBulletSlotStride = 0x90;
constexpr uint32_t kBulletCurPosOffset = 0x50;
constexpr uint32_t kBulletDirOffset = 0x60;
constexpr uint32_t kBulletSpeedOffset = 0x7C;
constexpr uint32_t kBulletSpawnTimeOffset = 0x84;
constexpr uint32_t kBulletActiveOffset = 0x88;

constexpr uint32_t kProjPoolPtr = 0x002D1998;
constexpr uint32_t kProjSlotStride = 0x120;
constexpr uint32_t kPoolASlots = 32;
constexpr uint32_t kPoolBOffset = 0x2400;
constexpr uint32_t kPoolBSlots = 16;
constexpr uint32_t kPoolAActiveFlags = 0x3700;
constexpr uint32_t kPoolBActiveFlags = 0x3720;
constexpr uint32_t kProjPosOffset = 0xD0;  // row 3 of +0xA0 4x4 transform matrix

constexpr uint32_t kSolverGlobalPtr = 0x002D45F4;
constexpr uint32_t kSolverSlotStride = 0x50;
constexpr uint32_t kSlotOwnerOffset = 0x30;
constexpr uint32_t kSlotHolderOffset = 0x34;
constexpr uint32_t kHolderInnerOffset = 0x0C;
constexpr uint32_t kInnerBodyOffset = 0x58;
constexpr uint32_t kCharKindOffset = 0xC4;
constexpr uint32_t kCharPosOffset = 0xA0;
constexpr uint32_t kOwnerTransformOffset = 0x70;

constexpr uint32_t kBodyVelLinOffset = 0x20;    // float[3] (+0x2C is doubly-linked next!)
constexpr uint32_t kBodyNextOffset = 0x2C;
constexpr uint32_t kBodyVelAngOffset = 0x30;    // float[3] (+0x3C is doubly-linked prev!)
constexpr uint32_t kBodyTransformOffset = 0x40; // 64B world matrix + speed/inertia word
constexpr uint32_t kBodyVelPrevOffset = 0x80;   // float[3] (+0x8C is flags)
constexpr uint32_t kBodyForceAccOffset = 0x90;  // float[3]
constexpr uint32_t kBodyRestEnergyOffset = 0x9C;
constexpr uint32_t kBodyTorqueAccOffset = 0xA0; // float[3]
constexpr uint32_t kBodyRestTicksOffset = 0xAC;
constexpr uint32_t kBodyStructSize = 0xB0;

struct SolverSlotTable {
    uint32_t slots_offset;
    uint32_t flags_offset;
    uint32_t count;
};

constexpr SolverSlotTable kSolverTables[] = {
    {0x50u, 0x22B0u, 0x6Eu},
    {0x2320u, 0x2960u, 0x14u},
};

struct PhysBodyList {
    uint32_t sentinel_offset;
    uint32_t head_offset;
};

constexpr PhysBodyList kPhysActiveLists[] = {
    {0x110u, 0x13Cu},
    {0x1C0u, 0x1ECu},
    {0x270u, 0x29Cu},
};

struct QueuedBullet {
    uint32_t mgr = 0;
    uint32_t fire_pkt = 0;
    float muzzle[4] = {};
    float dir[4] = {};
    float range = 0.0f;
    float speed = 0.0f;
    uint8_t flags = 0;
};

struct BulletMarker {
    float pos[3] = {};
    float dir[3] = {};
    uint32_t kind = 0;  // 0=bullet, 1=grenade, 2=rocket
};

struct FrozenBodyState {
    uint32_t body = 0;
    float vel_lin[3] = {};
    float vel_ang[3] = {};
    uint8_t transform[64] = {};
    float vel_prev[3] = {};
    float force_acc[3] = {};
    float rest_energy = 0.0f;
    float torque_acc[3] = {};
    uint32_t rest_ticks = 0;
};

struct FrozenOwnerState {
    uint32_t owner = 0;
    uint8_t transform[64] = {};
};

std::mutex g_mutex;
common::TimeStopStateMachine g_sm;
std::atomic<bool> g_frozen_atomic{false};
std::atomic<bool> g_gameplay_atomic{false};

std::atomic<uint64_t> g_skipped_ai{0};
std::atomic<uint64_t> g_skipped_physics{0};
std::atomic<uint64_t> g_skipped_particles{0};
std::atomic<uint32_t> g_frozen_enemies_atomic{0};
std::atomic<uint32_t> g_frozen_bodies_atomic{0};
std::atomic<uint32_t> g_frozen_bullets_atomic{0};
std::atomic<uint32_t> g_frozen_grenades_atomic{0};
std::atomic<uint32_t> g_frozen_rockets_atomic{0};

std::vector<QueuedBullet> g_overflow_queue;
std::vector<BulletMarker> g_live_markers;
std::vector<uint32_t> g_frame_enemies;
uint32_t g_last_frozen_enemy_count = 0;

std::mutex g_phys_freeze_mutex;
std::unordered_map<uint32_t, FrozenBodyState> g_frozen_bodies;
std::unordered_map<uint32_t, FrozenOwnerState> g_frozen_owners;

bool g_has_prev_world_clock = false;
float g_prev_world_clock = 0.0f;
std::chrono::steady_clock::time_point g_last_tick_time{};
bool g_has_tick_time = false;
std::chrono::steady_clock::time_point g_last_heartbeat_time{};
bool g_has_freeze_start_pos = false;
float g_freeze_start_player_pos[3] = {};

bool IsValidGuestAddr(uint32_t va, uint32_t size) {
    if (va < mem::kUserLow || size == 0 || va > mem::kUserHigh - size) return false;
    return mem::IsCommitted(va) && mem::IsCommitted(va + size - 1u);
}

bool IsValidAnyGuestAddr(uint32_t va, uint32_t size) {
    if (va < 0x10000u || size == 0 || (uint64_t(va) + size) > 0x100000000ULL) return false;
    return mem::IsCommitted(va) && mem::IsCommitted(va + size - 1u);
}

bool ReadPlayerPos(float out_pos[3]) {
    const uint32_t world = R32(layout::kWorld);
    if (!IsValidGuestAddr(world, layout::kWorldPlayer + kCharPosOffset + 12u)) return false;
    const uint32_t player = world + layout::kWorldPlayer;
    for (int i = 0; i < 3; ++i) {
        float v = G<float>(player + kCharPosOffset + uint32_t(i) * 4u);
        if (!std::isfinite(v)) return false;
        out_pos[i] = v;
    }
    return true;
}

float PlayerDisplacementSinceFreezeStart() {
    float cur[3] = {};
    if (!g_has_freeze_start_pos || !ReadPlayerPos(cur)) return 0.0f;
    const float dx = cur[0] - g_freeze_start_player_pos[0];
    const float dy = cur[1] - g_freeze_start_player_pos[1];
    const float dz = cur[2] - g_freeze_start_player_pos[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

common::TimeStopConfig CurrentConfig() {
    const auto& enh = modsettings::Registry().GetEnhanced();
    common::TimeStopConfig cfg;
    cfg.mode = (enh.time_stop_mode == 1) ? common::TimeStopMode::Hold : common::TimeStopMode::Toggle;
    cfg.max_duration_sec = enh.time_stop_max_duration;
    cfg.cooldown_sec = enh.time_stop_cooldown;
    return cfg;
}

void SyncSchemaEnabledFlag(bool frozen_now) {
    auto& reg = modsettings::Registry();
    if (reg.GetEnhanced().time_stop_enabled != frozen_now) {
        reg.GetEnhanced().time_stop_enabled = frozen_now;
    }
}

void LogTransition(const char* event, bool frozen_now) {
    float cur[3] = {};
    const bool has_pos = ReadPlayerPos(cur);
    if (frozen_now && has_pos) {
        std::memcpy(g_freeze_start_player_pos, cur, sizeof(cur));
        g_has_freeze_start_pos = true;
        g_last_heartbeat_time = std::chrono::steady_clock::now();
    }
    const float moved = PlayerDisplacementSinceFreezeStart();
    if (!frozen_now) {
        g_has_freeze_start_pos = false;
    }
    LOGR("[TIME_STOP] %s | frozen=%d enemies=%u bodies=%u bullets=%u grenades=%u rockets=%u overflow=%zu moved=%.2f",
         event, frozen_now ? 1 : 0, g_last_frozen_enemy_count,
         g_frozen_bodies_atomic.load(std::memory_order_relaxed),
         g_frozen_bullets_atomic.load(std::memory_order_relaxed),
         g_frozen_grenades_atomic.load(std::memory_order_relaxed),
         g_frozen_rockets_atomic.load(std::memory_order_relaxed),
         g_overflow_queue.size(), moved);
}

void WriteFloat3(uint32_t addr, const float v[3]) {
    for (uint32_t i = 0; i < 3; ++i) {
        G<float>(addr + i * 4u) = v[i];
    }
}

void ReadFloat3(uint32_t addr, float v[3]) {
    for (uint32_t i = 0; i < 3; ++i) {
        v[i] = G<float>(addr + i * 4u);
    }
}

void ZeroBodyKinematics(uint32_t body) {
    static constexpr float kZero[3] = {0.0f, 0.0f, 0.0f};
    WriteFloat3(body + kBodyVelLinOffset, kZero);
    WriteFloat3(body + kBodyVelAngOffset, kZero);
    WriteFloat3(body + kBodyVelPrevOffset, kZero);
    WriteFloat3(body + kBodyForceAccOffset, kZero);
    WriteFloat3(body + kBodyTorqueAccOffset, kZero);
}

void CollectPhysWorldBodies(uint32_t phys_world, std::vector<uint32_t>& out_bodies,
                            std::unordered_set<uint32_t>& out_set) {
    if (!IsValidGuestAddr(phys_world, 0x2A0u)) return;
    for (const auto& list : kPhysActiveLists) {
        const uint32_t sentinel = phys_world + list.sentinel_offset;
        uint32_t cur = R32(phys_world + list.head_offset);
        for (uint32_t step = 0; step < 4096u && cur != 0 && cur != sentinel; ++step) {
            if (!IsValidGuestAddr(cur, kBodyStructSize)) break;
            if (!out_set.insert(cur).second) break;
            out_bodies.push_back(cur);
            cur = R32(cur + kBodyNextOffset);
        }
    }
}

void CollectSolverParticipants(uint32_t solver, std::unordered_set<uint32_t>& out_player_bodies,
                               std::vector<uint32_t>& out_non_player_owners) {
    if (!IsValidGuestAddr(solver, 0x2974u)) return;
    const uint32_t world = R32(layout::kWorld);
    const uint32_t player = IsValidGuestAddr(world, layout::kWorldPlayer + 4u) ? (world + layout::kWorldPlayer) : 0u;

    for (const auto& table : kSolverTables) {
        for (uint32_t i = 0; i < table.count; ++i) {
            if (G<uint8_t>(solver + table.flags_offset + i) == 0) continue;
            const uint32_t slot = solver + table.slots_offset + i * kSolverSlotStride;
            const uint32_t owner = R32(slot + kSlotOwnerOffset);
            const uint32_t holder = R32(slot + kSlotHolderOffset);
            uint32_t body = 0;
            if (IsValidGuestAddr(holder, kHolderInnerOffset + 4u)) {
                const uint32_t inner = R32(holder + kHolderInnerOffset);
                if (IsValidGuestAddr(inner, kInnerBodyOffset + 4u)) {
                    body = R32(inner + kInnerBodyOffset);
                }
            }
            const bool is_player = (owner != 0) && ((player != 0 && owner == player) ||
                                   (IsValidGuestAddr(owner + kCharKindOffset, 4u) && R32(owner + kCharKindOffset) == 2u));
            if (is_player) {
                if (body != 0) out_player_bodies.insert(body);
            } else if (owner != 0 && IsValidGuestAddr(owner + kOwnerTransformOffset, 64u)) {
                out_non_player_owners.push_back(owner);
            }
        }
    }
}

void PrepareSelectivePhysicsSolve(uint32_t solver, std::unordered_set<uint32_t>& active_bodies,
                                  std::vector<uint32_t>& active_owners) {
    if (!IsValidGuestAddr(solver, 4u)) return;
    const uint32_t phys_world = R32(solver);

    std::unordered_set<uint32_t> player_bodies;
    CollectSolverParticipants(solver, player_bodies, active_owners);

    std::vector<uint32_t> bodies;
    CollectPhysWorldBodies(phys_world, bodies, active_bodies);

    std::lock_guard lock(g_phys_freeze_mutex);
    for (uint32_t owner : active_owners) {
        auto it = g_frozen_owners.find(owner);
        if (it == g_frozen_owners.end()) {
            FrozenOwnerState st;
            st.owner = owner;
            std::memcpy(st.transform, kernel::g_base + owner + kOwnerTransformOffset, 64u);
            g_frozen_owners.emplace(owner, st);
        } else {
            std::memcpy(kernel::g_base + owner + kOwnerTransformOffset, it->second.transform, 64u);
        }
    }

    for (uint32_t body : bodies) {
        if (player_bodies.count(body)) {
            g_frozen_bodies.erase(body);
            continue;
        }
        auto it = g_frozen_bodies.find(body);
        if (it == g_frozen_bodies.end()) {
            FrozenBodyState st;
            st.body = body;
            ReadFloat3(body + kBodyVelLinOffset, st.vel_lin);
            ReadFloat3(body + kBodyVelAngOffset, st.vel_ang);
            std::memcpy(st.transform, kernel::g_base + body + kBodyTransformOffset, 64u);
            ReadFloat3(body + kBodyVelPrevOffset, st.vel_prev);
            ReadFloat3(body + kBodyForceAccOffset, st.force_acc);
            st.rest_energy = G<float>(body + kBodyRestEnergyOffset);
            ReadFloat3(body + kBodyTorqueAccOffset, st.torque_acc);
            st.rest_ticks = R32(body + kBodyRestTicksOffset);
            g_frozen_bodies.emplace(body, st);
        } else {
            std::memcpy(kernel::g_base + body + kBodyTransformOffset, it->second.transform, 64u);
        }
        ZeroBodyKinematics(body);
    }
    g_frozen_bodies_atomic.store(uint32_t(g_frozen_bodies.size()), std::memory_order_relaxed);
}

void LockAfterSelectivePhysicsSolve(uint32_t solver, const std::vector<uint32_t>& active_owners) {
    std::vector<uint32_t> bodies;
    std::unordered_set<uint32_t> live_bodies;
    if (IsValidGuestAddr(solver, 4u)) {
        CollectPhysWorldBodies(R32(solver), bodies, live_bodies);
    }

    std::lock_guard lock(g_phys_freeze_mutex);
    for (uint32_t owner : active_owners) {
        auto it = g_frozen_owners.find(owner);
        if (it != g_frozen_owners.end() && IsValidGuestAddr(owner + kOwnerTransformOffset, 64u)) {
            std::memcpy(kernel::g_base + owner + kOwnerTransformOffset, it->second.transform, 64u);
        }
    }
    for (const auto& [body, st] : g_frozen_bodies) {
        if (live_bodies.count(body) && IsValidGuestAddr(body, kBodyStructSize)) {
            std::memcpy(kernel::g_base + body + kBodyTransformOffset, st.transform, 64u);
            ZeroBodyKinematics(body);
        }
    }
}

void RestoreFrozenPhysicsOnResume() {
    std::unordered_map<uint32_t, FrozenBodyState> bodies_to_restore;
    {
        std::lock_guard lock(g_phys_freeze_mutex);
        if (g_frozen_bodies.empty() && g_frozen_owners.empty()) {
            g_frozen_bodies_atomic.store(0, std::memory_order_relaxed);
            return;
        }
        bodies_to_restore.swap(g_frozen_bodies);
        g_frozen_owners.clear();
        g_frozen_bodies_atomic.store(0, std::memory_order_relaxed);
    }

    const uint32_t solver = R32(kSolverGlobalPtr);
    const uint32_t phys_world = IsValidGuestAddr(solver, 4u) ? R32(solver) : 0u;
    std::vector<uint32_t> bodies;
    std::unordered_set<uint32_t> live_bodies;
    CollectPhysWorldBodies(phys_world, bodies, live_bodies);

    for (const auto& [body, st] : bodies_to_restore) {
        if (!live_bodies.count(body) || !IsValidGuestAddr(body, kBodyStructSize)) continue;
        std::memcpy(kernel::g_base + body + kBodyTransformOffset, st.transform, 64u);
        WriteFloat3(body + kBodyVelLinOffset, st.vel_lin);
        WriteFloat3(body + kBodyVelAngOffset, st.vel_ang);
        WriteFloat3(body + kBodyVelPrevOffset, st.vel_prev);
        WriteFloat3(body + kBodyForceAccOffset, st.force_acc);
        G<float>(body + kBodyRestEnergyOffset) = st.rest_energy;
        WriteFloat3(body + kBodyTorqueAccOffset, st.torque_acc);
        G<uint32_t>(body + kBodyRestTicksOffset) = st.rest_ticks;
    }
}

void MaybeRestoreFrozenPhysics() {
    if (g_frozen_bodies_atomic.load(std::memory_order_relaxed) != 0) {
        RestoreFrozenPhysicsOnResume();
    }
}

void CompensateBulletLifetimes(float world_dt) {
    if (world_dt <= 0.0f) return;
    uint32_t mgr = R32(kWeaponMgrPtr);
    if (!mgr) return;
    for (uint32_t i = 0; i < kBulletSlotCount; ++i) {
        uint32_t slot = mgr + kBulletPoolOffset + i * kBulletSlotStride;
        if (G<uint8_t>(slot + kBulletActiveOffset) != 0) {
            G<float>(slot + kBulletSpawnTimeOffset) += world_dt;
        }
    }
}

uint32_t CountActiveBullets() {
    uint32_t mgr = R32(kWeaponMgrPtr);
    if (!mgr) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < kBulletSlotCount; ++i) {
        uint32_t slot = mgr + kBulletPoolOffset + i * kBulletSlotStride;
        if (G<uint8_t>(slot + kBulletActiveOffset) != 0) ++count;
    }
    return count;
}

void SnapshotActiveProjectilesLocked() {
    g_live_markers.clear();
    uint32_t bcount = 0, gcount = 0, rcount = 0;

    uint32_t mgr = R32(kWeaponMgrPtr);
    if (mgr) {
        for (uint32_t i = 0; i < kBulletSlotCount; ++i) {
            uint32_t slot = mgr + kBulletPoolOffset + i * kBulletSlotStride;
            if (G<uint8_t>(slot + kBulletActiveOffset) == 0) continue;
            ++bcount;
            BulletMarker mk;
            mk.kind = 0;
            mk.pos[0] = G<float>(slot + kBulletCurPosOffset + 0);
            mk.pos[1] = G<float>(slot + kBulletCurPosOffset + 4);
            mk.pos[2] = G<float>(slot + kBulletCurPosOffset + 8);
            mk.dir[0] = G<float>(slot + kBulletDirOffset + 0);
            mk.dir[1] = G<float>(slot + kBulletDirOffset + 4);
            mk.dir[2] = G<float>(slot + kBulletDirOffset + 8);
            g_live_markers.push_back(mk);
        }
    }

    uint32_t proj = R32(kProjPoolPtr);
    if (proj) {
        for (uint32_t i = 0; i < kPoolASlots; ++i) {
            if (G<uint8_t>(proj + kPoolAActiveFlags + i) == 0) continue;
            ++gcount;
            uint32_t obj = proj + i * kProjSlotStride;
            BulletMarker mk;
            mk.kind = 1;
            mk.pos[0] = G<float>(obj + kProjPosOffset + 0);
            mk.pos[1] = G<float>(obj + kProjPosOffset + 4);
            mk.pos[2] = G<float>(obj + kProjPosOffset + 8);
            g_live_markers.push_back(mk);
        }
        for (uint32_t i = 0; i < kPoolBSlots; ++i) {
            if (G<uint8_t>(proj + kPoolBActiveFlags + i) == 0) continue;
            ++rcount;
            uint32_t obj = proj + kPoolBOffset + i * kProjSlotStride;
            BulletMarker mk;
            mk.kind = 2;
            mk.pos[0] = G<float>(obj + kProjPosOffset + 0);
            mk.pos[1] = G<float>(obj + kProjPosOffset + 4);
            mk.pos[2] = G<float>(obj + kProjPosOffset + 8);
            g_live_markers.push_back(mk);
        }
    }

    g_frozen_bullets_atomic.store(bcount, std::memory_order_relaxed);
    g_frozen_grenades_atomic.store(gcount, std::memory_order_relaxed);
    g_frozen_rockets_atomic.store(rcount, std::memory_order_relaxed);
}

}  // namespace

bool IsFrozen() { return g_frozen_atomic.load(std::memory_order_relaxed); }

bool IsActiveGameplay() { return g_gameplay_atomic.load(std::memory_order_relaxed); }

void SetMenuEnabled(bool enable) {
    bool resumed = false;
    {
        std::lock_guard lock(g_mutex);
        const bool was = g_sm.IsFrozen();
        const bool gameplay = IsActiveGameplay();
        const auto cfg = CurrentConfig();
        if (enable) {
            g_sm.SetMenuPinned(true);
            g_sm.SetMenuEnabled(true, cfg, gameplay);
        } else {
            g_sm.ForceResume(false);
        }
        const bool now = g_sm.IsFrozen();
        g_frozen_atomic.store(now, std::memory_order_relaxed);
        SyncSchemaEnabledFlag(now);
        resumed = (was && !now);
        if (was != now) {
            LogTransition(enable ? "MENU_ENABLE" : "MENU_DISABLE", now);
        }
    }
    if (resumed) RestoreFrozenPhysicsOnResume();
}

void ForceResume(bool apply_cooldown) {
    bool resumed = false;
    {
        std::lock_guard lock(g_mutex);
        const bool was = g_sm.IsFrozen();
        const auto cfg = CurrentConfig();
        g_sm.ForceResume(apply_cooldown, cfg.cooldown_sec);
        g_frozen_atomic.store(false, std::memory_order_relaxed);
        SyncSchemaEnabledFlag(false);
        resumed = was;
        if (was) {
            LogTransition("EMERGENCY_RESUME", false);
        }
    }
    if (resumed) RestoreFrozenPhysicsOnResume();
}

void OnTriggerPressed() {
    bool resumed = false;
    {
        std::lock_guard lock(g_mutex);
        const bool was = g_sm.IsFrozen();
        const bool gameplay = IsActiveGameplay();
        const auto cfg = CurrentConfig();
        if (cfg.mode == common::TimeStopMode::Hold) {
            g_sm.SetMenuPinned(false);
            g_sm.SetHoldInput(true, cfg, gameplay);
        } else {
            g_sm.SetMenuPinned(false);
            g_sm.OnTriggerPressed(cfg, gameplay);
        }
        const bool now = g_sm.IsFrozen();
        g_frozen_atomic.store(now, std::memory_order_relaxed);
        SyncSchemaEnabledFlag(now);
        resumed = (was && !now);
        if (was != now) {
            LogTransition("TRIGGER", now);
        }
    }
    if (resumed) RestoreFrozenPhysicsOnResume();
}

void SetHoldInput(bool held) {
    bool resumed = false;
    {
        std::lock_guard lock(g_mutex);
        const bool was = g_sm.IsFrozen();
        const bool gameplay = IsActiveGameplay();
        const auto cfg = CurrentConfig();
        g_sm.SetHoldInput(held, cfg, gameplay);
        const bool now = g_sm.IsFrozen();
        g_frozen_atomic.store(now, std::memory_order_relaxed);
        SyncSchemaEnabledFlag(now);
        resumed = (was && !now);
        if (was != now) {
            LogTransition(held ? "HOLD_DOWN" : "HOLD_UP", now);
        }
    }
    if (resumed) RestoreFrozenPhysicsOnResume();
}

void RecordFrozenEnemy(uint32_t enemy_ptr) {
    if (!enemy_ptr) return;
    std::lock_guard lock(g_mutex);
    if (std::find(g_frame_enemies.begin(), g_frame_enemies.end(), enemy_ptr) == g_frame_enemies.end()) {
        g_frame_enemies.push_back(enemy_ptr);
    }
}

void RecordSkippedAiTick() { g_skipped_ai.fetch_add(1, std::memory_order_relaxed); }
void RecordSkippedPhysicsTick() { g_skipped_physics.fetch_add(1, std::memory_order_relaxed); }
void RecordSkippedParticleTick() { g_skipped_particles.fetch_add(1, std::memory_order_relaxed); }

bool TryQueueOverflowBullet(uint32_t mgr, uint32_t muzzle_ea, uint32_t fire_pkt, uint32_t dir_ea,
                            float range, float speed, uint8_t flags, uint32_t caller_esp) {
    if (!IsFrozen() || !IsActiveGameplay()) return false;
    const uint32_t expected_mgr = R32(kWeaponMgrPtr);
    if (mgr == 0 || mgr != expected_mgr || CountActiveBullets() < kBulletSlotCount) return false;
    if (fire_pkt < 0x10000u) return false;
    const int64_t diff = int64_t(fire_pkt) - int64_t(caller_esp);
    if (diff >= -0x4000 && diff <= 0x4000) return false;

    std::lock_guard lock(g_mutex);
    if (g_overflow_queue.size() >= 64) return false;
    QueuedBullet q;
    q.mgr = mgr;
    q.fire_pkt = fire_pkt;
    for (int i = 0; i < 4; ++i) {
        q.muzzle[i] = G<float>(muzzle_ea + i * 4);
        q.dir[i] = G<float>(dir_ea + i * 4);
    }
    q.range = range;
    q.speed = speed;
    q.flags = flags;
    g_overflow_queue.push_back(q);
    return true;
}

TelemetrySnapshot GetTelemetry() {
    std::lock_guard lock(g_mutex);
    const auto cfg = CurrentConfig();
    TelemetrySnapshot snap;
    snap.frozen = g_sm.IsFrozen();
    snap.on_cooldown = g_sm.IsOnCooldown();
    snap.in_gameplay = IsActiveGameplay();
    snap.elapsed_sec = g_sm.ActiveElapsedSec();
    snap.remaining_sec = g_sm.RemainingDurationSec(cfg);
    snap.cooldown_sec = g_sm.CooldownRemainingSec();
    snap.frozen_enemies = g_last_frozen_enemy_count;
    snap.frozen_bodies = g_frozen_bodies_atomic.load(std::memory_order_relaxed);
    snap.frozen_bullets = g_frozen_bullets_atomic.load(std::memory_order_relaxed);
    snap.frozen_grenades = g_frozen_grenades_atomic.load(std::memory_order_relaxed);
    snap.frozen_rockets = g_frozen_rockets_atomic.load(std::memory_order_relaxed);
    snap.queued_overflow_bullets = uint32_t(g_overflow_queue.size());
    snap.skipped_ai_ticks = g_skipped_ai.load(std::memory_order_relaxed);
    snap.skipped_physics_ticks = g_skipped_physics.load(std::memory_order_relaxed);
    snap.skipped_particle_ticks = g_skipped_particles.load(std::memory_order_relaxed);
    return snap;
}

void DrawOverlay(float screen_w, float screen_h) {
    if (!IsActiveGameplay() || game::g_hide_hud || app::MenuOpen()) {
        return;
    }

    if (!IsFrozen()) {
        return;
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;

    // Project REAL suspended projectile positions from guest memory while Time Stop is active.
    std::vector<BulletMarker> markers;
    {
        std::lock_guard lock(g_mutex);
        markers = g_live_markers;
    }
    for (const BulletMarker& m : markers) {
        float norm_p[2] = {};
        if (!game::ProjectWorldPoint(m.pos, norm_p)) continue;
        const float sx = norm_p[0] * screen_w;
        const float sy = norm_p[1] * screen_h;
        if (sx < -40.0f || sx > screen_w + 40.0f || sy < -40.0f || sy > screen_h + 40.0f) continue;

        if (m.kind == 0) {
            // Suspended physical bullet in [0x002D12D0]+0x6E0: crisp cyan-gold reticle + short velocity ray
            float tip[3] = {m.pos[0] + m.dir[0] * 0.45f, m.pos[1] + m.dir[1] * 0.45f, m.pos[2] + m.dir[2] * 0.45f};
            float norm_tip[2] = {};
            if (game::ProjectWorldPoint(tip, norm_tip)) {
                dl->AddLine(ImVec2(sx, sy), ImVec2(norm_tip[0] * screen_w, norm_tip[1] * screen_h),
                            IM_COL32(90, 230, 255, 220), 2.2f);
            }
            dl->AddCircleFilled(ImVec2(sx, sy), 4.5f, IM_COL32(255, 220, 90, 240));
            dl->AddCircle(ImVec2(sx, sy), 8.0f, IM_COL32(90, 230, 255, 210), 16, 1.5f);
        } else {
            // Pool A grenade (orange) / Pool B rocket (red-gold)
            ImU32 col = (m.kind == 1) ? IM_COL32(255, 175, 50, 240) : IM_COL32(255, 85, 65, 240);
            dl->AddCircle(ImVec2(sx, sy), 11.0f, col, 20, 2.0f);
        }
    }
}

}  // namespace game::time_stop

// ============================================================================
// Guest x86 Subsystem Gating Hooks
// ============================================================================

X86_EXTERN_FUNC(__imp__sub_001F1080);
X86_EXTERN_FUNC(__imp__sub_001CCBB0);
X86_EXTERN_FUNC(__imp__sub_001E7FF0);
X86_EXTERN_FUNC(__imp__sub_000E0AA0);
X86_EXTERN_FUNC(__imp__sub_001780B0);
X86_EXTERN_FUNC(__imp__sub_001F6280);
X86_EXTERN_FUNC(__imp__sub_001D0500);
X86_EXTERN_FUNC(__imp__sub_001E4A30);
X86_EXTERN_FUNC(__imp__sub_001E43F0);
X86_EXTERN_FUNC(__imp__sub_001E1630);
X86_EXTERN_FUNC(__imp__sub_001E26E0);
X86_EXTERN_FUNC(__imp__sub_001DB7B0);

namespace game::time_stop {

namespace {
std::atomic<uint32_t> g_last_player_pkt{0};
std::atomic<float> g_last_player_range{80.0f};
std::atomic<float> g_last_player_speed{160.0f};
std::atomic<uint32_t> g_last_player_flags{0};
}  // namespace

bool LastPlayerFirePkt(uint32_t& pkt, float& range, float& speed, uint8_t& flags) {
    pkt = g_last_player_pkt.load(std::memory_order_relaxed);
    if (!pkt || !IsValidAnyGuestAddr(pkt, 8u)) return false;
    const uint32_t wpn = R32(pkt + 4u);
    const uint32_t world = R32(layout::kWorld);
    if (!wpn || !world || !IsValidAnyGuestAddr(wpn, 0x110u)) return false;
    if (R32(wpn + 0xF0u) != world + layout::kWorldPlayer) return false;
    range = g_last_player_range.load(std::memory_order_relaxed);
    speed = g_last_player_speed.load(std::memory_order_relaxed);
    flags = uint8_t(g_last_player_flags.load(std::memory_order_relaxed) & 0xFFu);
    return std::isfinite(range) && std::isfinite(speed) && range > 1.0f && speed > 1.0f;
}

void InvokeBulletSpawnerAndObserve(X86Context& ctx, uint8_t* base, uint32_t expected_mgr, uint32_t arg_mgr,
                                   uint32_t muzzle_ea, uint32_t fire_pkt, uint32_t dir_ea,
                                   float range, float speed, uint8_t flags) {
    if (fire_pkt && IsValidAnyGuestAddr(fire_pkt, 8u)) {
        const uint32_t wpn = R32(fire_pkt + 4u);
        const uint32_t world = R32(layout::kWorld);
        if (wpn && world && IsValidAnyGuestAddr(wpn, 0x110u) && R32(wpn + 0xF0u) == world + layout::kWorldPlayer) {
            g_last_player_pkt.store(fire_pkt, std::memory_order_relaxed);
            g_last_player_range.store(range, std::memory_order_relaxed);
            g_last_player_speed.store(speed, std::memory_order_relaxed);
            g_last_player_flags.store(flags, std::memory_order_relaxed);
        }
    }
    bool tracked = (expected_mgr != 0 && arg_mgr == expected_mgr &&
                    IsValidAnyGuestAddr(expected_mgr + kBulletPoolOffset, kBulletSlotCount * kBulletSlotStride) &&
                    IsValidAnyGuestAddr(muzzle_ea, 12u) && IsValidAnyGuestAddr(dir_ea, 12u));
    std::array<bool, kBulletSlotCount> was_active{};
    std::array<float, kBulletSlotCount> prev_time{};
    float origin[3]{};
    float dir[3]{};
    if (tracked) {
        ReadFloat3(muzzle_ea, origin);
        ReadFloat3(dir_ea, dir);
        for (uint32_t i = 0; i < kBulletSlotCount; ++i) {
            const uint32_t slot = expected_mgr + kBulletPoolOffset + i * kBulletSlotStride;
            was_active[i] = (G<uint8_t>(slot + kBulletActiveOffset) != 0);
            prev_time[i] = G<float>(slot + kBulletSpawnTimeOffset);
        }
    }

    __imp__sub_001DB7B0(ctx, base);

    if (tracked && (ctx.eax & 0xFFu) != 0) {
        for (uint32_t i = 0; i < kBulletSlotCount; ++i) {
            const uint32_t slot = expected_mgr + kBulletPoolOffset + i * kBulletSlotStride;
            if (G<uint8_t>(slot + kBulletActiveOffset) != 0 &&
                (!was_active[i] || G<float>(slot + kBulletSpawnTimeOffset) != prev_time[i])) {
                game::ballistics::OnPhysicalBulletSpawn(slot, fire_pkt, origin, dir, speed, range, flags);
                break;
            }
        }
    }
}

void ReplayOverflowBullets(X86Context& ctx, uint8_t* base) {
    uint32_t mgr = R32(kWeaponMgrPtr);
    if (!mgr) {
        g_overflow_queue.clear();
        return;
    }

    while (!g_overflow_queue.empty() && CountActiveBullets() < kBulletSlotCount) {
        QueuedBullet q = g_overflow_queue.front();
        g_overflow_queue.erase(g_overflow_queue.begin());
        if (q.mgr != mgr) continue;

        // Carve temporary stack space strictly below current esp for muzzle & dir vec4 buffers + 5 args.
        const uint32_t saved_esp = ctx.esp;
        const uint32_t saved_eax = ctx.eax;
        const uint32_t saved_ecx = ctx.ecx;
        const uint32_t saved_edx = ctx.edx;

        const uint32_t scratch = (saved_esp - 0x40u) & ~0xFu;
        const uint32_t muzzle_ea = scratch;
        const uint32_t dir_ea = scratch + 0x10u;
        for (int i = 0; i < 4; ++i) {
            G<float>(muzzle_ea + i * 4) = q.muzzle[i];
            G<float>(dir_ea + i * 4) = q.dir[i];
        }

        // Push args right-to-left for __imp__sub_001DB7B0 (ret 0x14): flags, speed, range, dir*, fire_pkt
        ctx.esp = scratch;
        X86_PUSH32(uint32_t(q.flags));
        uint32_t spd_bits = 0, rng_bits = 0;
        std::memcpy(&spd_bits, &q.speed, 4);
        std::memcpy(&rng_bits, &q.range, 4);
        X86_PUSH32(spd_bits);
        X86_PUSH32(rng_bits);
        X86_PUSH32(dir_ea);
        X86_PUSH32(q.fire_pkt);
        ctx.ecx = muzzle_ea;
        ctx.edx = mgr;
        X86_PUSH32(0xDEADBAADu);
        InvokeBulletSpawnerAndObserve(ctx, base, mgr, mgr, muzzle_ea, q.fire_pkt, dir_ea, q.range, q.speed, q.flags);

        ctx.esp = saved_esp;
        ctx.eax = saved_eax;
        ctx.ecx = saved_ecx;
        ctx.edx = saved_edx;
    }
}

void PreFrameSync(X86Context& ctx, uint8_t* base) {
    const uint32_t world = R32(layout::kWorld);
    const bool was_gameplay = g_gameplay_atomic.load(std::memory_order_relaxed);
    const bool gameplay = (world != 0 && R32(world + kWorldStateOffset) == kWorldGameplayState);
    g_gameplay_atomic.store(gameplay, std::memory_order_relaxed);
    if (was_gameplay && !gameplay) {
        game::ballistics::ResetAll();
        game::flamethrower::ResetLevelState();
    }

    const auto now = std::chrono::steady_clock::now();
    float real_dt = 0.016f;
    if (g_has_tick_time) {
        real_dt = std::chrono::duration<float>(now - g_last_tick_time).count();
    }
    g_last_tick_time = now;
    g_has_tick_time = true;

    float world_dt = 0.0f;
    if (gameplay) {
        float cur_clock = G<float>(world + kWorldClockOffset);
        if (g_has_prev_world_clock && std::isfinite(cur_clock)) {
            world_dt = common::SanitizeFreezeWorldDelta(cur_clock - g_prev_world_clock);
        }
        g_prev_world_clock = cur_clock;
        g_has_prev_world_clock = true;
    } else {
        g_has_prev_world_clock = false;
    }

    bool need_phys_restore = false;
    {
        std::lock_guard lock(g_mutex);
        const bool was_frozen = g_sm.IsFrozen();
        const auto cfg = CurrentConfig();

        // Update live state machine BEFORE running subsystems.
        g_sm.Update(real_dt, cfg, gameplay);
        const bool now_frozen = g_sm.IsFrozen();
        g_frozen_atomic.store(now_frozen, std::memory_order_relaxed);
        SyncSchemaEnabledFlag(now_frozen);

        if (was_frozen && !now_frozen) {
            LogTransition(gameplay ? "RESUME" : "LEFT_GAMEPLAY_RESUME", false);
            need_phys_restore = true;
        }

        if (gameplay) {
            if (now_frozen) {
                CompensateBulletLifetimes(world_dt);
                g_last_frozen_enemy_count = uint32_t(g_frame_enemies.size());
                g_frozen_enemies_atomic.store(g_last_frozen_enemy_count, std::memory_order_relaxed);
                g_frame_enemies.clear();

                if (now - g_last_heartbeat_time >= std::chrono::milliseconds(1500)) {
                    g_last_heartbeat_time = now;
                    LOGR("[TIME_STOP] HEARTBEAT | frozen=1 elapsed=%.2f enemies=%u bodies=%u bullets=%u grenades=%u rockets=%u overflow=%zu moved=%.2f",
                         g_sm.ActiveElapsedSec(), g_last_frozen_enemy_count,
                         g_frozen_bodies_atomic.load(std::memory_order_relaxed),
                         g_frozen_bullets_atomic.load(std::memory_order_relaxed),
                         g_frozen_grenades_atomic.load(std::memory_order_relaxed),
                         g_frozen_rockets_atomic.load(std::memory_order_relaxed),
                         g_overflow_queue.size(), PlayerDisplacementSinceFreezeStart());
                }
            } else {
                g_frame_enemies.clear();
                g_last_frozen_enemy_count = 0;
                g_frozen_enemies_atomic.store(0, std::memory_order_relaxed);
                if (!g_overflow_queue.empty()) {
                    ReplayOverflowBullets(ctx, base);
                }
            }
        } else {
            g_frame_enemies.clear();
            g_last_frozen_enemy_count = 0;
            g_frozen_enemies_atomic.store(0, std::memory_order_relaxed);
            g_overflow_queue.clear();
            g_live_markers.clear();
            g_frozen_bullets_atomic.store(0, std::memory_order_relaxed);
            g_frozen_grenades_atomic.store(0, std::memory_order_relaxed);
            g_frozen_rockets_atomic.store(0, std::memory_order_relaxed);
        }
    }
    if (need_phys_restore) {
        RestoreFrozenPhysicsOnResume();
    }
}

void PostFrameSnapshot() {
    if (!IsActiveGameplay()) return;
    std::lock_guard lock(g_mutex);
    SnapshotActiveProjectilesLocked();
}

}  // namespace game::time_stop

// Master frame tick (sub_001F1080)
X86_FUNC(sub_001F1080) {
    game::time_stop::PreFrameSync(ctx, base);
    __imp__sub_001F1080(ctx, base);
    game::time_stop::PostFrameSnapshot();
}

// Squad / combat coordinator [0x2D1980] — ret 0x04
X86_FUNC(sub_001CCBB0) {
    if (game::time_stop::IsFrozen()) {
        game::time_stop::RecordSkippedAiTick();
        ctx.esp += 8;
        return;
    }
    __imp__sub_001CCBB0(ctx, base);
}

// AI director [0x2D45D0] — ret 0x08
X86_FUNC(sub_001E7FF0) {
    if (game::time_stop::IsFrozen()) {
        game::time_stop::RecordSkippedAiTick();
        ctx.esp += 12;
        return;
    }
    __imp__sub_001E7FF0(ctx, base);
}

// Rigid-body & ragdoll world solver [0x2D45F4] — ret 0x04
// Selective isolation: keep player locomotion & collision stepping while locking all non-player bodies & owners.
X86_FUNC(sub_000E0AA0) {
    if (game::time_stop::IsFrozen() && game::time_stop::IsActiveGameplay()) {
        const uint32_t solver = X86_MEM32(ctx.esp + 4);
        std::unordered_set<uint32_t> active_bodies;
        std::vector<uint32_t> active_owners;
        game::time_stop::PrepareSelectivePhysicsSolve(solver, active_bodies, active_owners);
        __imp__sub_000E0AA0(ctx, base);
        game::time_stop::LockAfterSelectivePhysicsSolve(solver, active_owners);
        if (!active_bodies.empty()) {
            game::time_stop::RecordSkippedPhysicsTick();
        }
        return;
    }
    game::time_stop::MaybeRestoreFrozenPhysics();
    __imp__sub_000E0AA0(ctx, base);
}

// Particle, smoke, & shatter shard simulation [0x2D1990] — ret
X86_FUNC(sub_001780B0) {
    if (game::time_stop::IsFrozen()) {
        game::time_stop::RecordSkippedParticleTick();
        ctx.esp += 4;
        return;
    }
    __imp__sub_001780B0(ctx, base);
}

// World event scripts [0x2D19B0] — ret (only freeze during active 0x1C gameplay)
X86_FUNC(sub_001F6280) {
    if (game::time_stop::IsFrozen() && game::time_stop::IsActiveGameplay()) {
        ctx.esp += 4;
        return;
    }
    __imp__sub_001F6280(ctx, base);
}

// World timed emitters [0x2D0D7C] — ret 0x04
X86_FUNC(sub_001D0500) {
    if (game::time_stop::IsFrozen() && game::time_stop::IsActiveGameplay()) {
        ctx.esp += 8;
        return;
    }
    __imp__sub_001D0500(ctx, base);
}

// Pool A Throwables / Grenades vtable[0] (0x28BDD8) — ret 0x04; MUST allow dt == 0.0f spawn init!
X86_FUNC(sub_001E4A30) {
    if (game::time_stop::IsFrozen()) {
        float dt = X86_MEMF32(ctx.esp + 4);
        if (dt > 0.0f) {
            game::time_stop::RecordSkippedPhysicsTick();
            ctx.esp += 8;
            return;
        }
    }
    __imp__sub_001E4A30(ctx, base);
}

// Pool B Rockets / M203 vtable[0] (0x28C740) — ret 0x04; MUST allow dt == 0.0f spawn init!
X86_FUNC(sub_001E43F0) {
    if (game::time_stop::IsFrozen()) {
        float dt = X86_MEMF32(ctx.esp + 4);
        if (dt > 0.0f) {
            game::time_stop::RecordSkippedPhysicsTick();
            ctx.esp += 8;
            return;
        }
    }
    __imp__sub_001E43F0(ctx, base);
}

// Secondary projectile / tracer stepper [0x2D12D0]+0x20 — ret 0x04
X86_FUNC(sub_001E1630) {
    if (game::time_stop::IsFrozen()) {
        float dt = X86_MEMF32(ctx.esp + 4);
        if (dt > 0.0f) {
            ctx.esp += 8;
            return;
        }
    }
    __imp__sub_001E1630(ctx, base);
}

// Physical player bullet stepper [0x2D12D0]+0x6E0 — ret 0x04
X86_FUNC(sub_001E26E0) {
    if (game::time_stop::IsFrozen()) {
        float dt = X86_MEMF32(ctx.esp + 4);
        if (dt > 0.0f) {
            game::time_stop::RecordSkippedPhysicsTick();
            ctx.esp += 8;
            return;
        }
    }
    const uint32_t mgr = R32(game::time_stop::kWeaponMgrPtr);
    const bool track = game::time_stop::IsActiveGameplay() &&
                       game::time_stop::IsValidAnyGuestAddr(mgr + game::time_stop::kBulletPoolOffset,
                                                            game::time_stop::kBulletSlotCount *
                                                                game::time_stop::kBulletSlotStride);
    std::array<bool, game::time_stop::kBulletSlotCount> was_active{};
    float prev_pos[game::time_stop::kBulletSlotCount][3]{};
    if (track) {
        for (uint32_t i = 0; i < game::time_stop::kBulletSlotCount; ++i) {
            const uint32_t slot = mgr + game::time_stop::kBulletPoolOffset + i * game::time_stop::kBulletSlotStride;
            was_active[i] = (G<uint8_t>(slot + game::time_stop::kBulletActiveOffset) != 0);
            if (was_active[i]) {
                game::time_stop::ReadFloat3(slot + game::time_stop::kBulletCurPosOffset, prev_pos[i]);
            }
        }
    }

    __imp__sub_001E26E0(ctx, base);

    if (track) {
        for (uint32_t i = 0; i < game::time_stop::kBulletSlotCount; ++i) {
            if (!was_active[i]) continue;
            const uint32_t slot = mgr + game::time_stop::kBulletPoolOffset + i * game::time_stop::kBulletSlotStride;
            const bool active_after = (G<uint8_t>(slot + game::time_stop::kBulletActiveOffset) != 0);
            float new_pos[3]{};
            game::time_stop::ReadFloat3(slot + game::time_stop::kBulletCurPosOffset, new_pos);
            game::ballistics::OnPhysicalBulletStep(slot, prev_pos[i], new_pos, active_after);
        }
    }
}

// Physical bullet spawner into [0x2D12D0]+0x6E0 — ret 0x14
X86_FUNC(sub_001DB7B0) {
    const uint32_t fire_pkt = X86_MEM32(ctx.esp + 4);
    const uint32_t dir_ea = X86_MEM32(ctx.esp + 8);
    const float range = X86_MEMF32(ctx.esp + 12);
    const float speed = X86_MEMF32(ctx.esp + 16);
    const uint8_t flags = uint8_t(X86_MEM32(ctx.esp + 20) & 0xFFu);
    if (game::time_stop::TryQueueOverflowBullet(
            ctx.edx, ctx.ecx, fire_pkt, dir_ea, range, speed, flags, ctx.esp)) {
        ctx.eax = (ctx.eax & ~0xFFu) | 1u;
        ctx.esp += 24;
        return;
    }
    const uint32_t expected_mgr = R32(game::time_stop::kWeaponMgrPtr);
    game::time_stop::InvokeBulletSpawnerAndObserve(ctx, base, expected_mgr, ctx.edx, ctx.ecx, fire_pkt, dir_ea,
                                                    range, speed, flags);
}
