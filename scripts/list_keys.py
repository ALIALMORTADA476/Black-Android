with open('src/common/translations.cpp', 'r', encoding='utf-8') as f:
    lines = f.readlines()

keys = []
for line in lines:
    line = line.strip()
    if line.startswith('{"'):
        q2 = line.find('"', 2)
        if q2 != -1:
            k = line[2:q2]
            keys.append(k)

print(f"Total entries: {len(keys)}")
for k in keys[190:]:
    print(k)

