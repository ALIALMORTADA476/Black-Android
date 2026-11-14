import json

data = json.load(open('scripts/all_strings.json', 'r', encoding='utf-8'))

keywords = [
    'objective', 'destroy', 'checkpoint', 'failed', 'completed', 'intel:',
    'hold', 'press', 'lennox', 'valencio', 'survive', 'trench', 'bridge',
    'foundry', 'asylum', 'cemetery', 'gulag', 'veblensk', 'headshot', 'silencer',
    'grenade', 'sniper', 'bunker', 'cellar', 'gate', 'door', 'target', 'cover'
]

matches = []
for s in data:
    txt = s['english'].lower()
    for kw in keywords:
        if kw in txt:
            matches.append(s)
            break

print(f'Found {len(matches)} key gameplay & objective strings')
for s in matches[:35]:
    print(f"{s['id']}: \"{s['english']}\"")

