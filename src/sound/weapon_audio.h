#pragma once
// BLACK Enhanced Edition — Ultra Realistic 48 kHz True-Stereo HDR Weapon & World Audio Remaster Engine.
// Reads BWAR v3 (and v2 fallback) asset pack (app/src/main/assets/weapon_audio/black_weapon_audio.pak),
// intercepts ISO wave signatures + sub_001EEFD0 gunfire triggers, and renders multi-layer
// 48 kHz Stereo/3D stems (Primary Report + Rolling Mattak Tail + Environmental Reflections + Distant
// Battlefield Gunfire + Supersonic Flyby Cracks + Foley/Impacts) through HDR ducking & -1 dBTP limiting.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sound::weapon_audio {

enum class StemCategory : uint8_t {
    Weapon = 0,
    Mechanical = 1,
    Reload = 2,
    Environment = 3,
    GunshotTail = 4,
    EnvironmentalEcho = 5,
    DistantGunfire = 6,
    BulletFlyby = 7,
};

struct Telemetry {
    bool pack_loaded = false;
    uint32_t pack_version = 0;
    uint32_t sample_rate = 48000;
    uint32_t slot_count = 0;
    uint32_t clip_count = 0;
    uint32_t stereo_clips = 0;
    uint32_t signature_count = 0;
    uint32_t active_voices = 0;
    uint64_t shots_enhanced = 0;
    uint64_t reloads_enhanced = 0;
    uint64_t mechanicals_enhanced = 0;
    uint64_t impacts_enhanced = 0;
    uint64_t tails_enhanced = 0;
    uint64_t distant_enhanced = 0;
    uint64_t flybys_enhanced = 0;
    uint64_t fp_loops_muted = 0;
    uint64_t synth_hit_confirms = 0;
    uint64_t synth_shell_clinks = 0;
    uint64_t synth_overdrive_cues = 0;
};

// Loads black_weapon_audio.pak (idempotent; safe to call on host or Android).
bool EnsureLoaded();
bool LoadPackFromFile(const char* path);
bool LoadPackFromMemory(const void* data, size_t bytes);

// Canonical FNV-1a 64-bit wave payload fingerprint (matches tools/build_weapon_audio_pack.py).
uint64_t Fnv1a64(const void* data, size_t bytes);

// Looks up a wave signature; returns slot_id >= 0 on match, -1 otherwise.
int32_t FindSignature(uint64_t fnv64, uint32_t orig_bytes, uint16_t* out_flags = nullptr);

// Called on sub_001EEFD0 player shot completion (immediate zero-latency layered gunfire trigger).
void OnPlayerFire(std::string_view weapon_code, bool suppressed, bool alt_fire = false);

// Called from buffers.cpp Play(). Caches signature match in sig_slot/sig_flags; returns true
// when Enhanced audio handles this sound and the original 16 kHz buffer output should be muted.
bool OnVoicePlay(uint32_t voice_obj, const void* raw_data, uint32_t bytes,
                 int32_t& sig_slot, uint16_t& sig_flags,
                 bool is3d, uint32_t mode, const float pos[3],
                 float min_dist, float max_dist, float volume);

// Called from buffers.cpp Stop() so abrupt burst cutoffs / silencer stops stay crisp
// while natural outdoor gunshot tails & room reflections ring out untruncated.
void OnVoiceStop(uint32_t voice_obj);

// Stops all currently ringing remaster voices (e.g. instant switch to Original mode).
void StopAll();

// Updates live room reverb telemetry from mixer.cpp so indoor/outdoor reflections adapt dynamically.
void SetEnvironmentState(bool reverb_active, float reverb_late);

// Procedural crisp 48 kHz stereo hit-confirm and bouncing shell-clink audio cues.
void TriggerHitConfirm(bool headshot, bool lethal);
void TriggerShellClink(float pan_right_ratio, bool is_shotgun_hull);

// Procedural Overdrive Protocol combat synth cues (Kinetic Dash, Shockwave Slam, Surge, Special Ammo cycle)
void TriggerKineticDash(bool with_shockwave = false);
void TriggerOverdriveCue(bool activating);
void TriggerSpecialAmmoCue(uint8_t ammo_mode);

// Renders active 48 kHz polyphonic layered weapon/tail/echo/distant/flyby stems into `out` and `send`
// before g_reverb.Run() executes in mixer.cpp. Safe on the audio thread.
void RenderMix(float* out, float* send, uint32_t frames,
               uint32_t out_channels, uint32_t fl_ch, uint32_t fr_ch,
               bool surround, uint32_t lfe_ch, uint32_t bl_ch, uint32_t br_ch,
               const float listener_pos[3], const float listener_front[3], const float listener_top[3]);

Telemetry GetTelemetry();

}  // namespace sound::weapon_audio
