#!/usr/bin/env python3
"""
BLACK Enhanced Edition — Stage 1 Settings Core & Preset State Machine Test Suite

Implements verification for all 9 required test categories:
1. Default settings.
2. Settings save/load.
3. Invalid values.
4. Missing keys.
5. Unknown keys.
6. Preset switching.
7. Custom snapshot recovery.
8. Restart-required settings.
9. Schema migration.
"""

import math
import os
import shutil
import sys
import tempfile
import tomllib
from typing import Any, Dict, List, Optional, Tuple

SCHEMA_VERSION = 4
DEFAULTS_VERSION = 2

PRESET_MANAGED_KEYS = {
    "render_percent", "fps_cap", "vsync", "anisotropic_filtering", "anti_aliasing",
    "screen_filter", "bloom", "fog", "hdr", "reload_blur", "draw_distance",
    "dynamic_resolution", "dynamic_target_fps", "dynamic_min_percent", "dynamic_max_percent",
    "dynamic_hold", "fps_counter", "tone_mapping", "cas_sharpen_enabled",
    "cas_sharpen_strength", "bloom_intensity", "texture_lod_bias", "trilinear_filtering",
    "soft_shadows", "screen_damage_vignette", "damage_vignette_intensity",
    "low_health_desaturation", "explosion_glow_boost", "particle_fade_scale"
}

EXCLUDED_FROM_PRESETS = {
    "infinite_ammo", "rapid_fire", "no_recoil", "game_speed", "god_mode",
    "one_hit_kill", "infinite_grenades", "unlock_levels", "unlock_black_ops",
    "esp", "aimbot", "fov", "viewmodel_fov", "walk_speed", "hold_aim", "invert_mouse",
    "mouse_sensitivity", "touch_sensitivity", "left_stick_deadzone", "right_stick_deadzone",
    "left_stick_sensitivity", "right_stick_sensitivity", "language", "keys"
}

PLANNED_LOCKED_KEYS = {
    "hd_texture_packs", "blood_particle_multiplier", "persistent_blood_decals",
    "diverse_bullet_impacts", "custom_character_models",
    "arabic_voice_pack", "overlay_enabled"
}

BOOL_KEYS = {
    "vsync", "skip_intros", "fps_counter", "reload_blur", "bloom", "fog", "hdr",
    "dynamic_resolution", "hold_aim", "invert_mouse", "infinite_ammo", "no_recoil",
    "god_mode", "one_hit_kill", "infinite_grenades", "unlock_levels", "unlock_black_ops",
    "esp", "aimbot", "cas_sharpen_enabled", "trilinear_filtering", "hd_texture_packs",
    "screen_damage_vignette", "low_health_desaturation", "persistent_blood_decals",
    "diverse_bullet_impacts", "hitmarker_feedback", "shell_casings",
    "ragdoll_60fps_physics", "custom_character_models",
    "combat_dynamic_boost", "mod_audio_pack_enabled", "arabic_ui_enabled",
    "arabic_rtl_mirror", "arabic_subtitles_overlay", "arabic_flash_menus",
    "arabic_voice_pack", "enhanced_recoil_feedback", "disable_aim_assist",
    "advanced_perf_hud", "show_restart_banners", "hide_hud_on_screenshots", "overlay_enabled",
    "time_stop_enabled"
}

ALLOWED_UINTS = {
    "anisotropic_filtering": [0, 2, 4, 8, 16],
    "anti_aliasing": [0, 1, 2],
    "draw_distance": [1, 2, 4, 8],
    "dynamic_target_fps": [30, 45, 60, 90, 120],
    "fmv_scale": [0, 1],
    "audio_output": [0, 1, 2, 3],
    "fps_cap": [0, 1, 30, 60, 75, 90, 120, 144, 165, 240, 360],
    "time_stop_mode": [0, 1],
}

ALLOWED_STRINGS = {
    "language": ["ara", "int", "fra", "deu", "ita", "esp", "ind"],
    "display_mode": ["fullscreen", "borderless", "windowed"]
}

BOUNDS = {
    "render_percent": (20, 200, 5),
    "dynamic_min_percent": (20, 200, 5),
    "dynamic_max_percent": (20, 200, 5),
    "dynamic_hold": (1, 30, 1),
    "fov": (45, 140, 1),
    "viewmodel_fov": (45, 140, 1),
    "walk_speed": (10, 100, 5),
    "left_stick_deadzone": (0, 50, 1),
    "right_stick_deadzone": (0, 50, 1),
    "rapid_fire": (1, 4, 1),
    "game_speed": (25, 200, 25),
    "mouse_sensitivity": (0.1, 5.0, 0.1),
    "touch_sensitivity": (0.1, 5.0, 0.1),
    "left_stick_sensitivity": (0.1, 3.0, 0.1),
    "right_stick_sensitivity": (0.1, 3.0, 0.1),
    "explosion_force": (0.25, 10.00, 0.05),
    "impact_force": (0.25, 10.00, 0.05),
    "gravity_scale": (0.05, 5.00, 0.05),
    "debris_intensity": (0.25, 6.00, 0.05),
    "character_reaction": (0.25, 8.00, 0.05),
    "weapon_recoil": (0.00, 4.00, 0.05),
    "explosion_radius": (0.50, 5.00, 0.05),
    "ragdoll_launch": (0.25, 10.00, 0.05),
    "structural_destruction": (0.25, 10.00, 0.05),
    "prop_bounciness": (0.25, 6.00, 0.05),
    "camera_shockwave": (0.00, 4.00, 0.05),
    "physics_time_scale": (0.20, 2.00, 0.05),
    "time_stop_max_duration": (0.00, 30.00, 1.00),
    "time_stop_cooldown": (0.00, 30.00, 1.00),
    "weapon_audio_volume": (0, 150, 5),
    "mechanical_audio_volume": (0, 150, 5),
    "reload_audio_volume": (0, 150, 5),
    "environmental_audio_volume": (0, 150, 5),
    "audio_tail_intensity": (0, 150, 5),
    "audio_echo_intensity": (0, 150, 5),
    "audio_distant_gunfire": (0, 150, 5),
    "audio_bullet_flyby": (0, 150, 5),
    "audio_variation_percent": (0, 100, 5),
}


class SettingsRegistryModel:
    def __init__(self):
        self.reset_to_defaults()

    def reset_to_defaults(self):
        self.schema_version = SCHEMA_VERSION
        self.defaults = DEFAULTS_VERSION
        self.display_mode = "fullscreen"
        self.width = 0
        self.height = 0
        self.render_percent = 67
        self.fps_cap = 0
        self.vsync = False
        self.anisotropic_filtering = 0
        self.join_ip = ""

        # Game options
        self.skip_intros = False
        self.fps_counter = False
        self.fmv_scale = 0
        self.screen_filter = 0
        self.reload_blur = True
        self.bloom = True
        self.fog = True
        self.anti_aliasing = 0
        self.hdr = False
        self.draw_distance = 1
        self.dynamic_resolution = False
        self.dynamic_target_fps = 60
        self.dynamic_min_percent = 40
        self.dynamic_max_percent = 100
        self.dynamic_hold = 5
        self.audio_output = 0
        self.language = "ara"

        self.fov = 70
        self.viewmodel_fov = 70
        self.walk_speed = 50
        self.hold_aim = True
        self.invert_mouse = False
        self.mouse_sensitivity = 1.0
        self.touch_sensitivity = 1.0
        self.left_stick_deadzone = 0
        self.right_stick_deadzone = 0
        self.left_stick_sensitivity = 1.0
        self.right_stick_sensitivity = 1.0

        self.infinite_ammo = False
        self.rapid_fire = 1
        self.no_recoil = False
        self.game_speed = 100
        self.god_mode = False
        self.one_hit_kill = False
        self.infinite_grenades = False
        self.unlock_levels = False
        self.unlock_black_ops = False
        self.esp = False
        self.aimbot = False
        self.keys: Dict[str, str] = {}

        # Enhanced Sections
        self.graphics: Dict[str, Any] = {
            "tone_mapping": 0,
            "cas_sharpen_enabled": False,
            "cas_sharpen_strength": 35,
            "bloom_intensity": 100,
            "texture_lod_bias": 0.0,
            "trilinear_filtering": False,
            "soft_shadows": 0,
            "ambient_occlusion": 0,
            "hd_texture_packs": False,
            "weapon_reflections": True,
        }
        self.combat: Dict[str, Any] = {
            "screen_damage_vignette": False,
            "damage_vignette_intensity": 75,
            "low_health_desaturation": False,
            "explosion_glow_boost": 100,
            "particle_fade_scale": 100,
            "blood_particle_multiplier": 1,
            "persistent_blood_decals": False,
            "max_blood_decals": 512,
            "corpse_persistence_mode": 2,
            "diverse_bullet_impacts": False,
            "hitmarker_feedback": True,
            "shell_casings": True,
        }
        self.characters: Dict[str, Any] = {
            "ragdoll_60fps_physics": True,
            "character_lod_scale": 1,
            "custom_character_models": False,
        }
        self.audio: Dict[str, Any] = {
            "combat_dynamic_boost": False,
            "master_gain_percent": 100,
            "mod_audio_pack_enabled": True,
            "weapon_audio_volume": 100,
            "mechanical_audio_volume": 100,
            "reload_audio_volume": 100,
            "environmental_audio_volume": 100,
            "audio_tail_intensity": 100,
            "audio_echo_intensity": 100,
            "audio_distant_gunfire": 100,
            "audio_bullet_flyby": 100,
            "audio_variation_percent": 50,
        }
        self.arabic: Dict[str, Any] = {
            "arabic_ui_enabled": True,
            "arabic_rtl_mirror": True,
            "arabic_subtitles_overlay": False,
            "arabic_flash_menus": True,
            "arabic_voice_pack": False,
        }
        self.gameplay: Dict[str, Any] = {
            "enhanced_recoil_feedback": True,
            "disable_aim_assist": False,
        }
        self.interface: Dict[str, Any] = {
            "settings_theme_accent": "brass",
            "advanced_perf_hud": False,
            "show_restart_banners": True,
            "hide_hud_on_screenshots": False,
        }
        self.packages: Dict[str, Any] = {
            "overlay_enabled": False,
        }
        self.physics_lab: Dict[str, Any] = {
            "preset": "realistic",
            "explosion_force": 2.20,
            "impact_force": 2.10,
            "gravity_scale": 1.15,
            "debris_intensity": 2.25,
            "character_reaction": 1.85,
            "weapon_recoil": 1.30,
            "explosion_radius": 1.45,
            "ragdoll_launch": 1.85,
            "structural_destruction": 2.40,
            "prop_bounciness": 1.25,
            "camera_shockwave": 1.50,
            "physics_time_scale": 1.00,
            "time_stop_enabled": False,
            "time_stop_mode": 0,
            "time_stop_max_duration": 0.00,
            "time_stop_cooldown": 0.00,
        }
        self.presets: Dict[str, Any] = {
            "active": "custom",
            "last_custom": {"saved": False}
        }
        self.unknown_nodes: Dict[str, Any] = {}
        self.boot_language = "ara"

    def capture_boot_snapshot(self):
        self.boot_language = self.language

    def requires_app_restart(self) -> bool:
        return self.language != self.boot_language

    def pending_restart_count(self) -> int:
        return 1 if self.requires_app_restart() else 0

    def sanitize_val(self, key: str, val: Any, fallback: Any = None) -> Tuple[Any, bool]:
        if key in BOOL_KEYS:
            if isinstance(val, bool):
                return val, False
            return (fallback if isinstance(fallback, bool) else True), True
        if key in ALLOWED_UINTS:
            if isinstance(val, bool) or not isinstance(val, int) or val < 0:
                return ALLOWED_UINTS[key][0], True
            if val not in ALLOWED_UINTS[key]:
                best = min(ALLOWED_UINTS[key], key=lambda x: abs(x - val))
                return best, True
            return val, False
        if key in ALLOWED_STRINGS:
            if not isinstance(val, str) or val not in ALLOWED_STRINGS[key]:
                return ALLOWED_STRINGS[key][0], True
            return val, False
        if key in BOUNDS:
            lo, hi, step = BOUNDS[key]
            if isinstance(lo, float):
                if isinstance(val, bool) or not isinstance(val, (int, float)) or not math.isfinite(val):
                    return (fallback if fallback is not None else 1.0), True
                clamped = max(lo, min(hi, float(val)))
                return clamped, (clamped != val)
            else:
                if isinstance(val, bool) or not isinstance(val, int) or val < 0:
                    return lo, True
                clamped = max(lo, min(hi, val))
                if step > 1:
                    rem = (clamped - lo) % step
                    clamped -= rem
                return clamped, (clamped != val)
        return val, False

    def set_value(self, key: str, val: Any, section: Optional[str] = None) -> Tuple[bool, str]:
        # Guard against modifying planned/locked switches as functional
        if key in PLANNED_LOCKED_KEYS:
            return False, f"Setting '{key}' is Planned and locked"
        sanitized, _ = self.sanitize_val(key, val)
        if section and hasattr(self, section):
            sec_dict = getattr(self, section)
            sec_dict[key] = sanitized
            return True, ""
        if hasattr(self, key):
            setattr(self, key, sanitized)
            return True, ""
        return False, f"Unknown key {key}"

    def load_from_dict(self, data: Dict[str, Any]) -> Dict[str, Any]:
        self.reset_to_defaults()
        sanitized_count = 0
        prev_version = data.get("schema_version", 0)
        migrated = prev_version < SCHEMA_VERSION

        defaults_ver = data.get("defaults", 0)

        known_roots = {
            "schema_version", "defaults", "display_mode", "width", "height", "render_percent",
            "fps_cap", "vsync", "anisotropic_filtering", "join_ip", "skip_intros", "fps_counter",
            "fmv_scale", "screen_filter", "reload_blur", "bloom", "fog", "anti_aliasing", "hdr",
            "draw_distance", "dynamic_resolution", "dynamic_target_fps", "dynamic_min_percent",
            "dynamic_max_percent", "dynamic_hold", "audio_output", "language", "fov",
            "viewmodel_fov", "walk_speed", "hold_aim", "invert_mouse", "mouse_sensitivity",
            "touch_sensitivity", "left_stick_deadzone", "right_stick_deadzone",
            "left_stick_sensitivity", "right_stick_sensitivity", "infinite_ammo", "rapid_fire",
            "no_recoil", "game_speed", "god_mode", "one_hit_kill", "infinite_grenades",
            "unlock_levels", "unlock_black_ops", "esp", "aimbot", "keys", "presets",
            "graphics", "combat", "characters", "audio", "arabic", "gameplay", "interface",
            "packages"
        }

        # Populate root attributes
        for k, v in data.items():
            if k in known_roots:
                if k == "keys" and isinstance(v, dict):
                    self.keys = {str(kk): str(vv) for kk, vv in v.items()}
                elif k in ("presets", "graphics", "combat", "characters", "audio", "arabic", "gameplay", "interface", "packages"):
                    if isinstance(v, dict):
                        target_dict = getattr(self, k)
                        for sk, sv in v.items():
                            s_clean, adj = self.sanitize_val(sk, sv, target_dict.get(sk))
                            if adj: sanitized_count += 1
                            target_dict[sk] = s_clean
                else:
                    cur = getattr(self, k, None)
                    v_clean, adj = self.sanitize_val(k, v, cur)
                    if adj: sanitized_count += 1
                    setattr(self, k, v_clean)
            else:
                self.unknown_nodes[k] = v

        # Apply PhoneFrameDefaults after loading root fields if legacy defaults < 2
        if defaults_ver < DEFAULTS_VERSION:
            self.fps_cap = 0
            self.vsync = False
            migrated = True

        # Migration logic: HDR -> tone_mapping, bloom false -> bloom_intensity 0
        if self.hdr and "tone_mapping" not in data.get("graphics", {}):
            self.graphics["tone_mapping"] = 1
        if not self.bloom and "bloom_intensity" not in data.get("graphics", {}):
            self.graphics["bloom_intensity"] = 0
        if prev_version < 3:
            self.audio["mod_audio_pack_enabled"] = True

        # Cross-validation
        if self.dynamic_min_percent > self.dynamic_max_percent:
            self.dynamic_min_percent = self.dynamic_max_percent
            sanitized_count += 1

        return {
            "parsed_ok": True,
            "migrated": migrated,
            "previous_schema_version": prev_version,
            "sanitized_count": sanitized_count
        }

    def serialize_to_dict(self) -> Dict[str, Any]:
        out = dict(self.unknown_nodes)
        out["schema_version"] = SCHEMA_VERSION
        out["defaults"] = DEFAULTS_VERSION
        out["display_mode"] = self.display_mode
        out["width"] = self.width
        out["height"] = self.height
        out["render_percent"] = self.render_percent
        out["fps_cap"] = self.fps_cap
        out["vsync"] = self.vsync
        out["anisotropic_filtering"] = self.anisotropic_filtering
        out["join_ip"] = self.join_ip

        out["skip_intros"] = self.skip_intros
        out["fps_counter"] = self.fps_counter
        out["fmv_scale"] = self.fmv_scale
        out["screen_filter"] = self.screen_filter
        out["reload_blur"] = self.reload_blur
        out["bloom"] = self.bloom
        out["fog"] = self.fog
        out["anti_aliasing"] = self.anti_aliasing
        out["hdr"] = self.hdr
        out["draw_distance"] = self.draw_distance
        out["dynamic_resolution"] = self.dynamic_resolution
        out["dynamic_target_fps"] = self.dynamic_target_fps
        out["dynamic_min_percent"] = self.dynamic_min_percent
        out["dynamic_max_percent"] = self.dynamic_max_percent
        out["dynamic_hold"] = self.dynamic_hold
        out["audio_output"] = self.audio_output
        out["language"] = self.language

        out["fov"] = self.fov
        out["viewmodel_fov"] = self.viewmodel_fov
        out["walk_speed"] = self.walk_speed
        out["hold_aim"] = self.hold_aim
        out["invert_mouse"] = self.invert_mouse
        out["mouse_sensitivity"] = self.mouse_sensitivity
        out["touch_sensitivity"] = self.touch_sensitivity
        out["left_stick_deadzone"] = self.left_stick_deadzone
        out["right_stick_deadzone"] = self.right_stick_deadzone
        out["left_stick_sensitivity"] = self.left_stick_sensitivity
        out["right_stick_sensitivity"] = self.right_stick_sensitivity

        out["infinite_ammo"] = self.infinite_ammo
        out["rapid_fire"] = self.rapid_fire
        out["no_recoil"] = self.no_recoil
        out["game_speed"] = self.game_speed
        out["god_mode"] = self.god_mode
        out["one_hit_kill"] = self.one_hit_kill
        out["infinite_grenades"] = self.infinite_grenades
        out["unlock_levels"] = self.unlock_levels
        out["unlock_black_ops"] = self.unlock_black_ops
        out["esp"] = self.esp
        out["aimbot"] = self.aimbot
        out["keys"] = dict(self.keys)

        out["graphics"] = dict(self.graphics)
        out["combat"] = dict(self.combat)
        out["characters"] = dict(self.characters)
        out["audio"] = dict(self.audio)
        out["arabic"] = dict(self.arabic)
        out["gameplay"] = dict(self.gameplay)
        out["interface"] = dict(self.interface)
        out["packages"] = dict(self.packages)
        out["presets"] = dict(self.presets)

        return out

    def to_toml_string(self) -> str:
        d = self.serialize_to_dict()
        lines = []

        def format_val(v):
            if isinstance(v, bool):
                return "true" if v else "false"
            if isinstance(v, str):
                escaped = v.replace('\\', '\\\\').replace('"', '\\"')
                return f'"{escaped}"'
            if isinstance(v, float):
                return f"{v:.4f}".rstrip('0').rstrip('.') if not math.isnan(v) else "0.0"
            return str(v)

        for k, v in d.items():
            if not isinstance(v, dict):
                lines.append(f"{k} = {format_val(v)}")
        lines.append("")

        for section in ["keys", "presets", "graphics", "combat", "characters", "audio", "arabic", "gameplay", "interface", "packages"]:
            if section in d and isinstance(d[section], dict):
                lines.append(f"[{section}]")
                sub = d[section]
                for sk, sv in sub.items():
                    if isinstance(sv, dict):
                        lines.append(f"[{section}.{sk}]")
                        for ssk, ssv in sv.items():
                            lines.append(f"{ssk} = {format_val(ssv)}")
                    else:
                        lines.append(f"{sk} = {format_val(sv)}")
                lines.append("")

        # Any extra tables
        for k, v in d.items():
            if isinstance(v, dict) and k not in ["keys", "presets", "graphics", "combat", "characters", "audio", "arabic", "gameplay", "interface", "packages"]:
                lines.append(f"[{k}]")
                for sk, sv in v.items():
                    lines.append(f"{sk} = {format_val(sv)}")
                lines.append("")

        return "\n".join(lines)


class PresetStateMachineModel:
    def __init__(self, reg: SettingsRegistryModel):
        self.reg = reg
        self.active_kind = "custom"
        self.custom_snapshot: Dict[str, Any] = {"saved": False}

    def capture_custom_snapshot(self):
        self.custom_snapshot = {
            "saved": True,
            "render_percent": self.reg.render_percent,
            "fps_cap": self.reg.fps_cap,
            "vsync": self.reg.vsync,
            "anisotropic_filtering": self.reg.anisotropic_filtering,
            "anti_aliasing": self.reg.anti_aliasing,
            "screen_filter": self.reg.screen_filter,
            "bloom": self.reg.bloom,
            "fog": self.reg.fog,
            "hdr": self.reg.hdr,
            "reload_blur": self.reg.reload_blur,
            "draw_distance": self.reg.draw_distance,
            "dynamic_resolution": self.reg.dynamic_resolution,
            "dynamic_target_fps": self.reg.dynamic_target_fps,
            "dynamic_min_percent": self.reg.dynamic_min_percent,
            "dynamic_max_percent": self.reg.dynamic_max_percent,
            "dynamic_hold": self.reg.dynamic_hold,
            "fps_counter": self.reg.fps_counter,
        }
        self.reg.presets["last_custom"] = dict(self.custom_snapshot)

    def restore_custom_snapshot(self):
        if not self.custom_snapshot.get("saved"):
            return
        for k in PRESET_MANAGED_KEYS:
            if k in self.custom_snapshot and hasattr(self.reg, k):
                setattr(self.reg, k, self.custom_snapshot[k])

    def apply_preset(self, target: str) -> bool:
        if self.active_kind == "custom" and target != "custom":
            self.capture_custom_snapshot()

        if target == "original":
            self.reg.render_percent = 67
            self.reg.fps_cap = 0
            self.reg.vsync = False
            self.reg.anisotropic_filtering = 0
            self.reg.anti_aliasing = 0
            self.reg.screen_filter = 0
            self.reg.bloom = True
            self.reg.fog = True
            self.reg.hdr = False
            self.reg.reload_blur = True
            self.reg.draw_distance = 1
            self.reg.dynamic_resolution = False
            self.reg.fps_counter = False
            self.active_kind = "original"
            self.reg.presets["active"] = "original"
            return True

        elif target == "enhanced":
            self.reg.render_percent = 100
            self.reg.fps_cap = 0
            self.reg.vsync = False
            self.reg.anisotropic_filtering = 16
            self.reg.anti_aliasing = 1
            self.reg.screen_filter = 0
            self.reg.bloom = True
            self.reg.fog = True
            self.reg.reload_blur = True
            self.reg.draw_distance = 2
            self.reg.dynamic_resolution = True
            self.reg.dynamic_max_percent = 100
            self.reg.dynamic_min_percent = 67
            self.reg.dynamic_target_fps = 60
            self.reg.dynamic_hold = 5
            self.active_kind = "enhanced"
            self.reg.presets["active"] = "enhanced"
            return True

        elif target == "performance":
            self.reg.render_percent = 67
            self.reg.fps_cap = 0
            self.reg.vsync = False
            self.reg.anisotropic_filtering = 4
            self.reg.anti_aliasing = 0
            self.reg.screen_filter = 0
            self.reg.bloom = False
            self.reg.fog = True
            self.reg.hdr = False
            self.reg.reload_blur = False
            self.reg.draw_distance = 1
            self.reg.dynamic_resolution = True
            self.reg.dynamic_min_percent = 45
            self.reg.dynamic_max_percent = 80
            self.reg.dynamic_target_fps = 60
            self.reg.dynamic_hold = 3
            self.active_kind = "performance"
            self.reg.presets["active"] = "performance"
            return True

        elif target == "custom":
            if self.custom_snapshot.get("saved"):
                self.restore_custom_snapshot()
            self.active_kind = "custom"
            self.reg.presets["active"] = "custom"
            return True

        return False

    def notify_setting_changed(self, key: str):
        if key in PRESET_MANAGED_KEYS:
            self.active_kind = "custom"
            self.reg.presets["active"] = "custom"
            self.capture_custom_snapshot()


# ==============================================================================
# TEST CASES
# ==============================================================================

def test_1_default_settings():
    print("[RUN ] Test 1: Default Settings")
    reg = SettingsRegistryModel()
    assert reg.render_percent == 67, f"render_percent {reg.render_percent}"
    assert reg.anisotropic_filtering == 0
    assert reg.anti_aliasing == 0
    assert reg.draw_distance == 1
    assert reg.fov == 70
    assert reg.language == "ara"
    assert reg.bloom is True
    assert reg.dynamic_resolution is False
    assert reg.hdr is False
    assert reg.god_mode is False
    assert reg.infinite_ammo is False
    # Anti-cosmetic check: setting locked feature must be rejected
    ok, reason = reg.set_value("overlay_enabled", True, "packages")
    assert not ok, "Locked feature must not be accepted"
    assert "Planned" in reason
    assert reg.packages["overlay_enabled"] is False
    print("[PASS] Test 1: Default Settings")
    return True


def test_2_settings_save_and_load():
    print("[RUN ] Test 2: Settings Save and Load")
    tmp_dir = tempfile.mkdtemp(prefix="black_test_save_")
    cfg_file = os.path.join(tmp_dir, "black.toml")
    try:
        reg1 = SettingsRegistryModel()
        presets1 = PresetStateMachineModel(reg1)
        reg1.render_percent = 90
        reg1.anisotropic_filtering = 16
        reg1.fov = 95
        reg1.bloom = False
        reg1.infinite_ammo = True
        reg1.keys["fire"] = "LeftMouse"
        presets1.apply_preset("custom")

        content = reg1.to_toml_string()
        with open(cfg_file, "w", encoding="utf-8") as f:
            f.write(content)

        with open(cfg_file, "rb") as f:
            parsed = tomllib.load(f)

        assert parsed["render_percent"] == 90
        assert parsed["anisotropic_filtering"] == 16
        assert parsed["fov"] == 95
        assert parsed["bloom"] is False
        assert parsed["infinite_ammo"] is True
        assert parsed["keys"]["fire"] == "LeftMouse"
        assert parsed["schema_version"] == 4

        reg2 = SettingsRegistryModel()
        reg2.load_from_dict(parsed)
        assert reg2.render_percent == 90
        assert reg2.anisotropic_filtering == 16
        assert reg2.fov == 95
        assert reg2.bloom is False
        assert reg2.infinite_ammo is True
        assert reg2.keys["fire"] == "LeftMouse"
    finally:
        shutil.rmtree(tmp_dir, ignore_errors=True)
    print("[PASS] Test 2: Settings Save and Load")
    return True


def test_3_invalid_values():
    print("[RUN ] Test 3: Invalid Values & Robustness")
    bad_toml = """
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
"""
    parsed = tomllib.loads(bad_toml)
    reg = SettingsRegistryModel()
    res = reg.load_from_dict(parsed)

    assert res["sanitized_count"] > 0
    assert reg.render_percent == 200, f"Got {reg.render_percent}"  # clamped to max 200
    assert reg.anisotropic_filtering in [0, 8, 16]               # snapped to nearest discrete valid
    assert reg.anti_aliasing == 0                                # clamped to 0
    assert reg.draw_distance in [1, 8]
    assert reg.dynamic_min_percent <= reg.dynamic_max_percent
    assert reg.fov == 45                                         # clamped to min 45
    assert abs(reg.mouse_sensitivity - 1.0) < 0.001
    assert reg.language == "ara"                                 # fallback for invalid language
    assert reg.bloom is True

    # Check NaN handling
    ok, _ = reg.set_value("mouse_sensitivity", float("nan"))
    assert not ok or math.isfinite(reg.mouse_sensitivity)
    assert math.isfinite(reg.mouse_sensitivity)
    print("[PASS] Test 3: Invalid Values & Robustness")
    return True


def test_4_missing_keys():
    print("[RUN ] Test 4: Missing Keys Partial Load")
    partial = """
defaults = 2
fov = 110
vsync = true
"""
    parsed = tomllib.loads(partial)
    reg = SettingsRegistryModel()
    reg.load_from_dict(parsed)
    assert reg.fov == 110
    assert reg.vsync is True
    # Missing keys retain safe defaults
    assert reg.render_percent == 67
    assert reg.walk_speed == 50
    assert reg.language == "ara"
    assert reg.hold_aim is True
    print("[PASS] Test 4: Missing Keys Partial Load")
    return True


def test_5_unknown_keys_preservation():
    print("[RUN ] Test 5: Unknown Keys Preservation")
    input_toml = """
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
"""
    parsed = tomllib.loads(input_toml)
    reg = SettingsRegistryModel()
    reg.load_from_dict(parsed)
    assert reg.render_percent == 80
    assert reg.unknown_nodes["custom_user_flag"] is True
    assert reg.unknown_nodes["future_port_option"] == "vulkan_async_compute"

    reg.fov = 88
    serialized = reg.to_toml_string()
    reparsed = tomllib.loads(serialized)

    assert reparsed["fov"] == 88
    assert reparsed["custom_user_flag"] is True
    assert reparsed["future_port_option"] == "vulkan_async_compute"
    assert reparsed["user_notes"]["profile_tag"] == "sd8gen2"
    assert reparsed["user_notes"]["benchmark_runs"] == 4
    print("[PASS] Test 5: Unknown Keys Preservation")
    return True


def test_6_preset_switching():
    print("[RUN ] Test 6: Preset Switching & Cheat/Control Isolation")
    reg = SettingsRegistryModel()
    presets = PresetStateMachineModel(reg)

    # User modifies cheats, controls, and language
    reg.infinite_ammo = True
    reg.god_mode = True
    reg.rapid_fire = 3
    reg.left_stick_deadzone = 12
    reg.right_stick_sensitivity = 2.4
    reg.mouse_sensitivity = 1.8
    reg.language = "fra"
    reg.keys["jump"] = "Space"

    # 1. Switch to Enhanced
    assert presets.apply_preset("enhanced")
    assert presets.active_kind == "enhanced"
    assert reg.anisotropic_filtering == 16
    assert reg.anti_aliasing == 1
    assert reg.draw_distance == 2
    assert reg.dynamic_resolution is True
    # Excluded cheats/controls/language untouched
    assert reg.infinite_ammo is True
    assert reg.god_mode is True
    assert reg.rapid_fire == 3
    assert reg.left_stick_deadzone == 12
    assert abs(reg.right_stick_sensitivity - 2.4) < 0.001
    assert abs(reg.mouse_sensitivity - 1.8) < 0.001
    assert reg.language == "fra"
    assert reg.keys["jump"] == "Space"

    # 2. Switch to Performance
    assert presets.apply_preset("performance")
    assert presets.active_kind == "performance"
    assert reg.anti_aliasing == 0
    assert reg.draw_distance == 1
    assert reg.bloom is False
    assert reg.reload_blur is False
    assert reg.anisotropic_filtering == 4
    assert reg.dynamic_resolution is True
    # Excluded untouched
    assert reg.infinite_ammo is True
    assert reg.left_stick_deadzone == 12

    # 3. Switch to Original
    assert presets.apply_preset("original")
    assert presets.active_kind == "original"
    assert reg.render_percent == 67
    assert reg.anisotropic_filtering == 0
    assert reg.anti_aliasing == 0
    assert reg.draw_distance == 1
    assert reg.bloom is True
    assert reg.fog is True
    assert reg.hdr is False
    # Excluded untouched
    assert reg.infinite_ammo is True
    assert reg.keys["jump"] == "Space"

    # 4. Manual edit of preset-managed option transitions to Custom
    reg.draw_distance = 4
    presets.notify_setting_changed("draw_distance")
    assert presets.active_kind == "custom"

    # 5. Manual edit of cheat does NOT change preset
    assert presets.apply_preset("performance")
    reg.god_mode = False
    presets.notify_setting_changed("god_mode")
    assert presets.active_kind == "performance"

    print("[PASS] Test 6: Preset Switching & Cheat/Control Isolation")
    return True


def test_7_custom_snapshot_recovery():
    print("[RUN ] Test 7: Custom Snapshot Recovery")
    reg = SettingsRegistryModel()
    presets = PresetStateMachineModel(reg)

    reg.render_percent = 135
    presets.notify_setting_changed("render_percent")
    reg.anisotropic_filtering = 8
    presets.notify_setting_changed("anisotropic_filtering")
    reg.draw_distance = 4
    presets.notify_setting_changed("draw_distance")
    assert presets.active_kind == "custom"

    # Switch away to Performance
    assert presets.apply_preset("performance")
    assert presets.custom_snapshot["saved"] is True
    assert reg.render_percent != 135

    # Serialize and reload
    saved_dict = reg.serialize_to_dict()
    reg2 = SettingsRegistryModel()
    reg2.load_from_dict(saved_dict)
    presets2 = PresetStateMachineModel(reg2)
    presets2.active_kind = reg2.presets.get("active", "custom")
    presets2.custom_snapshot = dict(reg2.presets.get("last_custom", {}))

    assert presets2.active_kind == "performance"
    assert presets2.custom_snapshot.get("saved") is True

    # Restore Custom
    assert presets2.apply_preset("custom")
    assert presets2.active_kind == "custom"
    assert reg2.render_percent == 135
    assert reg2.anisotropic_filtering == 8
    assert reg2.draw_distance == 4

    print("[PASS] Test 7: Custom Snapshot Recovery")
    return True


def test_8_restart_required_settings():
    print("[RUN ] Test 8: Restart Required Settings & Boot Snapshot")
    reg = SettingsRegistryModel()
    reg.capture_boot_snapshot()

    assert not reg.requires_app_restart()
    assert reg.pending_restart_count() == 0

    # Changing FOV (Immediate) does not require restart
    reg.set_value("fov", 100)
    assert not reg.requires_app_restart()

    # Changing Language (AppRestart) marks restart pending
    reg.set_value("language", "fra")
    assert reg.requires_app_restart()
    assert reg.pending_restart_count() == 1
    assert reg.boot_language == "ara"

    # Reverting back to boot value clears pending restart
    reg.set_value("language", "ara")
    assert not reg.requires_app_restart()
    assert reg.pending_restart_count() == 0

    print("[PASS] Test 8: Restart Required Settings & Boot Snapshot")
    return True


def test_9_schema_migration():
    print("[RUN ] Test 9: Schema Migration")
    legacy_toml = """
defaults = 1
display_mode = "fullscreen"
render_percent = 100
fps_cap = 60
vsync = true
hdr = true
bloom = false
fov = 85
"""
    parsed = tomllib.loads(legacy_toml)
    reg = SettingsRegistryModel()
    res = reg.load_from_dict(parsed)

    assert res["migrated"] is True
    assert res["previous_schema_version"] == 0

    # defaults < 2 forces phone frame defaults (fps_cap = 0, vsync = false)
    assert reg.fps_cap == 0
    assert reg.vsync is False
    assert reg.fov == 85
    # HDR legacy flag migrated into tone_mapping staging
    assert reg.hdr is True
    assert reg.graphics["tone_mapping"] == 1
    # bloom = false migrated into bloom_intensity = 0
    assert reg.bloom is False
    assert reg.graphics["bloom_intensity"] == 0

    serialized = reg.serialize_to_dict()
    assert serialized["schema_version"] == 4
    assert serialized["defaults"] == 2

    print("[PASS] Test 9: Schema Migration")
    return True


def test_10_physics_lab_and_enhanced_implemented():
    print("[RUN ] Test 10: Physics Lab & Enhanced Settings Unlocked")
    reg = SettingsRegistryModel()
    for sect, key, val in [
        ("graphics", "tone_mapping", 2),
        ("graphics", "bloom_intensity", 160),
        ("graphics", "trilinear_filtering", False),
        ("graphics", "weapon_reflections", False),
        ("combat", "screen_damage_vignette", False),
        ("combat", "damage_vignette_intensity", 95),
        ("combat", "low_health_desaturation", False),
        ("combat", "explosion_glow_boost", 150),
        ("characters", "ragdoll_60fps_physics", False),
        ("audio", "combat_dynamic_boost", True),
        ("audio", "master_gain_percent", 125),
        ("gameplay", "disable_aim_assist", True),
        ("interface", "advanced_perf_hud", True),
        ("interface", "hide_hud_on_screenshots", True),
    ]:
        ok, reason = reg.set_value(key, val, sect)
        assert ok, f"Expected {sect}.{key} unlocked, got: {reason}"
    assert reg.physics_lab["preset"] == "realistic"
    assert reg.physics_lab["explosion_force"] == 2.20
    for phy_key, val, expected in [
        ("explosion_force", 15.0, 10.0),
        ("impact_force", 9.0, 9.0),
        ("gravity_scale", 0.01, 0.05),
        ("debris_intensity", 5.5, 5.5),
        ("character_reaction", 7.0, 7.0),
        ("weapon_recoil", 3.5, 3.5),
        ("explosion_radius", 4.0, 4.0),
        ("ragdoll_launch", 8.5, 8.5),
        ("structural_destruction", 6.0, 6.0),
        ("prop_bounciness", 4.5, 4.5),
        ("camera_shockwave", 3.0, 3.0),
        ("physics_time_scale", 0.5, 0.5),
    ]:
        ok, reason = reg.set_value(phy_key, val, "physics_lab")
        assert ok, f"Expected physics_lab.{phy_key} unlocked, got: {reason}"
        assert abs(reg.physics_lab[phy_key] - expected) < 1e-6
    print("[PASS] Test 10: Physics Lab & Enhanced Settings Unlocked")
    return True


def test_11_time_stop_controls_and_arabic():
    print("[RUN ] Test 11: Time Stop Controls & Arabic Strings")
    reg = SettingsRegistryModel()
    for phy_key, val, expected in [
        ("time_stop_enabled", True, True),
        ("time_stop_mode", 1, 1),
        ("time_stop_max_duration", 12.0, 12.0),
        ("time_stop_cooldown", 5.0, 5.0),
    ]:
        ok, reason = reg.set_value(phy_key, val, "physics_lab")
        assert ok, f"Expected physics_lab.{phy_key} unlocked, got: {reason}"
        assert reg.physics_lab[phy_key] == expected

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    with open(os.path.join(repo_root, "src", "common", "translations.cpp"), "r", encoding="utf-8") as f:
        tr_src = f.read()
    for arabic_label in [
        "إيقاف الزمن",
        "تجميد العالم",
        "استئناف الزمن",
        "مدة التجميد",
    ]:
        assert arabic_label in tr_src, f"Missing required Arabic label in translations.cpp: {arabic_label}"

    with open(os.path.join(repo_root, "src", "common", "mod_schema.cpp"), "r", encoding="utf-8") as f:
        schema_src = f.read()
    for key_name in [
        "time_stop_enabled",
        "time_stop_mode",
        "time_stop_max_duration",
        "time_stop_cooldown",
    ]:
        assert key_name in schema_src, f"Missing schema registration in mod_schema.cpp: {key_name}"

    print("[PASS] Test 11: Time Stop Controls & Arabic Strings")
    return True


def test_12_weapon_audio_remaster_schema():
    print("[RUN ] Test 12: Weapon Audio Remaster Schema & Controls")
    reg = SettingsRegistryModel()
    assert reg.audio["mod_audio_pack_enabled"] is True, "Enhanced weapon audio should default to enabled"
    for key, val, expected in [
        ("mod_audio_pack_enabled", False, False),
        ("weapon_audio_volume", 140, 140),
        ("mechanical_audio_volume", 200, 150),
        ("reload_audio_volume", 85, 85),
        ("environmental_audio_volume", 120, 120),
        ("audio_tail_intensity", 135, 135),
        ("audio_echo_intensity", 200, 150),
        ("audio_distant_gunfire", 110, 110),
        ("audio_bullet_flyby", 95, 95),
        ("audio_variation_percent", 150, 100),
    ]:
        ok, reason = reg.set_value(key, val, "audio")
        assert ok, f"Expected audio.{key} unlocked, got: {reason}"
        assert reg.audio[key] == expected, f"Expected audio.{key} == {expected}, got {reg.audio[key]}"

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    with open(os.path.join(repo_root, "src", "common", "mod_schema.cpp"), "r", encoding="utf-8") as f:
        schema_src = f.read()
    for key_name in [
        "mod_audio_pack_enabled",
        "weapon_audio_volume",
        "mechanical_audio_volume",
        "reload_audio_volume",
        "environmental_audio_volume",
        "audio_tail_intensity",
        "audio_echo_intensity",
        "audio_distant_gunfire",
        "audio_bullet_flyby",
        "audio_variation_percent",
    ]:
        assert key_name in schema_src, f"Missing schema registration in mod_schema.cpp: {key_name}"

    print("[PASS] Test 12: Weapon Audio Remaster Schema & Controls")
    return True


def test_13_ballistics_tracers_and_arabic():
    print("[RUN ] Test 13: Ballistics Tracers Schema & Arabic Menu Coverage")
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    with open(os.path.join(repo_root, "src", "common", "mod_schema.cpp"), "r", encoding="utf-8") as f:
        schema_src = f.read()
    for key_name in [
        "ballistics_preset",
        "tracers_enabled",
        "tracer_frequency",
        "tracer_trail_length",
        "tracer_brightness",
        "tracer_color",
        "smoke_trails_enabled",
        "smoke_mode",
        "smoke_intensity",
        "smoke_duration",
        "weapon_reflections",
    ]:
        assert key_name in schema_src, f"Missing ballistics schema key in mod_schema.cpp: {key_name}"

    with open(os.path.join(repo_root, "src", "common", "translations.cpp"), "r", encoding="utf-8") as f:
        tr_src = f.read()
    for ar_label in [
        "BALLISTICS & TRACERS",
        "BULLET TRACERS",
        "TRACER FREQUENCY",
        "EVERY BULLET",
        "EVERY 2ND BULLET",
        "EVERY 3RD BULLET",
        "EVERY 5TH BULLET",
        "SELECTIVE",
        "SMOKE TRAILS",
        "SMOKE MODE",
        "SMOKE INTENSITY",
        "SMOKE DURATION",
        "RESTORE ORIGINAL BULLET EFFECTS",
    ]:
        assert ar_label in tr_src, f"Missing Arabic translation entry in translations.cpp: {ar_label}"

    print("[PASS] Test 13: Ballistics Tracers Schema & Arabic Menu Coverage")
    return True


def test_14_combat_juice_gyro_and_arabic():
    print("[RUN ] Test 14: Combat Feedback, Shell Casings, Enhanced Recoil, Gyro Filter & Arabic Coverage")
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    schema_h = open(os.path.join(root, "src", "common", "mod_schema.h"), encoding="utf-8").read()
    schema_cpp = open(os.path.join(root, "src", "common", "mod_schema.cpp"), encoding="utf-8").read()
    menu_cpp = open(os.path.join(root, "src", "common", "menu.cpp"), encoding="utf-8").read()
    tr_cpp = open(os.path.join(root, "src", "common", "translations.cpp"), encoding="utf-8").read()
    gyro_cpp = open(os.path.join(root, "native", "gyro.cpp"), encoding="utf-8").read()

    for sym in ["CombatHitmarkers", "CombatShellCasings", "GameplayEnhancedRecoil"]:
        assert sym in schema_h and sym in schema_cpp, f"Missing schema symbol: {sym}"
    for key in ["hitmarker_feedback", "shell_casings", "enhanced_recoil_feedback"]:
        assert key in schema_cpp, f"Missing TOML key: {key}"
    for label in ["TACTICAL HITMARKERS", "3D SHELL CASINGS", "ENHANCED RECOIL CURVES"]:
        assert label in menu_cpp, f"Missing menu option: {label}"
        assert label in tr_cpp, f"Missing translation entry: {label}"
    assert "OneEuroGyroFilter" in gyro_cpp, "Gyro engine must wire OneEuroGyroFilter"

    print("[PASS] Test 14: Combat Feedback, Shell Casings, Enhanced Recoil, Gyro Filter & Arabic Coverage")
    return True


def test_15_overdrive_protocol_and_arabic():
    print("[RUN ] Test 15: Kinetic Dash, Tactical Gear, Top-Bar Removal & Crash Safety")
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    schema_h = open(os.path.join(root, "src", "common", "mod_schema.h"), encoding="utf-8").read()
    schema_cpp = open(os.path.join(root, "src", "common", "mod_schema.cpp"), encoding="utf-8").read()
    menu_cpp = open(os.path.join(root, "src", "common", "menu.cpp"), encoding="utf-8").read()
    tr_cpp = open(os.path.join(root, "src", "common", "translations.cpp"), encoding="utf-8").read()
    vulkan_cpp = open(os.path.join(root, "native", "vulkan.cpp"), encoding="utf-8").read()
    touch_cpp = open(os.path.join(root, "native", "touch.cpp"), encoding="utf-8").read()
    overdrive_cpp = open(os.path.join(root, "src", "game", "overdrive.cpp"), encoding="utf-8").read()
    flame_cpp = open(os.path.join(root, "src", "game", "flamethrower.cpp"), encoding="utf-8").read()

    for sym in [
        "KineticDashEnabled",
        "SpecialAmmoMode",
        "ThermalVisorEnabled",
    ]:
        assert sym in schema_h and sym in schema_cpp, f"Missing Tactical SettingId: {sym}"

    for key in [
        "kinetic_dash_enabled",
        "special_ammo_mode",
        "thermal_visor_enabled",
    ]:
        assert key in schema_cpp, f"Missing Tactical TOML key: {key}"

    for removed_label in [
        "ADRENALINE OVERDRIVE",
        "TRIGGER OVERDRIVE SURGE",
        "DASH SHOCKWAVE SLAM",
        "KILL SIPHON (HP + AMMO)",
        "REFLEX MULTI-KILL SLOW-MO",
    ]:
        assert removed_label not in menu_cpp, f"Removed Adrenaline/Surge label still present in menu: {removed_label}"

    for label in [
        "KINETIC DASH & TACTICAL GEAR",
        "KINETIC DASH",
        "SPECIAL AMMO MODE",
        "THERMAL THREAT VISOR",
    ]:
        assert label in menu_cpp, f"Missing menu option: {label}"
        assert label in tr_cpp, f"Missing translation entry: {label}"

    for btn in ['"dash"', '"ammo_mode"', '"thermal"']:
        assert btn in touch_cpp, f"Missing touch HUD button: {btn}"
    assert '{"overdrive"' not in touch_cpp, "Overdrive counter button must be removed from Touch HUD"

    assert "ADR %.0f%%" not in overdrive_cpp and "OVERDRIVE %.1fs" not in overdrive_cpp, \
        "Top status bar strip must be removed from DrawHudOverlay"
    assert "FBits(4.2f)" not in overdrive_cpp and "FBits(4.5f)" not in flame_cpp, \
        "Unsafe float-as-int8 arg5 (FBits) passed to 0x001E74B0 must not exist"
    assert "thermal_mix" in vulkan_cpp or "ThermalVisor" in vulkan_cpp, "Thermal shader wiring missing in vulkan.cpp"
    print("[PASS] Test 15: Kinetic Dash, Tactical Gear, Top-Bar Removal & Crash Safety")
    return True


def test_16_full_mod_overhaul_and_unlocked_features():
    print("[RUN ] Test 16: Full Mod Overhaul, Unlocked Graphics/Gore, Audio DSP, HUD & Arabic")
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    schema_h = open(os.path.join(root, "src", "common", "mod_schema.h"), encoding="utf-8").read()
    schema_cpp = open(os.path.join(root, "src", "common", "mod_schema.cpp"), encoding="utf-8").read()
    menu_cpp = open(os.path.join(root, "src", "common", "menu.cpp"), encoding="utf-8").read()
    tr_cpp = open(os.path.join(root, "src", "common", "translations.cpp"), encoding="utf-8").read()
    vulkan_cpp = open(os.path.join(root, "native", "vulkan.cpp"), encoding="utf-8").read()
    gyro_cpp = open(os.path.join(root, "native", "gyro.cpp"), encoding="utf-8").read()
    touch_cpp = open(os.path.join(root, "native", "touch.cpp"), encoding="utf-8").read()
    flash_cpp = open(os.path.join(root, "src", "game", "flash_arabic.cpp"), encoding="utf-8").read()

    for sym in [
        "GraphicsSoftShadows",
        "GraphicsAmbientOcclusion",
        "CombatMaxBloodDecals",
        "CombatCorpsePersistenceMode",
        "CombatDirectionalDamage",
        "CombatHitstopEnabled",
        "CombatScorchAndHeatEnabled",
        "AudioTinnitusEnabled",
        "AudioOcclusionEnabled",
        "ArabicFlashMenus",
        "GameplayAiSuppressionEnabled",
        "GameplayKillCamEnabled",
        "GameplayActiveReloadEnabled",
        "ControlsTouchHudPreset",
    ]:
        assert sym in schema_h and sym in schema_cpp and sym in menu_cpp, f"Unwired SettingId: {sym}"

    for label in [
        "SOFT SHADOWS (PCF)",
        "AMBIENT OCCLUSION (SSAO)",
        "MAX BLOOD & SCORCH DECALS",
        "CORPSE PERSISTENCE",
        "DIRECTIONAL DAMAGE & KICK",
        "TACTICAL HITSTOP",
        "BLAST SCORCH & HEAT SHIMMER",
        "AI SUPPRESSION & GRENADE SCATTER",
        "CRITICAL / SNIPER KILL-CAM",
        "ACTIVE RELOAD & INSPECT",
        "TINNITUS & CONCUSSION",
        "ACOUSTIC OCCLUSION & CLINKS",
        "ARABIC FLASH MENUS",
        "TOUCH HUD PRESET",
        "ADS ONLY",
    ]:
        assert label in menu_cpp, f"Missing menu row: {label}"
        assert label in tr_cpp, f"Missing Arabic translation: {label}"

    assert "concussion_daze" in vulkan_cpp and "ao_strength" in vulkan_cpp
    assert "enh.soft_shadows > 0" in vulkan_cpp
    assert "ShouldRunForMode" in gyro_cpp
    assert "touch_hud_preset" in touch_cpp
    assert "arabic_flash_menus" in flash_cpp
    print("[PASS] Test 16: Full Mod Overhaul, Unlocked Graphics/Gore, Audio DSP, HUD & Arabic")
    return True


def main():
    tests = [
        test_1_default_settings,
        test_2_settings_save_and_load,
        test_3_invalid_values,
        test_4_missing_keys,
        test_5_unknown_keys_preservation,
        test_6_preset_switching,
        test_7_custom_snapshot_recovery,
        test_8_restart_required_settings,
        test_9_schema_migration,
        test_10_physics_lab_and_enhanced_implemented,
        test_11_time_stop_controls_and_arabic,
        test_12_weapon_audio_remaster_schema,
        test_13_ballistics_tracers_and_arabic,
        test_14_combat_juice_gyro_and_arabic,
        test_15_overdrive_protocol_and_arabic,
        test_16_full_mod_overhaul_and_unlocked_features,
    ]
    passed = 0
    failed = 0
    for t in tests:
        try:
            if t():
                passed += 1
        except Exception as e:
            print(f"[FAIL] {t.__name__}: {e}")
            failed += 1

    print(f"\nResults: {passed} passed, {failed} failed out of {len(tests)} tests.")
    if failed != 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
