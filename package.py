"""Package the built payload with source, provenance and documentation."""
from pathlib import Path
import hashlib
import json
import shutil
import zipfile

HERE=Path(__file__).resolve().parent
DIST=HERE/'dist'
DIST.mkdir(exist_ok=True)
VERSION='0.3.1'
name=f'GT7Patcher-v{VERSION}'
elf=DIST/f'{name}.elf'
shutil.copyfile(HERE/'build/gt7patcher.elf',elf)
source_roots=('assets','data','patches','src','tests','vendor','web')
source_files=('Makefile','generate.py','build_cleaner.py','build_hook.py','build_live_bridge.py','package.py','.gitignore',
              'README.md','THIRD_PARTY_NOTICES.md','LICENSE')
paths=[p for root in source_roots for p in (HERE/root).rglob('*')
       if p.is_file() and '__pycache__' not in p.parts and p.suffix not in {'.pyc','.pyo'}]
paths.extend(HERE/name for name in source_files)
sources={p.relative_to(HERE).as_posix():hashlib.sha256(p.read_bytes()).hexdigest()
         for p in sorted(paths)}
manifest=dict(version=VERSION,target=dict(title='PPSA01317',app_version='01.710.000',firmware='not checked at runtime'),
    elf=dict(file=elf.name,size=elf.stat().st_size,sha256=hashlib.sha256(elf.read_bytes()).hexdigest()),
    verification=dict(ps5_build='passed',
                      cleaner='Browser worker reconstruction and ZIP CRC/hash verification against all reference files passed',
                      console_conversion_test='User confirmed the seven converted files plus three unchanged clean-reference files boot without the four optional files; vanilla and GT7Patcher offline play both tested',
                      live_settings='Enabled in this build; continue console validation before release',
                      release_payload_console_test='Not deployed or tested on console in this task'),
    region='display presets only; full internal region conversion is not implemented',
    engine=json.loads((HERE/'vendor/provenance.json').read_text()),
    assets=json.loads((HERE/'build/assets.json').read_text()),source_sha256=sources)
manifest['bridge']=json.loads((HERE/'build/hook.json').read_text())
(DIST/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
shutil.copyfile(HERE/'README.md',DIST/'README.md')
with zipfile.ZipFile(DIST/f'{name}.zip','w',zipfile.ZIP_DEFLATED) as z:
    for p in [elf,DIST/'manifest.json',DIST/'README.md']:z.write(p,p.name)
    for relative in sources:z.write(HERE/relative,'source/'+relative)
print(json.dumps(dict(elf=str(elf),size=elf.stat().st_size,sha256=manifest['elf']['sha256'],zip=str(DIST/f'{name}.zip')),indent=2))
