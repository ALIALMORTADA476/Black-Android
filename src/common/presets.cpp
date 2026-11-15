#include "common/presets.h"

namespace modsettings {
const char *PresetName(PresetKind kind) {
  switch (kind) {
  case PresetKind::Original:
    return "original";
  case PresetKind::Enhanced:
    return "enhanced";
  case PresetKind::Performance:
    return "performance";
  case PresetKind::Custom:
  default:
    return "custom";
  }
}

PresetKind PresetFromName(std::string_view name) {
  if (name == "original")
    return PresetKind::Original;
  if (name == "enhanced")
    return PresetKind::Enhanced;
  if (name == "performance")
    return PresetKind::Performance;
  return PresetKind::Custom;
}

PresetStateMachine &Presets() {
  static PresetStateMachine instance;
  return instance;
}

bool PresetStateMachine::IsPresetManaged(SettingId id) {
  switch (id) {
  // Visual & Performance settings managed by Original / Enhanced / Performance
  // presets. Strictly EXCLUDES cheats, controller sensitivities/deadzones,
  // keybindings, and language!
  case SettingId::RenderPercent:
  case SettingId::FpsCap:
  case SettingId::Vsync:
  case SettingId::AnisotropicFiltering:
  case SettingId::AntiAliasing:
  case SettingId::ScreenFilter:
  case SettingId::Bloom:
  case SettingId::Fog:
  case SettingId::Hdr:
  case SettingId::ReloadBlur:
  case SettingId::DrawDistance:
  case SettingId::DynamicResolution:
  case SettingId::DynamicTargetFps:
  case SettingId::DynamicMinPercent:
  case SettingId::DynamicMaxPercent:
  case SettingId::DynamicHold:
  case SettingId::FpsCounter:
  case SettingId::GraphicsToneMapping:
  case SettingId::GraphicsCasSharpenEnabled:
  case SettingId::GraphicsCasSharpenStrength:
  case SettingId::GraphicsBloomIntensity:
  case SettingId::GraphicsTextureLodBias:
  case SettingId::GraphicsTrilinearFiltering:
  case SettingId::GraphicsCinematicGrade:
  case SettingId::GraphicsLensVignette:
  case SettingId::GraphicsFilmGrain:
  case SettingId::GraphicsBlastLensPulse:
  case SettingId::GraphicsSoftShadows:
  case SettingId::CombatScreenDamageVignette:
  case SettingId::CombatDamageVignetteIntensity:
  case SettingId::CombatLowHealthDesaturation:
  case SettingId::CombatExplosionGlowBoost:
  case SettingId::CombatParticleFadeScale:
    return true;
  default:
    return false;
  }
}

void PresetStateMachine::Bind(const SettingsRegistry &reg,
                              const app::Settings &s, const game::Options &o) {
  if (active_kind_ == PresetKind::Custom) {
    CaptureCustomSnapshot(reg, s, o);
  }
}

void PresetStateMachine::CaptureCustomSnapshot(const SettingsRegistry &reg,
                                               const app::Settings &s,
                                               const game::Options &o) {
  const EnhancedOptions &enh = reg.GetEnhanced();
  custom_snapshot_.valid = true;
  custom_snapshot_.render_percent = s.render_percent;
  custom_snapshot_.fps_cap = s.fps_cap;
  custom_snapshot_.vsync = s.vsync;
  custom_snapshot_.aniso = s.aniso;

  custom_snapshot_.anti_aliasing = o.anti_aliasing;
  custom_snapshot_.screen_filter = o.screen_filter;
  custom_snapshot_.bloom = o.bloom;
  custom_snapshot_.fog = o.fog;
  custom_snapshot_.hdr = o.hdr;
  custom_snapshot_.reload_blur = o.reload_blur;
  custom_snapshot_.draw_distance = o.draw_distance;
  custom_snapshot_.dynamic_resolution = o.dynamic_resolution;
  custom_snapshot_.dynamic_target_fps = o.dynamic_target_fps;
  custom_snapshot_.dynamic_min_percent = o.dynamic_min_percent;
  custom_snapshot_.dynamic_max_percent = o.dynamic_max_percent;
  custom_snapshot_.dynamic_hold = o.dynamic_hold;
  custom_snapshot_.fps_counter = o.fps_counter;
  custom_snapshot_.skip_intros = o.skip_intros;
  custom_snapshot_.fmv_scale = o.fmv_scale;

  custom_snapshot_.tone_mapping = enh.tone_mapping;
  custom_snapshot_.cas_sharpen_enabled = enh.cas_sharpen_enabled;
  custom_snapshot_.cas_sharpen_strength = enh.cas_sharpen_strength;
  custom_snapshot_.bloom_intensity = enh.bloom_intensity;
  custom_snapshot_.texture_lod_bias = enh.texture_lod_bias;
  custom_snapshot_.trilinear_filtering = enh.trilinear_filtering;
  custom_snapshot_.cinematic_grade = enh.cinematic_grade;
  custom_snapshot_.lens_vignette = enh.lens_vignette;
  custom_snapshot_.film_grain = enh.film_grain;
  custom_snapshot_.blast_lens_pulse = enh.blast_lens_pulse;
  custom_snapshot_.soft_shadows = enh.soft_shadows;
  custom_snapshot_.screen_damage_vignette = enh.screen_damage_vignette;
  custom_snapshot_.damage_vignette_intensity = enh.damage_vignette_intensity;
  custom_snapshot_.low_health_desaturation = enh.low_health_desaturation;
  custom_snapshot_.explosion_glow_boost = enh.explosion_glow_boost;
  custom_snapshot_.particle_fade_scale = enh.particle_fade_scale;
}

void PresetStateMachine::RestoreCustomSnapshot(SettingsRegistry &reg,
                                               app::Settings &s,
                                               game::Options &o) {
  if (!custom_snapshot_.valid)
    return;

  reg.SetUint(SettingId::RenderPercent, custom_snapshot_.render_percent, s, o);
  reg.SetUint(SettingId::FpsCap, custom_snapshot_.fps_cap, s, o);
  reg.SetBool(SettingId::Vsync, custom_snapshot_.vsync, s, o);
  reg.SetUint(SettingId::AnisotropicFiltering, custom_snapshot_.aniso, s, o);

  reg.SetUint(SettingId::AntiAliasing, custom_snapshot_.anti_aliasing, s, o);
  reg.SetUint(SettingId::ScreenFilter, custom_snapshot_.screen_filter, s, o);
  reg.SetBool(SettingId::Bloom, custom_snapshot_.bloom, s, o);
  reg.SetBool(SettingId::Fog, custom_snapshot_.fog, s, o);
  reg.SetBool(SettingId::Hdr, custom_snapshot_.hdr, s, o);
  reg.SetBool(SettingId::ReloadBlur, custom_snapshot_.reload_blur, s, o);
  reg.SetUint(SettingId::DrawDistance, custom_snapshot_.draw_distance, s, o);
  reg.SetBool(SettingId::DynamicResolution, custom_snapshot_.dynamic_resolution,
              s, o);
  reg.SetUint(SettingId::DynamicMinPercent, 20u, s, o);
  reg.SetUint(SettingId::DynamicMaxPercent,
              custom_snapshot_.dynamic_max_percent, s, o);
  reg.SetUint(SettingId::DynamicMinPercent,
              custom_snapshot_.dynamic_min_percent, s, o);
  reg.SetUint(SettingId::DynamicTargetFps, custom_snapshot_.dynamic_target_fps,
              s, o);
  reg.SetUint(SettingId::DynamicHold, custom_snapshot_.dynamic_hold, s, o);
  reg.SetBool(SettingId::FpsCounter, custom_snapshot_.fps_counter, s, o);

  reg.SetUint(SettingId::GraphicsToneMapping, custom_snapshot_.tone_mapping, s,
              o);
  reg.SetBool(SettingId::GraphicsCasSharpenEnabled,
              custom_snapshot_.cas_sharpen_enabled, s, o);
  reg.SetUint(SettingId::GraphicsCasSharpenStrength,
              custom_snapshot_.cas_sharpen_strength, s, o);
  reg.SetUint(SettingId::GraphicsBloomIntensity,
              custom_snapshot_.bloom_intensity, s, o);
  reg.SetFloat(SettingId::GraphicsTextureLodBias,
               custom_snapshot_.texture_lod_bias, s, o);
  reg.SetBool(SettingId::GraphicsTrilinearFiltering,
              custom_snapshot_.trilinear_filtering, s, o);
  reg.SetUint(SettingId::GraphicsCinematicGrade,
              custom_snapshot_.cinematic_grade, s, o);
  reg.SetUint(SettingId::GraphicsLensVignette, custom_snapshot_.lens_vignette,
              s, o);
  reg.SetUint(SettingId::GraphicsFilmGrain, custom_snapshot_.film_grain, s, o);
  reg.SetBool(SettingId::GraphicsBlastLensPulse,
              custom_snapshot_.blast_lens_pulse, s, o);
  reg.SetBool(SettingId::CombatScreenDamageVignette,
              custom_snapshot_.screen_damage_vignette, s, o);
  reg.SetUint(SettingId::CombatDamageVignetteIntensity,
              custom_snapshot_.damage_vignette_intensity, s, o);
  reg.SetBool(SettingId::CombatLowHealthDesaturation,
              custom_snapshot_.low_health_desaturation, s, o);
  reg.SetUint(SettingId::CombatExplosionGlowBoost,
              custom_snapshot_.explosion_glow_boost, s, o);
  reg.SetUint(SettingId::CombatParticleFadeScale,
              custom_snapshot_.particle_fade_scale, s, o);
}

bool PresetStateMachine::ApplyPreset(PresetKind target, SettingsRegistry &reg,
                                     app::Settings &s, game::Options &o) {
  // Save Custom snapshot before leaving Custom so the user never loses manual
  // adjustments
  if (active_kind_ == PresetKind::Custom && target != PresetKind::Custom) {
    CaptureCustomSnapshot(reg, s, o);
  }

  switch (target) {
  case PresetKind::Original: {
    // Restores faithful 2006 console visual baseline (leaves cheats, controls,
    // language untouched)
    reg.SetUint(SettingId::RenderPercent, 67u, s, o);
    reg.SetUint(SettingId::FpsCap, 0u, s, o);
    reg.SetBool(SettingId::Vsync, false, s, o);
    reg.SetUint(SettingId::AnisotropicFiltering, 0u, s, o);
    reg.SetUint(SettingId::AntiAliasing, 0u, s, o);
    reg.SetUint(SettingId::ScreenFilter, 0u, s, o);
    reg.SetBool(SettingId::Bloom, true, s, o);
    reg.SetBool(SettingId::Fog, true, s, o);
    reg.SetBool(SettingId::Hdr, false, s, o);
    reg.SetBool(SettingId::ReloadBlur, true, s, o);
    reg.SetUint(SettingId::DrawDistance, 1u, s, o);
    reg.SetBool(SettingId::DynamicResolution, false, s, o);
    reg.SetBool(SettingId::FpsCounter, false, s, o);
    auto &enh = reg.GetEnhanced();
    enh.tone_mapping = 0;
    enh.cas_sharpen_enabled = false;
    enh.cas_sharpen_strength = 45;
    enh.bloom_intensity = 100;
    enh.texture_lod_bias = 0.0f;
    enh.trilinear_filtering = false;
    enh.cinematic_grade = 0;
    enh.lens_vignette = 0;
    enh.film_grain = 0;
    enh.blast_lens_pulse = false;
    enh.screen_damage_vignette = false;
    enh.low_health_desaturation = false;
    enh.explosion_glow_boost = 100;
    enh.particle_fade_scale = 100;
    active_kind_ = PresetKind::Original;
    return true;
  }

  case PresetKind::Enhanced: {
    // Enables supported high-clarity graphics upgrades balanced for modern
    // mobile GPUs
    reg.SetUint(SettingId::RenderPercent, 100u, s, o);
    reg.SetUint(SettingId::FpsCap, 0u, s, o);
    reg.SetBool(SettingId::Vsync, false, s, o);
    reg.SetUint(SettingId::AnisotropicFiltering, 16u, s, o);
    reg.SetUint(SettingId::AntiAliasing, 1u, s, o); // FXAA
    reg.SetUint(SettingId::ScreenFilter, 0u, s, o);
    reg.SetBool(SettingId::Bloom, true, s, o);
    reg.SetBool(SettingId::Fog, true, s, o);
    reg.SetBool(SettingId::ReloadBlur, true, s, o);
    reg.SetUint(SettingId::DrawDistance, 2u, s, o);
    reg.SetBool(SettingId::DynamicResolution, true, s, o);
    reg.SetUint(SettingId::DynamicMinPercent, 20u, s, o);
    reg.SetUint(SettingId::DynamicMaxPercent, 100u, s, o);
    reg.SetUint(SettingId::DynamicMinPercent, 67u, s, o);
    reg.SetUint(SettingId::DynamicTargetFps, 60u, s, o);
    reg.SetUint(SettingId::DynamicHold, 5u, s, o);
    auto &enh = reg.GetEnhanced();
    enh.tone_mapping = 1; // Reinhard HDR
    enh.cas_sharpen_enabled = true;
    enh.cas_sharpen_strength = 45;
    enh.bloom_intensity = 115;
    enh.texture_lod_bias = -0.5f;
    enh.trilinear_filtering = true;
    enh.cinematic_grade = 0;
    enh.lens_vignette = 15;
    enh.film_grain = 0;
    enh.blast_lens_pulse = true;
    active_kind_ = PresetKind::Enhanced;
    return true;
  }

  case PresetKind::Performance: {
    // Disables expensive optional enhancements to sustain smooth frame rates &
    // cooler thermals
    reg.SetUint(SettingId::RenderPercent, 67u, s, o);
    reg.SetUint(SettingId::FpsCap, 0u, s, o);
    reg.SetBool(SettingId::Vsync, false, s, o);
    reg.SetUint(SettingId::AnisotropicFiltering, 4u, s, o);
    reg.SetUint(SettingId::AntiAliasing, 0u, s, o); // Off
    reg.SetUint(SettingId::ScreenFilter, 0u, s, o);
    reg.SetBool(SettingId::Bloom, false, s,
                o); // Disable glow composite overhead
    reg.SetBool(SettingId::Fog, true, s, o);
    reg.SetBool(SettingId::Hdr, false, s, o);
    reg.SetBool(SettingId::ReloadBlur, false, s, o);
    reg.SetUint(SettingId::DrawDistance, 1u, s, o);
    reg.SetBool(SettingId::DynamicResolution, true, s, o);
    reg.SetUint(SettingId::DynamicMinPercent, 20u, s, o);
    reg.SetUint(SettingId::DynamicMaxPercent, 80u, s, o);
    reg.SetUint(SettingId::DynamicMinPercent, 45u, s, o);
    reg.SetUint(SettingId::DynamicTargetFps, 60u, s, o);
    reg.SetUint(SettingId::DynamicHold, 3u, s, o);
    auto &enh = reg.GetEnhanced();
    enh.tone_mapping = 0;
    enh.cas_sharpen_enabled = false;
    enh.cinematic_grade = 0;
    enh.lens_vignette = 0;
    enh.film_grain = 0;
    enh.blast_lens_pulse = false;
    active_kind_ = PresetKind::Performance;
    return true;
  }

  case PresetKind::Custom:
    if (custom_snapshot_.valid)
      RestoreCustomSnapshot(reg, s, o);
    active_kind_ = PresetKind::Custom;
    return true;
  }
  return false;
}

void PresetStateMachine::NotifySettingChanged(SettingId id,
                                              const SettingsRegistry &reg,
                                              const app::Settings &s,
                                              const game::Options &o) {
  if (!IsPresetManaged(id))
    return;
  active_kind_ = PresetKind::Custom;
  CaptureCustomSnapshot(reg, s, o);
}

void PresetStateMachine::Serialize(toml::table &presets_table) const {
  presets_table.insert_or_assign("active", PresetName(active_kind_));

  toml::table snap;
  snap.insert_or_assign("saved", custom_snapshot_.valid);
  if (custom_snapshot_.valid) {
    snap.insert_or_assign("render_percent",
                          int64_t(custom_snapshot_.render_percent));
    snap.insert_or_assign("fps_cap", int64_t(custom_snapshot_.fps_cap));
    snap.insert_or_assign("vsync", custom_snapshot_.vsync);
    snap.insert_or_assign("anisotropic_filtering",
                          int64_t(custom_snapshot_.aniso));
    snap.insert_or_assign("anti_aliasing",
                          int64_t(custom_snapshot_.anti_aliasing));
    snap.insert_or_assign("screen_filter",
                          int64_t(custom_snapshot_.screen_filter));
    snap.insert_or_assign("bloom", custom_snapshot_.bloom);
    snap.insert_or_assign("fog", custom_snapshot_.fog);
    snap.insert_or_assign("hdr", custom_snapshot_.hdr);
    snap.insert_or_assign("reload_blur", custom_snapshot_.reload_blur);
    snap.insert_or_assign("draw_distance",
                          int64_t(custom_snapshot_.draw_distance));
    snap.insert_or_assign("dynamic_resolution",
                          custom_snapshot_.dynamic_resolution);
    snap.insert_or_assign("dynamic_target_fps",
                          int64_t(custom_snapshot_.dynamic_target_fps));
    snap.insert_or_assign("dynamic_min_percent",
                          int64_t(custom_snapshot_.dynamic_min_percent));
    snap.insert_or_assign("dynamic_max_percent",
                          int64_t(custom_snapshot_.dynamic_max_percent));
    snap.insert_or_assign("dynamic_hold",
                          int64_t(custom_snapshot_.dynamic_hold));
    snap.insert_or_assign("fps_counter", custom_snapshot_.fps_counter);
  }
  presets_table.insert_or_assign("last_custom", snap);
}

void PresetStateMachine::Deserialize(const toml::table &presets_table) {
  std::string active_name =
      presets_table["active"].value_or(std::string("custom"));
  active_kind_ = PresetFromName(active_name);

  if (const toml::table *snap = presets_table["last_custom"].as_table()) {
    custom_snapshot_.valid = (*snap)["saved"].value_or(false);
    if (custom_snapshot_.valid) {
      custom_snapshot_.render_percent =
          (*snap)["render_percent"].value_or(custom_snapshot_.render_percent);
      custom_snapshot_.fps_cap =
          (*snap)["fps_cap"].value_or(custom_snapshot_.fps_cap);
      custom_snapshot_.vsync =
          (*snap)["vsync"].value_or(custom_snapshot_.vsync);
      custom_snapshot_.aniso =
          (*snap)["anisotropic_filtering"].value_or(custom_snapshot_.aniso);
      custom_snapshot_.anti_aliasing =
          (*snap)["anti_aliasing"].value_or(custom_snapshot_.anti_aliasing);
      custom_snapshot_.screen_filter =
          (*snap)["screen_filter"].value_or(custom_snapshot_.screen_filter);
      custom_snapshot_.bloom =
          (*snap)["bloom"].value_or(custom_snapshot_.bloom);
      custom_snapshot_.fog = (*snap)["fog"].value_or(custom_snapshot_.fog);
      custom_snapshot_.hdr = (*snap)["hdr"].value_or(custom_snapshot_.hdr);
      custom_snapshot_.reload_blur =
          (*snap)["reload_blur"].value_or(custom_snapshot_.reload_blur);
      custom_snapshot_.draw_distance =
          (*snap)["draw_distance"].value_or(custom_snapshot_.draw_distance);
      custom_snapshot_.dynamic_resolution =
          (*snap)["dynamic_resolution"].value_or(
              custom_snapshot_.dynamic_resolution);
      custom_snapshot_.dynamic_target_fps =
          (*snap)["dynamic_target_fps"].value_or(
              custom_snapshot_.dynamic_target_fps);
      custom_snapshot_.dynamic_min_percent =
          (*snap)["dynamic_min_percent"].value_or(
              custom_snapshot_.dynamic_min_percent);
      custom_snapshot_.dynamic_max_percent =
          (*snap)["dynamic_max_percent"].value_or(
              custom_snapshot_.dynamic_max_percent);
      custom_snapshot_.dynamic_hold =
          (*snap)["dynamic_hold"].value_or(custom_snapshot_.dynamic_hold);
      custom_snapshot_.fps_counter =
          (*snap)["fps_counter"].value_or(custom_snapshot_.fps_counter);
    }
  }
}

} // namespace modsettings
