#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "common/mod_schema.h"
#include "common/options.h"

namespace modsettings {

enum class PresetKind : uint32_t {
    Original = 0,
    Enhanced = 1,
    Performance = 2,
    Custom = 3
};

const char* PresetName(PresetKind kind);
PresetKind PresetFromName(std::string_view name);

// Captures preset-managed fields (excludes cheats, controls, keybinds, language)
struct PresetSnapshot {
    bool valid = false;

    // App settings
    uint32_t render_percent = 67;
    uint32_t fps_cap = 0;
    bool vsync = false;
    uint32_t aniso = 0;

    // Game visual/perf settings
    uint32_t anti_aliasing = 0;
    uint32_t screen_filter = 0;
    bool bloom = true;
    bool fog = true;
    bool hdr = false;
    bool reload_blur = true;
    uint32_t draw_distance = 1;
    bool dynamic_resolution = false;
    uint32_t dynamic_target_fps = 60;
    uint32_t dynamic_min_percent = 40;
    uint32_t dynamic_max_percent = 100;
    uint32_t dynamic_hold = 5;
    bool fps_counter = false;
    bool skip_intros = false;
    uint32_t fmv_scale = 0;

    // Enhanced host/hook fields (when unlocked/implemented)
    uint32_t tone_mapping = 0;
    bool cas_sharpen_enabled = false;
    uint32_t cas_sharpen_strength = 45;
    uint32_t bloom_intensity = 100;
    float texture_lod_bias = 0.0f;
    bool trilinear_filtering = false;
    uint32_t cinematic_grade = 0;
    uint32_t lens_vignette = 0;
    uint32_t film_grain = 0;
    bool blast_lens_pulse = false;
    uint32_t soft_shadows = 0;
    bool screen_damage_vignette = false;
    uint32_t damage_vignette_intensity = 75;
    bool low_health_desaturation = false;
    uint32_t explosion_glow_boost = 100;
    uint32_t particle_fade_scale = 100;
};

class PresetStateMachine {
public:
    PresetStateMachine() = default;

    // Bind initial values (e.g., after ResetToDefaults or initial load)
    void Bind(const SettingsRegistry& reg, const app::Settings& s, const game::Options& o);

    PresetKind ActiveKind() const { return active_kind_; }
    void SetActiveKind(PresetKind kind) { active_kind_ = kind; }

    bool HasCustomSnapshot() const { return custom_snapshot_.valid; }

    // Apply a preset: snapshots current Custom state if moving off Custom, updates only preset-managed Implemented settings.
    // Never touches cheats, controller/input sensitivity, keybinds, or localization/language!
    bool ApplyPreset(PresetKind target, SettingsRegistry& reg, app::Settings& s, game::Options& o);

    // Called whenever a setting is manually altered. Transitions to Custom if the setting is preset-managed.
    void NotifySettingChanged(SettingId id, const SettingsRegistry& reg, const app::Settings& s, const game::Options& o);

    // Serialization into [presets]
    void Serialize(toml::table& root_or_presets_table) const;
    void Deserialize(const toml::table& presets_table);

    static bool IsPresetManaged(SettingId id);

private:
    void CaptureCustomSnapshot(const SettingsRegistry& reg, const app::Settings& s, const game::Options& o);
    void RestoreCustomSnapshot(SettingsRegistry& reg, app::Settings& s, game::Options& o);

    PresetKind active_kind_ = PresetKind::Custom;
    PresetSnapshot custom_snapshot_;
};

PresetStateMachine& Presets();

}  // namespace modsettings

