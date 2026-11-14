# BLACK Enhanced Edition — Stage 5
# Arabic Subtitle & Objective Overlay System Architecture

## 1. Architectural Overview

In the original BLACK engine (RenderWare 4.1 on original Xbox), all in-game text rendering passes through a legacy 2D text subsystem. This subsystem imposes two fundamental constraints:
1. **127-byte Buffer Limit**: Memory structures allocated for HUD and prompt messages hardcode 128 bytes (`kStringRoom = 128`). Injecting multi-byte UTF-8 into guest pointers causes buffer overflows or memory corruption.
2. **7-Bit Glyph Masking**: Font glyph indexing masks character codes with `(code & 0x7F) * 4`. Any Unicode code point beyond ASCII 127 collides directly with ASCII Latin characters.

### The Host-Rendered Solution
To provide authentic, high-DPI Arabic text with correct contextual shaping and bidirectional layout, BLACK Enhanced Edition bypasses the guest rendering pipeline entirely.

```
+-------------------------------------------------------------------+
|                     GUEST X86 EXECUTION                           |
|  sub_001F3A80(ctx) [Resolves guest string by 32-bit ID]           |
|         |                                                         |
|         +---> Original Latin string preserved in ctx.eax          |
|         |     (Guest 2D engine continues operating normally)      |
|         |                                                         |
|         v                                                         |
|  game::subtitles::OnStringResolved(id, ReadCString(ctx.eax))      |
+-----------------------------------|-------------------------------+
                                    | Thread-Safe IPC / Queue
                                    v
+-------------------------------------------------------------------+
|                     HOST ENHANCED ENGINE                          |
|  1. Binary search string ID in game::FindArabicString(id)         |
|  2. Debounce duplicate requests (< 2000ms)                        |
|  3. Categorize into Objective, Prompt, Intel, or Subtitle         |
|  4. Word-boundary wrap at 48-52 characters: arabic::WrapText()    |
|  5. Contextual Shaping + BiDi Reordering: arabic::ShapeAndBidi()  |
|  6. Calculate smooth alpha fade-in (250ms) and fade-out (450ms)   |
|                                                                   |
|  VulkanBackend::RenderUi() -> app::DrawUi()                       |
|         |                                                         |
|         v                                                         |
|  game::subtitles::DrawOverlay(screen_w, screen_h)                 |
|  [Rendered via Dear ImGui + Noto Naskh Arabic Vector Font]        |
+-------------------------------------------------------------------+
```

---

## 2. Component Implementation Details

### 2.1. Interception Hook (`src/game/prompts.cpp`)
The hook is situated directly in the recompilation bridge `sub_001F3A80`. It extracts the 32-bit string ID and the resolved ASCII/UTF-8 string without altering guest register state:

```cpp
X86_FUNC(sub_001F3A80) {
    uint32_t id = R32(ctx.esp + 4);
    __imp__sub_001F3A80(ctx, base);
    
    // Existing early exits (OptionsText, MenuText, SaveName)
    ...
    if (!ctx.eax) return;

    // Enhanced Edition Hook: Safe notification to host overlay
    // Guest ctx.eax is NEVER mutated with multi-byte Arabic UTF-8!
    game::subtitles::OnStringResolved(id, ReadCString(ctx.eax, 1024).c_str());

    // Legacy guest wording flow continues...
}
```

### 2.2. Dual-Layer Visual Presentation (`src/game/subtitles.cpp`)
The system distinguishes between two primary categories of in-game messages:

#### A. Primary Mission Objectives (Top Banner)
- **Position**: Centered at top of screen (`Y = 24dp` to `Y = 72dp`).
- **Visual Styling**:
  - Border: Tactical Gold/Brass (`#D4AF37`, `RGBA: 0.85, 0.68, 0.22, alpha`).
  - Background: Dark Translucent Military Charcoal (`RGBA: 0.08, 0.08, 0.10, 0.90 * alpha`).
  - Header: Small golden tactical tag (`تحديث المهمة الرئيسية` or `PRIMARY OBJECTIVE UPDATE`).
  - Text: Large, shaped Arabic text with high legibility.
- **Duration**: 6.0 seconds with smooth 250ms fade-in and 450ms fade-out.

#### B. Subtitles, Intel & Prompts (Bottom Box)
- **Position**: Centered horizontally at bottom of screen (`Y = screen_h - 130dp`).
- **Visual Styling**:
  - Border: Cyber Tactical Cyan (`RGBA: 0.25, 0.70, 0.90, alpha`).
  - Background: Semi-transparent Black (`RGBA: 0.05, 0.05, 0.07, 0.82 * alpha`).
  - Header: Source attribution (e.g., `معلومات بلاك التكتيكية` or `مكالمة لاسلكية`).
  - Padding: `16dp` horizontal, `10dp` vertical.
- **Duration**: 4.0 to 6.5 seconds depending on message category.

### 2.3. Anti-Flicker & Debounce Logic
Many game loops query active strings every frame or multiple times per second. To prevent UI flickering and message restarts:
```cpp
// Debounce duplicate queries within 2.0 seconds
{
    std::lock_guard lock(g_lock);
    if (id == g_last_string_id && (now - g_last_id_time) < 2000) {
        return;
    }
}
```

### 2.4. Smooth Alpha Blending
Alpha interpolation guarantees smooth transitions between states:
```cpp
float CalculateAlpha(int64_t now, int64_t started, int64_t expires) {
    if (now >= expires) return 0.0f;
    constexpr int64_t kFadeInMs = 250;
    constexpr int64_t kFadeOutMs = 450;
    if (now - started < kFadeInMs) {
        return float(now - started) / float(kFadeInMs);
    }
    if (expires - now < kFadeOutMs) {
        return float(expires - now) / float(kFadeOutMs);
    }
    return 1.0f;
}
```

---

## 3. Real-Time Mod Menu Integration

Subtitles are governed by the typed settings core:
- **Registry Setting**: `SettingId::ArabicSubtitlesOverlay`
- **Field**: `enhanced.arabic_subtitles_overlay`
- **Default**: `true`
- **Scope**: `ApplyScope::Immediate` (No game restart required)

When toggled off via the Tactical Ops Console:
1. `enhanced.arabic_subtitles_overlay` is updated immediately.
2. `game::subtitles::Clear()` purges any currently active or queued subtitles from the screen.
3. Subtitle rendering stops immediately on the next frame.

