#include "game/mission_select_rules.h"

#include <algorithm>

namespace game::mission {

uint8_t LevelIdFromMissionIndex(uint32_t index) {
    return (index < kMissionCount) ? kMissionLevelIds[index] : kMissionLevelIds[0];
}

uint32_t MissionIndexFromLevelId(uint8_t level_id) {
    for (uint32_t i = 0; i < kMissionCount; i++) {
        if (kMissionLevelIds[i] == level_id) return i;
    }
    return 0;
}

uint32_t ResolveDefaultMissionIndex(const bool open_for_diff[kMissionCount],
                                    const bool beaten_any_diff[kMissionCount],
                                    uint8_t active_level_id,
                                    bool unlock_levels) {
    uint32_t organic_highest = 0;
    bool all_open = true;
    for (uint32_t m = 0; m < kMissionCount; m++) {
        if (open_for_diff && open_for_diff[m]) {
            organic_highest = m;
        } else {
            all_open = false;
        }
    }
    if (!unlock_levels || !all_open) {
        return organic_highest;
    }

    // When unlock_levels opened all 8 slots (or a prior save persisted all-open flags),
    // prefer actual campaign progress (next un-beaten mission or active campaign level)
    // instead of defaulting straight to Mission 8 (Spetriniv Gulag).
    uint32_t next_after_beaten = 0;
    for (uint32_t m = 0; m < kMissionCount; m++) {
        if (beaten_any_diff && beaten_any_diff[m]) {
            next_after_beaten = std::min(m + 1u, kMissionCount - 1u);
        }
    }
    const uint32_t active_idx = MissionIndexFromLevelId(active_level_id);
    return std::max(next_after_beaten, active_idx);
}

void ApplyMissionUnlockUiFlags(uint32_t fe_level_unlocked[kMissionCount],
                                 uint32_t focus_index,
                                 bool unlock_levels) {
    if (!fe_level_unlocked) return;
    const uint32_t clamped_focus = (focus_index < kMissionCount) ? focus_index : 0u;
    for (uint32_t m = 0; m < kMissionCount; m++) {
        if (m == clamped_focus) {
            fe_level_unlocked[m] = 2u;
        } else if (unlock_levels || fe_level_unlocked[m] != 0u) {
            fe_level_unlocked[m] = 1u;
        }
    }
}

MapClickDecision ResolveMapTouchClick(bool has_active_map,
                                      uint32_t map_value_at_point,
                                      uint32_t active_map_value,
                                      bool has_button_clips) {
    if (map_value_at_point != 0u) {
        if (active_map_value != 0u && map_value_at_point == active_map_value) {
            return {0u, kNavConfirm};
        }
        return {map_value_at_point, kNavNone};
    }
    // Maps such as MissionModeSelect have a single PressXMenuButton without a dedicated hit box;
    // do not turn background taps away from mission nodes into an accidental confirm.
    if (has_active_map) {
        return {0u, kNavNone};
    }
    if (!has_button_clips) {
        return {0u, kNavConfirm};
    }
    return {0u, kNavNone};
}

uint32_t MapSwipeToInput(bool has_active_map, bool has_vertical_links, int hwheel, int vwheel) {
    if (has_active_map) {
        if (hwheel != 0) return (hwheel > 0) ? kNavRight : kNavLeft;
        if (vwheel != 0) {
            if (has_vertical_links) return (vwheel > 0) ? kNavUp : kNavDown;
            return (vwheel < 0) ? kNavRight : kNavLeft;
        }
        return kNavNone;
    }
    if (vwheel != 0) return (vwheel > 0) ? kNavUp : kNavDown;
    return kNavNone;
}

}  // namespace game::mission
