#pragma once
#include <cstdint>
#include <string_view>

namespace game {

enum class StringCategory {
    Unknown,
    Objective,
    Prompt,
    Intel,
    Subtitle,
    Hud
};

struct ArabicStringEntry {
    uint32_t id;
    const char* english;
    const char* arabic_u8;
    const char* context;
    const char* category;
};

// Returns pointer to entry if found, nullptr otherwise
const ArabicStringEntry* FindArabicString(uint32_t id);

// Convenience functions
const char* ArabicText(uint32_t id);
const char* EnglishText(uint32_t id);
StringCategory CategoryOf(uint32_t id);

}  // namespace game

