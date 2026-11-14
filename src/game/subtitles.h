#pragma once
#include <cstdint>
#include <string>

namespace game {

namespace subtitles {

// Initializes subtitle engine
void Init();

// Called from guest string hook sub_001F3A80 whenever a string is queried
void OnStringResolved(uint32_t id, const char* guest_text);

// Renders the host Arabic text overlay (objectives at top, subtitles/prompts at bottom)
void DrawOverlay(float screen_w, float screen_h);

// Clears active subtitles and objective notifications
void Clear();

// Direct trigger API for testing and synthetic subtitles
void ShowSubtitle(const char* text_en, const char* text_ar = nullptr, float duration_sec = 5.0f);
void ShowObjective(const char* header, const char* text_en, const char* text_ar = nullptr, float duration_sec = 6.0f);

}  // namespace subtitles

}  // namespace game

