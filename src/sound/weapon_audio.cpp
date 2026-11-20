#include "sound/weapon_audio.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "common/mod_schema.h"

#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <android/log.h>
#define WA_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BLACK_AUDIO", __VA_ARGS__)
#else
#define WA_LOGI(...) ((void)0)
#endif

namespace sound::weapon_audio {
namespace {

constexpr uint32_t kPackMagic = 0x52415742u;  // "BWAR" little-endian
constexpr uint32_t kVersionV2 = 2;
constexpr uint32_t kVersionV3 = 3;
constexpr uint32_t kExpectedRate = 48000;
constexpr uint32_t kMaxSlots = 192;
constexpr size_t kMaxActiveVoices = 80;
constexpr float kInv32768 = 1.0f / 32768.0f;

// Canonical Slot IDs matching tools/build_weapon_audio_pack.py (BWAR v3)
constexpr int32_t kSlotPstFire = 0, kSlotGk1Fire = 1, kSlotMgnFire = 2, kSlotSmgFire = 3;
constexpr int32_t kSlotSm3Fire = 4, kSlotMp1Fire = 5, kSlotP90Fire = 6, kSlotAk1Fire = 7;
constexpr int32_t kSlotAsrFire = 8, kSlotM16Fire = 9, kSlotBnsFire = 10, kSlotShgFire = 11;
constexpr int32_t kSlotRm1Fire = 12, kSlotSnrFire = 13, kSlotHvyFire = 14, kSlotRpgFire = 15;
constexpr int32_t kSlotGrlFire = 16;
constexpr int32_t kSlotPstSupp = 17, kSlotGk1Supp = 18, kSlotSmgSupp = 19, kSlotSm3Supp = 20;
constexpr int32_t kSlotMp1Supp = 21, kSlotP90Supp = 22, kSlotAk1Supp = 23, kSlotAsrSupp = 24;
constexpr int32_t kSlotM16Supp = 25, kSlotSnrSupp = 26;
constexpr int32_t kSlotAiTail = 79, kSlotEnvTail = 84, kSlotEnvFlyby = 85, kSlotEnvRpgPass = 86;
constexpr int32_t kSlotImpConcrete = 87, kSlotImpRicochet = 91, kSlotImpFlesh = 93, kSlotExplosion = 94;
constexpr int32_t kSlotBnsM203 = 95, kSlotSilencerCase = 96;
constexpr int32_t kSlotTailPistolOutdoor = 97, kSlotTailSmgOutdoor = 98, kSlotTailRifleMattak = 99;
constexpr int32_t kSlotTailHeavyMattak = 100, kSlotTailIndoorEcho = 101, kSlotDistantBattlefield = 102;
constexpr int32_t kSlotEnvSupersonicCrack = 103, kSlotTailShotgunOutdoor = 104, kSlotTailSuppressedCrack = 105;
// Expanded Non-Weapon & Stereo Foley Slots (BWAR v3, 106..128)
constexpr int32_t kSlotFsConcreteWalk = 106, kSlotFsConcreteRun = 107, kSlotFsGrassWalk = 108;
constexpr int32_t kSlotFsDirtGravel = 109, kSlotFsMetal = 110, kSlotFsWaterPuddle = 111;
constexpr int32_t kSlotLandImpact = 112, kSlotBodyfall = 113, kSlotMeleeSwing = 114, kSlotMeleeHit = 115;
constexpr int32_t kSlotWaterSplash = 116, kSlotFireLoop = 117;
constexpr int32_t kSlotPickupAmmo = 118, kSlotPickupHealth = 119;
constexpr int32_t kSlotSniperBreath = 120, kSlotHeartbeatLowHlth = 121;
constexpr int32_t kSlotDebrisRubble = 122, kSlotCarWindshield = 123, kSlotWeaponDrop = 124;
constexpr int32_t kSlotUiHudConfirm = 125, kSlotUiHudHint = 126, kSlotUiHudAlert = 127;
constexpr int32_t kSlotFlameIgnite = 128;
// Special Arsenal CC0 Web Audio Extended Slots (129..151)
constexpr int32_t kSlotFlmIgniteSquib = 129, kSlotFlmValveShutoff = 130, kSlotFlmTankReload = 131;
constexpr int32_t kSlotM91Fire = 132, kSlotM91Bolt = 133, kSlotM91Clip = 134, kSlotM91Dry = 135;
constexpr int32_t kSlotC96Fire = 136, kSlotC96Breech = 137, kSlotC96Clip = 138, kSlotC96Dry = 139, kSlotC96LockOpen = 140;
constexpr int32_t kSlotMxmFire = 141, kSlotMxmReload = 142, kSlotMxmBoil = 143, kSlotMxmSteam = 144;
constexpr int32_t kSlotTailM91Outdoor = 145, kSlotTailMxmHeavy = 146;
constexpr int32_t kSlotFlmPilotHum = 147, kSlotFlmFoamSplash = 148, kSlotM91ChamberClose = 149;
constexpr int32_t kSlotMxmWheelRoll = 150, kSlotMxmBeltAdvance = 151, kSlotTailC96Outdoor = 152;
constexpr int32_t kSlotM91Equip = 153, kSlotM91Supp = 154;
constexpr int32_t kSlotCasingBrass = 72, kSlotCasingShell = 73;
constexpr int32_t kSlotCasingMetal = 155, kSlotCasingDirt = 156, kSlotCasingWood = 157;
constexpr int32_t kSlotWeaponInspect = 158, kSlotBassSubPunch = 159;

struct ClipRec {
    uint32_t slot_id = 0;
    uint32_t sample_offset = 0;
    uint32_t frame_count = 0;
    uint16_t channels = 1;
    uint16_t clip_flags = 100;
};

struct SigRec {
    uint64_t fnv64 = 0;
    uint32_t orig_size = 0;
    uint16_t slot_id = 0;
    uint16_t flags = 0;
};

struct ActiveVoice {
    bool active = false;
    uint32_t owner_voice_obj = 0;
    uint32_t slot_id = 0;
    uint32_t sample_offset = 0;
    uint32_t frame_count = 0;
    uint16_t channels = 1;
    uint16_t clip_flags = 100;
    uint32_t delay_samples = 0;
    double cursor = 0.0;
    double rate_ratio = 1.0;
    float gain = 1.0f;
    float stereo_width = 1.0f;
    float reverb_send = 0.16f;
    float lp_state_l = 0.0f;
    float lp_state_r = 0.0f;
    float side_lp_state = 0.0f;
    bool is_duckable_hdr = false;
    bool is_loop_voice = false;
    StemCategory stem = StemCategory::Weapon;
    bool is3d = false;
    uint32_t mode = 0;
    float pos[3] = {0.0f, 0.0f, 0.0f};
    float min_dist = 2.0f;
    float max_dist = 120.0f;
    bool has_forced_pan = false;
    float forced_pan = 0.0f;
};

enum class SynthKind : uint8_t {
    HitBody = 0,
    HitHead = 1,
    HitKill = 2,
    BrassClink = 3,
    PlasticHullClink = 4,
    KineticDashWhoosh = 5,
    ShockwaveBlast = 6,
    OverdriveSurge = 7,
    SpecialAmmoSwitch = 8
};

struct SynthVoice {
    bool active = false;
    SynthKind kind = SynthKind::HitBody;
    uint32_t sample_idx = 0;
    uint32_t length_samples = 0;
    uint32_t delay_samples = 0;
    float gain_l = 0.0f;
    float gain_r = 0.0f;
    float pitch_mul = 1.0f;
};

struct EngineState {
    std::mutex pack_mutex;
    std::atomic<bool> load_attempted{false};
    std::atomic<bool> loaded{false};
    uint32_t pack_version = 0;
    uint32_t sample_rate = kExpectedRate;
    uint32_t slot_count = 0;
    uint32_t stereo_clips = 0;
    std::vector<ClipRec> clips;
    std::vector<SigRec> sigs;
    std::vector<int16_t> pcm;
    std::array<std::vector<uint32_t>, kMaxSlots> slot_clips;
    std::array<uint32_t, kMaxSlots> last_clip_choice{};

    std::atomic<bool> env_reverb_active{false};
    std::atomic<float> env_reverb_late{0.0f};

    std::mutex mix_mutex;
    std::array<ActiveVoice, kMaxActiveVoices> pool{};
    std::array<SynthVoice, 16> synth_pool{};
    uint32_t rng_state = 0x51A7F00Du;
    uint64_t last_player_fire_ms = 0;
    uint64_t last_clink_ms = 0;
    float hdr_env = 0.0f;
    float limiter_gain = 1.0f;
    uint64_t last_fp_fallback_ms = 0;
    bool has_listener_pos = false;
    float listener_pos[3] = {0.0f, 0.0f, 0.0f};
    struct VoiceCooldown {
        uint32_t voice_obj = 0;
        uint64_t ms = 0;
    };
    std::array<VoiceCooldown, 24> cooldowns{};
    uint32_t cooldown_cursor = 0;

    std::atomic<uint64_t> shots_enhanced{0};
    std::atomic<uint64_t> reloads_enhanced{0};
    std::atomic<uint64_t> mechanicals_enhanced{0};
    std::atomic<uint64_t> impacts_enhanced{0};
    std::atomic<uint64_t> tails_enhanced{0};
    std::atomic<uint64_t> distant_enhanced{0};
    std::atomic<uint64_t> flybys_enhanced{0};
    std::atomic<uint64_t> fp_loops_muted{0};
    std::atomic<uint64_t> synth_hit_confirms{0};
    std::atomic<uint64_t> synth_shell_clinks{0};
    std::atomic<uint64_t> synth_overdrive_cues{0};

    // Concussion "Duck & Ring" tinnitus DSP state (mixer thread + atomic UI telemetry)
    std::atomic<float> concussion_daze{0.0f};
    float tinnitus_env = 0.0f;
    double tinnitus_phase_a = 0.0;
    double tinnitus_phase_b = 0.0;
    std::array<float, 6> post_lp_state{};
};

EngineState g_state;

uint32_t ReadU32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint64_t ReadU64(const uint8_t* p) {
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint16_t ReadU16(const uint8_t* p) {
    uint16_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint64_t NowMs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

float NextRandBipolar(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return (float(s >> 8) * (2.0f / 16777215.0f)) - 1.0f;
}

bool IsSuppressedFireSlot(uint32_t slot_id) {
    return (slot_id >= uint32_t(kSlotPstSupp) && slot_id <= uint32_t(kSlotSnrSupp));
}

bool IsAiGunfireSlot(uint32_t slot_id) {
    return (slot_id >= 74u && slot_id <= 83u && slot_id != uint32_t(kSlotAiTail));
}

bool IsUiSlot(uint32_t slot_id) {
    return slot_id >= uint32_t(kSlotUiHudConfirm) && slot_id <= uint32_t(kSlotUiHudAlert);
}

bool IsDuckableSlot(StemCategory stem, uint32_t slot_id) {
    return stem == StemCategory::GunshotTail ||
           stem == StemCategory::EnvironmentalEcho ||
           stem == StemCategory::DistantGunfire ||
           slot_id == uint32_t(kSlotFireLoop);
}

StemCategory StemForSlot(uint32_t slot_id) {
    if (slot_id <= uint32_t(kSlotGrlFire) || IsSuppressedFireSlot(slot_id) ||
        slot_id == uint32_t(kSlotBnsM203) || slot_id == uint32_t(kSlotFlameIgnite) ||
        slot_id == uint32_t(kSlotFlmIgniteSquib) || slot_id == uint32_t(kSlotM91Fire) ||
        slot_id == uint32_t(kSlotM91Supp) ||
        slot_id == uint32_t(kSlotC96Fire) || slot_id == uint32_t(kSlotMxmFire)) {
        return StemCategory::Weapon;
    }
    if (IsAiGunfireSlot(slot_id) || slot_id == uint32_t(kSlotDistantBattlefield)) {
        return StemCategory::DistantGunfire;
    }
    if (slot_id == uint32_t(kSlotAiTail) ||
        (slot_id >= uint32_t(kSlotTailPistolOutdoor) && slot_id <= uint32_t(kSlotTailHeavyMattak)) ||
        slot_id == uint32_t(kSlotTailShotgunOutdoor) || slot_id == uint32_t(kSlotTailSuppressedCrack) ||
        slot_id == uint32_t(kSlotTailM91Outdoor) || slot_id == uint32_t(kSlotTailMxmHeavy) ||
        slot_id == uint32_t(kSlotTailC96Outdoor)) {
        return StemCategory::GunshotTail;
    }
    if (slot_id == uint32_t(kSlotEnvTail) || slot_id == uint32_t(kSlotTailIndoorEcho)) {
        return StemCategory::EnvironmentalEcho;
    }
    if (slot_id == uint32_t(kSlotEnvFlyby) || slot_id == uint32_t(kSlotImpRicochet) ||
        slot_id == uint32_t(kSlotEnvSupersonicCrack)) {
        return StemCategory::BulletFlyby;
    }
    if (slot_id >= uint32_t(kSlotEnvRpgPass) && slot_id <= uint32_t(kSlotExplosion)) {
        return StemCategory::Environment;
    }
    if (slot_id == uint32_t(kSlotFlmTankReload) || slot_id == uint32_t(kSlotM91Clip) ||
        slot_id == uint32_t(kSlotC96Clip) || slot_id == uint32_t(kSlotMxmReload)) {
        return StemCategory::Reload;
    }
    if (slot_id == uint32_t(kSlotFlmValveShutoff) || slot_id == uint32_t(kSlotM91Bolt) ||
        slot_id == uint32_t(kSlotM91Dry) || slot_id == uint32_t(kSlotM91ChamberClose) ||
        slot_id == uint32_t(kSlotM91Equip) ||
        slot_id == uint32_t(kSlotC96Breech) || slot_id == uint32_t(kSlotC96Dry) ||
        slot_id == uint32_t(kSlotC96LockOpen) || slot_id == uint32_t(kSlotMxmBeltAdvance)) {
        return StemCategory::Mechanical;
    }
    if (slot_id == uint32_t(kSlotMxmBoil) || slot_id == uint32_t(kSlotMxmSteam) ||
        slot_id == uint32_t(kSlotFlmPilotHum) || slot_id == uint32_t(kSlotFlmFoamSplash) ||
        slot_id == uint32_t(kSlotMxmWheelRoll)) {
        return StemCategory::Environment;
    }
    // Expanded BWAR v3 non-weapon slots:
    if (slot_id == uint32_t(kSlotMeleeSwing) || slot_id == uint32_t(kSlotMeleeHit) ||
        slot_id == uint32_t(kSlotPickupAmmo) || slot_id == uint32_t(kSlotPickupHealth) ||
        slot_id == uint32_t(kSlotWeaponDrop) || slot_id == uint32_t(kSlotWeaponInspect) ||
        slot_id == uint32_t(kSlotCasingMetal) || slot_id == uint32_t(kSlotCasingDirt) ||
        slot_id == uint32_t(kSlotCasingWood)) {
        return StemCategory::Mechanical;
    }
    if (slot_id >= uint32_t(kSlotFsConcreteWalk) && slot_id <= uint32_t(kSlotUiHudAlert)) {
        return StemCategory::Environment;
    }
    // Mechanical vs Reload inside 27..73 & 96
    switch (slot_id) {
        case 27: case 28: case 30: case 31: case 33: case 34:
        case 35: case 36: case 38: case 39: case 41: case 42:
        case 44: case 45: case 47: case 48: case 50: case 51:
        case 54: case 56: case 57: case 59: case 60:
        case 62: case 63: case 64: case 65:
            return StemCategory::Reload;
        default:
            return StemCategory::Mechanical;
    }
}

void RecordStemTelemetry(StemCategory stem) {
    switch (stem) {
        case StemCategory::Weapon: g_state.shots_enhanced.fetch_add(1, std::memory_order_relaxed); break;
        case StemCategory::Reload: g_state.reloads_enhanced.fetch_add(1, std::memory_order_relaxed); break;
        case StemCategory::Mechanical: g_state.mechanicals_enhanced.fetch_add(1, std::memory_order_relaxed); break;
        case StemCategory::Environment: g_state.impacts_enhanced.fetch_add(1, std::memory_order_relaxed); break;
        case StemCategory::GunshotTail: g_state.tails_enhanced.fetch_add(1, std::memory_order_relaxed); break;
        case StemCategory::EnvironmentalEcho: g_state.impacts_enhanced.fetch_add(1, std::memory_order_relaxed); break;
        case StemCategory::DistantGunfire: g_state.distant_enhanced.fetch_add(1, std::memory_order_relaxed); break;
        case StemCategory::BulletFlyby: g_state.flybys_enhanced.fetch_add(1, std::memory_order_relaxed); break;
    }
}

std::string NormalizeWeaponCode(std::string_view code) {
    if (code.size() >= 4 && (code.substr(0, 4) == "BG1_" || code.substr(0, 4) == "bg1_")) {
        code.remove_prefix(4);
    }
    std::string up;
    up.reserve(code.size());
    for (char c : code) up.push_back(char(std::toupper(static_cast<unsigned char>(c))));
    if (up == "PS5") return "PST";
    if (up == "GK5") return "GK1";
    if (up == "MG5") return "MGN";
    if (up == "SM5") return "SMG";
    if (up == "MC5") return "SM3";
    if (up == "MP5") return "MP1";
    if (up == "P95") return "P90";
    if (up == "AK5") return "AK1";
    if (up == "AS5") return "ASR";
    if (up == "M45") return "M16";
    if (up == "SH5") return "SHG";
    if (up == "RM5") return "RM1";
    if (up == "SN5") return "SNR";
    if (up == "HV5") return "HVY";
    return up;
}

int32_t SlotForPlayerFire(std::string_view code_raw, bool suppressed, bool alt_fire) {
    std::string code = NormalizeWeaponCode(code_raw);
    if (code == "BNS" && alt_fire) return kSlotBnsM203;
    if (suppressed) {
        if (code == "PST") return kSlotPstSupp;
        if (code == "GK1") return kSlotGk1Supp;
        if (code == "SMG") return kSlotSmgSupp;
        if (code == "SM3") return kSlotSm3Supp;
        if (code == "MP1") return kSlotMp1Supp;
        if (code == "P90") return kSlotP90Supp;
        if (code == "AK1") return kSlotAk1Supp;
        if (code == "ASR") return kSlotAsrSupp;
        if (code == "M16" || code == "BNS") return kSlotM16Supp;
        if (code == "SNR") return kSlotSnrSupp;
    }
    if (code == "PST") return kSlotPstFire;
    if (code == "GK1") return kSlotGk1Fire;
    if (code == "MGN") return kSlotMgnFire;
    if (code == "SMG") return kSlotSmgFire;
    if (code == "SM3") return kSlotSm3Fire;
    if (code == "MP1") return kSlotMp1Fire;
    if (code == "P90") return kSlotP90Fire;
    if (code == "AK1") return kSlotAk1Fire;
    if (code == "ASR") return kSlotAsrFire;
    if (code == "M16") return kSlotM16Fire;
    if (code == "BNS") return kSlotBnsFire;
    if (code == "SHG") return kSlotShgFire;
    if (code == "RM1") return kSlotRm1Fire;
    if (code == "SNR") return kSlotSnrFire;
    if (code == "HVY") return kSlotHvyFire;
    if (code == "RPG") return kSlotRpgFire;
    if (code == "GRL") return kSlotGrlFire;
    if (code == "FLM" || code == "FLAME") return kSlotFlameIgnite;
    if (code == "M91") return kSlotM91Fire;
    if (code == "C96") return kSlotC96Fire;
    if (code == "MXM") return kSlotMxmFire;
    return -1;
}

uint32_t TailSlotForPlayerSlot(uint32_t fire_slot) {
    if (fire_slot == uint32_t(kSlotFlameIgnite)) return uint32_t(kSlotFireLoop);
    if (fire_slot == uint32_t(kSlotM91Fire)) return uint32_t(kSlotTailM91Outdoor);
    if (fire_slot == uint32_t(kSlotC96Fire)) return uint32_t(kSlotTailC96Outdoor);
    if (fire_slot == uint32_t(kSlotMxmFire)) return uint32_t(kSlotTailMxmHeavy);
    if (IsSuppressedFireSlot(fire_slot)) return uint32_t(kSlotTailSuppressedCrack);
    if (fire_slot <= uint32_t(kSlotGk1Fire)) return uint32_t(kSlotTailPistolOutdoor);
    if (fire_slot >= uint32_t(kSlotSmgFire) && fire_slot <= uint32_t(kSlotP90Fire)) return uint32_t(kSlotTailSmgOutdoor);
    if (fire_slot == uint32_t(kSlotShgFire) || fire_slot == uint32_t(kSlotRm1Fire)) return uint32_t(kSlotTailShotgunOutdoor);
    if (fire_slot == uint32_t(kSlotMgnFire) || fire_slot >= uint32_t(kSlotSnrFire)) return uint32_t(kSlotTailHeavyMattak);
    return uint32_t(kSlotTailRifleMattak);
}

bool SpawnClipForSlotLocked(uint32_t slot_id, uint32_t owner_voice_obj,
                            bool is3d, uint32_t mode, const float pos[3],
                            float min_dist, float max_dist, float volume,
                            uint32_t delay_samples = 0, float reverb_override = -1.0f,
                            bool is_loop_voice = false) {
    if (!g_state.loaded.load(std::memory_order_acquire) || slot_id >= g_state.slot_count || slot_id >= kMaxSlots) {
        return false;
    }
    const auto& choices = g_state.slot_clips[slot_id];
    if (choices.empty()) return false;

    uint32_t idx = 0;
    if (choices.size() > 1) {
        uint32_t prev = g_state.last_clip_choice[slot_id];
        uint32_t step = 1 + (g_state.rng_state % uint32_t(choices.size() - 1));
        g_state.rng_state = g_state.rng_state * 1664525u + 1013904223u;
        idx = (prev + step) % uint32_t(choices.size());
    }
    g_state.last_clip_choice[slot_id] = idx;
    const ClipRec& rec = g_state.clips[choices[idx]];
    if (rec.frame_count == 0) return false;

    // Pick free voice slot or evict furthest along (preferring older non-tail slots if busy)
    ActiveVoice* target = nullptr;
    double best_score = -1.0;
    for (auto& av : g_state.pool) {
        if (!av.active) {
            target = &av;
            break;
        }
        double prog = av.frame_count > 0 ? av.cursor / double(av.frame_count) : 1.0;
        double weight = (av.stem == StemCategory::GunshotTail) ? prog * 0.75 : prog;
        if (weight > best_score) {
            best_score = weight;
            target = &av;
        }
    }
    if (!target) return false;

    const auto& enh = modsettings::Registry().GetEnhanced();
    const float var_scale = std::clamp(float(enh.audio_variation_percent), 0.0f, 100.0f) * 0.01f;
    const float r_pitch = NextRandBipolar(g_state.rng_state);
    const float r_gain = NextRandBipolar(g_state.rng_state);
    const StemCategory stem = StemForSlot(slot_id);

    ActiveVoice nv;
    nv.active = true;
    nv.owner_voice_obj = owner_voice_obj;
    nv.slot_id = slot_id;
    nv.sample_offset = rec.sample_offset;
    nv.frame_count = rec.frame_count;
    nv.channels = rec.channels;
    nv.clip_flags = rec.clip_flags;
    nv.delay_samples = delay_samples;
    nv.cursor = 0.0;
    nv.rate_ratio = std::clamp(1.0 + double(r_pitch) * 0.028 * double(var_scale), 0.93, 1.07);
    nv.gain = std::clamp(volume, 0.0f, 1.6f) * (1.0f + r_gain * 0.05f * var_scale);
    const uint16_t w_pct = (rec.clip_flags >= 10u && rec.clip_flags <= 200u) ? rec.clip_flags : 100u;
    nv.stereo_width = (rec.channels == 2) ? (float(w_pct) * 0.01f) : 0.0f;
    nv.is_duckable_hdr = IsDuckableSlot(stem, slot_id);
    nv.is_loop_voice = is_loop_voice;
    if (reverb_override >= 0.0f) {
        nv.reverb_send = reverb_override;
    } else {
        switch (stem) {
            case StemCategory::EnvironmentalEcho: nv.reverb_send = 0.45f; break;
            case StemCategory::GunshotTail: nv.reverb_send = 0.32f; break;
            case StemCategory::DistantGunfire: nv.reverb_send = 0.36f; break;
            case StemCategory::Environment: nv.reverb_send = IsUiSlot(slot_id) ? 0.02f : 0.24f; break;
            case StemCategory::Weapon: nv.reverb_send = 0.20f; break;
            case StemCategory::BulletFlyby: nv.reverb_send = 0.14f; break;
            default: nv.reverb_send = 0.08f; break;
        }
    }
    nv.stem = stem;
    nv.is3d = is3d;
    nv.mode = mode;
    if (pos) {
        nv.pos[0] = pos[0];
        nv.pos[1] = pos[1];
        nv.pos[2] = pos[2];
    }
    nv.min_dist = std::max(min_dist, 1.0f);
    nv.max_dist = std::max(max_dist, nv.min_dist + 1.0f);
    *target = nv;

    RecordStemTelemetry(stem);
    return true;
}

void TriggerPlayerWeaponLayersLocked(uint32_t fire_slot, uint32_t owner_voice_obj) {
    const auto& enh = modsettings::Registry().GetEnhanced();
    const bool suppressed = IsSuppressedFireSlot(fire_slot);
    const bool is_flame = (fire_slot == uint32_t(kSlotFlameIgnite));

    // Layer A: Primary Close Report (direct punch & transient crack / burner burst)
    const float close_gain = suppressed ? 1.02f : 1.18f;
    SpawnClipForSlotLocked(fire_slot, owner_voice_obj, false, 0, nullptr, 2.0f, 120.0f,
                           close_gain, 0u, (suppressed || is_flame) ? 0.08f : 0.22f);

    if (is_flame) {
        // Real flame uses sustained gas-burner body rather than valley gunshot tail/indoor slam
        SpawnClipForSlotLocked(uint32_t(kSlotFireLoop), owner_voice_obj, false, 0, nullptr,
                               2.0f, 120.0f, 0.58f, 480u, 0.12f, true);
        return;
    }

    // Layer B: Rolling Outdoor Gunshot Tail ("Mattak" valley/open-area roll; owner=0 so never clipped short!)
    const bool rev_on = g_state.env_reverb_active.load(std::memory_order_relaxed);
    const float rev_late = g_state.env_reverb_late.load(std::memory_order_relaxed);
    const bool is_mauser91 = (fire_slot == uint32_t(kSlotM91Fire));
    const bool is_c96 = (fire_slot == uint32_t(kSlotC96Fire));
    if (enh.audio_tail_intensity > 0) {
        const uint32_t tail_slot = TailSlotForPlayerSlot(fire_slot);
        const uint32_t delay = suppressed ? 640u :
            (is_c96 ? 820u :
            ((fire_slot <= uint32_t(kSlotGk1Fire) || (fire_slot >= uint32_t(kSlotSmgFire) && fire_slot <= uint32_t(kSlotP90Fire))) ? 960u : 1200u));
        float tail_gain = suppressed ? 0.40f : 0.78f;
        if (is_mauser91) {
            tail_gain = rev_on ? 0.62f : 0.94f;
        } else if (is_c96) {
            tail_gain = rev_on ? 0.54f : 0.84f;
        }
        const float tail_rev = suppressed ? 0.12f : (is_mauser91 ? 0.40f : (is_c96 ? 0.36f : 0.34f));
        SpawnClipForSlotLocked(tail_slot, 0, false, 0, nullptr, 2.0f, 120.0f,
                               tail_gain, delay, tail_rev);
    }

    // Layer C: Authentic Indoor/Corridor Environmental Reflections (adapts dynamically to active I3DL2 room zone)
    if (enh.audio_echo_intensity > 0) {
        const float base_room = is_mauser91 ? 0.85f : (is_c96 ? 0.76f : 0.68f);
        const float open_room = is_mauser91 ? 0.36f : (is_c96 ? 0.40f : 0.44f);
        const float room_factor = rev_on
            ? std::clamp(base_room + rev_late * 1.45f, 0.68f, 1.45f)
            : open_room;
        const float echo_gain = (suppressed ? 0.28f : (is_mauser91 ? 0.82f : (is_c96 ? 0.76f : 0.72f))) * room_factor;
        SpawnClipForSlotLocked(uint32_t(kSlotTailIndoorEcho), 0, false, 0, nullptr, 2.0f, 120.0f,
                               echo_gain, 720u, is_mauser91 ? 0.48f : (is_c96 ? 0.46f : 0.44f));
    }
}

float Estimate3DDistance(bool is3d, uint32_t mode, const float pos[3]) {
    if (!is3d || mode == 2 || !pos) return 1.0f;
    if (mode == 1) {
        return std::sqrt(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]);
    }
    if (!g_state.has_listener_pos) return 18.0f;
    const float dx = pos[0] - g_state.listener_pos[0];
    const float dy = pos[1] - g_state.listener_pos[1];
    const float dz = pos[2] - g_state.listener_pos[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float Compute3DGainAndPan(const ActiveVoice& av, const float lp[3], const float lf[3], const float lt[3],
                          float& out_l, float& out_r, float& out_att) {
    if (!av.is3d || av.mode == 2 || !lp || !lf || !lt) {
        out_l = 0.7071f;
        out_r = 0.7071f;
        out_att = 1.0f;
        return 1.0f;
    }
    const bool rel = (av.mode == 1);
    float d[3] = {av.pos[0], av.pos[1], av.pos[2]};
    if (!rel) {
        d[0] -= lp[0];
        d[1] -= lp[1];
        d[2] -= lp[2];
    }
    const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float att = 1.0f;
    if (dist > av.min_dist) {
        // Inverse-distance rolloff; distant gunfire carries further across valleys before floor
        const float max_eff = (av.stem == StemCategory::DistantGunfire || av.stem == StemCategory::GunshotTail)
            ? std::max(av.max_dist, 240.0f)
            : av.max_dist;
        att = av.min_dist / std::min(dist, max_eff);
    }
    att = std::clamp(att, 0.025f, 1.0f);

    static constexpr float kHeadFront[3] = {0.0f, 0.0f, 1.0f};
    static constexpr float kHeadRight[3] = {1.0f, 0.0f, 0.0f};
    const float right[3] = {
        lt[1] * lf[2] - lt[2] * lf[1],
        lt[2] * lf[0] - lt[0] * lf[2],
        lt[0] * lf[1] - lt[1] * lf[0]
    };
    const float* front = rel ? kHeadFront : lf;
    const float* side = rel ? kHeadRight : right;
    const float f_dot = d[0] * front[0] + d[2] * front[2];
    const float s_dot = d[0] * side[0] + d[2] * side[2];
    const float horiz = std::sqrt(f_dot * f_dot + s_dot * s_dot);
    const float pan = (horiz > 1e-4f) ? std::clamp(s_dot / horiz, -1.0f, 1.0f) : 0.0f;
    out_l = std::sqrt(0.5f * (1.0f - pan * 0.85f));
    out_r = std::sqrt(0.5f * (1.0f + pan * 0.85f));
    out_att = att;
    return dist;
}

float StemVolumeGain(StemCategory stem, const modsettings::EnhancedOptions& enh) {
    uint32_t pct = 100;
    switch (stem) {
        case StemCategory::Weapon: pct = enh.weapon_audio_volume; break;
        case StemCategory::Mechanical: pct = enh.mechanical_audio_volume; break;
        case StemCategory::Reload: pct = enh.reload_audio_volume; break;
        case StemCategory::Environment: pct = enh.environmental_audio_volume; break;
        case StemCategory::GunshotTail: pct = enh.audio_tail_intensity; break;
        case StemCategory::EnvironmentalEcho: pct = enh.audio_echo_intensity; break;
        case StemCategory::DistantGunfire: pct = enh.audio_distant_gunfire; break;
        case StemCategory::BulletFlyby: pct = enh.audio_bullet_flyby; break;
    }
    return std::clamp(float(pct), 0.0f, 150.0f) * 0.01f;
}

float SoftClipCeiling(float x) {
    constexpr float kKnee = 0.88f;
    constexpr float kHead = 0.12f;
    if (x > kKnee) return kKnee + kHead * std::tanh((x - kKnee) / kHead);
    if (x < -kKnee) return -kKnee - kHead * std::tanh((-x - kKnee) / kHead);
    return x;
}

bool TryReadFileBytes(const char* path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize sz = f.tellg();
    if (sz < 24) return false;
    f.seekg(0, std::ios::beg);
    out.resize(size_t(sz));
    return bool(f.read(reinterpret_cast<char*>(out.data()), sz));
}

}  // namespace

uint64_t Fnv1a64(const void* data, size_t bytes) {
    if (!data || bytes == 0) return 0;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

bool LoadPackFromMemory(const void* data, size_t bytes) {
    if (!data || bytes < 24) return false;
    const uint8_t* raw = static_cast<const uint8_t*>(data);
    if (ReadU32(raw) != kPackMagic) return false;
    const uint32_t version = ReadU32(raw + 4);
    const uint32_t rate = ReadU32(raw + 8);
    const uint32_t slot_count = ReadU32(raw + 12);
    const uint32_t clip_count = ReadU32(raw + 16);
    const uint32_t sig_count = ReadU32(raw + 20);
    if ((version != kVersionV2 && version != kVersionV3) || rate != kExpectedRate ||
        slot_count == 0 || slot_count > kMaxSlots) {
        return false;
    }
    const size_t clip_stride = (version == kVersionV3) ? 16u : 12u;
    const size_t clip_bytes = size_t(clip_count) * clip_stride;
    const size_t sig_bytes = size_t(sig_count) * 16;
    const size_t header_and_tables = 24 + clip_bytes + sig_bytes;
    if (bytes <= header_and_tables || ((bytes - header_and_tables) % 2) != 0) return false;
    const size_t total_samples = (bytes - header_and_tables) / 2;

    std::vector<ClipRec> parsed_clips(clip_count);
    std::array<std::vector<uint32_t>, kMaxSlots> parsed_slot_clips{};
    uint32_t stereo_cnt = 0;
    const uint8_t* cp = raw + 24;
    for (uint32_t i = 0; i < clip_count; ++i, cp += clip_stride) {
        ClipRec c;
        c.slot_id = ReadU32(cp);
        c.sample_offset = ReadU32(cp + 4);
        c.frame_count = ReadU32(cp + 8);
        if (version == kVersionV3) {
            c.channels = ReadU16(cp + 12);
            c.clip_flags = ReadU16(cp + 14);
            if (c.channels != 1 && c.channels != 2) return false;
        } else {
            c.channels = 1;
            c.clip_flags = 100;
        }
        if (c.channels == 2) ++stereo_cnt;
        const size_t span = size_t(c.frame_count) * size_t(c.channels);
        if (c.slot_id >= slot_count || size_t(c.sample_offset) + span > total_samples) {
            return false;
        }
        parsed_clips[i] = c;
        parsed_slot_clips[c.slot_id].push_back(i);
    }

    std::vector<SigRec> parsed_sigs(sig_count);
    const uint8_t* sp = raw + 24 + clip_bytes;
    for (uint32_t i = 0; i < sig_count; ++i, sp += 16) {
        SigRec s;
        s.fnv64 = ReadU64(sp);
        s.orig_size = ReadU32(sp + 8);
        s.slot_id = ReadU16(sp + 12);
        s.flags = ReadU16(sp + 14);
        if (s.slot_id >= slot_count) return false;
        parsed_sigs[i] = s;
    }

    std::vector<int16_t> parsed_pcm(total_samples);
    std::memcpy(parsed_pcm.data(), raw + header_and_tables, total_samples * sizeof(int16_t));

    std::lock_guard<std::mutex> lock(g_state.pack_mutex);
    g_state.pack_version = version;
    g_state.sample_rate = rate;
    g_state.slot_count = slot_count;
    g_state.stereo_clips = stereo_cnt;
    g_state.clips = std::move(parsed_clips);
    g_state.sigs = std::move(parsed_sigs);
    g_state.pcm = std::move(parsed_pcm);
    g_state.slot_clips = std::move(parsed_slot_clips);
    g_state.last_clip_choice.fill(0);
    g_state.loaded.store(true, std::memory_order_release);

    WA_LOGI("WeaponAudio BWAR v%u: loaded %u clips (%u stereo) across %u slots (%u ISO sigs, %.2f MB)",
            version, clip_count, stereo_cnt, slot_count, sig_count, float(bytes) / 1048576.0f);
    return true;
}

bool LoadPackFromFile(const char* path) {
    if (!path || !*path) return false;
    std::vector<uint8_t> buf;
    if (!TryReadFileBytes(path, buf)) return false;
    return LoadPackFromMemory(buf.data(), buf.size());
}

bool EnsureLoaded() {
    if (g_state.loaded.load(std::memory_order_acquire)) return true;
    bool expected = false;
    if (!g_state.load_attempted.compare_exchange_strong(expected, true)) {
        return g_state.loaded.load(std::memory_order_acquire);
    }

#ifdef __ANDROID__
    if (SDL_IOStream* io = SDL_IOFromFile("weapon_audio/black_weapon_audio.pak", "rb")) {
        const Sint64 sz = SDL_GetIOSize(io);
        if (sz > 24 && sz < 160 * 1024 * 1024) {
            std::vector<uint8_t> buf(static_cast<size_t>(sz));
            if (SDL_ReadIO(io, buf.data(), buf.size()) == buf.size()) {
                SDL_CloseIO(io);
                if (LoadPackFromMemory(buf.data(), buf.size())) return true;
            } else {
                SDL_CloseIO(io);
            }
        } else {
            SDL_CloseIO(io);
        }
    }
    static constexpr const char* kCandidates[] = {
        "/storage/emulated/0/Black/weapon_audio/black_weapon_audio.pak",
        "weapon_audio/black_weapon_audio.pak",
    };
#else
    static constexpr const char* kCandidates[] = {
        "app/src/main/assets/weapon_audio/black_weapon_audio.pak",
        "../app/src/main/assets/weapon_audio/black_weapon_audio.pak",
        "../../app/src/main/assets/weapon_audio/black_weapon_audio.pak",
        "weapon_audio/black_weapon_audio.pak",
    };
#endif
    for (const char* p : kCandidates) {
        if (LoadPackFromFile(p)) return true;
    }
    WA_LOGI("WeaponAudio: pack not found; fallback to original game audio");
    return false;
}

int32_t FindSignature(uint64_t fnv64, uint32_t orig_bytes, uint16_t* out_flags) {
    if (!g_state.loaded.load(std::memory_order_acquire)) return -1;
    auto it = std::lower_bound(
        g_state.sigs.begin(), g_state.sigs.end(),
        std::pair<uint64_t, uint32_t>{fnv64, orig_bytes},
        [](const SigRec& r, const std::pair<uint64_t, uint32_t>& k) {
            return r.fnv64 < k.first || (r.fnv64 == k.first && r.orig_size < k.second);
        });
    if (it != g_state.sigs.end() && it->fnv64 == fnv64 && it->orig_size == orig_bytes) {
        if (out_flags) *out_flags = it->flags;
        return int32_t(it->slot_id);
    }
    return -1;
}

void OnPlayerFire(std::string_view weapon_code, bool suppressed, bool alt_fire, bool force_pack) {
    if (!force_pack && !modsettings::Registry().GetEnhanced().mod_audio_pack_enabled) return;
    if (!EnsureLoaded()) return;
    const int32_t slot = SlotForPlayerFire(weapon_code, suppressed, alt_fire);
    if (slot < 0) return;

    std::lock_guard<std::mutex> lock(g_state.mix_mutex);
    g_state.last_player_fire_ms = NowMs();
    TriggerPlayerWeaponLayersLocked(uint32_t(slot), 0);
}

bool OnVoicePlay(uint32_t voice_obj, const void* raw_data, uint32_t bytes,
                 int32_t& sig_slot, uint16_t& sig_flags,
                 bool is3d, uint32_t mode, const float pos[3],
                 float min_dist, float max_dist, float volume) {
    if (sig_slot == -2) {
        if (raw_data && bytes >= 512 && bytes <= 1048576 && EnsureLoaded()) {
            const uint64_t h = Fnv1a64(raw_data, bytes);
            sig_slot = FindSignature(h, bytes, &sig_flags);
        } else {
            sig_slot = -1;
            sig_flags = 0;
        }
    }
    if (sig_slot < 0) return false;
    const auto& enh = modsettings::Registry().GetEnhanced();
    if (!enh.mod_audio_pack_enabled) return false;
    if (!g_state.loaded.load(std::memory_order_acquire)) return false;

    const bool is_fp_fire_loop = (sig_flags & 1u) != 0;
    const bool is_world_loop = (sig_flags & 2u) != 0;
    const uint64_t now = NowMs();

    std::lock_guard<std::mutex> lock(g_state.mix_mutex);
    if (is_fp_fire_loop) {
        g_state.fp_loops_muted.fetch_add(1, std::memory_order_relaxed);
        // Only synthesize fallback layered shot if sub_001EEFD0 did not already trigger within 60 ms.
        if (now - g_state.last_player_fire_ms >= 60 && now - g_state.last_fp_fallback_ms >= 60) {
            g_state.last_fp_fallback_ms = now;
            TriggerPlayerWeaponLayersLocked(uint32_t(sig_slot), voice_obj);
        }
        return true;
    }

    // Throttle identical voice re-triggers on rapid AI burst loops (40 ms cooldown per Voice object).
    for (const auto& cd : g_state.cooldowns) {
        if (cd.voice_obj == voice_obj && now - cd.ms < 40) {
            return true;
        }
    }
    g_state.cooldowns[g_state.cooldown_cursor] = {voice_obj, now};
    g_state.cooldown_cursor = (g_state.cooldown_cursor + 1) % g_state.cooldowns.size();

    SpawnClipForSlotLocked(uint32_t(sig_slot), voice_obj, is3d, mode, pos, min_dist, max_dist, volume,
                           0u, -1.0f, is_world_loop);

    // Distance-aware far-field gunfire layering for enemy weapons (never triggers on player fire)
    if (IsAiGunfireSlot(uint32_t(sig_slot)) && is3d && enh.audio_distant_gunfire > 0) {
        const float dist = Estimate3DDistance(is3d, mode, pos);
        if (dist > 14.0f) {
            const uint32_t far_slot = (dist > 45.0f) ? uint32_t(kSlotDistantBattlefield) : uint32_t(kSlotTailRifleMattak);
            SpawnClipForSlotLocked(far_slot, 0, is3d, mode, pos,
                                   std::max(min_dist, 4.0f), std::max(max_dist, 240.0f),
                                   std::clamp(volume * 0.65f, 0.22f, 0.88f), 1152u, 0.38f);
        }
    }

    // Authentic supersonic bullet crack / whizz layered ONLY on real flyby or ricochet events
    if ((sig_slot == kSlotEnvFlyby || (sig_slot == kSlotImpRicochet && (g_state.rng_state & 1u) == 0)) &&
        enh.audio_bullet_flyby > 0) {
        SpawnClipForSlotLocked(uint32_t(kSlotEnvSupersonicCrack), 0, is3d, mode, pos,
                               std::max(min_dist, 4.0f), std::max(max_dist, 95.0f),
                               std::clamp(volume * 0.85f, 0.25f, 1.0f), 0u, 0.16f);
    }
    return true;
}

void OnVoiceStop(uint32_t voice_obj) {
    if (!voice_obj) return;
    std::lock_guard<std::mutex> lock(g_state.mix_mutex);
    for (auto& av : g_state.pool) {
        // Trim primary close weapon loops and world continuous loops; GunshotTail & IndoorEcho ring out naturally.
        if (av.active && av.owner_voice_obj == voice_obj &&
            (av.stem == StemCategory::Weapon || av.is_loop_voice)) {
            const uint32_t fade_keep = av.is_loop_voice ? 1200u : 600u;
            const uint32_t remaining = uint32_t(double(av.frame_count) - av.cursor);
            if (remaining > fade_keep) av.frame_count = uint32_t(av.cursor) + fade_keep;
        }
    }
}

void StopAll() {
    std::lock_guard<std::mutex> lock(g_state.mix_mutex);
    for (auto& av : g_state.pool) av.active = false;
    for (auto& sv : g_state.synth_pool) sv.active = false;
    g_state.hdr_env = 0.0f;
    g_state.limiter_gain = 1.0f;
}

void SetEnvironmentState(bool reverb_active, float reverb_late) {
    g_state.env_reverb_active.store(reverb_active, std::memory_order_relaxed);
    g_state.env_reverb_late.store(reverb_late, std::memory_order_relaxed);
}

namespace {

void SpawnSynthLocked(SynthKind kind, uint32_t len_samples, uint32_t delay_samples,
                      float gain_l, float gain_r, float pitch_mul) {
    SynthVoice* dst = nullptr;
    uint32_t max_idx = 0;
    for (auto& sv : g_state.synth_pool) {
        if (!sv.active) {
            dst = &sv;
            break;
        }
        if (sv.sample_idx >= max_idx) {
            max_idx = sv.sample_idx;
            dst = &sv;
        }
    }
    if (!dst) return;
    dst->active = true;
    dst->kind = kind;
    dst->sample_idx = 0;
    dst->length_samples = len_samples;
    dst->delay_samples = delay_samples;
    dst->gain_l = gain_l;
    dst->gain_r = gain_r;
    dst->pitch_mul = pitch_mul;
}

float EvalSynthSample(SynthKind kind, uint32_t idx, float pitch_mul) {
    constexpr float kTwoPi = 6.28318530718f;
    const float t = float(idx) / float(kExpectedRate);
    switch (kind) {
        case SynthKind::HitBody: {
            const float env_body = std::exp(-t * 145.0f);
            const float env_tick = std::exp(-t * 420.0f);
            const float freq = (145.0f - 78.0f * std::min(1.0f, t * 45.0f)) * pitch_mul;
            const float thud = std::sin(kTwoPi * freq * t) * env_body;
            const float tick = std::sin(kTwoPi * 2150.0f * t) * env_tick * 0.48f;
            return thud + tick;
        }
        case SynthKind::HitHead: {
            const float env_snap = std::exp(-t * 210.0f);
            const float env_chime = std::exp(-t * 105.0f);
            const float thud = std::sin(kTwoPi * 195.0f * t) * std::exp(-t * 130.0f) * 0.65f;
            const float snap = (std::sin(kTwoPi * 3420.0f * pitch_mul * t) * 0.58f +
                                std::sin(kTwoPi * 4890.0f * pitch_mul * t) * 0.36f) * env_snap +
                               std::sin(kTwoPi * 1240.0f * pitch_mul * t) * env_chime * 0.42f;
            return thud + snap;
        }
        case SynthKind::HitKill: {
            const float env_lock = std::exp(-t * 62.0f);
            const float env_sub = std::exp(-t * 85.0f);
            const float sub = std::sin(kTwoPi * 112.0f * t) * env_sub * 0.72f;
            const float chime = (std::sin(kTwoPi * 690.0f * t) * 0.55f +
                                 std::sin(kTwoPi * 1380.0f * t) * 0.35f) * env_lock;
            return sub + chime;
        }
        case SynthKind::BrassClink: {
            const float env_a = std::exp(-t * 175.0f);
            const float ping_a = (std::sin(kTwoPi * 4120.0f * pitch_mul * t) * 0.62f +
                                  std::sin(kTwoPi * 6380.0f * pitch_mul * t) * 0.38f) * env_a;
            const float t_b = t - 0.017f;
            float ping_b = 0.0f;
            if (t_b > 0.0f) {
                const float env_b = std::exp(-t_b * 210.0f);
                ping_b = (std::sin(kTwoPi * 4280.0f * pitch_mul * t_b) * 0.60f +
                          std::sin(kTwoPi * 6590.0f * pitch_mul * t_b) * 0.35f) * env_b * 0.44f;
            }
            return ping_a + ping_b;
        }
        case SynthKind::PlasticHullClink: {
            const float env_a = std::exp(-t * 200.0f);
            const float ping_a = (std::sin(kTwoPi * 1340.0f * pitch_mul * t) * 0.72f +
                                  std::sin(kTwoPi * 3240.0f * pitch_mul * t) * 0.28f) * env_a;
            const float t_b = t - 0.022f;
            float ping_b = 0.0f;
            if (t_b > 0.0f) {
                const float env_b = std::exp(-t_b * 230.0f);
                ping_b = (std::sin(kTwoPi * 1410.0f * pitch_mul * t_b) * 0.70f +
                          std::sin(kTwoPi * 3310.0f * pitch_mul * t_b) * 0.25f) * env_b * 0.48f;
            }
            return ping_a + ping_b;
        }
        case SynthKind::KineticDashWhoosh: {
            const float env = std::exp(-t * 16.0f);
            const float punch = std::exp(-t * 95.0f);
            const float freq = (165.0f - 115.0f * std::min(1.0f, t * 7.5f)) * pitch_mul;
            const float air = std::sin(kTwoPi * (540.0f - 320.0f * std::min(1.0f, t * 8.0f)) * t) * env * 0.35f;
            const float sub = std::sin(kTwoPi * freq * t) * env * 0.72f +
                              std::sin(kTwoPi * 72.0f * t) * punch * 0.55f;
            return sub + air;
        }
        case SynthKind::ShockwaveBlast: {
            const float sub_env = std::exp(-t * 11.5f);
            const float crack_env = std::exp(-t * 58.0f);
            const float drop = (128.0f - 96.0f * std::min(1.0f, t * 5.0f));
            const float sub = std::sin(kTwoPi * drop * t) * sub_env * 0.92f;
            const float ring = (std::sin(kTwoPi * 310.0f * t) * 0.40f +
                                std::sin(kTwoPi * 620.0f * t) * 0.22f) * crack_env;
            return sub + ring;
        }
        case SynthKind::OverdriveSurge: {
            const float env = std::exp(-t * 5.8f);
            const float f0 = (260.0f + 420.0f * std::min(1.0f, t * 3.8f)) * pitch_mul;
            const float chord = std::sin(kTwoPi * f0 * t) * 0.52f +
                                std::sin(kTwoPi * f0 * 1.498f * t) * 0.32f +
                                std::sin(kTwoPi * 96.0f * t) * std::exp(-t * 14.0f) * 0.45f;
            return chord * env;
        }
        case SynthKind::SpecialAmmoSwitch: {
            const float e1 = std::exp(-t * 110.0f);
            const float c1 = std::sin(kTwoPi * 1760.0f * pitch_mul * t) * e1 * 0.55f;
            const float t2 = t - 0.032f;
            float c2 = 0.0f;
            if (t2 > 0.0f) {
                c2 = std::sin(kTwoPi * 2480.0f * pitch_mul * t2) * std::exp(-t2 * 95.0f) * 0.58f;
            }
            return c1 + c2;
        }
    }
    return 0.0f;
}

}  // namespace

void TriggerHitConfirm(bool headshot, bool lethal) {
    const auto& enh = modsettings::Registry().GetEnhanced();
    if (!enh.hitmarker_feedback) return;
    std::lock_guard<std::mutex> lock(g_state.mix_mutex);
    const float var = 1.0f + NextRandBipolar(g_state.rng_state) * 0.035f;
    if (headshot) {
        SpawnSynthLocked(SynthKind::HitHead, 2000u, 0u, 0.26f, 0.26f, var);
    } else {
        SpawnSynthLocked(SynthKind::HitBody, 1500u, 0u, 0.22f, 0.22f, var);
    }
    if (lethal) {
        SpawnSynthLocked(SynthKind::HitKill, 3600u, headshot ? 480u : 0u, 0.30f, 0.30f, var);
    }
    g_state.synth_hit_confirms.fetch_add(1, std::memory_order_relaxed);
}

void TriggerShellClink(float pan_right_ratio, bool is_shotgun_hull, SurfaceKind surface) {
    const auto& enh = modsettings::Registry().GetEnhanced();
    if (!enh.shell_casings) return;
    const uint64_t now_ms = NowMs();
    std::lock_guard<std::mutex> lock(g_state.mix_mutex);
    if (g_state.last_clink_ms != 0 && now_ms >= g_state.last_clink_ms &&
        (now_ms - g_state.last_clink_ms) < 38u) {
        return;
    }
    g_state.last_clink_ms = now_ms;
    const float pan = std::clamp(pan_right_ratio, -1.0f, 1.0f);
    const float gain = is_shotgun_hull ? 0.145f : 0.135f;
    const float gl = gain * std::sqrt(0.5f * (1.0f - pan * 0.85f));
    const float gr = gain * std::sqrt(0.5f * (1.0f + pan * 0.85f));
    const float pitch = 1.0f + NextRandBipolar(g_state.rng_state) * 0.08f;

    // Layer real surface-specific WSTFG casing foley when pack is loaded & occlusion/surface audio enabled
    if (enh.mod_audio_pack_enabled && enh.audio_occlusion_enabled &&
        g_state.loaded.load(std::memory_order_acquire) && enh.mechanical_audio_volume > 0) {
        int32_t slot = is_shotgun_hull ? kSlotCasingShell : kSlotCasingBrass;
        if (!is_shotgun_hull) {
            switch (surface) {
                case SurfaceKind::Metal: slot = kSlotCasingMetal; break;
                case SurfaceKind::Dirt:  slot = kSlotCasingDirt; break;
                case SurfaceKind::Wood:  slot = kSlotCasingWood; break;
                case SurfaceKind::Concrete:
                default: slot = kSlotCasingBrass; break;
            }
        }
        if (SpawnClipForSlotLocked(uint32_t(slot), 0u, false, 0u, nullptr, 1.0f, 22.0f, 0.24f, 0u, 0.08f, false)) {
            ActiveVoice* newest = nullptr;
            for (auto& av : g_state.pool) {
                if (av.active && av.slot_id == uint32_t(slot)) newest = &av;
            }
            if (newest) {
                newest->has_forced_pan = true;
                newest->forced_pan = pan;
            }
        }
    }

    SpawnSynthLocked(is_shotgun_hull ? SynthKind::PlasticHullClink : SynthKind::BrassClink,
                     is_shotgun_hull ? 2400u : 2200u, 0u, gl, gr, pitch);
    g_state.synth_shell_clinks.fetch_add(1, std::memory_order_relaxed);
}

void TriggerWeaponInspect() {
    const auto& enh = modsettings::Registry().GetEnhanced();
    if (!enh.mod_audio_pack_enabled || !EnsureLoaded()) return;
    std::lock_guard<std::mutex> lock(g_state.mix_mutex);
    if (enh.mechanical_audio_volume == 0) return;
    SpawnClipForSlotLocked(uint32_t(kSlotWeaponInspect), 0u, false, 0u, nullptr, 1.0f, 60.0f, 0.72f, 0u, 0.14f, false);
}

float ComputeConcussionIntensity(float dist_m) {
    if (!std::isfinite(dist_m) || dist_m < 0.0f) return 0.0f;
    if (dist_m >= 16.0f) return 0.0f;
    const float u = 1.0f - (dist_m / 16.0f);
    return std::clamp(u * u * 1.15f, 0.0f, 1.0f);
}

float ComputeAcousticLowPassAlpha(bool occluded_by_geometry, float dist_m) {
    const float d = std::isfinite(dist_m) ? std::max(dist_m, 0.0f) : 0.0f;
    const float air = (d > 12.0f) ? std::clamp(18.0f / (d + 6.0f), 0.18f, 1.0f) : 1.0f;
    if (!occluded_by_geometry) return air;
    // Behind-wall / behind-listener occlusion gently rolls off harsh highs cleanly
    return std::clamp(air * 0.34f, 0.08f, 0.42f);
}

void TriggerExplosionConcussion(float dist_m) {
    const auto& enh = modsettings::Registry().GetEnhanced();
    if (!enh.audio_tinnitus_enabled) return;
    const float strength = ComputeConcussionIntensity(dist_m);
    if (strength <= 0.10f) return;
    std::lock_guard<std::mutex> lock(g_state.mix_mutex);
    g_state.tinnitus_env = std::max(g_state.tinnitus_env, strength);
    g_state.concussion_daze.store(g_state.tinnitus_env, std::memory_order_relaxed);
}

float CurrentConcussionDaze() {
    return g_state.concussion_daze.load(std::memory_order_relaxed);
}

void ApplyMasterPostDsp(float* out, uint32_t frames, uint32_t out_channels,
                        uint32_t fl_ch, uint32_t fr_ch) {
    if (!out || frames == 0 || out_channels < 2) return;
    const auto& enh = modsettings::Registry().GetEnhanced();
    std::lock_guard<std::mutex> lock(g_state.mix_mutex);
    if (!enh.audio_tinnitus_enabled || g_state.tinnitus_env <= 0.002f) {
        g_state.tinnitus_env = 0.0f;
        g_state.concussion_daze.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const uint32_t ch_limit = std::min<uint32_t>(out_channels, uint32_t(g_state.post_lp_state.size()));
    const float master = std::clamp(float(enh.master_gain_percent) * 0.01f, 0.10f, 1.50f);
    // Logarithmic decay (~2.3s recovery) + smooth hearing un-muffling
    constexpr double kTwoPi = 6.283185307179586;
    constexpr double kIncA = kTwoPi * 3920.0 / double(kExpectedRate);
    constexpr double kIncB = kTwoPi * 4185.0 / double(kExpectedRate);
    const float decay_per_sample = std::exp(-1.0f / (float(kExpectedRate) * 1.15f));

    for (uint32_t f = 0; f < frames; ++f) {
        const float env = g_state.tinnitus_env;
        g_state.tinnitus_env *= decay_per_sample;
        if (env <= 0.0015f) break;

        // Hearing occlusion sweeps back open gradually as tinnitus subsides
        const float lp_alpha = std::clamp(1.0f - env * 0.86f, 0.10f, 1.0f);
        const float duck_gain = std::clamp(1.0f - env * 0.34f, 0.58f, 1.0f);
        float* row = out + size_t(f) * out_channels;
        for (uint32_t c = 0; c < ch_limit; ++c) {
            g_state.post_lp_state[c] += lp_alpha * (row[c] - g_state.post_lp_state[c]);
            row[c] = g_state.post_lp_state[c] * duck_gain;
        }
        const float ring = (float(std::sin(g_state.tinnitus_phase_a)) * 0.65f +
                            float(std::sin(g_state.tinnitus_phase_b)) * 0.35f) *
                           (env * env) * 0.085f * master;
        g_state.tinnitus_phase_a += kIncA;
        if (g_state.tinnitus_phase_a > kTwoPi) g_state.tinnitus_phase_a -= kTwoPi;
        g_state.tinnitus_phase_b += kIncB;
        if (g_state.tinnitus_phase_b > kTwoPi) g_state.tinnitus_phase_b -= kTwoPi;

        if (fl_ch < out_channels) row[fl_ch] = std::clamp(row[fl_ch] + ring, -0.994f, 0.994f);
        if (fr_ch < out_channels) row[fr_ch] = std::clamp(row[fr_ch] + ring * 0.92f, -0.994f, 0.994f);
    }
    if (g_state.tinnitus_env <= 0.002f) g_state.tinnitus_env = 0.0f;
    g_state.concussion_daze.store(g_state.tinnitus_env, std::memory_order_relaxed);
}

void TriggerKineticDash(bool with_shockwave) {
    std::lock_guard<std::mutex> lock(g_state.mix_mutex);
    const float var = 1.0f + NextRandBipolar(g_state.rng_state) * 0.04f;
    SpawnSynthLocked(SynthKind::KineticDashWhoosh, 7200u, 0u, 0.38f, 0.38f, var);
    if (with_shockwave) {
        SpawnSynthLocked(SynthKind::ShockwaveBlast, 12000u, 480u, 0.46f, 0.46f, var);
    }
    g_state.synth_overdrive_cues.fetch_add(1, std::memory_order_relaxed);
}

void TriggerOverdriveCue(bool activating) {
    std::lock_guard<std::mutex> lock(g_state.mix_mutex);
    const float pitch = activating ? 1.08f : 0.72f;
    const float gain = activating ? 0.36f : 0.24f;
    SpawnSynthLocked(SynthKind::OverdriveSurge, 11500u, 0u, gain, gain, pitch);
    g_state.synth_overdrive_cues.fetch_add(1, std::memory_order_relaxed);
}

void TriggerSpecialAmmoCue(uint8_t ammo_mode) {
    std::lock_guard<std::mutex> lock(g_state.mix_mutex);
    const float pitch = 0.88f + float(ammo_mode % 4u) * 0.14f;
    SpawnSynthLocked(SynthKind::SpecialAmmoSwitch, 4200u, 0u, 0.26f, 0.26f, pitch);
    g_state.synth_overdrive_cues.fetch_add(1, std::memory_order_relaxed);
}

void TriggerSpecialSlot(int32_t slot_id, float gain, bool is_loop, bool force_pack) {
    if (slot_id < 0) return;
    if (!force_pack && !modsettings::Registry().GetEnhanced().mod_audio_pack_enabled) return;
    if (!EnsureLoaded()) return;
    std::lock_guard<std::mutex> lock(g_state.mix_mutex);
    SpawnClipForSlotLocked(uint32_t(slot_id), 0u, false, 0, nullptr, 2.0f, 120.0f,
                           std::clamp(gain, 0.05f, 1.45f), 0u, 0.18f, is_loop);
}

void TriggerWorldImpact(const float pos[3], bool ricochet, bool supersonic_crack) {
    if (!pos) return;
    const auto& enh = modsettings::Registry().GetEnhanced();
    if (!enh.mod_audio_pack_enabled || !EnsureLoaded()) return;
    std::lock_guard<std::mutex> lock(g_state.mix_mutex);
    if (enh.environmental_audio_volume > 0) {
        SpawnClipForSlotLocked(uint32_t(kSlotImpConcrete), 0u, true, 0u, pos,
                               4.0f, 140.0f, 0.78f, 0u, 0.36f, false);
    }
    if (ricochet && enh.audio_bullet_flyby > 0) {
        SpawnClipForSlotLocked(uint32_t(kSlotImpRicochet), 0u, true, 0u, pos,
                               4.0f, 140.0f, 0.68f, 192u, 0.32f, false);
    }
    if (supersonic_crack && enh.audio_bullet_flyby > 0) {
        SpawnClipForSlotLocked(uint32_t(kSlotEnvSupersonicCrack), 0u, true, 0u, pos,
                               5.0f, 160.0f, 0.76f, 0u, 0.24f, false);
    }
}

void RenderMix(float* out, float* send, uint32_t frames,
               uint32_t out_channels, uint32_t fl_ch, uint32_t fr_ch,
               bool surround, uint32_t lfe_ch, uint32_t bl_ch, uint32_t br_ch,
               const float listener_pos[3], const float listener_front[3], const float listener_top[3]) {
    if (!out || frames == 0 || out_channels < 2) return;
    const auto& enh = modsettings::Registry().GetEnhanced();

    std::lock_guard<std::mutex> lock(g_state.mix_mutex);
    const bool pack_ready = enh.mod_audio_pack_enabled && g_state.loaded.load(std::memory_order_acquire);
    bool any_synth = false;
    for (const auto& sv : g_state.synth_pool) {
        if (sv.active) {
            any_synth = true;
            break;
        }
    }
    if (!pack_ready && !any_synth) return;

    if (listener_pos) {
        g_state.listener_pos[0] = listener_pos[0];
        g_state.listener_pos[1] = listener_pos[1];
        g_state.listener_pos[2] = listener_pos[2];
        g_state.has_listener_pos = true;
    }

    const int16_t* pcm_data = pack_ready ? g_state.pcm.data() : nullptr;
    const size_t pcm_size = pack_ready ? g_state.pcm.size() : 0u;
    const uint32_t ch_limit = std::min(out_channels, 6u);
    constexpr uint32_t kTileFrames = 256u;

    for (uint32_t tile_start = 0; tile_start < frames;) {
        const uint32_t n = std::min(frames - tile_start, kTileFrames);
        float* tile_out = out + size_t(tile_start) * out_channels;
        float* tile_send = send ? (send + tile_start) : nullptr;
        float fg_abs[kTileFrames] = {};
        float duck_curve[kTileFrames];
        bool rendered_any = false;

        // Pass 0 renders priority foreground transients; Pass 1 ducks tails/ambience via HDR envelope.
        if (pack_ready) {
        for (int pass = 0; pass < 2; ++pass) {
            if (pass == 1) {
                for (uint32_t f = 0; f < n; ++f) {
                    const float p = fg_abs[f];
                    const float k = (p > g_state.hdr_env) ? 0.08f : 0.00022f;
                    g_state.hdr_env += k * (p - g_state.hdr_env);
                    duck_curve[f] = std::clamp(1.0f - std::max(0.0f, g_state.hdr_env - 0.30f) * 0.62f, 0.50f, 1.0f);
                }
            }

            for (auto& av : g_state.pool) {
                if (!av.active || av.is_duckable_hdr != (pass == 1)) continue;

                float stem_gain = StemVolumeGain(av.stem, enh);
                if (IsUiSlot(av.slot_id)) stem_gain = std::max(stem_gain, 0.55f);
                if (stem_gain <= 1e-4f) {
                    if (av.delay_samples >= n) {
                        av.delay_samples -= n;
                    } else {
                        const uint32_t adv = n - av.delay_samples;
                        av.delay_samples = 0;
                        av.cursor += double(adv) * av.rate_ratio;
                        if (av.cursor >= double(av.frame_count - 1)) av.active = false;
                    }
                    continue;
                }
                if (av.delay_samples >= n) {
                    av.delay_samples -= n;
                    continue;
                }
                const uint32_t start_f = av.delay_samples;
                av.delay_samples = 0;

                float pan_l = 0.7071f, pan_r = 0.7071f, att = 1.0f;
                const float dist = Compute3DGainAndPan(av, listener_pos, listener_front, listener_top, pan_l, pan_r, att);
                if (av.channels == 2 && (!av.is3d || av.mode == 2)) {
                    pan_l = 0.95f;
                    pan_r = 0.95f;
                }
                if (av.has_forced_pan) {
                    const float p = std::clamp(av.forced_pan, -1.0f, 1.0f);
                    pan_l = std::sqrt(0.5f * (1.0f - p * 0.85f));
                    pan_r = std::sqrt(0.5f * (1.0f + p * 0.85f));
                }
                const float total_gain = av.gain * stem_gain * att;
                bool occluded = false;
                if (enh.audio_occlusion_enabled && av.is3d && listener_pos && listener_front && dist > 3.5f) {
                    const float dx = av.pos[0] - listener_pos[0];
                    const float dz = av.pos[2] - listener_pos[2];
                    const float dlen = std::sqrt(dx * dx + dz * dz);
                    if (dlen > 1e-4f) {
                        const float dot_f = (dx * listener_front[0] + dz * listener_front[2]) / dlen;
                        occluded = (dot_f < -0.20f) || (dist > 34.0f && av.stem == StemCategory::DistantGunfire);
                    }
                }
                const float air_alpha = av.is3d
                    ? (enh.audio_occlusion_enabled
                           ? ComputeAcousticLowPassAlpha(occluded, dist)
                           : ((dist > 12.0f) ? std::clamp(18.0f / (dist + 6.0f), 0.16f, 1.0f) : 1.0f))
                    : 1.0f;
                const float rev_send = av.reverb_send * att * stem_gain * av.gain;
                const uint32_t tail_fade_samples = (av.frame_count > 1200) ? 720u : 120u;
                const bool is_sub_stem = (av.stem == StemCategory::Weapon ||
                                          av.stem == StemCategory::GunshotTail ||
                                          av.stem == StemCategory::Environment);
                const float eff_width = (av.is3d && av.mode != 2)
                    ? av.stereo_width * std::clamp(1.0f - (dist - 3.0f) * 0.06f, 0.12f, 0.72f)
                    : av.stereo_width;
                rendered_any = true;

                for (uint32_t f = start_f; f < n; ++f) {
                    const uint32_t idx = uint32_t(av.cursor);
                    if (idx + 1 >= av.frame_count) {
                        av.active = false;
                        break;
                    }
                    const size_t pos0 = size_t(av.sample_offset) + size_t(idx) * size_t(av.channels);
                    const size_t pos1 = pos0 + size_t(av.channels);
                    if (pos1 + size_t(av.channels - 1) >= pcm_size) {
                        av.active = false;
                        break;
                    }
                    const float frac = float(av.cursor - double(idx));
                    float in_l = 0.0f;
                    float in_r = 0.0f;
                    if (av.channels == 2) {
                        const float l0 = float(pcm_data[pos0]) * kInv32768;
                        const float r0 = float(pcm_data[pos0 + 1]) * kInv32768;
                        const float l1 = float(pcm_data[pos1]) * kInv32768;
                        const float r1 = float(pcm_data[pos1 + 1]) * kInv32768;
                        const float raw_l = l0 + (l1 - l0) * frac;
                        const float raw_r = r0 + (r1 - r0) * frac;
                        const float mid = 0.5f * (raw_l + raw_r);
                        const float side = 0.5f * (raw_l - raw_r);
                        // Keep sub-120Hz phase-locked to Mid for mono/phone speaker safety
                        av.side_lp_state += 0.0156f * (side - av.side_lp_state);
                        const float side_hp = side - av.side_lp_state;
                        in_l = mid + side_hp * eff_width;
                        in_r = mid - side_hp * eff_width;
                    } else {
                        const float s0 = float(pcm_data[pos0]) * kInv32768;
                        const float s1 = float(pcm_data[pos1]) * kInv32768;
                        in_l = s0 + (s1 - s0) * frac;
                        in_r = in_l;
                    }

                    if (air_alpha < 0.999f) {
                        av.lp_state_l += air_alpha * (in_l - av.lp_state_l);
                        av.lp_state_r += air_alpha * (in_r - av.lp_state_r);
                        in_l = av.lp_state_l;
                        in_r = av.lp_state_r;
                    }

                    const uint32_t rem = av.frame_count - idx;
                    if (rem < tail_fade_samples) {
                        const float fade = float(rem) / float(tail_fade_samples);
                        in_l *= fade;
                        in_r *= fade;
                    }

                    const float duck = (pass == 1) ? duck_curve[f] : 1.0f;
                    const float g = total_gain * duck;
                    const float out_l = in_l * g * pan_l;
                    const float out_r = in_r * g * pan_r;
                    float* frame_out = tile_out + size_t(f) * out_channels;
                    if (fl_ch < out_channels) frame_out[fl_ch] += out_l;
                    if (fr_ch < out_channels) frame_out[fr_ch] += out_r;
                    if (pass == 0) fg_abs[f] += std::max(std::fabs(out_l), std::fabs(out_r));
                    if (surround) {
                        const float mid_out = 0.5f * (in_l + in_r) * g;
                        if (lfe_ch < out_channels && is_sub_stem) frame_out[lfe_ch] += mid_out * 0.24f;
                        if (bl_ch < out_channels) frame_out[bl_ch] += out_l * 0.34f;
                        if (br_ch < out_channels) frame_out[br_ch] += out_r * 0.34f;
                    }
                    if (tile_send) tile_send[f] += 0.5f * (in_l + in_r) * rev_send * duck;
                    av.cursor += av.rate_ratio;
                }
            }
        }
        }  // if (pack_ready)

        // Procedural tactile hit-confirm & bouncing shell-clink audio cues
        const float synth_master = std::clamp(float(enh.master_gain_percent) * 0.01f, 0.10f, 1.50f);
        for (auto& sv : g_state.synth_pool) {
            if (!sv.active) continue;
            if (sv.delay_samples >= n) {
                sv.delay_samples -= n;
                continue;
            }
            const uint32_t start_f = sv.delay_samples;
            sv.delay_samples = 0;
            rendered_any = true;
            for (uint32_t f = start_f; f < n; ++f) {
                if (sv.sample_idx >= sv.length_samples) {
                    sv.active = false;
                    break;
                }
                const float s = EvalSynthSample(sv.kind, sv.sample_idx++, sv.pitch_mul) * synth_master;
                float* frame_out = tile_out + size_t(f) * out_channels;
                if (fl_ch < out_channels) frame_out[fl_ch] += s * sv.gain_l;
                if (fr_ch < out_channels) frame_out[fr_ch] += s * sv.gain_r;
                if (tile_send) tile_send[f] += s * 0.5f * (sv.gain_l + sv.gain_r) * 0.14f;
            }
            if (sv.sample_idx >= sv.length_samples) sv.active = false;
        }

        // Transparent True-Peak HDR Limiter (-1.0 dBTP threshold) prevents clipping
        if (rendered_any) {
            constexpr float kLimitThresh = 0.89125f;
            for (uint32_t f = 0; f < n; ++f) {
                float* frame_out = tile_out + size_t(f) * out_channels;
                float frame_max = 0.0f;
                for (uint32_t c = 0; c < ch_limit; ++c) {
                    frame_max = std::max(frame_max, std::fabs(frame_out[c]));
                }
                const float target_g = (frame_max > kLimitThresh) ? (kLimitThresh / frame_max) : 1.0f;
                const float alpha = (target_g < g_state.limiter_gain) ? 0.65f : 0.0018f;
                g_state.limiter_gain += alpha * (target_g - g_state.limiter_gain);
                for (uint32_t c = 0; c < ch_limit; ++c) {
                    float v = frame_out[c] * g_state.limiter_gain;
                    if (std::fabs(v) > 0.88f) v = SoftClipCeiling(v);
                    frame_out[c] = std::clamp(v, -0.994f, 0.994f);
                }
            }
        }

        tile_start += n;
    }
}

Telemetry GetTelemetry() {
    Telemetry t;
    t.pack_loaded = g_state.loaded.load(std::memory_order_acquire);
    t.pack_version = g_state.pack_version;
    t.sample_rate = g_state.sample_rate;
    t.slot_count = g_state.slot_count;
    t.clip_count = uint32_t(g_state.clips.size());
    t.stereo_clips = g_state.stereo_clips;
    t.signature_count = uint32_t(g_state.sigs.size());
    uint32_t active = 0;
    {
        std::unique_lock<std::mutex> lock(g_state.mix_mutex, std::try_to_lock);
        if (lock.owns_lock()) {
            for (const auto& av : g_state.pool) {
                if (av.active) active++;
            }
        }
    }
    t.active_voices = active;
    t.shots_enhanced = g_state.shots_enhanced.load(std::memory_order_relaxed);
    t.reloads_enhanced = g_state.reloads_enhanced.load(std::memory_order_relaxed);
    t.mechanicals_enhanced = g_state.mechanicals_enhanced.load(std::memory_order_relaxed);
    t.impacts_enhanced = g_state.impacts_enhanced.load(std::memory_order_relaxed);
    t.tails_enhanced = g_state.tails_enhanced.load(std::memory_order_relaxed);
    t.distant_enhanced = g_state.distant_enhanced.load(std::memory_order_relaxed);
    t.flybys_enhanced = g_state.flybys_enhanced.load(std::memory_order_relaxed);
    t.fp_loops_muted = g_state.fp_loops_muted.load(std::memory_order_relaxed);
    t.synth_hit_confirms = g_state.synth_hit_confirms.load(std::memory_order_relaxed);
    t.synth_shell_clinks = g_state.synth_shell_clinks.load(std::memory_order_relaxed);
    t.synth_overdrive_cues = g_state.synth_overdrive_cues.load(std::memory_order_relaxed);
    return t;
}

}  // namespace sound::weapon_audio
