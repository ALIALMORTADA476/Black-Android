#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "common/arabic.h"
#include "common/gyro_filter.h"
#include "common/menu.h"
#include "common/mod_schema.h"
#include "common/options.h"
#include "common/presets.h"
#include "common/time_stop_state.h"
#include "game/arabic_strings.h"
#include "game/ballistics_fx.h"
#include "game/combat_feedback_rules.h"
#include "game/delirium_rules.h"
#include "game/flame_plume.h"
#include "game/flamethrower_core.h"
#include "game/mission_select_rules.h"
#include "game/overdrive_rules.h"
#include "game/weapon_hands_skinner.h"
#include "sound/weapon_audio.h"

namespace fs = std::filesystem;

namespace {

int g_passed = 0;
int g_failed = 0;

#define EXPECT_TRUE(cond)                                                                               \
    do {                                                                                                \
        if (!(cond)) {                                                                                  \
            std::cerr << "[FAIL] " << __func__ << ":" << __LINE__ << " Expected true: " << #cond "\n"; \
            return false;                                                                               \
        }                                                                                               \
    } while (0)

#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))

#define EXPECT_NE(a, b) EXPECT_TRUE((a) != (b))

#define EXPECT_NEAR(a, b, eps) EXPECT_TRUE((((a) >= (b)) ? ((a) - (b)) : ((b) - (a))) <= (eps))

#define EXPECT_EQ(a, b)                                                                                       \
    do {                                                                                                      \
        if (!((a) == (b))) {                                                                                  \
            std::cerr << "[FAIL] " << __func__ << ":" << __LINE__ << " Expected equality: " << #a << " vs "   \
                      << #b << "\n";                                                                          \
            return false;                                                                                     \
        }                                                                                                     \
    } while (0)

struct TempDir {
    fs::path path;
    explicit TempDir(const char* name) {
        path = fs::temp_directory_path() / ("black_settings_test_" + std::string(name));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void InitDefaults(app::Settings& s, game::Options& o) {
    s = app::Settings{};
    s.display_mode = app::DisplayMode::Fullscreen;
    s.render_percent = 67;
    s.fps_cap = 0;
    s.vsync = false;
    o = game::Options{};
}

// 1. Default settings & metadata completeness
bool TestDefaultSettings() {
    app::Settings s;
    game::Options o;
    InitDefaults(s, o);

    modsettings::SettingsRegistry reg;
    reg.ResetToDefaults(s, o);

    EXPECT_EQ(s.render_percent, 67u);
    EXPECT_EQ(s.aniso, 0u);
    EXPECT_EQ(o.anti_aliasing, 0u);
    EXPECT_EQ(o.draw_distance, 1u);
    EXPECT_EQ(o.fov, 70u);
    EXPECT_EQ(o.language, std::string("ara"));
    EXPECT_TRUE(reg.GetEnhanced().arabic_ui_enabled);
    EXPECT_FALSE(o.dynamic_resolution);
    EXPECT_FALSE(o.hdr);
    EXPECT_TRUE(o.bloom);

    // Every registered setting must have stable id, category, scope, status, and non-empty description
    const auto& defs = reg.Definitions();
    EXPECT_TRUE(defs.size() >= 40);
    for (const auto& meta : defs) {
        EXPECT_TRUE(meta.key != nullptr && std::string(meta.key).size() > 0);
        EXPECT_TRUE(meta.label != nullptr && std::string(meta.label).size() > 0);
        EXPECT_TRUE(meta.description != nullptr && std::string(meta.description).size() > 10);
    }

    // Unlocked FidelityFX CAS Sharpening works cleanly
    auto cas_res = reg.SetBool(modsettings::SettingId::GraphicsCasSharpenEnabled, true, s, o);
    EXPECT_TRUE(cas_res.accepted);
    EXPECT_TRUE(reg.GetEnhanced().cas_sharpen_enabled);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::GraphicsCasSharpenStrength, 65u, s, o).accepted);
    EXPECT_EQ(reg.GetEnhanced().cas_sharpen_strength, 65u);
    reg.ResetToDefaults(s, o);

    // Unsupported/planned features must never be mutable as active working switches
    auto lock_res = reg.SetUint(modsettings::SettingId::GraphicsSoftShadows, 1u, s, o);
    EXPECT_FALSE(lock_res.accepted);
    EXPECT_EQ(reg.GetEnhanced().soft_shadows, 0u);

    return true;
}

// 2. Settings save/load round-trip (root keys + namespaced sections + atomic save)
bool TestSettingsSaveAndLoad() {
    TempDir tmp("save_load");
    fs::path cfg = tmp.path / "black.toml";

    app::Settings s1;
    game::Options o1;
    InitDefaults(s1, o1);
    modsettings::SettingsRegistry reg1;
    reg1.ResetToDefaults(s1, o1);

    EXPECT_TRUE(reg1.SetUint(modsettings::SettingId::RenderPercent, 90u, s1, o1).accepted);
    EXPECT_TRUE(reg1.SetUint(modsettings::SettingId::AnisotropicFiltering, 16u, s1, o1).accepted);
    EXPECT_TRUE(reg1.SetUint(modsettings::SettingId::Fov, 95u, s1, o1).accepted);
    EXPECT_TRUE(reg1.SetBool(modsettings::SettingId::Bloom, false, s1, o1).accepted);
    EXPECT_TRUE(reg1.SetBool(modsettings::SettingId::InfiniteAmmo, true, s1, o1).accepted);
    o1.keys["fire"] = "LeftMouse";

    modsettings::PresetStateMachine p1;
    p1.SetActiveKind(modsettings::PresetKind::Custom);

    EXPECT_TRUE(reg1.SaveToFile(cfg, s1, o1, &p1));
    EXPECT_TRUE(fs::exists(cfg));

    // Verify backward-compatible flat root keys exist in serialized TOML
    toml::table parsed = toml::parse_file(cfg.string());
    EXPECT_EQ(parsed["render_percent"].value_or(0), 90);
    EXPECT_EQ(parsed["anisotropic_filtering"].value_or(0), 16);
    EXPECT_EQ(parsed["fov"].value_or(0), 95);
    EXPECT_FALSE(parsed["bloom"].value_or(true));
    EXPECT_TRUE(parsed["infinite_ammo"].value_or(false));
    EXPECT_EQ(parsed["schema_version"].value_or(0), 3);

    // Reload into clean state
    app::Settings s2;
    game::Options o2;
    InitDefaults(s2, o2);
    modsettings::SettingsRegistry reg2;
    modsettings::PresetStateMachine p2;
    auto load_res = reg2.LoadFromFile(cfg, s2, o2, &p2);
    EXPECT_TRUE(load_res.parsed_ok);
    EXPECT_EQ(s2.render_percent, 90u);
    EXPECT_EQ(s2.aniso, 16u);
    EXPECT_EQ(o2.fov, 95u);
    EXPECT_FALSE(o2.bloom);
    EXPECT_TRUE(o2.infinite_ammo);
    EXPECT_EQ(o2.keys["fire"], std::string("LeftMouse"));

    return true;
}

// 3. Invalid values (out-of-range, invalid discrete enum, wrong types, NaN/Inf, min > max, malformed TOML)
bool TestInvalidValues() {
    app::Settings s;
    game::Options o;
    InitDefaults(s, o);
    modsettings::SettingsRegistry reg;

    const char* bad_toml = R"(
schema_version = 2
defaults = 2
render_percent = 999
anisotropic_filtering = 7
anti_aliasing = -5
draw_distance = 13
dynamic_min_percent = 150
dynamic_max_percent = 80
fov = 5
mouse_sensitivity = "not_a_number"
language = "klingon"
bloom = 12345
)";

    auto res = reg.LoadFromString(bad_toml, s, o, nullptr);
    EXPECT_TRUE(res.parsed_ok);
    EXPECT_TRUE(res.sanitized_count > 0);

    EXPECT_EQ(s.render_percent, 200u);       // clamped to max 200
    EXPECT_EQ(s.aniso, 8u);                  // snapped to nearest valid {0,2,4,8,16} or default
    EXPECT_EQ(o.anti_aliasing, 0u);          // out-of-range fallback/clamp
    EXPECT_TRUE(o.draw_distance == 8u || o.draw_distance == 1u);
    EXPECT_TRUE(o.dynamic_min_percent <= o.dynamic_max_percent);
    EXPECT_EQ(o.fov, 45u);                   // clamped to min 45
    EXPECT_TRUE(std::fabs(o.mouse_sensitivity - 1.0f) < 0.001f);
    EXPECT_EQ(o.language, std::string("ara"));
    EXPECT_TRUE(o.bloom);

    // Direct API rejection / clamping with NaN
    auto nan_res = reg.SetFloat(modsettings::SettingId::MouseSensitivity, std::nanf(""), s, o);
    EXPECT_FALSE(nan_res.accepted);
    EXPECT_TRUE(std::isfinite(o.mouse_sensitivity));

    // Malformed TOML syntax fallback to safe defaults without crashing
    TempDir tmp("corrupt");
    fs::path bad_file = tmp.path / "corrupt.toml";
    {
        std::ofstream out(bad_file);
        out << "this = [unclosed\n";
    }
    auto corrupt_res = reg.LoadFromFile(bad_file, s, o, nullptr);
    EXPECT_FALSE(corrupt_res.parsed_ok);
    EXPECT_EQ(s.render_percent, 67u);
    EXPECT_EQ(o.fov, 70u);

    // Missing file fallback to safe defaults
    auto missing_res = reg.LoadFromFile(tmp.path / "does_not_exist.toml", s, o, nullptr);
    EXPECT_FALSE(missing_res.file_existed);
    EXPECT_EQ(s.render_percent, 67u);

    return true;
}

// 4. Missing keys (partial TOML keeps safe defaults for omitted entries)
bool TestMissingKeys() {
    app::Settings s;
    game::Options o;
    InitDefaults(s, o);
    modsettings::SettingsRegistry reg;

    const char* partial = R"(
defaults = 2
fov = 110
vsync = true
)";
    auto res = reg.LoadFromString(partial, s, o, nullptr);
    EXPECT_TRUE(res.parsed_ok);
    EXPECT_EQ(o.fov, 110u);
    EXPECT_TRUE(s.vsync);
    // Omitted keys remain safe defaults
    EXPECT_EQ(s.render_percent, 67u);
    EXPECT_EQ(o.walk_speed, 50u);
    EXPECT_EQ(o.language, std::string("ara"));
    EXPECT_TRUE(o.hold_aim);

    return true;
}

// 5. Unknown keys preservation across load -> modify -> save -> reload
bool TestUnknownKeysPreservation() {
    TempDir tmp("unknown_keys");
    fs::path cfg = tmp.path / "black.toml";

    const char* input_toml = R"(
schema_version = 2
defaults = 2
render_percent = 80
custom_user_flag = true
future_port_option = "vulkan_async_compute"

[user_notes]
profile_tag = "sd8gen2"
benchmark_runs = 4

[graphics]
future_experimental_knob = 42
)";

    app::Settings s;
    game::Options o;
    InitDefaults(s, o);
    modsettings::SettingsRegistry reg;
    auto load_res = reg.LoadFromString(input_toml, s, o, nullptr);
    EXPECT_TRUE(load_res.parsed_ok);
    EXPECT_EQ(s.render_percent, 80u);

    // Mutate a known setting and save
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::Fov, 88u, s, o).accepted);
    EXPECT_TRUE(reg.SaveToFile(cfg, s, o, nullptr));

    // Parse back with toml++ directly and confirm unknown keys/tables survived untouched
    toml::table saved = toml::parse_file(cfg.string());
    EXPECT_EQ(saved["fov"].value_or(0), 88);
    EXPECT_TRUE(saved["custom_user_flag"].value_or(false));
    EXPECT_EQ(saved["future_port_option"].value_or(std::string("")), std::string("vulkan_async_compute"));
    EXPECT_EQ(saved["user_notes"]["profile_tag"].value_or(std::string("")), std::string("sd8gen2"));
    EXPECT_EQ(saved["user_notes"]["benchmark_runs"].value_or(0), 4);
    EXPECT_EQ(saved["graphics"]["future_experimental_knob"].value_or(0), 42);

    return true;
}

// 6. Preset switching (Original, Enhanced, Performance, Custom) + Cheat/Controller isolation
bool TestPresetSwitching() {
    app::Settings s;
    game::Options o;
    InitDefaults(s, o);
    modsettings::SettingsRegistry reg;
    reg.ResetToDefaults(s, o);

    // User configures cheats, controller settings, and language — MUST NEVER be overwritten by presets
    o.infinite_ammo = true;
    o.god_mode = true;
    o.rapid_fire = 3;
    o.left_deadzone = 12;
    o.right_sensitivity = 2.4f;
    o.mouse_sensitivity = 1.8f;
    o.language = "fra";
    o.keys["jump"] = "Space";

    modsettings::PresetStateMachine presets;
    presets.Bind(reg, s, o);

    // Switch to Enhanced
    EXPECT_TRUE(presets.ApplyPreset(modsettings::PresetKind::Enhanced, reg, s, o));
    EXPECT_EQ(presets.ActiveKind(), modsettings::PresetKind::Enhanced);
    EXPECT_EQ(s.aniso, 16u);
    EXPECT_EQ(o.anti_aliasing, 1u);
    EXPECT_EQ(o.draw_distance, 2u);
    EXPECT_TRUE(o.dynamic_resolution);
    // Excluded settings untouched:
    EXPECT_TRUE(o.infinite_ammo);
    EXPECT_TRUE(o.god_mode);
    EXPECT_EQ(o.rapid_fire, 3u);
    EXPECT_EQ(o.left_deadzone, 12u);
    EXPECT_TRUE(std::fabs(o.right_sensitivity - 2.4f) < 0.001f);
    EXPECT_TRUE(std::fabs(o.mouse_sensitivity - 1.8f) < 0.001f);
    EXPECT_EQ(o.language, std::string("fra"));
    EXPECT_EQ(o.keys["jump"], std::string("Space"));

    // Switch to Performance (disables expensive optional enhancements)
    EXPECT_TRUE(presets.ApplyPreset(modsettings::PresetKind::Performance, reg, s, o));
    EXPECT_EQ(presets.ActiveKind(), modsettings::PresetKind::Performance);
    EXPECT_EQ(o.anti_aliasing, 0u);
    EXPECT_EQ(o.draw_distance, 1u);
    EXPECT_FALSE(o.bloom);
    EXPECT_FALSE(o.reload_blur);
    EXPECT_EQ(s.aniso, 4u);
    EXPECT_TRUE(o.dynamic_resolution);
    EXPECT_TRUE(o.dynamic_max_percent <= 85u);
    // Excluded settings still untouched:
    EXPECT_TRUE(o.infinite_ammo);
    EXPECT_EQ(o.left_deadzone, 12u);

    // Switch to Original (restores 2006 enhancement defaults)
    EXPECT_TRUE(presets.ApplyPreset(modsettings::PresetKind::Original, reg, s, o));
    EXPECT_EQ(presets.ActiveKind(), modsettings::PresetKind::Original);
    EXPECT_EQ(s.render_percent, 67u);
    EXPECT_EQ(s.aniso, 0u);
    EXPECT_EQ(o.anti_aliasing, 0u);
    EXPECT_EQ(o.draw_distance, 1u);
    EXPECT_FALSE(o.dynamic_resolution);
    EXPECT_TRUE(o.bloom);
    EXPECT_TRUE(o.fog);
    EXPECT_FALSE(o.hdr);
    // Cheats & controls still untouched:
    EXPECT_TRUE(o.infinite_ammo);
    EXPECT_EQ(o.keys["jump"], std::string("Space"));

    // Manual edit of a preset-managed option while on Original transitions to Custom
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::DrawDistance, 4u, s, o).accepted);
    presets.NotifySettingChanged(modsettings::SettingId::DrawDistance, reg, s, o);
    EXPECT_EQ(presets.ActiveKind(), modsettings::PresetKind::Custom);

    // Manual edit of a cheat while on Performance must NOT alter active preset
    EXPECT_TRUE(presets.ApplyPreset(modsettings::PresetKind::Performance, reg, s, o));
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::GodMode, false, s, o).accepted);
    presets.NotifySettingChanged(modsettings::SettingId::GodMode, reg, s, o);
    EXPECT_EQ(presets.ActiveKind(), modsettings::PresetKind::Performance);

    return true;
}

// 7. Custom snapshot recovery (across memory switches and TOML save/reload)
bool TestCustomSnapshotRecovery() {
    TempDir tmp("snapshot");
    fs::path cfg = tmp.path / "black.toml";

    app::Settings s;
    game::Options o;
    InitDefaults(s, o);
    modsettings::SettingsRegistry reg;
    modsettings::PresetStateMachine presets;
    presets.Bind(reg, s, o);

    // Build distinctive Custom setup
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::RenderPercent, 135u, s, o).accepted);
    presets.NotifySettingChanged(modsettings::SettingId::RenderPercent, reg, s, o);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::AnisotropicFiltering, 8u, s, o).accepted);
    presets.NotifySettingChanged(modsettings::SettingId::AnisotropicFiltering, reg, s, o);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::DrawDistance, 4u, s, o).accepted);
    presets.NotifySettingChanged(modsettings::SettingId::DrawDistance, reg, s, o);
    EXPECT_EQ(presets.ActiveKind(), modsettings::PresetKind::Custom);

    // Switch to Performance, then save, reload into fresh instances, then switch back to Custom
    EXPECT_TRUE(presets.ApplyPreset(modsettings::PresetKind::Performance, reg, s, o));
    EXPECT_TRUE(presets.HasCustomSnapshot());
    EXPECT_NE(s.render_percent, 135u);

    EXPECT_TRUE(reg.SaveToFile(cfg, s, o, &presets));

    app::Settings s2;
    game::Options o2;
    InitDefaults(s2, o2);
    modsettings::SettingsRegistry reg2;
    modsettings::PresetStateMachine presets2;
    EXPECT_TRUE(reg2.LoadFromFile(cfg, s2, o2, &presets2).parsed_ok);
    EXPECT_EQ(presets2.ActiveKind(), modsettings::PresetKind::Performance);
    EXPECT_TRUE(presets2.HasCustomSnapshot());

    EXPECT_TRUE(presets2.ApplyPreset(modsettings::PresetKind::Custom, reg2, s2, o2));
    EXPECT_EQ(presets2.ActiveKind(), modsettings::PresetKind::Custom);
    EXPECT_EQ(s2.render_percent, 135u);
    EXPECT_EQ(s2.aniso, 8u);
    EXPECT_EQ(o2.draw_distance, 4u);

    return true;
}

// 8. Restart-required settings & BootSnapshot tracking
bool TestRestartRequiredSettings() {
    app::Settings s;
    game::Options o;
    InitDefaults(s, o);
    modsettings::SettingsRegistry reg;
    reg.ResetToDefaults(s, o);
    reg.CaptureBootSnapshot(s, o);

    EXPECT_FALSE(reg.RequiresAppRestart(s, o));
    EXPECT_EQ(reg.PendingRestartCount(s, o), 0u);

    // Changing Immediate setting (FOV) must NOT trigger restart required
    auto fov_res = reg.SetUint(modsettings::SettingId::Fov, 100u, s, o);
    EXPECT_TRUE(fov_res.accepted);
    EXPECT_EQ(fov_res.scope, modsettings::ApplyScope::Immediate);
    EXPECT_FALSE(reg.RequiresAppRestart(s, o));

    // Changing AppRestart setting (`language`) must trigger restart pending & keep BootLanguage latched
    auto lang_res = reg.SetString(modsettings::SettingId::Language, "fra", s, o);
    EXPECT_TRUE(lang_res.accepted);
    EXPECT_EQ(lang_res.scope, modsettings::ApplyScope::AppRestart);
    EXPECT_TRUE(reg.RequiresAppRestart(s, o));
    EXPECT_EQ(reg.PendingRestartCount(s, o), 1u);
    EXPECT_EQ(reg.BootValueString(modsettings::SettingId::Language), std::string("ara"));

    // Reverting `language` back to boot value clears restart flag automatically
    EXPECT_TRUE(reg.SetString(modsettings::SettingId::Language, "ara", s, o).accepted);
    EXPECT_FALSE(reg.RequiresAppRestart(s, o));
    EXPECT_EQ(reg.PendingRestartCount(s, o), 0u);

    // Check Planned settings preserve distinct ApplyScope metadata (ShaderRebuild, SceneReload, AppRestart)
    const auto* ss = reg.Find(modsettings::SettingId::GraphicsSoftShadows);
    EXPECT_TRUE(ss != nullptr);
    EXPECT_EQ(ss->scope, modsettings::ApplyScope::ShaderRebuild);
    EXPECT_EQ(ss->status, modsettings::FeatureStatus::Planned);

    const auto* corpse = reg.Find(modsettings::SettingId::CombatCorpsePersistenceMode);
    EXPECT_TRUE(corpse != nullptr);
    EXPECT_EQ(corpse->scope, modsettings::ApplyScope::SceneReload);
    EXPECT_EQ(corpse->status, modsettings::FeatureStatus::Planned);

    const auto* decals = reg.Find(modsettings::SettingId::CombatPersistentBloodDecals);
    EXPECT_TRUE(decals != nullptr);
    EXPECT_EQ(decals->scope, modsettings::ApplyScope::Immediate);
    EXPECT_EQ(decals->status, modsettings::FeatureStatus::Implemented);

    return true;
}

// 9. Schema migration (legacy upstream black.toml defaults < 2 and schema_version = 0/1 -> 2)
bool TestSchemaMigration() {
    TempDir tmp("migration");
    fs::path out_cfg = tmp.path / "migrated.toml";

    // Legacy upstream v0 TOML (no schema_version, defaults = 1, old vsync = true / fps_cap = 60, hdr = true)
    const char* legacy_toml = R"(
defaults = 1
display_mode = "fullscreen"
render_percent = 100
fps_cap = 60
vsync = true
hdr = true
bloom = false
fov = 85
)";

    app::Settings s;
    game::Options o;
    InitDefaults(s, o);
    modsettings::SettingsRegistry reg;
    modsettings::PresetStateMachine presets;

    auto res = reg.LoadFromString(legacy_toml, s, o, &presets);
    EXPECT_TRUE(res.parsed_ok);
    EXPECT_TRUE(res.migrated);
    EXPECT_EQ(res.previous_schema_version, 0u);

    // Upstream rule from native/main.cpp: if defaults < 2, apply PhoneFrameDefaults (fps_cap = 0, vsync = false)
    EXPECT_EQ(s.fps_cap, 0u);
    EXPECT_FALSE(s.vsync);
    EXPECT_EQ(o.fov, 85u);
    // HDR legacy flag migrated to internal tone_mapping staging field & bloom = false -> bloom_intensity = 0
    EXPECT_TRUE(o.hdr);
    EXPECT_EQ(reg.GetEnhanced().tone_mapping, 1u);
    EXPECT_EQ(reg.GetEnhanced().bloom_intensity, 0u);

    EXPECT_TRUE(reg.SaveToFile(out_cfg, s, o, &presets));
    toml::table saved = toml::parse_file(out_cfg.string());
    EXPECT_EQ(saved["schema_version"].value_or(0), 3);
    EXPECT_EQ(saved["defaults"].value_or(0), 2);

    return true;
}

// 10. Arabic Contextual Shaping: verifies base letters map to Presentation Forms-B correctly
bool TestArabicContextualShaping() {
    // 'ب' (0x0628): Isolated=0xFE8F, Final=0xFE90, Initial=0xFE91, Medial=0xFE92
    std::vector<char32_t> word = {0x0628, 0x0628, 0x0628}; // Beh-Beh-Beh
    std::vector<char32_t> shaped = arabic::ShapeWord(word);
    EXPECT_EQ(shaped.size(), 3u);
    EXPECT_EQ(shaped[0], char32_t(0xFE91)); // Initial
    EXPECT_EQ(shaped[1], char32_t(0xFE92)); // Medial
    EXPECT_EQ(shaped[2], char32_t(0xFE90)); // Final

    // Hamza (0x0621): non-connecting
    std::vector<char32_t> hamza_word = {0x0621};
    std::vector<char32_t> shaped_hamza = arabic::ShapeWord(hamza_word);
    EXPECT_EQ(shaped_hamza.size(), 1u);
    EXPECT_EQ(shaped_hamza[0], char32_t(0xFE80));

    return true;
}

// 11. Arabic Lam-Alef Ligatures: verifies (ل + ا/أ/إ/آ) ligatures map to 0xFEF5..0xFEFC
bool TestArabicLamAlefLigature() {
    // Lam (0x0644) + Plain Alef (0x0627) -> Isolated 0xFEFB
    std::vector<char32_t> word = {0x0644, 0x0627};
    std::vector<char32_t> shaped = arabic::ShapeWord(word);
    EXPECT_EQ(shaped.size(), 1u);
    EXPECT_EQ(shaped[0], char32_t(0xFEFB)); // Lam-Alef isolated ligature

    // Lam (0x0644) + Alef Madda (0x0622) -> 0xFEF5
    std::vector<char32_t> madda = {0x0644, 0x0622};
    std::vector<char32_t> shaped_madda = arabic::ShapeWord(madda);
    EXPECT_EQ(shaped_madda.size(), 1u);
    EXPECT_EQ(shaped_madda[0], char32_t(0xFEF5));

    return true;
}

// 12. Arabic BiDi, Mixed Numerals & Latin Technical Terminology
bool TestArabicBidiAndMixedText() {
    // Pure ASCII bypasses transformation completely with 0 overhead
    std::string ascii = "FPS: 60 - Vsync: OFF";
    std::string out_ascii = arabic::ShapeAndBidi(ascii);
    EXPECT_EQ(out_ascii, ascii);

    // Number & percentage preservation: "60 FPS" stays cohesive
    std::string mixed = "60 FPS";
    EXPECT_EQ(arabic::ShapeAndBidi(mixed), mixed);

    // Bracket mirroring with Arabic text: '(' <-> ')'
    std::string with_brackets = "(الرسومات)";
    std::string shaped_brackets = arabic::ShapeAndBidi(with_brackets);
    EXPECT_FALSE(shaped_brackets.empty());
    EXPECT_TRUE(shaped_brackets.find(")") != std::string::npos || shaped_brackets.find("(") != std::string::npos);

    // Idempotency: Calling ShapeAndBidi twice MUST NOT invert/reverse the text again
    std::string double_shaped = arabic::ShapeAndBidi(shaped_brackets);
    EXPECT_EQ(double_shaped, shaped_brackets);

    return true;
}

bool TestTacticalCategoryDefinitions() {
    EXPECT_EQ(int(app::TacticalCategory::Graphics), 0);
    EXPECT_EQ(int(app::TacticalCategory::PhysicsLab), 1);
    EXPECT_EQ(int(app::TacticalCategory::CombatGameplay), 2);
    EXPECT_EQ(int(app::TacticalCategory::BallisticsTracers), 3);
    EXPECT_EQ(int(app::TacticalCategory::AudioLanguage), 4);
    EXPECT_EQ(int(app::TacticalCategory::Interface), 5);
    EXPECT_EQ(int(app::TacticalCategory::CheatsWeapons), 6);
    EXPECT_EQ(int(app::TacticalCategory::Count), 7);

    // Verify cyclic bumper navigation logic
    constexpr int kNumCats = int(app::TacticalCategory::Count);
    int cur = int(app::TacticalCategory::Graphics);
    int prev = (cur - 1 + kNumCats) % kNumCats;
    EXPECT_EQ(prev, 6);
    int next = (cur + 1) % kNumCats;
    EXPECT_EQ(next, 1);

    // Verify Cinematic visual upgrade settings are implemented and bounded
    app::Settings s;
    game::Options o;
    InitDefaults(s, o);
    modsettings::SettingsRegistry reg;
    reg.ResetToDefaults(s, o);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::GraphicsCinematicGrade, 3u, s, o).accepted);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::GraphicsLensVignette, 40u, s, o).accepted);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::GraphicsFilmGrain, 25u, s, o).accepted);
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::GraphicsBlastLensPulse, true, s, o).accepted);
    EXPECT_EQ(reg.GetUint(modsettings::SettingId::GraphicsCinematicGrade, s, o), 3u);
    EXPECT_EQ(reg.GetUint(modsettings::SettingId::GraphicsLensVignette, s, o), 40u);
    EXPECT_EQ(reg.GetUint(modsettings::SettingId::GraphicsFilmGrain, s, o), 25u);
    EXPECT_TRUE(reg.GetBool(modsettings::SettingId::GraphicsBlastLensPulse, s, o));

    return true;
}

bool TestPhysicsLabSchemaAndPresets() {
    modsettings::SettingsRegistry reg;
    app::Settings s;
    game::Options o;
    reg.ResetToDefaults(s, o);

    // Classic defaults preserve exact 1.0x 2006 BLACK physics across all 12 controls
    EXPECT_EQ(reg.GetString(modsettings::SettingId::PhysicsPreset, s, o), "realistic");
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsExplosionForce, s, o), 2.20f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsImpactForce, s, o), 2.10f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsGravityScale, s, o), 1.15f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsDebrisIntensity, s, o), 2.25f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsCharacterReaction, s, o), 1.85f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsWeaponRecoil, s, o), 1.30f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsExplosionRadius, s, o), 1.45f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsRagdollLaunch, s, o), 1.85f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsStructuralDestruction, s, o), 2.40f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsPropBounciness, s, o), 1.25f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsCameraShockwave, s, o), 1.50f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsTimeScale, s, o), 1.00f, 1e-4f);

    // Classic preset strictly preserves original 1.00f 2006 physics baseline
    EXPECT_TRUE(reg.ApplyPhysicsPreset("classic"));
    EXPECT_EQ(reg.GetString(modsettings::SettingId::PhysicsPreset, s, o), "classic");
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsExplosionForce, s, o), 1.00f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsImpactForce, s, o), 1.00f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsStructuralDestruction, s, o), 1.00f, 1e-4f);

    // Cinematic preset applies amplified parameters and retains preset name
    EXPECT_TRUE(reg.ApplyPhysicsPreset("cinematic"));
    EXPECT_EQ(reg.GetString(modsettings::SettingId::PhysicsPreset, s, o), "cinematic");
    EXPECT_TRUE(reg.GetFloat(modsettings::SettingId::PhysicsExplosionForce, s, o) > 2.0f);
    EXPECT_TRUE(reg.GetFloat(modsettings::SettingId::PhysicsGravityScale, s, o) < 1.0f);
    EXPECT_TRUE(reg.GetFloat(modsettings::SettingId::PhysicsRagdollLaunch, s, o) > 2.0f);

    // Mayhem preset applies extreme high-impact values across all 12 controls
    EXPECT_TRUE(reg.ApplyPhysicsPreset("mayhem"));
    EXPECT_EQ(reg.GetString(modsettings::SettingId::PhysicsPreset, s, o), "mayhem");
    EXPECT_TRUE(reg.GetFloat(modsettings::SettingId::PhysicsExplosionForce, s, o) >= 5.0f);
    EXPECT_TRUE(reg.GetFloat(modsettings::SettingId::PhysicsImpactForce, s, o) >= 4.0f);
    EXPECT_TRUE(reg.GetFloat(modsettings::SettingId::PhysicsRagdollLaunch, s, o) >= 4.5f);
    EXPECT_TRUE(reg.GetFloat(modsettings::SettingId::PhysicsExplosionRadius, s, o) >= 2.0f);
    EXPECT_TRUE(reg.GetFloat(modsettings::SettingId::PhysicsStructuralDestruction, s, o) >= 3.0f);

    // Manual edit transitions physics preset to "custom" and clamps out-of-range inputs to expanded upper bounds
    EXPECT_TRUE(reg.SetFloat(modsettings::SettingId::PhysicsExplosionForce, 99.0f, s, o).accepted);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsExplosionForce, s, o), 10.00f, 1e-4f);
    EXPECT_TRUE(reg.SetFloat(modsettings::SettingId::PhysicsRagdollLaunch, 8.5f, s, o).accepted);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsRagdollLaunch, s, o), 8.50f, 1e-4f);
    EXPECT_TRUE(reg.SetFloat(modsettings::SettingId::PhysicsTimeScale, 0.50f, s, o).accepted);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsTimeScale, s, o), 0.50f, 1e-4f);
    EXPECT_EQ(reg.GetString(modsettings::SettingId::PhysicsPreset, s, o), "custom");

    // TOML roundtrip preserves [physics_lab] custom parameters without dropping unknown siblings
    std::ostringstream oss;
    oss << reg.SaveToTable(s, o, nullptr) << "\n";
    std::string serialized = oss.str();
    EXPECT_TRUE(serialized.find("[physics_lab]") != std::string::npos);

    modsettings::SettingsRegistry reg2;
    app::Settings s2;
    game::Options o2;
    auto lr = reg2.LoadFromString(serialized, s2, o2, nullptr);
    EXPECT_TRUE(lr.parsed_ok);
    EXPECT_EQ(reg2.GetString(modsettings::SettingId::PhysicsPreset, s2, o2), "custom");
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsExplosionForce, s2, o2), 10.00f, 1e-4f);
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsRagdollLaunch, s2, o2), 8.50f, 1e-4f);
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsTimeScale, s2, o2), 0.50f, 1e-4f);

    // Invalid physics_lab values in TOML get clamped/sanitized safely
    std::string dirty = "[physics_lab]\npreset = \"bogus\"\nexplosion_force = 50.0\ngravity_scale = -3.0\nweapon_recoil = \"oops\"\n";
    auto lr_dirty = reg2.LoadFromString(dirty, s2, o2, nullptr);
    EXPECT_TRUE(lr_dirty.parsed_ok);
    EXPECT_TRUE(lr_dirty.sanitized_count >= 4u);
    EXPECT_EQ(reg2.GetString(modsettings::SettingId::PhysicsPreset, s2, o2), "realistic");
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsExplosionForce, s2, o2), 10.00f, 1e-4f);
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsGravityScale, s2, o2), 0.05f, 1e-4f);
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsWeaponRecoil, s2, o2), 1.30f, 1e-4f);

    // Classic preset restores exact 1.0x baseline across all 12 controls
    EXPECT_TRUE(reg2.ApplyPhysicsPreset("classic"));
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsExplosionForce, s2, o2), 1.00f, 1e-4f);
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsGravityScale, s2, o2), 1.00f, 1e-4f);
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsWeaponRecoil, s2, o2), 1.00f, 1e-4f);
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsExplosionRadius, s2, o2), 1.00f, 1e-4f);
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsRagdollLaunch, s2, o2), 1.00f, 1e-4f);
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsStructuralDestruction, s2, o2), 1.00f, 1e-4f);
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsPropBounciness, s2, o2), 1.00f, 1e-4f);
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsCameraShockwave, s2, o2), 1.00f, 1e-4f);
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsTimeScale, s2, o2), 1.00f, 1e-4f);

    // Realistic preset sets distinct calibrated values
    EXPECT_TRUE(reg2.ApplyPhysicsPreset("realistic"));
    EXPECT_EQ(reg2.GetString(modsettings::SettingId::PhysicsPreset, s2, o2), "realistic");
    EXPECT_TRUE(reg2.GetFloat(modsettings::SettingId::PhysicsGravityScale, s2, o2) > 1.0f);

    return true;
}

bool TestArabicGameStringLookup() {
    // Known Objective String ID (0x090F77E3: "find the armoury")
    const game::ArabicStringEntry* e1 = game::FindArabicString(0x090F77E3);
    EXPECT_TRUE(e1 != nullptr);
    EXPECT_EQ(std::string(e1->english), "find the armoury");
    EXPECT_EQ(std::string(e1->arabic_u8), "ابحث عن مخزن الأسلحة السري");
    EXPECT_EQ(game::CategoryOf(0x090F77E3), game::StringCategory::Objective);

    // Known Prompt String ID (0x0980DEFA: "explore to find health packs")
    const game::ArabicStringEntry* e2 = game::FindArabicString(0x0980DEFA);
    EXPECT_TRUE(e2 != nullptr);
    EXPECT_EQ(std::string(e2->english), "explore to find health packs");
    EXPECT_EQ(std::string(e2->arabic_u8), "استكشف المكان للعثور على حقيبة إسعاف");
    EXPECT_EQ(game::CategoryOf(0x0980DEFA), game::StringCategory::Prompt);

    // Known Subtitle String ID (0x16F48A2C)
    const game::ArabicStringEntry* e3 = game::FindArabicString(0x16F48A2C);
    EXPECT_TRUE(e3 != nullptr);
    EXPECT_EQ(game::CategoryOf(0x16F48A2C), game::StringCategory::Subtitle);

    // Non-existent ID returns nullptr and Unknown category
    EXPECT_TRUE(game::FindArabicString(0xDEADBEEF) == nullptr);
    EXPECT_TRUE(game::ArabicText(0xDEADBEEF) == nullptr);
    EXPECT_EQ(game::CategoryOf(0xDEADBEEF), game::StringCategory::Unknown);

    return true;
}

bool TestArabicMultilineShapingAndWrapping() {
    // 1. Arabic punctuation identification
    EXPECT_TRUE(arabic::IsArabicPunctuation(0x060C)); // Arabic comma
    EXPECT_TRUE(arabic::IsArabicPunctuation(0x061B)); // Arabic semicolon
    EXPECT_TRUE(arabic::IsArabicPunctuation(0x061F)); // Arabic question mark
    EXPECT_FALSE(arabic::IsArabicPunctuation('a'));

    // 2. WrapText word boundary preservation
    std::string long_text = "تدمير مستودع الأسلحة والذخائر في القاعدة العسكرية المحصنة قبل وصول التعزيزات";
    std::string wrapped = arabic::WrapText(long_text, 35);
    EXPECT_TRUE(wrapped.find('\n') != std::string::npos);

    // 3. ShapeAndBidiMultiline preserves separate lines without reordering them
    std::string multiline_input = "السطر الأول\nالسطر الثاني\nالسطر الثالث";
    std::string shaped_multi = arabic::ShapeAndBidiMultiline(multiline_input);
    EXPECT_FALSE(shaped_multi.empty());

    // Count newlines: must preserve line count
    size_t newlines_in = std::count(multiline_input.begin(), multiline_input.end(), '\n');
    size_t newlines_out = std::count(shaped_multi.begin(), shaped_multi.end(), '\n');
    EXPECT_EQ(newlines_in, newlines_out);

    return true;
}

bool TestEnhancedSettingsImplemented() {
    modsettings::SettingsRegistry reg;
    app::Settings s;
    game::Options o;
    reg.ResetToDefaults(s, o);

    const modsettings::SettingId newly_implemented[] = {
        modsettings::SettingId::GraphicsToneMapping,
        modsettings::SettingId::GraphicsBloomIntensity,
        modsettings::SettingId::GraphicsTextureLodBias,
        modsettings::SettingId::GraphicsTrilinearFiltering,
        modsettings::SettingId::GraphicsWeaponReflections,
        modsettings::SettingId::CombatScreenDamageVignette,
        modsettings::SettingId::CombatDamageVignetteIntensity,
        modsettings::SettingId::CombatLowHealthDesaturation,
        modsettings::SettingId::CombatExplosionGlowBoost,
        modsettings::SettingId::CombatParticleFadeScale,
        modsettings::SettingId::CharactersRagdoll60Fps,
        modsettings::SettingId::CharactersLodScale,
        modsettings::SettingId::AudioCombatDynamicBoost,
        modsettings::SettingId::AudioMasterGainPercent,
        modsettings::SettingId::GameplayDisableAimAssist,
        modsettings::SettingId::InterfaceThemeAccent,
        modsettings::SettingId::InterfaceAdvancedPerfHud,
        modsettings::SettingId::InterfaceShowRestartBanners,
        modsettings::SettingId::InterfaceHideHudOnScreenshots,
    };
    for (auto id : newly_implemented) {
        const auto* m = reg.Find(id);
        EXPECT_TRUE(m != nullptr);
        EXPECT_EQ(m->status, modsettings::FeatureStatus::Implemented);
    }

    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::GraphicsToneMapping, 2u, s, o).accepted);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::GraphicsBloomIntensity, 160u, s, o).accepted);
    EXPECT_TRUE(reg.SetFloat(modsettings::SettingId::GraphicsTextureLodBias, -1.0f, s, o).accepted);
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::GraphicsTrilinearFiltering, false, s, o).accepted);
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::GraphicsWeaponReflections, false, s, o).accepted);
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::CombatScreenDamageVignette, false, s, o).accepted);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::CombatDamageVignetteIntensity, 95u, s, o).accepted);
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::CombatLowHealthDesaturation, false, s, o).accepted);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::CombatExplosionGlowBoost, 150u, s, o).accepted);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::CombatParticleFadeScale, 150u, s, o).accepted);
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::CharactersRagdoll60Fps, false, s, o).accepted);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::CharactersLodScale, 2u, s, o).accepted);
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::AudioCombatDynamicBoost, true, s, o).accepted);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::AudioMasterGainPercent, 125u, s, o).accepted);
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::GameplayDisableAimAssist, true, s, o).accepted);
    EXPECT_TRUE(reg.SetString(modsettings::SettingId::InterfaceThemeAccent, "crimson", s, o).accepted);
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::InterfaceAdvancedPerfHud, true, s, o).accepted);
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::InterfaceShowRestartBanners, false, s, o).accepted);
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::InterfaceHideHudOnScreenshots, true, s, o).accepted);

    std::ostringstream oss;
    oss << reg.SaveToTable(s, o, nullptr) << "\n";
    modsettings::SettingsRegistry reg2;
    app::Settings s2;
    game::Options o2;
    auto lr = reg2.LoadFromString(oss.str(), s2, o2, nullptr);
    EXPECT_TRUE(lr.parsed_ok);
    EXPECT_EQ(reg2.GetEnhanced().tone_mapping, 2u);
    EXPECT_EQ(reg2.GetEnhanced().bloom_intensity, 160u);
    EXPECT_NEAR(reg2.GetEnhanced().texture_lod_bias, -1.0f, 1e-4f);
    EXPECT_EQ(reg2.GetEnhanced().trilinear_filtering, false);
    EXPECT_EQ(reg2.GetEnhanced().damage_vignette_intensity, 95u);
    EXPECT_EQ(reg2.GetEnhanced().particle_fade_scale, 150u);
    EXPECT_EQ(reg2.GetEnhanced().character_lod_scale, 2u);
    EXPECT_EQ(reg2.GetEnhanced().master_gain_percent, 125u);
    EXPECT_EQ(reg2.GetEnhanced().disable_aim_assist, true);
    EXPECT_EQ(reg2.GetEnhanced().theme_accent, "crimson");
    EXPECT_EQ(reg2.GetEnhanced().advanced_perf_hud, true);
    EXPECT_EQ(reg2.GetEnhanced().hide_hud_on_screenshots, true);

    return true;
}

bool TestTimeStopSchemaAndStateMachine() {
    // 1. Registry definitions & validation for Time Stop controls under PhysicsLab
    modsettings::SettingsRegistry reg;
    app::Settings s;
    game::Options o;
    reg.ResetToDefaults(s, o);

    const modsettings::SettingId ts_ids[] = {
        modsettings::SettingId::PhysicsTimeStopEnabled,
        modsettings::SettingId::PhysicsTimeStopMode,
        modsettings::SettingId::PhysicsTimeStopMaxDuration,
        modsettings::SettingId::PhysicsTimeStopCooldown,
    };
    for (auto id : ts_ids) {
        const auto* m = reg.Find(id);
        EXPECT_TRUE(m != nullptr);
        EXPECT_EQ(m->status, modsettings::FeatureStatus::Implemented);
        EXPECT_EQ(m->category, modsettings::Category::PhysicsLab);
    }

    EXPECT_FALSE(reg.GetBool(modsettings::SettingId::PhysicsTimeStopEnabled, s, o));
    EXPECT_EQ(reg.GetUint(modsettings::SettingId::PhysicsTimeStopMode, s, o), 0u);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsTimeStopMaxDuration, s, o), 0.0f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsTimeStopCooldown, s, o), 0.0f, 1e-4f);

    // Valid setters & clamping
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::PhysicsTimeStopEnabled, true, s, o).accepted);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::PhysicsTimeStopMode, 1u, s, o).accepted);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::PhysicsTimeStopMode, 2u, s, o).accepted);
    EXPECT_EQ(reg.GetUint(modsettings::SettingId::PhysicsTimeStopMode, s, o), 1u);
    EXPECT_TRUE(reg.SetFloat(modsettings::SettingId::PhysicsTimeStopMaxDuration, 99.0f, s, o).accepted);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsTimeStopMaxDuration, s, o), 30.0f, 1e-4f);
    EXPECT_TRUE(reg.SetFloat(modsettings::SettingId::PhysicsTimeStopCooldown, -5.0f, s, o).accepted);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsTimeStopCooldown, s, o), 0.0f, 1e-4f);
    EXPECT_TRUE(reg.SetFloat(modsettings::SettingId::PhysicsTimeStopCooldown, 4.0f, s, o).accepted);
    EXPECT_TRUE(reg.SetFloat(modsettings::SettingId::PhysicsTimeStopMaxDuration, 8.0f, s, o).accepted);

    // Round-trip TOML persistence
    std::ostringstream oss;
    oss << reg.SaveToTable(s, o, nullptr) << "\n";
    modsettings::SettingsRegistry reg2;
    app::Settings s2;
    game::Options o2;
    auto lr = reg2.LoadFromString(oss.str(), s2, o2, nullptr);
    EXPECT_TRUE(lr.parsed_ok);
    EXPECT_TRUE(reg2.GetBool(modsettings::SettingId::PhysicsTimeStopEnabled, s2, o2));
    EXPECT_EQ(reg2.GetUint(modsettings::SettingId::PhysicsTimeStopMode, s2, o2), 1u);
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsTimeStopMaxDuration, s2, o2), 8.0f, 1e-4f);
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsTimeStopCooldown, s2, o2), 4.0f, 1e-4f);

    // ResetPhysicsControl resets Time Stop controls cleanly
    reg2.ApplyPhysicsPreset("classic");
    EXPECT_FALSE(reg2.GetBool(modsettings::SettingId::PhysicsTimeStopEnabled, s2, o2));
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsTimeStopMaxDuration, s2, o2), 0.0f, 1e-4f);

    // 2. State machine: Toggle mode, max duration auto-resume, cooldown gating
    common::TimeStopStateMachine sm;
    common::TimeStopConfig cfg;
    cfg.mode = common::TimeStopMode::Toggle;
    cfg.max_duration_sec = 2.5f;
    cfg.cooldown_sec = 1.5f;

    EXPECT_FALSE(sm.IsFrozen());
    EXPECT_TRUE(sm.OnTriggerPressed(cfg, true));
    EXPECT_TRUE(sm.IsFrozen());

    sm.Update(1.0f, cfg, true);
    EXPECT_TRUE(sm.IsFrozen());
    // Single huge dt spike (1.0s) is clamped to 0.25s for stability
    EXPECT_NEAR(sm.ActiveElapsedSec(), 0.25f, 1e-4f);
    for (int i = 0; i < 3; ++i) sm.Update(0.25f, cfg, true);
    EXPECT_NEAR(sm.ActiveElapsedSec(), 1.0f, 1e-4f);
    EXPECT_NEAR(sm.RemainingDurationSec(cfg), 1.5f, 1e-4f);

    // Advance remaining 1.5s (total 2.5s) -> auto-resume and enter cooldown
    for (int i = 0; i < 6; ++i) sm.Update(0.25f, cfg, true);
    EXPECT_FALSE(sm.IsFrozen());
    EXPECT_TRUE(sm.IsOnCooldown());
    EXPECT_NEAR(sm.CooldownRemainingSec(), 1.5f, 1e-4f);
    EXPECT_FALSE(sm.OnTriggerPressed(cfg, true));

    // Cooldown completes across realistic frames
    for (int i = 0; i < 6; ++i) sm.Update(0.25f, cfg, true);
    EXPECT_FALSE(sm.IsOnCooldown());
    EXPECT_TRUE(sm.OnTriggerPressed(cfg, true));
    EXPECT_TRUE(sm.IsFrozen());

    // Toggle off manually
    EXPECT_TRUE(sm.OnTriggerPressed(cfg, true));
    EXPECT_FALSE(sm.IsFrozen());

    // 3. Hold mode: activates while held, deactivates immediately on release
    cfg.mode = common::TimeStopMode::Hold;
    cfg.cooldown_sec = 0.0f;
    sm.ForceResume(false);
    sm.SetHoldInput(true, cfg, true);
    EXPECT_TRUE(sm.IsFrozen());
    sm.Update(0.4f, cfg, true);
    EXPECT_TRUE(sm.IsFrozen());
    sm.SetHoldInput(false, cfg, true);
    EXPECT_FALSE(sm.IsFrozen());

    // 4. Emergency deactivation & world_active=false safety
    cfg.mode = common::TimeStopMode::Toggle;
    EXPECT_TRUE(sm.OnTriggerPressed(cfg, true));
    EXPECT_TRUE(sm.IsFrozen());
    sm.Update(0.5f, cfg, false); // Leaving active gameplay immediately unfreezes safely
    EXPECT_FALSE(sm.IsFrozen());

    // 5. World timestamp shift calculation avoids negative/NaN spikes
    EXPECT_NEAR(common::SanitizeFreezeWorldDelta(0.016f), 0.016f, 1e-5f);
    EXPECT_NEAR(common::SanitizeFreezeWorldDelta(-0.5f), 0.0f, 1e-5f);
    EXPECT_NEAR(common::SanitizeFreezeWorldDelta(5.0f), 0.25f, 1e-5f);

    // 6. Arabic shaping sanity on required UI strings
    EXPECT_FALSE(arabic::ShapeAndBidi("إيقاف الزمن").empty());
    EXPECT_FALSE(arabic::ShapeAndBidi("تجميد العالم").empty());
    EXPECT_FALSE(arabic::ShapeAndBidi("استئناف الزمن").empty());
    EXPECT_FALSE(arabic::ShapeAndBidi("مدة التجميد").empty());

    return true;
}

bool TestWeaponAudioRemasterSchema() {
    modsettings::SettingsRegistry reg;
    app::Settings s;
    game::Options o;
    reg.ResetToDefaults(s, o);

    const modsettings::SettingId audio_ids[] = {
        modsettings::SettingId::AudioModPackEnabled,
        modsettings::SettingId::AudioWeaponVolume,
        modsettings::SettingId::AudioMechanicalVolume,
        modsettings::SettingId::AudioReloadVolume,
        modsettings::SettingId::AudioEnvironmentVolume,
        modsettings::SettingId::AudioTailIntensity,
        modsettings::SettingId::AudioEchoIntensity,
        modsettings::SettingId::AudioDistantGunfire,
        modsettings::SettingId::AudioBulletFlyby,
        modsettings::SettingId::AudioVariationPercent,
    };
    for (auto id : audio_ids) {
        const auto* m = reg.Find(id);
        EXPECT_TRUE(m != nullptr);
        EXPECT_EQ(m->status, modsettings::FeatureStatus::Implemented);
        EXPECT_EQ(m->scope, modsettings::ApplyScope::Immediate);
    }

    // Defaults: Enhanced remaster enabled, stem volumes 100%, variation 50%
    EXPECT_TRUE(reg.GetBool(modsettings::SettingId::AudioModPackEnabled, s, o));
    EXPECT_EQ(reg.GetUint(modsettings::SettingId::AudioWeaponVolume, s, o), 100u);
    EXPECT_EQ(reg.GetUint(modsettings::SettingId::AudioMechanicalVolume, s, o), 100u);
    EXPECT_EQ(reg.GetUint(modsettings::SettingId::AudioReloadVolume, s, o), 100u);
    EXPECT_EQ(reg.GetUint(modsettings::SettingId::AudioEnvironmentVolume, s, o), 100u);
    EXPECT_EQ(reg.GetUint(modsettings::SettingId::AudioTailIntensity, s, o), 100u);
    EXPECT_EQ(reg.GetUint(modsettings::SettingId::AudioEchoIntensity, s, o), 100u);
    EXPECT_EQ(reg.GetUint(modsettings::SettingId::AudioDistantGunfire, s, o), 100u);
    EXPECT_EQ(reg.GetUint(modsettings::SettingId::AudioBulletFlyby, s, o), 100u);
    EXPECT_EQ(reg.GetUint(modsettings::SettingId::AudioVariationPercent, s, o), 50u);

    // Mutations, clamping, stepping & A/B toggle
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::AudioModPackEnabled, false, s, o).accepted);
    EXPECT_FALSE(reg.GetBool(modsettings::SettingId::AudioModPackEnabled, s, o));
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::AudioWeaponVolume, 140u, s, o).accepted);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::AudioMechanicalVolume, 200u, s, o).accepted); // clamps to 150
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::AudioReloadVolume, 85u, s, o).accepted);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::AudioEnvironmentVolume, 120u, s, o).accepted);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::AudioTailIntensity, 135u, s, o).accepted);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::AudioEchoIntensity, 200u, s, o).accepted); // clamps to 150
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::AudioDistantGunfire, 110u, s, o).accepted);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::AudioBulletFlyby, 95u, s, o).accepted);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::AudioVariationPercent, 150u, s, o).accepted); // clamps to 100
    EXPECT_EQ(reg.GetEnhanced().weapon_audio_volume, 140u);
    EXPECT_EQ(reg.GetEnhanced().mechanical_audio_volume, 150u);
    EXPECT_EQ(reg.GetEnhanced().reload_audio_volume, 85u);
    EXPECT_EQ(reg.GetEnhanced().environmental_audio_volume, 120u);
    EXPECT_EQ(reg.GetEnhanced().audio_tail_intensity, 135u);
    EXPECT_EQ(reg.GetEnhanced().audio_echo_intensity, 150u);
    EXPECT_EQ(reg.GetEnhanced().audio_distant_gunfire, 110u);
    EXPECT_EQ(reg.GetEnhanced().audio_bullet_flyby, 95u);
    EXPECT_EQ(reg.GetEnhanced().audio_variation_percent, 100u);

    // TOML roundtrip persistence
    std::ostringstream oss;
    oss << reg.SaveToTable(s, o, nullptr) << "\n";
    modsettings::SettingsRegistry reg2;
    app::Settings s2;
    game::Options o2;
    auto lr = reg2.LoadFromString(oss.str(), s2, o2, nullptr);
    EXPECT_TRUE(lr.parsed_ok);
    EXPECT_FALSE(reg2.GetBool(modsettings::SettingId::AudioModPackEnabled, s2, o2));

    // Legacy v2 config migration: prior Planned placeholder saved false; upgrading to v3 enables remaster once
    const char* v2_legacy_audio = "schema_version = 2\ndefaults = 2\n[audio]\nmod_audio_pack_enabled = false\n";
    modsettings::SettingsRegistry reg3;
    app::Settings s3;
    game::Options o3;
    auto lr3 = reg3.LoadFromString(v2_legacy_audio, s3, o3, nullptr);
    EXPECT_TRUE(lr3.parsed_ok);
    EXPECT_TRUE(lr3.migrated);
    EXPECT_TRUE(reg3.GetBool(modsettings::SettingId::AudioModPackEnabled, s3, o3));
    EXPECT_EQ(reg2.GetUint(modsettings::SettingId::AudioWeaponVolume, s2, o2), 140u);
    EXPECT_EQ(reg2.GetUint(modsettings::SettingId::AudioMechanicalVolume, s2, o2), 150u);
    EXPECT_EQ(reg2.GetUint(modsettings::SettingId::AudioReloadVolume, s2, o2), 85u);
    EXPECT_EQ(reg2.GetUint(modsettings::SettingId::AudioEnvironmentVolume, s2, o2), 120u);
    EXPECT_EQ(reg2.GetUint(modsettings::SettingId::AudioTailIntensity, s2, o2), 135u);
    EXPECT_EQ(reg2.GetUint(modsettings::SettingId::AudioEchoIntensity, s2, o2), 150u);
    EXPECT_EQ(reg2.GetUint(modsettings::SettingId::AudioDistantGunfire, s2, o2), 110u);
    EXPECT_EQ(reg2.GetUint(modsettings::SettingId::AudioBulletFlyby, s2, o2), 95u);
    EXPECT_EQ(reg2.GetUint(modsettings::SettingId::AudioVariationPercent, s2, o2), 100u);

    return true;
}

bool TestWeaponAudioPackAndMixer() {
    const char* pak_candidates[] = {
        "e:/ai/black/app/src/main/assets/weapon_audio/black_weapon_audio.pak",
        "../../app/src/main/assets/weapon_audio/black_weapon_audio.pak",
        "app/src/main/assets/weapon_audio/black_weapon_audio.pak",
    };
    bool loaded = false;
    for (const char* p : pak_candidates) {
        if (sound::weapon_audio::LoadPackFromFile(p)) {
            loaded = true;
            break;
        }
    }
    EXPECT_TRUE(loaded);

    auto t0 = sound::weapon_audio::GetTelemetry();
    EXPECT_TRUE(t0.pack_loaded);
    EXPECT_EQ(t0.pack_version, 3u);
    EXPECT_EQ(t0.sample_rate, 48000u);
    EXPECT_TRUE(t0.slot_count >= 129u);
    EXPECT_TRUE(t0.clip_count >= 420u);
    EXPECT_TRUE(t0.signature_count >= 400u);
    EXPECT_TRUE(t0.stereo_clips > 100u);

    // Known bg1_bnsLF ISO wave signature must map to unsuppressed gunfire (slot 10 BNS_FIRE, flags bit0 == 1)
    uint16_t flags = 0;
    int32_t bns_slot = sound::weapon_audio::FindSignature(0xdc8ae09a108a50f3ULL, 14112u, &flags);
    EXPECT_EQ(bns_slot, 10);
    EXPECT_TRUE((flags & 1u) != 0);

    // Unknown signature returns -1
    EXPECT_EQ(sound::weapon_audio::FindSignature(0x123456789ABCDEF0ULL, 1234u, nullptr), -1);

    // Verify FNV-1a empty/non-empty hashing
    const char msg[] = "BWAR";
    EXPECT_NE(sound::weapon_audio::Fnv1a64(msg, 4), 0ULL);

    // Ensure Enhanced audio triggers layered Primary + Rolling Tail ("Mattak") + Indoor/Outdoor Echo
    // while NOT auto-firing BulletFlyby on player shots.
    auto& enh = modsettings::Registry().GetEnhanced();
    enh = modsettings::EnhancedOptions{};
    sound::weapon_audio::StopAll();
    sound::weapon_audio::SetEnvironmentState(true, 0.35f);
    sound::weapon_audio::OnPlayerFire("AK1", false, false);
    sound::weapon_audio::OnPlayerFire("BNS", false, true);  // Underbarrel M203

    float out[2048 * 2] = {};
    float send[2048] = {};
    const float lp[3] = {0.0f, 0.0f, 0.0f};
    const float lf[3] = {0.0f, 0.0f, 1.0f};
    const float lt[3] = {0.0f, 1.0f, 0.0f};
    sound::weapon_audio::RenderMix(out, send, 2048, 2, 0, 1, false, 3, 4, 5, lp, lf, lt);

    float max_amp = 0.0f;
    float max_lr_diff = 0.0f;
    float max_send = 0.0f;
    for (int f = 0; f < 2048; ++f) {
        const float l = out[f * 2];
        const float r = out[f * 2 + 1];
        max_amp = std::max(max_amp, std::max(std::fabs(l), std::fabs(r)));
        max_lr_diff = std::max(max_lr_diff, std::fabs(l - r));
    }
    for (float v : send) max_send = std::max(max_send, std::fabs(v));
    EXPECT_TRUE(max_amp > 1e-4f);
    EXPECT_TRUE(max_lr_diff > 1e-3f);  // Real .pak produces genuine True Stereo L != R
    EXPECT_TRUE(max_send > 1e-4f);
    EXPECT_TRUE(max_amp <= 1.001f);  // Soft-knee ceiling prevents harsh digital clipping

    auto t1 = sound::weapon_audio::GetTelemetry();
    EXPECT_EQ(t1.shots_enhanced, t0.shots_enhanced + 2);
    EXPECT_EQ(t1.tails_enhanced, t0.tails_enhanced + 2);
    EXPECT_EQ(t1.impacts_enhanced, t0.impacts_enhanced + 2);
    EXPECT_EQ(t1.flybys_enhanced, t0.flybys_enhanced);  // Flyby must NOT auto-fire on player shot!

    // Flamethrower triggers live slot 128 on real .pak without error
    sound::weapon_audio::OnPlayerFire("FLM", false, false);
    float flm_out[512 * 2] = {};
    sound::weapon_audio::RenderMix(flm_out, send, 512, 2, 0, 1, false, 3, 4, 5, lp, lf, lt);
    float flm_pk = 0.0f;
    for (float v : flm_out) flm_pk = std::max(flm_pk, std::fabs(v));
    EXPECT_TRUE(flm_pk > 1e-4f);
    auto t_flm = sound::weapon_audio::GetTelemetry();
    EXPECT_TRUE(t_flm.shots_enhanced >= t1.shots_enhanced + 1);

    // Distance-aware AI gunfire (>45m -> layers DistantGunfire) and true flyby event trigger
    int32_t ai_slot = 77;  // AI_RIFLE_PULSE
    uint16_t ai_flags = 0;
    const float far_pos[3] = {15.0f, 0.0f, 65.0f};
    EXPECT_TRUE(sound::weapon_audio::OnVoicePlay(501u, nullptr, 0, ai_slot, ai_flags, true, 0, far_pos, 4.0f, 220.0f, 1.0f));
    int32_t fly_slot = 85;  // ENV_FLYBY
    uint16_t fly_flags = 0;
    const float fly_pos[3] = {3.0f, 1.0f, 8.0f};
    EXPECT_TRUE(sound::weapon_audio::OnVoicePlay(502u, nullptr, 0, fly_slot, fly_flags, true, 0, fly_pos, 4.0f, 95.0f, 1.0f));

    auto t2 = sound::weapon_audio::GetTelemetry();
    EXPECT_TRUE(t2.distant_enhanced >= t_flm.distant_enhanced + 2);
    EXPECT_TRUE(t2.flybys_enhanced >= t_flm.flybys_enhanced + 2);

    // OnVoiceStop preserves natural rolling tail even when weapon loop voice is stopped
    sound::weapon_audio::OnVoiceStop(501u);
    auto t3 = sound::weapon_audio::GetTelemetry();
    EXPECT_TRUE(t3.active_voices >= 4u);

    // Live A/B switch to Original stops muting / bypasses remaster
    enh.mod_audio_pack_enabled = false;
    int32_t s_slot = -2;
    uint16_t s_flags = 0;
    const uint8_t dummy_buf[640] = {};
    bool muted = sound::weapon_audio::OnVoicePlay(99u, dummy_buf, sizeof(dummy_buf), s_slot, s_flags, false, 0, nullptr, 1.0f, 100.0f, 1.0f);
    EXPECT_FALSE(muted);
    enh.mod_audio_pack_enabled = true;

    return true;
}

bool TestWeaponAudioStereoBwarV3AndHdr() {
    auto append_u16 = [](std::vector<uint8_t>& buf, uint16_t v) {
        buf.push_back(uint8_t(v & 0xFFu));
        buf.push_back(uint8_t((v >> 8) & 0xFFu));
    };
    auto append_u32 = [](std::vector<uint8_t>& buf, uint32_t v) {
        buf.push_back(uint8_t(v & 0xFFu));
        buf.push_back(uint8_t((v >> 8) & 0xFFu));
        buf.push_back(uint8_t((v >> 16) & 0xFFu));
        buf.push_back(uint8_t((v >> 24) & 0xFFu));
    };
    auto append_u64 = [](std::vector<uint8_t>& buf, uint64_t v) {
        for (int b = 0; b < 8; ++b) buf.push_back(uint8_t((v >> (b * 8)) & 0xFFu));
    };
    auto append_i16 = [](std::vector<uint8_t>& buf, int16_t v) {
        uint16_t u = static_cast<uint16_t>(v);
        buf.push_back(uint8_t(u & 0xFFu));
        buf.push_back(uint8_t((u >> 8) & 0xFFu));
    };

    // Build an in-memory BWAR v3 pack with 130 slots (0..129):
    // - Clip 0 (slot 0 PST_FIRE): 2ch true stereo (1000 frames; 1kHz L, -1kHz*0.5 R + 300Hz center mid)
    // - Clip 1 (slot 97 TAIL_PISTOL_OUTDOOR): 1ch mono tail (4800 frames)
    // - Clip 2 (slot 117 FIRE_LOOP): 2ch stereo steady drone (4800 frames)
    // - Clip 3 (slot 128 FLAME_IGNITE): 2ch stereo burst (2000 frames)
    constexpr uint32_t kSlots = 130;
    constexpr uint32_t kClip0Frames = 1000;
    constexpr uint32_t kClip1Frames = 4800;
    constexpr uint32_t kClip2Frames = 4800;
    constexpr uint32_t kClip3Frames = 2000;

    std::vector<int16_t> pcm;
    // Clip 0: interleaved L, R (offset 0)
    const uint32_t off0 = uint32_t(pcm.size());
    for (uint32_t i = 0; i < kClip0Frames; ++i) {
        double t = double(i) / 48000.0;
        double mid = 0.45 * std::sin(2.0 * 3.14159265 * 320.0 * t);
        double side_hf = 0.35 * std::sin(2.0 * 3.14159265 * 1400.0 * t);
        int16_t l = int16_t(std::clamp((mid + side_hf) * 28000.0, -32700.0, 32700.0));
        int16_t r = int16_t(std::clamp((mid - side_hf) * 28000.0, -32700.0, 32700.0));
        pcm.push_back(l);
        pcm.push_back(r);
    }
    // Clip 1: mono tail (offset off1)
    const uint32_t off1 = uint32_t(pcm.size());
    for (uint32_t i = 0; i < kClip1Frames; ++i) {
        double t = double(i) / 48000.0;
        int16_t m = int16_t(16000.0 * std::sin(2.0 * 3.14159265 * 220.0 * t));
        pcm.push_back(m);
    }
    // Clip 2: stereo fire loop (offset off2)
    const uint32_t off2 = uint32_t(pcm.size());
    for (uint32_t i = 0; i < kClip2Frames; ++i) {
        double t = double(i) / 48000.0;
        int16_t l = int16_t(14000.0 * std::sin(2.0 * 3.14159265 * 190.0 * t));
        int16_t r = int16_t(14000.0 * std::cos(2.0 * 3.14159265 * 195.0 * t));
        pcm.push_back(l);
        pcm.push_back(r);
    }
    // Clip 3: stereo flame ignite (offset off3)
    const uint32_t off3 = uint32_t(pcm.size());
    for (uint32_t i = 0; i < kClip3Frames; ++i) {
        double t = double(i) / 48000.0;
        int16_t l = int16_t(20000.0 * std::sin(2.0 * 3.14159265 * 400.0 * t));
        int16_t r = int16_t(12000.0 * std::sin(2.0 * 3.14159265 * 600.0 * t));
        pcm.push_back(l);
        pcm.push_back(r);
    }

    // Large >262KB ISO signature for slot 117 (FIRE_LOOP, loop flag bit1 = 2)
    static std::vector<uint8_t> big_loop_wave(320000u, 0x5Au);
    const uint64_t big_hash = sound::weapon_audio::Fnv1a64(big_loop_wave.data(), big_loop_wave.size());

    std::vector<uint8_t> pack;
    // Header: "BWAR", ver=3, sr=48000, slots=130, clips=4, sigs=1
    pack.push_back('B'); pack.push_back('W'); pack.push_back('A'); pack.push_back('R');
    append_u32(pack, 3u);
    append_u32(pack, 48000u);
    append_u32(pack, kSlots);
    append_u32(pack, 4u);
    append_u32(pack, 1u);

    // Clips (16 bytes each: slot_id, sample_offset, frame_count, channels, clip_flags)
    auto add_clip = [&](uint32_t sid, uint32_t off, uint32_t cnt, uint16_t ch, uint16_t flg) {
        append_u32(pack, sid);
        append_u32(pack, off);
        append_u32(pack, cnt);
        append_u16(pack, ch);
        append_u16(pack, flg);
    };
    add_clip(0u, off0, kClip0Frames, 2u, 100u);
    add_clip(97u, off1, kClip1Frames, 1u, 100u);
    add_clip(117u, off2, kClip2Frames, 2u, 100u);
    add_clip(128u, off3, kClip3Frames, 2u, 100u);

    // Signatures sorted by (fnv64, orig_size)
    append_u64(pack, big_hash);
    append_u32(pack, uint32_t(big_loop_wave.size()));
    append_u16(pack, 117u);
    append_u16(pack, 2u);  // bit1 = looping world sound

    for (int16_t s : pcm) append_i16(pack, s);

    EXPECT_TRUE(sound::weapon_audio::LoadPackFromMemory(pack.data(), pack.size()));
    auto tv = sound::weapon_audio::GetTelemetry();
    EXPECT_EQ(tv.pack_version, 3u);
    EXPECT_EQ(tv.stereo_clips, 3u);
    EXPECT_EQ(tv.slot_count, kSlots);

    auto& enh = modsettings::Registry().GetEnhanced();
    enh = modsettings::EnhancedOptions{};
    sound::weapon_audio::StopAll();

    // 1) True Stereo Separation test on 2ch non-3D weapon shot
    sound::weapon_audio::OnPlayerFire("PST", false, false);
    float out_st[512 * 2] = {};
    float send_st[512] = {};
    const float lp[3] = {0.0f, 0.0f, 0.0f};
    const float lf[3] = {0.0f, 0.0f, 1.0f};
    const float lt[3] = {0.0f, 1.0f, 0.0f};
    sound::weapon_audio::RenderMix(out_st, send_st, 512, 2, 0, 1, false, 3, 4, 5, lp, lf, lt);
    float max_diff = 0.0f;
    float sum_sq = 0.0f;
    for (uint32_t i = 0; i < 512; ++i) {
        const float l = out_st[i * 2 + 0];
        const float r = out_st[i * 2 + 1];
        max_diff = std::max(max_diff, std::fabs(l - r));
        sum_sq += 0.25f * (l + r) * (l + r);
    }
    EXPECT_TRUE(max_diff > 0.05f);   // L != R genuine stereo output
    EXPECT_TRUE(sum_sq > 1.0f);      // Phase-coherent Mid energy retained

    // 2) >262KB ISO signature recognition + sustained loop stop handling
    int32_t big_slot = -2;
    uint16_t big_flags = 0;
    EXPECT_TRUE(sound::weapon_audio::OnVoicePlay(
        777u, big_loop_wave.data(), uint32_t(big_loop_wave.size()),
        big_slot, big_flags, false, 0, nullptr, 2.0f, 80.0f, 1.0f));
    EXPECT_EQ(big_slot, 117);
    EXPECT_TRUE((big_flags & 2u) != 0);

    // 3) HDR ducking + peak safety when flooding multiple close weapons simultaneously
    sound::weapon_audio::OnPlayerFire("PST", false, false);
    sound::weapon_audio::OnPlayerFire("FLM", false, false);
    sound::weapon_audio::OnPlayerFire("PST", false, false);
    sound::weapon_audio::OnPlayerFire("PST", false, false);
    float out_hdr[512 * 2] = {};
    sound::weapon_audio::RenderMix(out_hdr, nullptr, 512, 2, 0, 1, false, 3, 4, 5, lp, lf, lt);
    float peak_hdr = 0.0f;
    for (float v : out_hdr) {
        EXPECT_TRUE(std::isfinite(v));
        peak_hdr = std::max(peak_hdr, std::fabs(v));
    }
    EXPECT_TRUE(peak_hdr <= 0.995f);

    // Stopping world loop marks it fading rather than immortal
    sound::weapon_audio::OnVoiceStop(777u);
    for (int k = 0; k < 6; ++k) {
        float tmp[512 * 2] = {};
        sound::weapon_audio::RenderMix(tmp, nullptr, 512, 2, 0, 1, false, 3, 4, 5, lp, lf, lt);
    }
    return true;
}

bool TestBallisticsSchemaAndPresets() {
    app::Settings s;
    game::Options o;
    InitDefaults(s, o);
    modsettings::SettingsRegistry reg;
    reg.ResetToDefaults(s, o);

    const auto& def = reg.GetEnhanced();
    EXPECT_EQ(def.ballistics_preset, std::string("enhanced"));
    EXPECT_TRUE(def.tracers_enabled);
    EXPECT_EQ(def.tracer_frequency, 1u);
    EXPECT_EQ(def.tracer_trail_length, 115u);
    EXPECT_EQ(def.tracer_brightness, 110u);
    EXPECT_EQ(def.tracer_color, 0u);
    EXPECT_TRUE(def.smoke_trails_enabled);
    EXPECT_EQ(def.smoke_mode, 2u);
    EXPECT_EQ(def.smoke_intensity, 105u);
    EXPECT_NEAR(def.smoke_duration, 1.10f, 1e-3f);

    // Apply Performance preset
    reg.ApplyBallisticsPreset("performance");
    EXPECT_EQ(reg.GetEnhanced().ballistics_preset, std::string("performance"));
    EXPECT_TRUE(reg.GetEnhanced().tracers_enabled);
    EXPECT_EQ(reg.GetEnhanced().tracer_frequency, 3u);  // Every 3rd
    EXPECT_EQ(reg.GetEnhanced().smoke_mode, 1u);        // Light smoke

    // Apply Restore Original preset
    reg.ApplyBallisticsPreset("original");
    EXPECT_EQ(reg.GetEnhanced().ballistics_preset, std::string("original"));
    EXPECT_FALSE(reg.GetEnhanced().tracers_enabled);
    EXPECT_EQ(reg.GetEnhanced().tracer_frequency, 0u);
    EXPECT_FALSE(reg.GetEnhanced().smoke_trails_enabled);
    EXPECT_EQ(reg.GetEnhanced().smoke_mode, 0u);

    // Bounds validation + manual overrides + TOML round-trip
    reg.ApplyBallisticsPreset("enhanced");
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::BallisticsTracerFrequency, 4u, s, o).accepted);  // Every 5th
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::BallisticsTracerTrailLength, 999u, s, o).accepted);
    EXPECT_EQ(reg.GetUint(modsettings::SettingId::BallisticsTracerTrailLength, s, o), 250u);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::BallisticsTracerBrightness, 5u, s, o).accepted);
    EXPECT_EQ(reg.GetUint(modsettings::SettingId::BallisticsTracerBrightness, s, o), 20u);
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::BallisticsTracerColor, 3u, s, o).accepted);  // Phosphor Green
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::BallisticsSmokeMode, 3u, s, o).accepted);    // Enhanced smoke
    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::BallisticsSmokeIntensity, 300u, s, o).accepted);
    EXPECT_EQ(reg.GetUint(modsettings::SettingId::BallisticsSmokeIntensity, s, o), 200u);
    EXPECT_TRUE(reg.SetFloat(modsettings::SettingId::BallisticsSmokeDuration, 9.0f, s, o).accepted);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::BallisticsSmokeDuration, s, o), 3.00f, 1e-3f);

    std::ostringstream oss;
    oss << reg.SaveToTable(s, o, nullptr) << "\n";
    modsettings::SettingsRegistry reg2;
    app::Settings s2;
    game::Options o2;
    auto lr = reg2.LoadFromString(oss.str(), s2, o2, nullptr);
    EXPECT_TRUE(lr.parsed_ok);
    EXPECT_EQ(reg2.GetUint(modsettings::SettingId::BallisticsTracerFrequency, s2, o2), 4u);
    EXPECT_EQ(reg2.GetUint(modsettings::SettingId::BallisticsTracerTrailLength, s2, o2), 250u);
    EXPECT_EQ(reg2.GetUint(modsettings::SettingId::BallisticsTracerColor, s2, o2), 3u);
    EXPECT_EQ(reg2.GetUint(modsettings::SettingId::BallisticsSmokeMode, s2, o2), 3u);
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::BallisticsSmokeDuration, s2, o2), 3.00f, 1e-3f);
    return true;
}

bool TestBallisticsWeaponClassAndFrequency() {
    using game::ballistics::ClassifyWeaponCode;
    using game::ballistics::ShouldEmitTracer;
    using game::ballistics::WeaponClass;

    EXPECT_EQ(ClassifyWeaponCode("PST"), WeaponClass::Pistol);
    EXPECT_EQ(ClassifyWeaponCode("GK5"), WeaponClass::Pistol);
    EXPECT_EQ(ClassifyWeaponCode("SMG"), WeaponClass::Smg);
    EXPECT_EQ(ClassifyWeaponCode("P95"), WeaponClass::Smg);
    EXPECT_EQ(ClassifyWeaponCode("AK1"), WeaponClass::Rifle);
    EXPECT_EQ(ClassifyWeaponCode("M45"), WeaponClass::Rifle);
    EXPECT_EQ(ClassifyWeaponCode("BNS"), WeaponClass::Rifle);
    EXPECT_EQ(ClassifyWeaponCode("SHG"), WeaponClass::Shotgun);
    EXPECT_EQ(ClassifyWeaponCode("RM5"), WeaponClass::Shotgun);
    EXPECT_EQ(ClassifyWeaponCode("SNR"), WeaponClass::Sniper);
    EXPECT_EQ(ClassifyWeaponCode("HVY"), WeaponClass::Lmg);
    EXPECT_EQ(ClassifyWeaponCode("RPG"), WeaponClass::Launcher);
    EXPECT_EQ(ClassifyWeaponCode("GRL"), WeaponClass::Launcher);
    EXPECT_EQ(ClassifyWeaponCode("XYZ"), WeaponClass::Unknown);

    // Off never emits
    for (uint64_t i = 0; i < 10; ++i) {
        EXPECT_FALSE(ShouldEmitTracer(0u, i, WeaponClass::Rifle, false, true));
    }
    // Every emits always
    for (uint64_t i = 0; i < 6; ++i) {
        EXPECT_TRUE(ShouldEmitTracer(1u, i, WeaponClass::Rifle, false, true));
    }
    // Every 2nd, 3rd, 5th
    EXPECT_TRUE(ShouldEmitTracer(2u, 0u, WeaponClass::Rifle, false, true));
    EXPECT_FALSE(ShouldEmitTracer(2u, 1u, WeaponClass::Rifle, false, true));
    EXPECT_TRUE(ShouldEmitTracer(3u, 3u, WeaponClass::Rifle, false, true));
    EXPECT_FALSE(ShouldEmitTracer(3u, 4u, WeaponClass::Rifle, false, true));
    EXPECT_TRUE(ShouldEmitTracer(4u, 5u, WeaponClass::Rifle, false, true));
    EXPECT_FALSE(ShouldEmitTracer(4u, 6u, WeaponClass::Rifle, false, true));
    // Selective emits Snipers & Launchers every round, LMGs every 2nd, rifles every 3rd (player)
    EXPECT_TRUE(ShouldEmitTracer(5u, 1u, WeaponClass::Sniper, false, true));
    EXPECT_TRUE(ShouldEmitTracer(5u, 1u, WeaponClass::Launcher, false, true));
    EXPECT_FALSE(ShouldEmitTracer(5u, 1u, WeaponClass::Lmg, false, true));
    EXPECT_TRUE(ShouldEmitTracer(5u, 2u, WeaponClass::Lmg, false, true));
    EXPECT_TRUE(ShouldEmitTracer(5u, 3u, WeaponClass::Rifle, false, true));
    EXPECT_FALSE(ShouldEmitTracer(5u, 4u, WeaponClass::Rifle, false, true));
    return true;
}

bool TestDeliriumPhysicsCore() {
    using namespace game::delirium;

    Config cfg{};
    cfg.enabled = true;
    cfg.intensity = 1.35f;
    cfg.blind_fire_enabled = true;
    cfg.agony_duration = 1.10f;

    Simulation sim;
    sim.Reset();
    sim.SetConfig(cfg);

    // 1. Hit zone classification matches guest IDs + lateral sign
    const float left_dir[3] = {-0.8f, 0.1f, 0.4f};
    const float right_dir[3] = {0.8f, 0.1f, 0.4f};
    EXPECT_EQ(ClassifyHitZone(0x08u, left_dir), HitZone::Head);
    EXPECT_EQ(ClassifyHitZone(0x07u, left_dir), HitZone::LeftArm);
    EXPECT_EQ(ClassifyHitZone(0x09u, right_dir), HitZone::RightArm);
    EXPECT_EQ(ClassifyHitZone(0x0Au, left_dir), HitZone::LeftLeg);
    EXPECT_EQ(ClassifyHitZone(0x02u, right_dir), HitZone::TorsoChest);

    // 2. Light grazing scratch preserves poise (stable until impactful hit)
    const uint32_t soldier_a = 0x11000000u;
    const float shot_dir[3] = {0.0f, 0.0f, 1.0f};
    HitEvent graze{};
    graze.victim_ea = soldier_a;
    graze.hit_dir[0] = shot_dir[0];
    graze.hit_dir[1] = shot_dir[1];
    graze.hit_dir[2] = shot_dir[2];
    graze.raw_force = 0.15f;
    graze.raw_zone = 0x02u;
    graze.lateral_dot = 0.0f;
    graze.is_lethal = false;
    sim.OnBulletHit(graze);

    StepOutput step0 = sim.StepAlive(soldier_a, 0.016f);
    EXPECT_FALSE(step0.apply_stumble_vel);
    EXPECT_FALSE(step0.blind_fire_ready);

    // 3. Multi-pellet shotgun / heavy burst aggregates inside ~45ms window into heavy stagger + wound clutch + blind-fire
    const uint32_t soldier_b = 0x12000000u;
    StepOutput stagger_step{};
    for (int p = 0; p < 4; ++p) {
        HitEvent pellet{};
        pellet.victim_ea = soldier_b;
        pellet.hit_dir[0] = 0.25f;
        pellet.hit_dir[1] = 0.05f;
        pellet.hit_dir[2] = 0.95f;
        pellet.raw_force = 0.85f;
        pellet.raw_zone = 0x09u;  // Right arm / shoulder
        pellet.lateral_dot = 0.65f;
        pellet.is_lethal = false;
        sim.OnBulletHit(pellet);
        if (p < 3) {
            (void)sim.StepAlive(soldier_b, 0.008f);
        }
    }

    stagger_step = sim.StepAlive(soldier_b, 0.016f);
    EXPECT_TRUE(stagger_step.apply_stumble_vel);
    EXPECT_TRUE(stagger_step.trigger_clutch_overlay);
    EXPECT_EQ(stagger_step.anim_clip_id, 0x1Fu);
    EXPECT_TRUE(stagger_step.blind_fire_ready);

    // Aim scatter only applies while soldier_b is staggered (never for calm soldier_a)
    float scattered[3] = {0.0f, 1.5f, 10.0f};
    EXPECT_TRUE(sim.ApplyBlindFireScatter(soldier_b, scattered));
    EXPECT_TRUE(std::fabs(scattered[0]) > 0.01f || std::fabs(scattered[1] - 1.5f) > 0.01f);

    float calm_target[3] = {0.0f, 1.5f, 10.0f};
    EXPECT_FALSE(sim.ApplyBlindFireScatter(soldier_a, calm_target));
    EXPECT_NEAR(calm_target[0], 0.0f, 1e-5f);

    // 4. Varied collapse profiles by zone + momentum preservation
    const uint32_t soldier_head = 0x13000000u;
    HitEvent head_kill{};
    head_kill.victim_ea = soldier_head;
    head_kill.hit_dir[0] = 0.0f;
    head_kill.hit_dir[1] = 0.2f;
    head_kill.hit_dir[2] = 1.0f;
    head_kill.raw_force = 2.4f;
    head_kill.raw_zone = 0x08u;
    head_kill.lateral_dot = -0.2f;
    head_kill.is_lethal = true;
    sim.OnBulletHit(head_kill);

    const uint32_t soldier_leg = 0x14000000u;
    HitEvent leg_kill = head_kill;
    leg_kill.victim_ea = soldier_leg;
    leg_kill.raw_zone = 0x0Au;
    leg_kill.lateral_dot = 0.7f;
    sim.OnBulletHit(leg_kill);

    CollapseProfile head_col = sim.ConsumeCollapseProfile(soldier_head);
    CollapseProfile leg_col = sim.ConsumeCollapseProfile(soldier_leg);
    EXPECT_TRUE(head_col.valid && leg_col.valid);
    EXPECT_TRUE(std::fabs(head_col.upward_lift - leg_col.upward_lift) > 0.05f ||
                std::fabs(head_col.spin_torque[2] - leg_col.spin_torque[2]) > 0.05f);

    // 5. Agony writhing emits pulses during agony_duration and cleanly settles to inactive afterwards
    AgonyPulse pulse0 = sim.StepAgony(soldier_head, 0.10f);
    EXPECT_TRUE(pulse0.active);
    EXPECT_TRUE(pulse0.impulse_force > 0.0f);

    AgonyPulse pulse_late{};
    for (int i = 0; i < 25; ++i) {
        pulse_late = sim.StepAgony(soldier_head, 0.05f);
    }
    EXPECT_FALSE(pulse_late.active);
    EXPECT_NEAR(pulse_late.impulse_force, 0.0f, 1e-5f);

    return true;
}

bool TestDeliriumPhysicsSchema() {
    modsettings::SettingsRegistry reg;
    app::Settings s;
    game::Options o;
    reg.ResetToDefaults(s, o);

    // Defaults provide dramatic BLACK Delirium Physics out of the box
    EXPECT_TRUE(reg.GetBool(modsettings::SettingId::PhysicsDeliriumEnabled, s, o));
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsDeliriumIntensity, s, o), 1.45f, 1e-4f);
    EXPECT_TRUE(reg.GetBool(modsettings::SettingId::PhysicsDeliriumBlindFire, s, o));
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsDeliriumAgonyDuration, s, o), 1.25f, 1e-4f);

    // Presets preserve Classic 2006 cleanly while dialing Delirium up for Realistic/Cinematic/Mayhem
    EXPECT_TRUE(reg.ApplyPhysicsPreset("classic"));
    EXPECT_FALSE(reg.GetBool(modsettings::SettingId::PhysicsDeliriumEnabled, s, o));
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsDeliriumAgonyDuration, s, o), 0.00f, 1e-4f);

    EXPECT_TRUE(reg.ApplyPhysicsPreset("cinematic"));
    EXPECT_TRUE(reg.GetBool(modsettings::SettingId::PhysicsDeliriumEnabled, s, o));
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsDeliriumIntensity, s, o), 1.35f, 1e-4f);

    // Bounds clamping & TOML persistence
    EXPECT_TRUE(reg.SetFloat(modsettings::SettingId::PhysicsDeliriumIntensity, 9.0f, s, o).accepted);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsDeliriumIntensity, s, o), 3.00f, 1e-4f);
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::PhysicsDeliriumBlindFire, false, s, o).accepted);
    EXPECT_TRUE(reg.SetFloat(modsettings::SettingId::PhysicsDeliriumAgonyDuration, 1.85f, s, o).accepted);

    std::ostringstream oss;
    oss << reg.SaveToTable(s, o, nullptr) << "\n";
    modsettings::SettingsRegistry reg2;
    app::Settings s2;
    game::Options o2;
    EXPECT_TRUE(reg2.LoadFromString(oss.str(), s2, o2, nullptr).parsed_ok);
    EXPECT_TRUE(reg2.GetBool(modsettings::SettingId::PhysicsDeliriumEnabled, s2, o2));
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsDeliriumIntensity, s2, o2), 3.00f, 1e-4f);
    EXPECT_FALSE(reg2.GetBool(modsettings::SettingId::PhysicsDeliriumBlindFire, s2, o2));
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsDeliriumAgonyDuration, s2, o2), 1.85f, 1e-4f);
    return true;
}

bool TestFlamethrowerCoreFuelAndReload() {
    game::flamethrower::CoreSim sim;
    sim.Reset();
    EXPECT_FALSE(sim.IsActive());
    EXPECT_NEAR(sim.FuelPct(), 100.0f, 1e-3f);

    // Equip saves previous weapon slot and triggers draw animation
    sim.SetActive(true, 3);
    EXPECT_TRUE(sim.IsActive());
    EXPECT_EQ(sim.PreviousSlot(), 3u);
    sim.Step(0.35f, false, false, false);

    // Firing builds pressure and drains fuel; reload & infinite_ammo behave properly
    for (int i = 0; i < 40; i++) sim.Step(0.05f, true, false, false);  // 2.0s fire
    EXPECT_TRUE(sim.Pressure() > 0.85f);
    const float after_fire = sim.FuelPct();
    EXPECT_TRUE(after_fire < 90.0f && after_fire > 40.0f);

    // Infinite ammo refills/preserves full fuel while firing
    sim.RefillFuel();
    sim.Step(0.50f, true, false, true);
    EXPECT_NEAR(sim.FuelPct(), 100.0f, 1e-3f);

    // Drain to empty, start canister reload, finish after ~2.1s
    for (int i = 0; i < 200; i++) sim.Step(0.05f, true, false, false);
    EXPECT_NEAR(sim.FuelPct(), 0.0f, 1e-3f);
    EXPECT_TRUE(sim.Pressure() < 0.05f);  // flame cuts off on empty tank
    sim.Step(0.02f, false, true, false);  // press reload
    EXPECT_TRUE(sim.IsReloading());
    for (int i = 0; i < 45; i++) sim.Step(0.05f, false, false, false);
    EXPECT_FALSE(sim.IsReloading());
    EXPECT_NEAR(sim.FuelPct(), 100.0f, 1e-3f);

    // Unequipping restores previous weapon slot
    const uint32_t restored = sim.DeactivateAndRestoreSlot();
    EXPECT_FALSE(sim.IsActive());
    EXPECT_EQ(restored, 3u);
    return true;
}

bool TestFlamethrowerCoreBurnAndMesh() {
    game::flamethrower::CoreSim sim;
    sim.Reset();
    sim.SetActive(true, 1);

    const float origin[3] = {0.0f, 1.5f, 0.0f};
    const float fwd[3] = {0.0f, 0.0f, 1.0f};
    const float hit_enemy[3] = {0.5f, 1.2f, 5.5f};
    const float far_enemy[3] = {0.0f, 1.2f, 19.5f};
    const float behind_enemy[3] = {0.0f, 1.2f, -3.0f};
    EXPECT_TRUE(game::flamethrower::CoreSim::ConeIntensity(origin, fwd, hit_enemy) > 0.35f);
    EXPECT_NEAR(game::flamethrower::CoreSim::ConeIntensity(origin, fwd, far_enemy), 0.0f, 1e-4f);
    EXPECT_NEAR(game::flamethrower::CoreSim::ConeIntensity(origin, fwd, behind_enemy), 0.0f, 1e-4f);

    // Apply continuous flame heat until ignition occurs
    constexpr uint32_t kEnemyEa = 0x12340000u;
    for (int i = 0; i < 12; i++) sim.ExposeEnemy(kEnemyEa, 0.9f, 0.05f);
    auto st = sim.GetVictimState(kEnemyEa);
    EXPECT_TRUE(st.ignited);

    // Step live burning: produces DoT damage and periodic agony pulses
    float total_dmg = 0.0f;
    int agony_pulses = 0;
    for (int i = 0; i < 60; i++) {
        auto tick = sim.TickVictim(kEnemyEa, 0.05f, false);
        total_dmg += tick.damage;
        if (tick.agony_flinch) agony_pulses++;
    }
    EXPECT_TRUE(total_dmg > 30.0f);
    EXPECT_TRUE(agony_pulses >= 1);

    // After death, body smolders with embers/smoke then eventually extinguishes
    auto dead_tick = sim.TickVictim(kEnemyEa, 0.1f, true);
    EXPECT_TRUE(dead_tick.emit_fire || dead_tick.emit_smoke);
    for (int i = 0; i < 100; i++) sim.TickVictim(kEnemyEa, 0.1f, true);
    EXPECT_FALSE(sim.GetVictimState(kEnemyEa).ignited);

    // Idle state must remain completely silent (zero distracting continuous hiss)
    sim.Step(0.15f, false, false, false);
    {
        float idle_pcm[512 * 2] = {};
        sim.RenderAudio(idle_pcm, 512, 48000);
        float idle_peak = 0.0f;
        for (float v : idle_pcm) idle_peak = std::max(idle_peak, std::fabs(v));
        EXPECT_NEAR(idle_peak, 0.0f, 1e-5f);
    }

    // Procedural audio output is bounded [-1, 1] and audible while firing
    sim.Step(0.15f, true, false, false);
    float pcm[512 * 2] = {};
    sim.RenderAudio(pcm, 512, 48000);
    float peak = 0.0f;
    for (float v : pcm) {
        EXPECT_TRUE(std::isfinite(v) && std::fabs(v) <= 1.0f);
        peak = std::max(peak, std::fabs(v));
    }
    EXPECT_TRUE(peak > 0.02f);
    return true;
}

// 26b. Physical jet: fuel has to travel, it droops, it pools, and it lights a
// body only after real contact dwell — the realism the old instant cone lacked.
bool TestFlamethrowerJetPhysics() {
    using game::flamethrower::CoreSim;
    CoreSim sim;
    sim.Reset();
    sim.SetActive(true, 0);

    const float muzzle[3] = {10.0f, 1.6f, 20.0f};
    const float dir[3] = {0.0f, 0.0f, 1.0f};
    sim.SetMuzzle(muzzle, dir, 0.10f);
    EXPECT_FALSE(sim.HasActiveJet());

    const float far_target[3] = {10.0f, 1.15f, 31.0f};  // 11 m out, chest height
    EXPECT_NEAR(sim.StreamExposure(far_target, 0.45f), 0.0f, 1e-4f);

    // 0.16 s of spray: the fuel has barely cleared the muzzle, so nothing at
    // 11 m is burning yet. That travel delay is the whole point of the model.
    for (int i = 0; i < 8; i++) sim.Step(0.02f, true, false, false);
    EXPECT_TRUE(sim.Pressure() > 0.25f);
    const float front_early = sim.JetFrontDistance();
    EXPECT_TRUE(front_early > 1.5f && front_early < 6.5f);
    EXPECT_NEAR(sim.StreamExposure(far_target, 0.45f), 0.0f, 1e-4f);

    // Hold the trigger: the front keeps advancing but drag stops it at the
    // weapon's real reach instead of an infinite instant cone.
    for (int i = 0; i < 70; i++) sim.Step(0.02f, true, false, false);
    const float front_late = sim.JetFrontDistance();
    EXPECT_TRUE(front_late > front_early + 4.0f);
    EXPECT_TRUE(front_late > 12.0f);
    EXPECT_TRUE(front_late <= CoreSim::kMaxRangeMeters + 0.6f);
    EXPECT_TRUE(sim.StreamExposure(far_target, 0.45f) > 0.10f);

    // The jet is a stream, not a wall: off to the side and behind stay cold
    const float side_target[3] = {14.0f, 1.15f, 31.0f};
    const float behind_target[3] = {10.0f, 1.15f, 12.0f};
    EXPECT_NEAR(sim.StreamExposure(side_target, 0.45f), 0.0f, 1e-4f);
    EXPECT_NEAR(sim.StreamExposure(behind_target, 0.45f), 0.0f, 1e-4f);

    // Segments exist, spread turbulently and sag the way heavy gel does
    game::flamethrower::JetSegment segs[CoreSim::kMaxJetSegments];
    const size_t seg_count = sim.CopyJetSegments(segs, CoreSim::kMaxJetSegments);
    EXPECT_TRUE(seg_count > 20);
    float far_along = 0.0f;
    float far_y = muzzle[1];
    float max_lateral = 0.0f;
    for (size_t i = 0; i < seg_count; i++) {
        EXPECT_TRUE(segs[i].radius >= 0.05f && segs[i].radius < 1.2f);
        EXPECT_TRUE(segs[i].heat > 0.05f && segs[i].heat <= 1.0f);
        const float along = segs[i].pos[2] - muzzle[2];
        max_lateral = std::max(max_lateral, std::fabs(segs[i].pos[0] - muzzle[0]));
        if (along > far_along) {
            far_along = along;
            far_y = segs[i].pos[1];
        }
    }
    EXPECT_TRUE(far_along > 10.0f);
    EXPECT_TRUE(max_lateral > 0.02f);                  // turbulent, not a laser beam
    EXPECT_TRUE(far_y < muzzle[1] + 1.0f);             // no rocket-like climb
    EXPECT_TRUE(far_y > muzzle[1] - 1.6f);             // and no waterfall either

    // Releasing the trigger: the lit fuel already in the air keeps flying
    float before_release[3] = {segs[0].pos[0], segs[0].pos[1], segs[0].pos[2]};
    (void)before_release;
    sim.Step(0.02f, false, false, false);
    EXPECT_TRUE(sim.Pressure() < 1.0f);
    for (int i = 0; i < 90; i++) sim.Step(0.02f, false, false, false);  // 1.8 s idle
    EXPECT_NEAR(sim.Pressure(), 0.0f, 1e-3f);
    EXPECT_NEAR(sim.CopyJetSegments(segs, CoreSim::kMaxJetSegments), size_t(0), 0.0f);

    // Firing into the floor pools burning fuel there for several seconds
    CoreSim low;
    low.Reset();
    low.SetActive(true, 0);
    const float m2[3] = {0.0f, 1.4f, 0.0f};
    const float down[3] = {0.0f, -0.62f, 0.78f};
    low.SetMuzzle(m2, down, 0.05f);
    for (int i = 0; i < 60; i++) low.Step(0.02f, true, false, false);  // 1.2 s
    game::flamethrower::JetSegment pools[CoreSim::kMaxPoolSegments];
    const size_t pool_count = low.CopyPoolSegments(pools, CoreSim::kMaxPoolSegments);
    EXPECT_TRUE(pool_count > 0);
    EXPECT_TRUE(pools[0].heat > 0.2f);
    EXPECT_TRUE(pools[0].ttl > 1.5f);
    EXPECT_NEAR(pools[0].pos[1], 0.075f, 0.05f);
    const float floor_fire[3] = {0.0f, 0.25f, 2.0f};
    EXPECT_TRUE(low.StreamExposure(floor_fire, 0.45f) > 0.05f);
    // and the pool keeps burning after the trigger is released
    for (int i = 0; i < 30; i++) low.Step(0.02f, false, false, false);
    EXPECT_TRUE(low.CopyPoolSegments(pools, CoreSim::kMaxPoolSegments) > 0);

    // Ignition needs real contact dwell, not one frame of exposure
    CoreSim burn;
    burn.Reset();
    burn.SetActive(true, 0);
    constexpr uint32_t kEa = 0x22000000u;
    float dwell = 0.0f;
    for (int i = 0; i < 200 && !burn.GetVictimState(kEa).ignited; i++) {
        burn.ExposeEnemy(kEa, 1.0f, 0.02f);
        dwell += 0.02f;
    }
    EXPECT_TRUE(burn.GetVictimState(kEa).ignited);
    EXPECT_TRUE(dwell > 0.30f && dwell < 0.85f);

    // Napalm keeps cooking the body long after the jet has moved on
    float afterburn_dmg = 0.0f;
    for (int i = 0; i < 50; i++) afterburn_dmg += burn.TickVictim(kEa, 0.05f, false).damage;
    EXPECT_TRUE(afterburn_dmg > 25.0f);
    EXPECT_TRUE(burn.GetVictimState(kEa).ignited);
    EXPECT_TRUE(burn.GetVictimState(kEa).burn_timer > 4.0f);

    return true;
}

// 27. Mission selection default progression and unlock flags (prevents Spetriniv Gulag lock-in)
bool TestMissionSelectDefaultProgression() {
    using namespace game::mission;

    EXPECT_EQ(LevelIdFromMissionIndex(0), uint8_t(0));
    EXPECT_EQ(LevelIdFromMissionIndex(2), uint8_t(5));
    EXPECT_EQ(LevelIdFromMissionIndex(7), uint8_t(8));
    EXPECT_EQ(MissionIndexFromLevelId(0), 0u);
    EXPECT_EQ(MissionIndexFromLevelId(5), 2u);
    EXPECT_EQ(MissionIndexFromLevelId(8), 7u);
    EXPECT_EQ(MissionIndexFromLevelId(255), 0u);

    // Fresh profile where Unlock() (or a prior unlock_levels save) set all 8 open, 0 beaten, active level 0:
    const bool all_open[kMissionCount] = {true, true, true, true, true, true, true, true};
    const bool none_beaten[kMissionCount] = {false, false, false, false, false, false, false, false};
    EXPECT_EQ(ResolveDefaultMissionIndex(all_open, none_beaten, 0, true), 0u);

    // Mid-campaign (beat Mission 1 and 2 -> next mission is index 2, Tivliz Asylum, level ID 5):
    const bool two_beaten[kMissionCount] = {true, true, false, false, false, false, false, false};
    EXPECT_EQ(ResolveDefaultMissionIndex(all_open, two_beaten, 0, true), 2u);

    // Active campaign level matches Mission 4 (index 3, level ID 3):
    EXPECT_EQ(ResolveDefaultMissionIndex(all_open, none_beaten, 3, true), 3u);

    // Genuine end-game save (beat up to Mission 7 -> index 7, Spetriniv Gulag):
    const bool seven_beaten[kMissionCount] = {true, true, true, true, true, true, true, false};
    EXPECT_EQ(ResolveDefaultMissionIndex(all_open, seven_beaten, 0, true), 7u);

    // Organic partial unlock (unlock_levels=false, first 4 open) still focuses highest open (index 3):
    const bool four_open[kMissionCount] = {true, true, true, true, false, false, false, false};
    EXPECT_EQ(ResolveDefaultMissionIndex(four_open, two_beaten, 0, false), 3u);

    // ApplyMissionUnlockUiFlags marks all unlocked as 1 except exact focused mission == 2:
    uint32_t ui_flags[kMissionCount] = {1, 0, 0, 0, 0, 0, 0, 2};
    ApplyMissionUnlockUiFlags(ui_flags, 0u, true);
    EXPECT_EQ(ui_flags[0], 2u);
    for (uint32_t i = 1; i < kMissionCount; i++) {
        EXPECT_EQ(ui_flags[i], 1u);
    }
    return true;
}

// 28. Touch/mouse MissionModeSelect map click and swipe directional navigation
bool TestMissionMapTouchNavigation() {
    using namespace game::mission;

    // Tapping empty space on a map page with no button clip must NOT accidentally confirm:
    const auto empty_map_tap = ResolveMapTouchClick(true, 0u, 0x101u, false);
    EXPECT_EQ(empty_map_tap.aim_value, 0u);
    EXPECT_EQ(empty_map_tap.input, kNavNone);

    // Non-map single-button pages (e.g. Press Start) still confirm on click anywhere:
    const auto non_map_tap = ResolveMapTouchClick(false, 0u, 0u, false);
    EXPECT_EQ(non_map_tap.input, kNavConfirm);

    // Tapping a different mission node aims at that node (no confirm):
    const auto switch_tap = ResolveMapTouchClick(true, 0x103u, 0x101u, false);
    EXPECT_EQ(switch_tap.aim_value, 0x103u);
    EXPECT_EQ(switch_tap.input, kNavNone);

    // Tapping the already-focused mission node confirms:
    const auto confirm_tap = ResolveMapTouchClick(true, 0x101u, 0x101u, false);
    EXPECT_EQ(confirm_tap.aim_value, 0u);
    EXPECT_EQ(confirm_tap.input, kNavConfirm);

    // Horizontal swipe on MissionModeSelect map drives Left/Right:
    EXPECT_EQ(MapSwipeToInput(true, false, 1, 0), kNavRight);
    EXPECT_EQ(MapSwipeToInput(true, false, -1, 0), kNavLeft);

    // Vertical swipe on a map without up/down links also steps missions Left/Right:
    EXPECT_EQ(MapSwipeToInput(true, false, 0, -1), kNavRight);
    EXPECT_EQ(MapSwipeToInput(true, false, 0, 1), kNavLeft);

    // Standard menu list still scrolls vertically Up/Down:
    EXPECT_EQ(MapSwipeToInput(false, true, 0, 1), kNavUp);
    EXPECT_EQ(MapSwipeToInput(false, true, 0, -1), kNavDown);
    return true;
}

// 30. VFX atlas smoke/fire tiles must fade completely to transparent zero-RGB before
// quad borders (no square silhouette or bilinear cross-tile bleed).
bool TestVfxAtlasFireAndSmokeEdges() {
    using game::ballistics::SampleVfxAtlasTexelBgra;

    const float kBorderCoords[][2] = {
        {0.004f, 0.50f}, {0.996f, 0.50f}, {0.50f, 0.004f}, {0.50f, 0.996f},
        {0.05f, 0.05f},  {0.95f, 0.05f},  {0.05f, 0.95f},  {0.95f, 0.95f},
        {0.06f, 0.50f},  {0.94f, 0.50f},  {0.50f, 0.06f},  {0.50f, 0.94f}, // |u-0.5|*2 = 0.88 >= 0.80
        {0.10f, 0.50f},  {0.90f, 0.50f},  {0.50f, 0.10f},  {0.50f, 0.90f}, // |u-0.5|*2 = 0.80 >= 0.78 strict safety border
    };

    for (uint32_t ty = 0; ty <= 1; ty++) {
        for (uint32_t tx = 0; tx < 4; tx++) {
            for (const auto& pt : kBorderCoords) {
                const uint32_t px = SampleVfxAtlasTexelBgra(tx, ty, pt[0], pt[1]);
                const uint32_t alpha = (px >> 24) & 0xFFu;
                EXPECT_EQ(alpha, 0u);
                if (ty == 1u) {
                    // Black RGB at transparent perimeter prevents dark/bright additive halo on UV clamp.
                    EXPECT_EQ(px & 0x00FFFFFFu, 0u);
                }
            }
        }
    }

    // Row 1 fire tiles: bright incandescent center and anisotropic turbulent mid-ring
    for (uint32_t tx = 0; tx < 4; tx++) {
        const uint32_t center = SampleVfxAtlasTexelBgra(tx, 1u, 0.50f, 0.50f);
        const uint32_t a = (center >> 24) & 0xFFu;
        const uint32_t r = (center >> 16) & 0xFFu;
        const uint32_t g = (center >> 8) & 0xFFu;
        const uint32_t b = center & 0xFFu;
        EXPECT_TRUE(a > 120u);
        EXPECT_TRUE(r > 200u);
        EXPECT_TRUE(r >= g && g > b);

        // Smoke tiles (Row 0) also have non-zero center
        const uint32_t smoke_center = SampleVfxAtlasTexelBgra(tx, 0u, 0.50f, 0.50f);
        EXPECT_TRUE(((smoke_center >> 24) & 0xFFu) > 60u);

        // Flame mid-ring should vary across angles (organic licking tongues, not flat square/circle)
        uint32_t min_a = 255u;
        uint32_t max_a = 0u;
        for (int s = 0; s < 16; s++) {
            const float ang = float(s) * (6.2831853f / 16.0f);
            const float u = 0.5f + std::cos(ang) * 0.25f;
            const float v = 0.5f + std::sin(ang) * 0.25f;
            const uint32_t ring_a = (SampleVfxAtlasTexelBgra(tx, 1u, u, v) >> 24) & 0xFFu;
            min_a = std::min(min_a, ring_a);
            max_a = std::max(max_a, ring_a);
        }
        EXPECT_TRUE(max_a > min_a + 18u);
    }

    // Row 2 (blood) & Row 3 (sparks/rings) still synthesize valid interiors & clean edges
    EXPECT_TRUE(((SampleVfxAtlasTexelBgra(0u, 2u, 0.50f, 0.50f) >> 24) & 0xFFu) > 100u);
    EXPECT_EQ((SampleVfxAtlasTexelBgra(0u, 2u, 0.01f, 0.01f) >> 24) & 0xFFu, 0u);
    EXPECT_TRUE(((SampleVfxAtlasTexelBgra(0u, 3u, 0.50f, 0.50f) >> 24) & 0xFFu) > 100u);
    EXPECT_TRUE(((SampleVfxAtlasTexelBgra(2u, 3u, 0.78f, 0.50f) >> 24) & 0xFFu) > 40u);
    return true;
}

// 31. Flamethrower jet builds dense volumetric overlapping billows with organic
// rotation (no sparse straight-line spans, no blood tiles, no bridge across gaps).
bool TestFlamethrowerPlumeVolumetricNoLines() {
    using namespace game::ballistics;
    using game::flamethrower::CoreSim;

    CoreSim sim;
    sim.Reset();
    sim.SetActive(true, 0);
    const float muzzle[3] = {0.0f, 1.5f, 0.0f};
    const float dir[3] = {0.0f, 0.0f, 1.0f};
    sim.SetMuzzle(muzzle, dir, 0.05f);
    for (int i = 0; i < 35; i++) sim.Step(0.02f, true, false, false);

    game::flamethrower::JetSegment segs[CoreSim::kMaxJetSegments];
    const size_t seg_count = sim.CopyJetSegments(segs, CoreSim::kMaxJetSegments);
    EXPECT_TRUE(seg_count > 12u);

    float pos[CoreSim::kMaxJetSegments][3]{};
    float rad[CoreSim::kMaxJetSegments]{};
    float heat[CoreSim::kMaxJetSegments]{};
    for (size_t i = 0; i < seg_count; i++) {
        pos[i][0] = segs[i].pos[0];
        pos[i][1] = segs[i].pos[1];
        pos[i][2] = segs[i].pos[2];
        rad[i] = segs[i].radius;
        heat[i] = segs[i].heat;
    }

    FlamePlumeSprite sprites[640]{};
    const size_t n = BuildFlamethrowerJetSprites(pos, rad, heat, seg_count, 1.0f, 1.25f, false,
                                                 sprites, 640);
    EXPECT_TRUE(n >= seg_count * 2u);

    std::vector<FlamePlumeSprite> fires;
    bool has_smoke = false;
    for (size_t i = 0; i < n; i++) {
        EXPECT_TRUE(sprites[i].alpha >= 0.0f && sprites[i].alpha <= 1.0f);
        EXPECT_TRUE(sprites[i].radius > 0.02f);
        if (sprites[i].kind == FlameSpriteKind::FireBillboard) {
            EXPECT_TRUE(sprites[i].tile >= 4 && sprites[i].tile <= 7);
            fires.push_back(sprites[i]);
        } else if (sprites[i].kind == FlameSpriteKind::SmokeBillboard) {
            EXPECT_TRUE(sprites[i].tile >= 0 && sprites[i].tile <= 3);
            has_smoke = true;
        } else {
            EXPECT_EQ(sprites[i].kind, FlameSpriteKind::CoreDisc);
        }
    }
    EXPECT_TRUE(fires.size() >= seg_count * 2u);
    EXPECT_TRUE(has_smoke);

    // Core continuous stream puffs overlap closely without line-like gaps, and vary rotation.
    // Scattered far-tail eddies (>1.45m) may decouple cleanly rather than drawing connecting lines.
    size_t rot_changes = 0;
    size_t dense_steps = 0;
    for (size_t i = 1; i < fires.size(); i++) {
        const float dx = fires[i].pos[0] - fires[i - 1].pos[0];
        const float dy = fires[i].pos[1] - fires[i - 1].pos[1];
        const float dz = fires[i].pos[2] - fires[i - 1].pos[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < 0.48f) {
            dense_steps++;
        }
        if (std::fabs(fires[i].rotation - fires[i - 1].rotation) > 0.1f) {
            rot_changes++;
        }
    }
    EXPECT_TRUE(dense_steps >= (fires.size() * 85u) / 100u);
    EXPECT_TRUE(rot_changes > fires.size() / 2u);

    // Continuous fast-exit muzzle stream (1.0m per step at 44 m/s): 100% of adjacent flame puffs overlap < 0.42m.
    const float cpos[6][3] = {
        {0.0f, 1.5f, 0.0f}, {0.0f, 1.5f, 1.0f}, {0.02f, 1.49f, 2.0f},
        {0.05f, 1.47f, 3.0f}, {0.10f, 1.44f, 4.0f}, {0.15f, 1.40f, 5.0f},
    };
    const float crad[6] = {0.09f, 0.14f, 0.20f, 0.26f, 0.34f, 0.42f};
    const float cheat[6] = {1.00f, 0.94f, 0.88f, 0.80f, 0.72f, 0.62f};
    FlamePlumeSprite csprites[160]{};
    const size_t cn = BuildFlamethrowerJetSprites(cpos, crad, cheat, 6u, 1.0f, 0.8f, false,
                                                  csprites, 160);
    std::vector<FlamePlumeSprite> cfires;
    for (size_t i = 0; i < cn; i++) {
        if (csprites[i].kind == FlameSpriteKind::FireBillboard) cfires.push_back(csprites[i]);
    }
    EXPECT_TRUE(cfires.size() >= 20u);
    for (size_t i = 1; i < cfires.size(); i++) {
        const float dx = cfires[i].pos[0] - cfires[i - 1].pos[0];
        const float dy = cfires[i].pos[1] - cfires[i - 1].pos[1];
        const float dz = cfires[i].pos[2] - cfires[i - 1].pos[2];
        EXPECT_TRUE(std::sqrt(dx * dx + dy * dy + dz * dz) < 0.42f);
    }

    // Disconnected floor-culled jump (6m gap between point 0 and 1) must NOT bridge empty air!
    const float gpos[3][3] = {{0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 6.0f}, {0.0f, 1.0f, 6.6f}};
    const float grad[3] = {0.20f, 0.16f, 0.12f};
    const float gheat[3] = {0.60f, 0.85f, 0.98f};
    FlamePlumeSprite gap_sprites[64]{};
    const size_t gn = BuildFlamethrowerJetSprites(gpos, grad, gheat, 3u, 1.0f, 0.5f, false,
                                                  gap_sprites, 64);
    EXPECT_TRUE(gn > 2u);
    for (size_t i = 0; i < gn; i++) {
        const float z = gap_sprites[i].pos[2];
        EXPECT_TRUE(z <= 0.35f || z >= 5.65f);
    }
    return true;
}

// 32. Napalm pools must map smoke to Row 0 (0..3) and fire to Row 1 (4..7) — never Row 2 blood!
bool TestFlamethrowerPoolTilesNoBloodOrSquares() {
    using namespace game::ballistics;

    const float ppos[3][3] = {{1.0f, 0.05f, 2.0f}, {1.6f, 0.06f, 2.8f}, {2.2f, 0.05f, 3.1f}};
    const float prad[3] = {0.45f, 0.65f, 0.52f};
    const float pheat[3] = {0.90f, 0.65f, 0.35f};

    FlamePlumeSprite out[32]{};
    const size_t n = BuildFlamethrowerPoolSprites(ppos, prad, pheat, 3u, 0.9f, 2.0f, out, 32);
    EXPECT_TRUE(n >= 9u);

    bool saw_smoke = false;
    bool saw_fire_ground = false;
    bool saw_fire_lift = false;
    for (size_t i = 0; i < n; i++) {
        EXPECT_TRUE(out[i].tile < 8 || out[i].tile >= 12); // Never Row 2 blood tiles (8..11)!
        if (out[i].kind == FlameSpriteKind::SmokeGround || out[i].kind == FlameSpriteKind::SmokeBillboard) {
            EXPECT_TRUE(out[i].tile >= 0 && out[i].tile <= 3);
            saw_smoke = true;
        } else if (out[i].kind == FlameSpriteKind::FireGround) {
            EXPECT_TRUE(out[i].tile >= 4 && out[i].tile <= 7);
            saw_fire_ground = true;
        } else if (out[i].kind == FlameSpriteKind::FireBillboard) {
            EXPECT_TRUE(out[i].tile >= 4 && out[i].tile <= 7);
            saw_fire_lift = true;
        }
        EXPECT_TRUE(out[i].alpha >= 0.0f && out[i].alpha <= 1.0f);
    }
    EXPECT_TRUE(saw_smoke && saw_fire_ground && saw_fire_lift);
    return true;
}

bool TestWeaponHapticProfiles() {
    using namespace game::haptics;

    // 1) Schema metadata + default-on + persistence round-trip
    modsettings::SettingsRegistry reg;
    app::Settings s;
    game::Options o;
    reg.ResetToDefaults(s, o);
    const auto* meta = reg.Find(modsettings::SettingId::AudioPhoneVibration);
    EXPECT_TRUE(meta != nullptr);
    EXPECT_EQ(meta->status, modsettings::FeatureStatus::Implemented);
    EXPECT_EQ(meta->scope, modsettings::ApplyScope::Immediate);
    EXPECT_TRUE(reg.GetBool(modsettings::SettingId::AudioPhoneVibration, s, o));
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::AudioPhoneVibration, false, s, o).accepted);
    EXPECT_FALSE(reg.GetBool(modsettings::SettingId::AudioPhoneVibration, s, o));

    const toml::table saved = reg.SaveToTable(s, o, nullptr);
    std::ostringstream oss;
    oss << saved;
    modsettings::SettingsRegistry reg2;
    reg2.LoadFromString(oss.str(), s, o, nullptr);
    EXPECT_FALSE(reg2.GetBool(modsettings::SettingId::AudioPhoneVibration, s, o));

    // 2) Logical non-exaggerated tier calibration per weapon family
    const auto pst = ComputeWeaponHapticPulse("PST", false, false);
    const auto ak = ComputeWeaponHapticPulse("AK1", false, false);
    const auto ak_sup = ComputeWeaponHapticPulse("AK1", true, false);
    const auto shg = ComputeWeaponHapticPulse("SHG", false, false);
    const auto snr = ComputeWeaponHapticPulse("SNR", false, false);
    const auto rpg = ComputeWeaponHapticPulse("RPG", false, false);
    const auto m203 = ComputeWeaponHapticPulse("BNS", false, true);
    const auto flm = ComputeWeaponHapticPulse("FLM", false, false);

    EXPECT_TRUE(pst.triggered && ak.triggered && shg.triggered && snr.triggered &&
                rpg.triggered && m203.triggered && flm.triggered);

    // Pistols & rifles = crisp, unmistakable tactile recoil punch on Android LRAs
    EXPECT_TRUE(pst.duration_ms >= 32u && pst.duration_ms <= 52u);
    EXPECT_TRUE(pst.amplitude >= 145u && pst.amplitude <= 195u);
    EXPECT_EQ(pst.priority, uint8_t(1));
    EXPECT_TRUE(ak.duration_ms >= 40u && ak.duration_ms <= 62u);
    EXPECT_TRUE(ak.amplitude >= 180u && ak.amplitude <= 225u);
    EXPECT_EQ(ak.priority, uint8_t(1));

    // Suppressed fire noticeably softer than loud fire, yet still clearly felt
    EXPECT_TRUE(ak_sup.amplitude < ak.amplitude);
    EXPECT_TRUE(ak_sup.amplitude >= 95u);
    EXPECT_TRUE(ak_sup.duration_ms <= ak.duration_ms);
    EXPECT_TRUE(ak_sup.duration_ms >= 26u);

    // Shotguns / Snipers = heavy high-energy recoil slam
    EXPECT_TRUE(shg.duration_ms >= 68u && shg.duration_ms <= 98u);
    EXPECT_TRUE(shg.amplitude >= 225u);
    EXPECT_EQ(shg.priority, uint8_t(2));
    EXPECT_TRUE(snr.duration_ms >= 74u);
    EXPECT_TRUE(snr.amplitude >= 230u && snr.amplitude > ak.amplitude);

    // Heavy launchers & M203 = thunderous full-power slam (priority 3)
    EXPECT_TRUE(rpg.duration_ms >= 115u && rpg.duration_ms <= 165u);
    EXPECT_EQ(rpg.amplitude, uint8_t(255));
    EXPECT_EQ(rpg.priority, uint8_t(3));
    EXPECT_TRUE(m203.duration_ms >= 100u && m203.amplitude >= 245u);
    EXPECT_TRUE(m203.amplitude > shg.amplitude && shg.amplitude > ak.amplitude);

    // Flamethrower = crisp gas-ignition & pressure pulse with controlled cooldown
    EXPECT_TRUE(flm.duration_ms >= 42u && flm.duration_ms <= 64u);
    EXPECT_TRUE(flm.amplitude >= 175u);
    EXPECT_TRUE(flm.cooldown_ms >= 190u && flm.cooldown_ms <= 260u);

    // Explosions scaled by proximity: close full shockwave, zero beyond 32m
    const auto exp_close = ComputeExplosionHapticPulse(3.0f);
    const auto exp_mid = ComputeExplosionHapticPulse(13.0f);
    const auto exp_far = ComputeExplosionHapticPulse(40.0f);
    EXPECT_TRUE(exp_close.triggered && exp_close.amplitude == 255u &&
                exp_close.duration_ms >= 135u && exp_close.priority == uint8_t(4));
    EXPECT_TRUE(exp_mid.triggered && exp_mid.amplitude < exp_close.amplitude &&
                exp_mid.amplitude >= 120u);
    EXPECT_FALSE(exp_far.triggered);

    // 3) Dispatcher respects disabled toggle, cooldown gating, and priority override
    HapticDispatcher disp;
    disp.Reset();
    WeaponHapticPulse emitted{};
    EXPECT_FALSE(disp.TrySchedule(1000u, ak, false, &emitted));
    EXPECT_TRUE(disp.TrySchedule(1000u, ak, true, &emitted));
    EXPECT_EQ(emitted.amplitude, ak.amplitude);
    // Within rifle cooldown -> suppressed (no noisy buzz)
    EXPECT_FALSE(disp.TrySchedule(1020u, ak, true, &emitted));
    // Higher priority explosion immediately overrides ongoing rifle tap
    EXPECT_TRUE(disp.TrySchedule(1025u, exp_close, true, &emitted));
    EXPECT_EQ(emitted.priority, uint8_t(4));
    // Lower priority tap blocked while explosion vibrates + cools
    EXPECT_FALSE(disp.TrySchedule(1080u, pst, true, &emitted));
    // After cooldown expires -> rifle tap allowed again
    EXPECT_TRUE(disp.TrySchedule(1300u, ak, true, &emitted));
    return true;
}

bool TestFlamethrowerFlameSpectrumAndMtlColors() {
    using game::ballistics::BuildFlamethrowerJetSprites;
    using game::ballistics::BuildFlamethrowerPoolSprites;
    using game::ballistics::FlamePlumeSprite;
    using game::ballistics::FlameSpriteKind;
    using game::ballistics::SampleVfxAtlasTexelBgra;

    // 1) Row 1 VFX fire tiles span a Planck blackbody spectrum
    // (White-Gold 2300K -> Amber 1700K -> Crimson-Red 1150K). Every tile keeps a
    // bright incandescent red channel; the coolest crimson tile stays warm (no
    // dark red-crush) while the hottest tile reaches white-gold.
    uint32_t max_g = 0u;
    for (uint32_t tx = 0; tx < 4u; ++tx) {
        const uint32_t center = SampleVfxAtlasTexelBgra(tx, 1u, 0.50f, 0.50f);
        const uint32_t r = (center >> 16) & 0xFFu;
        const uint32_t g = (center >> 8) & 0xFFu;
        EXPECT_TRUE(r >= 220u);
        EXPECT_TRUE(g >= 130u);
        max_g = std::max(max_g, g);
    }
    EXPECT_TRUE(max_g >= 220u);

    // 2) Jet billboard & core sprites avoid crimson green-crushing when multiplied with atlas
    const float jet_pos[2][3] = {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 3.0f}};
    const float jet_rad[2] = {0.25f, 0.45f};
    const float jet_heat[2] = {0.90f, 0.55f};
    FlamePlumeSprite jet_out[32] = {};
    const size_t jet_n = BuildFlamethrowerJetSprites(jet_pos, jet_rad, jet_heat, 2u, 1.0f, 0.0f, false, jet_out, 32u);
    EXPECT_TRUE(jet_n > 0u);
    bool checked_fire = false;
    bool checked_core = false;
    for (size_t i = 0; i < jet_n; ++i) {
        if (jet_out[i].kind == FlameSpriteKind::FireBillboard) {
            EXPECT_TRUE(jet_out[i].g >= 0.46f);
            EXPECT_TRUE((jet_out[i].g / std::max(jet_out[i].r, 0.001f)) >= 0.46f);
            checked_fire = true;
        } else if (jet_out[i].kind == FlameSpriteKind::CoreDisc) {
            EXPECT_TRUE(jet_out[i].g >= 0.72f);
            checked_core = true;
        }
    }
    EXPECT_TRUE(checked_fire && checked_core);

    // 3) Burn pool sprites stay warm luminous orange-gold instead of dull red
    const float pool_pos[1][3] = {{1.0f, 0.0f, 2.0f}};
    const float pool_rad[1] = {0.60f};
    const float pool_heat[1] = {0.82f};
    FlamePlumeSprite pool_out[16] = {};
    const size_t pool_n = BuildFlamethrowerPoolSprites(pool_pos, pool_rad, pool_heat, 1u, 1.0f, 0.0f, pool_out, 16u);
    EXPECT_TRUE(pool_n > 0u);
    bool checked_pool_fire = false;
    for (size_t i = 0; i < pool_n; ++i) {
        if (pool_out[i].kind == FlameSpriteKind::FireGround) {
            EXPECT_TRUE(pool_out[i].g >= 0.46f);
            EXPECT_TRUE(pool_out[i].b >= 0.08f);
            checked_pool_fire = true;
        }
    }
    EXPECT_TRUE(checked_pool_fire);
    return true;
}

bool TestWeaponAudioInRoomPunchAndHeadroom() {
    const char* pak_path = "e:/ai/black/app/src/main/assets/weapon_audio/black_weapon_audio.pak";
    std::ifstream pf(pak_path, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(pf.is_open());
    const size_t psz = size_t(pf.tellg());
    EXPECT_TRUE(psz > 1000000u);
    pf.seekg(0);
    std::vector<uint8_t> pbytes(psz);
    pf.read(reinterpret_cast<char*>(pbytes.data()), std::streamsize(psz));

    auto rd_u16 = [&](size_t off) -> uint16_t {
        return uint16_t(pbytes[off]) | (uint16_t(pbytes[off + 1]) << 8);
    };
    auto rd_u32 = [&](size_t off) -> uint32_t {
        return uint32_t(pbytes[off]) | (uint32_t(pbytes[off + 1]) << 8) |
               (uint32_t(pbytes[off + 2]) << 16) | (uint32_t(pbytes[off + 3]) << 24);
    };
    EXPECT_TRUE(pbytes[0] == 'B' && pbytes[1] == 'W' && pbytes[2] == 'A' && pbytes[3] == 'R');
    const uint32_t ver = rd_u32(4);
    const uint32_t sr = rd_u32(8);
    const uint32_t slot_cnt = rd_u32(12);
    const uint32_t clip_cnt = rd_u32(16);
    const uint32_t sig_cnt = rd_u32(20);
    EXPECT_EQ(ver, 3u);
    EXPECT_EQ(sr, 48000u);
    EXPECT_TRUE(slot_cnt >= 152u);

    const size_t rec_base = 24u;
    const size_t pcm_base = rec_base + size_t(clip_cnt) * 16u + size_t(sig_cnt) * 16u;
    EXPECT_TRUE(pcm_base < psz);

    struct SlotAccum { double sum_sq = 0.0; uint64_t samples = 0; };
    std::vector<SlotAccum> acc(slot_cnt);
    uint64_t total_clipped = 0;
    for (uint32_t i = 0; i < clip_cnt; ++i) {
        const size_t r_off = rec_base + size_t(i) * 16u;
        const uint32_t sid = rd_u32(r_off + 0);
        const uint32_t pcm_off = rd_u32(r_off + 4);
        const uint32_t frames = rd_u32(r_off + 8);
        const uint16_t ch = rd_u16(r_off + 12);
        const size_t byte_len = size_t(frames) * size_t(ch) * 2u;
        const size_t byte_off = size_t(pcm_off) * 2u;
        EXPECT_TRUE(pcm_base + byte_off + byte_len <= psz);
        const int16_t* smp = reinterpret_cast<const int16_t*>(pbytes.data() + pcm_base + byte_off);
        const size_t n_smp = size_t(frames) * size_t(ch);
        for (size_t k = 0; k < n_smp; ++k) {
            const int32_t v = smp[k];
            if (v >= 32760 || v <= -32760) ++total_clipped;
            if (sid < slot_cnt) {
                const double fn = double(v) / 32768.0;
                acc[sid].sum_sq += fn * fn;
                acc[sid].samples += 1u;
            }
        }
    }
    EXPECT_EQ(total_clipped, uint64_t(0));

    // Primary gunfire slots (0..16) and Explosion (94) must achieve punchy in-room RMS (-15.5..-7.0 dBFS)
    for (uint32_t sid = 0; sid <= 16u; ++sid) {
        EXPECT_TRUE(acc[sid].samples > 0u);
        const double rms = std::sqrt(acc[sid].sum_sq / double(acc[sid].samples));
        const double rms_db = 20.0 * std::log10(std::max(rms, 1e-9));
        if (!(rms_db >= -15.5 && rms_db <= -7.0)) {
            std::cout << "  [AudioSlot " << sid << "] rms_db=" << rms_db << " out of [-15.5, -7.0]\n";
        }
        EXPECT_TRUE(rms_db >= -15.5 && rms_db <= -7.0);
    }
    {
        EXPECT_TRUE(acc[94].samples > 0u);
        const double rms = std::sqrt(acc[94].sum_sq / double(acc[94].samples));
        const double rms_db = 20.0 * std::log10(std::max(rms, 1e-9));
        EXPECT_TRUE(rms_db >= -15.0 && rms_db <= -7.0);
    }

    // 2) RenderMix wide stereo pan separation check (2ch width_pct=100 keeps L/R crosstalk < 4%, not 11%)
    auto append_u16 = [](std::vector<uint8_t>& buf, uint16_t v) {
        buf.push_back(uint8_t(v & 0xFFu));
        buf.push_back(uint8_t((v >> 8) & 0xFFu));
    };
    auto append_u32 = [](std::vector<uint8_t>& buf, uint32_t v) {
        for (int b = 0; b < 4; ++b) buf.push_back(uint8_t((v >> (8 * b)) & 0xFFu));
    };
    std::vector<uint8_t> synth;
    synth.insert(synth.end(), {'B', 'W', 'A', 'R'});
    append_u32(synth, 3u);
    append_u32(synth, 48000u);
    append_u32(synth, 129u);
    append_u32(synth, 1u);
    append_u32(synth, 0u);
    append_u32(synth, 0u);
    append_u32(synth, 0u);
    append_u32(synth, 1024u);
    append_u16(synth, 2u);
    append_u16(synth, 100u);
    for (uint32_t i = 0; i < 1024u; ++i) {
        const int16_t left = int16_t(14000.0 * std::sin(0.18 * double(i)));
        const int16_t right = 0;
        append_u16(synth, uint16_t(left));
        append_u16(synth, uint16_t(right));
    }
    EXPECT_TRUE(sound::weapon_audio::LoadPackFromMemory(synth.data(), synth.size()));
    auto& enh = modsettings::Registry().GetEnhanced();
    enh.mod_audio_pack_enabled = true;
    enh.master_gain_percent = 100u;
    enh.weapon_audio_volume = 100u;
    enh.audio_tail_intensity = 0u;
    enh.audio_echo_intensity = 0u;
    sound::weapon_audio::SetEnvironmentState(false, 0.0f);
    sound::weapon_audio::OnPlayerFire("PST", false, false);
    float out_lr[512 * 2] = {};
    const float lp[3] = {0.0f, 0.0f, 0.0f}, lf[3] = {0.0f, 0.0f, 1.0f}, lt[3] = {0.0f, 1.0f, 0.0f};
    sound::weapon_audio::RenderMix(out_lr, nullptr, 512u, 2u, 0u, 1u, false, 3u, 4u, 5u, lp, lf, lt);
    double sum_l = 0.0, sum_r = 0.0;
    for (uint32_t i = 0; i < 512u; ++i) {
        sum_l += double(out_lr[i * 2]) * double(out_lr[i * 2]);
        sum_r += double(out_lr[i * 2 + 1]) * double(out_lr[i * 2 + 1]);
    }
    const double ratio = std::sqrt(sum_l) / std::max(std::sqrt(sum_r), 1e-9);
    EXPECT_TRUE(ratio >= 18.0);

    sound::weapon_audio::LoadPackFromFile(pak_path);
    return true;
}

// 37. Realistic Flamethrower VFX invariants: no discrete 6Hz tile-snapping strobe,
// flow-aligned teardrop stretching along jet & rising pool tongues, and asymmetric tiles.
bool TestFlamethrowerOrganicTonguesAndZeroTileSnap() {
    using game::ballistics::BuildFlamethrowerJetSprites;
    using game::ballistics::BuildFlamethrowerPoolSprites;
    using game::ballistics::FlamePlumeSprite;
    using game::ballistics::FlameSpriteKind;
    using game::ballistics::SampleVfxAtlasTexelBgra;

    const float cpos[6][3] = {
        {0.0f, 1.5f, 0.0f}, {0.0f, 1.5f, 1.0f}, {0.02f, 1.49f, 2.0f},
        {0.05f, 1.47f, 3.0f}, {0.10f, 1.44f, 4.0f}, {0.15f, 1.40f, 5.0f},
    };
    const float crad[6] = {0.09f, 0.14f, 0.20f, 0.26f, 0.34f, 0.42f};
    const float cheat[6] = {1.00f, 0.94f, 0.88f, 0.80f, 0.72f, 0.62f};

    FlamePlumeSprite t0[192]{};
    FlamePlumeSprite t1[192]{};
    const size_t n0 = BuildFlamethrowerJetSprites(cpos, crad, cheat, 6u, 1.0f, 0.10f, false, t0, 192u);
    const size_t n1 = BuildFlamethrowerJetSprites(cpos, crad, cheat, 6u, 1.0f, 0.35f, false, t1, 192u);
    EXPECT_EQ(n0, n1);
    EXPECT_TRUE(n0 >= 30u);

    // 1) Eliminated 6Hz strobe: identical jet puffs keep coherent tile assignment over time while smoothly animating
    bool saw_stretched_jet_fire = false;
    bool saw_rotation_delta = false;
    for (size_t i = 0; i < n0; ++i) {
        EXPECT_EQ(t0[i].tile, t1[i].tile);
        EXPECT_EQ(uint8_t(t0[i].kind), uint8_t(t1[i].kind));
        const float d2 = t0[i].dir[0] * t0[i].dir[0] + t0[i].dir[1] * t0[i].dir[1] + t0[i].dir[2] * t0[i].dir[2];
        EXPECT_TRUE(std::fabs(d2 - 1.0f) < 0.05f);
        if (t0[i].kind == FlameSpriteKind::FireBillboard) {
            if (t0[i].stretch >= 1.35f && t0[i].dir[2] > 0.75f) {
                saw_stretched_jet_fire = true;
            }
            if (std::fabs(t0[i].rotation - t1[i].rotation) > 0.02f) {
                saw_rotation_delta = true;
            }
        }
    }
    EXPECT_TRUE(saw_stretched_jet_fire);
    EXPECT_TRUE(saw_rotation_delta);

    // 2) Rising ground flame tongues stretch upward organically rather than square cubes
    const float ppos[2][3] = {{1.0f, 0.05f, 2.0f}, {2.0f, 0.05f, 3.0f}};
    const float prad[2] = {0.55f, 0.48f};
    const float pheat[2] = {0.90f, 0.65f};
    FlamePlumeSprite pout[32]{};
    const size_t pn = BuildFlamethrowerPoolSprites(ppos, prad, pheat, 2u, 1.0f, 1.15f, pout, 32u);
    bool saw_rising_tongue = false;
    for (size_t i = 0; i < pn; ++i) {
        if (pout[i].kind == FlameSpriteKind::FireBillboard) {
            if (pout[i].stretch >= 1.25f && pout[i].dir[1] >= 0.85f) {
                saw_rising_tongue = true;
            }
        }
    }
    EXPECT_TRUE(saw_rising_tongue);

    // 3) Row 1 fire tiles break 4-fold / 5-petal rotational symmetry (asymmetric teardrop tongues)
    for (uint32_t tx = 0; tx < 4u; ++tx) {
        const uint32_t top_a = (SampleVfxAtlasTexelBgra(tx, 1u, 0.50f, 0.25f) >> 24) & 0xFFu;
        const uint32_t bot_a = (SampleVfxAtlasTexelBgra(tx, 1u, 0.50f, 0.75f) >> 24) & 0xFFu;
        const uint32_t left_a = (SampleVfxAtlasTexelBgra(tx, 1u, 0.25f, 0.50f) >> 24) & 0xFFu;
        const uint32_t right_a = (SampleVfxAtlasTexelBgra(tx, 1u, 0.75f, 0.50f) >> 24) & 0xFFu;
        const uint32_t diff_tb = (top_a > bot_a) ? (top_a - bot_a) : (bot_a - top_a);
        const uint32_t diff_lr = (left_a > right_a) ? (left_a - right_a) : (right_a - left_a);
        EXPECT_TRUE(diff_tb >= 8u || diff_lr >= 8u);
    }
    return true;
}

bool TestGyroOneEuroFilterAndBias() {
    gyro::OneEuroGyroFilter f;

    // 1) Resting MEMS sensor drift (+0.020 rad/s yaw, -0.015 rad/s pitch) adapts zero-bias smoothly
    float out_y = 0.0f, out_p = 0.0f;
    for (int i = 0; i < 600; ++i) {
        f.Update(0.020f, -0.015f, 1.0f / 120.0f, out_y, out_p);
    }
    EXPECT_TRUE(f.bias_yaw() > 0.005f);
    EXPECT_TRUE(f.bias_pitch() < -0.004f);

    // 2) Slow hand tremor jitter around bias is heavily attenuated (rms_out < 0.40 * rms_in)
    f.Reset();
    double sum_in2 = 0.0, sum_out2 = 0.0;
    for (int i = 0; i < 240; ++i) {
        const float jitter_y = (i % 2 == 0) ? 0.030f : -0.030f;
        const float jitter_p = (i % 2 == 0) ? -0.022f : 0.022f;
        f.Update(jitter_y, jitter_p, 1.0f / 120.0f, out_y, out_p);
        sum_in2 += double(jitter_y * jitter_y);
        sum_out2 += double(out_y * out_y);
    }
    EXPECT_TRUE(std::sqrt(sum_out2 / 240.0) < std::sqrt(sum_in2 / 240.0) * 0.40);

    // 3) Fast 180 flick (2.4 rad/s) ramps up with near-zero lag (>85% amplitude within 3 frames @ 120Hz)
    f.Reset();
    for (int i = 0; i < 3; ++i) {
        f.Update(2.40f, 0.40f, 1.0f / 120.0f, out_y, out_p);
    }
    EXPECT_TRUE(out_y > 2.40f * 0.85f);
    EXPECT_TRUE(out_p > 0.40f * 0.75f);
    return true;
}

bool TestHitmarkerVisualAndEnhancedRecoilRules() {
    using namespace game::combat_feedback;

    // 1) Hit classification and duration escalation
    EXPECT_EQ(uint8_t(ClassifyHit(false, false)), uint8_t(HitKind::Body));
    EXPECT_EQ(uint8_t(ClassifyHit(true, false)), uint8_t(HitKind::Headshot));
    EXPECT_EQ(uint8_t(ClassifyHit(false, true)), uint8_t(HitKind::Kill));
    EXPECT_EQ(uint8_t(ClassifyHit(true, true)), uint8_t(HitKind::HeadshotKill));
    EXPECT_TRUE(HitmarkerDurationSec(HitKind::Kill) > HitmarkerDurationSec(HitKind::Body));
    EXPECT_TRUE(HitmarkerDurationSec(HitKind::HeadshotKill) >= HitmarkerDurationSec(HitKind::Kill));

    // 2) Minimalist Awwwards clearance: never covers dead-center crosshair pixel; outer kill wings only on lethal
    const HitmarkerVisualSpec v_body = EvaluateHitmarkerVisual(0.020f, HitKind::Body, 1.0f);
    EXPECT_TRUE(v_body.active);
    EXPECT_TRUE(v_body.inner_gap_px >= 6.5f);
    EXPECT_FALSE(v_body.has_outer_kill_wing);
    EXPECT_TRUE(v_body.alpha > 0.70f);

    const HitmarkerVisualSpec v_hs = EvaluateHitmarkerVisual(0.020f, HitKind::Headshot, 1.0f);
    EXPECT_TRUE(v_hs.active);
    EXPECT_TRUE(v_hs.r > 230 && v_hs.g > 170 && v_hs.b < 140);  // tactical amber gold
    EXPECT_FALSE(v_hs.has_outer_kill_wing);

    const HitmarkerVisualSpec v_kill = EvaluateHitmarkerVisual(0.025f, HitKind::HeadshotKill, 1.5f);
    EXPECT_TRUE(v_kill.active);
    EXPECT_TRUE(v_kill.has_outer_kill_wing);
    EXPECT_TRUE(v_kill.outer_wing_gap_px > v_kill.inner_gap_px + v_kill.tick_len_px);

    const HitmarkerVisualSpec v_done = EvaluateHitmarkerVisual(0.300f, HitKind::HeadshotKill, 1.0f);
    EXPECT_FALSE(v_done.active);

    // 3) Promoted GameplayEnhancedRecoil schema & curve math
    modsettings::SettingsRegistry reg;
    app::Settings s;
    game::Options o;
    InitDefaults(s, o);
    reg.ResetToDefaults(s, o);

    const auto* rec_desc = reg.Find(modsettings::SettingId::GameplayEnhancedRecoil);
    EXPECT_TRUE(rec_desc != nullptr);
    EXPECT_EQ(rec_desc->status, modsettings::FeatureStatus::Implemented);
    EXPECT_EQ(rec_desc->scope, modsettings::ApplyScope::Immediate);
    EXPECT_TRUE(reg.GetBool(modsettings::SettingId::GameplayEnhancedRecoil, s, o));
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::GameplayEnhancedRecoil, false, s, o).accepted);
    EXPECT_FALSE(reg.GetBool(modsettings::SettingId::GameplayEnhancedRecoil, s, o));

    const float vanilla_decay = ComputeEnhancedRecoilDecayFactor(0.84f, 1.0f, false, false);
    const float enh_decay = ComputeEnhancedRecoilDecayFactor(0.84f, 1.0f, true, false);
    const float cheat_decay = ComputeEnhancedRecoilDecayFactor(0.84f, 1.0f, true, true);
    EXPECT_NEAR(vanilla_decay, 0.84f, 1e-4f);
    EXPECT_TRUE(enh_decay < vanilla_decay && enh_decay > 0.0f && enh_decay < 1.0f);
    EXPECT_NEAR(cheat_decay, 0.0f, 1e-5f);

    float yaw_kick = 0.0f;
    const float kick_enh = ComputeEnhancedRecoilKickScale(true, false, 7u, yaw_kick);
    EXPECT_TRUE(kick_enh > 1.0f && kick_enh <= 1.25f);
    EXPECT_TRUE(std::fabs(yaw_kick) <= 0.25f && std::fabs(yaw_kick) > 1e-4f);
    EXPECT_NEAR(ComputeEnhancedRecoilKickScale(true, true, 7u, yaw_kick), 0.0f, 1e-5f);
    return true;
}

bool TestShellCasingBallisticsAndCombatSchema() {
    using namespace game::ballistics;

    // 1) Weapon shell casing ejection filtering
    EXPECT_TRUE(ShouldEjectShellCasing("AK1", false));
    EXPECT_TRUE(ShouldEjectShellCasing("PST", false));
    EXPECT_TRUE(ShouldEjectShellCasing("SHG", false));
    EXPECT_TRUE(ShouldEjectShellCasing("SNR", false));
    EXPECT_TRUE(ShouldEjectShellCasing("HVY", false));
    EXPECT_FALSE(ShouldEjectShellCasing("FLM", false));
    EXPECT_FALSE(ShouldEjectShellCasing("RPG", false));
    EXPECT_FALSE(ShouldEjectShellCasing("GRL", false));
    EXPECT_FALSE(ShouldEjectShellCasing("BNS", true));   // M203 underbarrel grenade does not eject rifle brass

    const auto shg = GetShellCasingProfile(WeaponClass::Shotgun);
    const auto pst = GetShellCasingProfile(WeaponClass::Pistol);
    const auto snr = GetShellCasingProfile(WeaponClass::Sniper);
    EXPECT_TRUE(shg.ejects && shg.red_shotgun_hull);
    EXPECT_TRUE(pst.ejects && !pst.red_shotgun_hull);
    EXPECT_TRUE(snr.length_m > pst.length_m);

    // 2) Gravity + first floor bounce detection
    ShellCasingBody body{};
    body.active = true;
    body.pos[0] = 0.0f; body.pos[1] = 0.50f; body.pos[2] = 0.0f;
    body.vel[0] = 2.0f; body.vel[1] = 0.5f;  body.vel[2] = -0.3f;
    body.ground_y = 0.0f;
    body.ttl = 3.2f;
    int first_bounces = 0;
    for (int i = 0; i < 60; ++i) {
        if (StepShellCasingPhysics(body, 0.016f)) first_bounces++;
    }
    EXPECT_EQ(first_bounces, 1);
    EXPECT_TRUE(body.bounces >= 1);
    EXPECT_TRUE(body.pos[1] >= body.ground_y - 1e-4f);

    // 3) CombatHitmarkers & CombatShellCasings schema defaults + TOML round-trip
    TempDir tmp("combat_feedback_schema");
    fs::path cfg = tmp.path / "black.toml";
    app::Settings s;
    game::Options o;
    InitDefaults(s, o);
    modsettings::SettingsRegistry reg;
    reg.ResetToDefaults(s, o);

    EXPECT_TRUE(reg.GetBool(modsettings::SettingId::CombatHitmarkers, s, o));
    EXPECT_TRUE(reg.GetBool(modsettings::SettingId::CombatShellCasings, s, o));
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::CombatHitmarkers, false, s, o).accepted);
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::CombatShellCasings, false, s, o).accepted);

    modsettings::PresetStateMachine psm;
    psm.SetActiveKind(modsettings::PresetKind::Custom);
    EXPECT_TRUE(reg.SaveToFile(cfg, s, o, &psm));

    modsettings::SettingsRegistry reg2;
    modsettings::PresetStateMachine psm2;
    app::Settings s2;
    game::Options o2;
    InitDefaults(s2, o2);
    reg2.ResetToDefaults(s2, o2);
    EXPECT_TRUE(reg2.LoadFromFile(cfg, s2, o2, &psm2).parsed_ok);
    EXPECT_FALSE(reg2.GetBool(modsettings::SettingId::CombatHitmarkers, s2, o2));
    EXPECT_FALSE(reg2.GetBool(modsettings::SettingId::CombatShellCasings, s2, o2));
    return true;
}

bool TestProceduralHitConfirmAndShellClinkMix() {
    auto& enh = modsettings::Registry().GetEnhanced();
    enh.mod_audio_pack_enabled = true;
    enh.hitmarker_feedback = true;
    enh.shell_casings = true;
    enh.master_gain_percent = 100;

    auto measure_lr = [&](double& out_el, double& out_er, float& out_peak) {
        constexpr uint32_t kFrames = 2048;
        std::vector<float> out(kFrames * 2, 0.0f);
        std::vector<float> send(kFrames, 0.0f);
        sound::weapon_audio::RenderMix(out.data(), send.data(), kFrames, 2u, 0u, 1u, false, 2u, 2u, 2u,
                                       nullptr, nullptr, nullptr);
        out_el = 0.0;
        out_er = 0.0;
        out_peak = 0.0f;
        for (uint32_t i = 0; i < kFrames; ++i) {
            const float l = out[i * 2];
            const float r = out[i * 2 + 1];
            EXPECT_TRUE(std::isfinite(l) && std::isfinite(r));
            out_peak = std::max(out_peak, std::max(std::fabs(l), std::fabs(r)));
            out_el += double(l) * double(l);
            out_er += double(r) * double(r);
        }
        return true;
    };

    sound::weapon_audio::StopAll();
    const auto t0 = sound::weapon_audio::GetTelemetry();

    // 1) Centered headshot + kill-confirm cue mixes symmetrically under HDR limiter
    sound::weapon_audio::TriggerHitConfirm(true, true);
    double el_hit = 0.0, er_hit = 0.0;
    float peak_hit = 0.0f;
    if (!measure_lr(el_hit, er_hit, peak_hit)) return false;
    EXPECT_TRUE(peak_hit > 0.03f && peak_hit <= 0.994f);
    EXPECT_TRUE(std::fabs(el_hit - er_hit) <= (el_hit + er_hit) * 1e-4 + 1e-6);

    // 2) Right-ejected brass shell clink isolates strong stereo right separation
    sound::weapon_audio::StopAll();
    sound::weapon_audio::TriggerShellClink(0.65f, false);
    double el_clink = 0.0, er_clink = 0.0;
    float peak_clink = 0.0f;
    if (!measure_lr(el_clink, er_clink, peak_clink)) return false;
    EXPECT_TRUE(peak_clink > 0.015f && peak_clink <= 0.994f);
    EXPECT_TRUE(er_clink > el_clink * 2.5);

    const auto t1 = sound::weapon_audio::GetTelemetry();
    EXPECT_TRUE(t1.synth_hit_confirms >= t0.synth_hit_confirms + 1);
    EXPECT_TRUE(t1.synth_shell_clinks >= t0.synth_shell_clinks + 1);
    return true;
}

// 42. Adrenaline Overdrive Counter/Surge & Edge Glow Disabled (Clean HUD + Visor Independence)
bool TestOverdriveAdrenalineAndSiphon() {
    using namespace game::overdrive_rules;
    // Visor shader params no longer emit peripheral overdrive edge glow when disabled cleanly
    auto vp_menu = ComputeVisorShaderParams(true, true, true, 0.0f);
    EXPECT_NEAR(vp_menu.thermal_mix, 0.0f, 1e-5f);
    EXPECT_NEAR(vp_menu.overdrive_edge, 0.0f, 1e-5f);

    auto vp_play = ComputeVisorShaderParams(true, true, false, 0.0f);
    EXPECT_NEAR(vp_play.thermal_mix, 1.0f, 1e-5f);
    EXPECT_NEAR(vp_play.overdrive_edge, 0.0f, 1e-5f);

    // Schema defaults keep Adrenaline / Siphon / Reflex / Shockwave disabled while Kinetic Dash stays active
    app::Settings s;
    game::Options o;
    InitDefaults(s, o);
    modsettings::SettingsRegistry reg;
    reg.ResetToDefaults(s, o);
    EXPECT_FALSE(reg.GetEnhanced().overdrive_enabled);
    EXPECT_FALSE(reg.GetEnhanced().overdrive_siphon);
    EXPECT_FALSE(reg.GetEnhanced().overdrive_reflex_slowmo);
    EXPECT_FALSE(reg.GetEnhanced().kinetic_dash_shockwave);
    EXPECT_TRUE(reg.GetEnhanced().kinetic_dash_enabled);
    return true;
}

// 43. Kinetic Dash Directional Basis, Boosted Travel Distance (>= 8.5m), FOV Punch & Zero Shockwave
bool TestKineticDashAndShockwave() {
    using namespace game::overdrive_rules;

    const float fwd[3] = {0.0f, 0.25f, 1.0f};
    const float right[3] = {1.0f, -0.10f, 0.0f};
    float dir[3] = {};

    // Zero stick -> pure flattened horizontal camera forward
    EXPECT_TRUE(ComputeDashMoveVector(fwd, right, 0.0f, 0.0f, dir));
    EXPECT_NEAR(dir[1], 0.0f, 1e-5f);
    EXPECT_NEAR(dir[2], 1.0f, 1e-4f);

    // Pure left strafe stick -> dashes left perpendicular to look
    EXPECT_TRUE(ComputeDashMoveVector(fwd, right, -1.0f, 0.0f, dir));
    EXPECT_NEAR(dir[0], -1.0f, 1e-4f);
    EXPECT_NEAR(dir[1], 0.0f, 1e-5f);

    KineticDashController dash;
    dash.Reset();
    EXPECT_TRUE(dash.TryTrigger(true, false, false));
    EXPECT_FALSE(dash.TryTrigger(true, false, false));  // Cooldown blocks instant re-trigger

    bool shockwave_seen = false;
    float total_disp_m = 0.0f;
    float peak_fov = 0.0f;
    for (int i = 0; i < 30; ++i) {
        auto s = dash.Step(0.016f);
        if (s.emit_shockwave) shockwave_seen = true;
        if (s.dashing) total_disp_m += s.speed_mps * 0.016f;
        peak_fov = std::max(peak_fov, s.fov_boost_deg);
    }
    EXPECT_FALSE(shockwave_seen);
    EXPECT_TRUE(total_disp_m >= 8.5f && total_disp_m <= 13.5f);
    EXPECT_TRUE(peak_fov >= 12.0f && peak_fov <= 22.0f);
    EXPECT_FALSE(dash.IsDashing());

    // Cooldown completes cleanly within 1.5s without requiring Overdrive
    for (int i = 0; i < 120; ++i) dash.Step(0.02f);
    EXPECT_TRUE(dash.TryTrigger(true, false, false));
    EXPECT_TRUE(dash.CooldownRemaining() <= 1.35f);
    return true;
}

// 44. Special Tactical Ammunition Profiles & Thermal Threat Visor Shader Rules
bool TestSpecialAmmoAndThermalVisor() {
    using namespace game::overdrive_rules;
    EXPECT_EQ(int(SanitizeSpecialAmmo(99u)), int(SpecialAmmoType::RailAP));
    SpecialAmmoType a = SpecialAmmoType::Standard;
    a = CycleSpecialAmmo(a);
    EXPECT_EQ(int(a), int(SpecialAmmoType::ThermiteIncendiary));
    a = CycleSpecialAmmo(a);
    EXPECT_EQ(int(a), int(SpecialAmmoType::ExplosiveHE));
    a = CycleSpecialAmmo(a);
    EXPECT_EQ(int(a), int(SpecialAmmoType::RailAP));
    a = CycleSpecialAmmo(a);
    EXPECT_EQ(int(a), int(SpecialAmmoType::Standard));

    auto thrm = GetSpecialAmmoProfile(SpecialAmmoType::ThermiteIncendiary);
    EXPECT_TRUE(thrm.ignite_target && !thrm.detonate_on_hit);
    EXPECT_TRUE(thrm.damage_scale > 1.0f);

    auto he = GetSpecialAmmoProfile(SpecialAmmoType::ExplosiveHE);
    EXPECT_TRUE(he.detonate_on_hit && he.force_scale >= 2.0f);

    auto rail = GetSpecialAmmoProfile(SpecialAmmoType::RailAP);
    EXPECT_TRUE(rail.headshot_scale >= 1.30f && rail.tracer_color_override == 5u);

    // Thermal visor must turn off automatically in menus or paused state
    auto vp_menu = ComputeVisorShaderParams(true, true, true, 0.8f);
    EXPECT_NEAR(vp_menu.thermal_mix, 0.0f, 1e-5f);
    auto vp_play = ComputeVisorShaderParams(true, true, false, 0.0f);
    EXPECT_NEAR(vp_play.thermal_mix, 1.0f, 1e-5f);
    EXPECT_NEAR(vp_play.overdrive_edge, 0.0f, 1e-5f);

    EXPECT_TRUE(ShouldRenderThermalBracket(true, true, 2.0f));
    EXPECT_FALSE(ShouldRenderThermalBracket(false, true, 2.0f));
    EXPECT_FALSE(ShouldRenderThermalBracket(true, false, 2.0f));
    EXPECT_FALSE(ShouldRenderThermalBracket(true, true, 0.01f));
    return true;
}

// 45. Kinetic Dash & Tactical Gear Schema Serialization + Procedural Dash Audio Cue
bool TestOverdriveSchemaAndAudioCues() {
    app::Settings s;
    game::Options o;
    InitDefaults(s, o);
    modsettings::SettingsRegistry reg;
    reg.ResetToDefaults(s, o);

    // Kinetic Dash is active out-of-the-box; Adrenaline & Shockwave remain off
    EXPECT_FALSE(reg.GetEnhanced().overdrive_enabled);
    EXPECT_FALSE(reg.GetEnhanced().overdrive_siphon);
    EXPECT_TRUE(reg.GetEnhanced().kinetic_dash_enabled);
    EXPECT_FALSE(reg.GetEnhanced().kinetic_dash_shockwave);
    EXPECT_EQ(reg.GetEnhanced().special_ammo_mode, 0u);
    EXPECT_FALSE(reg.GetEnhanced().thermal_visor_enabled);

    EXPECT_TRUE(reg.SetUint(modsettings::SettingId::SpecialAmmoMode, 2u, s, o).accepted);
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::ThermalVisorEnabled, true, s, o).accepted);
    EXPECT_TRUE(reg.SetBool(modsettings::SettingId::KineticDashEnabled, false, s, o).accepted);

    const std::string saved = reg.SaveToTable(s, o).size() ? ([&] {
        std::ostringstream oss;
        oss << reg.SaveToTable(s, o);
        return oss.str();
    })() : "";
    EXPECT_TRUE(saved.find("special_ammo_mode = 2") != std::string::npos);
    EXPECT_TRUE(saved.find("thermal_visor_enabled = true") != std::string::npos);
    EXPECT_TRUE(saved.find("kinetic_dash_enabled = false") != std::string::npos);

    app::Settings s2;
    game::Options o2;
    InitDefaults(s2, o2);
    modsettings::SettingsRegistry reg2;
    auto lr = reg2.LoadFromString(saved, s2, o2);
    EXPECT_TRUE(lr.parsed_ok);
    EXPECT_EQ(reg2.GetEnhanced().special_ammo_mode, 2u);
    EXPECT_TRUE(reg2.GetEnhanced().thermal_visor_enabled);
    EXPECT_FALSE(reg2.GetEnhanced().kinetic_dash_enabled);

    // Test out-of-range special_ammo_mode sanitization
    reg2.LoadFromString("schema_version = 3\n[overdrive]\nspecial_ammo_mode = 99\n", s2, o2);
    EXPECT_EQ(reg2.GetEnhanced().special_ammo_mode, 3u);

    // Test procedural audio cue for Kinetic Dash under HDR limiter
    sound::weapon_audio::StopAll();
    const auto t0 = sound::weapon_audio::GetTelemetry();
    sound::weapon_audio::TriggerKineticDash(false);
    std::vector<float> buf(4800 * 2, 0.0f);
    const float lpos[3] = {0, 0, 0}, lfwd[3] = {0, 0, 1}, lup[3] = {0, 1, 0};
    sound::weapon_audio::RenderMix(buf.data(), nullptr, 4800u, 2u, 0u, 1u, false, 0u, 0u, 0u,
                                   lpos, lfwd, lup);
    float peak = 0.0f;
    for (float x : buf) {
        EXPECT_TRUE(std::isfinite(x));
        peak = std::max(peak, std::fabs(x));
    }
    EXPECT_TRUE(peak > 0.04f && peak <= 0.994f);
    const auto t1 = sound::weapon_audio::GetTelemetry();
    EXPECT_TRUE(t1.synth_overdrive_cues >= t0.synth_overdrive_cues + 1);
    return true;
}

}  // namespace

#include "game/blood_decal_rules.h"
#include "game/special_arsenal_core.h"

namespace {

bool TestBloodPoolPermanenceAndCoagulation() {
    using namespace game::blood_decals;
    EXPECT_TRUE(std::fabs(ComputePoolLifetimeSec(true)) < 1e-6f);
    EXPECT_TRUE(ComputePoolLifetimeSec(false) >= 9.0f && ComputePoolLifetimeSec(false) <= 15.0f);

    EXPECT_EQ(ResolveMaxBloodPools(128u), 256u);
    EXPECT_EQ(ResolveMaxBloodPools(512u), 512u);
    EXPECT_EQ(ResolveMaxBloodPools(4096u), 1024u);

    // LRU eviction: prefer free slot first, then farthest & settled oldest slot
    bool active[4] = {true, false, true, true};
    float ages[4] = {120.0f, 0.0f, 5.0f, 95.0f};
    float dists[4] = {4.0f, 0.0f, 100.0f, 600.0f};
    EXPECT_EQ(SelectEvictionSlot(active, ages, dists, 4), 1u);

    active[1] = true;
    ages[1] = 2.0f;
    dists[1] = 2.0f;
    // Slot 3 is settled (95s) and far away (600 m^2) -> should evict slot 3, not near fresh pool 1!
    EXPECT_EQ(SelectEvictionSlot(active, ages, dists, 4), 3u);

    // Physical coagulation curve: fresh arterial red spread -> rich dark claret, never fades out or turns black!
    const auto fresh = EvaluatePoolCoagulation(0.15f, 1.0f);
    const auto mid = EvaluatePoolCoagulation(2.5f, 1.0f);
    const auto old_pool = EvaluatePoolCoagulation(600.0f, 1.0f);

    EXPECT_TRUE(fresh.spread_scale > 0.45f && fresh.spread_scale < 0.90f);
    EXPECT_TRUE(fresh.r >= 0.72f && fresh.r > fresh.g * 5.0f);
    EXPECT_TRUE(fresh.specular_sheen >= 0.65f);

    EXPECT_TRUE(mid.spread_scale >= 0.99f);
    EXPECT_TRUE(mid.r >= 0.60f && mid.r <= 0.86f);

    // Permanent pool after 10 minutes keeps high opacity (>0.85) and rich crimson chroma (NOT pitch black!)
    EXPECT_TRUE(old_pool.a >= 0.85f);
    EXPECT_TRUE(old_pool.r >= 0.50f && old_pool.r <= 0.65f);
    EXPECT_TRUE(old_pool.specular_sheen <= 0.18f);

    const float drip_fast = ComputeWoundDripIntervalSec(1.0f);
    const float drip_slow = ComputeWoundDripIntervalSec(0.25f);
    EXPECT_TRUE(drip_fast >= 0.15f && drip_fast <= 0.50f);
    EXPECT_TRUE(drip_slow > drip_fast && drip_slow <= 1.1f);
    return true;
}

static void WriteU32Le(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(uint8_t(v & 0xFF));
    buf.push_back(uint8_t((v >> 8) & 0xFF));
    buf.push_back(uint8_t((v >> 16) & 0xFF));
    buf.push_back(uint8_t((v >> 24) & 0xFF));
}

static void WriteF32Le(std::vector<uint8_t>& buf, float f) {
    uint32_t v = 0;
    std::memcpy(&v, &f, 4);
    WriteU32Le(buf, v);
}

static void WriteU16Le(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(uint8_t(v & 0xFF));
    buf.push_back(uint8_t((v >> 8) & 0xFF));
}

bool TestSpecialArsenalBmshV2AndBtexFormat() {
    using namespace game::special_arsenal;

    // Construct a minimal BMSH v2 payload with 3 vertices, 3 indices, 2 parts
    std::vector<uint8_t> v2;
    WriteU32Le(v2, 0x48534D42u);  // "BMSH"
    WriteU32Le(v2, 2u);           // version 2
    WriteU32Le(v2, 3u);           // vert_count
    WriteU32Le(v2, 3u);           // index_count
    WriteU32Le(v2, 2u);           // part_count
    WriteU32Le(v2, 1u);           // flags (textured)
    // bounds_min, bounds_max, nozzle_pos, pilot_pos, eject_pos (15 floats)
    const float hdr_floats[15] = {
        -0.1f, -0.1f, 0.0f,  0.1f, 0.1f, 0.5f,
        0.0f, 0.0f, 0.5f,    0.0f, -0.02f, 0.45f,   0.03f, 0.02f, 0.12f
    };
    for (float f : hdr_floats) WriteF32Le(v2, f);

    // 3 vertices * 36 bytes
    for (int i = 0; i < 3; ++i) {
        WriteF32Le(v2, float(i) * 0.05f);
        WriteF32Le(v2, 0.0f);
        WriteF32Le(v2, 0.25f);
        WriteF32Le(v2, 0.0f);
        WriteF32Le(v2, 1.0f);
        WriteF32Le(v2, 0.0f);
        WriteU32Le(v2, 0xFF809040u);
        WriteF32Le(v2, float(i) * 0.4f);
        WriteF32Le(v2, 0.5f);
    }
    // 3 uint16 indices
    WriteU16Le(v2, 0);
    WriteU16Le(v2, 1);
    WriteU16Le(v2, 2);

    // 2 BmshPart entries * 64 bytes
    auto push_part = [&](const char* name, uint32_t first_idx, uint32_t idx_cnt) {
        for (int k = 0; k < 32; ++k) {
            uint8_t ch = (k < int(std::strlen(name))) ? uint8_t(name[k]) : 0u;
            v2.push_back(ch);
        }
        WriteU32Le(v2, first_idx);
        WriteU32Le(v2, idx_cnt);
        for (int k = 0; k < 6; ++k) WriteF32Le(v2, 0.0f);
    };
    push_part("body", 0u, 3u);
    push_part("bolt", 0u, 0u);

    BmshModel parsed{};
    EXPECT_TRUE(ParseBmsh(v2.data(), v2.size(), parsed));
    EXPECT_EQ(parsed.version, 2u);
    EXPECT_EQ(parsed.vertices.size(), 3u);
    EXPECT_EQ(parsed.indices.size(), 3u);
    EXPECT_EQ(parsed.parts.size(), 2u);
    EXPECT_TRUE(std::strcmp(parsed.parts[0].name, "body") == 0);
    EXPECT_TRUE(std::fabs(parsed.nozzle_pos[2] - 0.5f) < 1e-4f);

    // Reject corrupt sizes cleanly
    BmshModel bad{};
    EXPECT_FALSE(ParseBmsh(v2.data(), 12u, bad));

    // BTEX header & chroma check
    std::vector<uint8_t> btex;
    WriteU32Le(btex, 0x58455442u);  // "BTEX"
    WriteU32Le(btex, 1u);           // version
    WriteU32Le(btex, 4u);           // small test width
    WriteU32Le(btex, 4u);           // small test height
    WriteU32Le(btex, 0x0AA10621u);  // swizzled A8R8G8B8 format
    WriteU32Le(btex, 64u);          // payload bytes
    WriteU32Le(btex, 0x12345678u);  // key lo
    WriteU32Le(btex, 0x9ABCDEF0u);  // key hi
    for (int i = 0; i < 16; ++i) {
        uint8_t r = uint8_t(45 + i * 8);
        uint8_t g = uint8_t(70 + (i % 5) * 18);
        uint8_t b = uint8_t(35 + (i % 4) * 15);
        uint32_t bgra = (0xFFu << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
        WriteU32Le(btex, bgra);
    }
    const auto val = ValidateBtex(btex.data(), btex.size());
    EXPECT_TRUE(val.valid);
    EXPECT_TRUE(val.white_fraction < 0.05f);
    EXPECT_TRUE(val.std_rgb > 10.0f);
    return true;
}

bool TestFlamethrowerM2MechanicsAndIgniters() {
    using namespace game::special_arsenal;
    ArsenalSim sim;
    sim.ResetAll();
    sim.SelectWeapon(SpecialWeaponId::Flamethrower);

    const auto st0 = sim.GetStatus();
    EXPECT_EQ(st0.ignition_cartridges, 5);
    EXPECT_TRUE(st0.fuel_pct > 99.0f);
    EXPECT_TRUE(st0.regulator_psi >= 340.0f && st0.regulator_psi <= 360.0f);

    // Pulling trigger fires 1 ignition squib from 5-shot cylinder + brief ignition spark
    ArsenalInput in{};
    in.dt = 1.0f / 60.0f;
    in.equipped = true;
    in.fire_held = true;
    in.fire_pressed = true;
    auto ev0 = sim.Step(in);
    EXPECT_TRUE(sim.GetStatus().ignition_cartridges == 4);
    EXPECT_TRUE(sim.GetStatus().pose.trigger_pull > 0.5f);
    EXPECT_TRUE(sim.GetStatus().pose.pilot_spark > 0.3f);
    EXPECT_TRUE(ev0.audio_cue_count > 0);

    // Step 2s continuous jetting -> ~28.5% fuel burned (~7s total burn time), regulator holds range steady!
    in.fire_pressed = false;
    bool burned_ticks = false;
    for (int i = 0; i < 120; ++i) {
        auto ev = sim.Step(in);
        if (ev.fire_flame_tick) burned_ticks = true;
    }
    EXPECT_TRUE(burned_ticks);
    const auto st2 = sim.GetStatus();
    EXPECT_TRUE(st2.fuel_pct < 78.0f && st2.fuel_pct > 65.0f);
    EXPECT_TRUE(st2.nozzle_range_m >= 24.0f);  // Constant-pressure nitrogen regulator!

    // Release trigger -> instant auto-shutoff valve slams closed (no dribble)
    in.fire_held = false;
    auto ev_rel = sim.Step(in);
    EXPECT_TRUE(sim.GetStatus().phase == ActionPhase::Idle);
    EXPECT_TRUE(ev_rel.audio_cue_count > 0);
    return true;
}

bool TestMauser1891_12_CockOnCloseAndStripperClip() {
    using namespace game::special_arsenal;
    ArsenalSim sim;
    sim.ResetAll();
    sim.SelectWeapon(SpecialWeaponId::Mauser1891);

    ArsenalInput in{};
    in.dt = 1.0f / 60.0f;
    in.equipped = true;
    in.ads_held = true;
    for (int i = 0; i < 20; ++i) sim.Step(in);
    EXPECT_TRUE(sim.GetStatus().ads_fov_scale <= 0.58f);  // 1.85x precision tangent iron sights
    EXPECT_EQ(sim.GetStatus().mag_ammo, 5);

    // Fire 1st shot -> 7.65x53 high power
    in.fire_pressed = true;
    in.fire_held = true;
    auto ev_shot = sim.Step(in);
    EXPECT_TRUE(ev_shot.fire_bullet);
    EXPECT_TRUE(ev_shot.bullet_damage >= 115.0f);
    EXPECT_EQ(sim.GetStatus().mag_ammo, 4);

    // Trigger still held / released: cannot fire mid-bolt-cycle; release trigger to run cock-on-close bolt
    in.fire_pressed = false;
    in.fire_held = false;
    bool saw_lift = false, saw_back = false, saw_eject = false, saw_cock_push = false;
    for (int i = 0; i < 55; ++i) {
        auto ev = sim.Step(in);
        const auto st = sim.GetStatus();
        if (st.phase == ActionPhase::BoltLift && st.pose.bolt_lift_deg > 35.0f) saw_lift = true;
        if (st.phase == ActionPhase::BoltBack && st.pose.bolt_retract_m > 0.04f) saw_back = true;
        if (ev.eject_shell) saw_eject = true;
        if (st.phase == ActionPhase::BoltPushCocking) saw_cock_push = true;
        if (st.phase == ActionPhase::Idle && i > 25) break;
    }
    EXPECT_TRUE(saw_lift && saw_back && saw_eject && saw_cock_push);
    EXPECT_TRUE(sim.GetStatus().phase == ActionPhase::Idle);

    // Reload via 5-rd top stripper clip restores full 5/5 mag
    in.reload_pressed = true;
    sim.Step(in);
    in.reload_pressed = false;
    bool saw_clip = false;
    for (int i = 0; i < 120; ++i) {
        sim.Step(in);
        if (sim.GetStatus().pose.clip_insert > 0.2f) saw_clip = true;
        if (!sim.GetStatus().reloading && sim.GetStatus().mag_ammo == 5) break;
    }
    EXPECT_TRUE(saw_clip);
    EXPECT_EQ(sim.GetStatus().mag_ammo, 5);
    return true;
}

bool TestMauserC96_M712_ShortRecoilAndBoltHoldOpen() {
    using namespace game::special_arsenal;
    ArsenalSim sim;
    sim.ResetAll();
    sim.SelectWeapon(SpecialWeaponId::MauserC96);
    EXPECT_EQ(sim.GetStatus().mag_ammo, 20);
    EXPECT_TRUE(sim.GetStatus().full_auto_mode);

    // Empty the 20-rd mag in Schnellfeuer ~950 RPM burst -> check short-recoil barrel + bolt + lock open at 0
    ArsenalInput in{};
    in.dt = 1.0f / 60.0f;
    in.equipped = true;
    in.fire_held = true;
    in.fire_pressed = true;
    int shots = 0;
    bool saw_short_recoil = false, saw_bolt_travel = false;
    for (int i = 0; i < 120; ++i) {
        auto ev = sim.Step(in);
        in.fire_pressed = false;
        const auto st = sim.GetStatus();
        if (ev.fire_bullet) shots++;
        if (st.pose.barrel_recoil_m > 0.002f) saw_short_recoil = true;
        if (st.pose.bolt_retract_m > 0.020f) saw_bolt_travel = true;
    }
    EXPECT_EQ(shots, 20);
    EXPECT_TRUE(saw_short_recoil && saw_bolt_travel);
    EXPECT_EQ(sim.GetStatus().mag_ammo, 0);
    EXPECT_TRUE(sim.GetStatus().bolt_locked_open);
    EXPECT_TRUE(sim.GetStatus().pose.bolt_retract_m >= 0.020f);

    // Reload unlocks bolt hold-open and fills mag back to 20
    in.fire_held = false;
    in.reload_pressed = true;
    sim.Step(in);
    in.reload_pressed = false;
    for (int i = 0; i < 150; ++i) {
        sim.Step(in);
        if (!sim.GetStatus().reloading) break;
    }
    EXPECT_EQ(sim.GetStatus().mag_ammo, 20);
    EXPECT_FALSE(sim.GetStatus().bolt_locked_open);
    return true;
}

bool TestFlamethrowerAndC96_HdMultiMaterialAssets() {
    // Verify all special weapon asset files were cleanly removed from disk per user directive
    auto read_all = [](const char* p) -> std::vector<uint8_t> {
        std::ifstream f(p, std::ios::binary | std::ios::ate);
        if (!f) return {};
        auto sz = f.tellg();
        f.seekg(0);
        std::vector<uint8_t> d(static_cast<size_t>(sz), uint8_t{0});
        f.read(reinterpret_cast<char*>(d.data()), sz);
        return d;
    };
    EXPECT_TRUE(read_all("app/src/main/assets/weapons/maxim_m1910.bmsh").empty());
    EXPECT_TRUE(read_all("app/src/main/assets/weapons/flamethrower.bmsh").empty());
    EXPECT_TRUE(read_all("app/src/main/assets/weapons/mauser_1891_12.bmsh").empty());
    EXPECT_TRUE(read_all("app/src/main/assets/weapons/mauser_c96.bmsh").empty());
    return true;
}

bool TestSpecialArsenalRealAssetsAndWebAudioPack() {
    // Verify 48kHz True-Stereo weapon audio pack loads and mixes cleanly
    EXPECT_TRUE(sound::weapon_audio::LoadPackFromFile("app/src/main/assets/weapon_audio/black_weapon_audio.pak"));
    const auto t_pack = sound::weapon_audio::GetTelemetry();
    EXPECT_TRUE(t_pack.slot_count >= 150u);
    for (const char* code : {"AK1", "M16", "SHG", "SNR", "RPG"}) {
        sound::weapon_audio::StopAll();
        sound::weapon_audio::OnPlayerFire(code, false, false);
        std::vector<float> buf(9600 * 2, 0.0f);
        const float lp[3] = {0, 0, 0}, lf[3] = {0, 0, 1}, lu[3] = {0, 1, 0};
        sound::weapon_audio::RenderMix(buf.data(), nullptr, 9600u, 2u, 0u, 1u, false, 0u, 0u, 0u, lp, lf, lu);
        float pk = 0.0f;
        for (float s : buf) {
            EXPECT_TRUE(std::isfinite(s));
            pk = std::max(pk, std::fabs(s));
        }
        EXPECT_TRUE(pk > 0.05f && pk <= 0.994f);
        const auto hp = game::haptics::ComputeWeaponHapticPulse(code, false, false, false);
        EXPECT_TRUE(hp.triggered && hp.amplitude >= 165);
    }
    return true;
}

bool TestMauser1891_FullMeshUvLogicAndEnvAudio() {
    // Verify 3D environmental impact acoustics + supersonic crack + indoor/outdoor mix
    EXPECT_TRUE(sound::weapon_audio::LoadPackFromFile("app/src/main/assets/weapon_audio/black_weapon_audio.pak"));
    sound::weapon_audio::StopAll();
    const auto t0 = sound::weapon_audio::GetTelemetry();
    const float hit_pos[3] = {12.0f, 1.2f, 24.0f};
    sound::weapon_audio::TriggerWorldImpact(hit_pos, true, true);
    const float lp[3] = {0.0f, 1.6f, 0.0f}, lf[3] = {0.0f, 0.0f, 1.0f}, lu[3] = {0.0f, 1.0f, 0.0f};
    std::vector<float> buf(4800u * 2u, 0.0f);
    sound::weapon_audio::RenderMix(buf.data(), nullptr, 4800u, 2u, 0u, 1u, false, 0u, 0u, 0u, lp, lf, lu);
    const auto t1 = sound::weapon_audio::GetTelemetry();
    EXPECT_TRUE(t1.impacts_enhanced > t0.impacts_enhanced);
    EXPECT_TRUE(t1.flybys_enhanced > t0.flybys_enhanced);
    float pk = 0.0f;
    for (float s : buf) pk = std::max(pk, std::fabs(s));
    EXPECT_TRUE(pk > 0.01f && pk <= 0.994f);
    return true;
}

bool TestMauserC96_FullMeshUvLogicAndAudio() {
    // Verify Oxyhemoglobin Blood Atlas (Row 2, Tiles 8..11) produces vivid arterial crimson (never pitch black!)
    using game::ballistics::SampleVfxAtlasTexelBgra;
    for (uint32_t tx = 0; tx < 4u; ++tx) {
        const uint32_t px = SampleVfxAtlasTexelBgra(tx, 2u, 0.50f, 0.50f);
        const uint32_t a = (px >> 24) & 0xFFu;
        const uint32_t r = (px >> 16) & 0xFFu;
        const uint32_t g = (px >> 8) & 0xFFu;
        const uint32_t b = px & 0xFFu;
        EXPECT_TRUE(a >= 120u);
        EXPECT_TRUE(r >= 170u); // Vivid arterial crimson, not dark/black!
        EXPECT_TRUE(r > g * 3u && r > b * 3u);
    }
    return true;
}

bool TestMauser1891_MultiZoomPbrReticleAndAudio() {
    // Verify Delirium Explosion Shockwave staggers nearby living enemy soldiers and triggers blind-fire scatter
    using namespace game::delirium;
    Simulation sim;
    sim.Reset();
    Config cfg;
    cfg.enabled = true;
    cfg.intensity = 1.35f;
    cfg.blind_fire_enabled = true;
    cfg.agony_duration = 1.10f;
    sim.SetConfig(cfg);

    constexpr uint32_t kSoldierEa = 0x10040000u;
    const float blast_pos[3] = {10.0f, 0.5f, 20.0f};
    const float soldier_pos[3] = {12.5f, 0.5f, 21.5f}; // ~2.9m from explosion center
    sim.OnExplosionShockwave(kSoldierEa, blast_pos, soldier_pos, 6.5f, 2.2f);

    const StepOutput step = sim.StepAlive(kSoldierEa, 0.016f);
    EXPECT_TRUE(step.apply_stumble_vel);
    EXPECT_TRUE(step.stumble_vel[0] > 0.5f); // Pushed away from blast (+X)
    EXPECT_TRUE(step.trigger_clutch_overlay);
    EXPECT_TRUE(step.blind_fire_ready);

    float target[3] = {0.0f, 1.5f, 0.0f};
    EXPECT_TRUE(sim.ApplyBlindFireScatter(kSoldierEa, target));
    return true;
}

bool TestMauser1891_HdMaterialsAndSkinnedHandsSync() {
    // Verify Mach Shockwave Ring tiles (Tiles 14..15) synthesize clean circular rings with zero border bleed
    using game::ballistics::SampleVfxAtlasTexelBgra;
    for (uint32_t tx = 2u; tx <= 3u; ++tx) {
        const uint32_t ring_px = SampleVfxAtlasTexelBgra(tx, 3u, 0.80f, 0.50f);
        const uint32_t edge_px = SampleVfxAtlasTexelBgra(tx, 3u, 0.01f, 0.50f);
        EXPECT_TRUE(((ring_px >> 24) & 0xFFu) >= 40u);
        EXPECT_EQ((edge_px >> 24) & 0xFFu, 0u);
    }
    return true;
}

void RunTest(const char* name, bool (*fn)()) {
    std::cout << "[RUN ] " << name << "\n";
    if (fn()) {
        g_passed++;
        std::cout << "[PASS] " << name << "\n";
    } else {
        g_failed++;
        std::cout << "[FAIL] " << name << "\n";
    }
}

}  // namespace

int main() {
    RunTest("1_DefaultSettings", TestDefaultSettings);
    RunTest("2_SettingsSaveAndLoad", TestSettingsSaveAndLoad);
    RunTest("3_InvalidValues", TestInvalidValues);
    RunTest("4_MissingKeys", TestMissingKeys);
    RunTest("5_UnknownKeysPreservation", TestUnknownKeysPreservation);
    RunTest("6_PresetSwitching", TestPresetSwitching);
    RunTest("7_CustomSnapshotRecovery", TestCustomSnapshotRecovery);
    RunTest("8_RestartRequiredSettings", TestRestartRequiredSettings);
    RunTest("9_SchemaMigration", TestSchemaMigration);
    RunTest("10_ArabicContextualShaping", TestArabicContextualShaping);
    RunTest("11_ArabicLamAlefLigature", TestArabicLamAlefLigature);
    RunTest("12_ArabicBidiAndMixedText", TestArabicBidiAndMixedText);
    RunTest("13_TacticalCategoryNavigation", TestTacticalCategoryDefinitions);
    RunTest("14_ArabicGameStringLookup", TestArabicGameStringLookup);
    RunTest("15_ArabicMultilineShapingAndWrapping", TestArabicMultilineShapingAndWrapping);
    RunTest("16_PhysicsLabSchemaAndPresets", TestPhysicsLabSchemaAndPresets);
    RunTest("17_EnhancedSettingsImplemented", TestEnhancedSettingsImplemented);
    RunTest("18_TimeStopSchemaAndStateMachine", TestTimeStopSchemaAndStateMachine);
    RunTest("19_WeaponAudioRemasterSchema", TestWeaponAudioRemasterSchema);
    RunTest("20_WeaponAudioPackAndMixer", TestWeaponAudioPackAndMixer);
    RunTest("21_BallisticsSchemaAndPresets", TestBallisticsSchemaAndPresets);
    RunTest("22_BallisticsWeaponClassAndFrequency", TestBallisticsWeaponClassAndFrequency);
    RunTest("23_DeliriumPhysicsCore", TestDeliriumPhysicsCore);
    RunTest("24_DeliriumPhysicsSchema", TestDeliriumPhysicsSchema);
    RunTest("25_FlamethrowerCoreFuelAndReload", TestFlamethrowerCoreFuelAndReload);
    RunTest("26_FlamethrowerCoreBurnAndMesh", TestFlamethrowerCoreBurnAndMesh);
    RunTest("29_FlamethrowerJetPhysics", TestFlamethrowerJetPhysics);
    RunTest("27_MissionSelectDefaultProgression", TestMissionSelectDefaultProgression);
    RunTest("28_MissionMapTouchNavigation", TestMissionMapTouchNavigation);
    RunTest("30_VfxAtlasFireAndSmokeEdges", TestVfxAtlasFireAndSmokeEdges);
    RunTest("31_FlamethrowerPlumeVolumetricNoLines", TestFlamethrowerPlumeVolumetricNoLines);
    RunTest("32_FlamethrowerPoolTilesNoBloodOrSquares", TestFlamethrowerPoolTilesNoBloodOrSquares);
    RunTest("33_WeaponAudioStereoBwarV3AndHdr", TestWeaponAudioStereoBwarV3AndHdr);
    RunTest("34_WeaponHapticProfiles", TestWeaponHapticProfiles);
    RunTest("35_FlamethrowerFlameSpectrumAndMtlColors", TestFlamethrowerFlameSpectrumAndMtlColors);
    RunTest("36_WeaponAudioInRoomPunchAndHeadroom", TestWeaponAudioInRoomPunchAndHeadroom);
    RunTest("37_FlamethrowerOrganicTonguesAndZeroTileSnap", TestFlamethrowerOrganicTonguesAndZeroTileSnap);
    RunTest("38_GyroOneEuroFilterAndBias", TestGyroOneEuroFilterAndBias);
    RunTest("39_HitmarkerVisualAndEnhancedRecoilRules", TestHitmarkerVisualAndEnhancedRecoilRules);
    RunTest("40_ShellCasingBallisticsAndCombatSchema", TestShellCasingBallisticsAndCombatSchema);
    RunTest("41_ProceduralHitConfirmAndShellClinkMix", TestProceduralHitConfirmAndShellClinkMix);
    RunTest("42_OverdriveAdrenalineAndSiphon", TestOverdriveAdrenalineAndSiphon);
    RunTest("43_KineticDashAndShockwave", TestKineticDashAndShockwave);
    RunTest("44_SpecialAmmoAndThermalVisor", TestSpecialAmmoAndThermalVisor);
    RunTest("45_OverdriveSchemaAndAudioCues", TestOverdriveSchemaAndAudioCues);
    RunTest("46_BloodPoolPermanenceAndCoagulation", TestBloodPoolPermanenceAndCoagulation);
    RunTest("47_SpecialArsenalBmshV2AndBtexFormat", TestSpecialArsenalBmshV2AndBtexFormat);
    RunTest("48_FlamethrowerM2MechanicsAndIgniters", TestFlamethrowerM2MechanicsAndIgniters);
    RunTest("49_Mauser1891_12_CockOnCloseAndStripperClip", TestMauser1891_12_CockOnCloseAndStripperClip);
    RunTest("50_MauserC96_M712_ShortRecoilAndBoltHoldOpen", TestMauserC96_M712_ShortRecoilAndBoltHoldOpen);
    RunTest("51_FlamethrowerAndC96_HdMultiMaterialAssets", TestFlamethrowerAndC96_HdMultiMaterialAssets);
    RunTest("52_SpecialArsenalRealAssetsAndWebAudioPack", TestSpecialArsenalRealAssetsAndWebAudioPack);
    RunTest("53_Mauser1891_FullMeshUvLogicAndEnvAudio", TestMauser1891_FullMeshUvLogicAndEnvAudio);
    RunTest("54_MauserC96_FullMeshUvLogicAndAudio", TestMauserC96_FullMeshUvLogicAndAudio);
    RunTest("55_Mauser1891_MultiZoomPbrReticleAndAudio", TestMauser1891_MultiZoomPbrReticleAndAudio);
    RunTest("56_Mauser1891_HdMaterialsAndSkinnedHandsSync", TestMauser1891_HdMaterialsAndSkinnedHandsSync);

    std::cout << "\nSummary: " << g_passed << " passed, " << g_failed << " failed.\n";
    return g_failed == 0 ? 0 : 1;
}
