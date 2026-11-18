# حزمة مصدر قاذف اللهب التكتيكي — `flamethrower` Source Asset Pack

هذه الحزمة مبنية بالكامل وفق **"القانون الذهبي" (The Golden Rule — §0..§10)** كحزمة مصدر جاهزة للتحويل عبر أدوات `tools/` إلى الصيغ الأصلية للمحرك (`.bmsh` + `.btex` + `.pak`).

## 1. محتويات الحزمة ومطابقة المواصفات

- **`metadata.json` (§3)**: معرّف السلاح `flamethrower`، الفئة `flamethrower`، وربط `hand_ik_mounts.json` و`model/flamethrower_lod0.glb` والأنسجة.
- **`model/flamethrower_lod0.glb` (§6, §9)**:
  - صيغة `glTF 2.0` ثنائية (`.glb`) بـ **17,888 رأساً** (`uint16` أقل بكثير من حد `65,536`) و**26,726 مثلثاً**.
  - طول السلاح **1.11 متر** على المحور الأمامي `pack(x=forward, y=up, z=right)`.
  - مفصول إلى **8 عُقد هرمية مستقلة بأسمائها الدقيقة (exact)** لتحريك كل جزء حول محوره (`pivots_m`):
    1. `Weapon_Root`
    2. `Receiver_And_Body`
    3. `Scope_Assembly` (واقي الفوهة الحراري وشعلة الإشعال)
    4. `Trigger_Gatilho` (ذراع الزناد/الصمام الأمامي)
    5. `Bolt_Carrier` (جسم الصمام الرئيسي)
    6. `Bolt_Handle` (عجلة/ذراع ضغط الوقود)
    7. `Bolt_Safety_Sleeve` (صمام الأمان الخلفي)
    8. `Stripper_Clip` (وحدة خزانات الوقود المضغوط القابلة للتبديل)
    9. `Muzzle` (نقطة الفوهة عند `[0.990, -0.010, 0.000]` باتجاه `+X`).
- **`hands/hands_lod0.glb` (§5)**:
  - شبكة اليدين والذراعين الحقيقية (14,160 رأساً، 25,632 مثلثاً) مع Skinning كامل (`JOINTS_0`, `WEIGHTS_0`, `inverseBindMatrices`) وهيكل عظمي يضم المفاصل القياسية (`World`, `Extra`, `Root`, `Shoulder_lft/_rt`, `Elbow_lft/_rt`, `Joint_wrist_twist_lft/_rt`, `Wrist_lft/_rt`, `Thumb_Top|Mid|Tip`, `Finger1..4_Top|Mid|Tip`) موضوعة بحيث تلمس المقابس الستة في `hand_ik_mounts.json`.
- **`textures/` (§1, §9)**: أنسجة `1024×1024` بصيغة `.jpg` تشمل المعدن الداكن وخزان الوقود (`flamethrower_tex_0.jpg`)، الفولاذ الحراري (`flamethrower_tex_1.jpg`)، المقابض والخرطوم (`flamethrower_tex_2.jpg`)، الصمامات النحاسية (`flamethrower_tex_3.jpg`)، والأطلس المجمع 2×2 (`flamethrower_atlas.jpg`).
- **`audio/` (§4)**: **27 ملف WAV** بالأسماء الدقيقة، كلها بتردد **48 kHz، ستيريو (2 قناة)، 16-bit PCM**، بمدة **≥ 1.25 ثانية** وذروة **< 1.0**، مبنية من التسجيلات الحقيقية السبعة مع الطبقات الفيزيائية الخمس والصدى الداخلي والخارجي.
- **`animation/` (§6)**: **14 ملف JSON** لتحويلات الأجزاء الصلبة (`idle`, `walk`, `sprint`, `fire_hip`, `fire_ads`, `bolt_cycle`, `reload_stripper_clip`, `reload_single_round`, `equip`, `holster`, `ads_in`, `ads_out`, `inspect_weapon`, `dry_fire`) مع تضمين `pivots_m` الدقيقة في `bolt_cycle.json` وجميع الملفات.
- **`vfx/` (§7)**: ملف `vfx_config.json` بمعاملات طيف اللهب (`2300K / 1700K / 1150K`) والدخان وموجة الصدمة، مع أطلس `muzzle_flash_sheet.png` (1024×1024 مقسم 4×4 للصفوف الأربعة: سخام/دخان، ألسنة لهب، دم/هلام، شرر/موجة صدمة) و`smoke_trail_sheet.png` و`tracer_streak.png` وأربعة ملصقات تصادم `decal_impact_{concrete,wood,metal,glass}.png`.
