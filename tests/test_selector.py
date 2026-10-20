"""Host integration and generated-code tests. Never connects to a console."""
import itertools
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

HERE = Path(__file__).resolve().parents[1]
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / 'diagnostics'))
from verify_live_patch import self_segments, local_bytes
sys.path.insert(0, str(ROOT / 'diagnostics/python-libs'))
sys.path.insert(0, str(HERE / 'build/test-libs'))
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
sys.path.insert(0, str(HERE))

class SelectorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.clean = self_segments(ROOT / 'GRAN_TURISMO_7_Clean_Backport/eboot.bin')
        cls.original = ET.parse(ROOT / 'gt7-offline-v1.0/PPSA01317.xml').getroot().findall('Metadata')
        cls.md = Cs(CS_ARCH_X86, CS_MODE_64)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.state = Path(self.temp.name)
        self.launch()

    def launch(self):
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0))
            port = s.getsockname()[1]
        self.url = f'http://127.0.0.1:{port}'
        self.process = subprocess.Popen([str(HERE / 'build/gt7patcher-host'), str(self.state), str(port)], stdout=subprocess.DEVNULL)
        for _ in range(60):
            try:
                self.status = self.request('/api/status')[1]
                break
            except OSError:
                time.sleep(.05)
        else:
            raise RuntimeError('Host server did not start')
        self.c = self.status['config'].copy()

    def tearDown(self):
        self.process.terminate()
        self.process.wait(timeout=8)
        self.temp.cleanup()

    def request(self, path, data=None, token=True):
        headers = {}
        if data is not None and token:
            headers['X-GT7-Token'] = self.status['token']
        if isinstance(data, dict):
            data = urllib.parse.urlencode(data).encode()
        req = urllib.request.Request(self.url + path, data=data, headers=headers)
        try:
            r = urllib.request.urlopen(req, timeout=6)
        except urllib.error.HTTPError as e:
            r = e
        body = r.read()
        if 'application/json' in r.headers.get('Content-Type', ''):
            body = json.loads(body)
        return r.status, body

    def xml(self):
        return ET.parse(self.state / 'xml_prospero/PPSA01317.xml').getroot()

    def lines(self):
        return self.xml().findall('./Metadata/PatchList/Line')

    def test_original_options_exact_and_ordered(self):
        # Includes overlaps: Quick Store Rotation must follow the base offline lines.
        for mask in range(32):
            c = dict(self.c, mask=mask, units=0, language=-1)
            valid = not (mask & 4) or bool(mask & 1)
            code, _ = self.request('/api/config', c)
            self.assertEqual(code, 200 if valid else 400)
            if valid:
                expected = [e.attrib for i,m in enumerate(self.original) if mask & (1<<i) for e in m.findall('./PatchList/Line')]
                self.assertEqual([e.attrib for e in self.lines()], expected)

    def test_all_unit_combinations_decode_to_selected_targets(self):
        for speed,mass,power,torque,temp in itertools.product(range(2),range(2),range(3),range(3),(1,2)):
            c = dict(self.c, mask=0, language=-1, speed=speed, mass=mass, power=power, torque=torque, temperature=temp)
            self.assertEqual(self.request('/api/config',c)[0],200)
            rows=self.lines()
            self.assertEqual(len(rows),5)
            targets=[0x611aecc+i*4 for i in range(7)]
            values=[speed,power,torque,3,temp,0,mass]
            for row,ts,vs in [(rows[0],targets,values)]+[(rows[i+1],[t],[v]) for i,(t,v) in enumerate(zip([0x611aed0,0x611aedc,0x611aed4,0x611aecc],[power,temp,torque,speed]))]:
                a=int(row.get('Address'),16);b=bytes.fromhex(row.get('Value'))
                instructions=list(self.md.disasm(b,a))
                self.assertEqual(instructions[-1].mnemonic,'ret')
                self.assertEqual(len(instructions),len(ts)+1)
                for i,(target,value) in enumerate(zip(ts,vs)):
                    self.assertEqual(b[i*10:i*10+2],b'\xc7\x05')
                    displacement,v=struct.unpack_from('<iI',b,i*10+2)
                    self.assertEqual(a+i*10+10+displacement,target)
                    self.assertEqual(v,value)
                self.assertIsNotNone(local_bytes(self.clean,a,len(b)))

    def test_australian_output_matches_console_tested_patch(self):
        expected=ET.parse(ROOT/'gt7-australian-units-v1.0/PPSA01317.xml').getroot().findall('Metadata')[-2:]
        c=dict(self.c,mask=0)
        self.assertEqual(self.request('/api/config',c)[0],200)
        self.assertEqual([r.attrib for r in self.lines()],[r.attrib for m in expected for r in m.findall('./PatchList/Line')])

    def test_every_native_language_and_system_option(self):
        for lang in range(-1,27):
            self.assertEqual(self.request('/api/config',dict(self.c,mask=0,units=0,language=lang))[0],200)
            rows=self.lines()
            self.assertEqual(len(rows),0 if lang==-1 else 2)
            if lang>=0:
                self.assertEqual(bytes.fromhex(rows[0].get('Value')),b'\xbf'+struct.pack('<I',lang))
                self.assertEqual(rows[1].get('Value'),'eb03')
        self.assertEqual(local_bytes(self.clean,0xc21e93,7).hex(),'89df0fa3d97203')

    def test_bad_requests_never_replace_xml(self):
        path=self.state/'xml_prospero/PPSA01317.xml'
        before=path.read_bytes()
        for key,value in [('mask',32),('language',27),('power',3),('mass',-1),('preset',6),('temperature',0),('autostart',2),('autostart',-1)]:
            self.assertEqual(self.request('/api/config',dict(self.c,**{key:value}))[0],400)
            self.assertEqual(path.read_bytes(),before)
        self.assertEqual(self.request('/api/config',self.c,token=False)[0],403)
        self.assertEqual(self.request('/api/config',b'mask=1&mask=2')[0],400)
        self.assertEqual(self.request('/api/config',b'')[0],400)
        self.assertEqual(path.read_bytes(),before)
        self.assertEqual(self.request('/api/start',b'')[0],409) # host cannot load engines

    def test_persistence_is_same_atomic_snapshot(self):
        c=dict(self.c,mask=19,preset=0,language=3,power=1)
        self.assertEqual(self.request('/api/config',c)[0],200)
        before=(self.state/'xml_prospero/PPSA01317.xml').read_bytes()
        self.process.terminate();self.process.wait(timeout=8)
        self.launch()
        self.assertEqual(self.status['config'],c)
        self.assertEqual((self.state/'xml_prospero/PPSA01317.xml').read_bytes(),before)
        self.assertFalse((self.state/'xml_prospero/PPSA01317.xml.tmp').exists())

    def test_gt7_version_identity_and_minimal_state(self):
        m=self.xml().find('Metadata')
        self.assertFalse((self.state/'xml_prospero/!_settings.xml').exists())
        self.assertFalse((self.state/'xml_orbis').exists())
        self.assertFalse((self.state/'xml_sys').exists())
        self.assertEqual(m.get('AppVer'),'01.710.000')
        self.assertEqual(m.get('ImageBase'),'0x00400000')
        self.assertEqual([e.text for e in self.xml().findall('./TitleID/ID')],['PPSA01317'])

    def test_autostart_persists_without_changing_patch_bytes(self):
        before=[row.attrib for row in self.lines()]
        c=dict(self.c,autostart=1)
        self.assertEqual(self.request('/api/config',c)[0],200)
        self.assertEqual([row.attrib for row in self.lines()],before)
        self.process.terminate();self.process.wait(timeout=8);self.launch()
        self.assertEqual(self.status['config']['autostart'],1)
        self.assertFalse(self.status['engineRunning']) # host must never start a console engine

    def test_legacy_snapshot_migrates_with_autostart_off(self):
        self.process.terminate();self.process.wait(timeout=8)
        path=self.state/'xml_prospero/PPSA01317.xml'
        text=path.read_text().replace(' 1 0 -->',' 1 -->')
        path.write_text(text)
        self.launch()
        self.assertEqual(self.status['config']['autostart'],0)

    def test_html_and_mutating_get_are_safe(self):
        code,body=self.request('/')
        self.assertEqual(code,200)
        self.assertIn(b'Game Settings',body)
        self.assertIn(b'internal GameRegionCode',body)
        self.assertIn(b'id="version"',body)
        self.assertIn(b'Reinstall home screen shortcut',body)
        self.assertIn(b'Stop engine',body)
        self.assertIn(b'Stop payload',body)
        before=(self.state/'xml_prospero/PPSA01317.xml').read_bytes()
        self.assertEqual(self.request('/api/start')[0],404)
        self.assertEqual(self.request('/api/config?mask=31')[0],404)
        self.assertEqual(self.request('/api/engine/stop')[0],404)
        self.assertEqual(self.request('/api/shortcut/reinstall',b'',token=False)[0],403)
        self.assertEqual(self.request('/api/engine/stop',b'')[0],200)
        self.assertEqual(self.request('/api/shortcut/reinstall',b'')[0],409)
        self.assertEqual((self.state/'xml_prospero/PPSA01317.xml').read_bytes(),before)

if __name__=='__main__':
    unittest.main(verbosity=2)
