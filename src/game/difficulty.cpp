// The cheats page's mission unlocks. The save keeps, per difficulty, which of the 8 missions are open; the game opens
// a mission on Black Ops once it was beaten on hard, the next mission once one is beaten. UNLOCK ALL MISSIONS opens
// every mission on the difficulties shown, UNLOCK BLACK OPS opens on Black Ops each mission open on another one and
// has the difficulty select offer it. A mission stays open while its cheat is on: loading one reads the table in many
// places, and a mission closed again under them hung the load. Turned off, the cheat closes what it opened; a save
// written while it was on keeps them open.
#include <vector>

#include "common/options.h"
#include "game/layout.h"
#include "game/mission_select_rules.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// mission table: 8 missions of 0x20 bytes a difficulty, the difficulties 0x100 apart
constexpr uint32_t kMissionOpen = 0x2D0D9E;  // byte: 1 open
constexpr uint32_t kMissionSize = 0x20;
constexpr uint32_t kDifficultySize = 0x100;
constexpr uint32_t kMissions = 8;
constexpr uint32_t kBlackOps = 3;  // easy 0 to black ops 3

// as many difficulties as the difficulty select offers: 3 when no mission was picked on the level select, else as
// many as the mission is open on
constexpr uint32_t kDifficultySelection = 0x2EC560;

uint32_t Open(uint32_t difficulty, uint32_t mission) {
    return kMissionOpen + difficulty * kDifficultySize + mission * kMissionSize;
}

// the missions each cheat opened, closed again when it goes off
std::vector<uint32_t> g_levels;
std::vector<uint32_t> g_black_ops;

void Set(uint32_t at, std::vector<uint32_t>& opened) {
    if (G<uint8_t>(at)) return;
    G<uint8_t>(at) = 1;
    opened.push_back(at);
}

void Close(std::vector<uint32_t>& opened) {
    // a byte the game set to something else since is the game's
    for (uint32_t at : opened)
        if (G<uint8_t>(at) == 1) G<uint8_t>(at) = 0;
    opened.clear();
}

// the table as the cheats want it, before the game reads it
void Unlock() {
    const game::Options& o = game::Opt();
    if (!o.unlock_black_ops) Close(g_black_ops);
    if (!o.unlock_levels) Close(g_levels);
    for (uint32_t m = 0; m < kMissions; m++) {
        bool any = false;
        for (uint32_t d = 0; d < kBlackOps; d++) {
            if (o.unlock_levels) Set(Open(d, m), g_levels);
            any |= G<uint8_t>(Open(d, m)) == 1;
        }
        if (o.unlock_black_ops && any) Set(Open(kBlackOps, m), g_black_ops);
    }
}

// the count the cheat raised, given back when it goes off. The difficulty select's bindings set 3 once at boot and
// the level select a count when a mission is picked, so the cheat is kept there every tick of the front end too: on
// from the options, it shows without a restart
bool g_raised = false;
uint32_t g_count = 0;

void OfferBlackOps() {
    uint32_t count = R32(kDifficultySelection);
    if (game::Opt().unlock_black_ops) {
        if (count == kBlackOps + 1) return;
        g_count = count;
        g_raised = true;
        W32(kDifficultySelection, kBlackOps + 1);
    } else if (g_raised) {
        g_raised = false;
        if (count == kBlackOps + 1) W32(kDifficultySelection, g_count);
    }
}

}  // namespace

// the level select's flags and the difficulty select's ticks, by the difficulty in play; the front end's every tick
X86_EXTERN_FUNC(__imp__sub_00115030);
X86_FUNC(sub_00115030) {
    constexpr uint32_t kSelectedSlot = 0x2AC320;
    constexpr uint32_t kFeLevelUnlocked = 0x2EC4A4;
    constexpr uint32_t kMissionBeaten = 0x2D0D84;  // dword == 2 when mission beaten on that difficulty
    constexpr uint32_t kPostCompleteA = 0x2F412C;
    constexpr uint32_t kPostCompleteB = 0x2F412D;
    constexpr uint32_t kTrophyMask = 0x2EC598;
    constexpr uint32_t kGameDifficulty = 0x2014C;
    constexpr uint32_t kGameLevelId = 0x2020C;

    const bool unlock_levels = game::Opt().unlock_levels;
    if (unlock_levels) Close(g_levels);  // inspect organic kMissionOpen before re-opening

    const uint32_t game_ptr = R32(layout::kGame);
    const uint32_t cur_diff = game_ptr ? (R32(game_ptr + kGameDifficulty) < game::mission::kDifficultyCount
                                              ? R32(game_ptr + kGameDifficulty)
                                              : 0u)
                                       : 0u;
    const uint8_t active_level_id = game_ptr ? G<uint8_t>(game_ptr + kGameLevelId) : 0u;
    const uint32_t selected_slot = R32(kSelectedSlot);
    const bool selection_unset =
        (selected_slot == 0xFFFFFFFFu) && (G<uint8_t>(kPostCompleteA) == 0u) && (G<uint8_t>(kPostCompleteB) == 0u);

    bool open_for_diff[game::mission::kMissionCount] = {};
    bool beaten_any[game::mission::kMissionCount] = {};
    for (uint32_t m = 0; m < game::mission::kMissionCount; m++) {
        open_for_diff[m] = G<uint8_t>(Open(cur_diff, m)) != 0u;
        for (uint32_t d = 0; d <= kBlackOps; d++) {
            if (R32(kMissionBeaten + d * kDifficultySize + m * kMissionSize) == 2u) {
                beaten_any[m] = true;
            }
        }
    }

    __imp__sub_00115030(ctx, base);
    Unlock();
    OfferBlackOps();

    if (unlock_levels) {
        uint32_t ui_flags[game::mission::kMissionCount] = {};
        uint32_t focus = 0u;
        for (uint32_t m = 0; m < game::mission::kMissionCount; m++) {
            ui_flags[m] = R32(kFeLevelUnlocked + m * 4u);
            if (ui_flags[m] == 2u) focus = m;
        }
        if (selection_unset) {
            focus = game::mission::ResolveDefaultMissionIndex(open_for_diff, beaten_any, active_level_id, true);
            if (game_ptr && selected_slot == 0xFFFFFFFFu) {
                G<uint8_t>(game_ptr + kGameLevelId) = game::mission::LevelIdFromMissionIndex(focus);
            }
            uint32_t stars = 0u;
            for (uint32_t d = 0; d <= kBlackOps; d++) {
                if (R32(kMissionBeaten + d * kDifficultySize + focus * kMissionSize) == 2u) {
                    stars |= (1u << d);
                }
            }
            W32(kTrophyMask, stars);
        }
        game::mission::ApplyMissionUnlockUiFlags(ui_flags, focus, true);
        for (uint32_t m = 0; m < game::mission::kMissionCount; m++) {
            W32(kFeLevelUnlocked + m * 4u, ui_flags[m]);
        }
    }
}

// the level select's choice of mission: its ticks and the count of difficulties open on it
X86_EXTERN_FUNC(__imp__sub_0011B890);
X86_FUNC(sub_0011B890) {
    Unlock();
    __imp__sub_0011B890(ctx, base);
    OfferBlackOps();
}

// the difficulty select's bindings, the count among them
X86_EXTERN_FUNC(__imp__sub_00124F30);
X86_FUNC(sub_00124F30) {
    Unlock();
    __imp__sub_00124F30(ctx, base);
    OfferBlackOps();
}

// the mission starts on the difficulty chosen, nothing when it is not open there
X86_EXTERN_FUNC(__imp__sub_00124CF0);
X86_FUNC(sub_00124CF0) {
    Unlock();
    __imp__sub_00124CF0(ctx, base);
}
