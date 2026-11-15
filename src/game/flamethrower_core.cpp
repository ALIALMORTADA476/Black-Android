#include "game/flamethrower_core.h"
#include "game/special_arsenal_core.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace game::flamethrower {

namespace {

constexpr float kPi = 3.14159265f;

float FastRandSigned(uint32_t& s) noexcept {
    s = s * 1664525u + 1013904223u;
    int32_t v = int32_t((s >> 9) & 0xFFFFu) - 32768;
    return float(v) * (1.0f / 32768.0f);
}

// 0..1 unit random from a fast LCG (jet turbulence only, never gameplay-critical)
float UnitRand(uint32_t& s) noexcept {
    s = s * 1664525u + 1013904223u;
    return float((s >> 8) & 0xFFFFFFu) * (1.0f / 16777216.0f);
}

float SmoothStep01(float edge0, float edge1, float x) noexcept {
    const float t = std::clamp((x - edge0) / std::max(edge1 - edge0, 1e-5f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

bool ParseBmsh(const void* data, size_t size, BmshMesh& out) {
    out.vertices.clear();
    out.indices.clear();
    if (!data || size < sizeof(BmshHeader)) return false;

    BmshHeader hdr{};
    std::memcpy(&hdr, data, sizeof(hdr));
    if (hdr.magic != kBmshMagic) return false;

    // Support both legacy v1 static BMSH and v2 articulated+UV BMSH models cleanly.
    if (hdr.version == 2u) {
        game::special_arsenal::BmshModel v2{};
        if (!game::special_arsenal::ParseBmsh(static_cast<const uint8_t*>(data), size, v2)) {
            return false;
        }
        out.vertices.resize(v2.vertices.size());
        for (size_t i = 0; i < v2.vertices.size(); ++i) {
            const auto& sv = v2.vertices[i];
            out.vertices[i] = BmshVertex{sv.x, sv.y, sv.z, sv.nx, sv.ny, sv.nz, sv.bgra};
        }
        out.indices = std::move(v2.indices);
        for (int i = 0; i < 3; ++i) {
            out.nozzle_pos[i] = v2.nozzle_pos[i];
            out.pilot_pos[i] = v2.pilot_pos[i];
            out.bounds_min[i] = v2.bounds_min[i];
            out.bounds_max[i] = v2.bounds_max[i];
        }
        return true;
    }

    if (hdr.version != kBmshVersion) return false;
    if (hdr.vertex_count == 0 || hdr.vertex_count > 65535u) return false;
    if (hdr.index_count == 0 || (hdr.index_count % 3u) != 0u) return false;

    const size_t verts_bytes = size_t(hdr.vertex_count) * sizeof(BmshVertex);
    const size_t idx_bytes = size_t(hdr.index_count) * sizeof(uint16_t);
    if (size < sizeof(BmshHeader) + verts_bytes + idx_bytes) return false;

    const auto* ptr = static_cast<const uint8_t*>(data);
    out.vertices.resize(hdr.vertex_count);
    std::memcpy(out.vertices.data(), ptr + sizeof(BmshHeader), verts_bytes);
    out.indices.resize(hdr.index_count);
    std::memcpy(out.indices.data(), ptr + sizeof(BmshHeader) + verts_bytes, idx_bytes);

    for (uint16_t idx : out.indices) {
        if (idx >= hdr.vertex_count) {
            out.vertices.clear();
            out.indices.clear();
            return false;
        }
    }

    std::memcpy(out.nozzle_pos, hdr.nozzle_pos, sizeof(out.nozzle_pos));
    std::memcpy(out.pilot_pos, hdr.pilot_pos, sizeof(out.pilot_pos));
    std::memcpy(out.bounds_min, hdr.bounds_min, sizeof(out.bounds_min));
    std::memcpy(out.bounds_max, hdr.bounds_max, sizeof(out.bounds_max));
    return true;
}

void CoreSim::Reset() {
    active_ = false;
    prev_slot_ = 0;
    equip_anim_ = 0.0f;
    fuel_pct_ = 100.0f;
    pressure_ = 0.0f;
    firing_ = false;
    reloading_ = false;
    reload_elapsed_ = 0.0f;
    reload_refilled_ = false;
    ignition_count_ = 0;
    muzzle_valid_ = false;
    has_ground_ = false;
    emit_accum_ = 0.0f;
    jet_phase_ = 0.0f;
    jet_count_ = 0;
    pool_count_ = 0;
    ClearVictims();
    SyncAudioAtomics();
}

void CoreSim::SetMuzzle(const float pos[3], const float dir[3], float ground_y) {
    if (!pos || !dir) return;
    if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) return;
    if (!std::isfinite(dir[0]) || !std::isfinite(dir[1]) || !std::isfinite(dir[2])) return;
    const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len < 1e-4f) return;

    if (!muzzle_valid_) {
        std::memcpy(muzzle_pos_, pos, sizeof(muzzle_pos_));
        std::memcpy(muzzle_dir_, dir, sizeof(muzzle_dir_));
    }
    std::memcpy(aim_dir_, dir, sizeof(aim_dir_));
    aim_dir_[0] /= len;
    aim_dir_[1] /= len;
    aim_dir_[2] /= len;
    muzzle_pos_[0] = pos[0];
    muzzle_pos_[1] = pos[1];
    muzzle_pos_[2] = pos[2];
    muzzle_valid_ = true;
    if (std::isfinite(ground_y)) {
        ground_y_ = ground_y;
        has_ground_ = true;
    }
}

void CoreSim::SetActive(bool on, uint32_t current_slot) {
    if (on == active_) return;
    if (on) {
        prev_slot_ = current_slot;
        active_ = true;
        equip_anim_ = 0.0f;
        firing_ = false;
        pressure_ = 0.0f;
    } else {
        active_ = false;
        firing_ = false;
        reloading_ = false;
        pressure_ = 0.0f;
        // stowing the weapon clears the stream: nothing simulates while it is
        // lowered, so leftover fuel must not reappear on the next equip
        jet_count_ = 0;
        pool_count_ = 0;
        emit_accum_ = 0.0f;
        muzzle_valid_ = false;
    }
    SyncAudioAtomics();
}

uint32_t CoreSim::DeactivateAndRestoreSlot() {
    const uint32_t restore = prev_slot_;
    SetActive(false, prev_slot_);
    return restore;
}

void CoreSim::RefillFuel() {
    fuel_pct_ = 100.0f;
    reloading_ = false;
    reload_elapsed_ = 0.0f;
    reload_refilled_ = true;
    SyncAudioAtomics();
}

float CoreSim::ReloadProgress() const noexcept {
    if (!reloading_) return 0.0f;
    return std::clamp(reload_elapsed_ / kReloadDurationSec, 0.0f, 1.0f);
}

void CoreSim::Step(float dt, bool trigger_held, bool reload_pressed, bool infinite_ammo) {
    const float cdt = std::clamp(dt, 0.0f, 0.1f);
    if (!active_) {
        firing_ = false;
        pressure_ = 0.0f;
        StepJet(cdt);  // the lit stream keeps flying and burning out
        SyncAudioAtomics();
        return;
    }

    equip_anim_ = std::min(1.0f, equip_anim_ + cdt / 0.30f);

    if (infinite_ammo && !reloading_) {
        fuel_pct_ = 100.0f;
    }

    if (reload_pressed && !reloading_ && fuel_pct_ < 99.5f) {
        reloading_ = true;
        reload_elapsed_ = 0.0f;
        reload_refilled_ = false;
        firing_ = false;
    }

    if (reloading_) {
        reload_elapsed_ += cdt;
        // line pressure vents through the open coupling while the canister swaps
        pressure_ = std::max(0.0f, pressure_ - cdt / 0.10f);
        firing_ = false;
        if (!reload_refilled_ && reload_elapsed_ >= kReloadDurationSec * 0.68f) {
            fuel_pct_ = 100.0f;
            reload_refilled_ = true;
        }
        if (reload_elapsed_ >= kReloadDurationSec) {
            reloading_ = false;
            reload_elapsed_ = 0.0f;
            fuel_pct_ = 100.0f;
        }
        StepJet(cdt);
        SyncAudioAtomics();
        return;
    }

    const bool has_fuel = infinite_ammo || fuel_pct_ > 0.0f;
    if (trigger_held && has_fuel) {
        if (!firing_) {
            ignition_count_++;
            aud_ignite_seq_.store(ignition_count_, std::memory_order_release);
        }
        // valve snaps open, then the pump brings the line up: ~0.2s to full spray
        pressure_ += (1.0f - pressure_) * (1.0f - std::exp(-cdt / 0.20f));
        firing_ = true;
        if (!infinite_ammo) {
            // fuel leaves the tank in proportion to line pressure
            fuel_pct_ = std::max(0.0f, fuel_pct_ - (100.0f / kFullBurnSeconds) * pressure_ * cdt);
            if (fuel_pct_ <= 0.0f) {
                // empty tank: propellant is gone, so the jet dies in ~0.15s
                pressure_ = std::max(0.0f, pressure_ - cdt / 0.055f);
                firing_ = false;
            }
        }
    } else {
        firing_ = false;
        // releasing the trigger leaves fuel in the hose: it dribbles out burning
        const float tau = has_fuel ? 0.34f : 0.055f;
        pressure_ = std::max(0.0f, pressure_ - cdt / tau);
    }

    if (pressure_ > 0.05f) {
        emit_accum_ += kJetSpawnRate * pressure_ * cdt;
        while (emit_accum_ >= 1.0f) {
            emit_accum_ -= 1.0f;
            SpawnJetSegment(pressure_);
        }
    } else {
        emit_accum_ = 0.0f;
    }

    StepJet(cdt);
    SyncAudioAtomics();
}

float CoreSim::RandJet() noexcept {
    return UnitRand(rng_jet_);
}

void CoreSim::SpawnJetSegment(float pressure) noexcept {
    if (!muzzle_valid_ || jet_count_ >= kMaxJetSegments) return;

    // orthonormal basis around the aim ray for the turbulent cone
    const float dx = muzzle_dir_[0], dy = muzzle_dir_[1], dz = muzzle_dir_[2];
    float ux = (std::fabs(dy) < 0.95f) ? 0.0f : 1.0f;
    float uy = (std::fabs(dy) < 0.95f) ? 1.0f : 0.0f;
    float rx = uy * dz - 0.0f * dy;
    float ry = 0.0f * dx - ux * dz;
    float rz = ux * dy - uy * dx;
    const float rlen = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (rlen > 1e-5f) {
        rx /= rlen;
        ry /= rlen;
        rz /= rlen;
    } else {
        rx = 1.0f;
        ry = 0.0f;
        rz = 0.0f;
    }
    const float sx = dy * rz - dz * ry;
    const float sy = dz * rx - dx * rz;
    const float sz = dx * ry - dy * rx;

    // the nozzle throws a cone of ~6 deg; stronger flow keeps it tighter
    const float spread = 0.105f * (1.25f - 0.45f * std::clamp(pressure, 0.0f, 1.0f));
    const float ang = RandJet() * 6.28318f;
    const float rad = std::sqrt(RandJet()) * spread;
    const float offr = std::cos(ang) * rad;
    const float offu = std::sin(ang) * rad;

    JetSegment& s = jet_[jet_count_++];
    const float spd = kJetExitSpeed * (0.88f + 0.24f * RandJet()) *
                      (0.72f + 0.28f * std::clamp(pressure, 0.0f, 1.0f));
    s.vel[0] = (dx + rx * offr + sx * offu) * spd;
    s.vel[1] = (dy + ry * offr + sy * offu) * spd;
    s.vel[2] = (dz + rz * offr + sz * offu) * spd;
    s.pos[0] = muzzle_pos_[0] + dx * 0.06f;
    s.pos[1] = muzzle_pos_[1] + dy * 0.06f;
    s.pos[2] = muzzle_pos_[2] + dz * 0.06f;
    s.age = 0.0f;
    s.ttl = kJetSegmentTtl * (0.94f + 0.12f * RandJet());
    s.radius = 0.085f;
    s.heat = std::clamp(0.55f + 0.45f * pressure, 0.0f, 1.0f);
    s.spawn_heat = s.heat;
}

void CoreSim::SpawnPoolSegment(const float pos[3], float radius, float heat, float ttl) noexcept {
    if (pool_count_ >= kMaxPoolSegments) return;
    JetSegment& p = pool_[pool_count_++];
    p.pos[0] = pos[0];
    p.pos[1] = pos[1];
    p.pos[2] = pos[2];
    p.vel[0] = (RandJet() - 0.5f) * 1.3f;
    p.vel[1] = 0.0f;
    p.vel[2] = (RandJet() - 0.5f) * 1.3f;
    p.radius = radius;
    p.heat = std::clamp(heat, 0.15f, 1.0f);
    p.age = 0.0f;
    p.ttl = ttl;
}

void CoreSim::StepJet(float dt) noexcept {
    if (dt <= 0.0f) return;

    // the nozzle lags the aim slightly: fuel has mass in the line
    if (muzzle_valid_) {
        const float a = 1.0f - std::exp(-dt / 0.045f);
        muzzle_dir_[0] += (aim_dir_[0] - muzzle_dir_[0]) * a;
        muzzle_dir_[1] += (aim_dir_[1] - muzzle_dir_[1]) * a;
        muzzle_dir_[2] += (aim_dir_[2] - muzzle_dir_[2]) * a;
        const float l = std::sqrt(muzzle_dir_[0] * muzzle_dir_[0] + muzzle_dir_[1] * muzzle_dir_[1] +
                                  muzzle_dir_[2] * muzzle_dir_[2]);
        if (l > 1e-5f) {
            muzzle_dir_[0] /= l;
            muzzle_dir_[1] /= l;
            muzzle_dir_[2] /= l;
        }
    }

    jet_phase_ += dt * 11.0f;
    if (jet_phase_ > 6.28318f) jet_phase_ -= 6.28318f;
    const float turbulence = 0.80f + 0.20f * std::sin(jet_phase_);

    uint32_t alive = 0;
    for (uint32_t i = 0; i < jet_count_; i++) {
        JetSegment s = jet_[i];
        s.age += dt;
        if (s.age >= s.ttl) continue;

        const float u = s.age / s.ttl;
        // heavy gel droops out of the nozzle, then the burning vapour rises
        const float ay = -4.0f + 6.6f * SmoothStep01(0.05f, 0.55f, s.age);
        const float damp = std::exp(-kJetDragK * dt);
        s.vel[0] *= damp;
        s.vel[2] *= damp;
        s.vel[1] = s.vel[1] * damp + ay * dt;
        s.pos[0] += s.vel[0] * dt;
        s.pos[1] += s.vel[1] * dt;
        s.pos[2] += s.vel[2] * dt;
        // entrained air tears the stream apart: it widens as it loses speed
        s.radius = 0.085f + 0.66f * std::pow(u, 0.75f) * turbulence;
        s.heat = std::clamp(s.spawn_heat * (1.0f - 0.34f * u), 0.0f, 1.0f);

        if (has_ground_ && s.pos[1] <= ground_y_ + 0.03f) {
            // wet fuel hits the deck: it splashes, pools and keeps burning there
            const float gx = s.pos[0];
            const float gz = s.pos[2];
            const float pool_heat = std::clamp(s.heat, 0.25f, 1.0f);
            const float pool_pos[3] = {gx, ground_y_ + 0.025f, gz};
            SpawnPoolSegment(pool_pos, std::max(s.radius, 0.20f), pool_heat, 2.4f + 3.0f * pool_heat);
            if (s.radius > 0.22f && pool_count_ < kMaxPoolSegments) {
                const float sa = RandJet() * 6.28318f;
                const float sd = 0.18f + RandJet() * 0.42f;
                const float splash[3] = {gx + std::cos(sa) * sd, ground_y_ + 0.02f, gz + std::sin(sa) * sd};
                SpawnPoolSegment(splash, 0.14f + 0.12f * RandJet(), pool_heat * 0.85f,
                                 1.6f + 2.4f * pool_heat);
            }
            continue;
        }
        jet_[alive++] = s;
    }
    jet_count_ = alive;

    uint32_t pooled = 0;
    for (uint32_t i = 0; i < pool_count_; i++) {
        JetSegment p = pool_[i];
        p.age += dt;
        if (p.age >= p.ttl) continue;
        const float damp = std::exp(-9.0f * dt);
        p.vel[0] *= damp;
        p.vel[2] *= damp;
        p.pos[0] += p.vel[0] * dt;
        p.pos[2] += p.vel[2] * dt;
        if (has_ground_) p.pos[1] = ground_y_ + 0.025f;
        // the puddle spreads while it consumes itself
        p.radius += 0.34f * dt;
        p.heat = std::max(0.16f, p.heat * std::exp(-dt / 2.3f));
        pool_[pooled++] = p;
    }
    pool_count_ = pooled;
}

size_t CoreSim::CopyJetSegments(JetSegment* out, size_t max_segments) const {
    if (!out || max_segments == 0) return 0;
    const size_t n = std::min<size_t>(jet_count_, max_segments);
    for (size_t i = 0; i < n; i++) out[i] = jet_[i];
    return n;
}

size_t CoreSim::CopyPoolSegments(JetSegment* out, size_t max_segments) const {
    if (!out || max_segments == 0) return 0;
    const size_t n = std::min<size_t>(pool_count_, max_segments);
    for (size_t i = 0; i < n; i++) out[i] = pool_[i];
    return n;
}

float CoreSim::JetFrontDistance() const noexcept {
    if (!muzzle_valid_) return 0.0f;
    float best = 0.0f;
    for (uint32_t i = 0; i < jet_count_; i++) {
        const float dx = jet_[i].pos[0] - muzzle_pos_[0];
        const float dz = jet_[i].pos[2] - muzzle_pos_[2];
        const float along = dx * muzzle_dir_[0] + dz * muzzle_dir_[2] + (jet_[i].pos[1] - muzzle_pos_[1]) * muzzle_dir_[1];
        best = std::max(best, along);
    }
    return best;
}

float CoreSim::StreamExposure(const float target_pos[3], float target_radius) const {
    if (!target_pos) return 0.0f;
    if (!std::isfinite(target_pos[0]) || !std::isfinite(target_pos[1]) || !std::isfinite(target_pos[2])) {
        return 0.0f;
    }
    const float tr = std::max(target_radius, 0.05f);
    float best = 0.0f;
    auto sample = [&](const JetSegment& s) {
        const float dx = target_pos[0] - s.pos[0];
        const float dy = target_pos[1] - s.pos[1];
        const float dz = target_pos[2] - s.pos[2];
        const float reach = s.radius + tr;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > reach * reach) return;
        const float d = std::sqrt(d2);
        // the core of the gobbet is far hotter than its edge
        const float core = 1.0f - 0.55f * std::clamp(d / reach, 0.0f, 1.0f);
        best = std::max(best, s.heat * core);
    };
    for (uint32_t i = 0; i < jet_count_; i++) sample(jet_[i]);
    for (uint32_t i = 0; i < pool_count_; i++) sample(pool_[i]);
    return std::clamp(best, 0.0f, 1.0f);
}

float CoreSim::ConeIntensity(const float origin[3], const float forward[3],
                             const float target_pos[3]) noexcept {
    if (!origin || !forward || !target_pos) return 0.0f;
    const float dx = target_pos[0] - origin[0];
    const float dy = target_pos[1] - origin[1];
    const float dz = target_pos[2] - origin[2];

    const float flen = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] +
                                 forward[2] * forward[2]);
    if (flen < 1e-5f) return 0.0f;
    const float fx = forward[0] / flen;
    const float fy = forward[1] / flen;
    const float fz = forward[2] / flen;

    const float along = dx * fx + dy * fy + dz * fz;
    if (along < -0.25f || along > kMaxRangeMeters) return 0.0f;

    const float px = dx - fx * along;
    const float py = dy - fy * along;
    const float pz = dz - fz * along;
    const float perp = std::sqrt(px * px + py * py + pz * pz);

    // Stream expands naturally from 0.72m at muzzle to ~2.75m at max reach
    const float max_rad = 0.72f + 0.125f * std::max(along, 0.0f);
    if (perp > max_rad) return 0.0f;

    const float axial_falloff = 1.0f - std::clamp(along / kMaxRangeMeters, 0.0f, 1.0f) * 0.52f;
    const float radial_falloff = 1.0f - (perp / max_rad) * 0.45f;
    return std::clamp(axial_falloff * radial_falloff, 0.10f, 1.0f);
}

VictimBurnState* CoreSim::FindOrAllocVictim(uint32_t victim_ea) noexcept {
    if (!victim_ea) return nullptr;
    for (auto& v : victims_) {
        if (v.victim_ea == victim_ea) return &v;
    }
    for (size_t i = 0; i < kMaxVictims; i++) {
        size_t idx = (victim_cursor_ + i) % kMaxVictims;
        if (!victims_[idx].victim_ea || (!victims_[idx].ignited && victims_[idx].heat <= 0.01f)) {
            victim_cursor_ = uint32_t((idx + 1) % kMaxVictims);
            victims_[idx] = VictimBurnState{};
            victims_[idx].victim_ea = victim_ea;
            return &victims_[idx];
        }
    }
    size_t idx = victim_cursor_ % kMaxVictims;
    victim_cursor_ = uint32_t((idx + 1) % kMaxVictims);
    victims_[idx] = VictimBurnState{};
    victims_[idx].victim_ea = victim_ea;
    return &victims_[idx];
}

void CoreSim::ExposeEnemy(uint32_t victim_ea, float intensity, float dt) {
    if (!victim_ea || intensity <= 0.0f || dt <= 0.0f) return;
    VictimBurnState* v = FindOrAllocVictim(victim_ea);
    if (!v) return;
    // A real jet needs roughly half a second of contact to set clothing alight.
    v->heat = std::min(1.5f, v->heat + intensity * 2.20f * dt);
    v->fuel_soak = std::min(3.0f, v->fuel_soak + intensity * dt);
    v->direct_heat = 0.28f;
    if (v->heat >= 1.0f) {
        if (!v->ignited) {
            v->ignited = true;
            // more gel soaked in = hotter, longer, more lethal burn
            v->severity = std::clamp(0.50f + 0.17f * v->fuel_soak, 0.50f, 1.0f);
            v->agony_cooldown = 0.01f;
        }
        // the jet keeps topping up the fuel already stuck to the body
        v->severity = std::max(v->severity, std::clamp(0.50f + 0.17f * v->fuel_soak, 0.50f, 1.0f));
        v->burn_timer = std::max(v->burn_timer, 5.0f + 8.0f * v->severity);
    }
}

VictimBurnTick CoreSim::TickVictim(uint32_t victim_ea, float dt, bool is_dying) {
    VictimBurnTick out{};
    if (!victim_ea || dt <= 0.0f) return out;
    const float cdt = std::clamp(dt, 0.0f, 0.1f);

    VictimBurnState* v = nullptr;
    for (auto& slot : victims_) {
        if (slot.victim_ea == victim_ea) {
            v = &slot;
            break;
        }
    }
    if (!v) return out;

    v->direct_heat = std::max(0.0f, v->direct_heat - cdt);

    if (!v->ignited) {
        if (v->direct_heat > 0.0f) {
            // Painful searing while the gel is still catching, but not lethal yet
            out.damage = 12.0f * cdt;
            out.emit_fire = (v->heat > 0.42f);
            out.fire_intensity = std::clamp(v->heat * 0.55f, 0.15f, 0.55f);
        } else {
            // soaked-in fuel cools off if the stream moved away before ignition
            v->heat = std::max(0.0f, v->heat - 0.65f * cdt);
            v->fuel_soak = std::max(0.0f, v->fuel_soak - 0.5f * cdt);
        }
        return out;
    }

    if (is_dying) {
        if (!v->was_dying) {
            v->was_dying = true;
            // gel on a corpse keeps burning for several seconds
            v->burn_timer = std::min(v->burn_timer, 7.5f);
        }
        v->burn_timer = std::max(0.0f, v->burn_timer - cdt);
        v->char_factor = std::min(0.92f, v->char_factor + 0.30f * cdt);
        out.emit_fire = (v->burn_timer > 2.2f);
        out.emit_smoke = (v->burn_timer > 0.0f);
        out.fire_intensity = out.emit_fire ? std::clamp((v->burn_timer - 2.2f) / 4.0f, 0.25f, 0.85f) : 0.0f;
        out.char_factor = v->char_factor;
        if (v->burn_timer <= 0.0f) {
            v->ignited = false;
            v->heat = 0.0f;
            v->fuel_soak = 0.0f;
        }
        return out;
    }

    // Alive and burning: napalm gel burns hottest right after it lands, then the
    // fuel is consumed and only the residual smoulder is left.
    v->burn_timer = std::max(0.0f, v->burn_timer - cdt);
    v->char_factor = std::min(0.85f, v->char_factor + 0.24f * cdt);
    if (v->direct_heat > 0.0f) {
        v->severity = std::max(v->severity, 0.62f);
    }
    v->severity = std::max(0.22f, v->severity * std::exp(-cdt / 3.2f));
    out.damage = (11.0f + 54.0f * v->severity) * cdt;
    out.emit_fire = true;
    out.emit_smoke = true;
    out.fire_intensity = (v->direct_heat > 0.0f) ? 1.0f
                                                 : std::clamp(v->severity * 1.15f, 0.40f, 0.95f);
    out.char_factor = v->char_factor;

    v->agony_cooldown -= cdt;
    if (v->agony_cooldown <= 0.0f) {
        out.agony_flinch = true;
        out.agony_clip = ((victim_ea ^ uint32_t(v->burn_timer * 10.0f)) & 1u) ? 0x1Eu : 0x1Fu;
        v->agony_cooldown = (v->direct_heat > 0.0f) ? 0.42f : 0.62f;
    }

    if (v->burn_timer <= 0.0f) {
        v->ignited = false;
        v->heat = 0.0f;
        v->fuel_soak = 0.0f;
    }
    return out;
}

VictimBurnState CoreSim::GetVictimState(uint32_t victim_ea) const noexcept {
    for (const auto& v : victims_) {
        if (v.victim_ea == victim_ea) return v;
    }
    return VictimBurnState{};
}

void CoreSim::ClearVictims() {
    for (auto& v : victims_) v = VictimBurnState{};
    victim_cursor_ = 0;
}

void CoreSim::SyncAudioAtomics() noexcept {
    aud_active_.store(active_, std::memory_order_relaxed);
    aud_firing_.store(firing_, std::memory_order_relaxed);
    aud_reloading_.store(reloading_, std::memory_order_relaxed);
    aud_pressure_.store(pressure_, std::memory_order_relaxed);
    aud_reload_t_.store(ReloadProgress(), std::memory_order_relaxed);
}

void CoreSim::RenderAudio(float* stereo_out, uint32_t frames, uint32_t sample_rate) {
    if (!stereo_out || frames == 0 || sample_rate == 0) return;

    const bool active = aud_active_.load(std::memory_order_relaxed);
    const float pressure = aud_pressure_.load(std::memory_order_relaxed);
    const bool reloading = aud_reloading_.load(std::memory_order_relaxed);
    const float reload_t = aud_reload_t_.load(std::memory_order_relaxed);
    const uint32_t ign_seq = aud_ignite_seq_.load(std::memory_order_acquire);

    if (ign_seq != audio_seen_ignite_) {
        audio_seen_ignite_ = ign_seq;
        audio_whoosh_env_ = 1.0f;
    }

    if (!active && audio_whoosh_env_ < 1e-4f && pressure < 1e-4f) return;

    const float sr = float(sample_rate);
    const float whoosh_decay = std::exp(-1.0f / (sr * 0.14f));
    const float lp_alpha = std::clamp(2.0f * kPi * 310.0f / sr, 0.005f, 0.25f);
    const float pilot_alpha = std::clamp(2.0f * kPi * 1400.0f / sr, 0.02f, 0.45f);
    const float lfo_inc = (2.0f * kPi * 13.5f) / sr;

    for (uint32_t i = 0; i < frames; i++) {
        const float wl = FastRandSigned(rng_l_);
        const float wr = FastRandSigned(rng_r_);

        // Paul Kellet-style light 3-pole pink noise approximation per channel
        pink_l_[0] = 0.997f * pink_l_[0] + wl * 0.032f;
        pink_l_[1] = 0.985f * pink_l_[1] + wl * 0.048f;
        pink_l_[2] = 0.945f * pink_l_[2] + wl * 0.075f;
        const float pl = pink_l_[0] + pink_l_[1] + pink_l_[2] + wl * 0.045f;

        pink_r_[0] = 0.997f * pink_r_[0] + wr * 0.032f;
        pink_r_[1] = 0.985f * pink_r_[1] + wr * 0.048f;
        pink_r_[2] = 0.945f * pink_r_[2] + wr * 0.075f;
        const float pr = pink_r_[0] + pink_r_[1] + pink_r_[2] + wr * 0.045f;

        lfo_phase_ += lfo_inc;
        if (lfo_phase_ >= 2.0f * kPi) lfo_phase_ -= 2.0f * kPi;
        const float vortex = 0.80f + 0.20f * std::sin(lfo_phase_);

        lp_l_ += lp_alpha * (pl - lp_l_);
        lp_r_ += lp_alpha * (pr - lp_r_);

        const float prev_hp_l = hp_l_;
        const float prev_hp_r = hp_r_;
        hp_l_ += 0.35f * (wl - hp_l_);
        hp_r_ += 0.35f * (wr - hp_r_);
        const float hiss_l = wl - prev_hp_l * 0.65f;
        const float hiss_r = wr - prev_hp_r * 0.65f;

        float l = 0.0f;
        float r = 0.0f;

        // Idle stays completely silent so there is no continuous distracting hiss when carrying.

        // 2. Ignition punch/whoosh transient
        if (audio_whoosh_env_ > 1e-4f) {
            const float thump = std::sin(lfo_phase_ * 5.2f) * 0.24f + lp_l_ * 0.72f;
            l += thump * audio_whoosh_env_ * 0.42f;
            r += thump * audio_whoosh_env_ * 0.40f;
            audio_whoosh_env_ *= whoosh_decay;
        }

        // 3. Roaring napalm flame jet: heavy turbulent bass + high-pressure fuel spray
        if (pressure > 1e-3f) {
            const float roar_l = (lp_l_ * 1.45f + pl * 0.55f) * vortex + hiss_l * 0.14f;
            const float roar_r = (lp_r_ * 1.45f + pr * 0.55f) * vortex + hiss_r * 0.14f;
            const float lvl = pressure * 0.36f;
            l += roar_l * lvl;
            r += roar_r * lvl;
        }

        // 3b. Choked sputter while line pressure drops: the jet coughs and dribbles
        if (pressure > 1e-3f && pressure < 0.35f) {
            const float flutter = 0.55f + 0.45f * std::sin(lfo_phase_ * 7.3f);
            const float sputter = lp_l_ * 0.34f * flutter * (0.35f - pressure) * 2.2f;
            l += sputter;
            r += sputter * 0.92f;
        }

        // 4. Canister swap reload venting & lock clacks
        if (reloading) {
            if (reload_t > 0.05f && reload_t < 0.32f) {
                const float vent = hiss_l * 0.16f * std::sin((reload_t - 0.05f) / 0.27f * kPi);
                l += vent;
                r += vent;
            }
            if ((reload_t > 0.42f && reload_t < 0.44f) ||
                (reload_t > 0.72f && reload_t < 0.74f)) {
                const float clack = wl * 0.22f;
                l += clack;
                r += clack;
            }
            if (reload_t > 0.80f && reload_t < 0.96f) {
                const float charge = hiss_r * 0.11f;
                l += charge;
                r += charge;
            }
        }

        stereo_out[i * 2 + 0] = std::clamp(stereo_out[i * 2 + 0] + l, -1.0f, 1.0f);
        stereo_out[i * 2 + 1] = std::clamp(stereo_out[i * 2 + 1] + r, -1.0f, 1.0f);
    }
}

}  // namespace game::flamethrower
