"""Turn the linked, relocation-free bridge into bytes and verified offsets."""
import json
import subprocess
import sys
from pathlib import Path
import struct
b = Path('build/hook.elf').read_bytes()
phoff = struct.unpack_from('<Q', b, 32)[0]
shoff = struct.unpack_from('<Q', b, 40)[0]
shsize, shnum = struct.unpack_from('<HH', b, 58)
for i in range(shnum):
    section = struct.unpack_from('<IIQQQQIIQQ', b, shoff+i*shsize)
    assert section[1] not in (4, 9) or section[5] == 0, 'bridge has relocations'
symbols = {}
for line in subprocess.check_output([sys.argv[1], '-n', 'build/hook.elf'], text=True).splitlines():
    fields = line.split()
    if len(fields) == 3:
        symbols[fields[2]] = int(fields[0], 16)
blob = Path('build/hook.bin').read_bytes()
assert 0 < len(blob) <= 4096
assert all(symbols[name] < len(blob) for name in ['launch_hook', 'icon_hook', 'imports'])
Path('build/hook_blob.h').write_text(
    f'#define HOOK_LAUNCH {symbols["launch_hook"]}\n#define HOOK_ICON {symbols["icon_hook"]}\n#define HOOK_IMPORTS {symbols["imports"]}\n'
    + 'static const unsigned char hook_blob[]={' + ','.join(map(str,blob)) + '};\n')
Path('build/hook.json').write_text(json.dumps(dict(size=len(blob),symbols=symbols),indent=2)+'\n')
