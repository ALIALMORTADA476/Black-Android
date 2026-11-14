import struct
import json

with open('assets/black.iso', 'rb') as f:
    f.seek(4735 * 2048)
    data = f.read(52858)

count = struct.unpack_from('<I', data, 8)[0]
strings = []
for i in range(count):
    off = struct.unpack_from('<I', data, 16 + i * 4)[0]
    sid, = struct.unpack_from('<I', data, off)
    end = data.find(b'\x00', off + 4)
    text = data[off+4:end].decode('latin-1', errors='replace')
    strings.append({'id': f'0x{sid:08X}', 'id_int': sid, 'english': text})

print(f'Total extracted strings: {len(strings)}')

# Load indonesian strings for reference
ind_map = {}
with open('src/game/strings_ind.inc', 'r', encoding='latin-1') as f:
    for line in f:
        line = line.strip()
        if line.startswith('{0x'):
            parts = line.split(',', 1)
            sid_str = parts[0].strip('{').strip()
            val = parts[1].strip().rstrip('},').strip('"')
            ind_map[sid_str] = val

print(f'Loaded {len(ind_map)} Indonesian strings')

# Save full inventory
with open('scripts/all_strings.json', 'w', encoding='utf-8') as f:
    json.dump(strings, f, ensure_ascii=False, indent=2)

print('Saved scripts/all_strings.json')

