import re

with open('src/common/translations.cpp', 'r', encoding='utf-8') as f:
    text = f.read()

cats = ['GRAPHICS', 'COMBAT', 'CHARACTERS', 'AUDIO', 'LOCALIZATION', 'GAMEPLAY', 'INTERFACE', 'CHEATS & WEAPONS']
for cat in cats:
    pattern = r'\{\s*"' + re.escape(cat) + r'"\s*,\s*\{([^}]+)\}\}'
    m = re.search(pattern, text)
    if m:
        entry = m.group(1).strip()
        parts = entry.split(',')
        ar = parts[5].strip() if len(parts) > 5 else '?'
        print(f"{cat}: {ar}")
    else:
        print(f"{cat}: NOT FOUND")

