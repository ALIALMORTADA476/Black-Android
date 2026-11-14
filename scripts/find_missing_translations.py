import re

with open('src/common/menu.cpp', 'r', encoding='utf-8') as f:
    menu = f.read()

with open('src/common/translations.cpp', 'r', encoding='utf-8') as f:
    trans = f.read()

# Extract keys in kEntries
trans_keys = set(re.findall(r'\{\s*"([^"]+)"', trans))

# Extract all string literals passed to Section, OptionRow, PlannedRow, Ui
menu_strings = set()
for m in re.finditer(r'(?:Section|OptionRow|PlannedRow|Ui)\s*\(\s*"([^"]+)"', menu):
    menu_strings.add(m.group(1))

# Row labels
for m in re.finditer(r'\{\s*Kind::[a-zA-Z]+,\s*[^,]+,\s*"([^"]+)"', menu):
    menu_strings.add(m.group(1))

# Category names
for m in re.finditer(r'TacticalCategory::[a-zA-Z]+,\s*"([^"]+)"', menu):
    menu_strings.add(m.group(1))

# Preset names
for s in ["ORIGINAL", "ENHANCED", "PERFORMANCE", "CUSTOM", "ORIGINAL (D3D)", "REINHARD HDR", "ACES FILMIC",
          "STANDARD (0.0)", "SHARP (-0.5)", "ULTRA SHARP (-1.0)", "FULL QUALITY", "HALF DISTANCE", "AGGRESSIVE",
          "TACTICAL GOLD", "SPECIAL OPS CRIMSON", "MILITARY OLIVE", "RESTORE DEFAULTS", "CLOSE CONSOLE"]:
    menu_strings.add(s)

missing = sorted([s for s in menu_strings if s not in trans_keys])
print(f"Total checked: {len(menu_strings)}")
print(f"Missing count: {len(missing)}")
for s in missing:
    print(f'MISSING: "{s}"')

