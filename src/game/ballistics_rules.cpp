#include "game/ballistics_fx.h"
#include "game/flame_plume.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace game {
namespace ballistics {

WeaponClass ClassifyWeaponCode(const char* code3) {
    if (!code3 || !code3[0]) return WeaponClass::Unknown;
    auto eq = [&](const char* k) { return std::strcmp(code3, k) == 0; };
    if (eq("PST") || eq("PS5") || eq("GK1") || eq("GK5") || eq("MGN") || eq("MG5") || eq("C96")) return WeaponClass::Pistol;
    if (eq("SMG") || eq("SM5") || eq("SM3") || eq("MC5") || eq("MP1") || eq("MP5") || eq("P90") || eq("P95"))
        return WeaponClass::Smg;
    if (eq("AK1") || eq("AK5") || eq("ASR") || eq("AS5") || eq("M16") || eq("M45") || eq("BNS"))
        return WeaponClass::Rifle;
    if (eq("SHG") || eq("SH5") || eq("RM1") || eq("RM5")) return WeaponClass::Shotgun;
    if (eq("SNR") || eq("SN5") || eq("M91")) return WeaponClass::Sniper;
    if (eq("HVY") || eq("HV5") || eq("MXM")) return WeaponClass::Lmg;
    if (eq("RPG") || eq("GRL") || eq("FLM")) return WeaponClass::Launcher;
    return WeaponClass::Unknown;
}

bool ShouldEmitTracer(uint32_t freq_mode, uint64_t shot_seq, WeaponClass cls, bool suppressed, bool is_player) {
    if (freq_mode == 0) return false;
    switch (freq_mode) {
        case 1: return true;                         // Every bullet
        case 2: return (shot_seq % 2ull) == 0ull;    // Every 2nd bullet
        case 3: return (shot_seq % 3ull) == 0ull;    // Every 3rd bullet
        case 4: return (shot_seq % 5ull) == 0ull;    // Every 5th bullet
        case 5: {  // Selective: authentic military mix
            if (cls == WeaponClass::Launcher || cls == WeaponClass::Sniper) return true;
            if (cls == WeaponClass::Lmg) return (shot_seq % 2ull) == 0ull;
            if (cls == WeaponClass::Shotgun) return (shot_seq % 2ull) == 0ull;
            if (suppressed) return (shot_seq % 3ull) == 0ull;
            if (cls == WeaponClass::Rifle || cls == WeaponClass::Smg) {
                return is_player ? ((shot_seq % 3ull) == 0ull) : ((shot_seq % 4ull) == 0ull);
            }
            return (shot_seq % 2ull) == 0ull;
        }
        default: return true;
    }
}

namespace {

inline float Clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

inline uint32_t PackBgra(float r, float g, float b, float a) {
    const uint32_t ua = uint32_t(Clamp01(a) * 255.0f + 0.5f);
    const uint32_t ur = uint32_t(Clamp01(r) * 255.0f + 0.5f);
    const uint32_t ug = uint32_t(Clamp01(g) * 255.0f + 0.5f);
    const uint32_t ub = uint32_t(Clamp01(b) * 255.0f + 0.5f);
    return (ua << 24) | (ur << 16) | (ug << 8) | ub;
}

inline float HashGrad2D(int ix, int iy, uint32_t seed, float gx, float gy) {
    uint32_t h = uint32_t(ix) * 374761393u + uint32_t(iy) * 668265263u + seed * 1442695041u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= (h >> 16);
    static constexpr float kGrads[8][2] = {
        { 1.0f,  0.0f}, {-1.0f,  0.0f}, { 0.0f,  1.0f}, { 0.0f, -1.0f},
        { 0.70710678f,  0.70710678f}, {-0.70710678f,  0.70710678f},
        { 0.70710678f, -0.70710678f}, {-0.70710678f, -0.70710678f},
    };
    const float* g = kGrads[h & 7u];
    return g[0] * gx + g[1] * gy;
}

// Triangular 2D Simplex gradient noise eliminates axis-aligned square ridges entirely.
inline float SimplexNoise2D(float x, float y, uint32_t seed) {
    constexpr float kF2 = 0.36602540378f;  // 0.5 * (sqrt(3) - 1)
    constexpr float kG2 = 0.21132486540f;  // (3 - sqrt(3)) / 6
    const float s = (x + y) * kF2;
    const int i = int(std::floor(x + s));
    const int j = int(std::floor(y + s));
    const float t = float(i + j) * kG2;
    const float x0 = x - (float(i) - t);
    const float y0 = y - (float(j) - t);
    const int i1 = (x0 > y0) ? 1 : 0;
    const int j1 = (x0 > y0) ? 0 : 1;
    const float x1 = x0 - float(i1) + kG2;
    const float y1 = y0 - float(j1) + kG2;
    const float x2 = x0 - 1.0f + 2.0f * kG2;
    const float y2 = y0 - 1.0f + 2.0f * kG2;

    float n = 0.0f;
    float t0 = 0.5f - x0 * x0 - y0 * y0;
    if (t0 > 0.0f) {
        t0 *= t0;
        n += t0 * t0 * HashGrad2D(i, j, seed, x0, y0);
    }
    float t1 = 0.5f - x1 * x1 - y1 * y1;
    if (t1 > 0.0f) {
        t1 *= t1;
        n += t1 * t1 * HashGrad2D(i + i1, j + j1, seed, x1, y1);
    }
    float t2 = 0.5f - x2 * x2 - y2 * y2;
    if (t2 > 0.0f) {
        t2 *= t2;
        n += t2 * t2 * HashGrad2D(i + 1, j + 1, seed, x2, y2);
    }
    return Clamp01(0.5f + 0.5f * std::clamp(70.0f * n, -1.0f, 1.0f));
}

inline float RotatedFbm2D(float x, float y, uint32_t seed, int octaves = 4) {
    float acc = 0.0f;
    float amp = 0.5f;
    float norm = 0.0f;
    for (int o = 0; o < octaves; o++) {
        acc += amp * SimplexNoise2D(x, y, seed + uint32_t(o) * 37u);
        norm += amp;
        // Orthonormal rotation + lacunarity 2.0 eliminates harmonic grid stacking.
        const float nx = 1.60f * x + 1.20f * y + 1.37f;
        const float ny = -1.20f * x + 1.60f * y - 0.91f;
        x = nx;
        y = ny;
        amp *= 0.5f;
    }
    return acc / norm;
}

inline void NormalizeDir3(const float in[3], float out[3]) {
    const float l2 = in[0] * in[0] + in[1] * in[1] + in[2] * in[2];
    if (l2 > 1e-8f) {
        const float inv = 1.0f / std::sqrt(l2);
        out[0] = in[0] * inv;
        out[1] = in[1] * inv;
        out[2] = in[2] * inv;
    } else {
        out[0] = 0.0f;
        out[1] = 1.0f;
        out[2] = 0.0f;
    }
}

inline bool IsFinite3(const float p[3]) {
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}

inline void BuildOrthoBasis(const float dir[3], float right[3], float up[3]) {
    const float ref[3] = {std::fabs(dir[1]) < 0.9f ? 0.0f : 1.0f,
                          std::fabs(dir[1]) < 0.9f ? 1.0f : 0.0f, 0.0f};
    right[0] = dir[1] * ref[2] - dir[2] * ref[1];
    right[1] = dir[2] * ref[0] - dir[0] * ref[2];
    right[2] = dir[0] * ref[1] - dir[1] * ref[0];
    const float rlen = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (rlen > 1e-5f) {
        right[0] /= rlen;
        right[1] /= rlen;
        right[2] /= rlen;
    } else {
        right[0] = 1.0f;
        right[1] = 0.0f;
        right[2] = 0.0f;
    }
    up[0] = dir[1] * right[2] - dir[2] * right[1];
    up[1] = dir[2] * right[0] - dir[0] * right[2];
    up[2] = dir[0] * right[1] - dir[1] * right[0];
}

}  // namespace

uint32_t SampleVfxAtlasTexelBgra(uint32_t tx, uint32_t ty, float u_tile, float v_tile) {
    const float ux = u_tile * 2.0f - 1.0f;
    const float uy = v_tile * 2.0f - 1.0f;
    const float r2 = ux * ux + uy * uy;
    const float r = std::sqrt(r2);
    const uint32_t tile_seed = (ty * 4u + tx) * 97u + 13u;

    // Generous circular zero-alpha safety guard keeps bilinear edges strictly black/transparent.
    constexpr float kMaxTileRadius = 0.78f;
    if (r >= kMaxTileRadius) {
        return 0u;
    }

    const float rn = r / kMaxTileRadius;
    const float env = 1.0f - rn;
    const float soft_env = env * env * (3.0f - 2.0f * env);
    // Keeps alpha rich through the inner flame body (rn <= 0.68) while fading cleanly to 0 before kMaxTileRadius.
    const float rim_ramp = Clamp01(env * 3.1f);
    const float rim_mask = rim_ramp * rim_ramp * (3.0f - 2.0f * rim_ramp);
    const float rgb_guard = Clamp01(env * 3.5f);

    if (ty == 0u) {
        // Row 0: Organic domain-warped hydrocarbon soot & smoke billows.
        const float qx = RotatedFbm2D(ux * 2.1f + float(tx) * 2.9f, uy * 2.1f + 1.4f, tile_seed, 4) - 0.5f;
        const float qy = RotatedFbm2D(ux * 2.1f - 1.8f, uy * 2.1f + float(tx) * 3.7f, tile_seed + 41u, 4) - 0.5f;
        const float w1 = Clamp01((RotatedFbm2D(ux * 3.8f + qx * 2.8f, uy * 3.8f + qy * 2.8f, tile_seed + 101u, 4) - 0.28f) * 2.1f);
        const float fine = Clamp01((RotatedFbm2D(ux * 7.5f - qy * 1.5f, uy * 7.5f + qx * 1.5f, tile_seed + 173u, 3) - 0.28f) * 2.0f);
        const float density = rim_mask * (1.0f - rn * 0.45f) * (0.42f + 0.65f * w1) * (0.64f + 0.36f * fine);
        const float alpha_f = Clamp01(density * 0.96f);
        const float lum = Clamp01(0.32f + 0.34f * w1 + 0.22f * fine - 0.14f * rn * rn) * rgb_guard;
        return PackBgra(lum, lum, lum, alpha_f);
    }

    if (ty == 1u) {
        // Row 1: Double domain-warped Simplex FBM flame tongues with carved cool eddy cavities
        // so layered additive sprites retain fiery internal curl instead of clamping to solid white.
        const float tilt_x = (tx == 0u || tx == 2u) ? 0.34f : -0.32f;
        const float qx = RotatedFbm2D(ux * 1.85f + float(tx) * 3.1f, uy * 1.85f - 1.7f, tile_seed, 4) - 0.5f;
        const float qy = RotatedFbm2D(ux * 1.85f - 2.3f, uy * 1.85f + float(tx) * 4.1f, tile_seed + 47u, 4) - 0.5f;
        const float wx = RotatedFbm2D(ux * 2.9f + 2.6f * qx + 1.7f, uy * 2.9f + 2.6f * qy + 9.2f,
                                      tile_seed + 109u, 4) - 0.5f;
        const float wy = RotatedFbm2D(ux * 2.9f + 2.6f * qx + 8.3f, uy * 2.9f + 2.6f * qy - 2.8f,
                                      tile_seed + 163u, 4) - 0.5f;

        // Strong fluid silhouette deformation breaks circularity into asymmetrical reaching flame tongues.
        const float up_reach = Clamp01(-uy * 0.90f);
        const float sx = ux * (0.90f + 0.28f * up_reach) + qx * 0.72f + wx * 0.54f +
                         tilt_x * (0.30f - uy) * 0.45f;
        const float sy_scale = (uy < 0.0f) ? 0.74f : 1.24f;
        const float sy = uy * sy_scale + qy * 0.74f + wy * 0.56f;
        const float rs = std::sqrt(sx * sx + sy * sy) / kMaxTileRadius;

        const float fine_raw = RotatedFbm2D(sx * 4.4f + wx * 2.4f, sy * 4.4f + wy * 2.4f, tile_seed + 241u, 4);
        const float wisp_raw = RotatedFbm2D(sx * 8.6f - wy * 1.6f, sy * 8.6f + wx * 1.6f, tile_seed + 317u, 3);
        const float billow = Clamp01((fine_raw - 0.34f) * 2.85f);
        const float tendril = Clamp01((wisp_raw - 0.36f) * 2.60f);
        const float curl_ridge = Clamp01(1.0f - std::fabs(wisp_raw * 2.0f - 1.0f) * 1.55f);
        const float licks = std::pow(Clamp01(billow * 0.64f + tendril * 0.24f + curl_ridge * 0.24f), 1.35f);

        // Compact luminous heart slightly above geometric lower-center without washing out the whole tile.
        const float core_dx = ux * 1.30f;
        const float core_dy = (uy - 0.04f) * 1.25f;
        const float core_dist = std::sqrt(core_dx * core_dx + core_dy * core_dy) / kMaxTileRadius;
        const float core = Clamp01(1.0f - core_dist * 1.55f);
        const float core_glow = core * core * (0.72f + 0.28f * billow);

        const float body_falloff = Clamp01(1.0f - rs * 0.96f);
        const float flame_mask =
            Clamp01(body_falloff * (0.04f + 1.28f * licks) + core_glow * 0.74f);
        const float alpha_f = Clamp01(flame_mask * rim_mask * 1.22f);

        // Rich hydrocarbon blackbody gradient: incandescent gold core -> vivid amber licks -> deep orange tips.
        const float heat = Clamp01(core_glow * 0.72f + licks * body_falloff * 0.72f - rs * 0.24f);
        const float r_f = Clamp01(0.92f + 0.08f * heat) * rgb_guard;
        const float g_f = Clamp01(0.30f + 0.54f * heat + 0.16f * heat * heat) * rgb_guard;
        const float b_f = Clamp01(0.03f + 0.56f * heat * heat * heat) * rgb_guard;
        return PackBgra(r_f, g_f, b_f, alpha_f);
    }

    if (ty == 2u) {
        // Row 2: Organic arterial blood splatters.
        const float sp = RotatedFbm2D(ux * 3.8f + float(tx) * 3.3f, uy * 3.8f - 2.2f, tile_seed + 307u, 4);
        const float drops = RotatedFbm2D(ux * 7.8f + sp, uy * 7.8f - sp, tile_seed + 409u, 3);
        const float mask = Clamp01((soft_env * 1.25f) * (0.45f + 0.85f * sp) - 0.18f * (1.0f - drops));
        const float shade = (0.32f + 0.40f * sp) * rgb_guard;
        return PackBgra(shade * 1.45f, shade * 0.12f, shade * 0.10f, Clamp01(mask * 1.4f));
    }

    // Row 3: tx 0..1 spark/muzzle stars; tx 2..3 shockwave rings.
    if (tx <= 1u) {
        const float ang = std::atan2(uy, ux);
        const float rays = 0.5f + 0.5f * std::cos(ang * (tx == 0u ? 4.0f : 6.0f));
        const float star = Clamp01((1.0f - rn) * (0.55f + 0.45f * rays * rays));
        const float flash = Clamp01(star * star * soft_env);
        return PackBgra(rgb_guard, 0.92f * rgb_guard, 0.72f * rgb_guard, flash);
    }

    const float ring_dist = std::fabs(rn - 0.64f) / 0.24f;
    const float ring = Clamp01(1.0f - ring_dist * ring_dist);
    return PackBgra(rgb_guard, 0.90f * rgb_guard, 0.78f * rgb_guard, ring * soft_env);
}

size_t BuildFlamethrowerJetSprites(const float (*positions)[3],
                                   const float* radii,
                                   const float* heats,
                                   size_t count,
                                   float stream_fade,
                                   float anim_time,
                                   bool perf_mode,
                                   FlamePlumeSprite* out,
                                   size_t max_out) {
    if (!positions || !radii || !heats || !out || max_out == 0u || count == 0u || stream_fade <= 0.002f) {
        return 0u;
    }

    size_t written = 0u;
    auto push = [&](const FlamePlumeSprite& sprite) {
        if (written < max_out && IsFinite3(sprite.pos)) {
            out[written++] = sprite;
        }
    };

    auto emit_puff = [&](const float p[3], float r, float h, uint32_t seq, const float dir[3],
                         const float right[3], const float up[3]) {
        const float hc = Clamp01(h);
        const float rc = std::clamp(r, 0.045f, 1.35f);
        const float phase = anim_time * 9.5f + float(seq) * 0.87f;
        const float wobble = (1.0f - hc * 0.68f) * rc * 0.22f;
        const float ox = std::cos(phase);
        const float oy = std::sin(phase * 1.19f + 0.7f);
        const float rise = (1.0f - hc) * rc * 0.24f * (0.5f + 0.5f * std::sin(phase * 0.83f));

        const float puff_pos[3] = {
            p[0] + right[0] * ox * wobble + up[0] * oy * wobble,
            p[1] + right[1] * ox * wobble + up[1] * oy * wobble + rise,
            p[2] + right[2] * ox * wobble + up[2] * oy * wobble,
        };

        // Flow-aligned elongation: fast-moving plasma jet near/mid muzzle stretches cleanly along velocity,
        // relaxing downrange into turbulent curling flame eddies (no discrete square cards).
        const float stretch_t = Clamp01((hc - 0.26f) / 0.74f);
        const float flutter = (1.0f - hc * 0.52f) * 0.24f;
        const float raw_dir[3] = {
            dir[0] + right[0] * ox * flutter + up[0] * oy * flutter,
            dir[1] + right[1] * ox * flutter + up[1] * oy * flutter + 0.16f * (1.0f - hc),
            dir[2] + right[2] * ox * flutter + up[2] * oy * flutter,
        };
        float flow_dir[3]{};
        NormalizeDir3(raw_dir, flow_dir);

        // 1. Subtle luminous spine glow along jet core (low alpha avoids washing out flames into white).
        FlamePlumeSprite core;
        core.kind = FlameSpriteKind::CoreDisc;
        core.pos[0] = puff_pos[0];
        core.pos[1] = puff_pos[1];
        core.pos[2] = puff_pos[2];
        core.dir[0] = flow_dir[0];
        core.dir[1] = flow_dir[1];
        core.dir[2] = flow_dir[2];
        core.radius = rc * (0.44f + 0.14f * hc);
        core.stretch = 1.15f + 0.80f * stretch_t;
        core.rotation = 0.0f;
        core.r = 1.0f;
        core.g = 0.76f + 0.18f * hc;
        core.b = 0.18f + 0.36f * hc * hc;
        core.alpha = Clamp01((0.16f + 0.22f * hc) * stream_fade);
        core.tile = 4;
        push(core);

        // 2. Flow-stretched swirling flame tongue (luminous golden-amber body without white clipping).
        FlamePlumeSprite fire;
        fire.kind = FlameSpriteKind::FireBillboard;
        fire.pos[0] = puff_pos[0];
        fire.pos[1] = puff_pos[1];
        fire.pos[2] = puff_pos[2];
        fire.dir[0] = flow_dir[0];
        fire.dir[1] = flow_dir[1];
        fire.dir[2] = flow_dir[2];
        fire.radius = rc * (1.36f + 0.54f * (1.0f - hc));
        fire.stretch = 1.18f + 1.12f * stretch_t;
        fire.rotation = 0.35f * std::sin(phase * 0.65f + float(seq) * 1.45f) +
                        (1.0f - stretch_t) * (phase * 0.45f + float(seq) * 0.95f);
        fire.r = 1.0f;
        fire.g = 0.52f + 0.36f * hc;
        fire.b = 0.04f + 0.16f * hc * hc;
        fire.alpha = Clamp01((0.36f + 0.24f * hc) * stream_fade);
        fire.tile = 4 + int((seq * 5u + 1u) & 3u);
        push(fire);

        // 3. Cooled outer hydrocarbon soot/smoke envelope (Row 0: tiles 0..3).
        if (hc < 0.78f && (seq & 1u) == 0u) {
            FlamePlumeSprite smoke;
            smoke.kind = FlameSpriteKind::SmokeBillboard;
            smoke.pos[0] = puff_pos[0];
            smoke.pos[1] = puff_pos[1] + rc * 0.34f;
            smoke.pos[2] = puff_pos[2];
            smoke.dir[0] = 0.0f;
            smoke.dir[1] = 1.0f;
            smoke.dir[2] = 0.0f;
            smoke.radius = rc * (1.82f + 0.48f * (1.0f - hc));
            smoke.stretch = 1.05f + 0.16f * (1.0f - hc);
            smoke.rotation = -phase * 0.35f + float(seq) * 0.67f;
            smoke.r = 0.16f;
            smoke.g = 0.14f;
            smoke.b = 0.13f;
            smoke.alpha = Clamp01((0.15f + 0.17f * (1.0f - hc)) * stream_fade);
            smoke.tile = int(seq & 3u);
            push(smoke);
        }
    };

    const float default_dir[3] = {0.0f, 0.0f, 1.0f};
    const float default_right[3] = {1.0f, 0.0f, 0.0f};
    const float default_up[3] = {0.0f, 1.0f, 0.0f};
    if (count == 1u) {
        if (IsFinite3(positions[0])) {
            emit_puff(positions[0], radii[0], heats[0], 0u, default_dir, default_right, default_up);
        }
        return written;
    }

    // Order valid indices from newest/hottest (muzzle) to oldest/coolest (tail) so
    // any out-of-order array compaction in the simulator never produces zig-zag spans.
    constexpr size_t kMaxInlineSegs = 256u;
    const size_t n_valid = std::min(count, kMaxInlineSegs);
    size_t order[kMaxInlineSegs]{};
    size_t order_count = 0u;
    for (size_t i = 0; i < n_valid; i++) {
        if (IsFinite3(positions[i])) {
            order[order_count++] = i;
        }
    }
    if (order_count == 0u) {
        return 0u;
    }
    std::sort(order, order + order_count, [&](size_t ia, size_t ib) {
        return heats[ia] > heats[ib];
    });
    if (order_count == 1u) {
        const size_t idx0 = order[0];
        emit_puff(positions[idx0], radii[idx0], heats[idx0], 0u, default_dir, default_right, default_up);
        return written;
    }

    // Threshold beyond which dispersed tail droplets should billow independently
    // rather than be connected by a straight line across empty air.
    constexpr float kMaxContinuousSpan = 1.45f;
    const float target_step = perf_mode ? 0.21f : 0.14f;
    const uint32_t max_substeps = perf_mode ? 10u : 16u;
    uint32_t seq = 0u;
    bool endpoint_emitted = false;

    for (size_t k = 0; k + 1 < order_count; k++) {
        const size_t ia = order[k];
        const size_t ib = order[k + 1];
        const float* a = positions[ia];
        const float* b = positions[ib];
        const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float seg_len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);

        float dir[3] = {0.0f, 0.0f, 1.0f};
        float right[3] = {1.0f, 0.0f, 0.0f};
        float up[3] = {0.0f, 1.0f, 0.0f};
        if (seg_len > 1e-4f) {
            dir[0] = d[0] / seg_len;
            dir[1] = d[1] / seg_len;
            dir[2] = d[2] / seg_len;
            BuildOrthoBasis(dir, right, up);
        }

        // Disconnected gap (dispersed tail droplet or ground cutoff): emit organic puffs at endpoints only.
        if (seg_len > kMaxContinuousSpan) {
            if (!endpoint_emitted) {
                emit_puff(a, radii[ia], heats[ia], seq++, dir, right, up);
            }
            emit_puff(b, radii[ib], heats[ib], seq++, dir, right, up);
            endpoint_emitted = true;
            continue;
        }

        const uint32_t substeps =
            std::clamp(uint32_t(std::ceil(seg_len / target_step)), 1u, max_substeps);
        for (uint32_t s = (endpoint_emitted ? 1u : 0u); s < substeps; s++) {
            const float f = float(s) / float(substeps);
            const float pos_s[3] = {a[0] + d[0] * f, a[1] + d[1] * f, a[2] + d[2] * f};
            const float rad_s = radii[ia] + (radii[ib] - radii[ia]) * f;
            const float heat_s = heats[ia] + (heats[ib] - heats[ia]) * f;
            emit_puff(pos_s, rad_s, heat_s, seq++, dir, right, up);
        }
        if (k + 2 == order_count) {
            emit_puff(b, radii[ib], heats[ib], seq++, dir, right, up);
        }
        endpoint_emitted = false;
    }

    return written;
}

size_t BuildFlamethrowerPoolSprites(const float (*positions)[3],
                                    const float* radii,
                                    const float* heats,
                                    size_t count,
                                    float stream_fade,
                                    float anim_time,
                                    FlamePlumeSprite* out,
                                    size_t max_out) {
    if (!positions || !radii || !heats || !out || max_out == 0u || count == 0u || stream_fade <= 0.002f) {
        return 0u;
    }

    size_t written = 0u;
    auto push = [&](const FlamePlumeSprite& sprite) {
        if (written < max_out && IsFinite3(sprite.pos)) {
            out[written++] = sprite;
        }
    };

    for (size_t i = 0; i < count; i++) {
        if (!IsFinite3(positions[i])) {
            continue;
        }
        const float h = Clamp01(heats[i]);
        const float r = std::clamp(radii[i], 0.12f, 2.2f);
        const int idx = int(i);

        // Ground hydrocarbon scorch / smoke base (Row 0: tiles 0..3).
        FlamePlumeSprite sg;
        sg.kind = FlameSpriteKind::SmokeGround;
        sg.pos[0] = positions[i][0];
        sg.pos[1] = positions[i][1];
        sg.pos[2] = positions[i][2];
        sg.radius = r * 1.28f;
        sg.rotation = float(i) * 1.07f;
        sg.r = 0.10f;
        sg.g = 0.09f;
        sg.b = 0.08f;
        sg.alpha = Clamp01(0.34f * stream_fade);
        sg.tile = idx & 3;
        push(sg);

        // Ground pool flame glow (Row 1: tiles 4..7 — never Row 2 blood tiles!).
        FlamePlumeSprite fg;
        fg.kind = FlameSpriteKind::FireGround;
        fg.pos[0] = positions[i][0];
        fg.pos[1] = positions[i][1];
        fg.pos[2] = positions[i][2];
        fg.radius = r * 1.02f;
        fg.rotation = anim_time * 1.1f - float(i) * 0.83f;
        fg.r = 1.0f;
        fg.g = 0.50f + 0.36f * h;
        fg.b = 0.10f + 0.22f * h * h;
        fg.alpha = Clamp01(0.50f * h * stream_fade);
        fg.tile = 4 + (idx & 3);
        push(fg);

        // Upward-licking stretched flame tongue above the pool (no sideways spinning squares).
        const float sway_x = std::sin(anim_time * 4.3f + float(i) * 1.71f) * 0.18f;
        const float sway_z = std::cos(anim_time * 3.9f + float(i) * 2.31f) * 0.18f;
        const float raw_up[3] = {sway_x, 1.0f, sway_z};
        FlamePlumeSprite fb;
        fb.kind = FlameSpriteKind::FireBillboard;
        fb.pos[0] = positions[i][0];
        fb.pos[1] = positions[i][1] + 0.14f + 0.20f * h;
        fb.pos[2] = positions[i][2];
        NormalizeDir3(raw_up, fb.dir);
        fb.radius = r * 0.84f;
        fb.stretch = 1.45f + 0.45f * h;
        fb.rotation = 0.14f * std::sin(anim_time * 5.2f + float(i) * 1.9f);
        fb.r = 1.0f;
        fb.g = 0.46f + 0.38f * h;
        fb.b = 0.08f + 0.16f * h * h;
        fb.alpha = Clamp01(0.46f * h * stream_fade);
        fb.tile = 4 + ((idx + 1) & 3);
        push(fb);

        if (h < 0.70f) {
            FlamePlumeSprite sb;
            sb.kind = FlameSpriteKind::SmokeBillboard;
            sb.pos[0] = positions[i][0];
            sb.pos[1] = positions[i][1] + 0.30f + 0.25f * (1.0f - h);
            sb.pos[2] = positions[i][2];
            sb.dir[0] = 0.0f;
            sb.dir[1] = 1.0f;
            sb.dir[2] = 0.0f;
            sb.radius = r * 1.15f;
            sb.stretch = 1.12f;
            sb.rotation = anim_time * 0.7f + float(i) * 0.59f;
            sb.r = 0.14f;
            sb.g = 0.12f;
            sb.b = 0.11f;
            sb.alpha = Clamp01(0.24f * (1.0f - h) * stream_fade);
            sb.tile = (idx + 2) & 3;
            push(sb);
        }
    }

    return written;
}

bool ShouldEjectShellCasing(const char* code3, bool alt_fire) {
    if (!code3 || !code3[0]) return false;
    char norm[8] = {};
    size_t i = 0;
    while (code3[i] && i < 7) {
        char c = code3[i];
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        norm[i++] = c;
    }
    norm[i] = '\0';
    if (std::strcmp(norm, "FLM") == 0) return false;
    if (std::strcmp(norm, "RPG") == 0 || std::strcmp(norm, "GRL") == 0) return false;
    if (alt_fire && std::strcmp(norm, "BNS") == 0) return false;  // Underbarrel M203 grenade
    const WeaponClass cls = ClassifyWeaponCode(norm);
    return cls != WeaponClass::Launcher && cls != WeaponClass::Unknown;
}

ShellCasingProfile GetShellCasingProfile(WeaponClass cls) {
    ShellCasingProfile p;
    switch (cls) {
        case WeaponClass::Pistol:
            p.ejects = true;
            p.length_m = 0.024f;
            p.radius_m = 0.0065f;
            p.eject_speed_right = 2.4f;
            p.eject_speed_up = 1.85f;
            p.eject_speed_back = 0.65f;
            break;
        case WeaponClass::Smg:
            p.ejects = true;
            p.length_m = 0.028f;
            p.radius_m = 0.0065f;
            p.eject_speed_right = 2.75f;
            p.eject_speed_up = 2.0f;
            p.eject_speed_back = 0.70f;
            break;
        case WeaponClass::Rifle:
            p.ejects = true;
            p.length_m = 0.048f;
            p.radius_m = 0.0075f;
            p.eject_speed_right = 2.95f;
            p.eject_speed_up = 2.1f;
            p.eject_speed_back = 0.85f;
            break;
        case WeaponClass::Shotgun:
            p.ejects = true;
            p.red_shotgun_hull = true;
            p.length_m = 0.068f;
            p.radius_m = 0.011f;
            p.eject_speed_right = 2.3f;
            p.eject_speed_up = 1.75f;
            p.eject_speed_back = 0.60f;
            break;
        case WeaponClass::Sniper:
            p.ejects = true;
            p.length_m = 0.064f;
            p.radius_m = 0.009f;
            p.eject_speed_right = 2.2f;
            p.eject_speed_up = 1.65f;
            p.eject_speed_back = 0.75f;
            break;
        case WeaponClass::Lmg:
            p.ejects = true;
            p.length_m = 0.056f;
            p.radius_m = 0.008f;
            p.eject_speed_right = 3.25f;
            p.eject_speed_up = 2.05f;
            p.eject_speed_back = 0.90f;
            break;
        default:
            p.ejects = false;
            break;
    }
    return p;
}

bool StepShellCasingPhysics(ShellCasingBody& body, float dt) {
    if (!body.active || !std::isfinite(dt) || dt <= 0.0f) return false;
    body.age += dt;
    if (body.age >= body.ttl) {
        body.active = false;
        return false;
    }

    body.vel[1] -= 9.81f * dt;
    body.pos[0] += body.vel[0] * dt;
    body.pos[1] += body.vel[1] * dt;
    body.pos[2] += body.vel[2] * dt;
    body.spin_rad += body.spin_vel * dt;

    bool first_bounce = false;
    if (body.pos[1] <= body.ground_y) {
        body.pos[1] = body.ground_y;
        if (body.bounces == 0) {
            first_bounce = true;
        }
        if (body.bounces < 8) {
            body.bounces++;
        }
        const float impact_vy = std::fabs(body.vel[1]);
        if (impact_vy > 0.35f) {
            body.vel[1] = impact_vy * 0.42f;
            body.vel[0] *= 0.72f;
            body.vel[2] *= 0.72f;
            body.spin_vel *= 0.65f;
        } else {
            body.vel[1] = 0.0f;
            body.vel[0] *= 0.85f;
            body.vel[2] *= 0.85f;
            body.spin_vel *= 0.50f;
        }
    }
    return first_bounce;
}

}  // namespace ballistics

namespace haptics {

namespace {

inline void NormalizeWeaponCode(const char* raw, char out[8]) {
  size_t n = 0;
  if (raw) {
    while (raw[n] && n < 7) {
      char c = raw[n];
      if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
      out[n++] = c;
    }
  }
  out[n] = '\0';
}

inline uint8_t ClampAmp(int v) {
  return uint8_t(std::clamp(v, 0, 255));
}

}  // namespace

WeaponHapticPulse ComputeWeaponHapticPulse(const char* weapon_code,
                                           bool suppressed,
                                           bool alt_fire,
                                           bool rapid_fire_cheat) {
  char code[8] = {};
  NormalizeWeaponCode(weapon_code, code);
  if (!code[0]) return WeaponHapticPulse{};

  auto is = [&](const char* k) { return std::strcmp(code, k) == 0; };

  WeaponHapticPulse p{};
  p.triggered = true;

  // Heavy launchers & underbarrel M203: thunderous full-power shockwave
  if (is("RPG") || (is("BNS") && alt_fire)) {
    p.duration_ms = is("RPG") ? 132u : 112u;
    p.amplitude = is("RPG") ? 255u : 252u;
    p.cooldown_ms = 240u;
    p.priority = 3u;
    suppressed = false;  // Launchers have no silencer reduction
  } else if (is("GRL")) {
    p.duration_ms = 116u;
    p.amplitude = 255u;
    p.cooldown_ms = 220u;
    p.priority = 3u;
    suppressed = false;
  // High-caliber Shotguns, Snipers, Magnum & Heavy Machine Gun: hard recoil slam
  } else if (is("SNR") || is("SN5")) {
    p.duration_ms = 82u;
    p.amplitude = 242u;
    p.cooldown_ms = 140u;
    p.priority = 2u;
  } else if (is("SHG") || is("SH5") || is("RM1") || is("RM5")) {
    p.duration_ms = 76u;
    p.amplitude = 236u;
    p.cooldown_ms = 115u;
    p.priority = 2u;
  } else if (is("MGN") || is("MG5")) {
    p.duration_ms = 70u;
    p.amplitude = 226u;
    p.cooldown_ms = 110u;
    p.priority = 2u;
  } else if (is("HVY") || is("HV5")) {
    p.duration_ms = 56u;
    p.amplitude = 212u;
    p.cooldown_ms = 80u;
    p.priority = 2u;
  // Flamethrower: heavy pressurized M2 combustion throb while jetting napalm
  } else if (is("FLM")) {
    p.duration_ms = 56u;
    p.amplitude = 212u;
    p.cooldown_ms = 195u;
    p.priority = 1u;
  // Mauser 1891-12 (7.65x53mm Argentine): sharp high-energy wooden-stock rifle kick
  } else if (is("M91")) {
    p.duration_ms = 68u;
    p.amplitude = 232u;
    p.cooldown_ms = 75u;
    p.priority = 2u;
  // Mauser C96 / M712 (7.63x25mm): snappy short-recoil high-velocity pistol impulse
  } else if (is("C96")) {
    p.duration_ms = 42u;
    p.amplitude = 188u;
    p.cooldown_ms = 48u;
    p.priority = 1u;
  // Maxim PM M1910 (7.62x54R): sustained heavyweight Sokolov carriage recoil cadence
  } else if (is("MXM")) {
    p.duration_ms = 54u;
    p.amplitude = 218u;
    p.cooldown_ms = 68u;
    p.priority = 2u;
  // Assault Rifles: punchy, crisp tactile recoil snap
  } else if (is("AK1") || is("AK5") || is("ASR") || is("AS5") || is("M16") ||
             is("M15") || is("M45") || is("BNS")) {
    p.duration_ms = 48u;
    p.amplitude = 196u;
    p.cooldown_ms = 62u;
    p.priority = 1u;
  // Sidearms: crisp palm kick
  } else if (is("PST") || is("PS5") || is("GK1") || is("GK5")) {
    p.duration_ms = 38u;
    p.amplitude = 165u;
    p.cooldown_ms = 58u;
    p.priority = 1u;
  // SMGs: rapid, unmistakable high-tempo recoil snaps
  } else if (is("SMG") || is("SM3") || is("SM5") || is("MC5") || is("MP1") ||
             is("MP5") || is("P90") || is("P95")) {
    p.duration_ms = 34u;
    p.amplitude = 152u;
    p.cooldown_ms = 52u;
    p.priority = 1u;
  } else {
    p.duration_ms = 40u;
    p.amplitude = 170u;
    p.cooldown_ms = 60u;
    p.priority = 1u;
  }

  if (suppressed) {
    p.duration_ms = std::max(26u, uint32_t(std::lround(float(p.duration_ms) * 0.72f)));
    p.amplitude = ClampAmp(int(std::lround(float(p.amplitude) * 0.62f)));
    p.amplitude = std::max<uint8_t>(p.amplitude, 96u);
  }
  if (rapid_fire_cheat) {
    p.cooldown_ms = std::max(p.cooldown_ms, 85u);
    p.amplitude = ClampAmp(int(std::lround(float(p.amplitude) * 0.85f)));
  }
  return p;
}

WeaponHapticPulse ComputeExplosionHapticPulse(float distance_m) {
  if (!std::isfinite(distance_m) || distance_m < 0.0f || distance_m >= 32.0f) {
    return WeaponHapticPulse{};
  }
  const float norm = std::clamp(1.0f - distance_m / 32.0f, 0.0f, 1.0f);
  const float curve = std::pow(norm, 1.15f);
  const uint32_t dur = 48u + uint32_t(std::lround(122.0f * curve));
  const int amp = 90 + int(std::lround(185.0f * curve));
  if (amp < 70) return WeaponHapticPulse{};

  WeaponHapticPulse p{};
  p.duration_ms = std::clamp(dur, 48u, 170u);
  p.amplitude = ClampAmp(amp);
  p.cooldown_ms = (distance_m <= 8.5f) ? 200u : 150u;
  p.priority = (distance_m <= 8.5f) ? 4u : ((distance_m <= 18.0f) ? 3u : 2u);
  p.triggered = true;
  return p;
}

void HapticDispatcher::Reset() {
  last_emit_ms_ = 0;
  busy_until_ms_ = 0;
  last_cooldown_ms_ = 0;
  last_priority_ = 0;
}

bool HapticDispatcher::TrySchedule(uint64_t now_ms,
                                   const WeaponHapticPulse& candidate,
                                   bool enabled,
                                   WeaponHapticPulse* out_emit) {
  if (out_emit) *out_emit = WeaponHapticPulse{};
  if (!enabled || !candidate.triggered || candidate.duration_ms == 0u ||
      candidate.amplitude == 0u) {
    return false;
  }

  const bool overrides = candidate.priority > last_priority_;
  if (last_emit_ms_ != 0u && !overrides) {
    if (now_ms < busy_until_ms_) return false;
    const uint32_t req_gap = std::max(candidate.cooldown_ms, last_cooldown_ms_);
    if (now_ms < last_emit_ms_ || (now_ms - last_emit_ms_) < uint64_t(req_gap)) {
      return false;
    }
  }

  last_emit_ms_ = now_ms;
  busy_until_ms_ = now_ms + uint64_t(candidate.duration_ms);
  last_cooldown_ms_ = candidate.cooldown_ms;
  last_priority_ = candidate.priority;
  if (out_emit) *out_emit = candidate;
  return true;
}

}  // namespace haptics
}  // namespace game
