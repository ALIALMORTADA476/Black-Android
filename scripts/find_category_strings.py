import re

with open('src/common/menu.cpp', 'r', encoding='utf-8') as f:
    lines = f.readlines()

in_categories = False
cat_strings = set()
for line in lines:
    if 'void DrawCategory' in line:
        in_categories = True
    if 'void DrawPanel' in line:
        in_categories = False
    if in_categories:
        # find string literals
        for s in re.findall(r'"([^"]+)"', line):
            s = s.strip()
            if len(s) > 1 and not s.startswith('##') and not s.startswith('%') and s not in ['<', '>', ' ', '<##dec', '>##inc']:
                cat_strings.add(s)

with open('src/common/translations.cpp', 'r', encoding='utf-8') as f:
    trans = f.read()

trans_keys = set(re.findall(r'\{\s*"([^"]+)"', trans))

missing = sorted([s for s in cat_strings if s not in trans_keys])
print(f"Total in categories: {len(cat_strings)}")
print(f"Missing in categories: {len(missing)}")
for s in missing:
    print(f'"{s}",')

