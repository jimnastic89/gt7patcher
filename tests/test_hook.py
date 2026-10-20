"""Execute the actual PS5 bridge bytes with mock imports on x86-64 Linux.

This verifies the bridge ABI and title filtering without touching a console.
"""
import ctypes as C
import json
import mmap
from pathlib import Path
import struct
import unittest

ROOT=Path(__file__).resolve().parents[1]
INT=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_void_p,C.c_void_p)
APP=C.CFUNCTYPE(C.c_int,C.c_int,C.c_void_p)
SOCK=C.CFUNCTYPE(C.c_int,C.c_int,C.c_int,C.c_int)
CONNECT=C.CFUNCTYPE(C.c_int,C.c_int,C.c_void_p,C.c_uint)
OPT=C.CFUNCTYPE(C.c_int,C.c_int,C.c_int,C.c_int,C.c_void_p,C.c_uint)
IO=C.CFUNCTYPE(C.c_long,C.c_int,C.c_void_p,C.c_size_t,C.c_int)
CLOSE=C.CFUNCTYPE(C.c_int,C.c_int)
INSTALL=C.CFUNCTYPE(C.c_int,C.c_char_p,C.c_char_p,C.c_int,C.c_int)

class HookTests(unittest.TestCase):
    def setUp(self):
        self.title=b'PPSA01317';self.events=[];self.sent=bytearray();self.connect_result=0;self.app_result=0
        self.short_io=False;self.socket_result=20
        def original(a,b,c):self.events.append('original');return 123
        def app(pid,buf):
            self.events.append(('app',pid))
            C.memset(buf,0,88);C.memmove(buf+16,self.title,len(self.title));return self.app_result
        def socket(a,b,c):self.events.append('socket');return self.socket_result
        def connect(fd,addr,n):
            self.events.append(('connect',C.string_at(addr,n)));return self.connect_result
        def opt(fd,level,key,val,n):self.events.append(('timeout',C.string_at(val,n)));return 0
        def send(fd,p,n,flags):
            count=min(n,1) if self.short_io else n
            self.sent.extend(C.string_at(p,count));return count
        def recv(fd,p,n,flags):self.events.append('ack');C.memset(p,1,n);return n
        def close(fd):self.events.append(('close',fd));return 0
        def install(title,path,a,b):self.events.append(('install',title,path,a,b));return 17
        self.callbacks=[INT(original),APP(app),SOCK(socket),CONNECT(connect),OPT(opt),IO(send),IO(recv),CLOSE(close),INSTALL(install)]
        self.meta=json.loads((ROOT/'build/hook.json').read_text())
        self.mem=mmap.mmap(-1,4096,prot=mmap.PROT_READ|mmap.PROT_WRITE|mmap.PROT_EXEC)
        self.mem.write((ROOT/'build/hook.bin').read_bytes())
        self.mem.seek(self.meta['symbols']['imports'])
        self.mem.write(b''.join(struct.pack('<Q',C.cast(fn,C.c_void_p).value) for fn in self.callbacks))
        self.base=C.addressof(C.c_char.from_buffer(self.mem))
        self.hook=INT(self.base+self.meta['symbols']['launch_hook'])

    def tearDown(self):self.mem.close()

    def call(self):
        pid=C.c_int(4321)
        self.assertEqual(self.hook(None,None,C.byref(pid)),123)
        self.assertEqual(self.events[0],'original')

    def test_gt7_launch_sends_pid_to_loopback_and_waits_for_ack(self):
        self.call();self.assertEqual(self.sent,struct.pack('<i',4321))
        sockaddr=next(e[1] for e in self.events if isinstance(e,tuple) and e[0]=='connect')
        self.assertEqual(sockaddr[:8],b'\x10\x02'+struct.pack('>H',7211)+b'\x7f\x00\x00\x01')
        self.assertEqual(len([e for e in self.events if isinstance(e,tuple) and e[0]=='timeout']),2)
        self.assertIn('ack',self.events);self.assertEqual(self.events[-1],('close',20))

    def test_non_gt7_titles_never_connect(self):
        for title in [b'PPSA01316',b'PPSA99999',b'NPXS40001',b'PPSA013170']:
            self.title=title;self.events=[];self.call();self.assertNotIn('socket',self.events)

    def test_null_and_failed_app_info_are_ignored(self):
        self.assertEqual(self.hook(None,None,None),123);self.assertEqual(self.events,['original'])
        self.events=[];self.app_result=-1;self.call();self.assertNotIn('socket',self.events)

    def test_socket_and_connect_failure_return_without_waiting(self):
        self.socket_result=-1;self.call();self.assertNotIn('ack',self.events)
        self.socket_result=20;self.connect_result=-1;self.events=[];self.call()
        self.assertFalse(self.sent);self.assertNotIn('ack',self.events);self.assertEqual(self.events[-1],('close',20))

    def test_short_sends_are_completed(self):
        self.short_io=True;self.call();self.assertEqual(self.sent,struct.pack('<i',4321));self.assertIn('ack',self.events)

    def test_icon_registration_is_fixed_to_gt7patcher_title(self):
        icon=C.CFUNCTYPE(C.c_int)(self.base+self.meta['symbols']['icon_hook'])
        self.assertEqual(icon(),17)
        self.assertEqual(self.events,[('install',b'GTPS00001',b'/user/app/',1,0)])

if __name__=='__main__':unittest.main(verbosity=2)
