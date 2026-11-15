#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "common/options.h"

namespace modsettings {

// Schema versioning: 1 = legacy flat upstream, 2 = Enhanced Edition modular schema, 3 = Audio Remaster default-on migration
inline constexpr int64_t kCurrentSchemaVersion = 3;
inline constexpr int64_t kCurrentDefaultsVersion = 2;

enum class Category : uint8_t {
    Graphics,
    Combat,
    Characters,
    Audio,
    Arabic,
    Gameplay,
    Interface,
    Controls,
    Cheats,
    PhysicsLab,
    Ballistics,
    General
};

enum class FeatureStatus : uint8_t {
    Implemented,  // Currently active and wired in host or guest recompilation code
    Planned,      // Designed in architecture; locked (non-interactive) in UI
    Unavailable   // Requires missing hardware capability or uninstalled asset package
};

enum class ApplyScope : uint8_t {
    Immediate,     // Takes effect on the next rendered frame or audio tick
    ShaderRebuild, // Triggers shader pipeline/variant update without full app restart
    SceneReload,   // Takes effect on next level load / mission reset
    AppRestart     // Latched at startup; requires app process restart
};

enum class SettingType : uint8_t {
    Bool,
    Uint,
    Float,
    String
};

enum class SettingId : uint32_t {
    // --- Legacy root settings (preserved 1:1) ---
    DisplayMode,
    RenderPercent,
    FpsCap,
    Vsync,
    AnisotropicFiltering,
    AntiAliasing,
    ScreenFilter,
    Bloom,
    Fog,
    Hdr,
    ReloadBlur,
    DrawDistance,
    DynamicResolution,
    DynamicTargetFps,
    DynamicMinPercent,
    DynamicMaxPercent,
    DynamicHold,
    FmvScale,
    SkipIntros,
    FpsCounter,
    AudioOutput,
    Language,
    Fov,
    ViewmodelFov,
    WalkSpeed,
    HoldAim,
    InvertMouse,
    MouseSensitivity,
    TouchSensitivity,
    LeftDeadzone,
    RightDeadzone,
    LeftSensitivity,
    RightSensitivity,
    InfiniteAmmo,
    RapidFire,
    NoRecoil,
    GameSpeed,
    GodMode,
    OneHitKill,
    InfiniteGrenades,
    UnlockLevels,
    UnlockBlackOps,
    Esp,
    Aimbot,

    // --- Enhanced Category 1: Graphics ---
    GraphicsToneMapping,
    GraphicsCasSharpenEnabled,
    GraphicsCasSharpenStrength,
    GraphicsBloomIntensity,
    GraphicsTextureLodBias,
    GraphicsTrilinearFiltering,
    GraphicsCinematicGrade,
    GraphicsLensVignette,
    GraphicsFilmGrain,
    GraphicsBlastLensPulse,
    GraphicsSoftShadows,
    GraphicsAmbientOcclusion,
    GraphicsHdTexturesEnabled,
    GraphicsWeaponReflections,

    // --- Enhanced Category 2: Combat & Blood ---
    CombatScreenDamageVignette,
    CombatDamageVignetteIntensity,
    CombatLowHealthDesaturation,
    CombatExplosionGlowBoost,
    CombatParticleFadeScale,
    CombatBloodParticleMultiplier,
    CombatPersistentBloodDecals,
    CombatMaxBloodDecals,
    CombatCorpsePersistenceMode,
    CombatDiverseBulletImpacts,
    CombatHitmarkers,
    CombatShellCasings,

    // --- Enhanced Category 3: Characters ---
    CharactersRagdoll60Fps,
    CharactersLodScale,
    CharactersCustomModels,

    // --- Enhanced Category 4: Audio ---
    AudioCombatDynamicBoost,
    AudioMasterGainPercent,
    AudioModPackEnabled,
    AudioWeaponVolume,
    AudioMechanicalVolume,
    AudioReloadVolume,
    AudioEnvironmentVolume,
    AudioTailIntensity,
    AudioEchoIntensity,
    AudioDistantGunfire,
    AudioBulletFlyby,
    AudioVariationPercent,
    AudioPhoneVibration,

    // --- Enhanced Category 5: Arabic Localization ---
    ArabicUiEnabled,
    ArabicRtlMirror,
    ArabicSubtitlesOverlay,
    ArabicFlashMenus,
    ArabicVoicePack,

    // --- Enhanced Category 6: Gameplay ---
    GameplayEnhancedRecoil,
    GameplayDisableAimAssist,
    GameplayHeadshotMultiplier,

    // --- Enhanced Category 7: Interface ---
    InterfaceThemeAccent,
    InterfaceAdvancedPerfHud,
    InterfaceShowRestartBanners,
    InterfaceHideHudOnScreenshots,

    // --- Package overlay master ---
    PackagesOverlayEnabled,

    // --- Enhanced Category 8: Physics Lab ---
    PhysicsPreset,
    PhysicsExplosionForce,
    PhysicsImpactForce,
    PhysicsGravityScale,
    PhysicsDebrisIntensity,
    PhysicsCharacterReaction,
    PhysicsWeaponRecoil,
    PhysicsExplosionRadius,
    PhysicsRagdollLaunch,
    PhysicsStructuralDestruction,
    PhysicsPropBounciness,
    PhysicsCameraShockwave,
    PhysicsTimeScale,
    PhysicsTimeStopEnabled,
    PhysicsTimeStopMode,
    PhysicsTimeStopMaxDuration,
    PhysicsTimeStopCooldown,
    PhysicsDeliriumEnabled,
    PhysicsDeliriumIntensity,
    PhysicsDeliriumBlindFire,
    PhysicsDeliriumAgonyDuration,

    // --- Enhanced Category 9: Ballistics & Tracers ---
    BallisticsPreset,
    BallisticsTracersEnabled,
    BallisticsTracerFrequency,
    BallisticsTracerTrailLength,
    BallisticsTracerBrightness,
    BallisticsTracerColor,
    BallisticsSmokeTrailsEnabled,
    BallisticsSmokeMode,
    BallisticsSmokeIntensity,
    BallisticsSmokeDuration,

    // --- Enhanced Category 10: Overdrive Protocol (Kinetic Dash, Special Ammo & Thermal Visor) ---
    OverdriveEnabled,
    OverdriveSiphonEnabled,
    OverdriveReflexSlowmo,
    KineticDashEnabled,
    KineticDashShockwave,
    SpecialAmmoMode,
    ThermalVisorEnabled
};

// Numeric validation bounds
struct NumericBounds {
    uint32_t uint_min = 0;
    uint32_t uint_max = 0;
    uint32_t uint_step = 1;
    float float_min = 0.0f;
    float float_max = 0.0f;
    float float_step = 0.1f;
    std::vector<uint32_t> allowed_uints;
    std::vector<std::string> allowed_strings;
};

struct ModFeatureMeta {
    SettingId id;
    const char* key;
    const char* section;  // nullptr for root legacy, or "graphics", "combat", etc.
    Category category;
    SettingType type;
    FeatureStatus status;
    ApplyScope scope;
    const char* label;
    const char* description;
    NumericBounds bounds;
};

// Extended options holding state for enhanced features
struct EnhancedOptions {
    // Graphics
    uint32_t tone_mapping = 0;               // 0=Original, 1=Reinhard HDR, 2=ACES Filmic
    bool cas_sharpen_enabled = false;
    uint32_t cas_sharpen_strength = 45;      // 0..100%
    uint32_t bloom_intensity = 100;          // 0..200%
    float texture_lod_bias = 0.0f;           // 0.0, -0.5, -1.0
    bool trilinear_filtering = false;
    uint32_t cinematic_grade = 0;            // 0=Off, 1=Blockbuster Teal&Orange, 2=Gritty Tactical, 3=Warzone HDR
    uint32_t lens_vignette = 0;              // 0..100% optical cinema corner framing
    uint32_t film_grain = 0;                 // 0..100% fine 35mm anti-banding film grain
    bool blast_lens_pulse = false;           // Explosion shockwave edge chromatic pulse & flash
    uint32_t soft_shadows = 0;               // 0=Off, 1=PCF 4-Tap, 2=PCF 9-Tap (Planned)
    uint32_t ambient_occlusion = 0;          // 0=Off, 1=Half-Res (Planned)
    bool hd_textures_enabled = false;        // (Planned)
    bool weapon_reflections = true;          // Specular sheen & Fresnel rim reflections on firearms

    // Combat
    bool screen_damage_vignette = false;
    uint32_t damage_vignette_intensity = 75; // 10..100%
    bool low_health_desaturation = false;
    uint32_t explosion_glow_boost = 100;     // 50..200%
    uint32_t particle_fade_scale = 100;      // 100..400%
    uint32_t blood_particle_multiplier = 2;  // 1..4x 3D blood burst & mist density
    bool persistent_blood_decals = true;     // 3D floor blood pools & splatters
    uint32_t max_blood_decals = 128;         // (Planned)
    uint32_t corpse_persistence_mode = 0;    // (Planned)
    bool diverse_bullet_impacts = true;      // 3D sparks & concrete/metal impact bursts
    bool hitmarker_feedback = true;          // Minimalist tactical hitmarker & kill confirm
    bool shell_casings = true;               // Physical 3D ejecting brass casings & floor clink

    // Characters
    bool ragdoll_60fps_physics = true;
    uint32_t character_lod_scale = 1;        // 1, 2, 4
    bool custom_character_models = false;    // (Planned/Unavailable)

    // Audio
    bool combat_dynamic_boost = false;
    uint32_t master_gain_percent = 100;         // 10..150%
    bool mod_audio_pack_enabled = true;         // Enhanced 48 kHz CC0 Weapon Audio Remaster
    uint32_t weapon_audio_volume = 100;         // 0..150%
    uint32_t mechanical_audio_volume = 100;     // 0..150%
    uint32_t reload_audio_volume = 100;         // 0..150%
    uint32_t environmental_audio_volume = 100;  // 0..150%
    uint32_t audio_tail_intensity = 100;        // 0..150% Gunshot Tail / Iraqi "Mattak" decay layer
    uint32_t audio_echo_intensity = 100;        // 0..150% Environmental indoor/outdoor reflections & reverb
    uint32_t audio_distant_gunfire = 100;       // 0..150% Far-field AI & distant battlefield gunfire
    uint32_t audio_bullet_flyby = 100;          // 0..150% Supersonic bullet cracks, whizzes & ricochet flybys
    uint32_t audio_variation_percent = 50;      // 0..100%
    bool phone_vibration = true;                // Logical per-weapon phone vibration feedback

    // Arabic
    bool arabic_ui_enabled = true;
    bool arabic_rtl_mirror = true;
    bool arabic_subtitles_overlay = true;
    bool arabic_flash_menus = false;         // (Planned)
    bool arabic_voice_pack = false;          // (Planned/Unavailable)

    // Gameplay
    bool enhanced_recoil_feedback = true;    // Tactile crisp recoil recovery curves & micro-kick
    bool disable_aim_assist = false;
    float headshot_multiplier = 2.0f;        // 1.0 .. 4.0x tactical headshot lethality

    // Interface
    std::string theme_accent = "brass";      // "brass" | "crimson" | "olive"
    bool advanced_perf_hud = false;
    bool show_restart_banners = true;
    bool hide_hud_on_screenshots = false;

    // Packages
    bool packages_overlay_enabled = false;

    // Physics Lab ("classic" = 1.0f 2006 baseline, "realistic" = default full-spectrum overhaul)
    std::string physics_preset = "realistic";  // "classic" | "realistic" | "cinematic" | "mayhem" | "custom"
    float physics_explosion_force = 2.20f;   // 0.25 .. 10.00
    float physics_impact_force = 2.10f;      // 0.25 .. 10.00
    float physics_gravity_scale = 1.15f;     // 0.05 .. 5.00
    float physics_debris_intensity = 2.25f;  // 0.25 .. 6.00
    float physics_character_reaction = 1.85f;// 0.25 .. 8.00
    float physics_weapon_recoil = 1.30f;     // 0.00 .. 4.00
    float physics_explosion_radius = 1.45f;  // 0.50 .. 5.00
    float physics_ragdoll_launch = 1.85f;    // 0.25 .. 10.00
    float physics_structural_destruction = 2.40f; // 0.25 .. 10.00
    float physics_prop_bounciness = 1.25f;   // 0.25 .. 6.00
    float physics_camera_shockwave = 1.50f;  // 0.00 .. 4.00
    float physics_time_scale = 1.00f;        // 0.20 .. 2.00
    bool time_stop_enabled = false;          // Live freeze / Dead Eye toggle
    uint32_t time_stop_mode = 0;             // 0 = Toggle, 1 = Hold
    float time_stop_max_duration = 0.0f;     // 0.0 = Unlimited .. 30.0s
    float time_stop_cooldown = 0.0f;         // 0.0 = None .. 30.0s
    bool delirium_physics_enabled = true;    // Delirium-inspired procedural stagger, clutching, & agony
    float delirium_intensity = 1.45f;        // 0.25 .. 3.00
    bool delirium_blind_fire = true;         // Off-balance erratic AI blind-fire
    float delirium_agony_duration = 1.25f;   // 0.00 .. 2.50 seconds

    // Ballistics & Bullet Tracers / Smoke Trails
    std::string ballistics_preset = "enhanced"; // "original" | "performance" | "enhanced" | "custom"
    bool tracers_enabled = true;             // Visible projectile & synchronized hitscan tracers
    uint32_t tracer_frequency = 1;           // 0=Off, 1=Every, 2=2nd, 3=3rd, 4=5th, 5=Selective
    uint32_t tracer_trail_length = 115;      // 25..250%
    uint32_t tracer_brightness = 110;        // 20..200%
    uint32_t tracer_color = 0;               // 0=Weapon-Real, 1=NATO Gold, 2=Crimson, 3=Green, 4=Ice-White
    bool smoke_trails_enabled = true;        // Independent weapon-path smoke trail toggle
    uint32_t smoke_mode = 2;                 // 0=Off, 1=Light, 2=Medium, 3=Enhanced
    uint32_t smoke_intensity = 105;          // 10..200%
    float smoke_duration = 1.10f;            // 0.20..3.00 seconds

    // Tactical Mobility & Gear (Kinetic Dash, Special Ammo, Thermal Visor; legacy Adrenaline/Shockwave disabled)
    bool overdrive_enabled = false;
    bool overdrive_siphon = false;
    bool overdrive_reflex_slowmo = false;
    bool kinetic_dash_enabled = true;
    bool kinetic_dash_shockwave = false;
    uint32_t special_ammo_mode = 0;          // 0=Standard, 1=Thermite, 2=Explosive HE, 3=Rail AP
    bool thermal_visor_enabled = false;
};

struct ValidationResult {
    bool accepted = false;
    ApplyScope scope = ApplyScope::Immediate;
    std::string rejection_reason;
};

struct LoadResult {
    bool file_existed = false;
    bool parsed_ok = false;
    bool migrated = false;
    uint32_t previous_schema_version = 0;
    uint32_t sanitized_count = 0;
};

// Forward declaration of PresetStateMachine
class PresetStateMachine;

class SettingsRegistry {
public:
    SettingsRegistry();

    const std::vector<ModFeatureMeta>& Definitions() const { return definitions_; }
    const ModFeatureMeta* Find(SettingId id) const;
    const ModFeatureMeta* FindByKey(std::string_view key, std::string_view section = "") const;

    // Access enhanced options
    EnhancedOptions& GetEnhanced() { return enhanced_; }
    const EnhancedOptions& GetEnhanced() const { return enhanced_; }

    // Physics Lab & Ballistics profiles + single-control reset
    bool ApplyPhysicsPreset(std::string_view preset_name);
    void ResetPhysicsControl(SettingId id);
    bool ApplyBallisticsPreset(std::string_view preset_name);

    // Safe getters
    bool GetBool(SettingId id, const app::Settings& s, const game::Options& o) const;
    uint32_t GetUint(SettingId id, const app::Settings& s, const game::Options& o) const;
    float GetFloat(SettingId id, const app::Settings& s, const game::Options& o) const;
    std::string GetString(SettingId id, const app::Settings& s, const game::Options& o) const;

    // Validated setters
    ValidationResult SetBool(SettingId id, bool val, app::Settings& s, game::Options& o);
    ValidationResult SetUint(SettingId id, uint32_t val, app::Settings& s, game::Options& o);
    ValidationResult SetFloat(SettingId id, float val, app::Settings& s, game::Options& o);
    ValidationResult SetString(SettingId id, std::string_view val, app::Settings& s, game::Options& o);

    // Default initialization
    void ResetToDefaults(app::Settings& s, game::Options& o);

    // Serialization & Deserialization
    LoadResult LoadFromFile(const std::filesystem::path& path, app::Settings& s, game::Options& o,
                            PresetStateMachine* presets = nullptr);
    LoadResult LoadFromString(std::string_view toml_content, app::Settings& s, game::Options& o,
                              PresetStateMachine* presets = nullptr);

    bool SaveToFile(const std::filesystem::path& path, const app::Settings& s, const game::Options& o,
                    const PresetStateMachine* presets = nullptr);
    toml::table SaveToTable(const app::Settings& s, const game::Options& o,
                            const PresetStateMachine* presets = nullptr) const;

    // Boot snapshot & restart-pending management
    void CaptureBootSnapshot(const app::Settings& s, const game::Options& o);
    bool RequiresAppRestart(const app::Settings& s, const game::Options& o) const;
    uint32_t PendingRestartCount(const app::Settings& s, const game::Options& o) const;
    std::string BootValueString(SettingId id) const;

private:
    void RegisterAllDefinitions();
    void SanitizeAll(app::Settings& s, game::Options& o, uint32_t& sanitized_count);

    std::vector<ModFeatureMeta> definitions_;
    EnhancedOptions enhanced_;
    toml::table unknown_nodes_;  // Preserves unknown root and table keys across roundtrips

    // Boot snapshot for restart detection
    bool boot_snapshot_captured_ = false;
    std::string boot_language_ = "ara";
    bool boot_arabic_ui_ = true;
    bool boot_arabic_subtitles_ = false;
    bool boot_packages_overlay_ = false;
};

// Global accessor
SettingsRegistry& Registry();

}  // namespace modsettings
