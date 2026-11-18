#include "game/flash_arabic.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/arabic.h"
#include "common/mod_schema.h"
#include "common/options.h"
#include "common/translations.h"
#include "core/log.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;

X86_EXTERN_FUNC(__imp__sub_00121E60);
X86_EXTERN_FUNC(__imp__sub_00121BC0);
X86_EXTERN_FUNC(__imp__sub_000E4840);
X86_EXTERN_FUNC(__imp__sub_000D9600);

namespace {

struct OverlayTextItem {
    std::string raw_arabic;
    float px = 0.0f;
    float py = 0.0f;
    float width_px = 0.0f;
    float height_px = 0.0f;
    float font_px = 18.0f;
    uint8_t align = 0;  // 0 = left, 1 = center, 2 = right
    uint8_t r = 220;
    uint8_t g = 220;
    uint8_t b = 220;
    uint8_t a = 255;
};

struct StageFrameItem {
    std::string raw_arabic;
    float d3d_x0 = 0.0f;
    float d3d_y0 = 0.0f;
    float d3d_x1 = 0.0f;
    float d3d_y1 = 0.0f;
    float font_d3d_h = 15.0f;
    uint8_t align = 0;
    uint8_t r = 220;
    uint8_t g = 220;
    uint8_t b = 220;
    uint8_t a = 255;
};

std::mutex g_overlay_mutex;
std::vector<StageFrameItem> g_staging_items;
std::vector<StageFrameItem> g_active_items;
std::chrono::steady_clock::time_point g_last_flash_frame{};
std::atomic<bool> g_in_flash{false};

std::mutex g_log_mutex;
std::unordered_set<std::string> g_logged_untranslated;
std::unordered_set<std::string> g_logged_translated;

bool IsArabicUiActive() {
    return game::Opt().language == "ara" || modsettings::Registry().GetEnhanced().arabic_ui_enabled;
}

std::string Utf16GuestToUtf8(uint32_t guest_ptr, size_t max_chars = 512) {
    if (!guest_ptr) return {};
    std::string out;
    out.reserve(64);
    for (size_t i = 0; i < max_chars; ++i) {
        uint16_t ch = G<uint16_t>(guest_ptr + uint32_t(i * 2));
        if (ch == 0) break;
        if (ch < 0x80) {
            out.push_back(static_cast<char>(ch));
        } else if (ch < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (ch >> 12)));
            out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }
    return out;
}

std::string GuestCString(uint32_t guest_ptr, size_t max_chars = 64) {
    if (!guest_ptr) return {};
    std::string out;
    for (size_t i = 0; i < max_chars; ++i) {
        uint8_t c = G<uint8_t>(guest_ptr + uint32_t(i));
        if (c == 0) break;
        out.push_back(static_cast<char>(c));
    }
    return out;
}

std::string TrimCopy(std::string_view s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return std::string(s.substr(start, end - start));
}

// Normalizes CRLF/whitespace and lowercases ASCII characters for case-insensitive matching.
std::string NormalizeKey(std::string_view s) {
    std::string trimmed = TrimCopy(s);
    std::string out;
    out.reserve(trimmed.size());
    bool prev_space = false;
    for (size_t i = 0; i < trimmed.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(trimmed[i]);
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            if (!prev_space) {
                out.push_back(' ');
                prev_space = true;
            }
            continue;
        }
        prev_space = false;
        // Map UTF-8 right single quotation mark (U+2019: E2 80 99) to ASCII '\''
        if (c == 0xE2 && i + 2 < trimmed.size() &&
            static_cast<unsigned char>(trimmed[i + 1]) == 0x80 &&
            static_cast<unsigned char>(trimmed[i + 2]) == 0x99) {
            out.push_back('\'');
            i += 2;
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

bool ContainsArabicCodepoint(std::string_view utf8) {
    for (size_t i = 0; i + 1 < utf8.size(); ++i) {
        uint8_t b0 = static_cast<uint8_t>(utf8[i]);
        if ((b0 & 0xE0) == 0xC0) {
            uint16_t cp = ((b0 & 0x1F) << 6) | (static_cast<uint8_t>(utf8[i + 1]) & 0x3F);
            if (cp >= 0x0600 && cp <= 0x06FF) return true;
        } else if ((b0 & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
            uint16_t cp = ((b0 & 0x0F) << 12) |
                          ((static_cast<uint8_t>(utf8[i + 1]) & 0x3F) << 6) |
                          (static_cast<uint8_t>(utf8[i + 2]) & 0x3F);
            if ((cp >= 0xFB50 && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF)) return true;
        }
    }
    return false;
}

bool HasPresentationForms(std::string_view utf8) {
    for (size_t i = 0; i + 2 < utf8.size(); ++i) {
        uint8_t b0 = static_cast<uint8_t>(utf8[i]);
        if ((b0 & 0xF0) == 0xE0) {
            uint16_t cp = ((b0 & 0x0F) << 12) |
                          ((static_cast<uint8_t>(utf8[i + 1]) & 0x3F) << 6) |
                          (static_cast<uint8_t>(utf8[i + 2]) & 0x3F);
            if ((cp >= 0xFB50 && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF)) return true;
        }
    }
    return false;
}

bool IsSymbolOrPureNumeric(std::string_view norm) {
    if (norm.empty()) return true;
    bool has_letter = false;
    for (unsigned char c : norm) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            has_letter = true;
            break;
        }
    }
    return !has_letter;
}

struct DictPair {
    const char* key;
    const char* ar;
};

constexpr DictPair kFlashDict[] = {
    // Press Start / Copyright / Loading
    {"loading", "جارٍ التحميل..."},
    {"loading...", "جارٍ التحميل..."},
    {"loading pause menus", "جارٍ تحميل قوائم الإيقاف..."},
    {"loading. please don't turn off your xbox console.", "جارٍ التحميل. يرجى عدم إغلاق الجهاز."},
    {"press anywhere", "اضغط في أي مكان للمتابعة"},
    {"press start", "اضغط للبدء"},
    {"\xc2\xa9 2006 electronic arts inc. all rights reserved.", "(C) 2006 Electronic Arts Inc. جميع الحقوق محفوظة."},
    {"electronic arts inc.", "Electronic Arts Inc."},

    // Touch / Button Navigation Prompts
    {"(drag) select", "(اسحب) للتحديد"},
    {"(tap) confirm", "(انقر) للتأكيد"},
    {"(<) back", "(<) رجوع"},
    {"back", "رجوع"},
    {"confirm", "تأكيد"},
    {"select", "تحديد"},
    {"change", "تغيير"},
    {"ok", "موافق"},
    {"cancel", "إلغاء"},
    {"yes", "نعم"},
    {"no", "لا"},
    {"done", "تم"},
    {"skip", "تخطي"},
    {"retry", "إعادة المحاولة"},
    {"continue", "متابعة"},
    {"quit", "خروج"},

    // Main Menu & Pause Menu
    {"start mission", "بدء المهمة"},
    {"campaign", "الحملة"},
    {"continue mission", "متابعة المهمة"},
    {"options", "الخيارات"},
    {"credits", "فريق العمل"},
    {"pause", "إيقاف مؤقت"},
    {"pause menu", "قائمة الإيقاف المؤقت"},
    {"pause menu options", "خيارات الإيقاف المؤقت"},
    {"resume", "استئناف"},
    {"restart", "إعادة المحاولة"},
    {"restart mission", "إعادة المهمة"},
    {"are you sure you want to quit?", "هل أنت متأكد من أنك تريد الخروج؟"},
    {"all checkpoint progress will be lost! are you sure you want to quit the mission?",
     "سيتم فقدان كل التقدم منذ آخر نقطة حفظ! هل أنت متأكد من مغادرة المهمة؟"},
    {"all checkpoint progress will be lost! are you sure you want to restart the mission?",
     "سيتم فقدان كل التقدم منذ آخر نقطة حفظ! هل أنت متأكد من إعادة المهمة؟"},
    {"if your progress has not been saved then it will be lost. continue anyway?",
     "إذا لم تحفظ تقدمك فسيتم فقدانه. هل تريد المتابعة على أي حال؟"},

    // Options Menus (Original + Recomp options_menu.cpp)
    {"profile", "الملف الشخصي"},
    {"operator", "العميل"},
    {"operator name", "اسم العميل"},
    {"operator profile", "ملف العميل"},
    {"picture setup", "إعدادات الصورة"},
    {"sound setup", "إعدادات الصوت"},
    {"control setup", "إعدادات التحكم"},
    {"custom control setup", "إعدادات التحكم المخصصة"},
    {"custom setup", "إعداد مخصص"},
    {"language", "اللغة"},
    {"arabic", "العربية"},
    {"english", "الإنجليزية"},
    {"english (restart)", "الإنجليزية (إعادة)"},
    {"arabic (restart)", "العربية (إعادة)"},
    {"skip intros", "تخطي المقدمات"},
    {"audio output", "مخرج الصوت"},
    {"- updates -", "- التحديثات -"},
    {"updates", "التحديثات"},
    {"check for updates", "فحص التحديثات"},
    {"startup update check", "فحص التحديث عند البدء"},
    {"graphics", "الرسومات"},
    {"touch controls", "التحكم باللمس"},
    {"general", "عام"},
    {"mouse and keyboard", "الفأرة ولوحة المفاتيح"},
    {"touch", "اللمس"},
    {"cheats", "الغش"},
    {"page", "الصفحة"},
    {"on", "مفعّل"},
    {"off", "معطّل"},
    {"automatic", "تلقائي"},
    {"check", "فحص"},
    {"brightness", "السطوع"},
    {"widescreen", "شاشة عريضة"},
    {"sfx volume", "صوت المؤثرات"},
    {"music volume", "صوت الموسيقى"},
    {"output", "مخرج الصوت"},
    {"mono", "أحادي"},
    {"stereo", "ستيريو"},
    {"surround", "محيطي"},
    {"dolby\xc2\xae pro logic\xc2\xae ii", "Dolby Pro Logic II"},
    {"subtitles", "الترجمة"},
    {"invert look", "عكس النظر"},
    {"toggle crouch", "تبديل الانحناء"},
    {"vibration", "الاهتزاز"},
    {"sensitivity", "الحساسية"},
    {"custom soundtrack", "موسيقى مخصصة"},
    {"next music track", "المقطع الموسيقي التالي"},
    {"shuffle", "عشوائي"},
    {"sequential", "متسلسل"},
    {"default", "افتراضي"},
    {"game default", "افتراضي اللعبة"},
    {"custom", "مخصص"},
    {"customise", "تخصيص"},
    {"manually configure", "إعداد يدوي"},
    {"restore defaults", "استعادة الافتراضي"},
    {"press a key", "اضغط على مفتاح"},
    {"unbound", "غير معيّن"},
    {"480p", "480p"},
    {"50hz", "50 هرتز"},
    {"60hz", "60 هرتز"},
    {"60hz test", "اختبار 60 هرتز"},
    {"the display is now in 480p (progressive scan mode). do you want to keep this display mode?",
     "الشاشة الآن في وضع 480p المسح التدريجي. هل تريد الإبقاء على هذا الوضع؟"},
    {"the display will now switch to progressive scan mode. this may not produce proper display results on your television. do you wish to continue?",
     "ستنتقل الشاشة الآن إلى وضع المسح التدريجي. هل تريد المتابعة؟"},
    {"if the image is unstable or flickers your television is not 60hz compatible. if the picture appears correctly your television supports 60hz.",
     "إذا كانت الصورة غير مستقرة أو تومض فشاشتك لا تدعم 60 هرتز. إذا ظهرت بوضوح فهي تدعم 60 هرتز."},
    {"50hz video is supported by all televisions. 60hz requires a television that is compatible with 60hz video. which mode would you like to use?",
     "وضع 50 هرتز مدعوم على جميع الشاشات، بينما يتطلب 60 هرتز شاشة متوافقة. أي وضع تفضل؟"},
    {"black supports 480p (progressive scan) video on compatible televisions. which mode would you like to use?",
     "تدعم اللعبة وضع 480p على الشاشات المتوافقة. أي وضع تريد استخدامه؟"},

    // Control Action Names
    {"look", "النظر"},
    {"look up/down", "النظر للأعلى/للأسفل"},
    {"turn left/right", "الالتفاف يميناً/يساراً"},
    {"run forward/back", "الجري للأمام/للخلف"},
    {"sidestep left/right", "الحركة الجانبية يميناً/يساراً"},
    {"primary fire", "إطلاق النار الأساسي"},
    {"secondary fire", "إطلاق النار الثانوي"},
    {"reload", "إعادة التلقيم"},
    {"crouch", "انحناء"},
    {"melee", "هجوم قريب"},
    {"grenade", "قنبلة يدوية"},
    {"grenades", "القنابل اليدوية"},
    {"aim", "التصويب"},
    {"change weapon", "تبديل السلاح"},
    {"change weapon up", "السلاح التالي"},
    {"change weapon down", "السلاح السابق"},
    {"change firing mode", "تغيير نمط الإطلاق"},
    {"hold breath", "حبس النفس"},
    {"pick up weapon", "التقاط السلاح"},
    {"left thumbstick", "العصا اليسرى"},
    {"right thumbstick", "العصا اليمنى"},
    {"click the left thumbstick", "اضغط العصا اليسرى"},
    {"click the right thumbstick", "اضغط العصا اليمنى"},
    {"left trigger", "الزناد الأيسر"},
    {"right trigger", "الزناد الأيمن"},
    {"southpaw", "أعسر"},
    {"standard", "قياسي"},
    {"several actions do not have a control assigned.", "توجد عدة إجراءات بدون زر مخصّص."},
    {"several controls are assigned to the same action.", "توجد عدة أزرار مخصّصة لنفس الإجراء."},
    {"please configure a control for the following actions:", "يرجى تعيين زر تحكم للإجراءات التالية:"},
    {"please configure only a single control for the following actions:", "يرجى تعيين زر تحكم واحد فقط للإجراءات التالية:"},
    {"press the button you wish to assign to this action now.", "اضغط الآن على الزر الذي تريد تعيينه لهذا الإجراء."},
    {"continue configuring", "متابعة الإعداد"},

    // Profile & Memory Card Messages (memcardmessage)
    {"checking xbox hard disk. please don't turn off your xbox console.",
     "جارٍ فحص وحدة التخزين. يرجى عدم إغلاق الجهاز."},
    {"saving profile. please don't turn off your xbox console.",
     "جارٍ حفظ الملف الشخصي. يرجى عدم إغلاق الجهاز."},
    {"black uses an autosave feature that will automatically save your progress through the game, and any options changes you make. autosave will overwrite the current profile without confirmation. do you want to enable the autosave feature?",
     "تستخدم اللعبة ميزة الحفظ التلقائي لحفظ تقدمك وتغييرات الإعدادات تلقائياً، وستقوم بالكتابة فوق الملف الشخصي الحالي دون تأكيد. هل تريد تفعيل الحفظ التلقائي؟"},
    {"are you sure you want to continue without the autosave feature? caution! if you choose to do so, your profile and options will not be saved automatically.",
     "هل أنت متأكد من المتابعة بدون ميزة الحفظ التلقائي؟ تنبيه: لن يتم حفظ ملفك وإعداداتك تلقائياً."},
    {"are you sure you want to continue without saving?", "هل أنت متأكد من المتابعة بدون حفظ؟"},
    {"autosave will be disabled. are you sure you want to continue without saving?",
     "سيتم تعطيل الحفظ التلقائي. هل أنت متأكد من المتابعة بدون حفظ؟"},
    {"autosave will be disabled. are you sure?", "سيتم تعطيل الحفظ التلقائي. هل أنت متأكد؟"},
    {"autosave will be enabled. are you sure?", "سيتم تفعيل الحفظ التلقائي. هل أنت متأكد؟"},
    {"do you want to save your profile now?", "هل تريد حفظ ملفك الشخصي الآن؟"},
    {"save successful.", "تم الحفظ بنجاح."},
    {"load successful.", "تم التحميل بنجاح."},
    {"new profile", "ملف شخصي جديد"},
    {"create profile", "إنشاء ملف شخصي"},
    {"save profile", "حفظ الملف الشخصي"},
    {"load profile", "تحميل الملف الشخصي"},
    {"delete profile", "حذف الملف الشخصي"},
    {"continue without saving", "المتابعة بدون حفظ"},
    {"free more blocks", "تحرير مساحة إضافية"},
    {"autosave", "الحفظ التلقائي"},
    {"save", "حفظ"},
    {"load", "تحميل"},
    {"new", "جديد"},
    {"blank", "فارغ"},
    {"<empty>", "<فارغ>"},
    {"<damaged>", "<تالف>"},
    {"profile appears to be damaged and cannot be used.", "يبدو أن الملف الشخصي تالف ولا يمكن استخدامه."},
    {"your xbox doesn't have enough free blocks to save games.", "لا توجد مساحة كافية لحفظ اللعبة."},
    {"alphabet", "الأحرف"},
    {"accents", "التشكيل"},
    {"symbols", "الرموز"},
    {"backspace", "مسح"},
    {"space", "مسافة"},
    {"shift", "تحويل"},
    {"caps lock", "أحرف كبيرة"},

    // Play Mode / Difficulty / Mission Select / Briefings / Challenges
    {"play mode", "وضع اللعب"},
    {"mission mode select", "اختيار وضع المهمة"},
    {"mission select", "اختيار المهمة"},
    {"difficulty select", "اختيار الصعوبة"},
    {"choose mission mode difficulty", "اختر صعوبة وضع المهمة"},
    {"difficulty", "الصعوبة"},
    {"easy", "سهل"},
    {"easy difficulty", "الصعوبة السهلة"},
    {"normal", "عادي"},
    {"normal difficulty", "الصعوبة العادية"},
    {"hard", "صعب"},
    {"hard difficulty", "الصعوبة الصعبة"},
    {"black ops", "العمليات السوداء"},
    {"are you sure you want to change difficulty? progress is tracked separately for each difficulty level.",
     "هل أنت متأكد من تغيير مستوى الصعوبة؟ يتم حفظ التقدم بشكل منفصل لكل مستوى."},
    {"primary objectives only. lots of healthpacks. no rewards.",
     "الأهداف الرئيسية فقط. الكثير من حقائب الإسعاف. بدون مكافآت."},
    {"primary and secondary objectives. normal amount of healthpacks. unlocks silver weapons.",
     "الأهداف الرئيسية والثانوية. عدد معتدل من حقائب الإسعاف. يفتح الأسلحة الفضية."},
    {"all primary, secondary and destruction objectives. no healthpacks. unlocks m16a2.",
     "جميع الأهداف الرئيسية والثانوية والتدميرية. بدون حقائب إسعاف. يفتح سلاح M16A2."},
    {"primary and secondary objectives. no healthpacks. unlocks silver weapons and black ops mode",
     "الأهداف الرئيسية والثانوية. بدون حقائب إسعاف. يفتح الأسلحة الفضية ووضع العمليات السوداء."},
    {"mission briefing", "إيجاز المهمة"},
    {"mission objectives", "أهداف المهمة"},
    {"black mission objectives", "أهداف مهمة BLACK"},
    {"mission info", "معلومات المهمة"},
    {"mission failed", "فشلت المهمة"},
    {"mission time", "وقت المهمة"},
    {"complete", "مكتمل"},
    {"incomplete", "غير مكتمل"},
    {"locked", "مقفل"},
    {"not available", "غير متاح"},
    {"congratulations", "تهانينا!"},
    {"all objectives complete", "تم إكمال جميع الأهداف"},
    {"results", "النتائج"},
    {"objectives", "الأهداف"},
    {"objective", "الهدف"},
    {"objective complete", "اكتمل الهدف"},
    {"objectives status", "حالة الأهداف"},
    {"primary", "رئيسي"},
    {"primary objective", "هدف رئيسي"},
    {"primary objective complete", "اكتمل الهدف الرئيسي"},
    {"primary objective failed", "فشل الهدف الرئيسي"},
    {"primary objective update", "تحديث الهدف الرئيسي"},
    {"complete all primary objectives", "أكمل جميع الأهداف الرئيسية"},
    {"complete your primary mission goals", "أكمل أهداف مهمتك الرئيسية"},
    {"secondary", "ثانوي"},
    {"destruction", "التدمير"},
    {"destruction objective update", "تحديث هدف التدمير"},
    {"destruction objective complete", "اكتمل هدف التدمير"},
    {"destruction objective failed", "فشل هدف التدمير"},
    {"new destruction objective", "هدف تدمير جديد"},
    {"new primary objective", "هدف رئيسي جديد"},
    {"no current destruction objective", "لا يوجد هدف تدمير حالي"},
    {"no current primary objective", "لا يوجد هدف رئيسي حالي"},
    {"recon", "الاستطلاع"},
    {"recon objective", "هدف استطلاع"},
    {"recon objective complete", "اكتمل هدف الاستطلاع"},
    {"armament", "التسليح"},
    {"armament objective", "هدف تسليح"},
    {"armament objective complete", "اكتمل هدف التسليح"},
    {"blackmail", "الابتزاز الاستخباراتي"},
    {"blackmail objective complete", "اكتمل هدف المعلومات السرية"},
    {"intel", "الاستخبارات"},
    {"intel objective complete", "اكتمل هدف الاستخبارات"},
    {"black intel", "معلومات BLACK"},
    {"location", "الموقع"},
    {"time", "الوقت"},
    {"time up", "انتهى الوقت"},
    {"checkpoint reached", "تم الوصول لنقطة الحفظ"},
    {"enemy count", "عدد الأعداء"},
    {"headshots", "إصابات الرأس"},
    {"head shot", "إصابة رأس"},
    {"number of headshots:", "عدد إصابات الرأس:"},
    {"mission rating:", "تقييم المهمة:"},
    {"current rank:", "الرتبة الحالية:"},
    {"current record", "الرقم القياسي الحالي"},
    {"current objective:", "الهدف الحالي:"},

    // Mission Titles
    {"veblensk city street", "شارع مدينة فيبلينسك"},
    {"treneska border crossing", "معبر ترينيسكا الحدودي"},
    {"naszran town", "بلدة نصران"},
    {"naszran foundry", "مسبك نصران"},
    {"tivliz asylum", "مصح تيفليز"},
    {"vratska dockyard", "حوض سفن فراتسكا"},
    {"graznei bridge", "جسر غرازني"},
    {"spetriniv gulag", "سجن سبيترينيف"},
    {"4 days earlier", "قبل 4 أيام"},

    // Medals & Ranks
    {"bronze", "برونزي"},
    {"silver", "فضي"},
    {"gold", "ذهبي"},
    {"black", "بلاك"},
    {"bronze medal awarded", "تم منح الميدالية البرونزية"},
    {"silver medal awarded", "تم منح الميدالية الفضية"},
    {"gold medal awarded", "تم منح الميدالية الذهبية"},
    {"black medal awarded", "تم منح ميدالية بلاك"},
    {"no medal awarded", "لم يتم منح ميدالية"},
    {"bronze medals", "الميداليات البرونزية"},
    {"silver medals", "الميداليات الفضية"},
    {"gold medals", "الميداليات الذهبية"},
    {"black medals", "ميداليات بلاك"},
    {"challenge medals", "ميداليات التحدي"},
    {"recruit", "مجند"},
    {"scout", "كشاف"},
    {"soldier", "جندي"},
    {"sergeant", "رقيب"},
    {"commando", "مغاوير"},
    {"elite", "نخبة"},
    {"task force", "قوة المهام"},
    {"special ops", "عمليات خاصة"},

    // Challenge & Bonus Modes
    {"challenge mode", "وضع التحدي"},
    {"challenge mode select", "اختيار وضع التحدي"},
    {"challenge mode overview", "نظرة عامة على وضع التحدي"},
    {"challenge mode details", "تفاصيل وضع التحدي"},
    {"challenge mode generic objective", "هدف وضع التحدي العام"},
    {"combat range", "ميدان الرماية"},
    {"combat range - blank", "ميدان الرماية - فارغ"},
    {"combat range select", "اختيار ميدان الرماية"},
    {"combat range start", "بدء ميدان الرماية"},
    {"danger room", "غرفة الخطر"},
    {"ranking ladder", "سلم التصنيف"},
    {"character viewer", "عارض الشخصيات"},
    {"object viewer", "عارض العناصر"},
    {"kill book", "سجل القتلى"},
    {"kill book kills", "قتلى سجل العمليات"},
    {"gun run", "مسار الأسلحة"},
    {"killing time", "سباق القتل"},
    {"headhunter", "صياد الرؤوس"},
    {"collateral damage", "أضرار جانبية"},
    {"blasted", "انفجار ساحق"},
    {"blown away", "تدمير شامل"},
    {"double kill", "قتل مزدوج"},
    {"multi kill", "قتل متعدد"},
    {"passing shot", "إصابة عابرة"},
    {"ricochet kill", "قتل بالارتداد"},

    // Weapons & Pickups
    {"weapon select", "اختيار السلاح"},
    {"select weapon", "اختر السلاح"},
    {"select your weapon", "اختر سلاحك"},
    {"choose your weapon:", "اختر سلاحك:"},
    {"choose your weapon challenge:", "اختر تحدي السلاح:"},
    {"single shot", "طلقة مفردة"},
    {"burst fire", "رشقات متقطعة"},
    {"fully automatic", "إطلاق آلي كامل"},
    {"long range", "مدى بعيد"},
    {"assault", "هجوم"},
    {"suppressor", "كاتم صوت"},
    {"health pack", "حقيبة إسعاف"},
    {"health packs full", "حقائب الإسعاف ممتلئة"},
    {"ak 47", "AK-47"},
    {"ak-47", "AK-47"},
    {"g36c", "G36C"},
    {"glock", "Glock"},
    {"dc3 elite", "DC3 Elite"},
    {"m16", "M16"},
    {"m16 a2", "M16 A2"},
    {"m16 a2 silver", "M16 A2 فضي"},
    {"m203", "M203"},
    {"m249", "M249"},
    {"mac10 elite", "MAC-10 Elite"},
    {"magnum", "Magnum"},
    {"mp5", "MP5"},
    {"p90", "P90"},
    {"remington 870", "Remington 870"},
    {"rpg", "RPG"},
    {"sniper rifle", "بندقية القنص"},
    {"spas 12", "SPAS-12"},
    {"suppressed uzi", "Uzi بكاتم صوت"},
    {"ak47 suppressor", "كاتم صوت AK-47"},
    {"g36c suppressor", "كاتم صوت G36C"},
    {"glock suppressor", "كاتم صوت Glock"},
    {"m16 suppressor", "كاتم صوت M16"},
    {"mac 10 suppressor", "كاتم صوت MAC-10"},
    {"mp5 suppressor", "كاتم صوت MP5"},
    {"p90 suppressor", "كاتم صوت P90"},
    {"smg suppressor", "كاتم صوت SMG"},
    {"suppressor for assault rifle", "كاتم صوت لبندقية الهجوم"},
    {"suppressor for grenades", "كاتم صوت للقنابل"},
    {"suppressor for m203", "كاتم صوت لسلاح M203"},
    {"suppressor for m249", "كاتم صوت لرشاش M249"},
    {"suppressor for magnum", "كاتم صوت لمسدس Magnum"},
    {"suppressor for pistol", "كاتم صوت للمسدس"},
    {"suppressor for rpg", "كاتم صوت لقاذف RPG"},
    {"suppressor for shotgun", "كاتم صوت لبندقية الرش"},
    {"suppressor for smg", "كاتم صوت للرشاش الخفيف"},
    {"suppressor for sniper rifle", "كاتم صوت لبندقية القنص"},

    // Common Tactical / Mission Objectives & Black Intel
    {"black intel: all weapons have a melee attack", "معلومات BLACK: جميع الأسلحة تملك هجوماً قريباً"},
    {"black intel: are you good enough to unlock the m16a2?", "معلومات BLACK: هل أنت ماهر بما يكفي لفتح M16A2؟"},
    {"black intel: are you good enough to use the silver weapons?", "معلومات BLACK: هل أنت ماهر بما يكفي لاستخدام الأسلحة الفضية؟"},
    {"black intel: blast objects out of the way to find secrets", "معلومات BLACK: فجّر العوائق في طريقك للعثور على الأسرار"},
    {"black intel: blurred crosshair indicates target is out of range", "معلومات BLACK: ضبابية المؤشر تعني أن الهدف خارج المدى"},
    {"black intel: can you find the sniper rifle near the vlodnik canal?", "معلومات BLACK: هل يمكنك إيجاد بندقية القنص قرب قناة فلودنيك؟"},
    {"black intel: collect mission information to complete recon objectives", "معلومات BLACK: اجمع معلومات المهمة لإكمال أهداف الاستطلاع"},
    {"black intel: complete armament objectives by finding hidden weapons", "معلومات BLACK: أكمل أهداف التسليح بالعثور على الأسلحة المخفية"},
    {"black intel: complete blackmail objectives by destroying secret laptops, briefcases and safes", "معلومات BLACK: أكمل أهداف الابتزاز بتدمير الحواسيب والحقائب والخزائن السرية"},
    {"black intel: complete intel objectives by finding hidden information", "معلومات BLACK: أكمل أهداف الاستخبارات بإيجاد المعلومات المخفية"},
    {"black intel: complete recon objectives by finding hidden blue document rolls", "معلومات BLACK: أكمل أهداف الاستطلاع بالعثور على لفائف الوثائق الزرقاء"},
    {"black intel: crouch behind cover to avoid enemy fire", "معلومات BLACK: انحنِ خلف السواتر لتجنب نيران العدو"},
    {"black intel: crouch behind cover to hide from enemies", "معلومات BLACK: انحنِ خلف الساتر للاختباء من الأعداء"},
    {"black intel: destroy information to complete blackmail objectives", "معلومات BLACK: دمّر المعلومات السرية لإكمال أهداف الابتزاز"},
    {"black intel: destroy vehicles to kill nearby enemies", "معلومات BLACK: دمّر المركبات للقضاء على الأعداء القريبين"},
    {"black intel: explore to find health packs", "معلومات BLACK: استكشف المكان للعثور على حقائب الإسعاف"},
    {"black intel: explore your surroundings to find suppressors", "معلومات BLACK: ابحث في محيطك عن كاتمات الصوت"},
    {"black intel: fallen enemies drop first aid kits", "معلومات BLACK: يسقط الأعداء القتلى حقائب إسعاف أولية"},
    {"black intel: fallen enemies drop their weapons", "معلومات BLACK: يسقط الأعداء القتلى أسلحتهم"},
    {"black intel: find weapons to complete armament objectives", "معلومات BLACK: اعثر على الأسلحة لإكمال أهداف التسليح"},
    {"black intel: hand grenades can be shot in mid air", "معلومات BLACK: يمكن تفجير القنابل اليدوية بإطلاق النار عليها في الهواء"},
    {"black intel: objects marked with a yellow triangle explode when shot", "معلومات BLACK: الأجسام المعلّمة بمثلث أصفر تنفجر عند إطلاق النار عليها"},
    {"black intel: recon objectives provide information for future missions", "معلومات BLACK: تمنحك أهداف الاستطلاع معلومات عن المهمات القادمة"},
    {"black intel: red health bar indicates health is running low", "معلومات BLACK: الشريط الأحمر يعني انخفاض مستوى الصحة"},
    {"black intel: red hit ring indicates direction of attack", "معلومات BLACK: حلقة الإصابة الحمراء تشير إلى اتجاه الهجوم"},
    {"black intel: smash windows with a melee attack", "معلومات BLACK: حطّم النوافذ بالهجوم القريب"},
    {"black intel: suppressed weapons are less powerful", "معلومات BLACK: الأسلحة المزودة بكاتم صوت أقل قوة"},
    {"black intel: switch to single-shot fire mode for better accuracy", "معلومات BLACK: بدّل إلى الإطلاق الفردي لدقة أعلى"},
    {"black intel: the magnum takes down enemies with one shot", "معلومات BLACK: مسدس Magnum يردي الأعداء بطلقة واحدة"},
    {"black intel: the p90 is a medium range weapon", "معلومات BLACK: رشاش P90 سلاح فعال في المدى المتوسط"},
    {"black intel: the m16 is a highly accurate medium range rifle", "معلومات BLACK: بندقية M16 دقيقة للغاية في المدى المتوسط"},
    {"black intel: the m16a2 has a grenade launcher attachment", "معلومات BLACK: بندقية M16A2 مزودة بقاذف قنابل"},
    {"black intel: the ak47 is a medium range rifle", "معلومات BLACK: بندقية AK-47 فعالة في المدى المتوسط"},
    {"black intel: use grenades to clear rooms", "معلومات BLACK: استخدم القنابل لتطهير الغرف"},
    {"black intel: use grenades to take down enemies with shields", "معلومات BLACK: استخدم القنابل للقضاء على حاملي الدروع"},
    {"black intel: use headshots to take out enemies with body armour", "معلومات BLACK: استهدف الرأس للتغلب على الأعداء المدرعين"},
    {"black intel: use health packs when your health bar is red", "معلومات BLACK: استخدم حقيبة الإسعاف عندما يصبح شريط صحتك أحمر"},
    {"black intel: use shotguns to blast down doors", "معلومات BLACK: استخدم بندقية الرش لفتح الأبواب المغلقة"},
    {"black intel: use weapon aim for more accurate shots", "معلومات BLACK: استخدم التصويب لتسديد طلقات أدق"},
    {"black intel: you can carry 2 weapons at once", "معلومات BLACK: يمكنك حمل سلاحين في آن واحد"},
    {"black intel: you can carry 3 health packs at once", "معلومات BLACK: يمكنك حمل 3 حقائب إسعاف في آن واحد"},
    {"black intel: silver weapons have unlimited ammunition", "معلومات BLACK: الأسلحة الفضية تملك ذخيرة لا نهائية"},
    {"black intel: silver weapons unlock after beating hard", "معلومات BLACK: تفتح الأسلحة الفضية بعد إنهاء الصعوبة الصعبة"},
    {"black intel: headshots drop your enemy with one shot", "معلومات BLACK: إصابات الرأس تردي العدو بطلقة واحدة"},
    {"black intel: shoot hand grenades mid air to kill nearby enemies.", "معلومات BLACK: أطلق النار على القنابل في الجو لقتل الأعداء القريبين"},
    {"black intel: shoot hand grenades mid air to kill nearby enemies", "معلومات BLACK: أطلق النار على القنابل في الجو لقتل الأعداء القريبين"},
    {"black intel: the walther 2000 has a 1000m range", "معلومات BLACK: بندقية Walther 2000 مداها 1000 متر"},
    {"black intel: the walther 2000 kills with one shot", "معلومات BLACK: بندقية Walther 2000 تقتل بطلقة واحدة"},
    {"black intel: the walther 2000 has two zoom levels", "معلومات BLACK: بندقية Walther 2000 تملك مستويين للتقريب"},
    {"black intel: the dc3 elite is best used at close range", "معلومات BLACK: مسدس DC3 Elite مثالي للمسافات القريبة"},
    {"black intel: the dc3 elite is accurate at close range", "معلومات BLACK: مسدس DC3 Elite دقيق في المسافات القريبة"},
    {"black intel: the spas 12 is an ultra close range weapon", "معلومات BLACK: بندقية SPAS-12 سلاح للمسافات القريبة جداً"},
    {"black intel: use the spas12 to blast open locked doors", "معلومات BLACK: استخدم SPAS-12 لتفجير الأبواب المقفلة"},
    {"black intel: the remington is a close range weapon", "معلومات BLACK: بندقية Remington سلاح للمسافات القريبة"},
    {"black intel: the m203 is a medium to long range grenade launcher", "معلومات BLACK: قاذف M203 فعال في المدى المتوسط والبعيد"},
    {"black intel: the m203 is a medium range weapon", "معلومات BLACK: قاذف M203 سلاح للمدى المتوسط"},
    {"black intel: the g36c is a medium range rifle", "معلومات BLACK: بندقية G36C فعالة في المدى المتوسط"},
    {"black intel: the mac 10 is a close range weapon", "معلومات BLACK: رشاش MAC-10 سلاح للمسافات القريبة"},
    {"black intel: the mac 10 is inaccurate at long range", "معلومات BLACK: رشاش MAC-10 غير دقيق في المدى البعيد"},
    {"black intel: use the uzi at close range", "معلومات BLACK: استخدم رشاش Uzi في المسافات القريبة"},
    {"black intel: the uzi is inaccurate at long range", "معلومات BLACK: رشاش Uzi غير دقيق في المدى البعيد"},
    {"black intel: use the uzi to clear rooms of multiple enemies", "معلومات BLACK: استخدم Uzi لتطهير الغرف من عدة أعداء"},
    {"black intel: use the glock for close range headshots", "معلومات BLACK: استخدم مسدس Glock لإصابات الرأس القريبة"},
    {"black intel: suppressed weapons are quieter but less powerful", "معلومات BLACK: الأسلحة المكتومة أهدأ لكنها أقل قوة"},
    {"black intel: use a suppressor to silence a weapon", "معلومات BLACK: استخدم كاتم الصوت لتخفيض صوت السلاح"},
    {"black intel: use suppressed weapons to kill enemies without alerting others", "معلومات BLACK: استخدم الأسلحة المكتومة لقتل الأعداء دون تنبيه الآخرين"},
    {"black intel: use suppressed weapons to stealthily kill enemies", "معلومات BLACK: استخدم الأسلحة المكتومة للقضاء على الأعداء بصمت"},
    {"black intel: try suppressed headshots for one-shot stealth kills", "معلومات BLACK: جرب إصابات الرأس بسلاح مكتوم للقتل الصامت بطلقة واحدة"},
    {"black intel: shoot off enemy helmets to take them down quickly", "معلومات BLACK: أسقط خوذات الأعداء بالرصاص للقضاء عليهم بسرعة"},
    {"black intel: shoot through cover to hit hiding enemies", "معلومات BLACK: أطلق النار عبر السواتر لإصابة الأعداء المختبئين"},
    {"black intel: shoot damaged walls and floors to open alternate routes", "معلومات BLACK: أطلق النار على الجدران والأرضيات المتضررة لفتح مسارات بديلة"},
    {"black intel: blast walls in the underground gulag to find bunkers", "معلومات BLACK: فجّر الجدران في السجن الأرضي للعثور على المخابئ"},
    {"black intel: blast walls in naszran town", "معلومات BLACK: فجّر الجدران في بلدة نصران"},
    {"black intel: shoot through walls in tivliz", "معلومات BLACK: أطلق النار عبر الجدران في مصح تيفليز"},
    {"black intel: destroy the crane in the dockyard", "معلومات BLACK: دمّر الرافعة في حوض السفن"},
    {"black intel: destroy the gas tanks in the foundry", "معلومات BLACK: دمّر خزانات الغاز في المسبك"},
    {"black intel: destroy the environment to take down enemies", "معلومات BLACK: دمّر البيئة المحيطة لإسقاط الأعداء"},
    {"black intel: objects enemies use for cover can often be destroyed", "معلومات BLACK: السواتر التي يحتمي بها الأعداء يمكن تدميرها غالباً"},
    {"black intel: shoot red objects to make them explode", "معلومات BLACK: أطلق النار على الأجسام الحمراء لتفجيرها"},
    {"black intel: use the black reticle to find and complete destruction objectives", "معلومات BLACK: استخدم مؤشر بلاك الأسود لإيجاد وإكمال أهداف التدمير"},
    {"black intel: use the black reticle to find explosive objects", "معلومات BLACK: استخدم مؤشر بلاك لإيجاد الأجسام القابلة للانفجار"},
    {"black intel: search the area for health packs, ammo and grenades", "معلومات BLACK: فتش المنطقة للعثور على حقائب الإسعاف والذخيرة والقنابل"},
    {"black intel: health packs can be carried and used when needed", "معلومات BLACK: يمكن حمل حقائب الإسعاف واستخدامها عند الحاجة"},
    {"black intel: first aid kits instantly restore health", "معلومات BLACK: حقائب الإسعاف الأولية تستعيد الصحة فوراً"},
    {"black intel: heartbeat sound indicates low health", "معلومات BLACK: صوت نبضات القلب ينذر بانخفاض مستوى الصحة"},
    {"black intel: red crosshair indicates enemy in range", "معلومات BLACK: المؤشر الأحمر يدل على أن العدو ضمن المدى"},
    {"black intel: take down enemies up close with a melee attack", "معلومات BLACK: أسقط الأعداء القريبين بالهجوم اليدوي"},
    {"black intel: pick up weapons from fallen enemies", "معلومات BLACK: التقط الأسلحة من الأعداء الذين سقطوا"},
    {"black intel: break windows near enemies to distract them", "معلومات BLACK: حطّم النوافذ قرب الأعداء لتشتيت انتباههم"},
    {"black intel: use grenades to take down multiple enemies", "معلومات BLACK: استخدم القنابل اليدوية لإسقاط عدة أعداء معاً"},
    {"black intel: use shotguns to take down enemies in body armour", "معلومات BLACK: استخدم الشوزن لإسقاط الأعداء ذوي الدروع الواقية"},
    {"black ops rating on all missions", "تقييم العمليات السوداء في جميع المهمات"},
    {"black mode complete", "اكتمل وضع العمليات السوداء"},

    // All Mission Primary / Secondary / Destruction / Recon Objectives & HUD Prompts
    {"objective target", "هدف المهمة"},
    {"target", "الهدف"},
    {"secondary objective target update", "تحديث هدف المهمة الثانوي"},
    {"rendezvous with black cell", "الالتقاء بخلية بلاك"},
    {"rendezvous with black cell member", "الالتقاء بعضو خلية بلاك"},
    {"meet with black cell member", "الالتقاء بعضو خلية بلاك"},
    {"rendezvous with 2nd black ops team", "الالتقاء بفريق العمليات السوداء الثاني"},
    {"meet with 2nd black ops team", "الالتقاء بفريق العمليات السوداء الثاني"},
    {"assist 2nd black ops team", "مساندة فريق العمليات السوداء الثاني"},
    {"find 2nd black ops team", "العثور على فريق العمليات السوداء الثاني"},
    {"rendezvous with mccarver", "الالتقاء بماكارفر"},
    {"meet mccarver", "الالتقاء بماكارفر"},
    {"meet mccarver at the merjakni farm", "الالتقاء بماكارفر في مزرعة ميرجاكني"},
    {"rendezvous with mccarver at the merjakni farm", "الالتقاء بماكارفر في مزرعة ميرجاكني"},
    {"reach the farm rendezvous", "الوصول إلى نقطة الالتقاء في المزرعة"},
    {"find the merjakni farm", "العثور على مزرعة ميرجاكني"},
    {"search the farmhouse", "تفتيش بيت المزرعة"},
    {"search the farm buildings", "تفتيش مباني المزرعة"},
    {"clear all outbuildings", "تطهير جميع المباني الخارجية"},
    {"clear all outbuildings without taking damage", "تطهير جميع المباني الخارجية دون تلقي أي ضرر"},
    {"meet solomon at foundry entrance", "الالتقاء بسولومون عند مدخل المسبك"},
    {"rendezvous with solomon at the foundry entrance", "الالتقاء بسولومون عند مدخل المسبك"},
    {"find the naszran foundry", "العثور على مسبك نصران"},
    {"go to the steel mill", "التوجه إلى مصنع الصلب"},
    {"cross the treneska border crossing", "عبور نقطة تفتيش ترينيسكا الحدودية"},
    {"cross the border at the treneska checkpoint", "عبور الحدود عند نقطة تفتيش ترينيسكا"},
    {"cross the border", "عبور الحدود"},
    {"find the border", "العثور على الحدود"},
    {"find the treneska border", "العثور على حدود ترينيسكا"},
    {"cross the vlodnik canal", "عبور قناة فلودنيك"},
    {"find the river crossing", "العثور على معبر النهر"},
    {"cross the minefield", "عبور حقل الألغام"},
    {"pass through the minefield", "المرور عبر حقل الألغام"},
    {"cross the cemetery", "عبور المقبرة"},
    {"cross sniper alley", "عبور زقاق القناصة"},
    {"cross naszran town", "عبور بلدة نصران"},
    {"cross naszran town with mccarver", "عبور بلدة نصران برفقة ماكارفر"},
    {"mccarver will give you sniper support", "سيوفر لك ماكارفر إسناد القنص"},
    {"take sniper positions", "السيطرة على مواقع القناصة"},
    {"secure sniper positions", "تأمين مواقع القناصة"},
    {"eliminate all enemies", "القضاء على جميع الأعداء"},
    {"eliminate all enemies in this area", "القضاء على جميع الأعداء في هذه المنطقة"},
    {"find and eliminate all enemies", "العثور على جميع الأعداء والقضاء عليهم"},
    {"find and eliminate all patrols", "العثور على جميع الدوريات والقضاء عليها"},
    {"eliminate incoming patrol", "القضاء على الدورية القادمة"},
    {"eliminate ambushing enemies", "القضاء على الأعداء الكامنين"},
    {"eliminate reinforcements", "القضاء على التعزيزات المعادية"},
    {"hold off enemy reinforcements", "صد تعزيزات العدو"},
    {"eliminate all snipers", "القضاء على جميع القناصة"},
    {"kill all snipers", "القضاء على جميع القناصة"},
    {"eliminate the sniper", "القضاء على القناص"},
    {"eliminate the rpg enemy", "القضاء على رامي RPG"},
    {"eliminate all rpg enemies", "القضاء على جميع رماة RPG"},
    {"eliminate the heavy machine gunners", "القضاء على رماة الرشاشات الثقيلة"},
    {"destroy machine gun nests", "تدمير أعشاش الرشاشات"},
    {"destroy the machine gun nest", "تدمير عش الرشاش"},
    {"destroy mortar positions", "تدمير مواقع الهاون"},
    {"destroy the troop truck", "تدمير شاحنة الجنود"},
    {"destroy all working vehicles", "تدمير جميع المركبات العاملة"},
    {"destroy the entrance gate", "تدمير بوابة الدخول"},
    {"destroy the exit gate", "تدمير بوابة الخروج"},
    {"destroy the fortified entry post", "تدمير نقطة الدخول المحصنة"},
    {"destroy the main gulag doors", "تدمير البوابات الرئيسية للسجن"},
    {"eliminate gulag entry defences", "القضاء على دفاعات مدخل السجن"},
    {"enter the spetriniv 6 gulag", "الدخول إلى سجن سبيترينيف 6"},
    {"reach the gulag", "الوصول إلى السجن"},
    {"find lennox", "العثور على لينوكس"},
    {"hunt down seventh wave suspect lennox", "ملاحقة المشتبه به في الموجة السابعة لينوكس"},
    {"capture seventh wave suspects", "القبض على المشتبه بهم من الموجة السابعة"},
    {"capture seventh wave suspect", "القبض على المشتبه به من الموجة السابعة"},
    {"capture or eliminate operative suspect", "القبض على العميل المشتبه به أو تصفيته"},
    {"eliminate 3 primary targets", "القضاء على 3 أهداف رئيسية"},
    {"find and kill 6 primary targets", "العثور على 6 أهداف رئيسية والقضاء عليها"},
    {"eliminate valencio's bodyguards", "القضاء على حراس فالنسيو الشخصيين"},
    {"defeat valencio's guards", "هزيمة حراس فالنسيو"},
    {"find the entrance to valencio's lair", "العثور على مدخل مخبأ فالنسيو"},
    {"enter the asylum", "الدخول إلى المصح"},
    {"enter the tivliz asylum interior", "الدخول إلى القسم الداخلي لمصح تيفليز"},
    {"enter the museum", "الدخول إلى المتحف"},
    {"storm the museum", "اقتحام المتحف"},
    {"assault the enemy stronghold", "اقتحام معقل العدو"},
    {"enter the building", "الدخول إلى المبنى"},
    {"find a way into the basement", "العثور على طريق إلى القبو"},
    {"find a way into the underground network", "العثور على مدخل إلى الشبكة الأرضية"},
    {"enter the underground complex", "الدخول إلى المجمع الأرضي"},
    {"enter the ressanov tunnel network", "الدخول إلى شبكة أنفاق ريسانوف"},
    {"find the bunker entrance", "العثور على مدخل المخبأ المحصن"},
    {"find the armoury", "العثور على مخزن الأسلحة"},
    {"find the weapons cache", "العثور على مخبأ الأسلحة"},
    {"find the shotgun", "العثور على بندقية الشوزن"},
    {"find the m203", "العثور على قاذف M203"},
    {"find tom & matt", "العثور على توم ومات"},
    {"clear the junction", "تطهير التقاطع"},
    {"go to the junction", "التوجه إلى التقاطع"},
    {"disable weapons production capacity", "تعطيل قدرة إنتاج الأسلحة"},
    {"destroy weapon manufacturing equipment", "تدمير معدات تصنيع الأسلحة"},
    {"destroy the weapon machine tools", "تدمير آلات تصنيع الأسلحة"},
    {"destroy 3 weapon presses", "تدمير 3 مكابس تصنيع أسلحة"},
    {"destroy the ammunition machine", "تدمير آلة تصنيع الذخيرة"},
    {"destroy the machinery", "تدمير الآلات"},
    {"destroy metal smelter control panels", "تدمير لوحات التحكم في مصهر المعادن"},
    {"destroy the control panels", "تدمير لوحات التحكم"},
    {"destroy the gasometers", "تدمير خزانات الغاز"},
    {"destroy both gasometers", "تدمير كلا خزاني الغاز"},
    {"cross the vratska dry dock area", "عبور منطقة الحوض الجاف في فراتسكا"},
    {"enter the main shipping area", "الدخول إلى منطقة الشحن الرئيسية"},
    {"destroy the dry dock controls", "تدمير وحدات التحكم بالحوض الجاف"},
    {"vratska dockyard weapons shipping facility", "منشأة شحن الأسلحة في حوض سفن فراتسكا"},
    {"cross the graznei bridge", "عبور جسر غرازني"},
    {"fight across the bridge", "القتال عبر الجسر"},
    {"complete the level", "إكمال المرحلة"},
    {"complete this section", "إكمال هذا القسم"},
    {"destroy all targets", "تدمير جميع الأهداف"},
    {"kill enemies with headshots", "اقتل الأعداء بإصابات الرأس"},
    {"kill enemies for extra time", "اقتل الأعداء لكسب وقت إضافي"},
    {"get 10 headshot kills", "حقق 10 قتلات بإصابة الرأس"},
    {"get 10 sniper kills", "حقق 10 قتلات ببندقية القنص"},
    {"get 10 mine kills", "حقق 10 قتلات بالألغام"},
    {"get 3 mine kills", "حقق 3 قتلات بالألغام"},
    {"get 10 indirect kills", "حقق 10 قتلات غير مباشرة"},
    {"get 5 ricochet kills", "حقق 5 قتلات بالارتداد"},
    {"get 5 stealth kills", "حقق 5 قتلات صامتة"},
    {"1000 enemies killed", "تم القضاء على 1000 عدو"},
    {"use gravestones for cover", "احتمِ بشواهد القبور"},
    {"use shotgun to blast doors", "استخدم الشوزن لتفجير الأبواب"},
    {"clear obstacles with shotgun", "أزل العوائق باستخدام الشوزن"},
    {"blast a path with the shotgun", "افتح طريقاً باستخدام الشوزن"},
    {"explore to find health packs", "استكشف المكان للعثور على حقائب الإسعاف"},
    {"attach suppressor to silence weapon", "ركّب كاتم الصوت لتخفيض صوت السلاح"},
    {"attach/detach suppressor", "تركيب/فك كاتم الصوت"},
    {"pick up weapons from fallen enemies", "التقط الأسلحة من الأعداء القتلى"},
    {"fallen enemies drop their weapons", "يسقط الأعداء القتلى أسلحتهم"},
    {"fallen enemies drop first aid kits", "يسقط الأعداء القتلى حقائب إسعاف"},
    {"health packs can be carried until needed", "يمكن حمل حقائب الإسعاف واستخدامها عند الحاجة"},
    {"first aid kits instantly restore health", "حقائب الإسعاف تستعيد الصحة فوراً"},
    {"use health pack when health bar is red", "استخدم حقيبة الإسعاف عندما يصبح شريط الصحة أحمر"},
    {"use health pack", "استخدم حقيبة الإسعاف"},
    {"red hit ring indicates attack direction", "الحلقة الحمراء تشير إلى اتجاه الهجوم"},
    {"red crosshair indicates enemy in range", "المؤشر الأحمر يدل على أن العدو في المدى"},
    {"green crosshair indicates friendly target", "المؤشر الأخضر يدل على هدف صديق"},
    {"blurred crosshair indicates out of range", "ضبابية المؤشر تعني أن الهدف خارج المدى"},
    {"heartbeat indicates low health", "نبضات القلب تشير إلى انخفاض الصحة"},
    {"red objects explode when shot", "الأجسام الحمراء تنفجر عند إطلاق النار عليها"},
    {"shoot mines to trigger explosions", "أطلق النار على الألغام لتفجيرها"},
    {"shoot mines to take down enemies", "أطلق النار على الألغام لإسقاط الأعداء"},
    {"breaking windows distracts enemies", "تحطيم النوافذ يشتت انتباه الأعداء"},
    {"use the black reticle to find destruction objects", "استخدم مؤشر بلاك لإيجاد أهداف التدمير"},
    {"use the black reticle to find and destroy objects.", "استخدم مؤشر بلاك للعثور على الأهداف وتدميرها."},
    {"find blue document rolls to help in future missions.", "اعثر على لفائف الوثائق الزرقاء للمساعدة في المهمات القادمة."},
    {"collect terrorist information and plans in folders and security briefcases.", "اجمع معلومات وخطط الإرهابيين في الملفات والحقائب الأمنية."},
    {"destroy laptops, safes and briefcases containing secret information.", "دمّر الحواسيب والخزائن والحقائب التي تحتوي على معلومات سرية."},
    {"find hidden weapons.", "اعثر على الأسلحة المخفية."},
    {"find hidden weapons", "اعثر على الأسلحة المخفية"},
    {"find weapons for armament objectives", "اعثر على الأسلحة لإكمال أهداف التسلح"},
    {"collect mission info for recon objectives", "اجمع معلومات المهمة لأهداف الاستطلاع"},
    {"collect info for intel objectives", "اجمع المعلومات لأهداف الاستخبارات"},
    {"collect information on terrorist activity", "اجمع معلومات حول النشاط الإرهابي"},
    {"find information for future missions", "اعثر على معلومات للمهمات القادمة"},
    {"destroy info for blackmail objectives", "دمّر الوثائق السرية لأهداف الابتزاز"},
    {"destroy us secrets to keep them hidden", "دمّر الأسرار السرية لإبقائها مخفية"},
    {"shoot objects for destruction objectives", "أطلق النار على الأهداف لإكمال مهام التدمير"},
    {"destroy objects for destruction objectives", "دمّر الأهداف لإكمال مهام التدمير"},
    {"weapons found", "الأسلحة المكتشفة"},
    {"weapon status", "حالة السلاح"},
    {"preferred weapon:", "السلاح المفضل:"},
    {"total bullets fired:", "إجمالي الطلقات المطلقة:"},
    {"total play time:", "إجمالي وقت اللعب:"},
    {"total game completion:", "نسبة إكمال اللعبة:"},
    {"total enemy count:", "إجمالي عدد الأعداء:"},
    {"welcome to black", "مرحباً بك في BLACK"},
    {"enter the real world of black ops", "ادخل العالم الحقيقي للعمليات السرية"},
    {"pull the trigger on the world's most explosive modern weaponry", "اضغط الزناد لأقوى الأسلحة الحديثة وأشدها انفجاراً"},
    {"unleash devastating destruction", "أطلق دماراً ساحقاً"},
    {"flawless", "بلا أخطاء"},
    {"perfect", "مثالي"},
    {"unlocked", "مفتوح"},
    {"none", "لا يوجد"},
    {"-unknown-", "-غير معروف-"},
    {"high velocity", "سرعة عالية"},
    {"high calibre", "عيار ثقيل"},
    {"close range", "مدى قريب"},
    {"rocket propelled", "مدفوع بالصواريخ"},
    {"fragmentation", "متشظي"},
    {"first aid kit", "حقيبة إسعاف أولية"},
    {"first aid kits full", "حقائب الإسعاف ممتلئة"},
    {"hand grenades full", "القنابل اليدوية ممتلئة"},
    {"pistol ammo", "ذخيرة مسدس"},
    {"pistol ammo full", "ذخيرة المسدس ممتلئة"},
    {"smg ammo", "ذخيرة رشاش خفيف"},
    {"smg ammo full", "ذخيرة الرشاش الخفيف ممتلئة"},
    {"assault rifle ammo", "ذخيرة بندقية هجومية"},
    {"assault rifle ammo full", "ذخيرة البندقية الهجومية ممتلئة"},
    {"shotgun ammo", "ذخيرة شوزن"},
    {"shotgun ammo full", "ذخيرة الشوزن ممتلئة"},
    {"sniper rifle ammo", "ذخيرة بندقية قنص"},
    {"sniper rifle ammo full", "ذخيرة بندقية القنص ممتلئة"},
    {"magnum ammo", "ذخيرة ماغنوم"},
    {"magnum ammo full", "ذخيرة الماغنوم ممتلئة"},
    {"m249 ammo", "ذخيرة M249"},
    {"m249 ammo full", "ذخيرة M249 ممتلئة"},
    {"m203 ammo", "ذخيرة M203"},
    {"m203 ammo full", "ذخيرة M203 ممتلئة"},
    {"rpg ammo", "ذخيرة RPG"},
    {"rpg ammo full", "ذخيرة RPG ممتلئة"},
    {"you found a hidden sniper rifle", "عثرت على بندقية قنص مخفية"},
    {"you found a hidden m16", "عثرت على بندقية M16 مخفية"},
    {"you found a hidden uzi", "عثرت على رشاش Uzi مخفي"},
    {"you found a hidden m203", "عثرت على سلاح M203 مخفي"},
    {"you found a hidden rpg", "عثرت على قاذف RPG مخفي"},
    {"you found a hidden magnum", "عثرت على مسدس Magnum مخفي"},

    // Credits Headers & Common Sections
    {"criterion games credits", "فريق تطوير Criterion Games"},
    {"eauk credits", "فريق EA UK"},
    {"story", "القصة"},
    {"music", "الموسيقى"},
    {"special thanks to", "شكر خاص إلى"},
};

const std::unordered_map<std::string, std::string_view>& FlashDictMap() {
    static const auto* map = []() {
        auto* m = new std::unordered_map<std::string, std::string_view>();
        m->reserve(sizeof(kFlashDict) / sizeof(kFlashDict[0]) + 32);
        for (const auto& entry : kFlashDict) {
            m->emplace(std::string(entry.key), std::string_view(entry.ar));
        }
        return m;
    }();
    return *map;
}

bool TryTranslatePattern(const std::string& norm, std::string& out_ar) {
    char buf[256];
    int n1 = 0, n2 = 0;

    if (std::sscanf(norm.c_str(), "mission status: %d%%", &n1) == 1) {
        std::snprintf(buf, sizeof(buf), "حالة المهمة: %d%%", n1);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "kill book: %d%%", &n1) == 1) {
        std::snprintf(buf, sizeof(buf), "سجل القتلى: %d%%", n1);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "objectives complete: %d/%d", &n1, &n2) == 2) {
        std::snprintf(buf, sizeof(buf), "الأهداف المكتملة: %d/%d", n1, n2);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "challenges complete: %d/%d", &n1, &n2) == 2) {
        std::snprintf(buf, sizeof(buf), "التحديات المكتملة: %d/%d", n1, n2);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "weapon challenges complete: %d/%d", &n1, &n2) == 2) {
        std::snprintf(buf, sizeof(buf), "تحديات السلاح المكتملة: %d/%d", n1, n2);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "weapons found: %d/%d", &n1, &n2) == 2) {
        std::snprintf(buf, sizeof(buf), "الأسلحة المكتشفة: %d/%d", n1, n2);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "destruction complete: %d/%d", &n1, &n2) == 2) {
        std::snprintf(buf, sizeof(buf), "التدمير المكتمل: %d/%d", n1, n2);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "complete %d of %d secondary objectives", &n1, &n2) == 2) {
        std::snprintf(buf, sizeof(buf), "أكمل %d من %d أهداف ثانوية", n1, n2);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "%d/%d secondary objectives complete", &n1, &n2) == 2) {
        std::snprintf(buf, sizeof(buf), "اكتمل %d/%d من الأهداف الثانوية", n1, n2);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "intel objective %d/%d", &n1, &n2) == 2) {
        std::snprintf(buf, sizeof(buf), "هدف الاستخبارات %d/%d", n1, n2);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "blackmail objective %d/%d", &n1, &n2) == 2) {
        std::snprintf(buf, sizeof(buf), "هدف الابتزاز %d/%d", n1, n2);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "gasometer %d of %d destroyed", &n1, &n2) == 2) {
        std::snprintf(buf, sizeof(buf), "تم تدمير خزان الغاز %d من %d", n1, n2);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "weapon machinery %d of %d destroyed", &n1, &n2) == 2) {
        std::snprintf(buf, sizeof(buf), "تم تدمير آلات السلاح %d من %d", n1, n2);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "smelter %d of %d destroyed", &n1, &n2) == 2) {
        std::snprintf(buf, sizeof(buf), "تم تدمير المصهر %d من %d", n1, n2);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "outbuilding %d of %d cleared", &n1, &n2) == 2) {
        std::snprintf(buf, sizeof(buf), "تم تطهير المبنى الخارجي %d من %d", n1, n2);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "dry dock control %d of %d destroyed", &n1, &n2) == 2) {
        std::snprintf(buf, sizeof(buf), "تم تدمير تحكم الحوض الجاف %d من %d", n1, n2);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "rpg enemy %d of %d has been eliminated", &n1, &n2) == 2) {
        std::snprintf(buf, sizeof(buf), "تم القضاء على رامي RPG %d من %d", n1, n2);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "sniper %d of %d has been eliminated", &n1, &n2) == 2) {
        std::snprintf(buf, sizeof(buf), "تم القضاء على القناص %d من %d", n1, n2);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "sniper position %d of %d taken", &n1, &n2) == 2) {
        std::snprintf(buf, sizeof(buf), "تم السيطرة على موقع القناص %d من %d", n1, n2);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "enemies killed: %d", &n1) == 1) {
        std::snprintf(buf, sizeof(buf), "الأعداء القتلى: %d", n1);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "body count: %d", &n1) == 1) {
        std::snprintf(buf, sizeof(buf), "عدد القتلى: %d", n1);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "black kills: %d", &n1) == 1) {
        std::snprintf(buf, sizeof(buf), "قتلى بلاك: %d", n1);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "black ops kills: %d", &n1) == 1) {
        std::snprintf(buf, sizeof(buf), "قتلى العمليات السوداء: %d", n1);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "current record: %d targets", &n1) == 1) {
        std::snprintf(buf, sizeof(buf), "الرقم القياسي الحالي: %d هدف", n1);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "current record: %d headshots", &n1) == 1) {
        std::snprintf(buf, sizeof(buf), "الرقم القياسي الحالي: %d إصابة رأس", n1);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "current record: %d enemies killed", &n1) == 1) {
        std::snprintf(buf, sizeof(buf), "الرقم القياسي الحالي: %d عدو قتيل", n1);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "mission time: %d:%d", &n1, &n2) == 2) {
        std::snprintf(buf, sizeof(buf), "وقت المهمة: %d:%02d", n1, n2);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "destruction %d of %d", &n1, &n2) == 2) {
        std::snprintf(buf, sizeof(buf), "تدمير %d من %d", n1, n2);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "complete %d objectives", &n1) == 1) {
        std::snprintf(buf, sizeof(buf), "أكمل %d من الأهداف", n1);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "%d new kill book kills", &n1) == 1) {
        std::snprintf(buf, sizeof(buf), "%d قتلى جدد في سجل العمليات", n1);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "%d new weapons unlocked!", &n1) == 1) {
        std::snprintf(buf, sizeof(buf), "تم فتح %d أسلحة جديدة!", n1);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "%d new weapon unlocked!", &n1) == 1) {
        std::snprintf(buf, sizeof(buf), "تم فتح سلاح جديد (%d)!", n1);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "%d new challenges unlocked!", &n1) == 1) {
        std::snprintf(buf, sizeof(buf), "تم فتح %d تحديات جديدة!", n1);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "%d new challenge unlocked!", &n1) == 1) {
        std::snprintf(buf, sizeof(buf), "تم فتح تحدٍ جديد (%d)!", n1);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(), "please reconnect the controller to controller port %d", &n1) == 1) {
        std::snprintf(buf, sizeof(buf), "يرجى إعادة توصيل وحدة التحكم بالمنفذ %d", n1);
        out_ar = buf;
        return true;
    }
    if (std::sscanf(norm.c_str(),
                    "your xbox doesn't have enough free blocks to save games. you need to free %d more block",
                    &n1) == 1) {
        std::snprintf(buf, sizeof(buf), "لا توجد مساحة كافية لحفظ اللعبة. تحتاج إلى تحرير %d وحدة إضافية.", n1);
        out_ar = buf;
        return true;
    }
    if (norm.rfind("you destroyed", 0) == 0) {
        out_ar = "لقد دمّرت هدف معلومات سرية (" + norm.substr(14) + ")";
        return true;
    }
    if (norm.rfind("you discovered", 0) == 0 || norm.rfind("you found", 0) == 0 ||
        norm.rfind("you uncovered", 0) == 0 || norm.rfind("you obtained", 0) == 0) {
        out_ar = "لقد عثرت على معلومات استطلاع سرية";
        return true;
    }
    if (norm.rfind("hold ", 0) == 0 && norm.find(" to pick up") != std::string::npos) {
        out_ar = "اضغط مطولاً لالتقاط السلاح";
        return true;
    }
    if (norm.rfind("hold ", 0) == 0 && norm.find(" to swap") != std::string::npos) {
        out_ar = "اضغط مطولاً لتبديل السلاح";
        return true;
    }
    if (norm.size() > 10 && norm.substr(norm.size() - 10) == " picked up") {
        std::string item = TrimCopy(norm.substr(0, norm.size() - 10));
        out_ar = "تم التقاط " + item;
        return true;
    }
    if (norm.size() > 10 && norm.substr(norm.size() - 10) == " ammo full") {
        std::string wpn = TrimCopy(norm.substr(0, norm.size() - 10));
        out_ar = "ذخيرة " + wpn + " ممتلئة";
        return true;
    }
    const std::string_view p_over = "are you sure you want to overwrite profile \"";
    if (norm.rfind(p_over, 0) == 0 && norm.size() > p_over.size() + 2 &&
        norm.substr(norm.size() - 2) == "\"?") {
        std::string name = TrimCopy(norm.substr(p_over.size(), norm.size() - p_over.size() - 2));
        out_ar = "هل أنت متأكد من الكتابة فوق الملف الشخصي \"" + name + "\"؟";
        return true;
    }
    const std::string_view p_save = "do you want to save profile \"";
    if (norm.rfind(p_save, 0) == 0 && norm.size() > p_save.size() + 2 &&
        norm.substr(norm.size() - 2) == "\"?") {
        std::string name = TrimCopy(norm.substr(p_save.size(), norm.size() - p_save.size() - 2));
        out_ar = "هل تريد حفظ الملف الشخصي \"" + name + "\"؟";
        return true;
    }
    return false;
}

// Translates a Flash text string into raw (unshaped) Arabic UTF-8.
// Returns false if the string is a native symbol/number or has no translation.
bool TranslateFlashToRawArabic(const std::string& english, std::string& out_raw_ar) {
    std::string trimmed = TrimCopy(english);
    if (trimmed.empty()) return false;

    std::string norm = NormalizeKey(trimmed);
    if (IsSymbolOrPureNumeric(norm)) return false;

    const auto& dict = FlashDictMap();
    if (auto it = dict.find(norm); it != dict.end()) {
        out_raw_ar.assign(it->second);
        return true;
    }
    if (TryTranslatePattern(norm, out_raw_ar)) {
        return true;
    }

    // Fallback to shared recomp UI table (translations.cpp) when running under Arabic.
    if (game::Opt().language == "ara") {
        std::string ui_ar = app::Ui(trimmed, false);
        if (!ui_ar.empty() && ui_ar != trimmed && ContainsArabicCodepoint(ui_ar)) {
            out_raw_ar = std::move(ui_ar);
            return true;
        }
    }
    return false;
}

std::string WrapAndShapeArabic(ImFont* font, float font_px, const std::string& raw_ar, float max_width_px) {
    if (HasPresentationForms(raw_ar)) {
        // Already shaped/reordered upstream (e.g., from app::Ui): do not double-shape.
        return raw_ar;
    }

    if (max_width_px <= 0.0f) {
        return arabic::ShapeAndBidiMultiline(raw_ar);
    }

    std::string wrapped;
    size_t pos = 0;
    while (pos <= raw_ar.size()) {
        size_t nl = raw_ar.find('\n', pos);
        std::string_view para = (nl == std::string_view::npos)
                                    ? std::string_view(raw_ar).substr(pos)
                                    : std::string_view(raw_ar).substr(pos, nl - pos);
        if (!wrapped.empty()) wrapped.push_back('\n');

        if (!para.empty()) {
            std::string current_line;
            size_t wi = 0;
            while (wi < para.size()) {
                while (wi < para.size() && para[wi] == ' ') ++wi;
                if (wi >= para.size()) break;
                size_t sp = para.find(' ', wi);
                if (sp == std::string_view::npos) sp = para.size();
                std::string_view word = para.substr(wi, sp - wi);
                wi = sp;

                if (current_line.empty()) {
                    current_line.assign(word);
                    continue;
                }
                std::string candidate = current_line + " ";
                candidate.append(word);
                std::string shaped_cand = arabic::ShapeAndBidi(candidate);
                float cand_w = font->CalcTextSizeA(font_px, 100000.0f, 0.0f, shaped_cand.c_str()).x;
                if (cand_w <= max_width_px) {
                    current_line = std::move(candidate);
                } else {
                    wrapped += arabic::ShapeAndBidi(current_line);
                    wrapped.push_back('\n');
                    current_line.assign(word);
                }
            }
            if (!current_line.empty()) {
                wrapped += arabic::ShapeAndBidi(current_line);
            }
        }

        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return wrapped;
}

void ComputeFlashColor(uint32_t ui, uint8_t& out_r, uint8_t& out_g, uint8_t& out_b, uint8_t& out_a) {
    uint32_t c0 = R32(ui + 0x3758);  // mul BGRA (0x80 = 1.0x)
    uint32_t c1 = R32(ui + 0x375C);  // add BGRA signed
    float gt_b = G<float>(ui + 0x3720);
    float gt_g = G<float>(ui + 0x3724);
    float gt_r = G<float>(ui + 0x3728);
    float gt_a = G<float>(ui + 0x372C);

    auto calc_rgb = [](uint8_t mul_u8, uint8_t add_u8, float gt, float base_chan) -> uint8_t {
        float mul_f = (mul_u8 / 128.0f) * (gt * 2.0f);
        float add_f = (static_cast<int8_t>(add_u8) / 127.0f) * (gt * 2.0f);
        float v = std::clamp(base_chan * mul_f + add_f, 0.0f, 1.0f);
        return static_cast<uint8_t>(std::lround(v * 255.0f));
    };

    // Unfocused BLACK Flash UI copy runs ~#D6D6D6; focused highlight uses c0=80000000, c1=007F7F7F -> #FFFFFF
    constexpr float kBaseWhite = 0.86f;
    out_b = calc_rgb(c0 & 0xFF, c1 & 0xFF, gt_b, kBaseWhite);
    out_g = calc_rgb((c0 >> 8) & 0xFF, (c1 >> 8) & 0xFF, gt_g, kBaseWhite);
    out_r = calc_rgb((c0 >> 16) & 0xFF, (c1 >> 16) & 0xFF, gt_r, kBaseWhite);

    float mul_a = ((c0 >> 24) & 0xFF) / 128.0f;
    float add_a = (static_cast<int8_t>((c1 >> 24) & 0xFF) / 127.0f);
    float a_f = std::clamp(mul_a * gt_a + add_a * gt_a, 0.0f, 1.0f);
    out_a = static_cast<uint8_t>(std::lround(a_f * 255.0f));
}

}  // namespace

X86_FUNC(sub_00121E60) {
    g_in_flash.store(true, std::memory_order_relaxed);
    g_staging_items.clear();
    __imp__sub_00121E60(ctx, base);
    g_in_flash.store(false, std::memory_order_relaxed);

    {
        std::lock_guard lock(g_overlay_mutex);
        g_active_items.swap(g_staging_items);
        g_last_flash_frame = std::chrono::steady_clock::now();
    }
}

X86_FUNC(sub_00121BC0) {
    uint32_t obj = R32(ctx.esp + 4);
    uint32_t mode = R32(ctx.esp + 8);
    uint32_t ui = R32(kUi);

    if (mode == 0 && IsArabicUiActive() && obj && ui) {
        uint32_t wide_ptr = R32(obj + 0x28);
        std::string text = Utf16GuestToUtf8(wide_ptr, 512);
        std::string raw_ar;

        if (!text.empty() && TranslateFlashToRawArabic(text, raw_ar)) {
            uint8_t r = 220, g = 220, b = 220, a = 0;
            ComputeFlashColor(ui, r, g, b, a);

            if (a > 2) {
                float sx = G<float>(ui + kStageScaleX);
                float sy = G<float>(ui + kStageScaleY);
                float m0 = G<float>(ui + 0x3740);
                float m1 = G<float>(ui + 0x3744);
                float m2 = G<float>(ui + 0x3748);
                float m3 = G<float>(ui + 0x374C);
                float tx = G<float>(ui + 0x3750);
                float ty = G<float>(ui + 0x3754);

                float bx = G<float>(obj + 0x18);
                float by = G<float>(obj + 0x1C);
                float bw = G<float>(obj + 0x20);
                float bh = G<float>(obj + 0x24);
                float obj_scale = G<float>(obj + 0x38);
                uint8_t align = G<uint8_t>(obj + 0x3E);

                auto proj_x = [&](float lx, float ly) { return (lx * m0 + ly * m2 + tx) * sx; };
                auto proj_y = [&](float lx, float ly) { return (lx * m1 + ly * m3 + ty) * sy; };

                StageFrameItem item;
                item.raw_arabic = std::move(raw_ar);
                item.d3d_x0 = proj_x(bx, by);
                item.d3d_y0 = proj_y(bx, by);
                item.d3d_x1 = proj_x(bx + bw, by + bh);
                item.d3d_y1 = proj_y(bx + bw, by + bh);
                item.font_d3d_h = obj_scale * std::max(0.25f, std::fabs(m3)) * sy;
                item.align = align;
                item.r = r;
                item.g = g;
                item.b = b;
                item.a = a;

                if (a >= 128) {
                    std::string page = GuestCString(ui + kPageName, 48);
                    std::string lkey = page + "|" + text;
                    bool should_log = false;
                    {
                        std::lock_guard lock(g_log_mutex);
                        if (g_logged_translated.size() < 256 && g_logged_translated.insert(lkey).second) {
                            should_log = true;
                        }
                    }
                    if (should_log) {
                        LOGR("[FLASH_AR] page='%s' text='%s' d3d=(%.1f,%.1f..%.1f,%.1f) h=%.1f al=%u rgba=(%u,%u,%u,%u)",
                             page.c_str(), TrimCopy(text).c_str(), item.d3d_x0, item.d3d_y0,
                             item.d3d_x1, item.d3d_y1, item.font_d3d_h, item.align, r, g, b, a);
                    }
                }

                g_staging_items.push_back(std::move(item));
            }
            // Suppress native Xbox 7-bit Eurostile draw so no English or missing-glyph boxes appear.
            // sub_00121BC0 is __cdecl: callee pops only the 4-byte return address (ret).
            ctx.esp += 4;
            return;
        }

        if (!text.empty()) {
            std::string norm = NormalizeKey(text);
            if (!IsSymbolOrPureNumeric(norm)) {
                std::string page = GuestCString(ui + kPageName, 48);
                std::string ukey = page + "|" + norm;
                bool log_miss = false;
                {
                    std::lock_guard lock(g_log_mutex);
                    if (g_logged_untranslated.size() < 256 && g_logged_untranslated.insert(ukey).second) {
                        log_miss = true;
                    }
                }
                if (log_miss) {
                    LOGR("[FLASH_AR_MISS] page='%s' text='%s'", page.c_str(), TrimCopy(text).c_str());
                }
            }
        }
    }

    __imp__sub_00121BC0(ctx, base);
}

X86_FUNC(sub_000E4840) { __imp__sub_000E4840(ctx, base); }

X86_FUNC(sub_000D9600) { __imp__sub_000D9600(ctx, base); }

namespace game::flash_arabic {

void DrawOverlay(float screen_w, float screen_h) {
    if (!IsArabicUiActive() || screen_w <= 0.0f || screen_h <= 0.0f) return;

    std::vector<StageFrameItem> items;
    {
        std::lock_guard lock(g_overlay_mutex);
        if (g_active_items.empty()) return;
        auto age = std::chrono::steady_clock::now() - g_last_flash_frame;
        if (age > std::chrono::milliseconds(150)) {
            g_active_items.clear();
            return;
        }
        items = g_active_items;
    }

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    if (!dl || !font) return;

    const float scale_x = screen_w / 640.0f;
    const float scale_y = screen_h / 480.0f;

    for (const auto& item : items) {
        if (item.a <= 2) continue;

        float x0 = item.d3d_x0 * scale_x;
        float y0 = item.d3d_y0 * scale_y;
        float x1 = item.d3d_x1 * scale_x;
        float y1 = item.d3d_y1 * scale_y;
        float box_w = std::fabs(x1 - x0);
        float box_h = std::fabs(y1 - y0);
        float left_x = std::min(x0, x1);
        float right_x = std::max(x0, x1);
        float top_y = std::min(y0, y1);

        // Skip items scrolled completely off-screen
        if (right_x < -40.0f || left_x > screen_w + 40.0f || y1 < -40.0f || top_y > screen_h + 40.0f) {
            continue;
        }

        float font_px = std::clamp(item.font_d3d_h * scale_y * 1.18f, 15.0f, 110.0f);

        // Wrap only genuinely multi-line/dialog boxes; single-line menu labels grow cleanly up to screen edge.
        float wrap_limit = 0.0f;
        bool has_newline = item.raw_arabic.find('\n') != std::string::npos;
        bool wide_dialog_box = (item.align == 1 && box_w > screen_w * 0.42f && item.raw_arabic.size() > 48);
        if (has_newline || wide_dialog_box) {
            wrap_limit = std::max(box_w, screen_w * 0.45f);
            wrap_limit = std::min(wrap_limit, screen_w * 0.85f);
        }

        std::string shaped = WrapAndShapeArabic(font, font_px, item.raw_arabic, wrap_limit);
        if (shaped.empty()) continue;

        // Prevent overflow past screen edges on single-line labels
        float avail_w = (item.align == 1) ? (screen_w * 0.90f)
                        : (item.align == 2) ? std::max(120.0f, right_x - 16.0f)
                                            : std::max(120.0f, screen_w - left_x - 16.0f);

        ImVec2 sz = font->CalcTextSizeA(font_px, 100000.0f, 0.0f, shaped.c_str());
        if (sz.x > avail_w && sz.x > 1.0f) {
            font_px = std::max(13.0f, font_px * (avail_w / sz.x) * 0.97f);
            sz = font->CalcTextSizeA(font_px, 100000.0f, 0.0f, shaped.c_str());
        }

        // Draw line by line so multi-line Arabic blocks align properly
        size_t line_start = 0;
        float line_step = font_px * 1.15f;
        float draw_y = top_y - font_px * 0.10f;
        if (box_h > font_px * 1.25f && shaped.find('\n') == std::string::npos) {
            draw_y = top_y + (box_h - sz.y) * 0.5f;
        }

        ImU32 fg = IM_COL32(item.r, item.g, item.b, item.a);
        uint8_t sh_a = static_cast<uint8_t>(std::min(255, int(item.a) * 210 / 255));
        ImU32 sh = IM_COL32(0, 0, 0, sh_a);
        float sh_off = std::max(1.5f, font_px * 0.055f);

        while (line_start <= shaped.size()) {
            size_t line_end = shaped.find('\n', line_start);
            if (line_end == std::string::npos) line_end = shaped.size();
            std::string_view line(shaped.data() + line_start, line_end - line_start);

            if (!line.empty()) {
                ImVec2 lsz = font->CalcTextSizeA(font_px, 100000.0f, 0.0f, line.data(), line.data() + line.size());
                float draw_x = left_x;
                if (item.align == 1) {
                    draw_x = (left_x + right_x - lsz.x) * 0.5f;
                } else if (item.align == 2) {
                    draw_x = right_x - lsz.x;
                }
                draw_x = std::clamp(draw_x, 8.0f, std::max(8.0f, screen_w - lsz.x - 8.0f));

                dl->AddText(font, font_px, ImVec2(draw_x + sh_off, draw_y + sh_off), sh, line.data(),
                            line.data() + line.size());
                dl->AddText(font, font_px, ImVec2(draw_x, draw_y), fg, line.data(), line.data() + line.size());
            }

            draw_y += line_step;
            if (line_end == shaped.size()) break;
            line_start = line_end + 1;
        }
    }
}

}  // namespace game::flash_arabic
