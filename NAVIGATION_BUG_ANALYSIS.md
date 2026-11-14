# BLACK Enhanced Edition — Navigation Bug Investigation & Root Cause Analysis

**Repository:** `E:\ai\black`  
**Branch:** `black-enhanced-edition`  
**Target Package:** `com.racas.black.enhanced`  
**Milestone:** Stage 4.1 — Tactical Console UX Redesign & Navigation Repair  
**Status:** Solved & Verified  

---

## 1. Executive Summary

During Stage 4 testing of the in-game Tactical Operations Console, two critical defects were reported by the user:
1. **Category Navigation Reset Bug:** Tapping categories such as Audio, Weapons, and Localization returned to the first tab (Graphics / "main menu"), and the selected category contents were not displayed.
2. **Menu Sizing Defect:** The console window was tiny (~750x450px), cramped, floating in the center of the mobile screen, occupying only ~35% of the display area, with horizontally scrolling tabs that were difficult to target with fingers.

This document details the root cause investigation, the technical mechanics of the failures in Dear ImGui and SDL touch event propagation, and the comprehensive architectural solution implemented in Build 27 (`versionCode: 27`, `versionName: "1.2.1-enhanced-arabic-ui"`).

---

## 2. Root Cause Investigation

### 2.1 The Tab Bar Selection Override Bug

In `src/common/menu.cpp` (lines 1670–1680 prior to Stage 4.1):
```cpp
if (ImGui::BeginTabBar("##tactical_tabs", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_NoTooltip)) {
    for (int t = 0; t < kNumTabs; t++) {
        int i = IsRtl() ? (kNumTabs - 1 - t) : t;
        ImGuiTabItemFlags flags = (active_tab == i) ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem(Ui(kTabNames[i]).c_str(), nullptr, flags)) {
            active_tab = i;
            ImGui::EndTabItem();
        }
    }
    ImGui::EndTabBar();
}
```

#### The Mechanism of Failure:
1. **Dear ImGui Tab Item Selection Semantics:** In Dear ImGui, passing `ImGuiTabItemFlags_SetSelected` tells the tab bar to force-select that tab during the current frame.
2. **Per-Frame Submission:** Because `active_tab` was initially `0` (`TacticalCategory::Graphics`), on every single frame, Tab `0` received `ImGuiTabItemFlags_SetSelected`.
3. **Click Override:** When a user tapped on Tab 3 ("AUDIO") or Tab 7 ("CHEATS & WEAPONS"), Dear ImGui registered the click on that frame. However, during that exact same frame, the loop executed `t=0` first, submitting `ImGuiTabItemFlags_SetSelected` on Tab 0! In Dear ImGui, the programmatic `SetSelected` flag has priority over mouse/touch clicks on sibling tabs.
4. **Immediate Reversion:** Tab 0 was immediately reselected, `BeginTabItem` for Tab 0 returned `true`, resetting `active_tab` back to `0`, and the clicked tab's `BeginTabItem` returned `false`.
5. **Observed Symptom:** To the user, tapping "Audio", "Weapons", or "Localization" appeared to "return to the main menu" (the first tab, Graphics), and the clicked category contents were never drawn.

### 2.2 Sizing & Viewport Scaling Defect

In `src/common/menu.cpp` (lines 1732–1734):
```cpp
ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
ImGui::SetNextWindowSize(ImVec2(em * 54, em * 33), ImGuiCond_Always);
```

#### The Impact on Modern Mobile Displays:
- On the target test device (Xiaomi Redmi Note 14 5G, display resolution `2712 x 1220`):
  - `io.FontGlobalScale` was scaled to `1220 / 720 = ~1.694`.
  - Base font size `14px` yielded `em = ~23.7px`.
  - `em * 54 = ~1280px` width (out of `2712px` screen width = 47%).
  - `em * 33 = ~780px` height (out of `1220px` screen height = 64%).
  - Total screen area was under 35%.
- 8 category tabs distributed across `1280px` (with window padding) gave only ~130px per tab.
- In Dear ImGui, when tab labels (especially translated Arabic phrases like `القتال والتأثيرات` or `الأسلحة والشفرات`) exceeded the available tab bar width, `ImGuiTabBarFlags_FittingPolicyScroll` introduced small arrow scroll buttons `[<]` and `[>]`.
- Tapping near tabs often clipped, missed the hitbox, or touched the window edge, resulting in erratic touches.

### 2.3 Input Propagation & Decoupling

In `native/touch.cpp` (lines 1134–1142):
```cpp
if (e.type == SDL_EVENT_FINGER_DOWN) {
    if (app::MenuOpen() && !g_editing) {
        size_t b = HitButton(x, y);
        if (b == kButtonCount || kButtons[b].press != kPanel) return false;
    }
    FingerDown(e.tfinger.fingerID, x, y);
    return true;
}
```
When `MenuOpen()` is true, touch events are routed directly to `ui::OnEvent(e)` which maps touch coordinates to Dear ImGui mouse events (`io.AddMousePosEvent`, `io.AddMouseButtonEvent`).
However, category button selection lacked typed isolation and stable unique IDs, meaning shaped/BiDi reversed Arabic strings could destabilize string-based tab identifiers.

---

## 3. Architecture of the Solution

### 3.1 Strong Typed Category State Model

Introduced `enum class TacticalCategory` in `src/common/menu.h`:
```cpp
enum class TacticalCategory : int {
    Graphics = 0,
    Combat,
    Characters,
    Audio,
    Localization,
    Gameplay,
    Interface,
    CheatsWeapons,
    Count
};

TacticalCategory GetActiveCategory();
void SetActiveCategory(TacticalCategory cat);
```
Implemented in `src/common/menu.cpp`:
```cpp
TacticalCategory g_active_category = TacticalCategory::Graphics;

TacticalCategory GetActiveCategory() { return g_active_category; }

void SetActiveCategory(TacticalCategory cat) {
    if (int(cat) >= 0 && int(cat) < int(TacticalCategory::Count)) {
        g_active_category = cat;
    }
}
```

### 3.2 Elimination of Fragile TabBar in Favor of Tactical Sidebar

Rather than relying on Dear ImGui's scrollable tab bar, the interface was re-architected into a **Split-Panel Landscape Tactical Console**:
- **Vertical Tactical Sidebar:** A dedicated column displaying 8 tall, comfortable tactical buttons.
- **Minimum Touch Targets:** Each category button has a guaranteed minimum height of `std::max(avail_h / 8, em * 2.2f)` (~52–60dp equivalent).
- **Unique Stable Identifiers:** Every button uses an explicit ID: `###cat_nav_0` through `###cat_nav_7`, completely insulating navigation from translation or BiDi glyph transformations.
- **Direct Assignment:** When clicked, `g_active_category = c.cat;` is assigned directly without intermediate polling or flag conflicts.
- **Active State Feedback:** Active category features a bright brass/gold border (2.5px), theme highlight background, active chevron (`▶ ` in LTR, ` ◀` in RTL), and high-contrast text.

### 3.3 Controller Bumpers (LB / RB) Cyclic Navigation

In addition to direct touch, console navigation seamlessly integrates controller bumpers:
```cpp
if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1)) {
    int next = (int(g_active_category) - 1 + kNumCats) % kNumCats;
    g_active_category = TacticalCategory(next);
}
if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1)) {
    int next = (int(g_active_category) + 1) % kNumCats;
    g_active_category = TacticalCategory(next);
}
```

### 3.4 Responsive Landscape Viewport (~92% x 90%)

The window size is now computed directly from the active viewport work area:
```cpp
float win_w = vp->WorkSize.x * 0.92f;
float win_h = vp->WorkSize.y * 0.90f;
ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
ImGui::SetNextWindowSize(ImVec2(win_w, win_h), ImGuiCond_Always);
```
On a 2712x1220 display, the console occupies ~2495 x 1098 pixels, providing generous spacing, readable typography, and comfortable thumb zones.

---

## 4. Verification & Regression Coverage

| Test Case | Method | Result | Notes |
|---|---|---|---|
| Category Enum Bounds | Native C++ (`TestTacticalCategoryNavigation`) | **PASS** | Validates 8 categories, order, and `Count`. |
| Bumper Wrap-Around | Native C++ | **PASS** | LB from 0 wraps to 7; RB from 7 wraps to 0. |
| Category Persistence | Android UI / Logic | **PASS** | `g_active_category` persists across frames indefinitely. |
| Unique Stable IDs | ImGui Inspection | **PASS** | `###cat_nav_<N>` avoids all BiDi shaping hash collisions. |
| Touch Target Dimensions | Geometry Audit | **PASS** | Sidebar buttons >= 52dp; Steppers >= 52dp; Close >= 56dp. |
| Arabic RTL Mirroring | Runtime Verification | **PASS** | Sidebar places on right side; content on left. |

---

## 5. Conclusion

The navigation bug was caused by a combination of Dear ImGui `ImGuiTabItemFlags_SetSelected` per-frame re-submission and cramped desktop-oriented window dimensions. The redesign completely eliminates the defect by transitioning to a robust, typed state model with a dedicated landscape sidebar navigation system.

