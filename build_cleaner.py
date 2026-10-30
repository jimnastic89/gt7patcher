"""Generate a small reverse-patch manifest; never embed complete game files."""
import argparse
import hashlib
import json
from pathlib import Path

OPTIONAL = {'k9.psp', 'fakelib/libSceAgc.sprx', 'fakelib/libSceAgcDriver.sprx', 'fakelib/libScePsml.sprx'}


def build(reference, old):
    files = []
    for path in sorted(reference.rglob('*')):
        if not path.is_file():
            continue
        rel = path.relative_to(reference).as_posix()
        clean = path.read_bytes()
        entry = dict(path=rel, required=rel not in OPTIONAL, size=len(clean),
                     sha256=hashlib.sha256(clean).hexdigest(), inputs=[])
        source = old / rel
        if source.is_file():
            before = source.read_bytes()
            if before != clean:
                shifted = (bytes(48) + before)[:len(clean)].ljust(len(clean), b'\0')
                records = []
                start = None
                for i, (a, b) in enumerate(zip(shifted, clean)):
                    if a != b and start is None:
                        start = i
                    elif a == b and start is not None:
                        records.append([start, clean[start:i].hex()])
                        start = None
                if start is not None:
                    records.append([start, clean[start:].hex()])
                restored = bytearray(shifted)
                for offset, hexdata in records:
                    restored[offset:offset+len(hexdata)//2] = bytes.fromhex(hexdata)
                assert restored == clean
                entry['inputs'].append(dict(size=len(before), sha256=hashlib.sha256(before).hexdigest(), shift=48, records=records))
            else:
                entry.update(required=False, unchanged=True)
        files.append(entry)
    assert len(files) == 14 and sum(f['required'] for f in files) == 7
    return dict(version=1, titleId='PPSA01317', appVersion='01.710.000', files=files)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--old', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parent/'data/cleaner.json')
    args = parser.parse_args()
    args.output.write_text(json.dumps(build(args.reference, args.old), separators=(',', ':'))+'\n')
