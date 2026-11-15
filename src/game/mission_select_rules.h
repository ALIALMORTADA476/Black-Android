#pragma once

#include <cstdint>

namespace game::mission {

constexpr uint32_t kMissionCount = 8;
constexpr uint32_t kDifficultyCount = 4;

// Mirrors default.xbe VA 0x288648[0..7]: maps UI mission index (0..7) -> engine level ID.
constexpr uint8_t kMissionLevelIds[kMissionCount] = {0, 1, 5, 3, 4, 6, 7, 8};

// Pure MenuInput mirror (matches layout::MenuInput without requiring xdk/hle.h in unit tests).
constexpr uint32_t kNavNone = 0;
constexpr uint32_t kNavUp = 1;
constexpr uint32_t kNavDown = 2;
constexpr uint32_t kNavLeft = 3;
constexpr uint32_t kNavRight = 4;
constexpr uint32_t kNavConfirm = 5;

/// Converts UI mission index (0..7) into the engine level ID (0x288648[idx]).
uint8_t LevelIdFromMissionIndex(uint32_t index);

/// Converts an engine level ID ([game + 0x2020C]) back to UI mission index (0..7), defaulting to 0.
uint32_t MissionIndexFromLevelId(uint8_t level_id);

/// Resolves which mission (0..7) should be focused initially when entering MissionModeSelect
/// while keeping unlock_levels enabled from defaulting incorrectly to Mission 8 (Spetriniv Gulag).
uint32_t ResolveDefaultMissionIndex(const bool open_for_diff[kMissionCount],
                                    const bool beaten_any_diff[kMissionCount],
                                    uint8_t active_level_id,
                                    bool unlock_levels);

/// Ensures exactly one slot (focus_index) is marked active (2) and unlocked/cheat slots are marked 1.
void ApplyMissionUnlockUiFlags(uint32_t fe_level_unlocked[kMissionCount],
                                 uint32_t focus_index,
                                 bool unlock_levels);

struct MapClickDecision {
    uint32_t aim_value = 0;
    uint32_t input = kNavNone;
};

/// Resolves a pointer click on a page that may host a BkValueList map (MissionModeSelect MyMap).
/// Avoids confirming via the single-button !clips shortcut when tapping empty map space.
MapClickDecision ResolveMapTouchClick(bool has_active_map,
                                      uint32_t map_value_at_point,
                                      uint32_t active_map_value,
                                      bool has_button_clips);

/// Converts horizontal and vertical swipe wheel notches into directional MenuInput events.
uint32_t MapSwipeToInput(bool has_active_map, bool has_vertical_links, int hwheel, int vwheel);

}  // namespace game::mission
