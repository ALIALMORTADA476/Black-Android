# حزمة تأثيرات الدم (Blood FX Pack)
تاريخ: 2026-11-17 — المشروع: E:\ai\black

هذه الحزمة جاهزة لرفعها إلى ذكاء اصطناعي آخر ليعدّل تأثيرات الدم دون لمس بقية اللعبة.

## محتوى الحزمة
- `README_BLOOD_FX_AR.md` — هذا الملف.
- `extracts/blood_sections.txt` — **مقتطعات الدم فقط** بأرقام الأسطر الأصلية (أسهل طريقة للبدء).
- `src/game/ballistics_fx.cpp` — الملف الكامل: كل منطق الدم + بقية المؤثرات.
- `src/game/ballistics_fx.h` — واجهة المؤثرات.
- `src/d3d/d3d8.cpp` — يحوي مولّد نسيج الدم داخل الـ VFX atlas (الصف 2).
- `src/d3d/world_overlay.h` — تعريف دوال الرسم والألوان.
- `src/game/blood_decal_rules.cpp` / `.h` — قواعد بقع الدم على الجثث.
- `BLOOD_AND_CORPSE_SYSTEM_ANALYSIS.md` — توثيق تحليلي لنظام الدم والجثث.

## أين الدم بالضبط؟
| الغرض | الملف | السطر |
|---|---|---|
| ثوابت السعة (384 جسيم، 96 جرح، 128 بركة) | `src/game/ballistics_fx.cpp` | 58-60 |
| تعريفات `BloodNode` / `WoundBleedStream` / `BloodPoolNode` | نفس الملف | 203 / 220 / 232 |
| مصفوفات الدم العامة | نفس الملف | 359-363 |
| **توليد الدم عند الإصابة** `OnEnemyBloodHit` | نفس الملف | 1521-1746 |
| **تحديث ورسم الدم** داخل `RenderWorldScene` | نفس الملف | 3516-3733 (3a/3b/3c) |
| **شكل بقعة الدم** (نسيج إجرائي، لا يوجد PNG منفصل) | `src/d3d/d3d8.cpp` | 1714-1733 (`if (ty == 2u)`) |

## ما يمكن تعديله بسهولة
داخل `OnEnemyBloodHit`:
- `mist_count` — عدد سحب الرذاذ (حالياً `2 + (mult>=3) + is_headshot`).
- `spray_count` — رذاذ شرياني سريع (`12 + mult*5 + is_headshot*10`).
- `drop_count` — القطرات (`14 + mult*8 + ...`).
- ألوان الرذاذ/القطرات: قيم `r,g,b` في دوال `push_blood` (مثل `0.044, 0.0005, 0.0012`).
- البرك: `target_radius`، `pool_ttl`، سرعة النمو في block 3c.

داخل `d3d8.cpp` (الصف 2):
- `blob` / `drips` تتحكمان في شكل البقعة وحوافها.
- `alpha_f` تتحكم في الشفافية، `thick` في سطوع الأحمر.
- البلاطات المستخدمة للدم هي 8-11 من شبكة 4x4 في الأطلس.

## قواعد مهمة قبل التعديل
1. الملف `src/game/ballistics_fx.cpp` **غير متتبّع في git (`??`)** ويحتوي أيضاً على لهب/انفجارات/دخان — غيّر أقسام الدم فقط، لا تحذف بقية الأقسام.
2. التنسيق الحالي 2 مسافة (clang-format). لا تغيّر التنسيق كلياً.
3. لا يوجد ملف صورة دم منفصل: النسيج مُولَّد برمجياً في `d3d8.cpp`، وأي تعديل على الشكل يجب أن يكون هناك.
4. الألوان تُرزم بـ `PackColorBgra(r,g,b,a)` من `world_overlay.h` (القيم 0..1).
5. بعد التعديل: `$env:PYTHONIOENCODING='utf-8'` للسكربتات، واختبار المضيف:
   `Set-Location E:\ai\black; .\tests\build\test_settings_core.exe` (المتوقع 52 ناجح، 0 فاشل).
6. بناء arm64: ninja في `app\.cxx\RelWithDebInfo\1m2l1xh6\arm64-v8a` للهدف
   `CMakeFiles/main.dir/src/game/ballistics_fx.cpp.o` و `CMakeFiles/main.dir/src/d3d/d3d8.cpp.o`،
   ثم `.\gradlew.bat assembleRelease --console=plain`.

## English summary
All blood particles/spawns are in `src/game/ballistics_fx.cpp` (`OnEnemyBloodHit`, line 1521;
render blocks 3516-3733). The blood *texture* has no PNG - it is generated procedurally in
`src/d3d/d3d8.cpp` line 1714 (`ty == 2u`, atlas tiles 8-11). `extracts/blood_sections.txt`
contains only those blocks with original line numbers. Do not touch the flamethrower,
explosion, smoke or screen-warp code in the same files.
