# حزمة تأثيرات النار والدخان (Fire & Smoke FX Pack)
تاريخ: 2026-11-17 — المشروع: E:\\ai\\black

هذه الحزمة جاهزة لرفعها إلى ذكاء اصطناعي آخر ليعدّل **أنميشن النار والدخان وأشكالها**
دون لمس بقية اللعبة. تم تجميع كل الملفات ذات الصلة في مضغوطة واحدة.

## أين الأشكال وأين الأنميشن؟ (الخريطة الأهم)
| الغرض | الملف | السطور |
|---|---|---|
| **أشكال النار/الدخان/الشرر (مولّدة إجرائياً)** | `src/game/ballistics_rules.cpp` | **168-268** (`SampleVfxAtlasTexelBgra`) |
| دوال مساعدة للأشكال (FBM، الضجيج، الألوان) | نفس الملف | 47-166 |
| **ربط الأطلس (4x4) واستدعاء مولّد الأشكال** | `src/d3d/d3d8.cpp` | 1651-1685 |
| **صف الأطلس 3: نوى الشرر + حلقات الصدمة/الغبار** | `src/d3d/d3d8.cpp` | 1905-1965 |
| **أنميشن وتحديث النار/الدخان** | `src/game/ballistics_fx.cpp` | 1961-4240 (كتل متعددة) |
| لهب قاذف اللهب (الشكل والحركة) | `flamethrower*.cpp/.h`, `flame_plume.h` | ملفات كاملة |
| توثيق + صورة دخان | `SMOKE_TRAIL_IMPLEMENTATION.md`, `smoke_trail_sheet.png` | — |

## تفاصيل الأشكال (مهم جداً)
النار والدخان **ليست صور PNG منفصلة**؛ الأشكال مولّدة برمجياً داخل دالة واحدة
`SampleVfxAtlasTexelBgra` في `ballistics_rules.cpp` وتُكتب في أطلس VFX مقاس 4x4
(كل بلاطة 256px) في `d3d8.cpp`:
- **صف 0 (ty==0)**: دخان/سخام عضوي بتمويه مجالي (domain-warped FBM) — لونرمادي.
- **صف 1 (ty==1)**: ألسنة لهب مزدوجة التمويه بتدرّج الجسم الأسود (blackbody)
  من الأصفر الساخن إلى البرتقالي — `r_f/g_f/b_f`.
- **صف 2 (ty==2)**: دم (نظام مختلف — لا يهم هذه الحزمة، موجود لأن الملف كامل).
- **صف 3 (ty==3)**: نوى شرر/وميض (tiles 12-13) + حلقات صدمة وغبار (tiles 14-15).

## ما يمكن تعديله بسهولة
- شكل اللهب/الدخان وألوانه → `ballistics_rules.cpp:168-268` (دوال `RotatedFbm2D`،
  `tilt_x`، `up_reach`، `core_glow`، تدرّج `r_f/g_f/b_f`، `alpha`).
- عدد/حجم/عمر الجسيمات → كتل `ballistics_fx.cpp` المذكورة أعلاه
  (مثل `fireball`، `smoke column`، `sparks`، `muzzle smoke`).
- حركة لهب قاذف اللهب → `flamethrower.cpp` / `flamethrower_core.cpp` / `flame_plume.h`.

## قواعد قبل التعديل
1. الأشكال برمجية (بلا PNG منفصل) — أي تغيير على **الشكل** يجب أن يكون في `ballistics_rules.cpp`.
2. الألوان تُرزم بـ `PackBgra(r,g,b,a)` (القيم 0..1) من نفس الملف.
3. `ballistics_fx.cpp` و`ballistics_rules.cpp` غير متتبّعين في git جزئياً ويحويان أنظمة أخرى
   (دم، انفجارات) — غيّر كتل النار/الدخان فقط.
4. التنسيق 2 مسافة (clang-format). الاختبار:
   `Set-Location E:\\ai\\black; .\\tests\\build\\test_settings_core.exe`.
5. البناء: ninja على `ballistics_fx.cpp.o` و`ballistics_rules.cpp.o` في
   `app\\.cxx\\RelWithDebInfo\\1m2l1xh6\\arm64-v8a` ثم
   `.\\gradlew.bat assembleRelease --console=plain`.

## English summary
Fire/smoke ANIMATION is in `src/game/ballistics_fx.cpp` (blocks from line 1961).
Fire/flame/smoke SHAPES are PROCEDURAL (no PNG) — generated in
`src/game/ballistics_rules.cpp` `SampleVfxAtlasTexelBgra` (lines 168-268) into a 4x4
VFX atlas built in `src/d3d/d3d8.cpp`. Rows: 0=soot/smoke, 1=flame tongues, 3=sparks/
shockwave rings (row 2 = blood, different system). `extracts/fire_smoke_sections.txt`
has all these blocks with original line numbers.
