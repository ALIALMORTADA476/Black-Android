#pragma once
#include <string>
#include <string_view>
// Text the recomp adds to the panel and the game's options menu, in the language the game booted in (translations.cpp)

namespace app {

// the text as UTF-8 with each phrase that has a translation (the longest from each word start) in that language, in
// capitals for caps; in English the text as it is. Phrases are the English text in any case.
std::string Ui(std::string_view text, bool caps = true);

// Legacy Xbox Flash UI (Eurostile LT Std) and single-byte guest prompt buffers only support ASCII/Latin glyphs.
// Returns English fallback instead of UTF-8 Arabic when Arabic UI is active so Flash menus never render □ boxes.
std::string FlashUi(std::string_view text, bool caps = true);

}  // namespace app
