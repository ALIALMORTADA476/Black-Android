with open('src/common/translations.cpp', 'r', encoding='utf-8') as f:
    text = f.read()

import re
entries = {}
# Matches: {"KEY", ...}
for m in re.finditer(r'\{\s*"([^"]+)"\s*,\s*\{([^}]+(?:\{[^}]*\}[^}]*)*)\}\}', text):
    key = m.group(1).upper()
    body = m.group(2)
    entries[key] = body

print(f"Properly parsed entries: {len(entries)}")

with open('src/common/menu.cpp', 'r', encoding='utf-8') as f:
    menu = f.read()

all_menu = set()
for m in re.finditer(r'(?:Section|OptionRow|PlannedRow|Ui)\s*\(\s*"([^"]+)"', menu):
    all_menu.add(m.group(1))

for m in re.finditer(r'\{\s*Kind::[a-zA-Z]+,\s*[^,]+,\s*"([^"]+)"', menu):
    all_menu.add(m.group(1))

for m in re.finditer(r'TacticalCategory::[a-zA-Z]+,\s*"([^"]+)"', menu):
    all_menu.add(m.group(1))

missing = sorted([s for s in all_menu if s.upper() not in entries])
print(f"Menu strings: {len(all_menu)}")
print(f"Missing count: {len(missing)}")
for s in missing:
    print(f'MISSING: "{s}"')

