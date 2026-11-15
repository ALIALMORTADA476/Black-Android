#pragma once

#include <cstddef>
#include <cstdint>

namespace game {
namespace ballistics {

enum class FlameSpriteKind : uint8_t {
    CoreDisc = 0,       // Soft untextured additive inner hot core glow
    FireBillboard = 1,  // Textured camera-facing organic flame (Row 1: tiles 4..7)
    SmokeBillboard = 2, // Textured camera-facing hydrocarbon soot/smoke (Row 0: tiles 0..3)
    FireGround = 3,     // Textured ground-hugging flame pool (Row 1: tiles 4..7)
    SmokeGround = 4,    // Textured ground-hugging scorch/smoke (Row 0: tiles 0..3)
};

struct FlamePlumeSprite {
    FlameSpriteKind kind = FlameSpriteKind::FireBillboard;
    float pos[3] = {0.0f, 0.0f, 0.0f};
    float dir[3] = {0.0f, 1.0f, 0.0f}; // World-space elongation tangent (flow / rising buoyancy)
    float radius = 0.1f;
    float stretch = 1.0f;               // Axial length multiplier along dir (>= 1.0)
    float rotation = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float alpha = 1.0f;
    int tile = 4; // Atlas tile [0..15]: 0..3 smoke, 4..7 fire, 8..11 blood, 12..15 sparks/rings
};

// Generates one texel of the 4x4 procedural VFX atlas (0xAARRGGBB).
// Keeps alpha and RGB strictly faded to zero before tile margins (r >= 0.85
// on smoke/fire) so quad edges and bilinear tile bleeding cannot create squares.
uint32_t SampleVfxAtlasTexelBgra(uint32_t tx, uint32_t ty, float u_tile, float v_tile);

// Expands simulated jet segments (ordered oldest/far first, newest/muzzle last)
// into overlapping volumetric fire/smoke puffs with organic curl and varied
// rotation, avoiding straight-line ribbon geometry and disconnected jumps.
size_t BuildFlamethrowerJetSprites(const float (*positions)[3],
                                   const float* radii,
                                   const float* heats,
                                   size_t count,
                                   float stream_fade,
                                   float anim_time,
                                   bool perf_mode,
                                   FlamePlumeSprite* out,
                                   size_t max_out);

// Expands settled ground napalm pools into layered soot/flame ground tiles and
// rising fire tongues using Row 0 smoke (0..3) and Row 1 fire (4..7) only.
size_t BuildFlamethrowerPoolSprites(const float (*positions)[3],
                                    const float* radii,
                                    const float* heats,
                                    size_t count,
                                    float stream_fade,
                                    float anim_time,
                                    FlamePlumeSprite* out,
                                    size_t max_out);

}  // namespace ballistics
}  // namespace game
