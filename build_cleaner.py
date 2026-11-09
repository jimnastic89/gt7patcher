"""Generate a small reverse-patch manifest; never embed complete game files."""
import argparse
import hashlib
import json
from pathlib import Path

OPTIONAL = {'k9.psp', 'fakelib/libSceAgc.sprx', 'fakelib/libSceAgcDriver.sprx', 'fakelib/libScePsml.sprx'}


def build(reference, old_dirs, base=None):
    base_files = {}
    if base is not None:
        base_files = {item['path']: item for item in json.loads(base.read_text())['files']}
    files = []
    for path in sorted(reference.rglob('*')):
        if not path.is_file():
            continue
        # pathlib keeps Windows separators in relative paths even when the
        # manifest format is explicitly POSIX-style.
        rel = '/'.join(path.relative_to(reference).parts)
        clean = path.read_bytes()
        entry = dict(path=rel, required=rel not in OPTIONAL, size=len(clean),
                     sha256=hashlib.sha256(clean).hexdigest(), inputs=[])
        base_entry = base_files.get(rel, {})
        old_entries = list(base_entry.get('inputs', []))
        if 'unchanged' in base_entry:
            entry.update(required=base_entry.get('required', False), unchanged=True)
        unchanged = False
        for old in old_dirs:
            source = old.joinpath(*path.relative_to(reference).parts)
            if not source.is_file():
                continue
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
                recipe = dict(size=len(before), sha256=hashlib.sha256(before).hexdigest(), shift=48, records=records)
                if not any(item['size'] == recipe['size'] and item['sha256'] == recipe['sha256'] for item in old_entries):
                    old_entries.append(recipe)
                entry.pop('unchanged', None)
            else:
                unchanged = True
        entry['inputs'] = old_entries
        if unchanged:
            entry.update(required=False, unchanged=True)
        files.append(entry)
    assert len(files) == 14 and sum(f['required'] for f in files) == 7
    return dict(version=1, titleId='PPSA01317', appVersion='01.710.000', files=files)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--old', type=Path, action='append', required=True,
                        help='Older patched-file directory; may be supplied more than once')
    parser.add_argument('--base', type=Path,
                        help='Existing manifest whose input recipes should be retained')
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parent/'data/cleaner.json')
    args = parser.parse_args()
    args.output.write_text(json.dumps(build(args.reference, args.old, args.base), separators=(',', ':'))+'\n')
