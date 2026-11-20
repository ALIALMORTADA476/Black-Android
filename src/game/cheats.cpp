// The cheats page's god mode, one-hit kill and infinite grenades. The rest live where the game
// already has a hook: infinite ammo, rapid fire and no recoil with the weapon's, game speed with the frame time
#include <algorithm>
#include <cmath>

#include "common/mod_schema.h"
#include "common/options.h"
#include "game/ballistics_fx.h"
#include "game/combat_feedback.h"
#include "game/delirium_physics.h"
#include "game/layout.h"
#include "game/overdrive.h"
#include "game/physics_lab.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;

namespace {

// character: the player is the one in the world container, its kind 2
constexpr uint32_t kCharKind = 0xC4;
constexpr uint32_t kCharGrenades = 0x286;  // uint16
constexpr uint32_t kCharHealth = 0x2F0;    // float, the difficulty's full at most
constexpr uint32_t kCharDying = 0x384;     // not 0 once it is killed
// byte the scripts set: the player's damage calls (its class's 0x1ED990, 0x1ED700, 0x1ED2B0, 0x1ED5D0, and
// 0x1ED1E0) take nothing while it is
constexpr uint32_t kCharInvulnerable = 0x8F2;
constexpr uint32_t kPlayerKind = 2;

constexpr uint32_t kDifficulty = 0x2014C;  // in the game object, 0 easy to 3 black ops
// full health, as 0x13F10 scales 1.0: easy and normal, then hard and black ops
constexpr float kFullHealth[] = {1200.0f, 750.0f};

uint32_t Player() { return R32(kWorld) ? R32(kWorld) + kWorldPlayer : 0; }

float FullHealth() { return kFullHealth[R32(R32(kGame) + kDifficulty) >= 2]; }

// One-hit kill: a hit of the player's on another character starts from a sliver of health, any damage kills.
// A hit that took nothing gives the health back
constexpr float kSliver = 0.001f;

struct Doomed {
    uint32_t who = 0;
    float health = 0;
};

Doomed Doom(uint32_t victim, uint32_t attacker) {
    uint32_t me = Player();
    if (!game::Opt().one_hit_kill || !me || attacker != me || victim == me || R32(victim + kCharDying)) return {};
    float& health = G<float>(victim + kCharHealth);
    if (!(health > kSliver)) return {};
    Doomed kept{victim, health};
    health = kSliver;
    return kept;
}

void Spare(const Doomed& kept) {
    if (!kept.who || R32(kept.who + kCharDying)) return;
    float& health = G<float>(kept.who + kCharHealth);
    if (health > 0) health = kept.health - (kSliver - health);
}

bool g_invulnerable;  // the flag is ours, cleared when god mode goes off

}  // namespace

// The player's update, a frame (ecx the player, its class's own): god mode fills the health and holds the flag
// its damage calls test
X86_EXTERN_FUNC(__imp__sub_001E5FA0);
X86_FUNC(sub_001E5FA0) {
    uint32_t me = ctx.ecx;
    if (me && me == Player() && R32(me + kCharKind) == kPlayerKind) {
        if (game::Opt().god_mode && !R32(me + kCharDying)) {
            float& health = G<float>(me + kCharHealth);
            health = std::max(health, FullHealth());
            G<uint8_t>(me + kCharInvulnerable) = 1;
            g_invulnerable = true;
        } else if (g_invulnerable) {
            G<uint8_t>(me + kCharInvulnerable) = 0;
            g_invulnerable = false;
        }
        game::overdrive::OnPlayerFramePreUpdate(ctx, base, me);
    }
    __imp__sub_001E5FA0(ctx, base);
}

// A soldier takes a bullet (ecx the victim, the sixth of seven arguments its attacker, ret 0x1C): the
// characters' class, the player's has its own
X86_EXTERN_FUNC(__imp__sub_001E74B0);
X86_FUNC(sub_001E74B0) {
    uint32_t victim = ctx.ecx;
    uint32_t attacker = R32(ctx.esp + 0x18);
    uint32_t hit_dir_ea = R32(ctx.esp + 0x08);
    float raw_force = G<float>(ctx.esp + 0x0C);
    uint8_t zone_byte = static_cast<uint8_t>(R32(ctx.esp + 0x10) & 0xFFu);
    if (!victim || !mem::IsCommitted(victim) || !mem::IsCommitted(victim + 0x388u)) {
        ctx.esp += 32;
        return;
    }
    const uint32_t v_node = R32(victim + 0x324u);
    const uint32_t v_268 = R32(victim + 0x268u);
    const bool valid_node = (v_node != 0u && mem::IsCommitted(v_node) && mem::IsCommitted(v_node + 0x94u));
    bool prop_hit_path = false;
    if (v_268 != 0u && mem::IsCommitted(v_268) && mem::IsCommitted(v_268 + 0x24u)) {
        const uint32_t slot = static_cast<uint8_t>(R32(v_268 + 0x18u) >> 8);
        if (mem::IsCommitted(v_268 + slot * 4u + 0x10u) &&
            R32(v_268 + slot * 4u + 0xCu) != 0u) {
            if (zone_byte == 0xFFu) {
                prop_hit_path = true;
            } else if (zone_byte == 0x04u) {
                const uint32_t v_320 = R32(victim + 0x320u);
                if (v_320 != 0u && mem::IsCommitted(v_320 + 8u)) {
                    const uint32_t kind = R32(v_320 + 4u);
                    prop_hit_path = (kind == 0x28u || kind == 0x2Au);
                }
            }
        }
    }
    const bool valid_attacker =
        (attacker != 0u && mem::IsCommitted(attacker) && mem::IsCommitted(attacker + 0x3A0u));
    if (!valid_node && !prop_hit_path) {
        ctx.esp += 32;
        return;
    }
    if (!prop_hit_path && !valid_attacker) {
        ctx.esp += 32;
        return;
    }
    uint32_t me = Player();
    const bool is_headshot = (zone_byte == 0x08u);
    const float imp = game::physics_lab::Params().impact_force.load(std::memory_order_relaxed);
    const float hs_mul = std::clamp(modsettings::Registry().GetEnhanced().headshot_multiplier, 1.0f, 4.0f);
    float health_scale = 1.0f;
    if (me && attacker == me && victim && victim != me) {
        if (std::fabs(imp - 1.0f) > 1e-3f) {
            float& force_arg = G<float>(ctx.esp + 0x0C);
            if (std::isfinite(force_arg)) {
                force_arg *= imp * 3.5f;
            }
            if (!game::Opt().one_hit_kill && imp > 1.01f && !R32(victim + kCharDying)) {
                health_scale *= imp;
            }
        }
        if (is_headshot && hs_mul > 1.01f && !game::Opt().one_hit_kill && !R32(victim + kCharDying)) {
            health_scale *= hs_mul;
        }
        if (!game::overdrive::InSplashOrBurn()) {
            float od_force = G<float>(ctx.esp + 0x0C);
            if (!std::isfinite(od_force)) od_force = raw_force;
            game::overdrive::ModifyPlayerBulletHit(victim, is_headshot, od_force, health_scale);
            if (std::isfinite(od_force)) {
                G<float>(ctx.esp + 0x0C) = od_force;
            }
        }
        if (health_scale > 1.01f) {
            float& hp = G<float>(victim + kCharHealth);
            if (std::isfinite(hp) && hp > kSliver * 2.0f) {
                hp = std::max(kSliver * 2.0f, hp / health_scale);
            } else {
                health_scale = 1.0f;
            }
        }
    }
    const bool was_alive_before_hit =
        (victim && victim != me && mem::IsCommitted(victim + kCharDying) &&
         R32(victim + kCharDying) == 0u);
    Doomed doomed = Doom(victim, attacker);
    __imp__sub_001E74B0(ctx, base);
    Spare(doomed);
        if (me && attacker == me && victim && victim != me &&
            mem::IsCommitted(victim + kCharDying)) {
            const bool is_lethal_hit = was_alive_before_hit && (R32(victim + kCharDying) != 0u);
            game::combat_feedback::RecordPlayerHit(is_headshot, is_lethal_hit);
            float dist_m = 12.0f;
            if (mem::IsCommitted(victim + 0xA8u) && mem::IsCommitted(me + 0xA8u)) {
                const float dx = G<float>(victim + 0xA0u) - G<float>(me + 0xA0u);
                const float dz = G<float>(victim + 0xA8u) - G<float>(me + 0xA8u);
                dist_m = std::sqrt(dx * dx + dz * dz);
            }
            const uint8_t tier = (raw_force >= 3.2f) ? 2u : ((raw_force >= 1.7f) ? 1u : 0u);
            game::combat_feedback::TriggerHitstop(is_headshot, is_lethal_hit, tier, dist_m);
        } else if (me && victim == me && attacker && attacker != me &&
                   mem::IsCommitted(attacker + 0xA8u)) {
            float eye[3]{}, right[3]{}, up[3]{}, fwd[3]{}, view[16]{}, proj[16]{};
            if (game::ballistics::GetSceneCameraVectors(eye, right, up, fwd, view, proj)) {
                const float atk_pos[3] = {
                    G<float>(attacker + 0xA0u),
                    G<float>(attacker + 0xA4u) + 1.25f,
                    G<float>(attacker + 0xA8u),
                };
                game::combat_feedback::RecordDirectionalDamage(eye, fwd, right, atk_pos, 0.72f);
            }
        }
    if (health_scale > 1.0f && victim && !R32(victim + kCharDying)) {
        float& hp = G<float>(victim + kCharHealth);
        if (std::isfinite(hp) && hp > 0.0f) {
            hp *= health_scale;
        }
    }
    if (victim && victim != me && !game::ballistics::InThermalImpact() && !game::overdrive::InSplashOrBurn() &&
        mem::IsCommitted(victim) && mem::IsCommitted(victim + 0x388u)) {
        const float feet_y = G<float>(victim + 0xA4u);
        float y_off = 1.15f;
        if (zone_byte == 0x08u) y_off = 1.52f;
        else if (zone_byte == 0x07u || zone_byte == 0x09u) y_off = 1.22f;
        else if (zone_byte == 0x0Au) y_off = 0.55f;
        float blood_pos[3] = {G<float>(victim + 0xA0u), feet_y + y_off, G<float>(victim + 0xA8u)};
        float hit_dir[3] = {0.0f, 0.0f, 1.0f};
        if (hit_dir_ea >= mem::kUserLow && mem::IsCommitted(hit_dir_ea) && mem::IsCommitted(hit_dir_ea + 11u)) {
            hit_dir[0] = G<float>(hit_dir_ea);
            hit_dir[1] = G<float>(hit_dir_ea + 4u);
            hit_dir[2] = G<float>(hit_dir_ea + 8u);
        }
        const bool is_lethal = (R32(victim + kCharDying) != 0u);
        game::ballistics::OnEnemyBloodHit(victim, blood_pos, feet_y + 0.008f, y_off,
                                          hit_dir, raw_force, is_headshot, is_lethal);
        if (me && attacker == me) {
            const bool newly_lethal = was_alive_before_hit && is_lethal;
            game::overdrive::OnPlayerBulletPostHit(ctx, base, victim, me, blood_pos, hit_dir,
                                                   is_headshot, newly_lethal);
        }
    }
    game::delirium_phys::OnBulletHit(ctx, base, victim, attacker, hit_dir_ea,
                                     raw_force, zone_byte);
}

// A character's animation event (64-bit hash, then a pointer to the character): the throw takes a grenade
// (0x1E8934), the cheat puts the player's back
X86_EXTERN_FUNC(__imp__sub_001E8830);
X86_FUNC(sub_001E8830) {
    uint32_t who = R32(ctx.esp + 0xC);
    uint32_t me = who ? R32(who) : 0;
    bool keep = game::Opt().infinite_grenades && me && me == Player();
    uint16_t grenades = keep ? G<uint16_t>(me + kCharGrenades) : 0;
    __imp__sub_001E8830(ctx, base);
    if (keep && G<uint16_t>(me + kCharGrenades) < grenades) G<uint16_t>(me + kCharGrenades) = grenades;
}
