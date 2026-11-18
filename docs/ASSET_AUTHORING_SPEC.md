# مواصفة تسليم أصول الأسلحة — من مولّد AI إلى محرك اللعبة

> الغرض: هذه المواصفة تُعطى **حرفياً** للنموذج/الأداة التي تصنع النماذج والأنيميشنات
> والأصوات. تشرح بالضبط **أي لغة/صيغة** تكتب، **وماذا يوضع داخل الملف المضغوط**،
> و**قائمة كاملة** بكل عنصر مطلوب لكل سلاح — من أصغر ملف إلى أكبره — حتى لا نُفاجأ
> برفض نوع ملف أو تحويل ناقص عند التركيب.

---

## 0. أهم قانون (افهمه أولاً) — The Golden Rule

المحرك **لا يقرأ** ملفات Unity / Three.js مباشرة. سلسلة العمل هكذا:

```
مولّد AI  →  "حزمة مصدر" (glb + jpg + wav + json)
          →  أدوات تحويل بايثون في  tools/  (نحن نملكها ونشغّلها)
          →  صيغ أصلية للمحرك:  .bmsh  +  .btex  +  .pak
          →  اللعبة على الجهاز
```

إذن: **أنت تنتج "حزمة المصدر" بهذه المواصفة، ونحن نُحوّلها.** لا تُنتج ملفات `.pak`
أو `.bmsh` أو `.btex` بنفسك، ولا تتوقع أن يقرأ المحرك ملف `.js` أو `.cs` أو `.glb`
مباشرةً وقت التشغيل.

---

## 1. اللغات والصيغ المسموح بها (ما تكتبه بالضبط)

| الغرض | الصيغة المطلوبة | ملاحظات صارمة |
|---|---|---|
| النموذج ثلاثي الأبعاد | **glTF 2.0 ثنائي `.glb`** | ملف واحد؛ لا `.fbx`، لا `.obj` كبديل للنموذج الرئيسي |
| النسيج (Texture) | **`.jpg` (أو `.png`) بدقة 1024×1024** | Albedo فقط؛ التحويل يبني الأطلس |
| الصوت | **`.wav` PCM، 48 kHz، ستيريو (2 قناة)، 16-bit** | أي شيء آخر يُعاد أخذ عيناته، لكن هذا هو المطلوب |
| البيانات الوصفية / المقابس / المحاور / الأنيميشن / الباليستيك | **`.json` UTF-8** | مفاتيح إنجليزية exact كما في هذه المواصفة |
| تأثيراتFire/Smoke | **`.png` مرجعي + `.json` معاملات** | الصور مرجع فقط؛ المحرك يبني الشكل إجرائياً (انظر §7) |
| سكربتات Unity/Three.js | اختيارية **للمرجع فقط** | لا تُستهلك وقت التشغيل — لا تعتمد عليها |

**لغة الملفات:** كل المعرّفات (أسماء العُقد، المقابس، المفاتيح) **إنجليزية** بحروف
exact كما أدناه. الشروحات بالعربية مسموحة في `README.md` فقط.

---

## 2. هيكل الملف المضغوط (Zip) — ضع exactly هذا

```
<weapon_name>/
├── README.md                     # وصف بالعربية/الإنجليزية (اختياري لكن مفيد)
├── metadata.json                 # بطاقة السلاح (§3)
├── weapon_stats.json             # الباليستيك والضرر (§8)
├── hand_ik_mounts.json           # المقابس + أوضاع الأصابع + منحنيات IK (§5)
│
├── model/
│   ├── <weapon>_lod0.glb         # مطلوب — المستوى المستخدم فعلياً
│   ├── <weapon>_lod1.glb         # اختياري (محركنا لا يستخدمه حالياً)
│   ├── <weapon>_lod2.glb         # اختياري
│   └── <weapon>_collision.obj    # اختياري (مرجع تصادم فقط)
│
├── hands/                        # يد حقيقية مُهيكلة (§5) — بدل توليد المحرك
│   ├── hands_lod0.glb            # شبكة اليدين/الذراعين مع skinning + هيكل عظمي
│   ├── hands_tex_0.jpg           # نسيج 1024² (جلد/قفاز)
│   ├── hands_rig.json            # أسماء المفاصل + أوضاع الأصابع + منحنيات IK
│   └── hands_readme.md           # وصف اختياري
│
├── textures/
│   ├── <weapon>_tex_0.jpg        # معدن أزرق/داكن (blued steel)
│   ├── <weapon>_tex_1.jpg        # فولاذ satin
│   ├── <weapon>_tex_2.jpg        # خشب
│   └── ...                       # أي نسيج إضافي يخص السلاح
│
├── animation/                    # 14 ملف JSON (§6)
│   ├── idle.json
│   ├── walk.json
│   ├── sprint.json
│   ├── fire_hip.json
│   ├── fire_ads.json
│   ├── bolt_cycle.json           # الأهم: يحوي pivots المحاور المتحركة
│   ├── reload_stripper_clip.json
│   ├── reload_single_round.json
│   ├── equip.json
│   ├── holster.json
│   ├── ads_in.json
│   ├── ads_out.json
│   ├── inspect_weapon.json
│   └── dry_fire.json
│
├── audio/                        # 27 ملف WAV (§4) — الأسماء exact
│   ├── fire_indoor_01.wav ... fire_indoor_04.wav
│   ├── fire_outdoor_01.wav ... fire_outdoor_03.wav
│   ├── fire_suppressed_01.wav
│   ├── bolt_open.wav / bolt_open_01.wav / bolt_open_02.wav
│   ├── bolt_pull.wav  / bolt_pull_01.wav  / bolt_pull_02.wav
│   ├── bolt_push.wav  / bolt_push_01.wav  / bolt_push_02.wav
│   ├── bolt_open_and_pull_raw.wav
│   ├── bolt_push_and_close_raw.wav
│   ├── bolt_cycle_full.wav
│   ├── bolt_close.wav / bolt_close_01.wav / bolt_close_02.wav
│   ├── clip_insert.wav
│   ├── dry_fire_click.wav
│   ├── equip_foley.wav
│   └── reload_full_master.wav
│
├── vfx/
│   ├── vfx_config.json           # معاملات اللهب/الدخان/الشرر (§7)
│   ├── muzzle_flash_sheet.png    # مرجع 1024p
│   ├── smoke_trail_sheet.png     # مرجع 1024p
│   ├── tracer_streak.png
│   └── decal_impact_{concrete,wood,metal,glass}.png
│
├── ui/
│   ├── ui_icon_<weapon>.png
│   └── sniper_scope_reticle_1024.png   # للبندقية ذات المنظار فقط
│
└── props/
    ├── bullet_<caliber>.glb / .obj
    └── spent_casing_<caliber>.glb / .obj
```

---

## 3. metadata.json — بطاقة السلاح

```json
{
  "weapon_id": "<machine_id>",
  "display_name": "<name>",
  "class": "sniper | pistol | smg | lmg | flamethrower",
  "hand_ik_config_file": "hand_ik_mounts.json",
  "sockets_preview": { "...": "..." },
  "lod0": "model/<weapon>_lod0.glb",
  "texture_sources": ["textures/<weapon>_tex_0.jpg", "..."]
}
```

---

## 4. الصوت — القائمة الكاملة والقواعد

**قواعد صارمة:**
- 48 kHz، ستيريو، 16-bit PCM WAV.
- **كل ملف ≥ 1.25 ثانية** (حتى لا يظهر `00:00`).
- خمس طبقات فيزيائية مطلوبة في كل طلقة: (1) انفجار موجة N فوق صوتية،
  (2) ضغط تحت-باس، (3) هدير البارود، (4) صوت الميكانيكا، (5) ردّين indoor/outdoor.
- لا clipping: الذروة < 1.0.

**التعيين إلى فتحات المحرك (slots)** — الأسماء يجب أن تطابق:

| slot | النوع | الملفات |
|---|---|---|
| 132 | طلقات داخلية | `fire_indoor_01..04` |
| 133 | ميكانيكا الترباس | `bolt_open/01/02`, `bolt_pull/01/02`, `bolt_push/01/02`, `bolt_cycle_full` |
| 134 | إدخال الممشط | `clip_insert` |
| 135 | طلقة فارغة | `dry_fire_click` |
| 145 | طلقات خارجية | `fire_outdoor_01..03` |
| 149 | إغلاق الترباس | `bolt_close/01/02` |

الملفات الخمسة `*_raw`, `equip_foley`, `fire_suppressed_01`, `reload_full_master`
نسخ خام/_master اختيارية — أرسلها إن وُجدت لكن المحرك لا يستخدمها حالياً.

---

## 5. hand_ik_mounts.json — المقابس (Socket) وأوضاع الأصابع

**نظام الإحداثيات:** المحرك يحوّل `pack(x=forward, y=up, z=right) → d3d(z, y, x)`.
أعطِ الإحداثيات بالأمتار بنظام pack.

**ستة مقابس بأسماء exact:**

```json
{
  "weapon_id": "<id>",
  "embedded_glb_hand_sockets": {
    "Socket_RightHand_Grip":     { "parent_node": "Weapon_Root",    "world_position_m": [0.245,-0.018,0.008], "rotation_euler_deg": [-18,0,-24] },
    "Socket_TriggerFinger_IK":   { "parent_node": "Trigger_Gatilho","world_position_m": [0.319,0.0,0.002] },
    "Socket_LeftHand_Foregrip":  { "parent_node": "Weapon_Root",    "world_position_m": [0.555,0.012,-0.006], "rotation_euler_deg": [75,10,8] },
    "Socket_BoltKnob_IK":        { "parent_node": "Bolt_Handle",    "world_position_m": [0.295,0.022,0.048] },
    "Socket_ClipThumb_IK":       { "parent_node": "Stripper_Clip",  "world_position_m": [0.335,0.068,0.0] },
    "Socket_Shoulder_Stock":     { "parent_node": "Weapon_Root",    "world_position_m": [0.015,-0.025,0.0] }
  },
  "finger_curl_poses_deg": {
    "right_hand_grip":     { "thumb":[22,34,28], "index_on_trigger":[18,42,35], "middle":[58,68,52], "ring":[62,72,55], "pinky":[65,74,58] },
    "left_hand_foregrip":  { "thumb":[35,42,25], "index":[48,58,42], "middle":[52,62,46], "ring":[55,64,48], "pinky":[58,66,50] },
    "right_hand_bolt_pinch":{ "thumb":[38,45,35], "index":[42,52,40], "middle":[46,56,44], "ring":[70,78,65], "pinky":[75,82,68] }
  },
  "animation_ik_weight_curves": {
    "idle_walk_sprint_fire": { "left_target":"Socket_LeftHand_Foregrip", "right_target":"Socket_RightHand_Grip" },
    "bolt_cycle": [
      {"time_s":0.0,"right_target":"Socket_RightHand_Grip"},
      {"time_s":0.08,"right_target":"Socket_BoltKnob_IK"},
      {"time_s":0.92,"right_target":"Socket_RightHand_Grip"}
    ],
    "reload_stripper_clip": [
      {"time_s":0.1,"right_target":"Socket_BoltKnob_IK"},
      {"time_s":0.48,"right_target":"Socket_ClipThumb_IK"},
      {"time_s":2.58,"right_target":"Socket_RightHand_Grip"}
    ]
  }
}
```

> ملاحظة مهمة (مُحدَّثة): أرسل **يداً حقيقية مُهيكلة بصيغة `.glb`** في مجلد `hands/`
> (راجع `docs/AI_HAND_GLB_PROMPT.md` للمواصفة الكاملة). لم نعد نعتمد على قفاز إجرائي
> يولّده المحرك. اليد يجب أن تحوي الهيكل العظمي بأسماء المفاصل المذكورة أدناه
> (`Shoulder/Elbow/Wrist/Finger1..4_Top|Mid|Tip/Thumb_Top|Mid|Tip` لليسار `_lft`
> واليمين `_rt`)، وتُوضَع بحيث تلمس المقابس الستة أدناه في وضع الإمساك.
> أوضاع الأصابع ومنحنيات IK تُترجم إلى ثوابت C++ — أرسلها كاملة فهي مرجعنا.

---

## 6. الأنيميشنات — النوع والمحتوى

**نوع الأنيميشن المطلوب: تحويلات صلبة لكل جزء (rigid per-part transforms)**
وليس skinning هيكلياً. المحرك يدوّر/ينقل كل جزء حول **محور pivot** بمعاملات عددية.

**أسماء العُقد داخل الـ GLB (exact):**
`Weapon_Root`, `Receiver_And_Body`, `Scope_Assembly`, `Trigger_Gatilho`,
`Bolt_Carrier`, `Bolt_Handle`, `Bolt_Safety_Sleeve`, `Stripper_Clip`.

**كل ملف أنيميشن JSON يجب أن يحوي:**
- `name`:相符 مع اسم الملف.
- `duration_s`, `fps`.
- `pivots_m`: إحداثيات محور الدوران لكل جزء متحرك (مثال bootstrap:
  `Bolt_Handle = [0.295,0.0395,0]`, `Stripper_Clip = [0.335,0.052,0]`).
- `tracks`: لكل جزء `{ "rotation_deg":[x,y,z], "translation_m":[x,y,z] }` لكل إطار،
  أو على الأقل القيم القصوى: `bolt_lift_deg` (0..68)، `bolt_retract_m` (0..0.096)،
  `clip_insert` (0..1)، `clip_eject` (0..1)، `trigger_pull` (0..1).

> الأهم عملياً: `bolt_cycle.json` — منه نأخذ الـ pivots. بقية الملفات مرجع للسلوك.

---

## 7. تأثيرات النار والدخان (Fire/Smoke/VFX) — ماذا نريد بالضبط

المحرك **يبني شكل اللهب/الدخان إجرائياً** في `SampleVfxAtlasTexelBgra` ويكتبه في
أطلس 4×4: الصف 0 = سخام/دخان، الصف 1 = ألسنة لهب، الصف 2 = دم، الصف 3 = شرر/موجة صدمة.

لذا أرسل:
1. **`vfx_config.json`** بمعاملات قابلة للنقل إلى C++:
```json
{
  "fire_spectrum_kelvin": {"hottest":2300,"mid":1700,"coolest":1150},
  "fire_colors": {"white_gold":[255,240,184],"amber":[254,224,155],"crimson":[246,140,39]},
  "muzzle_flash": {"scale":1.0,"duration_s":0.06,"tongues":7},
  "smoke": {"mode":2,"intensity":105,"duration_s":1.10,"buoyancy":0.8,"drag":0.35,"nodes":192},
  "shockwave": {"radius_m":2.8,"impulse_n":180},
  "tracer": {"length_m":1.2,"color":[255,190,80]},
  "decals": ["concrete","wood","metal","glass"]
}
```
2. **صور مرجعية PNG** (muzzle_flash_sheet, smoke_trail_sheet, tracer, decals) —
   للإلهام البصري؛ نحن نُطابقها إجرائياً.
3. لكل طلقة: **فوهة muzzle position** و**اتجاه** في الـ GLB (نقطة `muzzle`).

---

## 8. weapon_stats.json — الباليستيك والضرر

```json
{
  "caliber": "7.92x57mm Mauser",
  "projectile_mass_g": 12.8,
  "muzzle_velocity_mps": 760,
  "energy_j": 3695,
  "drag_model": "G7",
  "drag_coefficient": 0.242,
  "base_damage": 98,
  "weapon_class": "sniper",
  "drop_table_m": [ {"range":0,"drop_m":0}, {"range":100,"drop_m":...}, "... إلى 800m" ],
  "velocity_table": [ "... إلى 800m" ],
  "hitbox_multipliers": {"head":2.5,"neck":1.85,"upper_chest":1.25},
  "penetration_cm": {"wood":32,"concrete":9.5,"mild_steel":1.1,"glass":...,"sand":...},
  "scope": {"magnification":[4,6],"fov_deg":[15,10],"zeroing_m":[100,200,400,600,800]}
}
```

>_confirm_: المحرك لديه قيمه المدمجة حالياً؛ أرسل هذا الملف ليتمكن من مطابقتها أو تعديلها.

---

## 9. قيودGLB تقنية (حتى لا يُرفض النموذج)

- **عدد الرؤوس < 65,536** (الفهارس `uint16`). هدفنا ~30,942 رأس / 60,000 مثلث.
- الوحدة = **متر**؛ طول البندقية ~1.1 m.
- المواد: PBR metallic-roughness؛ **6 مواد كحد أقصى** تُربط بمناطق الأطلس.
- الأطلس النهائي 1024×1024، شبكة 2×2 (4 مناطق 512×512).
- كل عقدة متحركة يجب أن تكون **عقدة مستقلة** باسمها exact (§6) — لا دمجها في جسم واحد.
- أرسل اليدين كشبكة `.glb` **مستقلة** في `hands/` (§5) — لا تدمجهما في شبكة السلاح،
  ولا يولّدهما المحرك إجرائياً بعد الآن.

---

## 10. قائمة تحقق نهائية (من أصغر شيء لأكبره)

- [ ] `metadata.json` بمعرّف وclass.
- [ ] `hand_ik_mounts.json`: 6 مقابس + 3 أوضاع أصابع + منحنيات IK.
- [ ] `weapon_stats.json`: ضرر/سرعة/انحدار/hitbox/اختراق/منظار.
- [ ] `vfx_config.json` + صور VFX مرجعية + نقطة الفوهة.
- [ ] 27 WAV (48k/stereo/16-bit, ≥1.25s) بالأسماء exact.
- [ ] 14 أنيميشن JSON مع `pivots_m` في `bolt_cycle.json`.
- [ ] نسيج/أنسجة 1024².
- [ ] `model/<weapon>_lod0.glb` (<65k verts، عُقد exact، لا يدين).
- [ ] `hands/hands_lod0.glb` (يدان+ذراعان مع skinning، هيكل عظمي بأسماء المفاصل exact، تلمس المقابس الستة) + `hands/hands_tex_0.jpg` + `hands/hands_rig.json` (§5).
- [ ] `ui/` (أيقونة + رتICLE للمنظار)، `props/` (رصاصة/غلاف).

---

### ملاحظة للقارئ (أنت)
هذه المواصفة مبنية على **فحص فعلي** لأدوات التحويل في `tools/` وللحزمة الحالية،
وليست تخميناً. أي ملف لا يوجد له "مسار تحميل" في المحرك (مثل سكربتات Unity،
الأنيميشن الهيكلي، LOD1/2، المجسمات المستقلة) مُصنَّف "مرجعي" — أرسله لكن لا تعتمد
على أن اللعبة ستقرأه مباشرة.
