#include "common/mod_schema.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

#include "common/presets.h"

namespace modsettings {

namespace {

constexpr uint32_t kDefaultRenderPercent = 67;  // ~720p mobile baseline from native/main.cpp

const char* const kDisplayModes[] = {"fullscreen", "borderless", "windowed"};

bool ContainsUint(const std::vector<uint32_t>& list, uint32_t v) {
    return std::find(list.begin(), list.end(), v) != list.end();
}

bool ContainsString(const std::vector<std::string>& list, std::string_view v) {
    for (const auto& item : list)
        if (item == v) return true;
    return false;
}

uint32_t NearestUint(const std::vector<uint32_t>& list, uint32_t v) {
    if (list.empty()) return v;
    uint32_t best = list.front();
    uint64_t best_diff = UINT64_MAX;
    for (uint32_t candidate : list) {
        uint64_t diff = candidate > v ? uint64_t(candidate) - v : uint64_t(v) - candidate;
        if (diff < best_diff) {
            best_diff = diff;
            best = candidate;
        }
    }
    return best;
}

uint32_t ClampAndStepUint(uint32_t v, const NumericBounds& b, bool& adjusted) {
    adjusted = false;
    if (!b.allowed_uints.empty()) {
        if (!ContainsUint(b.allowed_uints, v)) {
            adjusted = true;
            return NearestUint(b.allowed_uints, v);
        }
        return v;
    }
    uint32_t orig = v;
    if (b.uint_max > b.uint_min) {
        v = std::clamp(v, b.uint_min, b.uint_max);
        if (b.uint_step > 1) {
            uint32_t rem = (v - b.uint_min) % b.uint_step;
            if (rem != 0) v -= rem;
        }
    }
    adjusted = (v != orig);
    return v;
}

bool ReadValidatedBool(const toml::node* n, bool fallback, uint32_t& sanitized) {
    if (!n) return fallback;
    if (n->is_boolean()) return n->as_boolean()->get();
    sanitized++;
    return fallback;
}

uint32_t ReadValidatedUint(const toml::node* n, uint32_t fallback, const NumericBounds& b, uint32_t& sanitized) {
    if (!n) return fallback;
    if (!n->is_integer()) {
        sanitized++;
        return fallback;
    }
    int64_t raw = n->as_integer()->get();
    if (raw < 0) {
        sanitized++;
        uint32_t floor_val = !b.allowed_uints.empty() ? b.allowed_uints.front() : b.uint_min;
        return floor_val;
    }
    if (raw > int64_t(UINT32_MAX)) {
        sanitized++;
        uint32_t ceil_val = !b.allowed_uints.empty() ? b.allowed_uints.back() : b.uint_max;
        return ceil_val;
    }
    bool adj = false;
    uint32_t out = ClampAndStepUint(uint32_t(raw), b, adj);
    if (adj) sanitized++;
    return out;
}

float ReadValidatedFloat(const toml::node* n, float fallback, const NumericBounds& b, uint32_t& sanitized) {
    if (!n) return fallback;
    std::optional<double> v;
    if (n->is_floating_point()) v = n->as_floating_point()->get();
    else if (n->is_integer()) v = double(n->as_integer()->get());
    if (!v.has_value() || !std::isfinite(*v)) {
        sanitized++;
        return fallback;
    }
    float f = float(*v);
    if (b.float_max > b.float_min) {
        float clamped = std::clamp(f, b.float_min, b.float_max);
        if (std::fabs(clamped - f) > 1e-6f) {
            sanitized++;
            f = clamped;
        }
    }
    return f;
}

std::string ReadValidatedString(const toml::node* n, std::string fallback, const NumericBounds& b, uint32_t& sanitized) {
    if (!n) return fallback;
    if (!n->is_string()) {
        sanitized++;
        return fallback;
    }
    std::string s = n->as_string()->get();
    if (!b.allowed_strings.empty() && !ContainsString(b.allowed_strings, s)) {
        sanitized++;
        return fallback;
    }
    return s;
}

bool IsKnownRootKey(std::string_view key) {
    static const std::set<std::string_view> kRootKeys = {
        "schema_version", "defaults", "display_mode", "width", "height", "join_ip", "keys",
        "presets", "graphics", "combat", "characters", "audio", "arabic", "gameplay", "interface", "packages",
        "physics_lab"};
    return kRootKeys.find(key) != kRootKeys.end();
}

}  // namespace

SettingsRegistry::SettingsRegistry() { RegisterAllDefinitions(); }

SettingsRegistry& Registry() {
    static SettingsRegistry instance;
    return instance;
}

void SettingsRegistry::RegisterAllDefinitions() {
    definitions_.clear();
    definitions_.reserve(72);

    auto add = [&](SettingId id, const char* key, const char* section, Category cat, SettingType type,
                   FeatureStatus status, ApplyScope scope, const char* label, const char* desc,
                   NumericBounds bounds = {}) {
        definitions_.push_back(ModFeatureMeta{id, key, section, cat, type, status, scope, label, desc, std::move(bounds)});
    };

    // --- Graphics (Implemented host/recomp) ---
    add(SettingId::RenderPercent, "render_percent", nullptr, Category::Graphics, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "RENDER RESOLUTION",
        "Internal rendering resolution percentage relative to device screen height.",
        NumericBounds{20, 200, 1, 0.f, 0.f, 0.1f, {}, {}});

    add(SettingId::DynamicResolution, "dynamic_resolution", nullptr, Category::Graphics, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "DYNAMIC RESOLUTION",
        "Automatically scales internal render resolution to maintain target FPS.");

    add(SettingId::DynamicTargetFps, "dynamic_target_fps", nullptr, Category::Graphics, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "TARGET FRAME RATE",
        "Target FPS threshold used by the dynamic resolution scaler.",
        NumericBounds{30, 120, 1, 0.f, 0.f, 0.1f, {30, 45, 60, 90, 120}, {}});

    add(SettingId::DynamicMinPercent, "dynamic_min_percent", nullptr, Category::Graphics, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "MIN RESOLUTION",
        "Lower percentage bound for automatic dynamic resolution scaling.",
        NumericBounds{20, 200, 5, 0.f, 0.f, 0.1f, {}, {}});

    add(SettingId::DynamicMaxPercent, "dynamic_max_percent", nullptr, Category::Graphics, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "MAX RESOLUTION",
        "Upper percentage bound for automatic dynamic resolution scaling.",
        NumericBounds{20, 200, 5, 0.f, 0.f, 0.1f, {}, {}});

    add(SettingId::DynamicHold, "dynamic_hold", nullptr, Category::Graphics, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "HOLD BEFORE RAISING",
        "Seconds of sustained target FPS required before dynamic resolution steps upward.",
        NumericBounds{1, 30, 1, 0.f, 0.f, 0.1f, {}, {}});

    add(SettingId::AnisotropicFiltering, "anisotropic_filtering", nullptr, Category::Graphics, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "ANISOTROPIC FILTERING",
        "Hardware anisotropic texture filtering level for oblique surfaces.",
        NumericBounds{0, 16, 1, 0.f, 0.f, 0.1f, {0, 2, 4, 8, 16}, {}});

    add(SettingId::AntiAliasing, "anti_aliasing", nullptr, Category::Graphics, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "ANTI-ALIASING",
        "Edge smoothing mode: Off (0), FXAA (1), or 2x Supersampling AA (2).",
        NumericBounds{0, 2, 1, 0.f, 0.f, 0.1f, {0, 1, 2}, {}});

    add(SettingId::ScreenFilter, "screen_filter", nullptr, Category::Graphics, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "SCREEN FILTER",
        "Post-process image filter preset (CRT, Sharpen, Vibrant, Noir, Sepia, Night Vision, VHS).",
        NumericBounds{0, 8, 1, 0.f, 0.f, 0.1f, {}, {}});

    add(SettingId::Bloom, "bloom", nullptr, Category::Graphics, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "BLOOM",
        "Additive overbright glow composite over muzzle flashes, lights, and explosions.");

    add(SettingId::Fog, "fog", nullptr, Category::Graphics, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "FOG",
        "Atmospheric distance fog rendered via fixed-function shader constants.");

    add(SettingId::Hdr, "hdr", nullptr, Category::Graphics, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "HDR TONE MAP",
        "Reinhard tone mapping pass recovering highlight color burnt out by Xbox overbright.");

    add(SettingId::ReloadBlur, "reload_blur", nullptr, Category::Graphics, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "RELOAD BLUR",
        "Depth-of-field background blur triggered while reloading weapons.");

    add(SettingId::DrawDistance, "draw_distance", nullptr, Category::Graphics, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "DRAW DISTANCE",
        "LOD, prop, ground cover, and particle visibility distance multiplier.",
        NumericBounds{1, 8, 1, 0.f, 0.f, 0.1f, {1, 2, 4, 8}, {}});

    add(SettingId::Vsync, "vsync", nullptr, Category::Graphics, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "VERTICAL SYNC",
        "Synchronize swapchain presentation with display refresh rate.");

    add(SettingId::FpsCap, "fps_cap", nullptr, Category::Graphics, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "MAX FRAME RATE",
        "Frame rate cap (0 = Unlimited, 1 = Display Refresh, or fixed target FPS).",
        NumericBounds{0, 360, 1, 0.f, 0.f, 0.1f, {0, 1, 30, 60, 75, 90, 120, 144, 165, 240, 360}, {}});

    // --- Graphics (Enhanced Edition) ---
    add(SettingId::GraphicsToneMapping, "tone_mapping", "graphics", Category::Graphics, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "TONE MAPPING CURVE",
        "Select Off, Reinhard HDR, or ACES Filmic post-process curve.",
        NumericBounds{0, 2, 1, 0.f, 0.f, 0.1f, {0, 1, 2}, {}});

    add(SettingId::GraphicsCasSharpenEnabled, "cas_sharpen_enabled", "graphics", Category::Graphics, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "CAS SHARPENING",
        "FidelityFX Contrast Adaptive Sharpening stackable with FXAA, HDR, and Cinematic Grading.");

    add(SettingId::GraphicsCasSharpenStrength, "cas_sharpen_strength", "graphics", Category::Graphics, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "CAS SHARPEN STRENGTH",
        "Adjusts Contrast Adaptive Sharpening intensity from 0% to 100%.",
        NumericBounds{0, 100, 5, 0.f, 0.f, 0.1f, {}, {}});

    add(SettingId::GraphicsBloomIntensity, "bloom_intensity", "graphics", Category::Graphics, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "BLOOM INTENSITY",
        "Scales additive glow composite and highlight bloom multiplier from 0% to 200%.",
        NumericBounds{0, 200, 10, 0.f, 0.f, 0.1f, {}, {}});

    add(SettingId::GraphicsTextureLodBias, "texture_lod_bias", "graphics", Category::Graphics, SettingType::Float,
        FeatureStatus::Implemented, ApplyScope::Immediate, "TEXTURE MIP LOD BIAS",
        "Negative sampler LOD bias (0.0, -0.5, -1.0) for crisper surface textures.",
        NumericBounds{0, 0, 1, -1.0f, 0.0f, 0.5f, {}, {}});

    add(SettingId::GraphicsTrilinearFiltering, "trilinear_filtering", "graphics", Category::Graphics, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "TRILINEAR FILTERING",
        "Upgrades world texture mipmap selection to smooth linear transitions.");

    add(SettingId::GraphicsCinematicGrade, "cinematic_grade", "graphics", Category::Graphics, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "CINEMATIC COLOR GRADE",
        "Selects Off, Blockbuster Teal & Orange, Gritty Tactical, or Warzone HDR grading.",
        NumericBounds{0, 3, 1, 0.f, 0.f, 0.1f, {0, 1, 2, 3}, {}});

    add(SettingId::GraphicsLensVignette, "lens_vignette", "graphics", Category::Graphics, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "CINEMA LENS VIGNETTE",
        "Optical dark corner lens framing from 0% (Off) to 100%.",
        NumericBounds{0, 100, 5, 0.f, 0.f, 0.1f, {}, {}});

    add(SettingId::GraphicsFilmGrain, "film_grain", "graphics", Category::Graphics, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "35MM FILM GRAIN",
        "Subtle temporal 35mm cinema grain to remove sky/smoke color banding (0..100%).",
        NumericBounds{0, 100, 5, 0.f, 0.f, 0.1f, {}, {}});

    add(SettingId::GraphicsBlastLensPulse, "blast_lens_pulse", "graphics", Category::Graphics, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "BLAST SHOCKWAVE PULSE",
        "Triggers subtle optical shockwave chromatic aberration on nearby explosions.");

    add(SettingId::GraphicsSoftShadows, "soft_shadows", "graphics", Category::Graphics, SettingType::Uint,
        FeatureStatus::Planned, ApplyScope::ShaderRebuild, "PCF SOFT SHADOWS",
        "[Planned Task 5] Overrides ShaderHooks::Shadow() with 4-tap or 9-tap Poisson PCF filtering.",
        NumericBounds{0, 2, 1, 0.f, 0.f, 0.1f, {0, 1, 2}, {}});

    add(SettingId::GraphicsAmbientOcclusion, "ambient_occlusion", "graphics", Category::Graphics, SettingType::Uint,
        FeatureStatus::Planned, ApplyScope::Immediate, "AMBIENT OCCLUSION",
        "[Planned Milestone 4] Half-resolution screen-space depth occlusion before HUD draw.",
        NumericBounds{0, 1, 1, 0.f, 0.f, 0.1f, {0, 1}, {}});

    add(SettingId::GraphicsHdTexturesEnabled, "hd_texture_packs", "graphics", Category::Graphics, SettingType::Bool,
        FeatureStatus::Planned, ApplyScope::AppRestart, "HD TEXTURE PACKS",
        "[Planned Phase 3] Enables external hash-matched replacement textures from Black/mods/.");

    // --- Combat & Visual Effects ---
    add(SettingId::CombatScreenDamageVignette, "screen_damage_vignette", "combat", Category::Combat, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "BLOOD DAMAGE VIGNETTE",
        "Displays radial blood pulse vignette when player health drops.");

    add(SettingId::CombatDamageVignetteIntensity, "damage_vignette_intensity", "combat", Category::Combat, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "VIGNETTE INTENSITY",
        "Controls opacity of screen-space damage feedback overlay.",
        NumericBounds{10, 100, 5, 0.f, 0.f, 0.1f, {}, {}});

    add(SettingId::CombatLowHealthDesaturation, "low_health_desaturation", "combat", Category::Combat, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "LOW HEALTH DESATURATION",
        "Gradually reduces color saturation when health falls below critical threshold.");

    add(SettingId::CombatExplosionGlowBoost, "explosion_glow_boost", "combat", Category::Combat, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "EXPLOSION GLOW BOOST",
        "Scales dynamic highlight bloom emitted during explosions and muzzle flashes.",
        NumericBounds{50, 500, 10, 0.f, 0.f, 0.1f, {}, {}});

    add(SettingId::CombatParticleFadeScale, "particle_fade_scale", "combat", Category::Combat, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "PARTICLE FADE DISTANCE",
        "Independently scales ParticleFade distance for smoke, sparks, and blood.",
        NumericBounds{50, 1000, 25, 0.f, 0.f, 0.1f, {}, {}});

    add(SettingId::CombatBloodParticleMultiplier, "blood_particle_multiplier", "combat", Category::Combat, SettingType::Uint,
        FeatureStatus::Planned, ApplyScope::SceneReload, "BLOOD PARTICLE DENSITY",
        "[Planned — Requires XBE RE] Multiplies blood emitter burst counts around sub_001E74B0.",
        NumericBounds{1, 3, 1, 0.f, 0.f, 0.1f, {1, 2, 3}, {}});

    add(SettingId::CombatPersistentBloodDecals, "persistent_blood_decals", "combat", Category::Combat, SettingType::Bool,
        FeatureStatus::Planned, ApplyScope::SceneReload, "PERSISTENT BLOOD DECALS",
        "[Planned — Requires XBE RE] Retains surface blood decals longer via expanded decal pool.");

    add(SettingId::CombatMaxBloodDecals, "max_blood_decals", "combat", Category::Combat, SettingType::Uint,
        FeatureStatus::Planned, ApplyScope::SceneReload, "MAX BLOOD DECALS",
        "[Planned — Requires XBE RE] Maximum simultaneous decals held in expanded guest ring buffer.",
        NumericBounds{64, 1024, 64, 0.f, 0.f, 0.1f, {}, {}});

    add(SettingId::CombatCorpsePersistenceMode, "corpse_persistence_mode", "combat", Category::Combat, SettingType::Uint,
        FeatureStatus::Planned, ApplyScope::SceneReload, "CORPSE PERSISTENCE",
        "[Planned — High AI Pool Risk] Extends ragdoll despawn countdown safely across combat waves.",
        NumericBounds{0, 2, 1, 0.f, 0.f, 0.1f, {0, 1, 2}, {}});

    add(SettingId::CombatDiverseBulletImpacts, "diverse_bullet_impacts", "combat", Category::Combat, SettingType::Bool,
        FeatureStatus::Planned, ApplyScope::SceneReload, "DIVERSE BULLET IMPACTS",
        "[Planned — Requires XBE RE] Triggers multi-layer sparks/debris on metal, concrete, and wood.");

    // --- Characters ---
    add(SettingId::CharactersRagdoll60Fps, "ragdoll_60fps_physics", "characters", Category::Characters, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "HIGH-FPS RAGDOLL SYNC",
        "Controls frame-rate independent 60+ FPS ragdoll damping and joint solver scaling.");

    add(SettingId::CharactersLodScale, "character_lod_scale", "characters", Category::Characters, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "CHARACTER DETAIL LOD",
        "Independent character mesh LOD multiplier distinct from global draw distance.",
        NumericBounds{1, 4, 1, 0.f, 0.f, 0.1f, {1, 2, 4}, {}});

    add(SettingId::CharactersCustomModels, "custom_character_models", "characters", Category::Characters, SettingType::Bool,
        FeatureStatus::Unavailable, ApplyScope::AppRestart, "CUSTOM CHARACTER PACK",
        "[Unavailable] Requires compatible modular VFS character package under Black/mods/.");

    // --- Audio ---
    add(SettingId::AudioOutput, "audio_output", nullptr, Category::Audio, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "AUDIO OUTPUT MODE",
        "AAudio output channel configuration: Auto (0), Mono (1), Stereo 2.0 (2), Surround 5.1 (3).",
        NumericBounds{0, 3, 1, 0.f, 0.f, 0.1f, {0, 1, 2, 3}, {}});

    add(SettingId::AudioCombatDynamicBoost, "combat_dynamic_boost", "audio", Category::Audio, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "COMBAT AUDIO BOOST",
        "Host-side punchy limiter and low-end punch curve for gunfire and explosions.");

    add(SettingId::AudioMasterGainPercent, "master_gain_percent", "audio", Category::Audio, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "MASTER GAIN",
        "Output gain multiplier applied at the AAudio host mixing stage.",
        NumericBounds{10, 150, 5, 0.f, 0.f, 0.1f, {}, {}});

    add(SettingId::AudioModPackEnabled, "mod_audio_pack_enabled", "audio", Category::Audio, SettingType::Bool,
        FeatureStatus::Unavailable, ApplyScope::AppRestart, "REMASTERED AUDIO PACK",
        "[Unavailable] Requires VFS overlay subsystem and external D:\\sound\\ package.");

    // --- Arabic Localization & Language ---
    add(SettingId::Language, "language", nullptr, Category::Arabic, SettingType::String,
        FeatureStatus::Implemented, ApplyScope::AppRestart, "GAME LANGUAGE",
        "Boot language code (int, fra, deu, ita, esp, ind, ara). Applied on next app restart.",
        NumericBounds{0, 0, 1, 0.f, 0.f, 0.1f, {}, {"int", "fra", "deu", "ita", "esp", "ind", "ara"}});

    add(SettingId::ArabicUiEnabled, "arabic_ui_enabled", "arabic", Category::Arabic, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::AppRestart, "ARABIC HOST INTERFACE",
        "Enables contextually shaped and BiDi-handled Arabic UI with Noto Naskh Arabic font.");

    add(SettingId::ArabicRtlMirror, "arabic_rtl_mirror", "arabic", Category::Arabic, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "RTL LAYOUT MIRRORING",
        "Mirrors settings columns, labels, sliders, and category tabs right-to-left.");

    add(SettingId::ArabicSubtitlesOverlay, "arabic_subtitles_overlay", "arabic", Category::Arabic, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "ARABIC SUBTITLES HUD",
        "Renders Arabic mission objectives and dialogue via host HUD bypassing 127B cap.");

    add(SettingId::ArabicFlashMenus, "arabic_flash_menus", "arabic", Category::Arabic, SettingType::Bool,
        FeatureStatus::Planned, ApplyScope::AppRestart, "ARABIC FLASH MENUS",
        "[Planned — Difficult] Injects Arabic glyph atlases into Eurostile LT Std Flash menus.");

    add(SettingId::ArabicVoicePack, "arabic_voice_pack", "arabic", Category::Arabic, SettingType::Bool,
        FeatureStatus::Unavailable, ApplyScope::AppRestart, "ARABIC DUBBING PACK",
        "[Unavailable] Requires external Arabic voice-over package under Black/mods/.");

    // --- Gameplay & Controls ---
    add(SettingId::Fov, "fov", nullptr, Category::Gameplay, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "FIELD OF VIEW",
        "Horizontal camera field of view in degrees on 4:3 baseline (70 = game default).",
        NumericBounds{45, 140, 1, 0.f, 0.f, 0.1f, {}, {}});

    add(SettingId::ViewmodelFov, "viewmodel_fov", nullptr, Category::Gameplay, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "VIEWMODEL FOV",
        "First-person weapon and arms field of view in degrees (70 = game default).",
        NumericBounds{45, 140, 1, 0.f, 0.f, 0.1f, {}, {}});

    add(SettingId::WalkSpeed, "walk_speed", nullptr, Category::Gameplay, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "WALK SPEED",
        "Movement speed percentage relative to run speed while holding the walk key.",
        NumericBounds{10, 100, 5, 0.f, 0.f, 0.1f, {}, {}});

    add(SettingId::HoldAim, "hold_aim", nullptr, Category::Controls, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "HOLD TO AIM",
        "Requires holding aim/scope instead of toggling on mouse/controller.");

    add(SettingId::InvertMouse, "invert_mouse", nullptr, Category::Controls, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "INVERT LOOK",
        "Inverts vertical pitch axis on mouse/touch look.");

    add(SettingId::MouseSensitivity, "mouse_sensitivity", nullptr, Category::Controls, SettingType::Float,
        FeatureStatus::Implemented, ApplyScope::Immediate, "MOUSE SENSITIVITY",
        "Look sensitivity multiplier for connected mouse input.",
        NumericBounds{0, 0, 1, 0.1f, 5.0f, 0.1f, {}, {}});

    add(SettingId::TouchSensitivity, "touch_sensitivity", nullptr, Category::Controls, SettingType::Float,
        FeatureStatus::Implemented, ApplyScope::Immediate, "TOUCH SENSITIVITY",
        "Look sensitivity multiplier for touchscreen camera drag.",
        NumericBounds{0, 0, 1, 0.1f, 5.0f, 0.1f, {}, {}});

    add(SettingId::LeftDeadzone, "left_stick_deadzone", nullptr, Category::Controls, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "LEFT STICK DEADZONE",
        "Radial inner deadzone percentage for movement stick (0 = game default).",
        NumericBounds{0, 50, 1, 0.f, 0.f, 0.1f, {}, {}});

    add(SettingId::RightDeadzone, "right_stick_deadzone", nullptr, Category::Controls, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "RIGHT STICK DEADZONE",
        "Radial inner deadzone percentage for aiming stick (0 = game default).",
        NumericBounds{0, 50, 1, 0.f, 0.f, 0.1f, {}, {}});

    add(SettingId::LeftSensitivity, "left_stick_sensitivity", nullptr, Category::Controls, SettingType::Float,
        FeatureStatus::Implemented, ApplyScope::Immediate, "LEFT STICK SENSITIVITY",
        "Controller movement stick scaling factor past deadzone.",
        NumericBounds{0, 0, 1, 0.1f, 3.0f, 0.1f, {}, {}});

    add(SettingId::RightSensitivity, "right_stick_sensitivity", nullptr, Category::Controls, SettingType::Float,
        FeatureStatus::Implemented, ApplyScope::Immediate, "RIGHT STICK SENSITIVITY",
        "Controller aim stick scaling factor past deadzone.",
        NumericBounds{0, 0, 1, 0.1f, 3.0f, 0.1f, {}, {}});

    // Cheats (never touched by Presets)
    add(SettingId::InfiniteAmmo, "infinite_ammo", nullptr, Category::Cheats, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "INFINITE AMMO",
        "Prevents magazine and reserve ammunition from decreasing when firing.");
    add(SettingId::RapidFire, "rapid_fire", nullptr, Category::Cheats, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "RAPID FIRE",
        "Weapon fire-rate multiplier (1 = default, up to 4x).",
        NumericBounds{1, 4, 1, 0.f, 0.f, 0.1f, {}, {}});
    add(SettingId::NoRecoil, "no_recoil", nullptr, Category::Cheats, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "NO RECOIL",
        "Disables weapon recoil kick during sustained firing.");
    add(SettingId::GameSpeed, "game_speed", nullptr, Category::Cheats, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "GAME SPEED",
        "Gameplay simulation speed multiplier percentage (25% to 200%).",
        NumericBounds{25, 200, 25, 0.f, 0.f, 0.1f, {}, {}});
    add(SettingId::GodMode, "god_mode", nullptr, Category::Cheats, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "GOD MODE",
        "Prevents enemy hits from reducing player health.");
    add(SettingId::OneHitKill, "one_hit_kill", nullptr, Category::Cheats, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "ONE-HIT KILL",
        "Any player bullet hit eliminates targeted soldiers instantly.");
    add(SettingId::InfiniteGrenades, "infinite_grenades", nullptr, Category::Cheats, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "INFINITE GRENADES",
        "Throwing grenades leaves inventory count unchanged.");
    add(SettingId::UnlockLevels, "unlock_levels", nullptr, Category::Cheats, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "UNLOCK ALL MISSIONS",
        "Unlocks all campaign missions across difficulties without editing saves.");
    add(SettingId::UnlockBlackOps, "unlock_black_ops", nullptr, Category::Cheats, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "UNLOCK BLACK OPS",
        "Unlocks Black Ops difficulty on unlocked missions without editing saves.");
    add(SettingId::Esp, "esp", nullptr, Category::Cheats, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "ESP LINES",
        "Draws diagnostic target boxes and lines to active enemy soldiers.");
    add(SettingId::Aimbot, "aimbot", nullptr, Category::Cheats, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "AIMBOT",
        "Snaps crosshair toward nearest living soldier while aiming down sights.");

    add(SettingId::GameplayEnhancedRecoil, "enhanced_recoil_feedback", "gameplay", Category::Gameplay, SettingType::Bool,
        FeatureStatus::Planned, ApplyScope::SceneReload, "ENHANCED RECOIL CURVES",
        "[Planned — Requires XBE RE] Tightens weapon recoil recovery curves via guest tables.");

    add(SettingId::GameplayDisableAimAssist, "disable_aim_assist", "gameplay", Category::Gameplay, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "DISABLE AIM ASSIST",
        "Disables console auto-aim magnetism hook when toggled on.");

    // --- Interface ---
    add(SettingId::SkipIntros, "skip_intros", nullptr, Category::Interface, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "SKIP INTROS",
        "Skips publisher intro movies and startup splash videos on launch.");

    add(SettingId::FmvScale, "fmv_scale", nullptr, Category::Interface, SettingType::Uint,
        FeatureStatus::Implemented, ApplyScope::Immediate, "FMV SCALE",
        "Presentation mode for 4:3 pre-rendered videos on wide screens (0 = Fill, 1 = 4:3).",
        NumericBounds{0, 1, 1, 0.f, 0.f, 0.1f, {0, 1}, {}});

    add(SettingId::FpsCounter, "fps_counter", nullptr, Category::Interface, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "FPS COUNTER",
        "Displays real-time frames-per-second counter in the game HUD.");

    add(SettingId::InterfaceThemeAccent, "settings_theme_accent", "interface", Category::Interface, SettingType::String,
        FeatureStatus::Implemented, ApplyScope::Immediate, "CONSOLE ACCENT THEME",
        "Switches settings console highlight palette (brass, crimson, olive).",
        NumericBounds{0, 0, 1, 0.f, 0.f, 0.1f, {}, {"brass", "crimson", "olive"}});

    add(SettingId::InterfaceAdvancedPerfHud, "advanced_perf_hud", "interface", Category::Interface, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "ADVANCED TELEMETRY HUD",
        "Shows frame time (ms) and live render resolution scale alongside FPS.");

    add(SettingId::InterfaceShowRestartBanners, "show_restart_banners", "interface", Category::Interface, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "RESTART BANNERS",
        "Shows persistent footer alert when boot-latched settings change.");

    add(SettingId::InterfaceHideHudOnScreenshots, "hide_hud_on_screenshots", "interface", Category::Interface, SettingType::Bool,
        FeatureStatus::Implemented, ApplyScope::Immediate, "CLEAN CAPTURE MODE",
        "Hides touch overlay and gameplay HUD for clean screenshots.");

    add(SettingId::PackagesOverlayEnabled, "overlay_enabled", "packages", Category::Interface, SettingType::Bool,
        FeatureStatus::Planned, ApplyScope::AppRestart, "MOD OVERLAY ENGINE",
        "[Planned Task 3] Enables non-destructive Black/mods/ VFS overrides with ISO fallback.");

    // --- Physics Lab (all multipliers default to 1.00x Classic 2006 behavior) ---
    add(SettingId::PhysicsPreset, "preset", "physics_lab", Category::PhysicsLab, SettingType::String,
        FeatureStatus::Implemented, ApplyScope::Immediate, "PHYSICS PROFILE",
        "Selects calibrated physics preset: classic (1.00x), realistic, cinematic, mayhem, or custom.",
        NumericBounds{0, 0, 1, 0.f, 0.f, 0.1f, {}, {"classic", "realistic", "cinematic", "mayhem", "custom"}});

    add(SettingId::PhysicsExplosionForce, "explosion_force", "physics_lab", Category::PhysicsLab, SettingType::Float,
        FeatureStatus::Implemented, ApplyScope::Immediate, "EXPLOSION FORCE",
        "Scales radial blast impulse applied by sub_000DCC40 on destructibles, props, and entities.",
        NumericBounds{0, 0, 1, 0.25f, 10.00f, 0.05f, {}, {}});

    add(SettingId::PhysicsImpactForce, "impact_force", "physics_lab", Category::PhysicsLab, SettingType::Float,
        FeatureStatus::Implemented, ApplyScope::Immediate, "IMPACT FORCE",
        "Scales kinetic bullet impact impulse applied by sub_000DCF80 on props and destructibles.",
        NumericBounds{0, 0, 1, 0.25f, 10.00f, 0.05f, {}, {}});

    add(SettingId::PhysicsGravityScale, "gravity_scale", "physics_lab", Category::PhysicsLab, SettingType::Float,
        FeatureStatus::Implemented, ApplyScope::Immediate, "GRAVITY SCALE",
        "Scales world physics Y-gravity (-600.0 u/s^2 baseline) and shard acceleration.",
        NumericBounds{0, 0, 1, 0.05f, 5.00f, 0.05f, {}, {}});

    add(SettingId::PhysicsDebrisIntensity, "debris_intensity", "physics_lab", Category::PhysicsLab, SettingType::Float,
        FeatureStatus::Implemented, ApplyScope::Immediate, "DEBRIS INTENSITY",
        "Scales shatter shard scatter velocity and extends particle visibility fade range.",
        NumericBounds{0, 0, 1, 0.25f, 6.00f, 0.05f, {}, {}});

    add(SettingId::PhysicsCharacterReaction, "character_reaction", "physics_lab", Category::PhysicsLab, SettingType::Float,
        FeatureStatus::Implemented, ApplyScope::Immediate, "CHARACTER REACTION",
        "Scales ragdoll speed ceiling and velocity retention during body simulation.",
        NumericBounds{0, 0, 1, 0.25f, 8.00f, 0.05f, {}, {}});

    add(SettingId::PhysicsWeaponRecoil, "weapon_recoil", "physics_lab", Category::PhysicsLab, SettingType::Float,
        FeatureStatus::Implemented, ApplyScope::Immediate, "WEAPON RECOIL",
        "Scales shot camera kick and secondary recoil target intensity.",
        NumericBounds{0, 0, 1, 0.00f, 4.00f, 0.05f, {}, {}});

    add(SettingId::PhysicsExplosionRadius, "explosion_radius", "physics_lab", Category::PhysicsLab, SettingType::Float,
        FeatureStatus::Implemented, ApplyScope::Immediate, "EXPLOSION RADIUS",
        "Scales effective blast radius and radial shockwave falloff distance.",
        NumericBounds{0, 0, 1, 0.50f, 5.00f, 0.05f, {}, {}});

    add(SettingId::PhysicsRagdollLaunch, "ragdoll_launch", "physics_lab", Category::PhysicsLab, SettingType::Float,
        FeatureStatus::Implemented, ApplyScope::Immediate, "RAGDOLL LAUNCH",
        "Scales vertical lift and airborne knockback applied to enemy ragdolls on hit or blast.",
        NumericBounds{0, 0, 1, 0.25f, 10.00f, 0.05f, {}, {}});

    add(SettingId::PhysicsStructuralDestruction, "structural_destruction", "physics_lab", Category::PhysicsLab, SettingType::Float,
        FeatureStatus::Implemented, ApplyScope::Immediate, "STRUCTURAL DESTRUCTION",
        "Scales structural damage against doors, locks, walls, cover, and breakable objects.",
        NumericBounds{0, 0, 1, 0.25f, 10.00f, 0.05f, {}, {}});

    add(SettingId::PhysicsPropBounciness, "prop_bounciness", "physics_lab", Category::PhysicsLab, SettingType::Float,
        FeatureStatus::Implemented, ApplyScope::Immediate, "PROP SPIN & BOUNCE",
        "Scales angular spin torque and reduces velocity damping on dynamic props and barrels.",
        NumericBounds{0, 0, 1, 0.25f, 6.00f, 0.05f, {}, {}});

    add(SettingId::PhysicsCameraShockwave, "camera_shockwave", "physics_lab", Category::PhysicsLab, SettingType::Float,
        FeatureStatus::Implemented, ApplyScope::Immediate, "BLAST SHOCKWAVE SHAKE",
        "Scales explosion shockwave camera shake intensity and radius.",
        NumericBounds{0, 0, 1, 0.00f, 4.00f, 0.05f, {}, {}});

    add(SettingId::PhysicsTimeScale, "physics_time_scale", "physics_lab", Category::PhysicsLab, SettingType::Float,
        FeatureStatus::Implemented, ApplyScope::Immediate, "PHYSICS SIMULATION SPEED",
        "Scales the rigid-body and ragdoll physics solver timestep (slow-motion or hyper-speed).",
        NumericBounds{0, 0, 1, 0.20f, 2.00f, 0.05f, {}, {}});
}

const ModFeatureMeta* SettingsRegistry::Find(SettingId id) const {
    for (const auto& m : definitions_)
        if (m.id == id) return &m;
    return nullptr;
}

const ModFeatureMeta* SettingsRegistry::FindByKey(std::string_view key, std::string_view section) const {
    for (const auto& m : definitions_) {
        std::string_view m_sec = m.section ? m.section : "";
        if (m.key == key && m_sec == section) return &m;
    }
    return nullptr;
}

void SettingsRegistry::ResetToDefaults(app::Settings& s, game::Options& o) {
    s = app::Settings{};
    s.display_mode = app::DisplayMode::Fullscreen;
    s.render_percent = kDefaultRenderPercent;
    s.fps_cap = 0;
    s.vsync = false;
    s.aniso = 0;
    o = game::Options{};
    enhanced_ = EnhancedOptions{};
    unknown_nodes_ = toml::table{};
}

bool SettingsRegistry::GetBool(SettingId id, const app::Settings& s, const game::Options& o) const {
    switch (id) {
        case SettingId::Vsync: return s.vsync;
        case SettingId::DynamicResolution: return o.dynamic_resolution;
        case SettingId::Bloom: return o.bloom;
        case SettingId::Fog: return o.fog;
        case SettingId::Hdr: return o.hdr;
        case SettingId::ReloadBlur: return o.reload_blur;
        case SettingId::SkipIntros: return o.skip_intros;
        case SettingId::FpsCounter: return o.fps_counter;
        case SettingId::HoldAim: return o.hold_aim;
        case SettingId::InvertMouse: return o.invert_mouse;
        case SettingId::InfiniteAmmo: return o.infinite_ammo;
        case SettingId::NoRecoil: return o.no_recoil;
        case SettingId::GodMode: return o.god_mode;
        case SettingId::OneHitKill: return o.one_hit_kill;
        case SettingId::InfiniteGrenades: return o.infinite_grenades;
        case SettingId::UnlockLevels: return o.unlock_levels;
        case SettingId::UnlockBlackOps: return o.unlock_black_ops;
        case SettingId::Esp: return o.esp;
        case SettingId::Aimbot: return o.aimbot;
        case SettingId::GraphicsCasSharpenEnabled: return enhanced_.cas_sharpen_enabled;
        case SettingId::GraphicsTrilinearFiltering: return enhanced_.trilinear_filtering;
        case SettingId::GraphicsBlastLensPulse: return enhanced_.blast_lens_pulse;
        case SettingId::GraphicsHdTexturesEnabled: return enhanced_.hd_textures_enabled;
        case SettingId::CombatScreenDamageVignette: return enhanced_.screen_damage_vignette;
        case SettingId::CombatLowHealthDesaturation: return enhanced_.low_health_desaturation;
        case SettingId::CombatPersistentBloodDecals: return enhanced_.persistent_blood_decals;
        case SettingId::CombatDiverseBulletImpacts: return enhanced_.diverse_bullet_impacts;
        case SettingId::CharactersRagdoll60Fps: return enhanced_.ragdoll_60fps_physics;
        case SettingId::CharactersCustomModels: return enhanced_.custom_character_models;
        case SettingId::AudioCombatDynamicBoost: return enhanced_.combat_dynamic_boost;
        case SettingId::AudioModPackEnabled: return enhanced_.mod_audio_pack_enabled;
        case SettingId::ArabicUiEnabled: return enhanced_.arabic_ui_enabled;
        case SettingId::ArabicRtlMirror: return enhanced_.arabic_rtl_mirror;
        case SettingId::ArabicSubtitlesOverlay: return enhanced_.arabic_subtitles_overlay;
        case SettingId::ArabicFlashMenus: return enhanced_.arabic_flash_menus;
        case SettingId::ArabicVoicePack: return enhanced_.arabic_voice_pack;
        case SettingId::GameplayEnhancedRecoil: return enhanced_.enhanced_recoil_feedback;
        case SettingId::GameplayDisableAimAssist: return enhanced_.disable_aim_assist;
        case SettingId::InterfaceAdvancedPerfHud: return enhanced_.advanced_perf_hud;
        case SettingId::InterfaceShowRestartBanners: return enhanced_.show_restart_banners;
        case SettingId::InterfaceHideHudOnScreenshots: return enhanced_.hide_hud_on_screenshots;
        case SettingId::PackagesOverlayEnabled: return enhanced_.packages_overlay_enabled;
        default: return false;
    }
}

uint32_t SettingsRegistry::GetUint(SettingId id, const app::Settings& s, const game::Options& o) const {
    switch (id) {
        case SettingId::RenderPercent: return s.render_percent;
        case SettingId::FpsCap: return s.fps_cap;
        case SettingId::AnisotropicFiltering: return s.aniso;
        case SettingId::AntiAliasing: return o.anti_aliasing;
        case SettingId::ScreenFilter: return o.screen_filter;
        case SettingId::DrawDistance: return o.draw_distance;
        case SettingId::DynamicTargetFps: return o.dynamic_target_fps;
        case SettingId::DynamicMinPercent: return o.dynamic_min_percent;
        case SettingId::DynamicMaxPercent: return o.dynamic_max_percent;
        case SettingId::DynamicHold: return o.dynamic_hold;
        case SettingId::FmvScale: return o.fmv_scale;
        case SettingId::AudioOutput: return o.audio_output;
        case SettingId::Fov: return o.fov;
        case SettingId::ViewmodelFov: return o.viewmodel_fov;
        case SettingId::WalkSpeed: return o.walk_speed;
        case SettingId::LeftDeadzone: return o.left_deadzone;
        case SettingId::RightDeadzone: return o.right_deadzone;
        case SettingId::RapidFire: return o.rapid_fire;
        case SettingId::GameSpeed: return o.game_speed;
        case SettingId::GraphicsToneMapping: return enhanced_.tone_mapping;
        case SettingId::GraphicsCasSharpenStrength: return enhanced_.cas_sharpen_strength;
        case SettingId::GraphicsBloomIntensity: return enhanced_.bloom_intensity;
        case SettingId::GraphicsCinematicGrade: return enhanced_.cinematic_grade;
        case SettingId::GraphicsLensVignette: return enhanced_.lens_vignette;
        case SettingId::GraphicsFilmGrain: return enhanced_.film_grain;
        case SettingId::GraphicsSoftShadows: return enhanced_.soft_shadows;
        case SettingId::GraphicsAmbientOcclusion: return enhanced_.ambient_occlusion;
        case SettingId::CombatDamageVignetteIntensity: return enhanced_.damage_vignette_intensity;
        case SettingId::CombatExplosionGlowBoost: return enhanced_.explosion_glow_boost;
        case SettingId::CombatParticleFadeScale: return enhanced_.particle_fade_scale;
        case SettingId::CombatBloodParticleMultiplier: return enhanced_.blood_particle_multiplier;
        case SettingId::CombatMaxBloodDecals: return enhanced_.max_blood_decals;
        case SettingId::CombatCorpsePersistenceMode: return enhanced_.corpse_persistence_mode;
        case SettingId::CharactersLodScale: return enhanced_.character_lod_scale;
        case SettingId::AudioMasterGainPercent: return enhanced_.master_gain_percent;
        default: return 0;
    }
}

float SettingsRegistry::GetFloat(SettingId id, const app::Settings& /*s*/, const game::Options& o) const {
    switch (id) {
        case SettingId::MouseSensitivity: return o.mouse_sensitivity;
        case SettingId::TouchSensitivity: return o.touch_sensitivity;
        case SettingId::LeftSensitivity: return o.left_sensitivity;
        case SettingId::RightSensitivity: return o.right_sensitivity;
        case SettingId::GraphicsTextureLodBias: return enhanced_.texture_lod_bias;
        case SettingId::PhysicsExplosionForce: return enhanced_.physics_explosion_force;
        case SettingId::PhysicsImpactForce: return enhanced_.physics_impact_force;
        case SettingId::PhysicsGravityScale: return enhanced_.physics_gravity_scale;
        case SettingId::PhysicsDebrisIntensity: return enhanced_.physics_debris_intensity;
        case SettingId::PhysicsCharacterReaction: return enhanced_.physics_character_reaction;
        case SettingId::PhysicsWeaponRecoil: return enhanced_.physics_weapon_recoil;
        case SettingId::PhysicsExplosionRadius: return enhanced_.physics_explosion_radius;
        case SettingId::PhysicsRagdollLaunch: return enhanced_.physics_ragdoll_launch;
        case SettingId::PhysicsStructuralDestruction: return enhanced_.physics_structural_destruction;
        case SettingId::PhysicsPropBounciness: return enhanced_.physics_prop_bounciness;
        case SettingId::PhysicsCameraShockwave: return enhanced_.physics_camera_shockwave;
        case SettingId::PhysicsTimeScale: return enhanced_.physics_time_scale;
        default: return 0.0f;
    }
}

std::string SettingsRegistry::GetString(SettingId id, const app::Settings& /*s*/, const game::Options& o) const {
    switch (id) {
        case SettingId::Language: return o.language;
        case SettingId::InterfaceThemeAccent: return enhanced_.theme_accent;
        case SettingId::PhysicsPreset: return enhanced_.physics_preset;
        default: return "";
    }
}

ValidationResult SettingsRegistry::SetBool(SettingId id, bool val, app::Settings& s, game::Options& o) {
    const ModFeatureMeta* m = Find(id);
    if (!m) return {false, ApplyScope::Immediate, "Unknown setting identifier"};
    if (m->type != SettingType::Bool) return {false, m->scope, "Type mismatch (expected Bool)"};
    if (m->status != FeatureStatus::Implemented) {
        return {false, m->scope,
                m->status == FeatureStatus::Planned ? "Feature is Planned and locked" : "Feature is Unavailable"};
    }
    switch (id) {
        case SettingId::Vsync: s.vsync = val; break;
        case SettingId::DynamicResolution: o.dynamic_resolution = val; break;
        case SettingId::Bloom: o.bloom = val; break;
        case SettingId::Fog: o.fog = val; break;
        case SettingId::Hdr: o.hdr = val; break;
        case SettingId::ReloadBlur: o.reload_blur = val; break;
        case SettingId::SkipIntros: o.skip_intros = val; break;
        case SettingId::FpsCounter: o.fps_counter = val; break;
        case SettingId::HoldAim: o.hold_aim = val; break;
        case SettingId::InvertMouse: o.invert_mouse = val; break;
        case SettingId::InfiniteAmmo: o.infinite_ammo = val; break;
        case SettingId::NoRecoil: o.no_recoil = val; break;
        case SettingId::GodMode: o.god_mode = val; break;
        case SettingId::OneHitKill: o.one_hit_kill = val; break;
        case SettingId::InfiniteGrenades: o.infinite_grenades = val; break;
        case SettingId::UnlockLevels: o.unlock_levels = val; break;
        case SettingId::UnlockBlackOps: o.unlock_black_ops = val; break;
        case SettingId::Esp: o.esp = val; break;
        case SettingId::Aimbot: o.aimbot = val; break;
        case SettingId::ArabicUiEnabled: enhanced_.arabic_ui_enabled = val; break;
        case SettingId::ArabicRtlMirror: enhanced_.arabic_rtl_mirror = val; break;
        case SettingId::ArabicSubtitlesOverlay: enhanced_.arabic_subtitles_overlay = val; break;
        case SettingId::GraphicsCasSharpenEnabled: enhanced_.cas_sharpen_enabled = val; break;
        case SettingId::GraphicsTrilinearFiltering: enhanced_.trilinear_filtering = val; break;
        case SettingId::GraphicsBlastLensPulse: enhanced_.blast_lens_pulse = val; break;
        case SettingId::CombatScreenDamageVignette: enhanced_.screen_damage_vignette = val; break;
        case SettingId::CombatLowHealthDesaturation: enhanced_.low_health_desaturation = val; break;
        case SettingId::CharactersRagdoll60Fps: enhanced_.ragdoll_60fps_physics = val; break;
        case SettingId::AudioCombatDynamicBoost: enhanced_.combat_dynamic_boost = val; break;
        case SettingId::GameplayDisableAimAssist: enhanced_.disable_aim_assist = val; break;
        case SettingId::InterfaceAdvancedPerfHud: enhanced_.advanced_perf_hud = val; break;
        case SettingId::InterfaceShowRestartBanners: enhanced_.show_restart_banners = val; break;
        case SettingId::InterfaceHideHudOnScreenshots: enhanced_.hide_hud_on_screenshots = val; break;
        default: return {false, m->scope, "Unwritable bool binding"};
    }
    return {true, m->scope, ""};
}

ValidationResult SettingsRegistry::SetUint(SettingId id, uint32_t val, app::Settings& s, game::Options& o) {
    const ModFeatureMeta* m = Find(id);
    if (!m) return {false, ApplyScope::Immediate, "Unknown setting identifier"};
    if (m->type != SettingType::Uint) return {false, m->scope, "Type mismatch (expected Uint)"};
    if (m->status != FeatureStatus::Implemented) {
        return {false, m->scope,
                m->status == FeatureStatus::Planned ? "Feature is Planned and locked" : "Feature is Unavailable"};
    }
    bool adj = false;
    uint32_t clamped = ClampAndStepUint(val, m->bounds, adj);
    switch (id) {
        case SettingId::RenderPercent: s.render_percent = clamped; break;
        case SettingId::FpsCap: s.fps_cap = clamped; break;
        case SettingId::AnisotropicFiltering: s.aniso = clamped; break;
        case SettingId::AntiAliasing: o.anti_aliasing = clamped; break;
        case SettingId::ScreenFilter: o.screen_filter = clamped; break;
        case SettingId::DrawDistance: o.draw_distance = clamped; break;
        case SettingId::DynamicTargetFps: o.dynamic_target_fps = clamped; break;
        case SettingId::DynamicMinPercent:
            o.dynamic_min_percent = std::min(clamped, o.dynamic_max_percent);
            break;
        case SettingId::DynamicMaxPercent:
            o.dynamic_max_percent = std::max(clamped, o.dynamic_min_percent);
            break;
        case SettingId::DynamicHold: o.dynamic_hold = clamped; break;
        case SettingId::FmvScale: o.fmv_scale = clamped; break;
        case SettingId::AudioOutput: o.audio_output = clamped; break;
        case SettingId::Fov: o.fov = clamped; break;
        case SettingId::ViewmodelFov: o.viewmodel_fov = clamped; break;
        case SettingId::WalkSpeed: o.walk_speed = clamped; break;
        case SettingId::LeftDeadzone: o.left_deadzone = clamped; break;
        case SettingId::RightDeadzone: o.right_deadzone = clamped; break;
        case SettingId::RapidFire: o.rapid_fire = clamped; break;
        case SettingId::GameSpeed: o.game_speed = clamped; break;
        case SettingId::GraphicsToneMapping: enhanced_.tone_mapping = clamped; break;
        case SettingId::GraphicsCasSharpenStrength: enhanced_.cas_sharpen_strength = clamped; break;
        case SettingId::GraphicsBloomIntensity: enhanced_.bloom_intensity = clamped; break;
        case SettingId::GraphicsCinematicGrade: enhanced_.cinematic_grade = clamped; break;
        case SettingId::GraphicsLensVignette: enhanced_.lens_vignette = clamped; break;
        case SettingId::GraphicsFilmGrain: enhanced_.film_grain = clamped; break;
        case SettingId::CombatDamageVignetteIntensity: enhanced_.damage_vignette_intensity = clamped; break;
        case SettingId::CombatExplosionGlowBoost: enhanced_.explosion_glow_boost = clamped; break;
        case SettingId::CombatParticleFadeScale: enhanced_.particle_fade_scale = clamped; break;
        case SettingId::CharactersLodScale: enhanced_.character_lod_scale = clamped; break;
        case SettingId::AudioMasterGainPercent: enhanced_.master_gain_percent = clamped; break;
        default: return {false, m->scope, "Unwritable uint binding"};
    }
    return {true, m->scope, ""};
}

ValidationResult SettingsRegistry::SetFloat(SettingId id, float val, app::Settings& /*s*/, game::Options& o) {
    const ModFeatureMeta* m = Find(id);
    if (!m) return {false, ApplyScope::Immediate, "Unknown setting identifier"};
    if (m->type != SettingType::Float) return {false, m->scope, "Type mismatch (expected Float)"};
    if (m->status != FeatureStatus::Implemented) {
        return {false, m->scope,
                m->status == FeatureStatus::Planned ? "Feature is Planned and locked" : "Feature is Unavailable"};
    }
    if (!std::isfinite(val)) return {false, m->scope, "Non-finite float value rejected"};
    float clamped = val;
    if (m->bounds.float_max > m->bounds.float_min) clamped = std::clamp(val, m->bounds.float_min, m->bounds.float_max);
    switch (id) {
        case SettingId::MouseSensitivity: o.mouse_sensitivity = clamped; break;
        case SettingId::TouchSensitivity: o.touch_sensitivity = clamped; break;
        case SettingId::LeftSensitivity: o.left_sensitivity = clamped; break;
        case SettingId::RightSensitivity: o.right_sensitivity = clamped; break;
        case SettingId::GraphicsTextureLodBias: enhanced_.texture_lod_bias = clamped; break;
        case SettingId::PhysicsExplosionForce:
            enhanced_.physics_explosion_force = clamped;
            enhanced_.physics_preset = "custom";
            break;
        case SettingId::PhysicsImpactForce:
            enhanced_.physics_impact_force = clamped;
            enhanced_.physics_preset = "custom";
            break;
        case SettingId::PhysicsGravityScale:
            enhanced_.physics_gravity_scale = clamped;
            enhanced_.physics_preset = "custom";
            break;
        case SettingId::PhysicsDebrisIntensity:
            enhanced_.physics_debris_intensity = clamped;
            enhanced_.physics_preset = "custom";
            break;
        case SettingId::PhysicsCharacterReaction:
            enhanced_.physics_character_reaction = clamped;
            enhanced_.physics_preset = "custom";
            break;
        case SettingId::PhysicsWeaponRecoil:
            enhanced_.physics_weapon_recoil = clamped;
            enhanced_.physics_preset = "custom";
            break;
        case SettingId::PhysicsExplosionRadius:
            enhanced_.physics_explosion_radius = clamped;
            enhanced_.physics_preset = "custom";
            break;
        case SettingId::PhysicsRagdollLaunch:
            enhanced_.physics_ragdoll_launch = clamped;
            enhanced_.physics_preset = "custom";
            break;
        case SettingId::PhysicsStructuralDestruction:
            enhanced_.physics_structural_destruction = clamped;
            enhanced_.physics_preset = "custom";
            break;
        case SettingId::PhysicsPropBounciness:
            enhanced_.physics_prop_bounciness = clamped;
            enhanced_.physics_preset = "custom";
            break;
        case SettingId::PhysicsCameraShockwave:
            enhanced_.physics_camera_shockwave = clamped;
            enhanced_.physics_preset = "custom";
            break;
        case SettingId::PhysicsTimeScale:
            enhanced_.physics_time_scale = clamped;
            enhanced_.physics_preset = "custom";
            break;
        default: return {false, m->scope, "Unwritable float binding"};
    }
    return {true, m->scope, ""};
}

ValidationResult SettingsRegistry::SetString(SettingId id, std::string_view val, app::Settings& /*s*/, game::Options& o) {
    const ModFeatureMeta* m = Find(id);
    if (!m) return {false, ApplyScope::Immediate, "Unknown setting identifier"};
    if (m->type != SettingType::String) return {false, m->scope, "Type mismatch (expected String)"};
    if (m->status != FeatureStatus::Implemented) {
        return {false, m->scope,
                m->status == FeatureStatus::Planned ? "Feature is Planned and locked" : "Feature is Unavailable"};
    }
    if (!m->bounds.allowed_strings.empty() && !ContainsString(m->bounds.allowed_strings, val)) {
        return {false, m->scope, "Value outside allowed whitelist"};
    }
    switch (id) {
        case SettingId::Language: o.language = std::string(val); break;
        case SettingId::InterfaceThemeAccent: enhanced_.theme_accent = std::string(val); break;
        case SettingId::PhysicsPreset: ApplyPhysicsPreset(val); break;
        default: return {false, m->scope, "Unwritable string binding"};
    }
    return {true, m->scope, ""};
}

void SettingsRegistry::SanitizeAll(app::Settings& s, game::Options& o, uint32_t& sanitized_count) {
    auto clamp_uint_field = [&](SettingId id, uint32_t& field) {
        const ModFeatureMeta* m = Find(id);
        if (!m) return;
        bool adj = false;
        field = ClampAndStepUint(field, m->bounds, adj);
        if (adj) sanitized_count++;
    };
    clamp_uint_field(SettingId::RenderPercent, s.render_percent);
    clamp_uint_field(SettingId::FpsCap, s.fps_cap);
    clamp_uint_field(SettingId::AnisotropicFiltering, s.aniso);
    clamp_uint_field(SettingId::AntiAliasing, o.anti_aliasing);
    clamp_uint_field(SettingId::ScreenFilter, o.screen_filter);
    clamp_uint_field(SettingId::DrawDistance, o.draw_distance);
    clamp_uint_field(SettingId::DynamicTargetFps, o.dynamic_target_fps);
    clamp_uint_field(SettingId::DynamicMinPercent, o.dynamic_min_percent);
    clamp_uint_field(SettingId::DynamicMaxPercent, o.dynamic_max_percent);
    clamp_uint_field(SettingId::DynamicHold, o.dynamic_hold);
    clamp_uint_field(SettingId::FmvScale, o.fmv_scale);
    clamp_uint_field(SettingId::AudioOutput, o.audio_output);
    clamp_uint_field(SettingId::Fov, o.fov);
    clamp_uint_field(SettingId::ViewmodelFov, o.viewmodel_fov);
    clamp_uint_field(SettingId::WalkSpeed, o.walk_speed);
    clamp_uint_field(SettingId::LeftDeadzone, o.left_deadzone);
    clamp_uint_field(SettingId::RightDeadzone, o.right_deadzone);
    clamp_uint_field(SettingId::RapidFire, o.rapid_fire);
    clamp_uint_field(SettingId::GameSpeed, o.game_speed);

    if (o.dynamic_min_percent > o.dynamic_max_percent) {
        o.dynamic_min_percent = o.dynamic_max_percent;
        sanitized_count++;
    }
}

LoadResult SettingsRegistry::LoadFromFile(const std::filesystem::path& path, app::Settings& s, game::Options& o,
                                          PresetStateMachine* presets) {
    ResetToDefaults(s, o);
    LoadResult res;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return res;
    res.file_existed = true;

    std::ifstream in(path, std::ios::binary);
    if (!in) return res;
    std::ostringstream buf;
    buf << in.rdbuf();
    return LoadFromString(buf.str(), s, o, presets);
}

LoadResult SettingsRegistry::LoadFromString(std::string_view toml_content, app::Settings& s, game::Options& o,
                                            PresetStateMachine* presets) {
    ResetToDefaults(s, o);
    LoadResult res;
    res.file_existed = true;

    if (toml_content.size() >= 3 &&
        static_cast<uint8_t>(toml_content[0]) == 0xEF &&
        static_cast<uint8_t>(toml_content[1]) == 0xBB &&
        static_cast<uint8_t>(toml_content[2]) == 0xBF) {
        toml_content.remove_prefix(3);
    }

    toml::table t;
    try {
        t = toml::parse(toml_content);
    } catch (...) {
        ResetToDefaults(s, o);
        res.parsed_ok = false;
        return res;
    }
    res.parsed_ok = true;

    // Schema migration checks
    int64_t prev_schema = t["schema_version"].value_or(int64_t(0));
    res.previous_schema_version = prev_schema > 0 ? uint32_t(prev_schema) : 0u;
    if (prev_schema < kCurrentSchemaVersion) res.migrated = true;

    int64_t defaults_ver = t["defaults"].value_or(int64_t(0));

    // Display mode & upstream root app settings
    if (const auto* n = t.get("display_mode")) {
        if (n->is_string()) {
            std::string m = n->as_string()->get();
            bool matched = false;
            for (uint32_t i = 0; i < 3; i++) {
                if (m == kDisplayModes[i]) {
                    s.display_mode = app::DisplayMode(i);
                    matched = true;
                }
            }
            if (!matched) res.sanitized_count++;
        } else {
            res.sanitized_count++;
        }
    }
    if (const auto* w = t.get("width")) {
        if (w->is_integer() && w->as_integer()->get() >= 0) s.width = uint32_t(w->as_integer()->get());
        else res.sanitized_count++;
    }
    if (const auto* h = t.get("height")) {
        if (h->is_integer() && h->as_integer()->get() >= 0) s.height = uint32_t(h->as_integer()->get());
        else res.sanitized_count++;
    }
    s.join_ip = t["join_ip"].value_or(s.join_ip);

    auto load_uint = [&](SettingId id, uint32_t& dst) {
        const ModFeatureMeta* m = Find(id);
        if (!m) return;
        const toml::table* src = m->section ? t[m->section].as_table() : &t;
        if (!src) return;
        dst = ReadValidatedUint(src->get(m->key), dst, m->bounds, res.sanitized_count);
    };
    auto load_bool = [&](SettingId id, bool& dst) {
        const ModFeatureMeta* m = Find(id);
        if (!m) return;
        const toml::table* src = m->section ? t[m->section].as_table() : &t;
        if (!src) return;
        dst = ReadValidatedBool(src->get(m->key), dst, res.sanitized_count);
    };
    auto load_float = [&](SettingId id, float& dst) {
        const ModFeatureMeta* m = Find(id);
        if (!m) return;
        const toml::table* src = m->section ? t[m->section].as_table() : &t;
        if (!src) return;
        dst = ReadValidatedFloat(src->get(m->key), dst, m->bounds, res.sanitized_count);
    };
    auto load_str = [&](SettingId id, std::string& dst) {
        const ModFeatureMeta* m = Find(id);
        if (!m) return;
        const toml::table* src = m->section ? t[m->section].as_table() : &t;
        if (!src) return;
        dst = ReadValidatedString(src->get(m->key), dst, m->bounds, res.sanitized_count);
    };

    // Root settings
    load_uint(SettingId::RenderPercent, s.render_percent);
    load_uint(SettingId::FpsCap, s.fps_cap);
    load_bool(SettingId::Vsync, s.vsync);
    load_uint(SettingId::AnisotropicFiltering, s.aniso);

    // Apply legacy PhoneFrameDefaults if defaults < 2 (matches native/main.cpp line 192)
    if (defaults_ver < kCurrentDefaultsVersion) {
        s.fps_cap = 0;
        s.vsync = false;
        res.migrated = true;
    }

    load_uint(SettingId::AntiAliasing, o.anti_aliasing);
    load_uint(SettingId::ScreenFilter, o.screen_filter);
    load_bool(SettingId::Bloom, o.bloom);
    load_bool(SettingId::Fog, o.fog);
    load_bool(SettingId::Hdr, o.hdr);
    load_bool(SettingId::ReloadBlur, o.reload_blur);
    load_uint(SettingId::DrawDistance, o.draw_distance);
    load_bool(SettingId::DynamicResolution, o.dynamic_resolution);
    load_uint(SettingId::DynamicTargetFps, o.dynamic_target_fps);
    load_uint(SettingId::DynamicMinPercent, o.dynamic_min_percent);
    load_uint(SettingId::DynamicMaxPercent, o.dynamic_max_percent);
    load_uint(SettingId::DynamicHold, o.dynamic_hold);
    load_uint(SettingId::FmvScale, o.fmv_scale);
    load_bool(SettingId::SkipIntros, o.skip_intros);
    load_bool(SettingId::FpsCounter, o.fps_counter);
    load_uint(SettingId::AudioOutput, o.audio_output);
    load_str(SettingId::Language, o.language);

    load_uint(SettingId::Fov, o.fov);
    load_uint(SettingId::ViewmodelFov, o.viewmodel_fov);
    load_uint(SettingId::WalkSpeed, o.walk_speed);
    load_bool(SettingId::HoldAim, o.hold_aim);
    load_bool(SettingId::InvertMouse, o.invert_mouse);
    load_float(SettingId::MouseSensitivity, o.mouse_sensitivity);
    if (!t.contains("touch_sensitivity")) o.touch_sensitivity = o.mouse_sensitivity;
    load_float(SettingId::TouchSensitivity, o.touch_sensitivity);
    load_uint(SettingId::LeftDeadzone, o.left_deadzone);
    load_uint(SettingId::RightDeadzone, o.right_deadzone);
    load_float(SettingId::LeftSensitivity, o.left_sensitivity);
    load_float(SettingId::RightSensitivity, o.right_sensitivity);

    load_bool(SettingId::InfiniteAmmo, o.infinite_ammo);
    load_uint(SettingId::RapidFire, o.rapid_fire);
    load_bool(SettingId::NoRecoil, o.no_recoil);
    load_uint(SettingId::GameSpeed, o.game_speed);
    load_bool(SettingId::GodMode, o.god_mode);
    load_bool(SettingId::OneHitKill, o.one_hit_kill);
    load_bool(SettingId::InfiniteGrenades, o.infinite_grenades);
    load_bool(SettingId::UnlockLevels, o.unlock_levels);
    load_bool(SettingId::UnlockBlackOps, o.unlock_black_ops);
    load_bool(SettingId::Esp, o.esp);
    load_bool(SettingId::Aimbot, o.aimbot);

    if (const toml::table* keys = t["keys"].as_table()) {
        for (auto&& [k, val] : *keys) {
            if (auto sv = val.value<std::string>()) o.keys[std::string(k.str())] = *sv;
        }
    }

    // Load namespaced Enhanced Edition sections (safe & validated even when Planned/locked at runtime)
    load_uint(SettingId::GraphicsToneMapping, enhanced_.tone_mapping);
    load_bool(SettingId::GraphicsCasSharpenEnabled, enhanced_.cas_sharpen_enabled);
    load_uint(SettingId::GraphicsCasSharpenStrength, enhanced_.cas_sharpen_strength);
    load_uint(SettingId::GraphicsBloomIntensity, enhanced_.bloom_intensity);
    load_float(SettingId::GraphicsTextureLodBias, enhanced_.texture_lod_bias);
    load_bool(SettingId::GraphicsTrilinearFiltering, enhanced_.trilinear_filtering);
    load_uint(SettingId::GraphicsCinematicGrade, enhanced_.cinematic_grade);
    load_uint(SettingId::GraphicsLensVignette, enhanced_.lens_vignette);
    load_uint(SettingId::GraphicsFilmGrain, enhanced_.film_grain);
    load_bool(SettingId::GraphicsBlastLensPulse, enhanced_.blast_lens_pulse);
    load_uint(SettingId::GraphicsSoftShadows, enhanced_.soft_shadows);
    load_uint(SettingId::GraphicsAmbientOcclusion, enhanced_.ambient_occlusion);
    load_bool(SettingId::GraphicsHdTexturesEnabled, enhanced_.hd_textures_enabled);

    load_bool(SettingId::CombatScreenDamageVignette, enhanced_.screen_damage_vignette);
    load_uint(SettingId::CombatDamageVignetteIntensity, enhanced_.damage_vignette_intensity);
    load_bool(SettingId::CombatLowHealthDesaturation, enhanced_.low_health_desaturation);
    load_uint(SettingId::CombatExplosionGlowBoost, enhanced_.explosion_glow_boost);
    load_uint(SettingId::CombatParticleFadeScale, enhanced_.particle_fade_scale);
    load_uint(SettingId::CombatBloodParticleMultiplier, enhanced_.blood_particle_multiplier);
    load_bool(SettingId::CombatPersistentBloodDecals, enhanced_.persistent_blood_decals);
    load_uint(SettingId::CombatMaxBloodDecals, enhanced_.max_blood_decals);
    load_uint(SettingId::CombatCorpsePersistenceMode, enhanced_.corpse_persistence_mode);
    load_bool(SettingId::CombatDiverseBulletImpacts, enhanced_.diverse_bullet_impacts);

    load_bool(SettingId::CharactersRagdoll60Fps, enhanced_.ragdoll_60fps_physics);
    load_uint(SettingId::CharactersLodScale, enhanced_.character_lod_scale);
    load_bool(SettingId::CharactersCustomModels, enhanced_.custom_character_models);

    load_bool(SettingId::AudioCombatDynamicBoost, enhanced_.combat_dynamic_boost);
    load_uint(SettingId::AudioMasterGainPercent, enhanced_.master_gain_percent);
    load_bool(SettingId::AudioModPackEnabled, enhanced_.mod_audio_pack_enabled);

    load_bool(SettingId::ArabicUiEnabled, enhanced_.arabic_ui_enabled);
    load_bool(SettingId::ArabicRtlMirror, enhanced_.arabic_rtl_mirror);
    load_bool(SettingId::ArabicSubtitlesOverlay, enhanced_.arabic_subtitles_overlay);
    load_bool(SettingId::ArabicFlashMenus, enhanced_.arabic_flash_menus);
    load_bool(SettingId::ArabicVoicePack, enhanced_.arabic_voice_pack);

    load_bool(SettingId::GameplayEnhancedRecoil, enhanced_.enhanced_recoil_feedback);
    load_bool(SettingId::GameplayDisableAimAssist, enhanced_.disable_aim_assist);

    load_str(SettingId::InterfaceThemeAccent, enhanced_.theme_accent);
    load_bool(SettingId::InterfaceAdvancedPerfHud, enhanced_.advanced_perf_hud);
    load_bool(SettingId::InterfaceShowRestartBanners, enhanced_.show_restart_banners);
    load_bool(SettingId::InterfaceHideHudOnScreenshots, enhanced_.hide_hud_on_screenshots);
    load_bool(SettingId::PackagesOverlayEnabled, enhanced_.packages_overlay_enabled);

    load_str(SettingId::PhysicsPreset, enhanced_.physics_preset);
    load_float(SettingId::PhysicsExplosionForce, enhanced_.physics_explosion_force);
    load_float(SettingId::PhysicsImpactForce, enhanced_.physics_impact_force);
    load_float(SettingId::PhysicsGravityScale, enhanced_.physics_gravity_scale);
    load_float(SettingId::PhysicsDebrisIntensity, enhanced_.physics_debris_intensity);
    load_float(SettingId::PhysicsCharacterReaction, enhanced_.physics_character_reaction);
    load_float(SettingId::PhysicsWeaponRecoil, enhanced_.physics_weapon_recoil);
    load_float(SettingId::PhysicsExplosionRadius, enhanced_.physics_explosion_radius);
    load_float(SettingId::PhysicsRagdollLaunch, enhanced_.physics_ragdoll_launch);
    load_float(SettingId::PhysicsStructuralDestruction, enhanced_.physics_structural_destruction);
    load_float(SettingId::PhysicsPropBounciness, enhanced_.physics_prop_bounciness);
    load_float(SettingId::PhysicsCameraShockwave, enhanced_.physics_camera_shockwave);
    load_float(SettingId::PhysicsTimeScale, enhanced_.physics_time_scale);

    // Legacy -> Enhanced migration helpers when enhanced keys were absent
    const toml::table* gfx = t["graphics"].as_table();
    if ((!gfx || !gfx->contains("tone_mapping")) && o.hdr) enhanced_.tone_mapping = 1;
    if ((!gfx || !gfx->contains("bloom_intensity")) && !o.bloom) enhanced_.bloom_intensity = 0;

    // Cross-field validation (e.g., min <= max)
    SanitizeAll(s, o, res.sanitized_count);

    // Preserve unknown root & subtable keys so custom/future fields survive save round-trips
    unknown_nodes_ = toml::table{};
    for (auto&& [k, node] : t) {
        std::string_view key_sv = k.str();
        if (IsKnownRootKey(key_sv)) {
            if (node.is_table() && key_sv != "keys" && key_sv != "presets") {
                const toml::table* sub = node.as_table();
                toml::table unknown_sub;
                for (auto&& [sk, snode] : *sub) {
                    if (!FindByKey(sk.str(), key_sv)) unknown_sub.insert(sk, snode);
                }
                if (!unknown_sub.empty()) unknown_nodes_.insert(key_sv, unknown_sub);
            }
            continue;
        }
        if (!FindByKey(key_sv, "")) {
            unknown_nodes_.insert(key_sv, node);
        }
    }

    // Load preset state & snapshot
    if (presets) {
        if (const toml::table* pt = t["presets"].as_table()) presets->Deserialize(*pt);
        else presets->Bind(*this, s, o);
    }

    return res;
}

toml::table SettingsRegistry::SaveToTable(const app::Settings& s, const game::Options& o,
                                          const PresetStateMachine* presets) const {
    // Start with preserved unknown root/subtable keys
    toml::table out = unknown_nodes_;

    out.insert_or_assign("schema_version", kCurrentSchemaVersion);
    out.insert_or_assign("defaults", kCurrentDefaultsVersion);
    out.insert_or_assign("display_mode", kDisplayModes[std::min(uint32_t(s.display_mode), 2u)]);
    out.insert_or_assign("width", int64_t(s.width));
    out.insert_or_assign("height", int64_t(s.height));
    out.insert_or_assign("render_percent", int64_t(s.render_percent));
    out.insert_or_assign("fps_cap", int64_t(s.fps_cap));
    out.insert_or_assign("vsync", s.vsync);
    out.insert_or_assign("anisotropic_filtering", int64_t(s.aniso));
    out.insert_or_assign("join_ip", s.join_ip);

    // Flat legacy game options
    out.insert_or_assign("skip_intros", o.skip_intros);
    out.insert_or_assign("fps_counter", o.fps_counter);
    out.insert_or_assign("fmv_scale", int64_t(o.fmv_scale));
    out.insert_or_assign("screen_filter", int64_t(o.screen_filter));
    out.insert_or_assign("reload_blur", o.reload_blur);
    out.insert_or_assign("bloom", o.bloom);
    out.insert_or_assign("fog", o.fog);
    out.insert_or_assign("anti_aliasing", int64_t(o.anti_aliasing));
    out.insert_or_assign("hdr", o.hdr);
    out.insert_or_assign("draw_distance", int64_t(o.draw_distance));
    out.insert_or_assign("dynamic_resolution", o.dynamic_resolution);
    out.insert_or_assign("dynamic_target_fps", int64_t(o.dynamic_target_fps));
    out.insert_or_assign("dynamic_min_percent", int64_t(o.dynamic_min_percent));
    out.insert_or_assign("dynamic_max_percent", int64_t(o.dynamic_max_percent));
    out.insert_or_assign("dynamic_hold", int64_t(o.dynamic_hold));
    out.insert_or_assign("audio_output", int64_t(o.audio_output));
    out.insert_or_assign("language", o.language);

    out.insert_or_assign("fov", int64_t(o.fov));
    out.insert_or_assign("viewmodel_fov", int64_t(o.viewmodel_fov));
    out.insert_or_assign("walk_speed", int64_t(o.walk_speed));
    out.insert_or_assign("hold_aim", o.hold_aim);
    out.insert_or_assign("invert_mouse", o.invert_mouse);
    out.insert_or_assign("mouse_sensitivity", double(o.mouse_sensitivity));
    out.insert_or_assign("touch_sensitivity", double(o.touch_sensitivity));
    out.insert_or_assign("left_stick_deadzone", int64_t(o.left_deadzone));
    out.insert_or_assign("right_stick_deadzone", int64_t(o.right_deadzone));
    out.insert_or_assign("left_stick_sensitivity", double(o.left_sensitivity));
    out.insert_or_assign("right_stick_sensitivity", double(o.right_sensitivity));

    out.insert_or_assign("infinite_ammo", o.infinite_ammo);
    out.insert_or_assign("rapid_fire", int64_t(o.rapid_fire));
    out.insert_or_assign("no_recoil", o.no_recoil);
    out.insert_or_assign("game_speed", int64_t(o.game_speed));
    out.insert_or_assign("god_mode", o.god_mode);
    out.insert_or_assign("one_hit_kill", o.one_hit_kill);
    out.insert_or_assign("infinite_grenades", o.infinite_grenades);
    out.insert_or_assign("unlock_levels", o.unlock_levels);
    out.insert_or_assign("unlock_black_ops", o.unlock_black_ops);
    out.insert_or_assign("esp", o.esp);
    out.insert_or_assign("aimbot", o.aimbot);

    toml::table keys_tbl;
    for (const auto& [id, val] : o.keys) keys_tbl.insert_or_assign(id, val);
    out.insert_or_assign("keys", keys_tbl);

    // Helper to merge known keys into subtable while keeping unknown subkeys intact
    auto get_or_make_sub = [&](const char* sec) -> toml::table {
        if (const auto* existing = out[sec].as_table()) return *existing;
        return toml::table{};
    };

    {
        toml::table gfx = get_or_make_sub("graphics");
        gfx.insert_or_assign("tone_mapping", int64_t(enhanced_.tone_mapping));
        gfx.insert_or_assign("cas_sharpen_enabled", enhanced_.cas_sharpen_enabled);
        gfx.insert_or_assign("cas_sharpen_strength", int64_t(enhanced_.cas_sharpen_strength));
        gfx.insert_or_assign("bloom_intensity", int64_t(enhanced_.bloom_intensity));
        gfx.insert_or_assign("texture_lod_bias", double(enhanced_.texture_lod_bias));
        gfx.insert_or_assign("trilinear_filtering", enhanced_.trilinear_filtering);
        gfx.insert_or_assign("cinematic_grade", int64_t(enhanced_.cinematic_grade));
        gfx.insert_or_assign("lens_vignette", int64_t(enhanced_.lens_vignette));
        gfx.insert_or_assign("film_grain", int64_t(enhanced_.film_grain));
        gfx.insert_or_assign("blast_lens_pulse", enhanced_.blast_lens_pulse);
        gfx.insert_or_assign("soft_shadows", int64_t(enhanced_.soft_shadows));
        gfx.insert_or_assign("ambient_occlusion", int64_t(enhanced_.ambient_occlusion));
        gfx.insert_or_assign("hd_texture_packs", enhanced_.hd_textures_enabled);
        out.insert_or_assign("graphics", gfx);
    }

    {
        toml::table cbt = get_or_make_sub("combat");
        cbt.insert_or_assign("screen_damage_vignette", enhanced_.screen_damage_vignette);
        cbt.insert_or_assign("damage_vignette_intensity", int64_t(enhanced_.damage_vignette_intensity));
        cbt.insert_or_assign("low_health_desaturation", enhanced_.low_health_desaturation);
        cbt.insert_or_assign("explosion_glow_boost", int64_t(enhanced_.explosion_glow_boost));
        cbt.insert_or_assign("particle_fade_scale", int64_t(enhanced_.particle_fade_scale));
        cbt.insert_or_assign("blood_particle_multiplier", int64_t(enhanced_.blood_particle_multiplier));
        cbt.insert_or_assign("persistent_blood_decals", enhanced_.persistent_blood_decals);
        cbt.insert_or_assign("max_blood_decals", int64_t(enhanced_.max_blood_decals));
        cbt.insert_or_assign("corpse_persistence_mode", int64_t(enhanced_.corpse_persistence_mode));
        cbt.insert_or_assign("diverse_bullet_impacts", enhanced_.diverse_bullet_impacts);
        out.insert_or_assign("combat", cbt);
    }

    {
        toml::table ch = get_or_make_sub("characters");
        ch.insert_or_assign("ragdoll_60fps_physics", enhanced_.ragdoll_60fps_physics);
        ch.insert_or_assign("character_lod_scale", int64_t(enhanced_.character_lod_scale));
        ch.insert_or_assign("custom_character_models", enhanced_.custom_character_models);
        out.insert_or_assign("characters", ch);
    }

    {
        toml::table au = get_or_make_sub("audio");
        au.insert_or_assign("combat_dynamic_boost", enhanced_.combat_dynamic_boost);
        au.insert_or_assign("master_gain_percent", int64_t(enhanced_.master_gain_percent));
        au.insert_or_assign("mod_audio_pack_enabled", enhanced_.mod_audio_pack_enabled);
        out.insert_or_assign("audio", au);
    }

    {
        toml::table ar = get_or_make_sub("arabic");
        ar.insert_or_assign("arabic_ui_enabled", enhanced_.arabic_ui_enabled);
        ar.insert_or_assign("arabic_rtl_mirror", enhanced_.arabic_rtl_mirror);
        ar.insert_or_assign("arabic_subtitles_overlay", enhanced_.arabic_subtitles_overlay);
        ar.insert_or_assign("arabic_flash_menus", enhanced_.arabic_flash_menus);
        ar.insert_or_assign("arabic_voice_pack", enhanced_.arabic_voice_pack);
        out.insert_or_assign("arabic", ar);
    }

    {
        toml::table gp = get_or_make_sub("gameplay");
        gp.insert_or_assign("enhanced_recoil_feedback", enhanced_.enhanced_recoil_feedback);
        gp.insert_or_assign("disable_aim_assist", enhanced_.disable_aim_assist);
        out.insert_or_assign("gameplay", gp);
    }

    {
        toml::table ui = get_or_make_sub("interface");
        ui.insert_or_assign("settings_theme_accent", enhanced_.theme_accent);
        ui.insert_or_assign("advanced_perf_hud", enhanced_.advanced_perf_hud);
        ui.insert_or_assign("show_restart_banners", enhanced_.show_restart_banners);
        ui.insert_or_assign("hide_hud_on_screenshots", enhanced_.hide_hud_on_screenshots);
        out.insert_or_assign("interface", ui);
    }

    {
        toml::table pl = get_or_make_sub("physics_lab");
        pl.insert_or_assign("preset", enhanced_.physics_preset);
        pl.insert_or_assign("explosion_force", double(enhanced_.physics_explosion_force));
        pl.insert_or_assign("impact_force", double(enhanced_.physics_impact_force));
        pl.insert_or_assign("gravity_scale", double(enhanced_.physics_gravity_scale));
        pl.insert_or_assign("debris_intensity", double(enhanced_.physics_debris_intensity));
        pl.insert_or_assign("character_reaction", double(enhanced_.physics_character_reaction));
        pl.insert_or_assign("weapon_recoil", double(enhanced_.physics_weapon_recoil));
        pl.insert_or_assign("explosion_radius", double(enhanced_.physics_explosion_radius));
        pl.insert_or_assign("ragdoll_launch", double(enhanced_.physics_ragdoll_launch));
        pl.insert_or_assign("structural_destruction", double(enhanced_.physics_structural_destruction));
        pl.insert_or_assign("prop_bounciness", double(enhanced_.physics_prop_bounciness));
        pl.insert_or_assign("camera_shockwave", double(enhanced_.physics_camera_shockwave));
        pl.insert_or_assign("physics_time_scale", double(enhanced_.physics_time_scale));
        out.insert_or_assign("physics_lab", pl);
    }

    if (presets) {
        toml::table pt = get_or_make_sub("presets");
        presets->Serialize(pt);
        out.insert_or_assign("presets", pt);
    }

    return out;
}

bool SettingsRegistry::SaveToFile(const std::filesystem::path& path, const app::Settings& s, const game::Options& o,
                                  const PresetStateMachine* presets) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);

    toml::table tbl = SaveToTable(s, o, presets);
    std::filesystem::path tmp = path;
    tmp += ".tmp";

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << tbl << "\n";
        f.flush();
        if (!f.good()) return false;
    }

    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::error_code ec2;
        std::filesystem::remove(path, ec2);
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec2);
            return false;
        }
    }
    return true;
}

void SettingsRegistry::CaptureBootSnapshot(const app::Settings& /*s*/, const game::Options& o) {
    boot_snapshot_captured_ = true;
    boot_language_ = o.language;
    boot_arabic_ui_ = enhanced_.arabic_ui_enabled;
    boot_arabic_subtitles_ = enhanced_.arabic_subtitles_overlay;
    boot_packages_overlay_ = enhanced_.packages_overlay_enabled;
}

uint32_t SettingsRegistry::PendingRestartCount(const app::Settings& /*s*/, const game::Options& o) const {
    if (!boot_snapshot_captured_) return 0;
    uint32_t count = 0;
    if (o.language != boot_language_) count++;
    if (enhanced_.arabic_ui_enabled != boot_arabic_ui_) count++;
    if (enhanced_.arabic_subtitles_overlay != boot_arabic_subtitles_) count++;
    if (enhanced_.packages_overlay_enabled != boot_packages_overlay_) count++;
    return count;
}

bool SettingsRegistry::RequiresAppRestart(const app::Settings& s, const game::Options& o) const {
    return PendingRestartCount(s, o) > 0;
}

std::string SettingsRegistry::BootValueString(SettingId id) const {
    switch (id) {
        case SettingId::Language: return boot_language_;
        case SettingId::ArabicUiEnabled: return boot_arabic_ui_ ? "true" : "false";
        case SettingId::ArabicSubtitlesOverlay: return boot_arabic_subtitles_ ? "true" : "false";
        case SettingId::PackagesOverlayEnabled: return boot_packages_overlay_ ? "true" : "false";
        default: return "";
    }
}

bool SettingsRegistry::ApplyPhysicsPreset(std::string_view preset_name) {
    if (preset_name == "classic") {
        enhanced_.physics_preset = "classic";
        enhanced_.physics_explosion_force = 1.00f;
        enhanced_.physics_impact_force = 1.00f;
        enhanced_.physics_gravity_scale = 1.00f;
        enhanced_.physics_debris_intensity = 1.00f;
        enhanced_.physics_character_reaction = 1.00f;
        enhanced_.physics_weapon_recoil = 1.00f;
        enhanced_.physics_explosion_radius = 1.00f;
        enhanced_.physics_ragdoll_launch = 1.00f;
        enhanced_.physics_structural_destruction = 1.00f;
        enhanced_.physics_prop_bounciness = 1.00f;
        enhanced_.physics_camera_shockwave = 1.00f;
        enhanced_.physics_time_scale = 1.00f;
        enhanced_.particle_fade_scale = 100;
        enhanced_.explosion_glow_boost = 100;
        return true;
    }
    if (preset_name == "realistic") {
        enhanced_.physics_preset = "realistic";
        enhanced_.physics_explosion_force = 1.50f;
        enhanced_.physics_impact_force = 1.60f;
        enhanced_.physics_gravity_scale = 1.15f;
        enhanced_.physics_debris_intensity = 1.50f;
        enhanced_.physics_character_reaction = 1.40f;
        enhanced_.physics_weapon_recoil = 1.20f;
        enhanced_.physics_explosion_radius = 1.25f;
        enhanced_.physics_ragdoll_launch = 1.35f;
        enhanced_.physics_structural_destruction = 1.50f;
        enhanced_.physics_prop_bounciness = 1.25f;
        enhanced_.physics_camera_shockwave = 1.30f;
        enhanced_.physics_time_scale = 1.00f;
        enhanced_.particle_fade_scale = 150;
        enhanced_.explosion_glow_boost = 135;
        return true;
    }
    if (preset_name == "cinematic") {
        enhanced_.physics_preset = "cinematic";
        enhanced_.physics_explosion_force = 3.20f;
        enhanced_.physics_impact_force = 2.80f;
        enhanced_.physics_gravity_scale = 0.75f;
        enhanced_.physics_debris_intensity = 3.00f;
        enhanced_.physics_character_reaction = 2.60f;
        enhanced_.physics_weapon_recoil = 1.35f;
        enhanced_.physics_explosion_radius = 1.85f;
        enhanced_.physics_ragdoll_launch = 2.80f;
        enhanced_.physics_structural_destruction = 2.50f;
        enhanced_.physics_prop_bounciness = 2.20f;
        enhanced_.physics_camera_shockwave = 1.85f;
        enhanced_.physics_time_scale = 0.85f;
        enhanced_.particle_fade_scale = 300;
        enhanced_.explosion_glow_boost = 220;
        return true;
    }
    if (preset_name == "mayhem") {
        enhanced_.physics_preset = "mayhem";
        enhanced_.physics_explosion_force = 7.50f;
        enhanced_.physics_impact_force = 6.00f;
        enhanced_.physics_gravity_scale = 0.45f;
        enhanced_.physics_debris_intensity = 5.00f;
        enhanced_.physics_character_reaction = 5.50f;
        enhanced_.physics_weapon_recoil = 0.50f;
        enhanced_.physics_explosion_radius = 3.00f;
        enhanced_.physics_ragdoll_launch = 6.50f;
        enhanced_.physics_structural_destruction = 6.00f;
        enhanced_.physics_prop_bounciness = 4.00f;
        enhanced_.physics_camera_shockwave = 2.50f;
        enhanced_.physics_time_scale = 1.00f;
        enhanced_.particle_fade_scale = 600;
        enhanced_.explosion_glow_boost = 350;
        return true;
    }
    if (preset_name == "custom") {
        enhanced_.physics_preset = "custom";
        return true;
    }
    return false;
}

void SettingsRegistry::ResetPhysicsControl(SettingId id) {
    switch (id) {
        case SettingId::PhysicsExplosionForce: enhanced_.physics_explosion_force = 1.00f; break;
        case SettingId::PhysicsImpactForce: enhanced_.physics_impact_force = 1.00f; break;
        case SettingId::PhysicsGravityScale: enhanced_.physics_gravity_scale = 1.00f; break;
        case SettingId::PhysicsDebrisIntensity: enhanced_.physics_debris_intensity = 1.00f; break;
        case SettingId::PhysicsCharacterReaction: enhanced_.physics_character_reaction = 1.00f; break;
        case SettingId::PhysicsWeaponRecoil: enhanced_.physics_weapon_recoil = 1.00f; break;
        case SettingId::PhysicsExplosionRadius: enhanced_.physics_explosion_radius = 1.00f; break;
        case SettingId::PhysicsRagdollLaunch: enhanced_.physics_ragdoll_launch = 1.00f; break;
        case SettingId::PhysicsStructuralDestruction: enhanced_.physics_structural_destruction = 1.00f; break;
        case SettingId::PhysicsPropBounciness: enhanced_.physics_prop_bounciness = 1.00f; break;
        case SettingId::PhysicsCameraShockwave: enhanced_.physics_camera_shockwave = 1.00f; break;
        case SettingId::PhysicsTimeScale: enhanced_.physics_time_scale = 1.00f; break;
        default: return;
    }
    constexpr float kEps = 1e-4f;
    const bool all_classic =
        std::fabs(enhanced_.physics_explosion_force - 1.00f) < kEps &&
        std::fabs(enhanced_.physics_impact_force - 1.00f) < kEps &&
        std::fabs(enhanced_.physics_gravity_scale - 1.00f) < kEps &&
        std::fabs(enhanced_.physics_debris_intensity - 1.00f) < kEps &&
        std::fabs(enhanced_.physics_character_reaction - 1.00f) < kEps &&
        std::fabs(enhanced_.physics_weapon_recoil - 1.00f) < kEps &&
        std::fabs(enhanced_.physics_explosion_radius - 1.00f) < kEps &&
        std::fabs(enhanced_.physics_ragdoll_launch - 1.00f) < kEps &&
        std::fabs(enhanced_.physics_structural_destruction - 1.00f) < kEps &&
        std::fabs(enhanced_.physics_prop_bounciness - 1.00f) < kEps &&
        std::fabs(enhanced_.physics_camera_shockwave - 1.00f) < kEps &&
        std::fabs(enhanced_.physics_time_scale - 1.00f) < kEps;
    enhanced_.physics_preset = all_classic ? "classic" : "custom";
}

}  // namespace modsettings
