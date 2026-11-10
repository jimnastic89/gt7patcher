import hashlib
import json
import re
import shutil
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
DIST = HERE / 'dist'
DIST.mkdir(exist_ok=True)
VERSION = '0.40.27-11-11_0834'
name = f'GT7Patcher-v{VERSION}'
elf = DIST / f'{name}.elf'
source_version = re.search(r'#define VERSION "([^"]+)"', (HERE / 'src/main.c').read_text())
if not source_version or source_version.group(1) != VERSION:
    raise SystemExit('Version mismatch between package.py and src/main.c')

source_roots = ('assets', 'data', 'patches', 'src', 'vendor', 'web')
source_files = (
    'Makefile', 'generate.py', 'build_cleaner.py', 'build_hook.py',
    'build_live_bridge.py', 'package.py', '.gitignore', 'README.md',
    'THIRD_PARTY_NOTICES.md', 'LICENSE'
)
excluded_sources = {'web/index.html.bak-hud', 'data/PPSA01317-original.xml'}
paths = [
    path for root in source_roots for path in (HERE / root).rglob('*')
    if path.is_file()
    and '__pycache__' not in path.parts
    and path.suffix not in {'.pyc', '.pyo'}
    and path.relative_to(HERE).as_posix() not in excluded_sources
]
paths.extend(HERE / source for source in source_files)
sources = {
    path.relative_to(HERE).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
    for path in sorted(paths)
}

shutil.copyfile(HERE / 'build/gt7patcher.elf', elf)
elf_bytes = elf.read_bytes()
if VERSION.encode() not in elf_bytes:
    raise SystemExit('Embedded payload version does not match package.py')

manifest = dict(
    version=VERSION,
    target=dict(title='PPSA01317', app_version='01.710.000', firmware='not checked at runtime'),
    elf=dict(file=elf.name, size=elf.stat().st_size, sha256=hashlib.sha256(elf_bytes).hexdigest()),
    verification=dict(
        ps5_build='passed',
        cleaner='Browser worker reconstruction and ZIP CRC/hash verification against all reference files passed',
        console_conversion_test='User confirmed the seven converted files plus three unchanged clean-reference files boot without the four optional files; vanilla and GT7Patcher offline play both tested',
        live_settings='Hidden from the release UI; bridge code retained for reactivation in a future build',
        release_payload_console_test='Not deployed or tested on console in this task',
    ),
    region='display presets only; full internal region conversion is not implemented',
    engine=json.loads((HERE / 'vendor/provenance.json').read_text()),
    assets=json.loads((HERE / 'build/assets.json').read_text()),
    source_sha256=sources,
)
manifest['bridge'] = json.loads((HERE / 'build/hook.json').read_text())
(DIST / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
shutil.copyfile(HERE / 'README.md', DIST / 'README.md')
with zipfile.ZipFile(DIST / f'{name}.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
    for path in (elf, DIST / 'manifest.json', DIST / 'README.md'):
        archive.write(path, path.name)
    for relative in sources:
        archive.write(HERE / relative, f'source/{relative}')

print(json.dumps({
    'elf': str(elf),
    'size': elf.stat().st_size,
    'sha256': manifest['elf']['sha256'],
    'zip': str(DIST / f'{name}.zip'),
}, indent=2))
