"""Package relocation-free bootstrap and bridge, with checked mailbox ABI."""
from pathlib import Path
import json
import struct
import subprocess
import sys

out=[]
metadata={}
for name,required,limit in [('live_boot',['live_boot_entry','live_boot_control'],0xf0),
                            ('live_bridge',['live_frame','live_mailbox','ime_open_hook','live_trace','live_menu_close'],0x14000)]:
    elf=Path(f'build/{name}.elf').read_bytes()
    offset=struct.unpack_from('<Q',elf,40)[0]
    stride,count=struct.unpack_from('<HH',elf,58)
    for i in range(count):
        section=struct.unpack_from('<IIQQQQIIQQ',elf,offset+i*stride)
        assert section[1] not in (4,9) or not section[5], 'runtime relocation'
    # Position-independence guard. The blob is linked at VMA 0 but mapped at a
    # high runtime address, so every symbol reference must be PC-relative. The
    # PS5 toolchain marks globals GLOBAL DEFAULT even under -fvisibility=hidden,
    # forcing a GOT reference; the linker's non-PIE relaxation then rewrites
    # `add sym@GOTPCREL(%rip),%reg` into an absolute link-time address, which is
    # garbage once the blob is loaded elsewhere. This caused a load-time
    # SIGSEGV in hud_walk (fault address 0x1ba1 = live_mailbox VMA + offset).
    # Only R_X86_64_PC32 (2) and R_X86_64_PLT32 (4) may appear in the object.
    obj=Path(f'build/{name}.o').read_bytes()
    obj_offset=struct.unpack_from('<Q',obj,40)[0]
    obj_stride,obj_count=struct.unpack_from('<HH',obj,58)
    for i in range(obj_count):
        section=struct.unpack_from('<IIQQQQIIQQ',obj,obj_offset+i*obj_stride)
        if section[1] not in (4,9) or not section[5]:
            continue
        entry=24 if section[1]==4 else 16
        for j in range(section[5]//entry):
            kind=struct.unpack_from('<Q',obj,section[4]+j*entry+8)[0]&0xffffffff
            assert kind in (2,4),f'{name}: relocation type {kind} is not PC-relative'
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
