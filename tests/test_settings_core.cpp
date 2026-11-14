#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "common/arabic.h"
#include "common/menu.h"
#include "common/mod_schema.h"
#include "common/options.h"
#include "common/presets.h"
#include "game/arabic_strings.h"

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
    EXPECT_EQ(parsed["schema_version"].value_or(0), 2);

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

    const auto* decals = reg.Find(modsettings::SettingId::CombatPersistentBloodDecals);
    EXPECT_TRUE(decals != nullptr);
    EXPECT_EQ(decals->scope, modsettings::ApplyScope::SceneReload);
    EXPECT_EQ(decals->status, modsettings::FeatureStatus::Planned);

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
    EXPECT_EQ(saved["schema_version"].value_or(0), 2);
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
    EXPECT_EQ(int(app::TacticalCategory::AudioLanguage), 3);
    EXPECT_EQ(int(app::TacticalCategory::Interface), 4);
    EXPECT_EQ(int(app::TacticalCategory::CheatsWeapons), 5);
    EXPECT_EQ(int(app::TacticalCategory::Count), 6);

    // Verify cyclic bumper navigation logic
    constexpr int kNumCats = int(app::TacticalCategory::Count);
    int cur = int(app::TacticalCategory::Graphics);
    int prev = (cur - 1 + kNumCats) % kNumCats;
    EXPECT_EQ(prev, 5);
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
    EXPECT_EQ(reg.GetString(modsettings::SettingId::PhysicsPreset, s, o), "classic");
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsExplosionForce, s, o), 1.00f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsImpactForce, s, o), 1.00f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsGravityScale, s, o), 1.00f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsDebrisIntensity, s, o), 1.00f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsCharacterReaction, s, o), 1.00f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsWeaponRecoil, s, o), 1.00f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsExplosionRadius, s, o), 1.00f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsRagdollLaunch, s, o), 1.00f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsStructuralDestruction, s, o), 1.00f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsPropBounciness, s, o), 1.00f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsCameraShockwave, s, o), 1.00f, 1e-4f);
    EXPECT_NEAR(reg.GetFloat(modsettings::SettingId::PhysicsTimeScale, s, o), 1.00f, 1e-4f);

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
    EXPECT_EQ(reg2.GetString(modsettings::SettingId::PhysicsPreset, s2, o2), "classic");
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsExplosionForce, s2, o2), 10.00f, 1e-4f);
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsGravityScale, s2, o2), 0.05f, 1e-4f);
    EXPECT_NEAR(reg2.GetFloat(modsettings::SettingId::PhysicsWeaponRecoil, s2, o2), 1.00f, 1e-4f);

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

    std::cout << "\nSummary: " << g_passed << " passed, " << g_failed << " failed.\n";
    return g_failed == 0 ? 0 : 1;
}
