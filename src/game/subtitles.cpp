#include "game/subtitles.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "common/arabic.h"
#include "common/mod_schema.h"
#include "common/options.h"
#include "game/arabic_strings.h"

namespace game {
namespace subtitles {

namespace {

struct ActiveMessage {
    std::string text;
    std::string header;
    int64_t started_ms = 0;
    int64_t expires_ms = 0;
    bool is_objective = false;
    uint32_t string_id = 0;
};

std::mutex g_lock;
ActiveMessage g_active_subtitle;
ActiveMessage g_active_objective;
int64_t g_last_id_time = 0;
uint32_t g_last_string_id = 0;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

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

}  // namespace

void Init() {
    std::lock_guard lock(g_lock);
    g_active_subtitle = {};
    g_active_objective = {};
    g_last_id_time = 0;
    g_last_string_id = 0;
}

void Clear() {
    std::lock_guard lock(g_lock);
    g_active_subtitle.expires_ms = 0;
    g_active_objective.expires_ms = 0;
}

void OnStringResolved(uint32_t id, const char* guest_text) {
    auto& reg = modsettings::Registry();
    if (!reg.GetEnhanced().arabic_subtitles_overlay) return;

    int64_t now = NowMs();

    // Debounce duplicate queries within 2.0 seconds
    {
        std::lock_guard lock(g_lock);
        if (id == g_last_string_id && (now - g_last_id_time) < 2000) {
            return;
        }
    }

    const ArabicStringEntry* entry = FindArabicString(id);
    if (!entry && (!guest_text || !guest_text[0])) {
        return;
    }

    bool is_arabic = (game::Opt().language == "ara" || reg.GetEnhanced().arabic_ui_enabled);
    std::string text;
    std::string header;
    bool is_objective = false;
    float duration_sec = 5.0f;

    StringCategory cat = CategoryOf(id);

    if (entry) {
        if (is_arabic && entry->arabic_u8) {
            text = entry->arabic_u8;
        } else if (entry->english) {
            text = entry->english;
        } else if (guest_text) {
            text = guest_text;
        }

        switch (cat) {
            case StringCategory::Objective:
                header = is_arabic ? "تحديث المهمة الرئيسية" : "PRIMARY OBJECTIVE UPDATE";
                is_objective = true;
                duration_sec = 6.0f;
                break;
            case StringCategory::Intel:
                header = is_arabic ? "معلومات بلاك التكتيكية" : "TACTICAL BLACK INTEL";
                duration_sec = 6.5f;
                break;
            case StringCategory::Prompt:
                header = is_arabic ? "إرشادات ميدانية" : "FIELD DIRECTIVE";
                duration_sec = 4.5f;
                break;
            case StringCategory::Subtitle:
                header = is_arabic ? "مكالمة لاسلكية" : "RADIO TRANSMISSION";
                duration_sec = 5.5f;
                break;
            case StringCategory::Hud:
                header = is_arabic ? "تقرير العمليات" : "STATUS REPORT";
                duration_sec = 4.0f;
                break;
            default:
                break;
        }
    } else {
        // Fallback for known guest text patterns
        text = guest_text ? guest_text : "";
        if (text.find("intel:") != std::string::npos || text.find("INTEL:") != std::string::npos) {
            header = is_arabic ? "معلومات تكتيكية" : "TACTICAL INTEL";
            duration_sec = 6.0f;
        } else if (text.find("objective") != std::string::npos || text.find("OBJECTIVE") != std::string::npos) {
            header = is_arabic ? "أهداف المهمة" : "MISSION OBJECTIVE";
            is_objective = true;
            duration_sec = 6.0f;
        }
    }

    if (text.empty()) return;

    std::lock_guard lock(g_lock);
    g_last_string_id = id;
    g_last_id_time = now;

    if (is_objective) {
        g_active_objective.text = std::move(text);
        g_active_objective.header = std::move(header);
        g_active_objective.started_ms = now;
        g_active_objective.expires_ms = now + int64_t(duration_sec * 1000.0f);
        g_active_objective.is_objective = true;
        g_active_objective.string_id = id;
    } else {
        g_active_subtitle.text = std::move(text);
        g_active_subtitle.header = std::move(header);
        g_active_subtitle.started_ms = now;
        g_active_subtitle.expires_ms = now + int64_t(duration_sec * 1000.0f);
        g_active_subtitle.is_objective = false;
        g_active_subtitle.string_id = id;
    }
}

void ShowSubtitle(const char* text_en, const char* text_ar, float duration_sec) {
    auto& reg = modsettings::Registry();
    bool is_arabic = (game::Opt().language == "ara" || reg.GetEnhanced().arabic_ui_enabled);
    const char* chosen = (is_arabic && text_ar) ? text_ar : text_en;
    if (!chosen || !chosen[0]) return;

    int64_t now = NowMs();
    std::lock_guard lock(g_lock);
    g_active_subtitle.text = chosen;
    g_active_subtitle.header = is_arabic ? "مكالمة لاسلكية" : "RADIO TRANSMISSION";
    g_active_subtitle.started_ms = now;
    g_active_subtitle.expires_ms = now + int64_t(duration_sec * 1000.0f);
    g_active_subtitle.is_objective = false;
    g_active_subtitle.string_id = 0;
}

void ShowObjective(const char* header, const char* text_en, const char* text_ar, float duration_sec) {
    auto& reg = modsettings::Registry();
    bool is_arabic = (game::Opt().language == "ara" || reg.GetEnhanced().arabic_ui_enabled);
    const char* chosen = (is_arabic && text_ar) ? text_ar : text_en;
    if (!chosen || !chosen[0]) return;

    int64_t now = NowMs();
    std::lock_guard lock(g_lock);
    g_active_objective.text = chosen;
    g_active_objective.header = header ? header : (is_arabic ? "تحديث المهمة الرئيسية" : "PRIMARY OBJECTIVE UPDATE");
    g_active_objective.started_ms = now;
    g_active_objective.expires_ms = now + int64_t(duration_sec * 1000.0f);
    g_active_objective.is_objective = true;
    g_active_objective.string_id = 0;
}

void DrawOverlay(float screen_w, float screen_h) {
    auto& reg = modsettings::Registry();
    if (!reg.GetEnhanced().arabic_subtitles_overlay) return;

    int64_t now = NowMs();
    ActiveMessage sub_copy;
    ActiveMessage obj_copy;

    {
        std::lock_guard lock(g_lock);
        sub_copy = g_active_subtitle;
        obj_copy = g_active_objective;
    }

    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    if (!draw_list) return;

    ImFont* font = ImGui::GetFont();
    float font_size = ImGui::GetFontSize();
    bool is_arabic = (game::Opt().language == "ara" || reg.GetEnhanced().arabic_ui_enabled);

    // 1. Draw Top Objective Banner if active
    if (obj_copy.expires_ms > now) {
        float alpha = CalculateAlpha(now, obj_copy.started_ms, obj_copy.expires_ms);
        if (alpha > 0.01f) {
            std::string wrapped_text = arabic::WrapText(obj_copy.text, 52);
            std::string shaped_body = is_arabic ? arabic::ShapeAndBidiMultiline(wrapped_text) : wrapped_text;
            std::string shaped_hdr = is_arabic ? arabic::ShapeAndBidi(obj_copy.header) : obj_copy.header;

            ImVec2 hdr_sz = font->CalcTextSizeA(font_size * 0.85f, FLT_MAX, 0.0f, shaped_hdr.c_str());
            ImVec2 body_sz = font->CalcTextSizeA(font_size * 1.05f, FLT_MAX, 0.0f, shaped_body.c_str());

            float pad_x = font_size * 1.4f;
            float pad_y = font_size * 0.7f;
            float box_w = std::max(hdr_sz.x, body_sz.x) + pad_x * 2.0f;
            float box_h = hdr_sz.y + body_sz.y + pad_y * 2.4f;

            float box_x = (screen_w - box_w) * 0.5f;
            float box_y = screen_h * 0.07f;  // Top 7% of screen

            ImU32 bg_col = ImColor(0.06f, 0.09f, 0.14f, 0.88f * alpha);
            ImU32 border_col = ImColor(0.85f, 0.65f, 0.18f, 0.90f * alpha);  // Tactical Gold
            ImU32 hdr_col = ImColor(0.95f, 0.78f, 0.25f, 1.0f * alpha);
            ImU32 body_col = ImColor(1.0f, 1.0f, 1.0f, 1.0f * alpha);

            draw_list->AddRectFilled(ImVec2(box_x, box_y), ImVec2(box_x + box_w, box_y + box_h), bg_col, 4.0f);
            draw_list->AddRect(ImVec2(box_x, box_y), ImVec2(box_x + box_w, box_y + box_h), border_col, 4.0f, 0, 2.0f);

            // Draw header centered
            float hdr_x = box_x + (box_w - hdr_sz.x) * 0.5f;
            float hdr_y = box_y + pad_y * 0.8f;
            draw_list->AddText(font, font_size * 0.85f, ImVec2(hdr_x, hdr_y), hdr_col, shaped_hdr.c_str());

            // Draw body centered
            float body_x = box_x + (box_w - body_sz.x) * 0.5f;
            float body_y = hdr_y + hdr_sz.y + pad_y * 0.5f;
            draw_list->AddText(font, font_size * 1.05f, ImVec2(body_x, body_y), body_col, shaped_body.c_str());
        }
    }

    // 2. Draw Bottom Subtitle Box if active
    if (sub_copy.expires_ms > now) {
        float alpha = CalculateAlpha(now, sub_copy.started_ms, sub_copy.expires_ms);
        if (alpha > 0.01f) {
            std::string wrapped_text = arabic::WrapText(sub_copy.text, 48);
            std::string shaped_body = is_arabic ? arabic::ShapeAndBidiMultiline(wrapped_text) : wrapped_text;
            std::string shaped_hdr = is_arabic ? arabic::ShapeAndBidi(sub_copy.header) : sub_copy.header;

            ImVec2 hdr_sz = sub_copy.header.empty()
                                ? ImVec2(0, 0)
                                : font->CalcTextSizeA(font_size * 0.75f, FLT_MAX, 0.0f, shaped_hdr.c_str());
            ImVec2 body_sz = font->CalcTextSizeA(font_size * 1.10f, FLT_MAX, 0.0f, shaped_body.c_str());

            float pad_x = font_size * 1.5f;
            float pad_y = font_size * 0.75f;
            float box_w = std::clamp(std::max(hdr_sz.x, body_sz.x) + pad_x * 2.0f, screen_w * 0.40f, screen_w * 0.88f);
            float box_h = (hdr_sz.y > 0 ? hdr_sz.y + pad_y * 0.4f : 0.0f) + body_sz.y + pad_y * 2.0f;

            float box_x = (screen_w - box_w) * 0.5f;
            float box_y = screen_h * 0.80f - (box_h * 0.5f);  // Centered vertically around 80%

            ImU32 bg_col = ImColor(0.04f, 0.06f, 0.10f, 0.85f * alpha);
            ImU32 border_col = ImColor(0.20f, 0.42f, 0.65f, 0.80f * alpha);  // Tactical Cyan / Slate
            ImU32 hdr_col = ImColor(0.35f, 0.70f, 0.95f, 0.95f * alpha);
            ImU32 body_col = ImColor(0.98f, 0.98f, 0.98f, 1.0f * alpha);

            draw_list->AddRectFilled(ImVec2(box_x, box_y), ImVec2(box_x + box_w, box_y + box_h), bg_col, 6.0f);
            draw_list->AddRect(ImVec2(box_x, box_y), ImVec2(box_x + box_w, box_y + box_h), border_col, 6.0f, 0, 1.8f);

            float cur_y = box_y + pad_y;
            if (!sub_copy.header.empty()) {
                float hdr_x = box_x + (box_w - hdr_sz.x) * 0.5f;
                draw_list->AddText(font, font_size * 0.75f, ImVec2(hdr_x, cur_y), hdr_col, shaped_hdr.c_str());
                cur_y += hdr_sz.y + pad_y * 0.4f;
            }

            float body_x = box_x + (box_w - body_sz.x) * 0.5f;
            draw_list->AddText(font, font_size * 1.10f, ImVec2(body_x, cur_y), body_col, shaped_body.c_str());
        }
    }
}

}  // namespace subtitles
}  // namespace game

