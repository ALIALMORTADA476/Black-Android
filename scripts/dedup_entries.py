import re

with open('src/common/translations.cpp', 'r', encoding='utf-8') as f:
    orig_trans = f.read()

orig_keys = set(re.findall(r'\{\s*"([^"]+)"', orig_trans))

with open('scripts/generated_entries.txt', 'r', encoding='utf-8') as f:
    gen_lines = [l.strip() for l in f.readlines() if l.strip()]

deduped = []
seen = set()
for line in gen_lines:
    m = re.match(r'\{\s*"([^"]+)"', line)
    if m:
        key = m.group(1)
        if key not in orig_keys and key not in seen:
            deduped.append(line)
            seen.add(key)
        else:
            print(f"Skipping duplicate: {key}")

with open('scripts/deduped_entries.txt', 'w', encoding='utf-8') as f:
    f.write('\n'.join(deduped) + '\n')

print(f"Final deduped new entries to add: {len(deduped)}")

