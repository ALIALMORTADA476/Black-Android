// the cheats panel, on the touch controls' gear button or F10
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "app/app.h"
#include "audio/output.h"
#include "common/keys.h"
#include "common/menu.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "common/translations.h"
#include "common/mod_schema.h"
#include "common/presets.h"
#include "game/physics_lab.h"
#include "game/subtitles.h"
#include "game/weapons.h"

// native/gyro.cpp: aiming by turning the phone
namespace gyro {
uint32_t CurrentMode();
const char* ModeName();
void SetMode(uint32_t mode);
float Sensitivity();
void SetSensitivity(float s);
bool InvertX();
bool InvertY();
void SetInvert(bool x, bool y);
}  // namespace gyro

// native/touch.cpp: the touch controls' layout
namespace touch {
const char* LayoutName();
void StepLayout(int step);
uint32_t ControlsOpacity();
void StepOpacity(int step);
uint32_t ControlsSize();
void StepSize(int step);
void EditLayout();
const char* SkinName();
void StepSkin(int step);
const char* StickModeName();
void StepStickMode(int step);
}  // namespace touch

// native/dynamic_resolution.cpp: the render resolution in use, percent of the screen's
namespace dynres {
uint32_t Percent();
}  // namespace dynres

// native/update.cpp: the activity asks GitHub for a newer release and says what it found
namespace host {
void CheckForUpdates();
bool StartupCheck();  // whether each launch asks GitHub
void SetStartupCheck(bool on);
}  // namespace host

namespace game::flash_arabic {
void DrawOverlay(float screen_w, float screen_h);
}  // namespace game::flash_arabic

namespace app {

Settings DefaultSettings();  // native/main.cpp: a phone's first run, unlimited frame rate and vsync off

namespace {

std::atomic<bool> g_open{false};

// Tactical High-Contrast Obsidian & Cyber Gold Palette
constexpr ImVec4 kPanelBg{0.06f, 0.08f, 0.12f, 0.98f};       // Deep Slate Obsidian #0F141F (98% opaque, blocks background clutter)
constexpr ImVec4 kBorder{1.0f, 0.82f, 0.20f, 0.90f};         // Vivid Cyber Gold #FFD133
constexpr ImVec4 kText{1.0f, 1.0f, 1.0f, 1.0f};              // Crisp 100% Pure White #FFFFFF
constexpr ImVec4 kHighlight{0.75f, 0.58f, 0.10f, 0.40f};     // Tactical Gold Highlight
constexpr ImVec4 kHeading{1.0f, 0.82f, 0.20f, 1.0f};         // Vivid Cyber Gold Heading #FFD133
constexpr ImVec4 kDim{0.75f, 0.82f, 0.92f, 1.0f};             // Bright Cool Steel / Cyber Silver #C0D1EB
constexpr ImVec4 kTab{0.12f, 0.16f, 0.24f, 1.0f};             // Deep Armor Slate #1F293D
constexpr ImVec4 kTabHovered{0.20f, 0.28f, 0.42f, 1.0f};      // Hover Slate Accent #33476B
constexpr ImVec4 kButton{0.14f, 0.19f, 0.28f, 1.0f};          // Tactile Armor Button #243047
constexpr ImVec4 kButtonHovered{0.25f, 0.35f, 0.52f, 1.0f};   // Vivid Action Blue/Slate #405985
constexpr ImVec4 kLine{1.0f, 0.82f, 0.20f, 0.40f};            // Gold Accent Separator Line

struct TacticalTheme {
    ImVec4 heading;
    ImVec4 border;
    ImVec4 highlight;
    ImVec4 line;
    ImVec4 button;
    ImVec4 button_hovered;
};

inline TacticalTheme CurrentTheme() {
    std::string_view accent = modsettings::Registry().GetEnhanced().theme_accent;
    if (accent == "crimson") {
        return {
            ImVec4(1.0f, 0.30f, 0.28f, 1.0f),       // Vivid Crimson #FF4D47
            ImVec4(0.95f, 0.25f, 0.22f, 0.90f),
            ImVec4(0.50f, 0.14f, 0.12f, 0.50f),
            ImVec4(1.0f, 0.30f, 0.28f, 0.45f),
            ImVec4(0.22f, 0.10f, 0.12f, 1.0f),
            ImVec4(0.40f, 0.16f, 0.18f, 1.0f)
        };
    }
    if (accent == "olive") {
        return {
            ImVec4(0.70f, 0.92f, 0.38f, 1.0f),       // Vivid Tactical Green #B3EB61
            ImVec4(0.60f, 0.82f, 0.30f, 0.90f),
            ImVec4(0.28f, 0.40f, 0.14f, 0.50f),
            ImVec4(0.70f, 0.92f, 0.38f, 0.45f),
            ImVec4(0.14f, 0.20f, 0.12f, 1.0f),
            ImVec4(0.26f, 0.38f, 0.20f, 1.0f)
        };
    }
    // Default: Tactical Cyber Gold (Vivid, High-Contrast)
    return {
        ImVec4(1.0f, 0.82f, 0.20f, 1.0f),           // #FFD133
        ImVec4(1.0f, 0.80f, 0.18f, 0.90f),         // #FFCC2E
        ImVec4(0.75f, 0.58f, 0.10f, 0.40f),
        ImVec4(1.0f, 0.82f, 0.20f, 0.45f),
        ImVec4(0.14f, 0.19f, 0.28f, 1.0f),         // #243047
        ImVec4(0.25f, 0.35f, 0.52f, 1.0f)          // #405985
    };
}

TacticalCategory g_active_category = TacticalCategory::Graphics;

TacticalCategory GetActiveCategory() {
    return g_active_category;
}

void SetActiveCategory(TacticalCategory cat) {
    if (int(cat) >= 0 && int(cat) < int(TacticalCategory::Count)) {
        g_active_category = cat;
    }
}

constexpr uint32_t kAnisoValues[] = {0, 2, 4, 8, 16};
const char* const kAnisoNames[] = {"GAME DEFAULT", "2x", "4x", "8x", "16x"};
constexpr uint32_t kDistances[] = {1, 2, 4, 8};
const char* const kDistanceNames[] = {"GAME DEFAULT", "2x", "4x", "8x"};
constexpr uint32_t kAntiAliasings[] = {0, 1, 2};
const char* const kAntiAliasingNames[] = {"OFF", "FXAA", "SSAA 2X"};
constexpr uint32_t kFmvScales[] = {0, 1};
const char* const kFmvScaleNames[] = {"FILL", "4:3"};
constexpr uint32_t kScreenFilters[] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
const char* const kScreenFilterNames[] = {"OFF",  "CRT LIGHT", "CRT STRONG",   "SHARPEN", "VIBRANT",
                                          "NOIR", "SEPIA",     "NIGHT VISION", "VHS"};
constexpr uint32_t kAudioOutputs[] = {audio::kOutputAuto, audio::kOutputMono, audio::kOutputStereo,
                                      audio::kOutputSurround};
const char* const kAudioOutputNames[] = {"AUTOMATIC", "MONO", "2.0 STEREO", "5.1 SURROUND"};
const char* const kLanguageNames[] = {"ENGLISH", "FRENCH", "GERMAN", "ITALIAN", "SPANISH",
                                      "INDONESIAN", "ARABIC"};  // as game::kLanguages
constexpr uint32_t kFpsCaps[] = {30, 60, 75, 90, 120, 144, 165, 240, 360, kFpsCapRefresh, 0};

int StepIndex(int index, int step, int count) {
    if (count <= 0) return 0;
    index = (index + step) % count;
    return index < 0 ? index + count : index;
}

int NearestIndex(const uint32_t* values, int count, uint32_t value) {
    int best = 0;
    uint32_t best_d = UINT32_MAX;
    for (int i = 0; i < count; i++) {
        uint32_t d = values[i] > value ? values[i] - value : value - values[i];
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

// index nearest value, stepped first
template <size_t N>
int StepTable(const uint32_t (&values)[N], uint32_t& value, int step) {
    int i = NearestIndex(values, int(N), value);
    if (step) {
        i = StepIndex(i, step, int(N));
        value = values[i];
    }
    return i;
}

// value clamped to lo to hi, stepped by inc through the values StepIndex wraps over
int StepRange(int value, int lo, int hi, int inc, int step) {
    int count = (hi - lo) / inc + 1;
    return lo + inc * StepIndex((std::clamp(value, lo, hi) - lo) / inc, step, count);
}

inline bool IsRtl() {
    auto& reg = modsettings::Registry();
    return (game::Opt().language == "ara" || reg.GetEnhanced().arabic_ui_enabled) && reg.GetEnhanced().arabic_rtl_mirror;
}

void Section(const char* title) {
    float em = ImGui::GetFontSize();
    ImGui::Dummy(ImVec2(0, em * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, CurrentTheme().heading);
    std::string shaped_title = Ui(title);
    if (IsRtl()) {
        float text_w = ImGui::CalcTextSize(shaped_title.c_str()).x;
        float avail_w = ImGui::GetContentRegionAvail().x;
        if (avail_w > text_w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail_w - text_w);
    }
    ImGui::TextUnformatted(shaped_title.c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, em * 0.2f));
}

constexpr float kValueEms = 12.0f;  // the value column's width

// a row's place and size: the label left (or right in RTL), the value block right (or left in RTL)
struct RowBox {
    float em;
    float row_w;
    float row_h;
    float value_w;
    float block_w;
    float row_x;
    float row_y;
};

RowBox MeasureRow() {
    const ImGuiStyle& style = ImGui::GetStyle();
    RowBox box;
    box.em = ImGui::GetFontSize();
    box.row_w = ImGui::GetContentRegionAvail().x;
    box.row_h = std::max(ImGui::GetFrameHeight(), box.em * 2.2f);
    box.value_w = box.em * kValueEms;
    box.block_w = box.row_h * 2 + box.value_w + style.ItemSpacing.x * 2;
    box.row_x = ImGui::GetCursorPosX();
    box.row_y = ImGui::GetCursorPosY();
    return box;
}

void RowLabel(const RowBox& box, const char* label) {
    const ImGuiStyle& style = ImGui::GetStyle();
    std::string text = Ui(label);
    float y_offset = (box.row_h - ImGui::GetTextLineHeight()) * 0.5f;
    if (IsRtl()) {
        // Mirrored RTL layout: right-aligned label on the right side
        float text_w = ImGui::CalcTextSize(text.c_str()).x;
        float pos_x = box.row_x + box.row_w - text_w - style.ItemInnerSpacing.x;
        ImGui::SetCursorPos(ImVec2(std::max(pos_x, box.row_x + box.block_w), box.row_y + y_offset));
    } else {
        // Standard LTR layout: left-aligned label on the left side
        ImGui::SetCursorPos(ImVec2(box.row_x + style.ItemInnerSpacing.x, box.row_y + y_offset));
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text.c_str());
}

// the cursor back to the row's start, then past the whole row
void EndRow(const RowBox& box) {
    ImGui::SetCursorPos(ImVec2(box.row_x, box.row_y));
    ImGui::Dummy(ImVec2(box.row_w, box.row_h));
}

void FillItem() {
    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                              ImGui::GetColorU32(ImGuiCol_Header));
}

// label left/right, arrows and value right/left, the row is the only nav stop
int OptionRow(const char* label, const char* value) {
    const ImGuiStyle& style = ImGui::GetStyle();
    RowBox box = MeasureRow();
    ImGui::PushID(label);
    int step = 0;
    ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(box.row_w, box.row_h));
    if (ImGui::IsItemFocused()) {
        FillItem();
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)) step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)) step = 1;
    }
    RowLabel(box, label);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav | ImGuiItemFlags_ButtonRepeat, true);  // held arrows keep stepping

    // In RTL, the value stepper block is placed on the LEFT side of the row; in LTR, on the RIGHT
    float block_x = IsRtl() ? box.row_x : (box.row_x + box.row_w - box.block_w);
    float btn_sz = box.row_h;
    float y_offset = (box.row_h - ImGui::GetTextLineHeight()) * 0.5f;

    ImGui::SetCursorPos(ImVec2(block_x, box.row_y));
    if (ImGui::Button("<##dec", ImVec2(btn_sz, btn_sz))) step = -1;

    float value_x = block_x + btn_sz + style.ItemSpacing.x;
    std::string shaped_value = Ui(value);
    float pad = (box.value_w - ImGui::CalcTextSize(shaped_value.c_str()).x) * 0.5f;
    ImGui::SetCursorPos(ImVec2(value_x + std::max(pad, 0.0f), box.row_y + y_offset));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(shaped_value.c_str());

    ImGui::SetCursorPos(ImVec2(value_x + box.value_w + style.ItemSpacing.x, box.row_y));
    if (ImGui::Button(">##inc", ImVec2(btn_sz, btn_sz))) step = 1;

    ImGui::PopItemFlag();
    ImGui::PopID();
    EndRow(box);
    return step;
}

bool IsRestartPending(modsettings::SettingId id) {
    auto& reg = modsettings::Registry();
    std::string boot = reg.BootValueString(id);
    std::string cur = reg.GetString(id, app::Get(), game::Opt());
    return !boot.empty() && boot != cur;
}

const char* OnOff(bool on) { return on ? "ON" : "OFF"; }

void Flip(bool& on, int step) {
    if (step) on = !on;
}

// applies and saves
void Changed() {
    audio::SetOutputMode(game::Opt().audio_output);
    game::physics_lab::SyncFromEnhanced(modsettings::Registry().GetEnhanced());
    Save();
}

enum RowId { kRenderResolution, kMaxFps, kVsync, kFpsCounter, kAniso, kReloadBlur, kSkipIntros, kSensitivity,
             kInvertMouse, kWalkSpeed, kFov, kViewmodelFov, kHoldAim, kDrawDistance, kAudioOutput, kLanguage,
             kFmvScale, kGyro, kGyroSensitivity, kTouchLayout, kTouchOpacity, kTouchSize, kTouchEdit,
             kInfiniteAmmo, kRapidFire, kNoRecoil, kGameSpeed, kGodMode, kOneHitKill, kInfiniteGrenades,
             kCheckUpdates, kTouchSensitivity, kLeftDeadzone, kRightDeadzone, kLeftSensitivity, kRightSensitivity,
             kScreenFilter, kGyroInvertX, kGyroInvertY, kUnlockLevels, kUnlockBlackOps, kTouchSkin, kCredit, kBloom,
             kFog, kAntiAliasing, kHdr, kEsp, kAimbot, kTouchStick, kDynamicRes, kDynamicTarget, kDynamicMin,
             kDynamicMax, kDynamicHold, kAutoUpdate };

using rows::Kind;
using rows::Row;

// the recomp's settings in pages of their own, grouped within; the game's options menu shows the headings too
constexpr Row kGraphicsRows[] = {
    {Kind::Heading, -1, "DISPLAY"},
    {Kind::Value, kRenderResolution, "RENDER RESOLUTION"},
    {Kind::Value, kFov, "FIELD OF VIEW"},
    {Kind::Value, kViewmodelFov, "VIEWMODEL FIELD OF VIEW"},
    {Kind::Value, kFmvScale, "FMV SCALE"},
    {Kind::Value, kScreenFilter, "SCREEN FILTER"},
    {Kind::Heading, -1, "PERFORMANCE"},
    {Kind::Value, kVsync, "VERTICAL SYNC"},
    {Kind::Value, kMaxFps, "MAX FRAME RATE"},
    {Kind::Value, kFpsCounter, "FPS COUNTER"},
    {Kind::Value, kDynamicRes, "DYNAMIC RESOLUTION"},
    {Kind::Value, kDynamicTarget, "TARGET FRAME RATE"},
    {Kind::Value, kDynamicMin, "MIN RESOLUTION"},
    {Kind::Value, kDynamicMax, "MAX RESOLUTION"},
    {Kind::Value, kDynamicHold, "HOLD BEFORE RAISING"},
    {Kind::Heading, -1, "QUALITY"},
    {Kind::Value, kAniso, "ANISOTROPIC FILTERING"},
    {Kind::Value, kDrawDistance, "DRAW DISTANCE"},
    {Kind::Value, kReloadBlur, "RELOAD BLUR"},
    {Kind::Value, kBloom, "BLOOM"},
    {Kind::Value, kFog, "FOG"},
    {Kind::Value, kAntiAliasing, "ANTI-ALIASING"},
    {Kind::Value, kHdr, "HDR"},
};

constexpr Row kTouchRows[] = {
    {Kind::Heading, -1, "TOUCH"},
    {Kind::Value, kTouchSensitivity, "TOUCH SENSITIVITY"},
    {Kind::Value, kTouchLayout, "CONTROLS LAYOUT"},
    {Kind::Value, kTouchOpacity, "CONTROLS OPACITY"},
    {Kind::Value, kTouchSize, "CONTROLS SIZE"},
    {Kind::Value, kTouchSkin, "BUTTON STYLE"},
    {Kind::Value, kTouchStick, "ANALOG STICK"},
    {Kind::Value, kTouchEdit, "EDIT CONTROLS POSITION"},
    {Kind::Heading, -1, "MOTION CONTROLS"},
    {Kind::Value, kGyro, "GYROSCOPE"},
    {Kind::Value, kGyroSensitivity, "GYRO SENSITIVITY"},
    {Kind::Value, kGyroInvertX, "GYRO INVERT X"},
    {Kind::Value, kGyroInvertY, "GYRO INVERT Y"},
};

// on the game's control setup page, under its own rows
constexpr Row kControllerRows[] = {
    {Kind::Value, kLeftDeadzone, "LEFT STICK DEADZONE"},
    {Kind::Value, kRightDeadzone, "RIGHT STICK DEADZONE"},
    {Kind::Value, kLeftSensitivity, "LEFT STICK SENSITIVITY"},
    {Kind::Value, kRightSensitivity, "RIGHT STICK SENSITIVITY"},
};

// LANGUAGE always shows: Indonesian is there whatever the disc
constexpr Row kGeneralRows[] = {
    {Kind::Value, kLanguage, "LANGUAGE"},
    {Kind::Value, kSkipIntros, "SKIP INTROS"},
    {Kind::Value, kAudioOutput, "AUDIO OUTPUT"},
    {Kind::Heading, -1, "UPDATES"},
    {Kind::Value, kCheckUpdates, "CHECK FOR UPDATES"},
    {Kind::Value, kAutoUpdate, "STARTUP UPDATE CHECK"},
};


// a page of its own, apart from the recomp's settings
constexpr Row kCheatRows[] = {
    {Kind::Heading, -1, "WEAPONS"},
    {Kind::Value, kInfiniteAmmo, "INFINITE AMMO"},
    {Kind::Value, kInfiniteGrenades, "INFINITE GRENADES"},
    {Kind::Value, kRapidFire, "RAPID FIRE"},
    {Kind::Value, kNoRecoil, "NO RECOIL"},
    {Kind::Heading, -1, "PLAYER"},
    {Kind::Value, kGodMode, "GOD MODE"},
    {Kind::Value, kOneHitKill, "ONE-HIT KILL"},
    {Kind::Value, kEsp, "ESP LINES"},
    {Kind::Value, kAimbot, "AIMBOT (WHILE AIMING IN)"},  // esp.cpp turns the view only at a zoom above 1
    {Kind::Heading, -1, "GAME"},
    {Kind::Value, kGameSpeed, "GAME SPEED"},
    {Kind::Value, kUnlockLevels, "UNLOCK ALL MISSIONS"},
    {Kind::Value, kUnlockBlackOps, "UNLOCK BLACK OPS"},
};

// the title menu's CREDITS page: who and what, and the links, which open in the browser. Its rows take no step; a
// row's id is kCredit and its place in kCredits
struct Credit {
    const char* label;  // a heading when there is no value
    const char* value;
    const char* url;
};
constexpr Credit kCredits[] = {
    {"CONTRIBUTORS", nullptr, nullptr},
    {"Rangga Casper", "ANDROID PORT", "https://github.com/RanggaCasper"},
    {"DATFOOLDIVE", "ANY WEAPON IN ANY LEVEL", "https://github.com/datfooldive"},
    {"BAGASROMADON", "BUTTON SKIN", "https://github.com/bagasromadon"},
    {"Zukiru", "HDR SHADER", nullptr},
    {"BUILT WITH", nullptr, nullptr},
    {"XBOXXBE-RECOMP", "TGP482", nullptr},
    {"SDL3, GLSLANG, TOML++", "LIBRARIES", nullptr},
    {"DEAR IMGUI", "LIBRARY", nullptr},
    {"XEMU, CXBX-RELOADED", "REFERENCES", nullptr},
    {"LINKS", nullptr, nullptr},
    {"YOUTUBE", "OPEN", "https://youtube.com/c/@RanggaCasper"},
    {"GITHUB", "OPEN", "https://github.com/RanggaCasper"},
    {"SUPPORT", "OPEN", "https://sociabuzz.com/racas/tribe"},
};

const std::vector<Row>& CreditRows() {
    static const std::vector<Row> list = [] {
        std::vector<Row> out;
        for (const Credit& c : kCredits) out.push_back({c.value ? Kind::Value : Kind::Heading, kCredit, c.label});
        return out;
    }();
    return list;
}

const Credit* CreditOf(const Row& row) {
    for (const Credit& c : kCredits)
        if (row.label == c.label) return &c;
    return nullptr;
}

const std::vector<Row>& InputRows() {
    static const std::vector<Row> list = [] {
        std::vector<Row> out = {{Kind::Heading, -1, "MOUSE"},
                                {Kind::Value, kSensitivity, "MOUSE SENSITIVITY"},
                                {Kind::Value, kInvertMouse, "INVERT LOOK"},
                                {Kind::Value, kHoldAim, "HOLD TO AIM"},
                                {Kind::Heading, -1, "KEYBOARD"}};
        size_t count = 0;
        const input::Action* actions = input::Actions(&count);
        for (size_t i = 0; i < count; i++) out.push_back({Kind::Bind, int(i), actions[i].label});
        out.push_back({Kind::Value, kWalkSpeed, "WALK SPEED"});
        return out;
    }();
    return list;
}

// the setting an ON / OFF row flips, nullptr for any other row
bool* ToggleFlag(Settings& s, int id) {
    game::Options& o = game::Opt();
    switch (id) {
        case kVsync: return &s.vsync;
        case kFpsCounter: return &o.fps_counter;
        case kReloadBlur: return &o.reload_blur;
        case kBloom: return &o.bloom;
        case kFog: return &o.fog;
        case kHdr: return &o.hdr;
        case kSkipIntros: return &o.skip_intros;
        case kInvertMouse: return &o.invert_mouse;
        case kHoldAim: return &o.hold_aim;
        case kInfiniteAmmo: return &o.infinite_ammo;
        case kNoRecoil: return &o.no_recoil;
        case kGodMode: return &o.god_mode;
        case kOneHitKill: return &o.one_hit_kill;
        case kInfiniteGrenades: return &o.infinite_grenades;
        case kUnlockLevels: return &o.unlock_levels;
        case kUnlockBlackOps: return &o.unlock_black_ops;
        case kEsp: return &o.esp;
        case kAimbot: return &o.aimbot;
        case kDynamicRes: return &o.dynamic_resolution;
        default: return nullptr;
    }
}

// both field of view rows: 45 to 140 in steps of 1, 70 the game's own
constexpr int kFovMin = 45;
constexpr int kFovMax = 140;
constexpr uint32_t kGameFov = 70;

void FovText(uint32_t fov, char (&text)[64]) {
    std::snprintf(text, sizeof(text), fov == kGameFov ? "%u (DEFAULT)" : "%u", fov);
}

// the dynamic resolution's frame rates to hold
constexpr uint32_t kDynamicTargets[] = {30, 45, 60, 90, 120};

// a resolution as percent of the screen's and its pixels
void PercentText(uint32_t percent, char (&text)[64]) {
    Mode out = Output();
    std::snprintf(text, sizeof(text), "%u%% (%ux%u)", percent, out.width * percent / 100, out.height * percent / 100);
}

// the display rows that are no ON / OFF, stepped first; false for another row
bool StepDisplayRow(int id, int step, Settings& s, char (&text)[64]) {
    game::Options& o = game::Opt();
    switch (id) {
        case kRenderResolution: {
            // 20% to 200% in steps of 5; stepping automatically switches off Dynamic Resolution so manual choice applies immediately
            if (step) {
                if (o.dynamic_resolution) {
                    s.render_percent = dynres::Percent();
                    o.dynamic_resolution = false;
                }
                s.render_percent = uint32_t(StepRange(int(s.render_percent), 20, 200, 5, step));
            }
            PercentText(o.dynamic_resolution ? dynres::Percent() : s.render_percent, text);
            return true;
        }
        case kDynamicTarget:
            std::snprintf(text, sizeof(text), "%u FPS", kDynamicTargets[StepTable(kDynamicTargets, o.dynamic_target_fps, step)]);
            return true;
        case kDynamicMin:
            // 20% up to the max, the max down to the min
            if (step) o.dynamic_min_percent = uint32_t(StepRange(int(o.dynamic_min_percent), 20, int(o.dynamic_max_percent), 5, step));
            PercentText(o.dynamic_min_percent, text);
            return true;
        case kDynamicMax:
            if (step) o.dynamic_max_percent = uint32_t(StepRange(int(o.dynamic_max_percent), int(o.dynamic_min_percent), 200, 5, step));
            PercentText(o.dynamic_max_percent, text);
            return true;
        case kDynamicHold:
            // 1 to 30 seconds
            if (step) o.dynamic_hold = uint32_t(StepRange(int(o.dynamic_hold), 1, 30, 1, step));
            std::snprintf(text, sizeof(text), "%u S", o.dynamic_hold);
            return true;
        case kFmvScale:
            std::snprintf(text, sizeof(text), "%s", kFmvScaleNames[StepTable(kFmvScales, o.fmv_scale, step)]);
            return true;
        case kAntiAliasing:
            std::snprintf(text, sizeof(text), "%s",
                          kAntiAliasingNames[StepTable(kAntiAliasings, o.anti_aliasing, step)]);
            return true;
        case kScreenFilter:
            std::snprintf(text, sizeof(text), "%s",
                          kScreenFilterNames[StepTable(kScreenFilters, o.screen_filter, step)]);
            return true;
        case kFov:
            if (step) o.fov = uint32_t(StepRange(int(o.fov), kFovMin, kFovMax, 1, step));
            FovText(o.fov, text);
            return true;
        case kViewmodelFov:
            if (step) o.viewmodel_fov = uint32_t(StepRange(int(o.viewmodel_fov), kFovMin, kFovMax, 1, step));
            FovText(o.viewmodel_fov, text);
            return true;
        case kMaxFps: {
            uint32_t cap = kFpsCaps[StepTable(kFpsCaps, s.fps_cap, step)];
            if (cap == kFpsCapRefresh) std::snprintf(text, sizeof(text), "REFRESH RATE");
            else if (cap) std::snprintf(text, sizeof(text), "%u", cap);
            else std::snprintf(text, sizeof(text), "UNLIMITED (DEFAULT)");
            return true;
        }
        default:
            return false;
    }
}

// the graphics rows that are no ON / OFF, stepped first; false for another row
bool StepGraphicsRow(int id, int step, Settings& s, char (&text)[64]) {
    game::Options& o = game::Opt();
    switch (id) {
        case kAniso:
            std::snprintf(text, sizeof(text), "%s", kAnisoNames[StepTable(kAnisoValues, s.aniso, step)]);
            return true;
        case kDrawDistance:
            std::snprintf(text, sizeof(text), "%s", kDistanceNames[StepTable(kDistances, o.draw_distance, step)]);
            return true;
        default:
            return false;
    }
}

// the cheat rows that are no ON / OFF, stepped first; false for another row
bool StepCheatRow(int id, int step, char (&text)[64]) {
    game::Options& o = game::Opt();
    switch (id) {
        case kRapidFire:
            // off, then 2 to 4 times the rate
            if (step) o.rapid_fire = uint32_t(StepRange(int(o.rapid_fire), 1, 4, 1, step));
            if (o.rapid_fire <= 1) std::snprintf(text, sizeof(text), "OFF");
            else std::snprintf(text, sizeof(text), "%uX", o.rapid_fire);
            return true;
        case kGameSpeed:
            // 25% to 200% in steps of 25
            if (step) o.game_speed = uint32_t(StepRange(int(o.game_speed), 25, 200, 25, step));
            std::snprintf(text, sizeof(text), o.game_speed == 100 ? "%u%% (DEFAULT)" : "%u%%", o.game_speed);
            return true;
        default:
            return false;
    }
}

// the general, mouse and keyboard rows that are no ON / OFF, stepped first
void StepInputRow(int id, int step, char (&text)[64]) {
    game::Options& o = game::Opt();
    switch (id) {
        case kAudioOutput:
            std::snprintf(text, sizeof(text), "%s", kAudioOutputNames[StepTable(kAudioOutputs, o.audio_output, step)]);
            break;
        case kSensitivity:
            // 0.1 to 5.0 in steps of 0.1
            if (step) {
                int tenths = StepRange(int(std::lround(o.mouse_sensitivity * 10.0f)), 1, 50, 1, step);
                o.mouse_sensitivity = float(tenths) / 10.0f;
            }
            std::snprintf(text, sizeof(text), "%.1f", o.mouse_sensitivity);
            break;
        case kTouchSensitivity:
            // as the mouse's: 0.1 to 5.0 in steps of 0.1
            if (step) {
                int tenths = StepRange(int(std::lround(o.touch_sensitivity * 10.0f)), 1, 50, 1, step);
                o.touch_sensitivity = float(tenths) / 10.0f;
            }
            std::snprintf(text, sizeof(text), "%.1f", o.touch_sensitivity);
            break;
        case kWalkSpeed:
            // 10% to 100% in steps of 5
            if (step) o.walk_speed = uint32_t(StepRange(int(o.walk_speed), 10, 100, 5, step));
            std::snprintf(text, sizeof(text), "%u%%", o.walk_speed);
            break;
        case kGyro:
            // off, always on, while firing
            if (step) gyro::SetMode(uint32_t(StepIndex(int(gyro::CurrentMode()), step, 3)));
            std::snprintf(text, sizeof(text), "%s", gyro::CurrentMode() == 0 ? "OFF"
                                                    : gyro::CurrentMode() == 1 ? "ALWAYS"
                                                                               : "WHILE FIRING");
            break;
        case kTouchLayout:
            // the presets, CUSTOM once a control was moved
            if (step) touch::StepLayout(step);
            std::snprintf(text, sizeof(text), "%s", touch::LayoutName());
            break;
        case kTouchOpacity:
            if (step) touch::StepOpacity(step);
            std::snprintf(text, sizeof(text), "%u%%", touch::ControlsOpacity());
            break;
        case kTouchSize:
            if (step) touch::StepSize(step);
            std::snprintf(text, sizeof(text), "%u%%", touch::ControlsSize());
            break;
        case kTouchSkin:
            if (step) touch::StepSkin(step);
            std::snprintf(text, sizeof(text), "%s", touch::SkinName());
            break;
        case kTouchStick:
            // under the thumb where it comes down, or fixed in its place
            if (step) touch::StepStickMode(step);
            std::snprintf(text, sizeof(text), "%s", touch::StickModeName());
            break;
        case kTouchEdit:
            // any step opens the editor over the screen
            if (step) touch::EditLayout();
            std::snprintf(text, sizeof(text), "OPEN");
            break;
        case kCheckUpdates:
            // any step asks; the answer comes in a dialog over the game
            if (step) host::CheckForUpdates();
            std::snprintf(text, sizeof(text), "CHECK");
            break;
        case kGyroSensitivity:
            // 0.1 to 5.0 in steps of 0.1
            if (step) gyro::SetSensitivity(float(StepRange(int(std::lround(gyro::Sensitivity() * 10.0f)), 1, 50, 1, step)) / 10.0f);
            std::snprintf(text, sizeof(text), "%.1f", gyro::Sensitivity());
            break;
        case kGyroInvertX:
        case kGyroInvertY: {
            // kept with the gyro's other settings in android.toml, so not one of ToggleFlag's
            bool x = gyro::InvertX(), y = gyro::InvertY();
            if (step) (id == kGyroInvertX ? x : y) ^= true;
            if (step) gyro::SetInvert(x, y);
            std::snprintf(text, sizeof(text), "%s", OnOff(id == kGyroInvertX ? x : y));
            break;
        }
        case kLeftDeadzone:
        case kRightDeadzone: {
            // 0% to 50% of the stick's travel in steps of 1, 0 leaves the game's own
            uint32_t& dz = id == kLeftDeadzone ? o.left_deadzone : o.right_deadzone;
            if (step) dz = uint32_t(StepRange(int(dz), 0, 50, 1, step));
            std::snprintf(text, sizeof(text), dz ? "%u%%" : "%u%% (DEFAULT)", dz);
            break;
        }
        case kLeftSensitivity:
        case kRightSensitivity: {
            // 0.1 to 3.0 in steps of 0.1
            float& s = id == kLeftSensitivity ? o.left_sensitivity : o.right_sensitivity;
            if (step) s = float(StepRange(int(std::lround(s * 10.0f)), 1, 30, 1, step)) / 10.0f;
            std::snprintf(text, sizeof(text), "%.1f", s);
            break;
        }
        case kLanguage: {
            int count = int(std::size(game::kLanguages));
            int i = 0;
            while (i < count - 1 && o.language != game::kLanguages[i]) i++;
            // past the languages this disc has no strings for; English, Indonesian and Arabic are always there
            if (step) {
                do i = StepIndex(i, step, count);
                while (!game::LanguageAvailable(game::kLanguages[i]));
                o.language = game::kLanguages[i];
                auto& enh = modsettings::Registry().GetEnhanced();
                enh.arabic_ui_enabled = (o.language == "ara");
            }
            const char* format = o.language == game::BootLanguage() ? "%s" : "%s (RESTART)";
            std::snprintf(text, sizeof(text), format, kLanguageNames[i]);
            break;
        }
        case kAutoUpdate:
            // kept in the activity's preferences: it is read at launch, before the game's settings
            if (step) host::SetStartupCheck(!host::StartupCheck());
            std::snprintf(text, sizeof(text), "%s", OnOff(host::StartupCheck()));
            break;
    }
}

void NotifySettingForId(int id) {
    auto& presets = modsettings::Presets();
    auto& reg = modsettings::Registry();
    auto& s = app::Get();
    auto& o = game::Opt();
    switch (id) {
        case kRenderResolution: presets.NotifySettingChanged(modsettings::SettingId::RenderPercent, reg, s, o); break;
        case kMaxFps: presets.NotifySettingChanged(modsettings::SettingId::FpsCap, reg, s, o); break;
        case kVsync: presets.NotifySettingChanged(modsettings::SettingId::Vsync, reg, s, o); break;
        case kAniso: presets.NotifySettingChanged(modsettings::SettingId::AnisotropicFiltering, reg, s, o); break;
        case kAntiAliasing: presets.NotifySettingChanged(modsettings::SettingId::AntiAliasing, reg, s, o); break;
        case kScreenFilter: presets.NotifySettingChanged(modsettings::SettingId::ScreenFilter, reg, s, o); break;
        case kBloom: presets.NotifySettingChanged(modsettings::SettingId::Bloom, reg, s, o); break;
        case kFog: presets.NotifySettingChanged(modsettings::SettingId::Fog, reg, s, o); break;
        case kHdr: presets.NotifySettingChanged(modsettings::SettingId::Hdr, reg, s, o); break;
        case kReloadBlur: presets.NotifySettingChanged(modsettings::SettingId::ReloadBlur, reg, s, o); break;
        case kDrawDistance: presets.NotifySettingChanged(modsettings::SettingId::DrawDistance, reg, s, o); break;
        case kDynamicRes: presets.NotifySettingChanged(modsettings::SettingId::DynamicResolution, reg, s, o); break;
        case kDynamicTarget: presets.NotifySettingChanged(modsettings::SettingId::DynamicTargetFps, reg, s, o); break;
        case kDynamicMin: presets.NotifySettingChanged(modsettings::SettingId::DynamicMinPercent, reg, s, o); break;
        case kDynamicMax: presets.NotifySettingChanged(modsettings::SettingId::DynamicMaxPercent, reg, s, o); break;
        case kDynamicHold: presets.NotifySettingChanged(modsettings::SettingId::DynamicHold, reg, s, o); break;
        case kFmvScale: presets.NotifySettingChanged(modsettings::SettingId::FmvScale, reg, s, o); break;
        case kFpsCounter: presets.NotifySettingChanged(modsettings::SettingId::FpsCounter, reg, s, o); break;
        case kSkipIntros: presets.NotifySettingChanged(modsettings::SettingId::SkipIntros, reg, s, o); break;
        default: break;
    }
}

// steps the value first when step is not zero
std::string RowText(int id, int step, Settings& s = Get()) {
    char text[64] = "";
    if (bool* on = ToggleFlag(s, id)) {
        Flip(*on, step);
        std::snprintf(text, sizeof(text), "%s", OnOff(*on));
    } else if (!StepDisplayRow(id, step, s, text) && !StepGraphicsRow(id, step, s, text) &&
               !StepCheatRow(id, step, text)) {
        StepInputRow(id, step, text);
    }
    if (step && &s == &Get()) {
        NotifySettingForId(id);
        Changed();
    }
    return text;
}

constexpr const char* kUnbound = "UNBOUND";

std::string KeyText(std::string keys) {
    if (keys.empty()) return kUnbound;
    for (char& c : keys) c = c == ',' ? '/' : char(std::toupper(uint8_t(c)));
    return keys;
}

// as options_menu.cpp's pages: 0 graphics
constexpr int kTouchPage = 1;
constexpr int kGeneralPage = 2;
constexpr int kKeyboardPage = 3;
constexpr int kCheatsPage = 4;
constexpr int kCreditsPage = 5;

void RestoreDefaults(int page) {
    game::Options& o = game::Opt();
    if (page == kCheatsPage) {
        game::Options none;
        o.infinite_ammo = none.infinite_ammo;
        o.rapid_fire = none.rapid_fire;
        o.no_recoil = none.no_recoil;
        o.game_speed = none.game_speed;
        o.god_mode = none.god_mode;
        o.one_hit_kill = none.one_hit_kill;
        o.infinite_grenades = none.infinite_grenades;
        o.unlock_levels = none.unlock_levels;
        o.unlock_black_ops = none.unlock_black_ops;
        o.esp = none.esp;
        o.aimbot = none.aimbot;
        Changed();
        return;
    }
    if (page == kKeyboardPage) {
        o.mouse_sensitivity = game::Options{}.mouse_sensitivity;
        o.invert_mouse = false;
        o.hold_aim = true;
        o.walk_speed = game::Options{}.walk_speed;
        input::ResetBindings();
        return;
    }
    game::Options keep = o;
    Get() = DefaultSettings();
    o = game::Options{};
    o.mouse_sensitivity = keep.mouse_sensitivity;
    o.invert_mouse = keep.invert_mouse;
    o.hold_aim = keep.hold_aim;
    o.walk_speed = keep.walk_speed;
    o.keys = keep.keys;
    // the cheats page has its own defaults
    o.infinite_ammo = keep.infinite_ammo;
    o.rapid_fire = keep.rapid_fire;
    o.no_recoil = keep.no_recoil;
    o.game_speed = keep.game_speed;
    o.god_mode = keep.god_mode;
    o.one_hit_kill = keep.one_hit_kill;
    o.infinite_grenades = keep.infinite_grenades;
    o.unlock_levels = keep.unlock_levels;
    o.unlock_black_ops = keep.unlock_black_ops;
    o.esp = keep.esp;
    o.aimbot = keep.aimbot;
    Changed();
}

// the row being rebound, -1 when idle; its keys must be released before the next press binds
std::atomic<int> g_capture{-1};
int g_capture_slot = 0;  // its box: 0 the key, 1 the alternate
int g_hovered_box = -1;  // the box under the mouse or focused with a key in it, this frame
bool g_capture_armed = false;
int g_capture_done = 0;  // frame of the last bind, the key that bound must not start another
int g_page = kCheatsPage;
// the action's key and its alternate as their boxes show them, the alternate blank when there is none
std::vector<std::string> KeyBoxes(size_t action) {
    std::string keys = input::Binding(action);
    size_t comma = keys.find(',');
    std::string second;
    if (comma != std::string::npos) second = keys.substr(comma + 1);
    second = second.substr(0, second.find(','));
    if (!second.empty()) second = KeyText(second);
    return {KeyText(keys.substr(0, comma)), second};
}

constexpr float kKeyBoxEms = 10.0f;  // a key box's width when the row has two

// label left, a box per key right (the key and its alternate side by side), ending where the other rows' values do;
// Enter or click starts a box's capture, right click or Delete sets `cleared`; returns the box, -1 for none
int BindRow(const char* label, const std::vector<std::string>& keys, int capturing, int& cleared) {
    const ImGuiStyle& style = ImGui::GetStyle();
    RowBox row = MeasureRow();
    float value_end = row.row_x + row.row_w - row.block_w + row.row_h + style.ItemSpacing.x + row.value_w;
    int n = int(keys.size());
    int pressed = -1;
    float box_w = n > 1 ? row.em * kKeyBoxEms : row.value_w;
    ImGui::PushID(label);
    RowLabel(row, label);
    for (int i = 0; i < n; i++) {
        float x = value_end - float(n - i) * box_w - float(n - 1 - i) * style.ItemSpacing.x;
        ImGui::SetCursorPos(ImVec2(x, row.row_y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##key", ImVec2(box_w, row.row_h))) pressed = i;
        bool deleted = ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete, false);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) || deleted) cleared = i;
        bool pointed = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        if (pointed && !keys[i].empty() && keys[i] != kUnbound) g_hovered_box = i;
        bool lit = capturing == i || ImGui::IsItemFocused() || ImGui::IsItemHovered();
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::GetColorU32(lit ? ImGuiCol_Header : ImGuiCol_FrameBg));
        // key names as they are
        std::string value = keys[i];
        if (capturing == i) value = Ui("PRESS A KEY");
        else if (value == kUnbound) value = Ui(kUnbound);
        float pad = (box_w - ImGui::CalcTextSize(value.c_str()).x) * 0.5f;
        ImGui::SetCursorPos(ImVec2(x + std::max(pad, 0.0f), row.row_y));
        ImGui::AlignTextToFramePadding();
        if (capturing == i) ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
        ImGui::TextUnformatted(value.c_str());
        if (capturing == i) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::PopID();
    EndRow(row);
    return pressed;
}

// Escape cancels, any other bindable key or mouse button binds
void PollCapture() {
    if (g_capture < 0) return;
    int vk = input::PressedKey();
    if (!g_capture_armed) {
        g_capture_armed = !vk;
        return;
    }
    if (!vk) return;
    if (vk != VK_ESCAPE) input::Bind(size_t(g_capture.load()), vk, g_capture_slot);
    g_capture = -1;
    g_capture_done = ImGui::GetFrameCount();
}

// not under a bind or on the button that just bound
bool CaptureFree() { return g_capture < 0 && ImGui::GetFrameCount() > g_capture_done + 1; }

void ValueRow(const Row& r) {
    int step = OptionRow(r.label, RowText(r.id, 0).c_str());
    if (step) RowText(r.id, step);
}

void ValueRow(int id, const char* label) {
    int step = OptionRow(label, RowText(id, 0).c_str());
    if (step) RowText(id, step);
}

void KeyRow(const Row& r) {
    int cleared = -1;
    int box = BindRow(r.label, KeyBoxes(size_t(r.id)), g_capture == r.id ? g_capture_slot : -1, cleared);
    // a Delete that answered a capture is no clear
    if (cleared >= 0 && CaptureFree()) input::Bind(size_t(r.id), 0, cleared);
    if (box < 0 || !CaptureFree()) return;
    g_capture_slot = box;
    g_capture = r.id;
    g_capture_armed = false;
}

void DrawList(const Row* list, size_t count) {
    for (size_t i = 0; i < count; i++) {
        const Row& r = list[i];
        if (r.kind == Kind::Heading) Section(r.label);
        else if (r.kind == Kind::Value) ValueRow(r);
        else KeyRow(r);
    }
}

// the touch controls page ends with the sticks', which the game's options show on its control setup page
void DrawRows(int page) {
    size_t count = 0;
    const Row* list = rows::Page(page, &count);
    DrawList(list, count);
    if (page != kTouchPage) return;
    Section("CONTROLLER");
    DrawList(kControllerRows, std::size(kControllerRows));
}

// under the cheats, the level's weapons as buttons (weapons.cpp: any weapon in any level); the one given goes in the
// slot not in hand and the game switches to it
// Level weapons and arsenal manager: allows equipping any player weapon and replenishing supplies
void DrawWeapons() {
    weapons::View v = weapons::Current();
    float em = ImGui::GetFontSize();
    float btn_h = std::max(ImGui::GetFrameHeight(), em * 2.2f);
    TacticalTheme theme = CurrentTheme();

    Section("LEVEL WEAPON ARSENAL");

    if (!v.in_level) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme.heading);
        ImGui::TextUnformatted(Ui("(!) Start or continue a level to choose its weapons.").c_str());
        ImGui::PopStyleColor();

        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(Ui("Available weapons in tactical arsenal:").c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, em * 0.2f));

        static const char* const kAllWeaponsPreview[] = {
            "AK-47", "AK-47 + Suppressor", "M16", "G36C",
            "MP5", "MP5 + Suppressor", "P90", "Uzi", "MAC-10",
            "SPAS 12", "Remington 870", "Walther 2000", "Walther 2000 (Silenced)",
            "Magnum", "Glock", "Glock + Suppressor", "Pistol", "Pistol (Silenced)",
            "M249 (Saw)", "RPG", "M16A2 + M203"
        };
        constexpr int kColumns = 3;
        float gap = ImGui::GetStyle().ItemSpacing.x;
        float w = (ImGui::GetContentRegionAvail().x - gap * (kColumns - 1)) / kColumns;

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.16f, 0.24f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_Text, kText);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30f, 0.40f, 0.55f, 0.6f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.2f);
        for (size_t i = 0; i < std::size(kAllWeaponsPreview); i++) {
            if (i % kColumns) ImGui::SameLine();
            ImGui::Button(Ui(kAllWeaponsPreview[i], false).c_str(), ImVec2(w, btn_h));
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);

        Section("SUPPLIES");
        float supply_w = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
        ImGui::PushStyleColor(ImGuiCol_Button, kTab);
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::Button(Ui("FILL AMMUNITION").c_str(), ImVec2(supply_w, btn_h));
        ImGui::SameLine();
        ImGui::Button(Ui("FILL MEDKITS").c_str(), ImVec2(supply_w, btn_h));
        ImGui::PopStyleColor(2);
        return;
    }

    constexpr int kColumns = 3;
    float gap = ImGui::GetStyle().ItemSpacing.x;
    float w = (ImGui::GetContentRegionAvail().x - gap * (kColumns - 1)) / kColumns;
    for (size_t i = 0; i < v.choices.size(); i++) {
        const weapons::Choice& c = v.choices[i];
        if (i % kColumns) ImGui::SameLine();
        ImGui::PushID(c.type);
        if (c.held) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.heading);
            ImGui::PushStyleColor(ImGuiCol_Button, theme.highlight);
            ImGui::PushStyleColor(ImGuiCol_Border, theme.heading);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.5f);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, kTab);
            ImGui::PushStyleColor(ImGuiCol_Text, kText);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.35f, 0.50f, 0.6f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.2f);
        }
        if (ImGui::Button(Ui(c.label, false).c_str(), ImVec2(w, btn_h))) {
            weapons::Give(c.type);
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        ImGui::PopID();
    }
    Section("SUPPLIES");
    float supply_w = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
    if (ImGui::Button(Ui("FILL AMMUNITION").c_str(), ImVec2(supply_w, btn_h))) weapons::FillAmmo();
    ImGui::SameLine();
    if (ImGui::Button(Ui("FILL MEDKITS").c_str(), ImVec2(supply_w, btn_h))) weapons::FillMedkits();
    if (!v.status.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.heading);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(Ui(v.status, false).c_str());
        ImGui::PopStyleColor();
    }
}

void Defaults() {
    g_capture = -1;
    RestoreDefaults(g_page);
}

// the panel is the cheats alone, the touch button's way to them in play; every other setting is in the game's own
// options menu
void DrawTitle(const char* caption) {
    std::string title = Ui(caption);
    float title_pad = (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(title.c_str()).x) * 0.5f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(title_pad, 0.0f));
    ImGui::TextUnformatted(title.c_str());
}

// the hint line, then Back and Defaults on the right
void DrawFooter(float em) {
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    const char* hint = "";
    if (g_capture >= 0) hint = "Press a key or mouse button to bind, Escape cancels";
    else if (g_page == kKeyboardPage && g_hovered_box >= 0) hint = "Right click or Delete clears the key";
    else if (g_page == kCheatsPage) hint = "A weapon replaces the one not in hand, gold names are held";
    ImGui::TextUnformatted(Ui(hint, false).c_str());
    ImGui::PopStyleColor();
    float bw = em * 8;
    float buttons_pad = ImGui::GetContentRegionAvail().x - bw * 2 - ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(buttons_pad, 0.0f));
    if (ImGui::Button(Ui("Back", false).c_str(), ImVec2(bw, 0))) ToggleMenu();
    ImGui::SameLine();
    if (ImGui::Button(Ui("Defaults", false).c_str(), ImVec2(bw, 0))) Defaults();
}

// Tactical touch-drag & kinetic inertia scrolling system for mobile touchscreens:
// - 1:1 direct finger tracking with zero dropped distance
// - Low activation deadzone (~10px) to distinguish tap from drag
// - Cancels widget click on drag initiation or when tapping to stop an active glide
// - Natural deceleration curves with boundary clamping
// - Independent scroll state for content panel and sidebar
struct TouchScrollState {
    bool is_touching = false;
    bool is_dragging = false;
    float start_touch_x = 0.0f;
    float start_touch_y = 0.0f;
    float start_scroll_y = 0.0f;
    float glide_velocity = 0.0f;
};

static TouchScrollState s_content_scroll;
static TouchScrollState s_sidebar_scroll;

void ResetScroll(const char* target = nullptr) {
    if (!target || std::strcmp(target, "content") == 0) s_content_scroll = TouchScrollState{};
    if (!target || std::strcmp(target, "sidebar") == 0) s_sidebar_scroll = TouchScrollState{};
}

void DragScroll(float em, const char* panel_id = "content") {
    TouchScrollState& s = (panel_id && std::strcmp(panel_id, "sidebar") == 0) ? s_sidebar_scroll : s_content_scroll;
    ImGuiIO& io = ImGui::GetIO();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window) return;

    // Check if the touch down point was inside this window's inner bounds
    bool inside = window->InnerRect.Contains(io.MouseClickedPos[0]);

    if (ImGui::IsMouseClicked(0)) {
        if (inside) {
            bool was_gliding = (std::fabs(s.glide_velocity) > 20.0f);
            s.is_touching = true;
            s.is_dragging = false;
            s.start_touch_x = io.MouseClickedPos[0].x;
            s.start_touch_y = io.MouseClickedPos[0].y;
            s.start_scroll_y = ImGui::GetScrollY();
            s.glide_velocity = 0.0f; // Instant halt on finger contact
            if (was_gliding) {
                ImGui::ClearActiveID(); // Tapping to stop a glide should not activate buttons
            }
        } else {
            s.is_touching = false;
            s.is_dragging = false;
        }
    }

    constexpr float kDragThreshold = 12.0f; // 12 physical pixels vertical touch slop

    if (io.MouseDown[0] && s.is_touching) {
        float dx = io.MousePos.x - s.start_touch_x;
        float dy = io.MousePos.y - s.start_touch_y;
        // Only hijack gesture for vertical scrolling when vertical motion clearly dominates horizontal slider drag
        if (!s.is_dragging && std::fabs(dy) > kDragThreshold && std::fabs(dy) > std::fabs(dx) * 1.5f) {
            s.is_dragging = true;
            ImGui::ClearActiveID();
        }

        if (s.is_dragging) {
            // Direct 1:1 physical finger tracking — absolute delta ensures no frame movement is lost
            float target_scroll = s.start_scroll_y - dy;
            ImGui::SetScrollY(target_scroll);
            ImGui::ClearActiveID();

            // Track frame delta for kinetic release momentum
            if (io.DeltaTime > 0.0005f) {
                float instant_v = -io.MouseDelta.y / io.DeltaTime;
                s.glide_velocity = s.glide_velocity * 0.35f + instant_v * 0.65f;
            }
            return;
        }
    } else {
        s.is_touching = false;
        s.is_dragging = false;
    }

    // Kinetic glide momentum with natural friction
    if (std::fabs(s.glide_velocity) > 8.0f) {
        float scroll = ImGui::GetScrollY();
        float max_scroll = ImGui::GetScrollMaxY();
        float next_scroll = scroll + s.glide_velocity * io.DeltaTime;

        if (next_scroll <= 0.0f) {
            ImGui::SetScrollY(0.0f);
            s.glide_velocity = 0.0f;
        } else if (next_scroll >= max_scroll) {
            ImGui::SetScrollY(max_scroll);
            s.glide_velocity = 0.0f;
        } else {
            ImGui::SetScrollY(next_scroll);
            constexpr float kFriction = 3.2f;
            s.glide_velocity *= std::max(0.0f, 1.0f - kFriction * io.DeltaTime);
        }
    } else {
        s.glide_velocity = 0.0f;
    }
}

// --- Enhanced Edition Step Functions ---
void StepToneMapping(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.tone_mapping = uint32_t(StepIndex(int(enh.tone_mapping), step, 3));
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::GraphicsToneMapping, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepCasEnabled(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.cas_sharpen_enabled = !enh.cas_sharpen_enabled;
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::GraphicsCasSharpenEnabled, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepCasStrength(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.cas_sharpen_strength = uint32_t(StepRange(int(enh.cas_sharpen_strength), 0, 100, 5, step));
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::GraphicsCasSharpenStrength, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepCinematicGrade(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.cinematic_grade = uint32_t(StepIndex(int(enh.cinematic_grade), step, 4));
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::GraphicsCinematicGrade, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepLensVignette(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.lens_vignette = uint32_t(StepRange(int(enh.lens_vignette), 0, 100, 5, step));
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::GraphicsLensVignette, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepFilmGrain(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.film_grain = uint32_t(StepRange(int(enh.film_grain), 0, 100, 5, step));
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::GraphicsFilmGrain, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepBlastLensPulse(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.blast_lens_pulse = !enh.blast_lens_pulse;
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::GraphicsBlastLensPulse, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepBloomIntensity(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.bloom_intensity = uint32_t(StepRange(int(enh.bloom_intensity), 50, 200, 10, step));
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::GraphicsBloomIntensity, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepTextureLodBias(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    constexpr float kBiasVals[] = {0.0f, -0.5f, -1.0f};
    int cur = (enh.texture_lod_bias <= -0.8f) ? 2 : (enh.texture_lod_bias <= -0.3f) ? 1 : 0;
    if (step) {
        cur = StepIndex(cur, step, 3);
        enh.texture_lod_bias = kBiasVals[cur];
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::GraphicsTextureLodBias, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepTrilinear(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.trilinear_filtering = !enh.trilinear_filtering;
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::GraphicsTrilinearFiltering, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepDamageVignette(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.screen_damage_vignette = !enh.screen_damage_vignette;
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::CombatScreenDamageVignette, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepDamageVignetteIntensity(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.damage_vignette_intensity = uint32_t(StepRange(int(enh.damage_vignette_intensity), 10, 100, 5, step));
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::CombatDamageVignetteIntensity, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepLowHealthDesat(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.low_health_desaturation = !enh.low_health_desaturation;
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::CombatLowHealthDesaturation, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepExplosionGlow(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.explosion_glow_boost = uint32_t(StepRange(int(enh.explosion_glow_boost), 50, 200, 10, step));
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::CombatExplosionGlowBoost, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepParticleFade(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.particle_fade_scale = uint32_t(StepRange(int(enh.particle_fade_scale), 100, 400, 25, step));
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::CombatParticleFadeScale, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepRagdollPhysics(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.ragdoll_60fps_physics = !enh.ragdoll_60fps_physics;
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::CharactersRagdoll60Fps, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepCharacterLod(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    constexpr uint32_t kLods[] = {1, 2, 4};
    int cur = (enh.character_lod_scale >= 4) ? 2 : (enh.character_lod_scale >= 2) ? 1 : 0;
    if (step) {
        cur = StepIndex(cur, step, 3);
        enh.character_lod_scale = kLods[cur];
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::CharactersLodScale, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepCombatAudioBoost(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.combat_dynamic_boost = !enh.combat_dynamic_boost;
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::AudioCombatDynamicBoost, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepMasterGain(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.master_gain_percent = uint32_t(StepRange(int(enh.master_gain_percent), 50, 150, 5, step));
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::AudioMasterGainPercent, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepArabicUi(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.arabic_ui_enabled = !enh.arabic_ui_enabled;
        if (enh.arabic_ui_enabled) {
            game::Opt().language = "ara";
        } else {
            game::Opt().language = "int";
        }
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::ArabicUiEnabled, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepArabicRtl(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.arabic_rtl_mirror = !enh.arabic_rtl_mirror;
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::ArabicRtlMirror, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepArabicSubtitles(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.arabic_subtitles_overlay = !enh.arabic_subtitles_overlay;
        if (!enh.arabic_subtitles_overlay) {
            game::subtitles::Clear();
        }
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::ArabicSubtitlesOverlay, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepDisableAimAssist(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.disable_aim_assist = !enh.disable_aim_assist;
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::GameplayDisableAimAssist, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepThemeAccent(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    constexpr const char* kThemes[] = {"brass", "crimson", "olive"};
    int cur = (enh.theme_accent == "crimson") ? 1 : (enh.theme_accent == "olive") ? 2 : 0;
    if (step) {
        cur = StepIndex(cur, step, 3);
        enh.theme_accent = kThemes[cur];
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::InterfaceThemeAccent, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepPerfHud(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.advanced_perf_hud = !enh.advanced_perf_hud;
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::InterfaceAdvancedPerfHud, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepRestartBanners(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.show_restart_banners = !enh.show_restart_banners;
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::InterfaceShowRestartBanners, reg, app::Get(), game::Opt());
        Changed();
    }
}

void StepHideHudScreenshots(int step) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    if (step) {
        enh.hide_hud_on_screenshots = !enh.hide_hud_on_screenshots;
        modsettings::Presets().NotifySettingChanged(modsettings::SettingId::InterfaceHideHudOnScreenshots, reg, app::Get(), game::Opt());
        Changed();
    }
}

// --- Category Renderers ---
void DrawCategoryGraphics(float em) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    auto& s = app::Get();
    auto& o = game::Opt();

    Section("VISUAL PRESET");
    {
        const float avail_w = ImGui::GetContentRegionAvail().x;
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float btn_w = (avail_w - gap * 2.0f) / 3.0f;
        const float btn_h = std::max(em * 2.2f, 48.0f);
        TacticalTheme theme = CurrentTheme();

        const bool is_vanilla = (enh.tone_mapping == 0 && !enh.cas_sharpen_enabled && enh.cinematic_grade == 0 &&
                                 enh.lens_vignette == 0 && enh.film_grain == 0 && !enh.blast_lens_pulse &&
                                 o.anti_aliasing == 0 && s.render_percent == 67);
        const bool is_hd = (enh.tone_mapping == 1 && enh.cas_sharpen_enabled && enh.cinematic_grade == 0 &&
                            enh.film_grain == 0 && o.anti_aliasing == 1 && s.render_percent == 100);
        const bool is_cinema = (enh.tone_mapping == 2 && enh.cas_sharpen_enabled && enh.cinematic_grade == 1 &&
                                enh.lens_vignette >= 25 && enh.film_grain >= 15 && enh.blast_lens_pulse &&
                                o.anti_aliasing == 1 && s.render_percent == 100);

        struct VisPreset {
            const char* name;
            bool active;
            int id;
        } items[3] = {
            {"VANILLA 2006", is_vanilla, 0},
            {"CRYSTAL HD", is_hd, 1},
            {"HOLLYWOOD CINEMA", is_cinema, 2},
        };
        for (int i = 0; i < 3; i++) {
            int idx = IsRtl() ? (2 - i) : i;
            if (i > 0) ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, items[idx].active ? theme.highlight : kTab);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.button_hovered);
            ImGui::PushStyleColor(ImGuiCol_Text, items[idx].active ? theme.heading : kText);
            ImGui::PushStyleColor(ImGuiCol_Border, items[idx].active ? theme.heading : ImVec4(0.30f, 0.42f, 0.58f, 0.65f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, items[idx].active ? 2.2f : 1.0f);
            std::string lbl = Ui(items[idx].name) + "###vis_pre_" + std::to_string(items[idx].id);
            if (ImGui::Button(lbl.c_str(), ImVec2(btn_w, btn_h))) {
                if (items[idx].id == 0) {
                    modsettings::Presets().ApplyPreset(modsettings::PresetKind::Original, reg, s, o);
                } else if (items[idx].id == 1) {
                    o.dynamic_resolution = false;
                    s.render_percent = 100;
                    s.aniso = 16;
                    o.anti_aliasing = 1;
                    o.screen_filter = 0;
                    o.bloom = true;
                    o.fog = true;
                    o.hdr = false;
                    o.draw_distance = 4;
                    enh.tone_mapping = 1;
                    enh.cas_sharpen_enabled = true;
                    enh.cas_sharpen_strength = 45;
                    enh.bloom_intensity = 115;
                    enh.texture_lod_bias = -0.5f;
                    enh.trilinear_filtering = true;
                    enh.cinematic_grade = 0;
                    enh.lens_vignette = 15;
                    enh.film_grain = 0;
                    enh.blast_lens_pulse = false;
                    enh.character_lod_scale = 4;
                    modsettings::Presets().SetActiveKind(modsettings::PresetKind::Custom);
                } else {
                    o.dynamic_resolution = false;
                    s.render_percent = 100;
                    s.aniso = 16;
                    o.anti_aliasing = 1;
                    o.screen_filter = 0;
                    o.bloom = true;
                    o.fog = true;
                    o.hdr = false;
                    o.draw_distance = 4;
                    enh.tone_mapping = 2;
                    enh.cas_sharpen_enabled = true;
                    enh.cas_sharpen_strength = 50;
                    enh.bloom_intensity = 145;
                    enh.texture_lod_bias = -0.5f;
                    enh.trilinear_filtering = true;
                    enh.cinematic_grade = 1;
                    enh.lens_vignette = 35;
                    enh.film_grain = 20;
                    enh.blast_lens_pulse = true;
                    enh.character_lod_scale = 4;
                    enh.ragdoll_60fps_physics = true;
                    modsettings::Presets().SetActiveKind(modsettings::PresetKind::Custom);
                }
                Changed();
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);
        }
        ImGui::Dummy(ImVec2(0, em * 0.2f));
    }

    Section("CINEMATIC POST-FX");
    {
        constexpr const char* kToneNames[] = {"ORIGINAL (D3D)", "REINHARD HDR", "ACES FILMIC"};
        int step = OptionRow("TONE MAPPING", kToneNames[std::min(enh.tone_mapping, 2u)]);
        if (step) StepToneMapping(step);
    }
    {
        constexpr const char* kGradeNames[] = {"OFF (NATURAL)", "TEAL & ORANGE", "GRITTY TACTICAL", "WARZONE HDR"};
        int step = OptionRow("CINEMATIC COLOR GRADE", kGradeNames[std::min(enh.cinematic_grade, 3u)]);
        if (step) StepCinematicGrade(step);
    }
    {
        int step = OptionRow("CAS CONTRAST SHARPENING", OnOff(enh.cas_sharpen_enabled));
        if (step) StepCasEnabled(step);
    }
    if (enh.cas_sharpen_enabled) {
        char ctext[24];
        std::snprintf(ctext, sizeof(ctext), "%u%%", enh.cas_sharpen_strength);
        int step = OptionRow("CAS SHARPEN STRENGTH", ctext);
        if (step) StepCasStrength(step);
    }
    {
        if (enh.lens_vignette == 0) {
            int step = OptionRow("CINEMA LENS VIGNETTE", OnOff(false));
            if (step) StepLensVignette(step);
        } else {
            char vtext[24];
            std::snprintf(vtext, sizeof(vtext), "%u%%", enh.lens_vignette);
            int step = OptionRow("CINEMA LENS VIGNETTE", vtext);
            if (step) StepLensVignette(step);
        }
    }
    {
        if (enh.film_grain == 0) {
            int step = OptionRow("35MM FILM GRAIN", OnOff(false));
            if (step) StepFilmGrain(step);
        } else {
            char gtext[24];
            std::snprintf(gtext, sizeof(gtext), "%u%%", enh.film_grain);
            int step = OptionRow("35MM FILM GRAIN", gtext);
            if (step) StepFilmGrain(step);
        }
    }
    {
        int step = OptionRow("BLAST SHOCKWAVE PULSE", OnOff(enh.blast_lens_pulse));
        if (step) StepBlastLensPulse(step);
    }
    ValueRow(kScreenFilter, "SCREEN FILTER");

    Section("DISPLAY & CAMERA");
    ValueRow(kRenderResolution, "RENDER RESOLUTION");
    {
        constexpr uint32_t kResPresets[] = {50, 75, 100, 125, 150, 200};
        const float avail_w = ImGui::GetContentRegionAvail().x;
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float btn_w = (avail_w - gap * 5.0f) / 6.0f;
        const float btn_h = std::max(em * 2.0f, 46.0f);
        TacticalTheme theme = CurrentTheme();
        const uint32_t cur_res = o.dynamic_resolution ? dynres::Percent() : s.render_percent;
        for (int i = 0; i < 6; i++) {
            int idx = IsRtl() ? (5 - i) : i;
            if (i > 0) ImGui::SameLine();
            uint32_t p = kResPresets[idx];
            bool active = (!o.dynamic_resolution && s.render_percent == p);
            ImGui::PushStyleColor(ImGuiCol_Button, active ? theme.highlight : kTab);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.button_hovered);
            ImGui::PushStyleColor(ImGuiCol_Text, active ? theme.heading : (cur_res == p ? theme.heading : kText));
            ImGui::PushStyleColor(ImGuiCol_Border, active ? theme.heading : ImVec4(0.30f, 0.42f, 0.58f, 0.65f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, active ? 2.2f : 1.0f);
            char lbl[32];
            std::snprintf(lbl, sizeof(lbl), "%u%%###res_preset_%u", p, p);
            if (ImGui::Button(lbl, ImVec2(btn_w, btn_h))) {
                o.dynamic_resolution = false;
                s.render_percent = p;
                modsettings::Presets().NotifySettingChanged(modsettings::SettingId::RenderPercent, reg, s, o);
                Changed();
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);
        }
        ImGui::Dummy(ImVec2(0, em * 0.15f));
    }
    ValueRow(kFov, "FIELD OF VIEW");
    ValueRow(kViewmodelFov, "VIEWMODEL FIELD OF VIEW");
    ValueRow(kFmvScale, "FMV SCALE");

    Section("LIGHTING & TEXTURE QUALITY");
    ValueRow(kAniso, "ANISOTROPIC FILTERING");
    ValueRow(kDrawDistance, "DRAW DISTANCE");
    {
        constexpr const char* kLodNames[] = {"1x (DEFAULT)", "2x (HIGH)", "4x (ULTRA)"};
        int cur = (enh.character_lod_scale >= 4) ? 2 : (enh.character_lod_scale >= 2) ? 1 : 0;
        int step = OptionRow("CHARACTER LOD SCALE", kLodNames[cur]);
        if (step) StepCharacterLod(step);
    }
    ValueRow(kBloom, "BLOOM GLOW");
    if (o.bloom) {
        char btext[32];
        std::snprintf(btext, sizeof(btext), enh.bloom_intensity == 100 ? "%u%% (DEFAULT)" : "%u%%", enh.bloom_intensity);
        int step = OptionRow("BLOOM INTENSITY", btext);
        if (step) StepBloomIntensity(step);
    }
    ValueRow(kFog, "VOLUMETRIC FOG");
    ValueRow(kAntiAliasing, "ANTI-ALIASING");
    ValueRow(kHdr, "HDR PIPELINE");
    ValueRow(kReloadBlur, "RELOAD BLUR");
    {
        constexpr const char* kBiasNames[] = {"0.0 (STANDARD)", "-0.5 (SHARP)", "-1.0 (ULTRA SHARP)"};
        int cur = (enh.texture_lod_bias <= -0.8f) ? 2 : (enh.texture_lod_bias <= -0.3f) ? 1 : 0;
        int step = OptionRow("TEXTURE LOD BIAS", kBiasNames[cur]);
        if (step) StepTextureLodBias(step);
    }
    {
        int step = OptionRow("TRILINEAR FILTERING", OnOff(enh.trilinear_filtering));
        if (step) StepTrilinear(step);
    }

    Section("PERFORMANCE & PACING");
    ValueRow(kVsync, "VERTICAL SYNC");
    ValueRow(kMaxFps, "MAX FRAME RATE");
    ValueRow(kFpsCounter, "FPS COUNTER");
    ValueRow(kDynamicRes, "DYNAMIC RESOLUTION");
    if (o.dynamic_resolution) {
        ValueRow(kDynamicTarget, "TARGET FRAME RATE");
        ValueRow(kDynamicMin, "MIN RESOLUTION");
        ValueRow(kDynamicMax, "MAX RESOLUTION");
        ValueRow(kDynamicHold, "HOLD BEFORE RAISING");
    }
}

void DrawCategoryCombatGameplay(float em) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();

    Section("COMBAT EFFECTS");
    {
        char ptext[48];
        std::snprintf(ptext, sizeof(ptext), enh.particle_fade_scale == 100 ? "%u%% (DEFAULT)" : "%u%%", enh.particle_fade_scale);
        int step = OptionRow("UNIFIED FIRE, SMOKE & BLAST", ptext);
        if (step) {
            StepParticleFade(step);
            const float v = std::clamp(float(enh.particle_fade_scale) * 0.01f, 0.50f, 10.00f);
            enh.explosion_glow_boost = uint32_t(std::clamp(int(std::lround(100.0f + (v - 1.0f) * 45.0f)), 50, 500));
            auto& s = app::Get();
            auto& o = game::Opt();
            reg.SetFloat(modsettings::SettingId::PhysicsExplosionForce, v, s, o);
            reg.SetFloat(modsettings::SettingId::PhysicsImpactForce, v, s, o);
            reg.SetFloat(modsettings::SettingId::PhysicsStructuralDestruction, v, s, o);
            reg.SetFloat(modsettings::SettingId::PhysicsExplosionRadius,
                         (v >= 1.0f) ? std::clamp(1.0f + (v - 1.0f) * 0.45f, 0.50f, 5.00f) : std::clamp(v, 0.50f, 1.00f), s, o);
            reg.SetFloat(modsettings::SettingId::PhysicsDebrisIntensity, std::clamp(v, 0.25f, 6.00f), s, o);
            modsettings::Presets().NotifySettingChanged(modsettings::SettingId::CombatExplosionGlowBoost, reg, s, o);
            Changed();
        }
    }
    {
        int step = OptionRow("DAMAGE VIGNETTE", OnOff(enh.screen_damage_vignette));
        if (step) StepDamageVignette(step);
    }
    if (enh.screen_damage_vignette) {
        char vtext[32];
        std::snprintf(vtext, sizeof(vtext), "%u%%", enh.damage_vignette_intensity);
        int step = OptionRow("VIGNETTE INTENSITY", vtext);
        if (step) StepDamageVignetteIntensity(step);
    }
    {
        int step = OptionRow("LOW HEALTH DESATURATION", OnOff(enh.low_health_desaturation));
        if (step) StepLowHealthDesat(step);
    }

    Section("GUNPLAY & SPEED");
    {
        int step = OptionRow("DISABLE AIM ASSIST", OnOff(enh.disable_aim_assist));
        if (step) StepDisableAimAssist(step);
    }
    ValueRow(kGameSpeed, "GAME SPEED");
    ValueRow(kWalkSpeed, "WALK SPEED");
    ValueRow(kHoldAim, "HOLD TO AIM");
    ValueRow(kInvertMouse, "INVERT LOOK");
    ValueRow(kSkipIntros, "SKIP INTROS");
}

void DrawCategoryPhysicsLab(float em) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    auto& s = app::Get();
    auto& o = game::Opt();
    TacticalTheme theme = CurrentTheme();
    const bool rtl = IsRtl();

    Section("PHYSICS PRESETS");

    // Active profile header & Classic preservation indicator
    {
        const char* status_key = (enh.physics_preset == "classic") ? "CLASSIC 2006 PRESERVED" : "CUSTOM PHYSICS ACTIVE";
        if (enh.physics_preset == "realistic") status_key = "REALISTIC";
        else if (enh.physics_preset == "cinematic") status_key = "CINEMATIC";
        else if (enh.physics_preset == "mayhem") status_key = "MAYHEM";

        std::string title = Ui("ACTIVE PHYSICS PROFILE");
        std::string badge = Ui(status_key);
        ImVec4 badge_col = (enh.physics_preset == "classic") ? theme.heading : ImVec4(0.95f, 0.78f, 0.28f, 1.0f);

        if (rtl) {
            float tw = ImGui::CalcTextSize(title.c_str()).x;
            float bw = ImGui::CalcTextSize(badge.c_str()).x;
            float sp = ImGui::GetStyle().ItemSpacing.x * 2.0f;
            float av = ImGui::GetContentRegionAvail().x;
            if (av > tw + bw + sp) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + av - (tw + bw + sp));
            ImGui::TextColored(badge_col, "%s", badge.c_str());
            ImGui::SameLine(0, sp);
            ImGui::TextColored(kDim, "%s", title.c_str());
        } else {
            ImGui::TextColored(kDim, "%s", title.c_str());
            ImGui::SameLine();
            ImGui::TextColored(badge_col, "%s", badge.c_str());
        }
    }

    // 5 Preset Cards (54dp+ touch targets)
    struct PhysicsProfileDef {
        const char* id;
        const char* label;
    };
    constexpr PhysicsProfileDef kProfiles[] = {
        {"classic", "CLASSIC"},
        {"realistic", "REALISTIC"},
        {"cinematic", "CINEMATIC"},
        {"mayhem", "MAYHEM"},
        {"custom", "CUSTOM"},
    };

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    constexpr int kNumProfiles = int(std::size(kProfiles));
    const float preset_w = (avail_w - gap * float(kNumProfiles - 1)) / float(kNumProfiles);
    const float touch_h = std::max(em * 2.4f, 54.0f);

    for (int idx = 0; idx < kNumProfiles; idx++) {
        int i = rtl ? (kNumProfiles - 1 - idx) : idx;
        if (idx > 0) ImGui::SameLine();
        bool active = (enh.physics_preset == kProfiles[i].id);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, theme.highlight);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.button_hovered);
            ImGui::PushStyleColor(ImGuiCol_Text, theme.heading);
            ImGui::PushStyleColor(ImGuiCol_Border, theme.heading);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, kTab);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kTabHovered);
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.35f, 0.50f, 0.6f));
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, active ? 2.5f : 1.0f);
        char btn_id[96];
        std::snprintf(btn_id, sizeof(btn_id), "%s###phy_preset_%s", Ui(kProfiles[i].label).c_str(), kProfiles[i].id);
        if (ImGui::Button(btn_id, ImVec2(preset_w, touch_h))) {
            reg.ApplyPhysicsPreset(kProfiles[i].id);
            Changed();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
    }

    ImGui::Dummy(ImVec2(0, em * 0.20f));
    Section("QUICK FUN MODES");
    {
        const float fun_w = (avail_w - gap * 3.0f) / 4.0f;
        struct FunAction {
            const char* tag;
            const char* label;
        };
        constexpr FunAction kFun[] = {
            {"zerog", "ZERO-G MOON"},
            {"launch", "SUPER LAUNCH"},
            {"wreck", "TOTAL DESTRUCTION"},
            {"slowmo", "SLOW-MO MATRIX"},
        };
        for (int idx = 0; idx < 4; idx++) {
            int i = rtl ? (3 - idx) : idx;
            if (idx > 0) ImGui::SameLine();
            bool active_fun = false;
            if (i == 0) active_fun = (enh.physics_gravity_scale <= 0.16f);
            else if (i == 1) active_fun = (enh.physics_ragdoll_launch >= 8.0f);
            else if (i == 2) active_fun = (enh.physics_structural_destruction >= 8.0f && enh.physics_explosion_force >= 8.0f);
            else if (i == 3) active_fun = (enh.physics_time_scale <= 0.45f);

            ImGui::PushStyleColor(ImGuiCol_Button, active_fun ? theme.highlight : kTab);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.button_hovered);
            ImGui::PushStyleColor(ImGuiCol_Text, active_fun ? theme.heading : kText);
            ImGui::PushStyleColor(ImGuiCol_Border, active_fun ? theme.heading : ImVec4(0.35f, 0.48f, 0.65f, 0.7f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, active_fun ? 2.5f : 1.0f);
            char fid[96];
            std::snprintf(fid, sizeof(fid), "%s###phy_fun_%s", Ui(kFun[i].label).c_str(), kFun[i].tag);
            if (ImGui::Button(fid, ImVec2(fun_w, touch_h))) {
                if (i == 0) {
                    reg.SetFloat(modsettings::SettingId::PhysicsGravityScale, active_fun ? 1.00f : 0.10f, s, o);
                    if (!active_fun) reg.SetFloat(modsettings::SettingId::PhysicsRagdollLaunch, 4.50f, s, o);
                } else if (i == 1) {
                    reg.SetFloat(modsettings::SettingId::PhysicsRagdollLaunch, active_fun ? 1.00f : 9.00f, s, o);
                    reg.SetFloat(modsettings::SettingId::PhysicsCharacterReaction, active_fun ? 1.00f : 7.00f, s, o);
                    reg.SetFloat(modsettings::SettingId::PhysicsImpactForce, active_fun ? 1.00f : 7.50f, s, o);
                } else if (i == 2) {
                    reg.SetFloat(modsettings::SettingId::PhysicsExplosionForce, active_fun ? 1.00f : 9.00f, s, o);
                    reg.SetFloat(modsettings::SettingId::PhysicsExplosionRadius, active_fun ? 1.00f : 4.50f, s, o);
                    reg.SetFloat(modsettings::SettingId::PhysicsImpactForce, active_fun ? 1.00f : 9.00f, s, o);
                    reg.SetFloat(modsettings::SettingId::PhysicsStructuralDestruction, active_fun ? 1.00f : 9.00f, s, o);
                    reg.SetFloat(modsettings::SettingId::PhysicsDebrisIntensity, active_fun ? 1.00f : 6.00f, s, o);
                    enh.particle_fade_scale = active_fun ? 100u : 900u;
                    enh.explosion_glow_boost = active_fun ? 100u : 450u;
                    modsettings::Presets().NotifySettingChanged(modsettings::SettingId::CombatParticleFadeScale, reg, s, o);
                    modsettings::Presets().NotifySettingChanged(modsettings::SettingId::CombatExplosionGlowBoost, reg, s, o);
                } else if (i == 3) {
                    reg.SetFloat(modsettings::SettingId::PhysicsTimeScale, active_fun ? 1.00f : 0.35f, s, o);
                }
                Changed();
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);
        }
    }

    ImGui::Dummy(ImVec2(0, em * 0.25f));
    {
        char rst_all_id[96];
        std::snprintf(rst_all_id, sizeof(rst_all_id), "%s (100%% / 1.00x)###phy_reset_all", Ui("RESTORE ORIGINAL PHYSICS").c_str());
        ImGui::PushStyleColor(ImGuiCol_Button, kTab);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kTabHovered);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.heading);
        if (ImGui::Button(rst_all_id, ImVec2(avail_w, std::max(em * 2.1f, 52.0f)))) {
            reg.ApplyPhysicsPreset("classic");
            enh.particle_fade_scale = 100u;
            enh.explosion_glow_boost = 100u;
            modsettings::Presets().NotifySettingChanged(modsettings::SettingId::CombatParticleFadeScale, reg, s, o);
            modsettings::Presets().NotifySettingChanged(modsettings::SettingId::CombatExplosionGlowBoost, reg, s, o);
            Changed();
        }
        ImGui::PopStyleColor(3);
    }

    Section("PHYSICS PARAMETERS");

    struct PhysicsMasterRow {
        modsettings::SettingId primary_id;
        const char* key_tag;
        const char* title;
        const char* desc;
        float min_v;
        float max_v;
        float step_v;
    };
    constexpr PhysicsMasterRow kRows[] = {
        {modsettings::SettingId::PhysicsExplosionForce, "exp", "UNIFIED FIRE, SMOKE, BLAST & IMPACT",
         "Controls fire size, smoke density, rocket/grenade blast radius, bullet impact, and wall destruction all at once.", 0.25f, 10.00f, 0.25f},
        {modsettings::SettingId::PhysicsRagdollLaunch, "lnc", "RAGDOLL SKY LAUNCH",
         "Launches enemies and objects high into the air when shot or blasted.", 0.25f, 10.00f, 0.25f},
        {modsettings::SettingId::PhysicsGravityScale, "grv", "GRAVITY SCALE",
         "Scales world Y-gravity (-600 baseline) from Zero-G moon float to heavy pull.", 0.05f, 5.00f, 0.05f},
        {modsettings::SettingId::PhysicsTimeScale, "tms", "PHYSICS SIMULATION SPEED",
         "Scales the rigid-body and ragdoll physics solver timestep (Slow-Mo Matrix).", 0.20f, 2.00f, 0.05f},
    };

    auto apply_coupled_physics = [&](int idx, float v) {
        if (idx == 0) {
            // Unified Master Control: Fire size + Smoke density + Blast force & radius + Bullet impact + Destruction
            reg.SetFloat(modsettings::SettingId::PhysicsExplosionForce, v, s, o);
            reg.SetFloat(modsettings::SettingId::PhysicsImpactForce, v, s, o);
            reg.SetFloat(modsettings::SettingId::PhysicsStructuralDestruction, v, s, o);
            const float rad = (v >= 1.0f) ? std::clamp(1.0f + (v - 1.0f) * 0.45f, 0.50f, 5.00f) : std::clamp(v, 0.50f, 1.00f);
            reg.SetFloat(modsettings::SettingId::PhysicsExplosionRadius, rad, s, o);
            reg.SetFloat(modsettings::SettingId::PhysicsDebrisIntensity, std::clamp(v, 0.25f, 6.00f), s, o);
            const float shk = (v >= 1.0f) ? std::clamp(1.0f + (v - 1.0f) * 0.30f, 0.0f, 4.00f) : v;
            reg.SetFloat(modsettings::SettingId::PhysicsCameraShockwave, shk, s, o);
            enh.particle_fade_scale = uint32_t(std::clamp(int(std::lround(v * 100.0f)), 50, 1000));
            enh.explosion_glow_boost = uint32_t(std::clamp(int(std::lround(100.0f + (v - 1.0f) * 45.0f)), 50, 500));
            modsettings::Presets().NotifySettingChanged(modsettings::SettingId::CombatParticleFadeScale, reg, s, o);
            modsettings::Presets().NotifySettingChanged(modsettings::SettingId::CombatExplosionGlowBoost, reg, s, o);
        } else if (idx == 1) {
            // Ragdoll Sky Launch couples ragdoll launch, character reaction, and prop spin/bounce
            reg.SetFloat(modsettings::SettingId::PhysicsRagdollLaunch, v, s, o);
            reg.SetFloat(modsettings::SettingId::PhysicsCharacterReaction, std::clamp(v, 0.25f, 8.00f), s, o);
            reg.SetFloat(modsettings::SettingId::PhysicsPropBounciness, std::clamp(v, 0.25f, 6.00f), s, o);
        } else if (idx == 2) {
            reg.SetFloat(modsettings::SettingId::PhysicsGravityScale, v, s, o);
        } else if (idx == 3) {
            reg.SetFloat(modsettings::SettingId::PhysicsTimeScale, v, s, o);
        }
    };

    const float pad_v = std::max(em * 0.55f, 12.0f);
    for (int idx = 0; idx < int(std::size(kRows)); idx++) {
        const auto& r = kRows[idx];
        ImGui::PushID(r.key_tag);

        float val = reg.GetFloat(r.primary_id, s, o);
        int pct = int(std::lround(val * 100.0f));
        bool is_default = std::fabs(val - 1.00f) < 0.005f;

        char val_badge[48];
        std::snprintf(val_badge, sizeof(val_badge), "%d%% (%.2fx)", pct, val);
        std::string orig_text = Ui("ORIGINAL DEFAULT", false) + ": 100% (1.00x)";
        std::string title_txt = Ui(r.title);
        std::string desc_txt = Ui(r.desc, false);

        const float card_w = ImGui::GetContentRegionAvail().x;
        const float line_right = ImGui::GetCursorPosX() + card_w;
        ImVec4 val_col = is_default ? ImVec4(0.65f, 0.85f, 0.65f, 1.0f) : theme.heading;

        if (rtl) {
            ImGui::TextColored(val_col, "%s", val_badge);
            ImGui::SameLine();
            float tw = ImGui::CalcTextSize(title_txt.c_str()).x;
            float rx = line_right - tw;
            if (rx > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(rx);
            ImGui::TextColored(theme.heading, "%s", title_txt.c_str());

            float dw = ImGui::CalcTextSize(desc_txt.c_str()).x;
            float dx = line_right - dw;
            if (dx > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(dx);
            ImGui::TextColored(kDim, "%s", desc_txt.c_str());
        } else {
            ImGui::TextColored(theme.heading, "%s", title_txt.c_str());
            ImGui::SameLine();
            float bw = ImGui::CalcTextSize(val_badge).x;
            float bx = line_right - bw;
            if (bx > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(bx);
            ImGui::TextColored(val_col, "%s", val_badge);
            ImGui::TextColored(kDim, "%s", desc_txt.c_str());
        }

        const float step_btn_w = std::max(em * 2.5f, 54.0f);
        const float reset_btn_w = std::max(em * 5.2f, 96.0f);
        const float ctrl_h = touch_h;
        const float slider_w = std::max(card_w - (step_btn_w * 2.0f + reset_btn_w + gap * 3.0f), em * 6.0f);

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em * 0.5f, pad_v));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, theme.heading);
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, theme.button_hovered);

        bool modified = false;
        float edited = val;

        auto draw_dec = [&]() {
            char did[32];
            std::snprintf(did, sizeof(did), "-##phy_dec_%s", r.key_tag);
            if (ImGui::Button(did, ImVec2(step_btn_w, ctrl_h))) {
                edited = std::clamp(val - r.step_v, r.min_v, r.max_v);
                modified = true;
            }
        };
        auto draw_inc = [&]() {
            char iid[32];
            std::snprintf(iid, sizeof(iid), "+##phy_inc_%s", r.key_tag);
            if (ImGui::Button(iid, ImVec2(step_btn_w, ctrl_h))) {
                edited = std::clamp(val + r.step_v, r.min_v, r.max_v);
                modified = true;
            }
        };
        auto draw_slider = [&]() {
            char sid[32];
            std::snprintf(sid, sizeof(sid), "##phy_sld_%s", r.key_tag);
            ImGui::SetNextItemWidth(slider_w);
            if (ImGui::SliderFloat(sid, &edited, r.min_v, r.max_v, "%.2fx")) {
                edited = std::clamp(std::round(edited / r.step_v) * r.step_v, r.min_v, r.max_v);
                modified = true;
            }
        };
        auto draw_reset = [&]() {
            char rid[96];
            std::snprintf(rid, sizeof(rid), "%s###phy_rst_%s", Ui("RESET").c_str(), r.key_tag);
            if (ImGui::Button(rid, ImVec2(reset_btn_w, ctrl_h))) {
                edited = 1.00f;
                modified = true;
            }
        };

        if (rtl) {
            draw_reset();
            ImGui::SameLine();
            draw_dec();
            ImGui::SameLine();
            draw_slider();
            ImGui::SameLine();
            draw_inc();
        } else {
            draw_dec();
            ImGui::SameLine();
            draw_slider();
            ImGui::SameLine();
            draw_inc();
            ImGui::SameLine();
            draw_reset();
        }

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();

        if (modified) {
            apply_coupled_physics(idx, edited);
            Changed();
        }

        if (rtl) {
            float ow = ImGui::CalcTextSize(orig_text.c_str()).x;
            float ox = line_right - ow;
            if (ox > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(ox);
        }
        ImGui::TextColored(kDim, "%s", orig_text.c_str());

        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, em * 0.2f));
        ImGui::PopID();
    }

    Section("CHARACTER PHYSICS");
    {
        int step = OptionRow("RAGDOLL 60 FPS PHYSICS", OnOff(enh.ragdoll_60fps_physics));
        if (step) StepRagdollPhysics(step);
    }

    Section("ENGINE TELEMETRY");
    {
        const auto tel = game::physics_lab::GetTelemetry();
        char tel_line[220];
        std::snprintf(tel_line, sizeof(tel_line),
                      "g=%.1f | blast=%llu | impact=%llu | rag=%llu | debris=%llu | recoil=%llu",
                      tel.active_gravity_y,
                      static_cast<unsigned long long>(tel.blast_hits),
                      static_cast<unsigned long long>(tel.impact_hits),
                      static_cast<unsigned long long>(tel.ragdoll_steps),
                      static_cast<unsigned long long>(tel.debris_steps),
                      static_cast<unsigned long long>(tel.recoil_kicks));
        ImGui::TextColored(kDim, "%s", tel_line);
    }
}

void DrawCategoryAudioLanguage(float em) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();

    Section("SOUND & MIXING");
    {
        int step = OptionRow("COMBAT DYNAMIC BOOST", OnOff(enh.combat_dynamic_boost));
        if (step) StepCombatAudioBoost(step);
    }
    {
        char mtext[32];
        std::snprintf(mtext, sizeof(mtext), enh.master_gain_percent == 100 ? "%u%% (DEFAULT)" : "%u%%", enh.master_gain_percent);
        int step = OptionRow("MASTER AUDIO GAIN", mtext);
        if (step) StepMasterGain(step);
    }
    ValueRow(kAudioOutput, "AUDIO OUTPUT MODE");

    Section("LANGUAGE & ARABIC");
    ValueRow(kLanguage, "LANGUAGE");
    {
        bool pending = IsRestartPending(modsettings::SettingId::ArabicUiEnabled);
        char atext[48];
        std::snprintf(atext, sizeof(atext), pending ? "%s (! RESTART)" : "%s", OnOff(enh.arabic_ui_enabled));
        int step = OptionRow("ARABIC UI TRANSLATION", atext);
        if (step) StepArabicUi(step);
    }
    {
        int step = OptionRow("RTL TEXT MIRRORING", OnOff(enh.arabic_rtl_mirror));
        if (step) StepArabicRtl(step);
    }
    {
        int step = OptionRow("SUBTITLES OVERLAY", OnOff(enh.arabic_subtitles_overlay));
        if (step) StepArabicSubtitles(step);
    }
}

void DrawCategoryInterface(float em) {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();

    Section("THEME & HUD PRESENTATION");
    {
        const char* theme_name = "TACTICAL BRASS";
        if (enh.theme_accent == "crimson") theme_name = "SPECIAL OPS CRIMSON";
        else if (enh.theme_accent == "olive") theme_name = "MILITARY OLIVE";
        int step = OptionRow("CONSOLE ACCENT THEME", theme_name);
        if (step) StepThemeAccent(step);
    }
    {
        int step = OptionRow("ADVANCED PERF HUD", OnOff(enh.advanced_perf_hud));
        if (step) StepPerfHud(step);
    }
    {
        int step = OptionRow("SHOW RESTART BANNERS", OnOff(enh.show_restart_banners));
        if (step) StepRestartBanners(step);
    }
    {
        int step = OptionRow("HIDE HUD ON SCREENSHOTS", OnOff(enh.hide_hud_on_screenshots));
        if (step) StepHideHudScreenshots(step);
    }

    Section("TOUCH CONTROLS SETUP");
    ValueRow(kTouchSensitivity, "TOUCH SENSITIVITY");
    ValueRow(kTouchLayout, "CONTROLS LAYOUT");
    ValueRow(kTouchOpacity, "CONTROLS OPACITY");
    ValueRow(kTouchSize, "CONTROLS SIZE");
    ValueRow(kTouchSkin, "BUTTON STYLE");
    ValueRow(kTouchStick, "ANALOG STICK MODE");
    ValueRow(kTouchEdit, "EDIT CONTROLS POSITION");

    Section("MOTION & GYROSCOPE");
    ValueRow(kGyro, "GYROSCOPE");
    ValueRow(kGyroSensitivity, "GYRO SENSITIVITY");
    ValueRow(kGyroInvertX, "GYRO INVERT X");
    ValueRow(kGyroInvertY, "GYRO INVERT Y");

    Section("CONTROLLER STICKS");
    ValueRow(kLeftDeadzone, "LEFT STICK DEADZONE");
    ValueRow(kRightDeadzone, "RIGHT STICK DEADZONE");
    ValueRow(kLeftSensitivity, "LEFT STICK SENSITIVITY");
    ValueRow(kRightSensitivity, "RIGHT STICK SENSITIVITY");
}

void RestoreAllToOriginalGame() {
    auto& reg = modsettings::Registry();
    auto& enh = reg.GetEnhanced();
    auto& s = app::Get();
    auto& o = game::Opt();

    // 1. Restore Original 2006 Graphics & Display Preset
    modsettings::Presets().ApplyPreset(modsettings::PresetKind::Original, reg, s, o);

    // 2. Restore Original 2006 Physics Lab & Weapon Firepower/Blast/Smoke Multipliers
    reg.ApplyPhysicsPreset("classic");

    // 3. Reset all Cheats, Weapon Overrides, and Gameplay Multipliers to Original Vanilla
    o.infinite_ammo = false;
    o.infinite_grenades = false;
    o.rapid_fire = 1;
    o.no_recoil = false;
    o.god_mode = false;
    o.one_hit_kill = false;
    o.esp = false;
    o.aimbot = false;
    o.game_speed = 100;
    o.fov = 70;
    o.viewmodel_fov = 70;
    o.walk_speed = 100;

    // 4. Reset all Enhanced Visual, Combat, Character, and Audio Overrides to Vanilla
    enh.tone_mapping = 0;
    enh.cas_sharpen_enabled = false;
    enh.cas_sharpen_strength = 45;
    enh.bloom_intensity = 100;
    enh.texture_lod_bias = 0.0f;
    enh.trilinear_filtering = false;
    enh.cinematic_grade = 0;
    enh.lens_vignette = 0;
    enh.film_grain = 0;
    enh.blast_lens_pulse = false;
    enh.screen_damage_vignette = false;
    enh.low_health_desaturation = false;
    enh.explosion_glow_boost = 100;
    enh.particle_fade_scale = 100;
    enh.ragdoll_60fps_physics = false;
    enh.character_lod_scale = 1;
    enh.combat_dynamic_boost = false;
    enh.master_gain_percent = 100;
    enh.disable_aim_assist = false;

    // Ensure active preset remains Original after all notifications
    modsettings::Presets().ApplyPreset(modsettings::PresetKind::Original, reg, s, o);
    Changed();
}

void DrawCategoryCheats(float em) {
    // Weapons Arsenal is presented first at the top for immediate accessibility
    DrawWeapons();

    Section("WEAPON FIREPOWER & BLAST LAB");
    {
        auto& reg = modsettings::Registry();
        auto& enh = reg.GetEnhanced();
        auto& s = app::Get();
        auto& o = game::Opt();
        TacticalTheme theme = CurrentTheme();
        const bool rtl = IsRtl();
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float touch_h = std::max(em * 2.3f, 52.0f);
        const float pad_v = std::max(em * 0.50f, 11.0f);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.38f, 0.14f, 0.14f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.20f, 0.20f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.heading);
        std::string rst_wpn_lbl = Ui("RESTORE ORIGINAL WEAPONS & CHEATS") + "###rst_wpn_all";
        if (ImGui::Button(rst_wpn_lbl.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, touch_h))) {
            RestoreAllToOriginalGame();
        }
        ImGui::PopStyleColor(3);
        ImGui::Dummy(ImVec2(0, em * 0.20f));

        struct WeaponPowerRow {
            const char* tag;
            const char* title;
            const char* desc;
            float min_v;
            float max_v;
            float step_v;
        };
        constexpr WeaponPowerRow kWpnRows[] = {
            {"wpn_master", "UNIFIED FIRE, SMOKE, BLAST & IMPACT",
             "Controls fire size, smoke density, rocket/grenade blast radius, bullet impact, and wall destruction all at once.", 0.25f, 10.00f, 0.25f},
        };

        for (int idx = 0; idx < 1; idx++) {
            const auto& r = kWpnRows[idx];
            ImGui::PushID(r.tag);

            float val = std::max({reg.GetFloat(modsettings::SettingId::PhysicsExplosionForce, s, o),
                                  reg.GetFloat(modsettings::SettingId::PhysicsImpactForce, s, o),
                                  std::clamp(float(enh.particle_fade_scale) * 0.01f, 0.25f, 10.00f)});

            int pct = int(std::lround(val * 100.0f));
            bool is_default = std::fabs(val - 1.00f) < 0.01f;
            char val_badge[48];
            std::snprintf(val_badge, sizeof(val_badge), "%d%% (%.2fx)", pct, val);
            std::string title_txt = Ui(r.title);
            std::string desc_txt = Ui(r.desc, false);

            const float card_w = ImGui::GetContentRegionAvail().x;
            const float line_right = ImGui::GetCursorPosX() + card_w;
            ImVec4 val_col = is_default ? ImVec4(0.65f, 0.85f, 0.65f, 1.0f) : theme.heading;

            if (rtl) {
                ImGui::TextColored(val_col, "%s", val_badge);
                ImGui::SameLine();
                float tw = ImGui::CalcTextSize(title_txt.c_str()).x;
                float rx = line_right - tw;
                if (rx > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(rx);
                ImGui::TextColored(theme.heading, "%s", title_txt.c_str());

                float dw = ImGui::CalcTextSize(desc_txt.c_str()).x;
                float dx = line_right - dw;
                if (dx > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(dx);
                ImGui::TextColored(kDim, "%s", desc_txt.c_str());
            } else {
                ImGui::TextColored(theme.heading, "%s", title_txt.c_str());
                ImGui::SameLine();
                float bw = ImGui::CalcTextSize(val_badge).x;
                float bx = line_right - bw;
                if (bx > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(bx);
                ImGui::TextColored(val_col, "%s", val_badge);
                ImGui::TextColored(kDim, "%s", desc_txt.c_str());
            }

            const float step_btn_w = std::max(em * 2.5f, 54.0f);
            const float reset_btn_w = std::max(em * 5.2f, 96.0f);
            const float slider_w = std::max(card_w - (step_btn_w * 2.0f + reset_btn_w + gap * 3.0f), em * 6.0f);

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em * 0.5f, pad_v));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, theme.heading);
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, theme.button_hovered);

            bool modified = false;
            float edited = val;

            auto draw_dec = [&]() {
                char did[32];
                std::snprintf(did, sizeof(did), "-##wpn_dec_%s", r.tag);
                if (ImGui::Button(did, ImVec2(step_btn_w, touch_h))) {
                    edited = std::clamp(val - r.step_v, r.min_v, r.max_v);
                    modified = true;
                }
            };
            auto draw_inc = [&]() {
                char iid[32];
                std::snprintf(iid, sizeof(iid), "+##wpn_inc_%s", r.tag);
                if (ImGui::Button(iid, ImVec2(step_btn_w, touch_h))) {
                    edited = std::clamp(val + r.step_v, r.min_v, r.max_v);
                    modified = true;
                }
            };
            auto draw_slider = [&]() {
                char sid[32];
                std::snprintf(sid, sizeof(sid), "##wpn_sld_%s", r.tag);
                ImGui::SetNextItemWidth(slider_w);
                if (ImGui::SliderFloat(sid, &edited, r.min_v, r.max_v, "%.2fx")) {
                    edited = std::clamp(std::round(edited / r.step_v) * r.step_v, r.min_v, r.max_v);
                    modified = true;
                }
            };
            auto draw_reset = [&]() {
                char rid[96];
                std::snprintf(rid, sizeof(rid), "%s###wpn_rst_%s", Ui("RESET").c_str(), r.tag);
                if (ImGui::Button(rid, ImVec2(reset_btn_w, touch_h))) {
                    edited = 1.00f;
                    modified = true;
                }
            };

            if (rtl) {
                draw_reset();
                ImGui::SameLine();
                draw_dec();
                ImGui::SameLine();
                draw_slider();
                ImGui::SameLine();
                draw_inc();
            } else {
                draw_dec();
                ImGui::SameLine();
                draw_slider();
                ImGui::SameLine();
                draw_inc();
                ImGui::SameLine();
                draw_reset();
            }

            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar();

            if (modified) {
                reg.SetFloat(modsettings::SettingId::PhysicsExplosionForce, edited, s, o);
                reg.SetFloat(modsettings::SettingId::PhysicsImpactForce, edited, s, o);
                reg.SetFloat(modsettings::SettingId::PhysicsStructuralDestruction, edited, s, o);
                const float rad = (edited >= 1.0f) ? std::clamp(1.0f + (edited - 1.0f) * 0.45f, 0.50f, 5.00f) : std::clamp(edited, 0.50f, 1.00f);
                reg.SetFloat(modsettings::SettingId::PhysicsExplosionRadius, rad, s, o);
                reg.SetFloat(modsettings::SettingId::PhysicsDebrisIntensity, std::clamp(edited, 0.25f, 6.00f), s, o);
                const float shk = (edited >= 1.0f) ? std::clamp(1.0f + (edited - 1.0f) * 0.30f, 0.0f, 4.00f) : edited;
                reg.SetFloat(modsettings::SettingId::PhysicsCameraShockwave, shk, s, o);
                if (edited > 1.0f || std::fabs(edited - 1.0f) < 0.01f) {
                    reg.SetFloat(modsettings::SettingId::PhysicsRagdollLaunch, edited, s, o);
                    reg.SetFloat(modsettings::SettingId::PhysicsCharacterReaction, std::clamp(edited, 0.25f, 8.00f), s, o);
                }
                enh.particle_fade_scale = uint32_t(std::clamp(int(std::lround(edited * 100.0f)), 50, 1000));
                enh.explosion_glow_boost = uint32_t(std::clamp(int(std::lround(100.0f + (edited - 1.0f) * 45.0f)), 50, 500));
                modsettings::Presets().NotifySettingChanged(modsettings::SettingId::CombatParticleFadeScale, reg, s, o);
                modsettings::Presets().NotifySettingChanged(modsettings::SettingId::CombatExplosionGlowBoost, reg, s, o);
                Changed();
            }

            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, em * 0.15f));
            ImGui::PopID();
        }
    }

    Section("WEAPON OVERRIDES");
    ValueRow(kInfiniteAmmo, "INFINITE AMMO");
    ValueRow(kInfiniteGrenades, "INFINITE GRENADES");
    ValueRow(kRapidFire, "RAPID FIRE");
    ValueRow(kNoRecoil, "NO RECOIL");

    Section("OPERATOR MODES");
    ValueRow(kGodMode, "GOD MODE");
    ValueRow(kOneHitKill, "ONE-HIT KILL");
    ValueRow(kEsp, "ESP TACTICAL LINES");
    ValueRow(kAimbot, "AIMBOT (WHILE AIMING IN)");

    Section("MISSION PROGRESSION");
    ValueRow(kUnlockLevels, "UNLOCK ALL MISSIONS");
    ValueRow(kUnlockBlackOps, "UNLOCK BLACK OPS");
}

void DrawPresetSelector(float em) {
    auto& presets = modsettings::Presets();
    auto& reg = modsettings::Registry();
    auto& s = app::Get();
    auto& o = game::Opt();
    modsettings::PresetKind active = presets.ActiveKind();
    TacticalTheme theme = CurrentTheme();

    const char* active_desc = "";
    switch (active) {
        case modsettings::PresetKind::Original: active_desc = "[ORIGINAL 2006 XBOX BASELINE]"; break;
        case modsettings::PresetKind::Enhanced: active_desc = "[MODERN ENHANCED FIDELITY]"; break;
        case modsettings::PresetKind::Performance: active_desc = "[BATTERY & 60FPS OPTIMIZED]"; break;
        case modsettings::PresetKind::Custom: active_desc = "[CUSTOM USER TUNING]"; break;
    }
    std::string shaped_desc = Ui(active_desc);
    std::string profile_txt = Ui("TACTICAL PROFILE:");

    if (IsRtl()) {
        float desc_w = ImGui::CalcTextSize(shaped_desc.c_str()).x;
        float prof_w = ImGui::CalcTextSize(profile_txt.c_str()).x;
        float total_w = desc_w + prof_w + ImGui::GetStyle().ItemSpacing.x;
        float avail_w = ImGui::GetContentRegionAvail().x;
        if (avail_w > total_w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail_w - total_w);

        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(shaped_desc.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.heading);
        ImGui::TextUnformatted(profile_txt.c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, theme.heading);
        ImGui::TextUnformatted(profile_txt.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(shaped_desc.c_str());
        ImGui::PopStyleColor();
    }

    constexpr modsettings::PresetKind kKinds[] = {
        modsettings::PresetKind::Original,
        modsettings::PresetKind::Enhanced,
        modsettings::PresetKind::Performance,
        modsettings::PresetKind::Custom
    };
    constexpr const char* kLabels[] = {
        "ORIGINAL",
        "ENHANCED",
        "PERFORMANCE",
        "CUSTOM"
    };

    float avail = ImGui::GetContentRegionAvail().x;
    float gap = ImGui::GetStyle().ItemSpacing.x;
    float btn_w = (avail - gap * 3.0f) / 4.0f;
    float btn_h = em * 1.9f;

    for (int idx = 0; idx < 4; idx++) {
        int i = IsRtl() ? (3 - idx) : idx;
        if (idx > 0) ImGui::SameLine();
        bool is_active = (active == kKinds[i]);
        if (is_active) {
            ImGui::PushStyleColor(ImGuiCol_Button, theme.highlight);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.button_hovered);
            ImGui::PushStyleColor(ImGuiCol_Text, theme.heading);
            ImGui::PushStyleColor(ImGuiCol_Border, theme.heading);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, kTab);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kTabHovered);
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.35f, 0.50f, 0.6f));
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, is_active ? 2.0f : 1.0f);
        if (ImGui::Button(Ui(kLabels[i]).c_str(), ImVec2(btn_w, btn_h))) {
            if (kKinds[i] == modsettings::PresetKind::Original) {
                RestoreAllToOriginalGame();
            } else {
                presets.ApplyPreset(kKinds[i], reg, s, o);
                Changed();
            }
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
    }
    ImGui::Dummy(ImVec2(0, em * 0.2f));
}

struct CategoryDef {
    TacticalCategory cat;
    const char* english_name;
    const char* icon;
};

constexpr CategoryDef kCategories[] = {
    {TacticalCategory::Graphics, "GRAPHICS & CINEMA", "[1]"},
    {TacticalCategory::PhysicsLab, "PHYSICS LAB", "[2]"},
    {TacticalCategory::CombatGameplay, "COMBAT & GAMEPLAY", "[3]"},
    {TacticalCategory::AudioLanguage, "AUDIO & LANGUAGE", "[4]"},
    {TacticalCategory::Interface, "CONTROLS & HUD", "[5]"},
    {TacticalCategory::CheatsWeapons, "WEAPONS & CHEATS", "[6]"}
};
constexpr int kNumCats = int(std::size(kCategories));

void DrawCategorySidebar(float width, float height, float em) {
    TacticalTheme theme = CurrentTheme();
    float item_spacing_y = ImGui::GetStyle().ItemSpacing.y;
    float avail_h = height > 0 ? height : ImGui::GetContentRegionAvail().y;
    float btn_h = (avail_h - item_spacing_y * float(kNumCats - 1)) / float(kNumCats);
    btn_h = std::max(btn_h, em * 2.2f);

    float target_w = (width > 0) ? width : ImGui::GetContentRegionAvail().x;

    for (int idx = 0; idx < kNumCats; idx++) {
        const auto& c = kCategories[idx];
        bool is_active = (g_active_category == c.cat);

        std::string raw_label = Ui(c.english_name);
        std::string display_label;
        if (is_active) {
            display_label = IsRtl() ? (raw_label + "  <<") : (">>  " + raw_label);
        } else {
            display_label = raw_label;
        }

        char btn_id[64];
        std::snprintf(btn_id, sizeof(btn_id), "%s###cat_nav_%d", display_label.c_str(), int(c.cat));

        if (is_active) {
            ImGui::PushStyleColor(ImGuiCol_Button, theme.highlight);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.button_hovered);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.button_hovered);
            ImGui::PushStyleColor(ImGuiCol_Text, theme.heading);
            ImGui::PushStyleColor(ImGuiCol_Border, theme.heading);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.5f);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, kTab);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kTabHovered);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.highlight);
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.35f, 0.50f, 0.6f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.2f);
        }

        if (ImGui::Button(btn_id, ImVec2(target_w, btn_h))) {
            g_active_category = c.cat;
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(5);
    }
}

void DrawCategoryContent(TacticalCategory cat, float em) {
    g_hovered_box = -1;
    switch (cat) {
        case TacticalCategory::Graphics: DrawCategoryGraphics(em); break;
        case TacticalCategory::PhysicsLab: DrawCategoryPhysicsLab(em); break;
        case TacticalCategory::CombatGameplay: DrawCategoryCombatGameplay(em); break;
        case TacticalCategory::AudioLanguage: DrawCategoryAudioLanguage(em); break;
        case TacticalCategory::Interface: DrawCategoryInterface(em); break;
        case TacticalCategory::CheatsWeapons: DrawCategoryCheats(em); break;
        default: DrawCategoryGraphics(em); break;
    }
}

void DrawTacticalFooter(float em) {
    auto& reg = modsettings::Registry();
    auto& s = app::Get();
    auto& o = game::Opt();
    bool restart_needed = reg.RequiresAppRestart(s, o);
    uint32_t restart_count = reg.PendingRestartCount(s, o);

    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, em * 0.15f));

    float bw_restore = em * 11.5f;
    float bw_close = em * 9.5f;
    float btn_h = em * 2.0f;
    float spacing_x = ImGui::GetStyle().ItemSpacing.x;
    float total_buttons_w = bw_restore + bw_close + spacing_x;
    float avail_w = ImGui::GetContentRegionAvail().x;

    if (!IsRtl()) {
        // LTR: Status hint on Left, Action buttons on Right
        if (restart_needed && reg.GetEnhanced().show_restart_banners) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.20f, 1.0f));
            char restart_msg[128];
            std::snprintf(restart_msg, sizeof(restart_msg), "(!) %s (%u)",
                          Ui("RESTART REQUIRED").c_str(), restart_count);
            ImGui::TextUnformatted(restart_msg);
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            const char* hint = "LB/RB: Categories | D-Pad: Adjust | Touch: Configure";
            ImGui::TextUnformatted(Ui(hint, false).c_str());
            ImGui::PopStyleColor();
        }

        ImGui::SameLine();
        float pad = avail_w - ImGui::GetCursorPosX() - total_buttons_w;
        if (pad > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);

        if (ImGui::Button(Ui("RESTORE DEFAULTS", false).c_str(), ImVec2(bw_restore, btn_h))) {
            RestoreAllToOriginalGame();
        }
        ImGui::SameLine();
        if (ImGui::Button(Ui("CLOSE CONSOLE", false).c_str(), ImVec2(bw_close, btn_h))) {
            ToggleMenu();
        }
    } else {
        // RTL: Action buttons on Left, Status hint on Right
        if (ImGui::Button(Ui("CLOSE CONSOLE", false).c_str(), ImVec2(bw_close, btn_h))) {
            ToggleMenu();
        }
        ImGui::SameLine();
        if (ImGui::Button(Ui("RESTORE DEFAULTS", false).c_str(), ImVec2(bw_restore, btn_h))) {
            RestoreAllToOriginalGame();
        }
        ImGui::SameLine();

        float remaining_w = avail_w - (bw_close + bw_restore + spacing_x * 2.0f);
        if (remaining_w > 0) {
            std::string status_txt;
            if (restart_needed && reg.GetEnhanced().show_restart_banners) {
                char restart_msg[128];
                std::snprintf(restart_msg, sizeof(restart_msg), "(!) %s (%u)",
                              Ui("RESTART REQUIRED").c_str(), restart_count);
                status_txt = restart_msg;
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.20f, 1.0f));
            } else {
                status_txt = Ui("LB/RB: SWITCH TABS | TOUCH TO CONFIGURE");
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            }
            float text_w = ImGui::CalcTextSize(status_txt.c_str()).x;
            float text_pos = ImGui::GetCursorPosX() + remaining_w - text_w;
            if (text_pos > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(text_pos);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(status_txt.c_str());
            ImGui::PopStyleColor();
        }
    }
}

void DrawPanel() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    TacticalTheme theme = CurrentTheme();

    // Gamepad bumper category switching
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1)) {
        int next = (int(g_active_category) - 1 + kNumCats) % kNumCats;
        g_active_category = TacticalCategory(next);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1)) {
        int next = (int(g_active_category) + 1) % kNumCats;
        g_active_category = TacticalCategory(next);
    }

    // Controller shortcut to close: Gamepad B (East) or Back button
    if ((ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight) || ImGui::IsKeyPressed(ImGuiKey_GamepadBack)) && CaptureFree()) {
        ToggleMenu();
        return;
    }

    // Responsive Landscape ~92% width and 90% height
    float win_w = vp->WorkSize.x * 0.92f;
    float win_h = vp->WorkSize.y * 0.90f;
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(win_w, win_h), ImGuiCond_Always);

    std::pair<ImGuiCol, ImVec4> panel_colors[] = {
        {ImGuiCol_WindowBg, kPanelBg}, {ImGuiCol_Border, theme.border}, {ImGuiCol_Text, kText},
        {ImGuiCol_Header, theme.highlight}, {ImGuiCol_HeaderHovered, theme.highlight}, {ImGuiCol_HeaderActive, theme.highlight},
        {ImGuiCol_Button, theme.button}, {ImGuiCol_ButtonHovered, theme.button_hovered}, {ImGuiCol_ButtonActive, theme.button_hovered},
        {ImGuiCol_FrameBg, kTab}, {ImGuiCol_FrameBgHovered, kTabHovered}, {ImGuiCol_FrameBgActive, theme.highlight},
        {ImGuiCol_ScrollbarBg, ImVec4(0.07f, 0.10f, 0.15f, 0.85f)},
        {ImGuiCol_ScrollbarGrab, ImVec4(0.24f, 0.44f, 0.68f, 0.85f)},
        {ImGuiCol_ScrollbarGrabHovered, ImVec4(0.35f, 0.60f, 0.90f, 1.0f)},
        {ImGuiCol_ScrollbarGrabActive, ImVec4(0.45f, 0.75f, 1.0f, 1.0f)},
        {ImGuiCol_Separator, theme.line}, {ImGuiCol_NavHighlight, theme.heading},
        {ImGuiCol_Tab, kTab}, {ImGuiCol_TabHovered, kTabHovered}, {ImGuiCol_TabActive, theme.highlight},
        {ImGuiCol_TabUnfocused, kTab}, {ImGuiCol_TabUnfocusedActive, theme.highlight}
    };
    for (const auto& [col, color] : panel_colors) ImGui::PushStyleColor(col, color);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.2f, em * 0.9f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em * 0.6f, em * 0.45f));

    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;

    if (ImGui::Begin("##tactical_console", nullptr, kFlags)) {
        float avail_w = ImGui::GetContentRegionAvail().x;
        float close_btn_w = em * 3.4f;
        float close_btn_h = em * 2.0f;

        // Tactical Header with Prominent Close [X] Button
        if (!IsRtl()) {
            // LTR Header: Title on Left, Version + Close [X] on Right
            ImGui::PushStyleColor(ImGuiCol_Text, theme.heading);
            ImGui::TextUnformatted(Ui("BLACK ENHANCED EDITION — TACTICAL OPS CONSOLE").c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextUnformatted(Ui("v1.3.0-enhanced (BUILD 34)", false).c_str());
            ImGui::PopStyleColor();

            ImGui::SameLine();
            float pad_x = avail_w - ImGui::GetCursorPosX() - close_btn_w;
            if (pad_x > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad_x);

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.15f, 0.12f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.20f, 0.16f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, theme.heading);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);
            if (ImGui::Button("[ X ]###hdr_close", ImVec2(close_btn_w, close_btn_h))) {
                ToggleMenu();
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);
        } else {
            // RTL Header: Close [X] on Left, Title + Version on Right
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.15f, 0.12f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.20f, 0.16f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, theme.heading);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);
            if (ImGui::Button("[ X ]###hdr_close", ImVec2(close_btn_w, close_btn_h))) {
                ToggleMenu();
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);

            ImGui::SameLine();
            std::string shaped_title = Ui("BLACK ENHANCED EDITION — TACTICAL OPS CONSOLE");
            std::string build_tag = Ui("v1.3.0-enhanced (BUILD 34)", false) + " ";
            float title_w = ImGui::CalcTextSize(shaped_title.c_str()).x;
            float build_w = ImGui::CalcTextSize(build_tag.c_str()).x;
            float total_w = title_w + build_w + ImGui::GetStyle().ItemSpacing.x;

            float pad_x = avail_w - ImGui::GetCursorPosX() - total_w;
            if (pad_x > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad_x);

            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(build_tag.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.heading);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(shaped_title.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, em * 0.15f));

        // Presets Selector Bar
        DrawPresetSelector(em);
        ImGui::Dummy(ImVec2(0, em * 0.15f));

        // Calculate Split Panel Dimensions
        float footer_reserved = em * 2.8f;
        float workspace_h = ImGui::GetContentRegionAvail().y - footer_reserved;
        float sidebar_w = std::clamp(win_w * 0.23f, em * 13.5f, em * 17.5f);
        float spacing_x = ImGui::GetStyle().ItemSpacing.x;
        float content_w = ImGui::GetContentRegionAvail().x - sidebar_w - spacing_x;

        // Detect category change to reset scroll position cleanly
        static TacticalCategory s_last_category = TacticalCategory::Graphics;
        bool cat_changed = (g_active_category != s_last_category);
        if (cat_changed) {
            s_last_category = g_active_category;
            ResetScroll("content");
        }

        if (!IsRtl()) {
            // LTR: Sidebar Left, Content Right
            ImGui::BeginChild("##category_sidebar", ImVec2(sidebar_w, workspace_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
            DragScroll(em, "sidebar");
            DrawCategorySidebar(sidebar_w, workspace_h, em);
            ImGui::EndChild();

            ImGui::SameLine();

            ImGui::BeginChild("##category_content", ImVec2(0, workspace_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
            if (cat_changed) ImGui::SetScrollY(0.0f);
            DragScroll(em, "content");
            DrawCategoryContent(g_active_category, em);
            ImGui::EndChild();
        } else {
            // RTL: Content Left, Sidebar Right
            ImGui::BeginChild("##category_content", ImVec2(content_w, workspace_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
            if (cat_changed) ImGui::SetScrollY(0.0f);
            DragScroll(em, "content");
            DrawCategoryContent(g_active_category, em);
            ImGui::EndChild();

            ImGui::SameLine();

            ImGui::BeginChild("##category_sidebar", ImVec2(0, workspace_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
            DragScroll(em, "sidebar");
            DrawCategorySidebar(0, workspace_h, em);
            ImGui::EndChild();
        }

        // Tactical Footer
        DrawTacticalFooter(em);
    }
    ImGui::End();
    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor(int(std::size(panel_colors)));
}

// the window's display's refresh rate, 0 when unreadable
uint32_t RefreshRate() {
    SDL_Window* w = static_cast<SDL_Window*>(Window());
    const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(w ? SDL_GetDisplayForWindow(w) : SDL_GetPrimaryDisplay());
    return m && m->refresh_rate > 1 ? uint32_t(std::lround(m->refresh_rate)) : 0;
}

// as app::Pace, at this cap
void PaceAt(uint32_t cap) {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace

void PaceFrame() {
    if (Get().fps_cap != kFpsCapRefresh) {
        Pace();
        return;
    }
    // a few under, so a variable refresh display stays in its range and vsync never waits a whole refresh
    constexpr uint32_t kRefreshMargin = 3;
    uint32_t rate = RefreshRate();
    PaceAt(rate > kRefreshMargin ? rate - kRefreshMargin : 0);
}

const rows::Row* rows::Page(int page, size_t* count) {
    if (page == kKeyboardPage) {
        *count = InputRows().size();
        return InputRows().data();
    }
    if (page == kCheatsPage) {
        *count = std::size(kCheatRows);
        return kCheatRows;
    }
    if (page == kCreditsPage) {
        *count = CreditRows().size();
        return CreditRows().data();
    }
    if (page == kTouchPage) {
        *count = std::size(kTouchRows);
        return kTouchRows;
    }
    if (page == kGeneralPage) {
        *count = std::size(kGeneralRows);
        return kGeneralRows;
    }
    *count = std::size(kGraphicsRows);
    return kGraphicsRows;
}

const rows::Row* rows::Controller(size_t* count) {
    *count = std::size(kControllerRows);
    return kControllerRows;
}

std::string rows::Value(const Row& row, int step) {
    if (row.kind == Kind::Bind) return KeyText(input::Binding(size_t(row.id)));
    if (row.id == kCredit) {
        // a step on a link opens it
        const Credit* c = CreditOf(row);
        if (!c || !c->value) return " ";
        if (step && c->url) SDL_OpenURL(c->url);
        return c->value;
    }
    return RowText(row.id, step);
}

void rows::Defaults(int page) { RestoreDefaults(page); }

// the recomp's own pages share one set of defaults, too much to undo from one of them
bool rows::Restorable(int page) { return page == kKeyboardPage || page == kCheatsPage; }

// the dynamic resolution's rows while it is on, RENDER RESOLUTION while it is off
bool rows::Enabled(const Row& row) {
    if (row.kind != Kind::Value) return true;
    bool dynamic = game::Opt().dynamic_resolution;
    switch (row.id) {
        case kRenderResolution: return true;
        case kDynamicTarget:
        case kDynamicMin:
        case kDynamicMax:
        case kDynamicHold: return dynamic;
        default: return true;
    }
}

bool rows::Opens(const Row& row) {
    // the credits' rows have no arrows: the links open, the rest do nothing
    return row.kind == Kind::Value && (row.id == kTouchEdit || row.id == kCheckUpdates || row.id == kCredit);
}

bool MenuOpen() { return g_open.load(std::memory_order_relaxed); }

bool Capturing() { return g_capture >= 0; }

void ToggleMenu() {
    bool open = !MenuOpen();
    g_open.store(open, std::memory_order_relaxed);
    g_capture = -1;
    if (open) input::ReleaseCursor();
}

void DrawUi(double fps) {
    game::g_fps = float(fps);
    ImGuiIO& io = ImGui::GetIO();
    if (MenuOpen()) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.KeyRepeatRate = 0.1f;  // held arrows step ten times a second, ImGui's twenty is too fast for settings
    // scale the font with the output height
    io.FontGlobalScale = std::max(1.15f, io.DisplaySize.y / 680.0f);
    if (MenuOpen()) {
        PollCapture();
        DrawPanel();
    } else {
        game::flash_arabic::DrawOverlay(io.DisplaySize.x, io.DisplaySize.y);
    game::subtitles::DrawOverlay(io.DisplaySize.x, io.DisplaySize.y);
    }
}

}  // namespace app
