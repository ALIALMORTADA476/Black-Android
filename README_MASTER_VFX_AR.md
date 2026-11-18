# الحزمة الشاملة الموحدة لمؤثرات النار والدخان والدم وموجة التشويه الشفافة (`vfx_master_pack.zip`)

تجمع هذه الحزمة **كل ملفات C++ الإجرائية للمحرك** (`src/game/ballistics_fx.cpp`, `src/game/ballistics_rules.cpp`, `src/game/blood_decal_rules.cpp`, `src/game/flamethrower*.cpp`, `src/d3d/d3d8.cpp`, `src/d3d/world_overlay.h`) مع **صور وأطالس ومعاملات `vfx/` القياسية (§7)** في ملف مضغوط واحد متكامل.

## 1. ما الذي تم دمجه وتطويره في هذه الحزمة؟
1. **تأثير التشويه الشفاف حول الانفجارات (`DrawScreenWarp` + `BlastLensNode` في `ballistics_fx.cpp` سطر 3310)**:
   - تم تطوير عدسة انكسار الهواء الشفافة (`kWarpGrid = 22`، وحتى `8` عدسات متزامنة) لتعمل بفيزياء **موجة صدمة سيدوف-تايلور (`r_front = t^0.48`)** وموجة الضغط **Friedlander N-Wave** (قانون Gladstone-Dale لانكسار الضوء في الهواء المضغوط):
     - **الحافة الأمامية المضغوطة**: تحني المشهد الخلفي للخارج بوضوح مع تلاشٍ ناعم (`edge_guard`) بحيث تبدو موجة الهواء الشفافة تتمدد بسرعة فوق صوتية حول كرة اللهب.
     - **موجة Mach الخلفية + السراب الحراري (Heat Haze)**: تموج حراري شفاف داخل قلب الانفجار يحاكي صعود الغازات الساخنة، ويُرسم **قبل** كرة النار والدخان حتى لا يشوّه النار نفسها.
2. **الأطلس الإجرائي الموحد 4×4 (`SampleVfxAtlasTexelBgra` في `ballistics_rules.cpp` و`d3d8.cpp`)**:
   - **الصف 0 (Tiles 0..3 — الدخان والسخام)**: تشتت ضوئي ذاتي التظليل بقانون **Beer-Lambert** مع حافة فضية (**Mie forward-scattering silver lining**) وحركة دوامات **Kelvin-Helmholtz**.
   - **الصف 1 (Tiles 4..7 — ألسنة اللهب والنابالم)**: إشعاع الجسم الأسود الفيزيائي (**Planck Blackbody**: ذهب أبيض `2300K` ← كهرماني `1700K` ← قرمزي `1150K`) مع تموجات **Rayleigh-Taylor**.
   - **الصف 2 (Tiles 8..11 — الدم الفيزيائي بـ 4 بلاطات متخصصة)**:
     - `Tile 8`: رذاذ شرياني متذري (Atomized arterial aerosol).
     - `Tile 9`: خيوط ونفثات دم شريانية ممتدة مع انقسام **Rayleigh-Plateau** في الهواء.
     - `Tile 10`: برك دم متخثرة على الأرض بثلاث مناطق عمق ضوئي (**Beer-Lambert hemoglobin absorption**) ولمعان بلازما رطب يجف تدريجياً (`blood_decal_rules.cpp`).
     - `Tile 11`: لطخات ارتطام باليستية ذات أشواك تاجية (**High-Weber-number corona splash**).
   - **الصف 3 (Tiles 12..15 — الشرر وموجات الصدمة)**: نوى شرر متوهجة مع حيود عدسي (Tiles 12–13) وحلقات صدمة فوق صوتية (Tiles 14–15).
3. **توليد أطالس `vfx/` بدقة 1024×1024 مباشرةً من كود الـ C++**:
   - `vfx/muzzle_flash_sheet.png` (الأطلس الكامل 4×4 الناتج مباشرةً من `SampleVfxAtlasTexelBgra`).
   - `vfx/shockwave_refraction_sheet.png` (خريطة الانكسار والتشويه الشفاف لموجة الانفجار عبر 4 مراحل زمنية).
   - `vfx/smoke_trail_sheet.png`، `vfx/tracer_streak.png`، و`vfx/decal_impact_{concrete,wood,metal,glass}.png` + `vfx/vfx_config.json`.
