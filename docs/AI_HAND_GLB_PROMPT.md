# برومبت مولّد اليد الحقيقية (.glb) — انسخه كما هو للـ AI

> الصق هذا البرومبت كاملاً في أول رسالة لمولّد الأصول ثلاثية الأبعاد.
> المعرّفات الإنجليزية يجب أن تُكتب **exact** كما هي (أسماء المفاصل/المقابس).
> supersedes القديم الذي كان يقول «المحرك يولّد اليد إجرائياً».

---

## 📋 البرومبت (ابدأ النسخ من هنا)

```
أنت مولّد أصول ثلاثية الأبعاد احترافي (AAA 3D asset generator).
صمّم يدين بشريتين واقعيتين مع ساعدين (first-person hands + forearms) لتُركّبا
على سلاح ناري في لعبة تصويب من المنظور الأول. المخرج النهائي يجب أن يكون
مُهيّئاً للاستيراد المباشر في محركنا دون أي تحويل إضافي.

═══════════════════════════════════════════
1) المخرجات المطلوبة (داخل نفس الملف المضغوط)
═══════════════════════════════════════════
hands/hands_lod0.glb        ← نموذج مُهيكل (skinned mesh) لليدين + الساعدين
hands/hands_tex_0.jpg       ← نسيج 1024×1024 (جلد طبيعي أو قفاز تكتيكي)
hands/hands_rig.json        ← أسماء المفاصل + أوضاع الأصابع + منحنيات IK (انظر §5)
hands/hands_readme.md       ← وصف مختصر (اختياري)

═══════════════════════════════════════════
2) نظام الإحداثيات (إلزامي — لا تخالفه)
═══════════════════════════════════════════
- المحرك يحوّل دائماً:  pack(x=forward, y=up, z=right)  →  d3d(z, y, x)
- أعطِ الإحداثيات بالأمتار بنظام pack. الوحدة = متر (ليس سنتيمتر).
- أصل الإحداثيات (0,0,0) = مركز السلاح Weapon_Root (مؤخرة السلاح).
- اتجاه x للأمام = اتجاه الفوهة (muzzle).

═══════════════════════════════════════════
3) الهيكل العظمي (Armature) — أسماء المفاصل exact
═══════════════════════════════════════════
هذه هي أسماء مفاصل يد اللعبة الأصلية — استخدمها حرفياً:

الجهة اليسرى (_lft):
  World, Extra, Root,
  Shoulder_lft, Elbow_lft, Joint_wrist_twist, Wrist_lft,
  Finger1_Top_lft, Finger1_Mid_lft, Finger1_Tip_lft,
  Finger2_Top_lft, Finger2_Mid_lft, Finger2_Tip_lft,
  Finger3_Top_lft, Finger3_Mid_lft, Finger3_Tip_lft,
  Finger4_Top_lft, Finger4_Mid_lft, Finger4_Tip_lft,
  Thumb_Top_lft,  Thumb_Mid_lft,  Thumb_Tip_lft

الجهة اليمنى (_rt):
  Shoulder_rt, Elbow_rt, Wrist_rt,
  Finger1_Top_rt, Finger1_Mid_rt, Finger1_Tip_rt,
  Finger2_Top_rt, Finger2_Mid_rt, Finger2_Tip_rt,
  Finger3_Top_rt, Finger3_Mid_rt, Finger3_Tip_rt,
  Finger4_Top_rt, Finger4_Mid_rt, Finger4_Tip_rt,
  Thumb_Top_rt,  Thumb_Mid_rt,  Thumb_Tip_rt

قواعد صارمة للهيكل:
- كل مفصل يجب أن يكون عقدة (node) مستقلة باسمه exact.
- اليدان يجب أن تكون skinned mesh على هذه العظام (weights صحيحة،
  لا تدمج اليدين في شبكة صلبة واحدة جامدة).
- Finger1 = السبابة، Finger2 = الوسطى، Finger3 = البنصر، Finger4 = الخنصر.
- أضف مفصل Joint_wrist_twist بين Elbow_lft و Wrist_lft (دوران المعصم).

═══════════════════════════════════════════
4) نقاط التثبيت (Sockets) — اليدان تلمسان هذه المواقع عند الإمساك
═══════════════════════════════════════════
  Socket_RightHand_Grip     : pos [ 0.245, -0.018, 0.008 ], rot [-18, 0, -24]
  Socket_TriggerFinger_IK   : pos [ 0.319,  0.000, 0.002 ]
  Socket_LeftHand_Foregrip  : pos [ 0.555,  0.012,-0.006 ], rot [ 75, 10, 8 ]
  Socket_BoltKnob_IK        : pos [ 0.295,  0.022, 0.048 ]
  Socket_ClipThumb_IK       : pos [ 0.335,  0.068, 0.000 ]
  Socket_Shoulder_Stock     : pos [ 0.015, -0.025, 0.000 ]

═══════════════════════════════════════════
5) hands_rig.json — أوضاع الأصابع + منحنيات IK
═══════════════════════════════════════════
أخرج ملف JSON بالبنية التالية (زوايا بالدرجات لكل مفصل [قاعدة، وسط، طرف]):

{
  "weapon_id": "<machine_id>",
  "finger_curl_poses_deg": {
    "right_hand_grip":      { "thumb":[22,34,28], "index_on_trigger":[18,42,35], "middle":[58,68,52], "ring":[62,72,55], "pinky":[65,74,58] },
    "left_hand_foregrip":   { "thumb":[35,42,25], "index":[48,58,42], "middle":[52,62,46], "ring":[55,64,48], "pinky":[58,66,50] },
    "right_hand_bolt_pinch":{ "thumb":[38,45,35], "index":[42,52,40], "middle":[46,56,44], "ring":[70,78,65], "pinky":[75,82,68] }
  },
  "animation_ik_weight_curves": {
    "idle_walk_sprint_fire": { "left_target":"Socket_LeftHand_Foregrip", "right_target":"Socket_RightHand_Grip" },
    "bolt_cycle": [
      {"time_s":0.0,  "right_target":"Socket_RightHand_Grip"},
      {"time_s":0.08, "right_target":"Socket_BoltKnob_IK"},
      {"time_s":0.92, "right_target":"Socket_RightHand_Grip"}
    ],
    "reload_stripper_clip": [
      {"time_s":0.1,  "right_target":"Socket_BoltKnob_IK"},
      {"time_s":0.48, "right_target":"Socket_ClipThumb_IK"},
      {"time_s":2.58, "right_target":"Socket_RightHand_Grip"}
    ]
  }
}

═══════════════════════════════════════════
6) القيود التقنية (حتى لا يُرفض النموذج)
═══════════════════════════════════════════
- glTF 2.0 ثنائي (.glb) فقط. لا .fbx / .obj كبديل للنموذج النهائي.
- عدد الرؤوس لليدين معاً < 65,536 (فهارس uint16). الهدف ~20,000-35,000 رأس.
- الوحدة متر؛ طول الساعد من الكوع إلى المعصم ~0.28 m، طول اليد ~0.19 m.
- مادة PBR metallic-roughness؛ نسيج albedo واحد 1024×1024 كحد أقصى.
- لا تضع اليدين داخل نموذج السلاح — ملف glb منفصل تماماً.
- لا تكتب سكربتات Unity (C#) أو Three.js (JS) — مرجعية فقط ولن تُستخدم.
- لا تضف تأثيرات/fire/smoke/ decryption — فقط هندسة اليد + الهيكل العظمي.

═══════════════════════════════════════════
7).what ليس مطلوباً
═══════════════════════════════════════════
- لا تُنتج قفازاً إجرائياً بسيطاً؛ نريد يداً حقيقية مُفصّلة (أصابع + أظافر
  + تجاعيد المفاصل + ساعد بملابس/قفاز).
- لا تعتمد على أن المحرك سيولّد اليد — المحرك يستقبل هذا الملف كما هو ويعرضه.
```

## ✅ قائمة تحقق سريعة للبرومبت
- [ ] glb منفصل عن السلاح، skinned على الأسماء exact.
- [ ] الإحداثيات بنظام pack وبالأمتار.
- [ ] 6 مقابس + 3 أوضاع أصابع + منحنيات IK في `hands_rig.json`.
- [ ] < 65k رأس، نسيج 1024²، لا سكربتات، لا تأثيرات.
