# BLACK Enhanced Edition — Graphics Pipeline & Renderer Integration Report

**Milestone**: Stage 4 — In-Game Tactical Mod Menu & Functional Feature Integration  
**Application ID**: `com.racas.black.enhanced`  
**Version**: `v1.2.0-enhanced` (Build `25`)  
**Graphics API**: Vulkan 1.1+ (ARM Mali-G610 MC4 / Adreno 6xx/7xx)  
**Repository**: `E:\ai\black`  

---

## 1. Vulkan Rendering Pipeline Architecture

The graphics subsystem of BLACK Android operates by translating the original Xbox Direct3D 8 fixed-function and programmable shader instructions into SPIR-V shaders executed via Vulkan (`native/vulkan.cpp`). 

```
+-----------------------------------------------------------------------------+
|                          ORIGINAL XBOX DIRECT3D 8                            |
|             (Transpiled Guest Vertex / Pixel Shaders & Fixed Pipeline)       |
+-----------------------------------------------------------------------------+
                                      |
                                      v
+-----------------------------------------------------------------------------+
|                           SPIR-V TRANSLATION LAYER                          |
|             (glslang Compiler & HLSL Shader Emulation in vk_shader.cpp)      |
+-----------------------------------------------------------------------------+
                                      |
                                      v
+-----------------------------------------------------------------------------+
|                         VULKAN BACKEND (native/vulkan.cpp)                  |
|                                                                             |
|  [Offscreen Render Target] <---- Scaled by render_percent / dynres          |
|            |                                                                |
|            +---> [Anisotropic Filtering Samplers] <---- aniso (0x..16x)     |
|            |                                                                |
|            +---> [Overbright Glow / Bloom Composite] <---- bloom & intensity|
|            |                                                                |
|            +---> [Anti-Aliasing Pass] <---- anti_aliasing (FXAA / SSAA 2x)  |
|            |                                                                |
|            +---> [Tone Mapping / HDR] <---- tone_mapping (D3D/Reinhard/ACES)|
|            |                                                                |
|            +---> [Screen Filters] <---- screen_filter (CRT/Noir/Vibrant...) |
|            |                                                                |
|            v                                                                |
|  [Vulkan Swapchain / Present Surface]                                       |
|            ^                                                                |
|            |                                                                |
|  [Dear ImGui Tactical Ops Console] <---- RenderUi() Overlay Frame           |
+-----------------------------------------------------------------------------+
```

---

## 2. Core Graphics Features & Engine Wiring

### 2.1 Resolution Scaling & Dynamic Pacing
- **Static Scaling (`render_percent`)**: 
  - Controlled by `app::Get().render_percent`.
  - Sizes the primary offscreen color attachment in `vulkan.cpp`.
  - Supports steps from 20% to 200% (e.g. 384x216 up to 3840x2160 on a 1080p display).
- **Dynamic Resolution Scaling (`dynamic_resolution`)**:
  - Controlled by `game::Opt().dynamic_resolution` and monitored via `dynres::Percent()`.
  - When active, the static render percent slider in the UI displays the live dynamic scaling factor in real time.
  - Dynamically lowers resolution during heavy explosions or intense firefights to maintain the target FPS (30, 45, 60, 90, or 120 FPS), raising resolution back to `dynamic_max_percent` after `dynamic_hold` seconds of stable frame delivery.

### 2.2 Texture Filtering & Sampler Optimization
- **Anisotropic Filtering (`aniso`)**:
  - Traced to `native/vulkan.cpp` (lines 2348–2353):
    ```cpp
    uint32_t aniso = app::Get().aniso;
    if (aniso && lin(min) && mipped && !stages.info[i].shadow) an = aniso;
    if (an > 1 && feats_.samplerAnisotropy) {
        sd.anisotropyEnable = VK_TRUE;
        sd.maxAnisotropy = std::min(float(an), props_.limits.maxSamplerAnisotropy);
    }
    ```
  - Eliminates blur on oblique road surfaces, concrete walls, and long corridors (notably in Trench and City Streets missions).
- **Texture LOD Bias (`texture_lod_bias`)**:
  - Injects negative mipmap LOD biases (`0.0`, `-0.5`, `-1.0`) into Vulkan sampler descriptors, enhancing fine specular details on firearm receivers and foliage.
- **Trilinear Filtering**:
  - Smooths transitions between adjacent mipmap levels, preventing noticeable mip-banding artifacts.

### 2.3 Lighting, Glow & Post-Processing
- **Overbright Bloom (`bloom`)**:
  - Traced to `native/vulkan.cpp` (line 2852):
    ```cpp
    if (!game::Opt().bloom && GlowComposite(s, stages)) return;
    ```
  - Controls the multi-pass high-intensity glow pass that gives BLACK its signature cinematic aesthetic.
- **Bloom Intensity Scaling (`graphics.bloom_intensity`)**:
  - Scaled from 50% to 200%, allowing players on OLED displays to tone down or amplify the bloom glare.
- **Anti-Aliasing (`anti_aliasing`)**:
  - Traced to `native/vulkan.cpp` (line 3173):
    - `0`: Off
    - `1`: Fast Approximate Anti-Aliasing (FXAA post-process pass)
    - `2`: Super-Sampled Anti-Aliasing (SSAA 2x)
- **HDR & Tone Mapping (`graphics.tone_mapping`)**:
  - `0`: Original Direct3D 8 clamp
  - `1`: Reinhard HDR tone mapping (retains highlight details in bright skyboxes)
  - `2`: ACES Filmic curve (cinematic contrast with high-end color grading)
- **Post-Process Screen Filters (`screen_filter`)**:
  - 9 integrated full-screen shaders: `OFF`, `CRT LIGHT`, `CRT STRONG`, `SHARPEN`, `VIBRANT`, `NOIR`, `SEPIA`, `NIGHT VISION`, and `VHS`.

### 2.4 Pacing & Refresh Rate Synchronization
- **Frame Rate Pacing (`fps_cap` & `PaceFrame()`)**:
  - `30`, `60`, `75`, `90`, `120`, `144`, `165`, `240`, `360`, `REFRESH RATE`, or `UNLIMITED`.
  - When set to `REFRESH RATE`, paces frames with a safety margin (`rate - 3 FPS`) so variable refresh displays stay within their VRR window without incurring vsync input latency.

---

## 3. Presets Performance Profiles

| Parameter | Original Preset | Enhanced Preset | Performance Preset |
|---|---|---|---|
| **Render Resolution** | 67% (~720p) | 100% (Native 1080p+) | Dynamic (40%–100%) |
| **Max Frame Rate** | 30 FPS Lock | Unlimited / Refresh | 60 FPS Target |
| **Vsync** | Enabled (FIFO) | Disabled (Low Latency) | Disabled |
| **Anisotropic Filtering** | Game Default (0x) | 8x | 2x |
| **Anti-Aliasing** | OFF | FXAA | OFF |
| **Draw Distance** | 1x (Original LOD) | 2x | 1x |
| **Bloom Glow** | Enabled (100%) | Enabled (100%) | Disabled |
| **Tone Mapping** | Original D3D | ACES Filmic | Original D3D |
| **Screen Filter** | OFF | OFF | OFF |
| **Target Hardware** | Battery Saver | Flagship SoC (Dimensity/Snapdragon) | Mid-range SoCs / Sustained Play |

