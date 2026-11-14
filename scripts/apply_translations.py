with open('src/common/translations.cpp', 'r', encoding='utf-8') as f:
    text = f.read()

import re
existing_keys = set()
for line in text.splitlines():
    line = line.strip()
    if line.startswith('{"'):
        q2 = line.find('"', 2)
        if q2 != -1:
            existing_keys.add(line[2:q2].upper())

with open('scripts/generated_entries.txt', 'r', encoding='utf-8') as f:
    gen_lines = [l.strip() for l in f.readlines() if l.strip()]

to_add = []
for line in gen_lines:
    q2 = line.find('"', 6)
    k = line[6:q2]
    if k.upper() not in existing_keys:
        to_add.append("    " + line)
        existing_keys.add(k.upper())
    else:
        print(f"Already in translations: {k}")

print(f"Entries to append: {len(to_add)}")

# Find end of kEntries table
end_marker = '};\n\nbool Same('
if end_marker not in text:
    end_marker = '};\r\n\r\nbool Same('

if end_marker in text:
    addition = '\n'.join(to_add) + '\n'
    new_text = text.replace(end_marker, addition + end_marker)
    with open('src/common/translations.cpp', 'w', encoding='utf-8') as f:
        f.write(new_text)
    print("Successfully appended new entries to src/common/translations.cpp!")
else:
    print("Could not find end of table!")

