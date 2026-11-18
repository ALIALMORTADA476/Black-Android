# حزمة المصدر الكاملة لسلاح القناص الألماني Mauser Kar98k (`kar_98`) — مطابقة 100% لمواصفة المحرك (`tools/` Pipeline)

## 1. ملخص المطابقة مع القواعد العشر (`Golden Rule Checklist`)
| البند | الملف / المسار | الحالة والمواصفات الفعلية المحققة |
| :--- | :--- | :--- |
| **§3 بطاقة السلاح** | `metadata.json` | `weapon_id: "kar_98"`, `class: "sniper"`, `lod0: "model/kar_98_lod0.glb"`, `hands_lod0: "hands/hands_lod0.glb"`, `hand_ik_config_file: "hand_ik_mounts.json"` |
| **§4 الصوت الفيزيائي** | `audio/*.wav` (`27` ملفاً) | `48,000 Hz`, `Stereo (2-ch)`, `16-bit PCM WAV`، كل ملف `>= 1.25s`، ذروة `< 1.0`، يغطي الفتحات `132, 133, 134, 135, 145, 149` بالأسماء الحرفية |
| **§5 المقابس واليدان** | `hand_ik_mounts.json` + `hands/` | `hands/hands_lod0.glb` (`22,238` رأساً، `40,811` مثلثاً، `40` عظمة `Shoulder/Elbow/Wrist/Finger1..4/Thumb_lft/rt`) + `hands_tex_0.jpg` (`1024×1024`) + `hands_rig.json` |
| **§6 الأنيميشن الصلب** | `animation/*.json` (`14` ملفاً) | تحويلات صلبة (`rotation_deg`, `translation_m`)، مع `pivots_m` ومعاملات `bolt_lift_deg (68°)`, `bolt_retract_m (0.088m)`, `clip_insert`, `trigger_pull` |
| **§7 النار والدخان** | `vfx/vfx_config.json` + `vfx/*.png` | معاملات طيف اللهب (`2300K/1700K/1150K`)، الدخان (`192 nodes`)، موجة الصدمة (`2.8m`, `180N`)، الخطاط، و4 آثار ثقوب (`concrete, wood, metal, glass`) |
| **§8 الباليستيك والمنظار** | `weapon_stats.json` | عيار `7.92x57mm Mauser` (`760 m/s`, `3695 J`, `G7 0.242`, ضرر `98`)، جداول الانحدار والسرعة حتى `800m`، الاختراق بالسنتيمتر، ومنظار `4x/6x` |
| **§9 قيود نموذج السلاح** | `model/kar_98_lod0.glb` | `30,945` رأساً (`< 65,536 uint16`)، `60,000` مثلثاً، `6` مواد PBR، العُقد الـ 8 المستقلة بأسمائها الحرفية دون دمج اليدين |
| **§10 الواجهة والمقذوفات** | `ui/` + `props/` | `ui_icon_kar_98.png`, `sniper_scope_reticle_1024.png`, `bullet_792x57.glb/.obj`, `spent_casing_792x57.glb/.obj` |
