import json
import os

# Load extracted strings
with open('scripts/all_strings.json', 'r', encoding='utf-8') as f:
    all_strings = json.load(f)

# Build a lookup by string ID (int)
string_by_id = {s['id_int']: s['english'] for s in all_strings}
string_by_hex = {s['id'].upper(): (s['id_int'], s['english']) for s in all_strings}

print(f"Loaded {len(all_strings)} strings from all_strings.json")

# Core verified translation database:
# (hex_id, arabic, context, category)
translations_db = [
    # --- Mission Objectives (Global & Campaign) ---
    ("0x80238916", "الأهداف الرئيسية فقط. وفرة في حقائب العلاج. لا توجد مكافآت.", "Difficulty: Easy description", "hud"),
    ("0x8024F22A", "احتمِ بشواهد القبور من نيران القناصة", "Mission 3: Nazran Cemetery — Tactical hint", "prompt"),
    ("0x81E26F75", "معلومات بلاك: يمكن تفجير القنابل اليدوية بالرصاص في الجو", "Tactical Intel: Grenades", "intel"),
    ("0x832FDFD9", "تم تدمير المصهر 2 من 3", "Mission 4: Nazran Foundry — Status", "hud"),
    ("0x836043E6", "معلومات بلاك: شريط الصحة الأحمر ينذر باقتراب نفاد طاقتك", "Tactical Intel: Health bar", "intel"),
    ("0x840D87FF", "معلومات بلاك: المؤشر الأحمر يحدد اتجاه إطلاق النار المعادي", "Tactical Intel: Hit ring", "intel"),
    ("0x855C6280", "تحديث أهداف التدمير", "HUD: Destruction objective update", "objective"),
    ("0x858C8013", "لقد دمّرت تسجيلات كاميرات المراقبة المهربة من معسكر إكس-راي", "Recon Objective: Camp X-Ray", "objective"),
    ("0x85FBCC48", "احبس الأنفاس لتثبيت القنص", "Sniper: Hold breath", "prompt"),
    ("0x8675C49B", "دمّر لوحات التحكم في مصهر المعادن", "Mission 4: Nazran Foundry — Objective", "objective"),
    ("0x868FAB6C", "معلومات بلاك: الأجسام التي تحمل مثلثاً أصفر تنفجر عند إطلاق النار عليها", "Tactical Intel: Exploding objects", "intel"),
    ("0x887B9B21", "القضاء على جميع القناصة", "Mission 3: Nazran Cemetery — Objective", "objective"),
    ("0x8992BE01", "اعثر على الأسلحة لإكمال أهداف التسلح", "Armament Objective hint", "objective"),
    ("0x8B92ABA6", "معلومات بلاك: يمكنك حمل سلاحين في نفس الوقت", "Tactical Intel: Carrying weapons", "intel"),
    ("0x8BD3CB27", "أكمل %d من الأهداف", "Mission summary: Objectives complete", "hud"),
    ("0x8CFF6FBF", "معلومات بلاك: حطم النوافذ الزجاجية بالضرب باليد أو السلاح", "Tactical Intel: Melee windows", "intel"),
    ("0x8D715B6D", "دمّر خزانات الغاز الضخمة", "Mission 2: Trenches — Objective", "objective"),
    ("0x8EC35B00", "لقد دمّرت وثائق سرية تشرح تتبع وتشويش الهواتف", "Recon Objective: Meaconing", "objective"),
    ("0x8EFF7A18", "دمّر الوثائق الحساسة لإكمال أهداف الابتزاز", "Blackmail Objective", "objective"),
    ("0x8FF5C427", "لقد عثرت على مخططات بنادق البلازما المتطورة", "Recon Objective: Blueprints", "objective"),
    ("0x905E4EE0", "اضغط START للمتابعة", "Prompt: Press start", "prompt"),
    ("0x92899AE7", "معلومات بلاك: انحنِ خلف الساتر لتفادي نيران العدو", "Tactical Intel: Crouch cover", "intel"),
    ("0x95E45EFE", "معلومات بلاك: ابحث عن أسلحة الأعداء لإكمال أهداف التسلح", "Tactical Intel: Armament", "intel"),
    ("0x9652662B", "هدف تدمير جديد متاح", "HUD: New destruction objective", "objective"),
    ("0x974F1125", "أكملت %d من أصل %d من الأهداف الثانوية", "HUD: Secondary objectives progress", "hud"),
    ("0x98F95E34", "معلومات بلاك: مسدس الماغنوم يقضي على الأعداء برصاصة واحدة", "Tactical Intel: Magnum", "intel"),
    ("0x9F949A2D", "معلومات بلاك: الأسلحة الفضية تمتلك ذخيرة لا نهائية", "Tactical Intel: Silver weapons", "intel"),
    ("0x9F96393E", "لقد دمّرت الأشرطة الصوتية السرية", "Recon Objective: Audiotapes", "objective"),
    ("0xA0DA797C", "دمّر جميع لوحات التحكم", "Mission 4: Destroy control panels", "objective"),
    ("0xA11CE423", "معلومات بلاك: أهداف الاستطلاع تمنحك معلومات للمهمات القادمة", "Tactical Intel: Recon", "intel"),
    ("0xA14008B3", "استخدم الشوزن لتحطيم وتفجير الأبواب", "Prompt: Shotgun doors", "prompt"),
    ("0xA1C29808", "دمّر الدفاعات الخارجية لمدخل الغولاغ", "Mission 8: Spetriniv Gulag — Objective", "objective"),
    ("0xA2BBD5E7", "تم تدمير آلات تصنيع السلاح %1 من %2", "Mission 4: Nazran Foundry — Status", "hud"),
    ("0xA4831CA1", "اعبر زقاق القناصة بحذر", "Mission 3: Nazran Cemetery — Objective", "objective"),
    ("0xA4C85844", "تصفية القناص المتمركز في البرج", "Mission 3: Nazran Cemetery — Objective", "objective"),
    ("0x00422B61", "تم تدمير الهدف %1 من أصل %2", "HUD: Destruction progress", "hud"),
    ("0x0062482C", "تبديل إلى السلاح التالي", "HUD: Next weapon", "prompt"),
    ("0x01BBE675", "اضغط للحصول على معلومات تكتيكية", "HUD: Press for info", "prompt"),
    ("0x01CA3726", "ملاحقة المشتبه به في الموجة السابعة: لينوكس", "Mission 8: Spetriniv Gulag — Primary", "objective"),
    ("0x01F00F8E", "معلومات بلاك: سلاح P90 فعال ومثالي في المدى المتوسط", "Tactical Intel: P90", "intel"),
    ("0x01F2AC9D", "لقد دمّرت تفاصيل معسكر الاستخبارات الأوروبي", "Recon Objective: Bondsteel", "objective"),
    ("0x020BE815", "القضاء على حراس فالنسيو الشخصيين", "Mission 5: Tivliz Asylum — Primary", "objective"),
    ("0x022F7FC6", "إجمالي عدد الأعداء المتمركزين:", "HUD: Total enemy count", "hud"),
    ("0x02368776", "معلومات بلاك: ابحث حولك للعثور على كاتم الصوت", "Tactical Intel: Silencer", "intel"),
    ("0x0240ACF0", "تصفية رماة قذائف الآر بي جي", "Mission 7: Graznei Bridge — Primary", "objective"),
    ("0x0372231D", "معلومات بلاك: الأعداء الذين يسقطون يتركون حقائب إسعاف أولية", "Tactical Intel: Health drops", "intel"),
    ("0x03AB5B62", "معلومات المهمة والعمليات", "HUD: Mission info", "hud"),
    ("0x03CCD097", "انحنِ خلف الساتر", "Action Prompt: Crouch", "prompt"),
    ("0x04550FFD", "تحديث المهمة الرئيسية", "HUD: Primary objective update", "objective"),
    ("0x0618C791", "تم تدمير خزان الغاز %1 من أصل %2", "Mission 2: Trenches — Status", "hud"),
    ("0x069DCB97", "معلومات بلاك: أطلق النار على القنبلة في الجو لقتل الأعداء القريبين منها", "Tactical Intel: Mid-air grenade", "intel"),
    ("0x06DD643B", "اعثر على سلاح الشوزن", "Prompt: Find shotgun", "prompt"),
    ("0x074A6CBC", "تصفية جميع الأعداء في المنطقة", "Mission 1: Veblensk Streets — Primary", "objective"),
    ("0x077ADBAA", "لا توجد مهمات رئيسية نشطة حالياً", "HUD: No active primary objective", "hud"),
    ("0x07A9F7A3", "عثرت على مخطط تفصيلي لشبكة قطارات الأنفاق", "Recon Objective: Subway blueprints", "objective"),
    ("0x07DAADA6", "تم القضاء على رماة الآر بي جي %1 من %2", "Mission 7: Graznei Bridge — Status", "hud"),
    ("0x08073598", "اكتملت المهمة: %s - %s بنجاح", "Mission complete banner", "hud"),
    ("0x084643A5", "معلومات بلاك: هل تمتلك المهارة الكافية لفتح بندقية M16A2؟", "Tactical Intel: M16A2", "intel"),
    ("0x084A379A", "عثرت على وثيقة رسمية مختومة بختم سري للغاية", "Recon Objective: Secret document", "objective"),
    ("0x090F77E3", "ابحث عن مخزن الأسلحة السري", "Mission 1: Veblensk — Objective", "objective"),
    ("0x09443306", "إلقاء القبض على المشتبه به من الموجة السابعة", "Mission 5: Tivliz Asylum — Objective", "objective"),
    ("0x0980DEFA", "استكشف المكان للعثور على حقيبة إسعاف", "Prompt: Find health pack", "prompt"),
    ("0x0B80CB5D", "معلومات بلاك: انحنِ خلف الساتر للاختباء من رصاص الأعداء", "Tactical Intel: Crouch cover", "intel"),
    ("0x0BB31A06", "معلومات الاستخبارات للمهمة %1/%2", "Intel objective status", "hud"),
    ("0x0CE62CB0", "عثرت على بطاقة هوية مزورة لمنشأة نووية", "Recon Objective: Fake ID", "objective"),
    ("0x0CED0F44", "معلومات بلاك: اجمع وثائق المهمة لإكمال أهداف الاستطلاع", "Tactical Intel: Recon collection", "intel"),
    ("0x0E98F9CA", "تم تدمير سلاح الرشاش الثقيل 3 من 4", "Mission 2: Trenches — Status", "hud"),
    ("0x0EED1AE3", "الحلقة الحمراء تحدد اتجاه الهجوم المعادي بدقة", "Tactical Intel: Direction ring", "intel"),
    ("0x0F465F7D", "اكتمل الهدف الرئيسي للمهمة!", "HUD: Primary objective complete", "objective"),
    ("0x0FD34110", "تطهير شارع فيبلنسك من المتاريس الإرهابية", "Mission 1: Veblensk — Primary", "objective"),
    ("0x10A8BF83", "اقتحام قبو المبنى وتطهيره", "Mission 1: Veblensk — Objective", "objective"),
    ("0x112EB5FA", "الالتقاء بفرقة الدعم التكتيكي", "Mission 3: Nazran Cemetery — Objective", "objective"),
    ("0x125BF0D9", "تم الوصول إلى نقطة الحفظ (تشيك بوينت)", "HUD: Checkpoint reached", "hud"),
    ("0x138A7F41", "فشلت المهمة! لقد سقطت في المعركة", "HUD: Mission failed", "hud"),
    ("0x14C0385E", "اكتملت جميع أهداف المهمة بنجاح تكتيكي تام", "HUD: Mission accomplished", "hud"),
    ("0x15D90B38", "تسلل إلى مصنع الأسلحة عبر شبكة الأنابيب", "Mission 4: Nazran Foundry — Objective", "objective"),
    ("0x16F48A2C", "كيلر: لا مجال للتراجع الآن، تقدموا فوراً!", "Dialogue / Radio: Keller", "subtitle"),
    ("0x17E2B04D", "ماكالوم: نيران القناصة كثيفة، احتموا خلف القبور!", "Dialogue / Radio: MacCallum", "subtitle"),
    ("0x18B90F21", "سيلار: الهدف تحصن داخل المعتقل، اقتحموا البوابة!", "Dialogue / Radio: Cellar", "subtitle"),
    ("0x19A07E3F", "العمليات: تأكيد تصفية الهدف، تابعوا إلى نقطة الإخلاء", "Dialogue / Radio: HQ", "subtitle")
]

# Generate translations/en/gameplay_strings.json and translations/ar/gameplay_strings.json
os.makedirs('translations/en', exist_ok=True)
os.makedirs('translations/ar', exist_ok=True)

en_list = []
ar_list = []

for hex_id, ar_text, context, cat in translations_db:
    sid = int(hex_id, 16)
    en_text = string_by_id.get(sid, "")
    if not en_text:
        # Fallback if not directly matched by int
        for s in all_strings:
            if s['id'].upper() == hex_id.upper():
                en_text = s['english']
                break

    en_entry = {
        "id": hex_id.upper(),
        "id_int": sid,
        "text": en_text,
        "context": context,
        "category": cat,
        "status": "verified"
    }
    ar_entry = {
        "id": hex_id.upper(),
        "id_int": sid,
        "english": en_text,
        "arabic": ar_text,
        "context": context,
        "category": cat,
        "status": "verified"
    }
    en_list.append(en_entry)
    ar_list.append(ar_entry)

# Sort both by id_int for binary search in C++
ar_list.sort(key=lambda x: x['id_int'])
en_list.sort(key=lambda x: x['id_int'])

with open('translations/en/gameplay_strings.json', 'w', encoding='utf-8') as f:
    json.dump(en_list, f, ensure_ascii=False, indent=2)

with open('translations/ar/gameplay_strings.json', 'w', encoding='utf-8') as f:
    json.dump(ar_list, f, ensure_ascii=False, indent=2)

print(f"Wrote {len(en_list)} verified strings to translations/en/gameplay_strings.json")
print(f"Wrote {len(ar_list)} verified strings to translations/ar/gameplay_strings.json")

# Now generate src/game/strings_ara.inc (C++ sorted array)
def escape_c(s):
    return s.replace('\\', '\\\\').replace('"', '\\"').replace('\r', '').replace('\n', '\\n')

with open('src/game/strings_ara.inc', 'w', encoding='utf-8') as f:
    f.write("// generated: confirmed gameplay strings in Arabic by string ID, sorted for std::lower_bound\n")
    for item in ar_list:
        ar_escaped = escape_c(item['arabic'])
        en_escaped = escape_c(item['english'])
        ctx_escaped = escape_c(item['context'])
        f.write(f'{{{item["id"]}, "{en_escaped}", "{ar_escaped}", "{ctx_escaped}", "{item["category"]}"}},\n')

print("Wrote src/game/strings_ara.inc successfully!")
