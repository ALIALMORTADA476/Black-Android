with open('src/common/translations.cpp', 'r', encoding='utf-8') as f:
    lines = f.readlines()

in_table = False
entries = {}
for line in lines:
    if 'constexpr Entry kEntries[]' in line:
        in_table = True
        continue
    if in_table and '};' in line:
        in_table = False
        break
    if in_table:
        line = line.strip()
        if line.startswith('{"'):
            # extract string
            q1 = line.find('"', 1)
            if q1 != -1:
                key = line[2:q1]
                # find Arabic part (u"...")
                u_pos = line.rfind('u"')
                if u_pos != -1:
                    u_end = line.find('"', u_pos + 2)
                    ar = line[u_pos+2:u_end]
                    entries[key.upper()] = ar

print(f"Total entries loaded: {len(entries)}")

# Check missing from menu
with open('scripts/find_missing_translations.py', 'r', encoding='utf-8') as f:
    pass

with open('src/common/menu.cpp', 'r', encoding='utf-8') as f:
    menu = f.read()

import re
all_menu = set()
for m in re.finditer(r'(?:Section|OptionRow|PlannedRow|Ui)\s*\(\s*"([^"]+)"', menu):
    all_menu.add(m.group(1))

for m in re.finditer(r'\{\s*Kind::[a-zA-Z]+,\s*[^,]+,\s*"([^"]+)"', menu):
    all_menu.add(m.group(1))

for m in re.finditer(r'TacticalCategory::[a-zA-Z]+,\s*"([^"]+)"', menu):
    all_menu.add(m.group(1))

presets = ["ORIGINAL", "ENHANCED", "PERFORMANCE", "CUSTOM", "ORIGINAL (D3D)", "REINHARD HDR", "ACES FILMIC",
           "STANDARD (0.0)", "SHARP (-0.5)", "ULTRA SHARP (-1.0)", "FULL QUALITY", "HALF DISTANCE", "AGGRESSIVE",
           "TACTICAL GOLD", "SPECIAL OPS CRIMSON", "MILITARY OLIVE", "RESTORE DEFAULTS", "CLOSE CONSOLE",
           "1x (DEFAULT)", "2x (HIGH)", "4x (ULTRA)", "0.0 (STANDARD)", "-0.5 (SHARP)", "-1.0 (ULTRA SHARP)",
           "BLACK ENHANCED EDITION — TACTICAL OPS CONSOLE", "TACTICAL PROFILE:",
           "[ORIGINAL 2006 XBOX BASELINE]", "[MODERN ENHANCED FIDELITY]", "[BATTERY & 60FPS OPTIMIZED]", "[CUSTOM USER TUNING]"]
for p in presets:
    all_menu.add(p)

missing = sorted([s for s in all_menu if s.upper() not in entries])
print(f"Total menu strings: {len(all_menu)}")
print(f"Missing count: {len(missing)}")
for s in missing:
    print(f'"{s}"')

