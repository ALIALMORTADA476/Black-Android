#include "game/arabic_strings.h"
#include <algorithm>
#include <cstring>
#include <iterator>

namespace game {

namespace {

constexpr ArabicStringEntry kArabicEntries[] = {
#include "game/strings_ara.inc"
};

}  // namespace

const ArabicStringEntry* FindArabicString(uint32_t id) {
    const ArabicStringEntry* at = std::lower_bound(
        std::begin(kArabicEntries), std::end(kArabicEntries), id,
        [](const ArabicStringEntry& e, uint32_t val) { return e.id < val; });
    return (at != std::end(kArabicEntries) && at->id == id) ? at : nullptr;
}

const char* ArabicText(uint32_t id) {
    const ArabicStringEntry* e = FindArabicString(id);
    return e ? e->arabic_u8 : nullptr;
}

const char* EnglishText(uint32_t id) {
    const ArabicStringEntry* e = FindArabicString(id);
    return e ? e->english : nullptr;
}

StringCategory CategoryOf(uint32_t id) {
    const ArabicStringEntry* e = FindArabicString(id);
    if (!e || !e->category) return StringCategory::Unknown;
    if (std::strcmp(e->category, "objective") == 0) return StringCategory::Objective;
    if (std::strcmp(e->category, "prompt") == 0) return StringCategory::Prompt;
    if (std::strcmp(e->category, "intel") == 0) return StringCategory::Intel;
    if (std::strcmp(e->category, "subtitle") == 0) return StringCategory::Subtitle;
    if (std::strcmp(e->category, "hud") == 0) return StringCategory::Hud;
    return StringCategory::Unknown;
}

}  // namespace game

