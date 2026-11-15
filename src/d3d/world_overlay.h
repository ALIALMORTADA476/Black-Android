#pragma once

#include <cstdint>

namespace d3d {

struct WorldVertex {
    float x, y, z;
    uint32_t bgra;  // D3DCOLOR: (A << 24) | (R << 16) | (G << 8) | B
};

inline uint32_t PackColorBgra(float r, float g, float b, float a) {
    auto clamp_u8 = [](float v) -> uint32_t {
        int c = int(v * 255.0f + 0.5f);
        return uint32_t(c < 0 ? 0 : (c > 255 ? 255 : c));
    };
    return (clamp_u8(a) << 24) | (clamp_u8(r) << 16) | (clamp_u8(g) << 8) | clamp_u8(b);
}

struct TexturedWorldVertex {
    float x, y, z;
    uint32_t bgra;  // D3DCOLOR: (A << 24) | (R << 16) | (G << 8) | B
    float u, v;
};

struct ViewmodelVertex {
    float x, y, z;
    float nx, ny, nz;
    uint32_t bgra;  // D3DCOLOR: (A << 24) | (R << 16) | (G << 8) | B
};

// Submits a camera-transformed 3D triangle list directly into the active scene pass
// with hardware depth testing (ZENABLE=1, ZWRITE=0, ZFUNC=LESSEQUAL, CULL=NONE).
// Must be called on the guest thread during the 3D scene pass (e.g. after kAlphaDrawList).
void DrawWorldOverlay(const WorldVertex* verts, uint32_t vertex_count, const float view[16],
                      const float proj[16], bool additive);

// Submits a textured 3D triangle list (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)
// bound to the procedural 4x4 VFX atlas (FBM smoke, sharp flames, organic blood splatters).
void DrawTexturedWorldOverlay(const TexturedWorldVertex* verts, uint32_t vertex_count,
                              const float view[16], const float proj[16], bool additive);

// Screen-space refraction lens used for explosion heat haze / air-blast bending.
// Vertices are supplied already in clip space (x,y in [-1,1], z is ignored) and
// their (u,v) pick up the frame that was on screen when the call was made: the
// current render target is captured once per frame and the warped grid is drawn
// back over the scene, so the world behind the blast visibly bends. Alpha comes
// from the vertex diffuse colour (rgb is modulated with the captured pixels).
// Call it before submitting the effect that must not be distorted itself (e.g.
// the fireball billboards). Returns false when no usable snapshot can be taken,
// so callers can fall back to ordinary overlays.
bool DrawScreenWarp(const TexturedWorldVertex* verts, uint32_t vertex_count);

// Draws a static lit metallic 3D viewmodel mesh (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE)
// in foreground viewmodel depth slice [0.0, 0.08] with tactical key/fill lights + nozzle flame point light.
void DrawViewmodelMesh(const ViewmodelVertex* verts, uint32_t vertex_count,
                       const uint16_t* indices, uint32_t index_count,
                       const float world[16], const float view[16], const float proj[16],
                       const float flame_pos_world[3], float flame_intensity);

}  // namespace d3d
