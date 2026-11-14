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

// Schema versioning: 1 = legacy flat upstream, 2 = Enhanced Edition modular schema
inline constexpr int64_t kCurrentSchemaVersion = 2;
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

    // --- Enhanced Category 3: Characters ---
    CharactersRagdoll60Fps,
    CharactersLodScale,
    CharactersCustomModels,

    // --- Enhanced Category 4: Audio ---
    AudioCombatDynamicBoost,
    AudioMasterGainPercent,
    AudioModPackEnabled,

    // --- Enhanced Category 5: Arabic Localization ---
    ArabicUiEnabled,
    ArabicRtlMirror,
    ArabicSubtitlesOverlay,
    ArabicFlashMenus,
    ArabicVoicePack,

    // --- Enhanced Category 6: Gameplay ---
    GameplayEnhancedRecoil,
    GameplayDisableAimAssist,

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
    PhysicsTimeScale
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

    // Combat
    bool screen_damage_vignette = false;
    uint32_t damage_vignette_intensity = 75; // 10..100%
    bool low_health_desaturation = false;
    uint32_t explosion_glow_boost = 100;     // 50..200%
    uint32_t particle_fade_scale = 100;      // 100..400%
    uint32_t blood_particle_multiplier = 1;  // (Planned)
    bool persistent_blood_decals = false;    // (Planned)
    uint32_t max_blood_decals = 128;         // (Planned)
    uint32_t corpse_persistence_mode = 0;    // (Planned)
    bool diverse_bullet_impacts = false;     // (Planned)

    // Characters
    bool ragdoll_60fps_physics = true;
    uint32_t character_lod_scale = 1;        // 1, 2, 4
    bool custom_character_models = false;    // (Planned/Unavailable)

    // Audio
    bool combat_dynamic_boost = false;
    uint32_t master_gain_percent = 100;      // 10..150%
    bool mod_audio_pack_enabled = false;     // (Planned/Unavailable)

    // Arabic
    bool arabic_ui_enabled = true;
    bool arabic_rtl_mirror = true;
    bool arabic_subtitles_overlay = true;
    bool arabic_flash_menus = false;         // (Planned)
    bool arabic_voice_pack = false;          // (Planned/Unavailable)

    // Gameplay
    bool enhanced_recoil_feedback = false;   // (Planned)
    bool disable_aim_assist = false;

    // Interface
    std::string theme_accent = "brass";      // "brass" | "crimson" | "olive"
    bool advanced_perf_hud = false;
    bool show_restart_banners = true;
    bool hide_hud_on_screenshots = false;

    // Packages
    bool packages_overlay_enabled = false;

    // Physics Lab (1.0f = Classic 2006 BLACK behavior)
    std::string physics_preset = "classic";  // "classic" | "realistic" | "cinematic" | "mayhem" | "custom"
    float physics_explosion_force = 1.00f;   // 0.25 .. 10.00
    float physics_impact_force = 1.00f;      // 0.25 .. 10.00
    float physics_gravity_scale = 1.00f;     // 0.05 .. 5.00
    float physics_debris_intensity = 1.00f;  // 0.25 .. 6.00
    float physics_character_reaction = 1.00f;// 0.25 .. 8.00
    float physics_weapon_recoil = 1.00f;     // 0.00 .. 4.00
    float physics_explosion_radius = 1.00f;  // 0.50 .. 5.00
    float physics_ragdoll_launch = 1.00f;    // 0.25 .. 10.00
    float physics_structural_destruction = 1.00f; // 0.25 .. 10.00
    float physics_prop_bounciness = 1.00f;   // 0.25 .. 6.00
    float physics_camera_shockwave = 1.00f;  // 0.00 .. 4.00
    float physics_time_scale = 1.00f;        // 0.20 .. 2.00
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

    // Physics Lab profiles & single-control reset
    bool ApplyPhysicsPreset(std::string_view preset_name);
    void ResetPhysicsControl(SettingId id);

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
