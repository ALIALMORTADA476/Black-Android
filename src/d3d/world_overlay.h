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

struct ViewmodelTexturedVertex {
    float x, y, z;
    float nx, ny, nz;
    uint32_t bgra;  // D3DCOLOR: (A << 24) | (R << 16) | (G << 8) | B
    float u, v;
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

struct ViewmodelTexturedBatch {
    const uint16_t* indices = nullptr;
    uint32_t index_count = 0;
    uint64_t texture_cache_key = 0;
    const uint32_t* swizzled_bgra_pixels = nullptr;
    uint32_t tex_width = 0;
    uint32_t tex_height = 0;
};

// Draws a textured PBR-shaded 3D viewmodel mesh (FVF 0x152: XYZ|NORMAL|DIFFUSE|TEX1)
// using a cached Morton-swizzled A8R8G8B8 texture, explicitly resetting
// all fixed-function texture stages [1..3] and material sources to prevent scene blowout.
void DrawViewmodelTexturedMesh(const ViewmodelTexturedVertex* verts, uint32_t vertex_count,
                               const uint16_t* indices, uint32_t index_count,
                               uint64_t texture_cache_key, const uint32_t* swizzled_bgra_pixels,
                               uint32_t tex_width, uint32_t tex_height,
                               const float world[16], const float view[16], const float proj[16],
                               const float flame_pos_world[3], float flame_intensity);

// Uploads the shared vertex array once into the fence-synchronized ring buffer and draws
// multiple material index ranges without same-frame buffer overwrite hazards.
void DrawViewmodelTexturedBatches(const ViewmodelTexturedVertex* verts, uint32_t vertex_count,
                                  const ViewmodelTexturedBatch* batches, uint32_t batch_count,
                                  const float world[16], const float view[16], const float proj[16],
                                  const float flame_pos_world[3], float flame_intensity);

// First-person gun-list draw capture/filter mode (0 off, 1 probe log, 2 arms-only filter).
void SetGunListMode(int mode);
int GunListMode();

}  // namespace d3d
