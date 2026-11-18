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

inline float SmoothStep(float e0, float e1, float x) {
    const float t = Clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
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
    // Keeps alpha rich through the inner flame/smoke body while fading cleanly to 0 before kMaxTileRadius.
    const float rim_ramp = Clamp01(env * 3.1f);
    const float rim_mask = rim_ramp * rim_ramp * (3.0f - 2.0f * rim_ramp);
    const float rgb_guard = Clamp01(env * 3.5f);

    if (ty == 0u) {
        // Row 0: Volumetric Self-Shadowed Smoke Billows (Smooth Multi-Lobe Optical
        // Density Integration + Beer-Lambert Directional Light Extinction + Mie Silver Lining).
        const float qx = (RotatedFbm2D(ux * 1.55f + float(tx) * 2.3f,
                                       uy * 1.55f + 0.9f, tile_seed, 4) - 0.5f) * 0.42f;
        const float qy = (RotatedFbm2D(ux * 1.55f - 1.4f,
                                       uy * 1.55f + float(tx) * 3.1f, tile_seed + 41u, 4) - 0.5f) * 0.42f;
        const float wx = ux + qx;
        const float wy = uy + qy;
        const float turb_low = RotatedFbm2D(wx * 2.15f + float(tx) * 1.7f,
                                            wy * 2.15f - 0.8f, tile_seed + 101u, 4);
        const float turb_mid = RotatedFbm2D(wx * 3.85f - qy * 1.6f,
                                            wy * 3.85f + qx * 1.6f, tile_seed + 173u, 4);
        const float turb_fine = RotatedFbm2D(wx * 6.80f + qx * 2.1f,
                                             wy * 6.80f - qy * 2.1f, tile_seed + 257u, 3);

        // Smoothly integrated volumetric density across 7 buoyant cauliflower smoke lobes
        auto sample_cloud_field = [&](float px, float py) -> float {
            float sum = 0.0f;
            for (int k = 0; k < 7; ++k) {
                float cx = 0.0f, cy = 0.0f, rad_lobe = 0.56f, weight = 0.66f;
                if (k > 0) {
                    const float ang = float(k - 1) * 1.04719755f + float(tx) * 0.73f +
                                      0.32f * std::sin(float(k * 3u + tx * 5u));
                    const float dist = 0.27f + 0.08f * std::cos(float(k * 7u + tx * 2u));
                    cx = std::cos(ang) * dist;
                    cy = std::sin(ang) * dist;
                    rad_lobe = 0.44f + 0.06f * std::sin(float(k * 5u + tx));
                    weight = 0.40f;
                }
                const float dx = (px - cx) / rad_lobe;
                const float dy = (py - cy) / rad_lobe;
                const float d2 = dx * dx + dy * dy;
                if (d2 < 1.0f) {
                    const float fall = 1.0f - d2;
                    sum += weight * fall * fall * (1.0f + 0.5f * d2);
                }
            }
            return Clamp01(sum * 0.94f);
        };

        const float base_field = sample_cloud_field(wx, wy);
        // Sample optical thickness towards upper-left key light L=(-0.07, -0.11) for Beer-Lambert self-shadowing
        const float lit_field = sample_cloud_field(wx - 0.07f, wy - 0.11f);
        const float lit_turb = RotatedFbm2D((wx - 0.05f) * 2.15f + float(tx) * 1.7f,
                                            (wy - 0.08f) * 2.15f - 0.8f, tile_seed + 101u, 3);

        const float fbm_billow = 0.52f * turb_low + 0.32f * turb_mid + 0.16f * turb_fine;
        const float density = Clamp01(
            SmoothStep(0.05f, 0.58f, base_field * (0.54f + 0.78f * fbm_billow)) * rim_mask);
        const float wisp_scale = (tx >= 2u) ? 0.88f : 0.96f;
        const float alpha_f = Clamp01(std::pow(density, 1.05f) * wisp_scale);

        // Beer-Lambert self-shadowing: optical depth along light ray attenuates interior/underbelly
        const float shadow_ray_depth = Clamp01((lit_field - base_field) * 2.35f +
                                               (lit_turb - turb_low) * 1.30f +
                                               (uy + 0.35f) * 0.40f);
        const float beer_transmittance = std::exp(-1.55f * shadow_ray_depth);
        // Mie forward-scattered silver-lining at thin wispy edges facing the light
        const float silver_lining =
            Clamp01(density * (1.0f - density) * 1.45f * beer_transmittance);

        const float lum = Clamp01(
            (0.20f + 0.54f * beer_transmittance * (0.68f + 0.32f * fbm_billow) +
             0.16f * silver_lining) *
            rgb_guard);
        return PackBgra(lum, lum, lum, alpha_f);
    }

    if (ty == 1u) {
        // Row 1: Coherent Laminar-to-Turbulent Diffusion Flame Tongues & Vortex Ribbons
        // with true Planck Blackbody Radiation spectrum (White-Gold 2300K -> Amber 1700K -> Crimson-Red 1150K).
        const float sign_x = (tx == 0u || tx == 2u) ? 1.0f : -1.0f;
        const float reach = Clamp01((0.65f - uy) * 0.78f);

        // Smooth S-curve Kelvin-Helmholtz vortex wave along vertical axis
        const float wave_phase = uy * 3.2f + float(tx) * 1.57f;
        const float s_curve = sign_x * std::sin(wave_phase) * (0.14f + 0.22f * reach);

        // Low-frequency coherent domain warp preserves smooth liquid-like flame sheets
        const float qx = RotatedFbm2D(ux * 1.35f + float(tx) * 2.7f,
                                      uy * 1.15f - 0.9f, tile_seed, 3) - 0.5f;
        const float qy = RotatedFbm2D(ux * 1.35f - 1.9f,
                                      uy * 1.15f + float(tx) * 3.3f, tile_seed + 47u, 3) - 0.5f;

        const float taper = 0.82f + 0.95f * reach * reach;
        const float sy_scale = 0.96f + 0.18f * uy;
        const float fx = (ux - s_curve + qx * (0.38f + 0.32f * reach)) * taper;
        const float fy = (uy - 0.08f + qy * 0.34f) * sy_scale;
        const float flame_r = std::sqrt(fx * fx + fy * fy) / kMaxTileRadius;

        // Secondary licking tongues & carved cool air entrainment eddies (Rayleigh-Taylor fingers)
        const float tongue_fbm = RotatedFbm2D(fx * 2.15f + qx * 1.2f,
                                              fy * 1.65f - 0.6f, tile_seed + 109u, 3);
        const float lick_fbm = RotatedFbm2D(fx * 3.60f - qy * 1.1f,
                                            fy * 2.40f + 1.1f, tile_seed + 181u, 3);
        const float eddy_carve = SmoothStep(0.28f, 0.68f, tongue_fbm * 0.68f + lick_fbm * 0.32f);

        const float sheet_body =
            SmoothStep(1.02f, 0.18f, flame_r - (eddy_carve - 0.45f) * 0.42f);
        const float outer_licks =
            SmoothStep(0.42f, 0.78f, lick_fbm) * SmoothStep(1.05f, 0.35f, flame_r);

        // Compact incandescent reaction heart anchored in lower-middle fuel zone
        const float core_dx = (ux - s_curve * 0.4f) * 1.35f;
        const float core_dy = (uy - 0.10f) * 1.30f;
        const float core_d = std::sqrt(core_dx * core_dx + core_dy * core_dy) / kMaxTileRadius;
        const float heart = SmoothStep(0.72f, 0.0f, core_d) * (0.75f + 0.25f * tongue_fbm);

        const float flame_density = Clamp01(sheet_body * (0.25f + 0.85f * eddy_carve) +
                                            outer_licks * 0.48f + heart * 0.65f);
        const float alpha_f = Clamp01(flame_density * rim_mask * 1.18f);

        // Planck Blackbody radiation spectrum mapped across normalized temperature T in [0..1]
        const float temp = Clamp01(heart * 0.68f + sheet_body * eddy_carve * 0.58f +
                                   outer_licks * 0.28f - rn * 0.18f);
        const float r_f =
            Clamp01(SmoothStep(0.02f, 0.24f, temp) * (0.88f + 0.12f * temp)) * rgb_guard;
        const float g_f =
            Clamp01(std::pow(Clamp01((temp - 0.12f) / 0.88f), 1.42f) * 0.94f) * rgb_guard;
        const float b_f =
            Clamp01(std::pow(Clamp01((temp - 0.46f) / 0.54f), 2.15f) * 0.72f) * rgb_guard;
        return PackBgra(r_f, g_f, b_f, alpha_f);
    }

    if (ty == 2u) {
        // Row 2 (Tiles 8..11): Specialized 4-tile ballistic blood atlas with Beer-Lambert hemoglobin absorption:
        // Tile 8 (tx=0): Atomized arterial aerosol mist + cellular micro-droplets
        // Tile 9 (tx=1): High-velocity directional arterial spurt & liquid ligaments
        // Tile 10 (tx=2): Coagulating ground pool with Marangoni surface-tension meniscus
        // Tile 11 (tx=3): Ballistic impact spatter with radial crown spines & satellite drops
        const float rad = std::sqrt(r2 + 1e-6f);
        const float dir_x = ux / rad;
        const float dir_y = uy / rad;
        const float border_guard = Clamp01((kMaxTileRadius - rad) * 9.0f);
        const float wx = ux + (RotatedFbm2D(ux * 3.2f, uy * 3.2f, tile_seed + 19u, 3) - 0.5f) * 0.30f;
        const float wy = uy + (RotatedFbm2D(ux * 3.2f + 3.7f, uy * 3.2f - 2.3f, tile_seed + 47u, 3) - 0.5f) * 0.30f;
        const float crown = RotatedFbm2D(dir_x * 6.5f + wx * 2.1f, dir_y * 6.5f + wy * 2.1f, tile_seed + 103u, 4);
        const float drops = RotatedFbm2D(ux * 9.5f + crown, uy * 9.5f - crown, tile_seed + 211u, 3);

        float alpha_f = 0.0f;
        float thick = 0.68f;
        float meniscus = 0.0f;
        if (tx == 0u) {
            const float aerosol = Clamp01((soft_env * (0.52f + 0.58f * crown)) * 1.25f);
            const float fine_drops = SmoothStep(0.52f, 0.78f, drops) * soft_env;
            alpha_f = Clamp01((aerosol * 0.68f + fine_drops * 0.92f) * border_guard);
            meniscus = fine_drops * 0.50f;
            thick = Clamp01(0.55f + 0.38f * fine_drops);
        } else if (tx == 1u) {
            const float streak_r2 = wx * wx * 2.15f + wy * wy * 0.85f;
            const float body_sdf = 1.0f - streak_r2 * (0.88f + 0.75f * (1.0f - crown));
            const float sharp_body = Clamp01((body_sdf - 0.16f) * 7.5f);
            const float sat = SmoothStep(0.56f, 0.80f, drops) * soft_env * (1.0f - sharp_body * 0.75f);
            alpha_f = Clamp01(std::max(sharp_body, sat * 0.94f) * border_guard);
            meniscus = sharp_body * (1.0f - Clamp01((body_sdf - 0.30f) * 5.0f)) * 0.85f;
            thick = Clamp01(0.54f + 0.36f * sharp_body + 0.14f * meniscus);
        } else if (tx == 2u) {
            const float pool_sdf = 0.68f + 0.28f * (crown - 0.5f) - (wx * wx + wy * wy);
            const float sharp_pool = Clamp01(pool_sdf * 11.5f);
            meniscus = sharp_pool * (1.0f - Clamp01((pool_sdf - 0.07f) * 10.5f));
            alpha_f = Clamp01(sharp_pool * (0.94f + 0.06f * meniscus) * border_guard);
            thick = Clamp01(0.48f + 0.38f * (1.0f - Clamp01(pool_sdf * 2.4f)) + 0.16f * meniscus);
        } else {
            const float spatter_sdf = 1.0f - (wx * wx + wy * wy) * 1.45f + (crown - 0.45f) * 0.64f * Clamp01(rad * 1.8f);
            const float main_splat = Clamp01((spatter_sdf - 0.20f) * 9.0f);
            const float rad_drops = SmoothStep(0.50f, 0.76f, drops) * soft_env * (1.0f - main_splat * 0.80f);
            alpha_f = Clamp01(std::max(main_splat, rad_drops * 0.95f) * border_guard);
            meniscus = main_splat * (1.0f - Clamp01((spatter_sdf - 0.32f) * 6.0f)) * 0.80f;
            thick = Clamp01(0.52f + 0.36f * main_splat + 0.15f * meniscus);
        }
        const float opt_depth = Clamp01(1.12f - 0.88f * thick);
        const float beer_r = std::exp(-1.15f * opt_depth);
        const float beer_g = std::exp(-4.80f * opt_depth);
        const float beer_b = std::exp(-5.40f * opt_depth);
        const float glint = meniscus * meniscus * 0.14f;
        return PackBgra(
            Clamp01(0.42f + 0.56f * beer_r + glint) * rgb_guard,
            Clamp01(0.022f + 0.085f * beer_g + glint * 0.65f) * rgb_guard,
            Clamp01(0.026f + 0.088f * beer_b + glint * 0.70f) * rgb_guard,
            alpha_f);
    }

    // Row 3: Tiles 12-13 incandescent blackbody spark/ember cores with lens diffraction;
    // Tiles 14-15 supersonic Mach shockwave & Rayleigh-Taylor turbulent dust rings.
    if (tx <= 1u) {
        const float core = std::exp(-r2 * 6.5f);
        const float halo = soft_env;
        const float diag_u = (ux + uy) * 0.70710678f;
        const float diag_v = (ux - uy) * 0.70710678f;
        const float star_primary =
            Clamp01(1.0f - (ux * ux * 34.0f + uy * uy * 2.1f)) * halo +
            Clamp01(1.0f - (uy * uy * 34.0f + ux * ux * 2.1f)) * halo;
        const float star_diag =
            (tx == 1u)
                ? (Clamp01(1.0f - (diag_u * diag_u * 48.0f + diag_v * diag_v * 3.0f)) +
                   Clamp01(1.0f - (diag_v * diag_v * 48.0f + diag_u * diag_u * 3.0f))) *
                      halo * 0.55f
                : 0.0f;
        const float alpha_f = Clamp01((halo * halo * 0.64f + core * 0.72f + (star_primary + star_diag) * 0.38f) * rim_mask);
        const float temp = Clamp01(core * 0.92f + star_primary * 0.28f + halo * 0.18f);
        return PackBgra(
            rgb_guard,
            Clamp01(0.24f + 0.74f * std::pow(temp, 1.35f)) * rgb_guard,
            Clamp01(0.04f + 0.82f * std::pow(temp, 2.60f)) * rgb_guard,
            alpha_f);
    }

    const float rad = std::sqrt(r2 + 1e-6f);
    const float dx = ux / rad;
    const float dy = uy / rad;
    const float n0 = RotatedFbm2D(ux * 3.8f + float(tx) * 2.7f, uy * 3.8f - 1.9f, tile_seed, 3);
    const float rt_fingers = RotatedFbm2D(dx * 5.5f + ux * 2.4f, dy * 5.5f + uy * 2.4f, tile_seed + 79u, 3);
    const float delta = rn - (0.64f + 0.10f * (n0 - 0.5f) + 0.06f * (rt_fingers - 0.5f));
    const float shock_ring = Clamp01(1.0f - delta * delta * 26.0f);
    const float inner_wake = Clamp01(1.0f - rn * (0.92f + 0.38f * (1.0f - n0))) * rt_fingers;
    const float alpha_f = Clamp01((shock_ring * (0.68f + 0.32f * rt_fingers) + inner_wake * 0.28f) * rim_mask);
    const float lum_f = Clamp01(0.54f + 0.36f * shock_ring + 0.14f * rt_fingers) * rgb_guard;
    return PackBgra(lum_f, lum_f, lum_f, alpha_f);
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
        // Kelvin-Helmholtz counter-rotating helical vortex dynamics + Rayleigh-Taylor buoyancy
        const float phase = anim_time * 10.8f + float(seq) * 0.91f;
        const float vortex_amp = (1.0f - hc * 0.64f) * rc * 0.26f;
        const float ox = std::cos(phase);
        const float oy = std::sin(phase * 1.17f + 0.65f);
        const float thermal_buoyancy =
            (1.0f - hc) * rc * 0.30f * (0.55f + 0.45f * std::sin(phase * 0.79f));

        const float puff_pos[3] = {
            p[0] + right[0] * ox * vortex_amp + up[0] * oy * vortex_amp,
            p[1] + right[1] * ox * vortex_amp + up[1] * oy * vortex_amp + thermal_buoyancy,
            p[2] + right[2] * ox * vortex_amp + up[2] * oy * vortex_amp,
        };

        const float stretch_t = Clamp01((hc - 0.22f) / 0.78f);
        const float flutter = (1.0f - hc * 0.48f) * 0.28f;
        const float raw_dir[3] = {
            dir[0] + right[0] * ox * flutter + up[0] * oy * flutter,
            dir[1] + right[1] * ox * flutter + up[1] * oy * flutter + 0.22f * (1.0f - hc),
            dir[2] + right[2] * ox * flutter + up[2] * oy * flutter,
        };
        float flow_dir[3]{};
        NormalizeDir3(raw_dir, flow_dir);

        // 1. Planck blackbody pressurized liquid-fuel core (white-gold near nozzle -> amber mid-stream).
        FlamePlumeSprite core;
        core.kind = FlameSpriteKind::CoreDisc;
        core.pos[0] = puff_pos[0];
        core.pos[1] = puff_pos[1];
        core.pos[2] = puff_pos[2];
        core.dir[0] = flow_dir[0];
        core.dir[1] = flow_dir[1];
        core.dir[2] = flow_dir[2];
        core.radius = rc * (0.40f + 0.16f * hc);
        core.stretch = 1.20f + 0.90f * stretch_t;
        core.rotation = 0.0f;
        core.r = 1.0f;
        core.g = 0.72f + 0.24f * hc;
        core.b = 0.16f + 0.46f * hc * hc;
        core.alpha = Clamp01((0.15f + 0.24f * hc) * stream_fade);
        core.tile = 4;
        push(core);

        // 2. Primary flow-stretched diffusion flame sheet (Planck temperature gradient).
        FlamePlumeSprite fire;
        fire.kind = FlameSpriteKind::FireBillboard;
        fire.pos[0] = puff_pos[0];
        fire.pos[1] = puff_pos[1];
        fire.pos[2] = puff_pos[2];
        fire.dir[0] = flow_dir[0];
        fire.dir[1] = flow_dir[1];
        fire.dir[2] = flow_dir[2];
        fire.radius = rc * (1.34f + 0.58f * (1.0f - hc));
        fire.stretch = 1.22f + 1.25f * stretch_t;
        fire.rotation = 0.32f * std::sin(phase * 0.68f + float(seq) * 1.41f) +
                        (1.0f - stretch_t) * (phase * 0.42f + float(seq) * 0.89f);
        fire.r = 1.0f;
        fire.g = 0.38f + 0.50f * hc;
        fire.b = 0.02f + 0.24f * hc * hc;
        fire.alpha = Clamp01((0.38f + 0.26f * hc) * stream_fade);
        fire.tile = 4 + int((seq * 5u + 1u) & 3u);
        push(fire);

        // 3. Secondary Rayleigh-Taylor upward-whipping flame tongue peeling off mid-stream vortex
        if (!perf_mode && hc > 0.24f && hc < 0.86f && (seq % 2u == 1u)) {
            const float whip_dir_raw[3] = {
                flow_dir[0] * 0.65f - right[0] * ox * 0.28f,
                flow_dir[1] * 0.65f + 0.58f,
                flow_dir[2] * 0.65f - right[2] * ox * 0.28f,
            };
            FlamePlumeSprite lick;
            lick.kind = FlameSpriteKind::FireBillboard;
            lick.pos[0] = puff_pos[0] - right[0] * ox * vortex_amp * 0.6f;
            lick.pos[1] = puff_pos[1] + rc * 0.22f;
            lick.pos[2] = puff_pos[2] - right[2] * ox * vortex_amp * 0.6f;
            NormalizeDir3(whip_dir_raw, lick.dir);
            lick.radius = rc * (0.92f + 0.35f * (1.0f - hc));
            lick.stretch = 1.45f + 0.65f * hc;
            lick.rotation = -0.28f * std::cos(phase * 0.75f + float(seq));
            lick.r = 0.98f;
            lick.g = 0.26f + 0.42f * hc;
            lick.b = 0.01f + 0.10f * hc * hc;
            lick.alpha = Clamp01((0.26f + 0.20f * hc) * stream_fade);
            lick.tile = 4 + int((seq * 3u + 2u) & 3u);
            push(lick);
        }

        // 4. Cooled outer hydrocarbon soot/smoke envelope with warm internal fire underlighting (Row 0: tiles 0..3).
        if (hc < 0.78f && (seq & 1u) == 0u) {
            const float underlight = hc * hc;
            FlamePlumeSprite smoke;
            smoke.kind = FlameSpriteKind::SmokeBillboard;
            smoke.pos[0] = puff_pos[0];
            smoke.pos[1] = puff_pos[1] + rc * (0.36f + 0.18f * (1.0f - hc));
            smoke.pos[2] = puff_pos[2];
            smoke.dir[0] = flow_dir[0] * 0.25f;
            smoke.dir[1] = 0.97f;
            smoke.dir[2] = flow_dir[2] * 0.25f;
            smoke.radius = rc * (1.85f + 0.54f * (1.0f - hc));
            smoke.stretch = 1.08f + 0.20f * (1.0f - hc);
            smoke.rotation = -phase * 0.32f + float(seq) * 0.67f;
            smoke.r = 0.12f + 0.24f * underlight;
            smoke.g = 0.10f + 0.10f * underlight;
            smoke.b = 0.09f + 0.04f * underlight;
            smoke.alpha = Clamp01((0.18f + 0.20f * (1.0f - hc)) * stream_fade);
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
        // Rayleigh-Taylor buoyant flickering frequency f ~ 1.5 / sqrt(D)
        const float rt_freq = 6.2f / std::sqrt(std::max(0.22f, r));
        const float flicker = 0.86f + 0.14f * std::sin(anim_time * rt_freq + float(i) * 2.1f);

        // 1. Ground hydrocarbon scorch / soot ring (Row 0: tiles 0..3).
        FlamePlumeSprite sg;
        sg.kind = FlameSpriteKind::SmokeGround;
        sg.pos[0] = positions[i][0];
        sg.pos[1] = positions[i][1];
        sg.pos[2] = positions[i][2];
        sg.radius = r * 1.30f;
        sg.rotation = float(i) * 1.07f;
        sg.r = 0.09f;
        sg.g = 0.08f;
        sg.b = 0.07f;
        sg.alpha = Clamp01(0.38f * stream_fade);
        sg.tile = idx & 3;
        push(sg);

        // 2. Pulsing incandescent liquid napalm pool (Row 1: tiles 4..7).
        FlamePlumeSprite fg;
        fg.kind = FlameSpriteKind::FireGround;
        fg.pos[0] = positions[i][0];
        fg.pos[1] = positions[i][1];
        fg.pos[2] = positions[i][2];
        fg.radius = r * (0.98f + 0.08f * flicker);
        fg.rotation = anim_time * 0.95f - float(i) * 0.83f;
        fg.r = 1.0f;
        fg.g = 0.46f + 0.42f * h;
        fg.b = 0.06f + 0.24f * h * h;
        fg.alpha = Clamp01(0.54f * h * flicker * stream_fade);
        fg.tile = 4 + (idx & 3);
        push(fg);

        // 3. Primary upward-licking stretched diffusion flame tongue (Row 1: tiles 4..7).
        const float sway_x = std::sin(anim_time * rt_freq + float(i) * 1.71f) * 0.19f;
        const float sway_z = std::cos(anim_time * (rt_freq * 0.88f) + float(i) * 2.31f) * 0.19f;
        const float raw_up[3] = {sway_x, 1.0f, sway_z};
        FlamePlumeSprite fb;
        fb.kind = FlameSpriteKind::FireBillboard;
        fb.pos[0] = positions[i][0];
        fb.pos[1] = positions[i][1] + 0.14f + 0.22f * h;
        fb.pos[2] = positions[i][2];
        NormalizeDir3(raw_up, fb.dir);
        fb.radius = r * 0.86f;
        fb.stretch = 1.48f + 0.52f * h * flicker;
        fb.rotation = 0.16f * std::sin(anim_time * 5.6f + float(i) * 1.9f);
        fb.r = 1.0f;
        fb.g = 0.42f + 0.42f * h;
        fb.b = 0.05f + 0.18f * h * h;
        fb.alpha = Clamp01(0.48f * h * stream_fade);
        fb.tile = 4 + ((idx + 1) & 3);
        push(fb);

        // 4. Warm-underlit rising soot plume above cooling pool (Row 0: tiles 0..3).
        if (h < 0.72f) {
            const float warm_soot = h * h;
            FlamePlumeSprite sb;
            sb.kind = FlameSpriteKind::SmokeBillboard;
            sb.pos[0] = positions[i][0] + sway_x * 0.12f;
            sb.pos[1] = positions[i][1] + 0.32f + 0.28f * (1.0f - h);
            sb.pos[2] = positions[i][2] + sway_z * 0.12f;
            sb.dir[0] = sway_x * 0.25f;
            sb.dir[1] = 0.98f;
            sb.dir[2] = sway_z * 0.25f;
            sb.radius = r * 1.18f;
            sb.stretch = 1.15f;
            sb.rotation = anim_time * 0.65f + float(i) * 0.59f;
            sb.r = 0.13f + 0.20f * warm_soot;
            sb.g = 0.11f + 0.08f * warm_soot;
            sb.b = 0.10f + 0.03f * warm_soot;
            sb.alpha = Clamp01(0.26f * (1.0f - h * 0.85f) * stream_fade);
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
