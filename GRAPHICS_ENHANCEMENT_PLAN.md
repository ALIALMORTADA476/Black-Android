# Black Android — Graphics & Rendering Enhancement Plan (`GRAPHICS_ENHANCEMENT_PLAN.md`)

---

## 1. Existing Graphics Capabilities Already in `Black-Android`

Before proposing new features, this audit confirmed the port **already implements** a surprisingly mature Vulkan graphics stack:

| Existing Feature | File & Symbols | Current Capabilities |
|---|---|---|
| **Resolution & Dynamic Scaling** | [`native/main.cpp`](native/main.cpp#L152), [`native/dynamic_resolution.cpp`](native/dynamic_resolution.cpp), [`native/vulkan.cpp`](native/vulkan.cpp#L3325-L3337) | Manual `render_percent` `20%–200%` (default `67%`, ~720p); automatic dynamic resolution (`dynamic_target_fps`, `dynamic_min_percent` `40%`, `dynamic_max_percent` `100%`, `dynamic_hold`). |
| **Texture Filtering (AF)** | [`native/vulkan.cpp`](native/vulkan.cpp#L1384,L2337-L2376), [`src/common/menu.cpp`](src/common/menu.cpp#L97-L98) | Hardware `samplerAnisotropy` (`2x, 4x, 8x, 16x`, or `GAME DEFAULT`) applied in `StageSampler()` to linear mipped non-shadow textures, plus `mipLodBias` clamped to `maxSamplerLodBias`. |
| **Anti-Aliasing** | [`native/vulkan.cpp`](native/vulkan.cpp#L67-L78,L120-L135,L3173), [`src/common/options.h`](src/common/options.h#L24) | `0 = Off`, `1 = FXAA` (`kFilterHlsl::Fxaa`), `2 = SSAA` (2× multi-tap grid in `kBlitHlsl`/`kFilterHlsl`, 4 taps per pixel). |
| **HDR & Tone Mapping** | [`native/vulkan.cpp`](native/vulkan.cpp#L108-L117,L3177), [`src/common/options.h`](src/common/options.h#L25) | `hdr` toggle enabling `Tone()` (Reinhard curve `c/(c+1)` + shadow saturation boost to tame Xbox's overbright framebuffer). |
| **Bloom, Fog & Reload Blur** | [`native/vulkan.cpp`](native/vulkan.cpp#L791-L798,L2814-L2829), [`src/game/hooks.cpp`](src/game/hooks.cpp#L617-L625) | `bloom` toggle via `GlowComposite()`, `fog` toggle via `FillFogConstants()`, `reload_blur` toggle in guest hook. |
| **Screen Filters & Sharpening** | [`native/vulkan.cpp`](native/vulkan.cpp#L142-L204,L3171-L3204) | `screen_filter` `0..8`: CRT Light, CRT Strong, CAS Sharpen (`pssharpen`, fixed strength `0.35`), Vibrant, Noir, Sepia, Night Vision, VHS. |
| **Draw Distance / LOD** | [`src/game/hooks.cpp`](src/game/hooks.cpp#L564-L615) | `draw_distance` `1x, 2x, 4x, 8x` scaling characters, props, ground cover (`GroundCoverFull`), particles (`ParticleFade`), and draw list capacities (`DrawListRoom`). |
| **Unused Translation Stub** | [`src/common/translations.cpp`](src/common/translations.cpp#L73) | `"AMBIENT OCCLUSION"` is already translated across `fra, deu, ita, esp, ind`, though not yet wired into `options.h`, `menu.cpp`, or `vulkan.cpp`. |

---

## 2. Proposed Graphics Enhancements & Technical Design

### 2.1 Improved Texture Filtering & External Texture Replacement
- **Layer**: **Native Host (`native/vulkan.cpp`)** + **Port Common (`src/common/options.h`, `menu.cpp`, `options_menu.cpp`)**.
- **Files & Functions**:
  - `VulkanBackend::StageSampler()` (`native/vulkan.cpp:2337-2376`)
  - `VulkanBackend::BindGuestTexture()` & `UploadTexture()` (`native/vulkan.cpp:2673-2733`)
- **Implementation Approaches**:
  1. **Trilinear Upgrade & Configurable Mip LOD Bias (`Easy`)**: Even with `aniso = 16x`, textures where the game sets `kTexfNearest` or positive mip bias stay soft/blocky. Add a `Texture Sharpening / Mip Bias` setting (e.g. `0.0`, `-0.5`, `-1.0`) subtracted from `sd.mipLodBias` (already clamped to `props_.limits.maxSamplerLodBias`) and optionally promote `MIPMAP_MODE_NEAREST` world textures to `VK_SAMPLER_MIPMAP_MODE_LINEAR`.
  2. **HD Texture Pack Override by Signature (`Moderate`)**: `BindGuestTexture()` already computes `MemSig(d.data, TotalSize(d))` + `palette.hash`. A host texture-pack loader (`Black/textures/<hash>.ktx2|.png`) can substitute higher-resolution RGBA/BC textures on upload without touching `.iso` files.
- **Risks & Performance**: Negative LOD bias (`< -1.0`) can introduce sub-pixel shimmering unless paired with FXAA/SSAA; minimal GPU cost. External HD textures increase RAM/VRAM usage (must tune `ReleaseOldTextures()` `kTexturesKept = 1024` carefully on 4 GB Android devices).
- **Feasibility**: **Easy** (LOD Bias & Trilinear forcing) / **Moderate** (Hash-based HD Texture Packs).

### 2.2 Enhanced Post-Processing & Visual Effects (Color Grading, ACES Tone Mapping, Adjustable Sharpening & Bloom Strength)
- **Layer**: **Native Host (`native/vulkan.cpp`)** + **Port Common (`src/common/options.h`, `game_info.cpp`, `menu.cpp`, `translations.cpp`)**.
- **Files & Functions**:
  - `kFilterHlsl` & `VulkanBackend::DrawFrame()` (`native/vulkan.cpp:85-205, 3155-3245`)
  - `GlowComposite()` / `Draw()` (`native/vulkan.cpp:2814-2905`)
  - `options.h`, `game_info.cpp`, `menu.cpp`, `options_menu.cpp` (adds rows into **GRAPHICS** automatically in both ImGui & Flash Options!)
- **Implementation Approaches**:
  1. **Tone Mapping Mode (`Off / Reinhard / ACES Filmic`)**: Extend `hdr` from `bool` to `uint32_t tone_map` (maintaining backward compatibility in `black.toml` load) adding an ACES Filmic approximation curve alongside existing `Tone()`.
  2. **Standalone CAS Sharpening Toggle / Strength (`0%–100%`)**: Currently `pssharpen` only runs when `screen_filter == 3`, forcing players to choose between FXAA/CRT/Vibrant and Sharpen! Fold CAS into `Frame()` in `kFilterHlsl` (or add a dedicated sharpen pass/parameter in `cbuffer B`) so CAS Sharpening stacks cleanly with FXAA and HDR.
  3. **Adjustable Contrast / Saturation / Gamma Exposure & Vignette**: Add lightweight uniform sliders in `kFilterHlsl`.
  4. **Bloom Intensity Scaling**: In `Draw()`, when `GlowComposite(s, stages)` returns `true`, scale `DrawConstants::tfactor` or `ShaderHooks::Output()` / blend constant color instead of only on/off skipping, allowing **Off / Low / Default / High** bloom.
- **Risks & Performance**: **Very Low** risk (entirely host-side HLSL & settings); < 0.3 ms GPU time per frame on mobile Vulkan.
- **Feasibility**: **Easy**.

### 2.3 Improved Lighting, Specular & Soft Shadows via `XboxXBE-Recomp` `ShaderHooks`
- **Layer**: **Native Host (`native/vulkan.cpp`)** interfacing with **SDK Shader Generator (`XboxXBE-Recomp/runtime/gpu/shaders.h`)**.
- **Key Architectural Discovery**: `XboxXBE-Recomp/runtime/gpu/shaders.h` (lines 64–79, 93) defines `gpu::ShaderHooks` (`gpu::g_shader_hooks`) with overridable HLSL injection hooks:
  - `ShaderHooks::Constants()` → exposes `DrawConstants::ext[4]` in `b1`
  - `ShaderHooks::Shadow()` → replaces default hardware depth shadow sampling function (`shadow(Texture2D<float> t, SamplerComparisonState s, ...)`)
  - `ShaderHooks::Fetched(const TexInfo&, int stage)` → transforms fetched stage samples
  - `ShaderHooks::Output()` → transforms final pixel shader output `outc` before target write
  - `native/vulkan.cpp` does **not** currently set `gpu::g_shader_hooks`!
- **Implementation Approaches**:
  1. **PCF Soft Shadows (`ShaderHooks::Shadow()`)**: Override `Shadow()` with a 4-tap or 9-tap Poisson-disk Percentage Closer Filtering kernel using `px` (texel size) passed by `shaders.h`! Controllable via a new **SHADOW QUALITY** (`Original / Soft 4-Tap / Soft 9-Tap`) option (note `StageSampler()` line 2370 configures `VK_COMPARE_OP_*` on shadow textures).
  2. **Specular / Rim Contrast & Overbright Tuning (`ShaderHooks::Output()`)**: Populate `DrawConstants::ext[4]` in `VulkanBackend::Draw()` for per-draw dynamic lighting intensity / specular response without modifying fixed-function vertex math (`FillLightConstants()` lines 926–933).
- **Compatibility Risk & Pipeline Cache**: Modifying generated HLSL changes `PixelKey` / shader source hash, which invalidates old `.spv` caches (`kCacheVersion` in `vk_shader.cpp` should be bumped from `2` to `3` cleanly!). Changing shadow taps costs extra texture fetches on shadowed surfaces.
- **Feasibility**: **Moderate** (clean, first-class SDK hook API; requires testing on both Adreno and Mali drivers to ensure `SamplerComparisonState` PCF behavior is glitch-free).

### 2.4 Screen-Space Ambient Occlusion (SSAO)
- **Layer**: **Native Host (`native/vulkan.cpp`)** + **Port Common (`options.h`, `menu.cpp`)**.
- **Context Clue**: `src/common/translations.cpp` line 73 already includes `"AMBIENT OCCLUSION"`, showing the port author previously considered adding it.
- **Confirmed Prerequisites Present**: `VulkanBackend` already creates and binds the scene depth target `draw_ds_` (`VK_FORMAT_D24_UNORM_S8_UINT` or `VK_FORMAT_D32_SFLOAT_S8_UINT` with `VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT` in `CreateDevice()` line 1421) and already has `DepthCopy()` + `kDepthHlsl` to sample depth textures in Vulkan shaders!
- **Technical Challenge (Why it's not trivial)**:
  - Black renders its 3D scene, then glow/bloom passes, then 2D HUD & Flash UI into the render target (`Target*`) within the same command stream. Injecting a screen-space depth occlusion pass **after** opaque 3D geometry (or before 2D/HUD draws) requires reliably detecting the boundary between the 3D scene pass and the glow/2D/HUD overlay in `VulkanBackend::Draw()` (or hooking a guest render-phase marker via `config/game.toml` right before `DrawOverlay` / HUD draw list).
  - Running SSAO at `DrawFrame()` (after HUD is already drawn) would wrongly darken HUD elements & Flash menus!
- **Implementation Approach**:
  1. Hook a tiny guest render-stage notification (or detect viewport/Z-disable/pre-2D transition) marking "3D world finished, HUD starting".
  2. Run a half-resolution compute or fullscreen graphics pass sampling `ds->img` depth (plus optional bilateral blur) and multiply AO onto the scene `Target` before HUD/2D rendering.
- **Performance Cost**: **High** on mobile GPUs (bandwidth-heavy depth reconstruction + 8–16 taps + blur; needs `Off / Low (Half-Res)` toggle and careful interaction with `dynamic_resolution.cpp`).
- **Feasibility**: **Moderate–Difficult** (clean depth access exists, but mid-frame 3D/HUD boundary detection & mobile bandwidth tuning require care).

### 2.5 True Dynamic Light / Shadowmap Resolution Upscaling or Per-Pixel Lighting Overhaul
- **Layer**: **Guest Engine (`default.xbe`) + `d3d8.cpp` / `vulkan.cpp`**.
- **Details**: Black uses pre-baked lightmaps + NV2A register combiners (`D3DPIXELSHADERDEF` compiled via `shaders.cpp`) + stencil/projected shadows allocated in guest render targets. Upgrading shadowmap resolutions (`Surface` dimensions inside guest RAM) or injecting new point-light/normal-map passes into compiled maps is **Difficult / Unconfirmed** due to fixed guest surface headers (`FindTarget()` matches `gw`/`gh`) and asset-baked lighting.
- **Feasibility**: **Difficult** (keep out of early milestones).

---

## 3. Recommended New Settings for the `GRAPHICS` Menu

All additions below plug directly into `game::Options` (`src/common/options.h`), `app::kGame.options` (`src/common/game_info.cpp`), and `kGraphicsRows[]` (`src/common/menu.cpp`), which automatically exposes them in **both** the ImGui Touch-Gear panel and the in-game Flash **Options → Graphics** menu (`options_menu.cpp`):

| Setting Key (`black.toml`) | Options / Range | Layer & Feasibility | GPU Cost |
|---|---|---|---|
| `tone_mapping` | `OFF`, `REINHARD`, `ACES FILMIC` | Host `kFilterHlsl` — **Easy** | Negligible |
| `sharpen_amount` | `0%` – `100%` (works with FXAA & HDR!) | Host `kFilterHlsl` — **Easy** | Very Low |
| `bloom_intensity` | `OFF`, `50%`, `100%`, `150%` | Host `GlowComposite` / `ShaderHooks` — **Easy** | None extra |
| `texture_lod_bias` | `DEFAULT`, `-0.5`, `-1.0` | Host `StageSampler` — **Easy** | Very Low |
| `soft_shadows` | `OFF`, `LOW (4-TAP)`, `HIGH (9-TAP)` | Host `ShaderHooks::Shadow` — **Moderate** | Low–Moderate |
| `ambient_occlusion` | `OFF`, `LOW (HALF-RES)` | Host `vulkan.cpp` + 1 phase hook — **Mod–Diff** | High |

