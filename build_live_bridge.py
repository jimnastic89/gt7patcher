"""Package relocation-free bootstrap and bridge, with checked mailbox ABI."""
from pathlib import Path
import json
import struct
import subprocess
import sys

out=[]
metadata={}
for name,required,limit in [('live_boot',['live_boot_entry','live_boot_control'],0xf0),
                            ('live_bridge',['live_frame','live_mailbox','ime_open_hook','credit_ime_handler'],0x10000)]:
    elf=Path(f'build/{name}.elf').read_bytes()
    offset=struct.unpack_from('<Q',elf,40)[0]
    stride,count=struct.unpack_from('<HH',elf,58)
    for i in range(count):
        section=struct.unpack_from('<IIQQQQIIQQ',elf,offset+i*stride)
        assert section[1] not in (4,9) or not section[5], 'runtime relocation'
    symbols={}
    for line in subprocess.check_output([sys.argv[1],'-n',f'build/{name}.elf'],text=True).splitlines():
        fields=line.split()
        if len(fields)==3: symbols[fields[2]]=int(fields[0],16)
    blob=Path(f'build/{name}.bin').read_bytes()
    assert 0<len(blob)<=limit,(name,len(blob))
    for symbol in required:
        assert symbols[symbol]<len(blob)
        out.append(f'#define {symbol.upper()} {symbols[symbol]}')
    out.append(f'static const unsigned char {name}_blob[]={{'+','.join(map(str,blob))+'};')
    metadata[name]=dict(size=len(blob),symbols={s:symbols[s] for s in required})
Path('build/live_blob.h').write_text('\n'.join(out)+'\n')
Path('build/live_bridge.json').write_text(json.dumps(metadata,indent=2)+'\n')
