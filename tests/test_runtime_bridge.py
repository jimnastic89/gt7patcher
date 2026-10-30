"""Execute the actual in-game bridge and bootstrap on x86-64 Linux.

Native game methods are replaced by ABI-compatible mocks. No console access.
"""
import ctypes as C
import json
import mmap
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class RuntimeBridgeTests(unittest.TestCase):
    def test_bootstrap_allocates_once_and_always_forwards(self):
        meta=json.loads((ROOT/'build/live_bridge.json').read_text())['live_boot']
        blob=(ROOT/'build/live_boot.bin').read_bytes()
        events=[]
        original=C.CFUNCTYPE(None,C.c_void_p,C.c_float)(lambda context,dt: events.append(('frame',context,dt)))
        def allocate(address,size,prot,flags,fd,offset):
            events.append(('map',address,size,prot,flags,fd,offset))
            return 0x100000000
        mapper=C.CFUNCTYPE(C.c_void_p,C.c_void_p,C.c_size_t,C.c_int,C.c_int,C.c_int,C.c_longlong)(allocate)
        with mmap.mmap(-1,4096,prot=7) as memory:
            memory.write(blob)
            base=C.addressof(C.c_char.from_buffer(memory))
            control=meta['symbols']['live_boot_control']
            memory[control:control+56]=struct.pack('<5Q2IQ',0x31544f4f424c3747,C.cast(original,C.c_void_p).value,
                C.cast(mapper,C.c_void_p).value,0,0,0,0,0)
            entry=C.CFUNCTYPE(None,C.c_void_p,C.c_float)(base+meta['symbols']['live_boot_entry'])
            entry(0x1234,0.125);entry(0x5678,0.25)
            self.assertEqual(events,[('map',None,16384,7,0x1002,-1,0),('frame',0x1234,0.125),('frame',0x5678,0.25)])
            state=struct.unpack('<5Q2I',memory[control:control+48])
            self.assertEqual(state[3:6],(0x100000000,2,2))
            bridge=C.CFUNCTYPE(None,C.c_void_p,C.c_float)(lambda ctx,dt: events.append(('bridge',ctx,dt)))
            memory[control+48:control+56]=struct.pack('<Q',C.cast(bridge,C.c_void_p).value)
            memory[control+40:control+44]=struct.pack('<I',3)
            entry(0xabcd,0.5)
            self.assertEqual(events[-1],('bridge',0xabcd,0.5))
            self.assertEqual(struct.unpack('<I',memory[control+40:control+44])[0],4)

    def test_actual_bridge_blob_idle_and_no_change_request(self):
        meta=json.loads((ROOT/'build/live_bridge.json').read_text())['live_bridge']
        blob=(ROOT/'build/live_bridge.bin').read_bytes()
        events=[]
        original=C.CFUNCTYPE(None,C.c_void_p,C.c_float)(lambda ctx,dt:events.append((ctx,dt)))
        native=[]
        def construct(event,delta,label,reason):
            self.assertEqual(delta,0)
            self.assertEqual(C.string_at(label,10),b'GT7Patcher')
            C.memset(event,0,0x160);native.append('construct')
        def setter(obj,amount):
            C.c_uint32.from_address(obj+0x14c).value=C.c_uint32.from_address(amount).value
            native.append('set')
        def acquire(event):
            self.assertEqual(C.c_int.from_address(event+0x30).value,0)
            native.append('acquire');return 0
        ctor=C.CFUNCTYPE(None,C.c_void_p,C.c_int,C.c_void_p,C.c_void_p)(construct)
        set_amount=C.CFUNCTYPE(None,C.c_void_p,C.c_void_p)(setter)
        grant=C.CFUNCTYPE(C.c_int,C.c_void_p)(acquire)
        destructor=C.CFUNCTYPE(None,C.c_void_p)(lambda event:native.append('destroy'))
        obj=(C.c_uint64*64)();root=C.c_uint64(C.addressof(obj))
        base=C.addressof(root)-0x066e9648
        obj[0]=base+0x053edca8
        C.c_uint32.from_address(C.addressof(obj)+0x14c).value=45950
        with mmap.mmap(-1,16384,prot=7) as memory:
            memory.write(blob);address=C.addressof(C.c_char.from_buffer(memory));off=meta['symbols']['live_mailbox']
            def header(state=0,amount=45950):
                return struct.pack('<10Q6I',0x3145474449523747,base,C.cast(original,C.c_void_p).value,
                    *[C.cast(fn,C.c_void_p).value for fn in (ctor,grant,destructor,set_amount)],
                    0,0,C.addressof(obj),state,1,45950,amount,0,1)
            memory[off:off+104]=header()
            frame=C.CFUNCTYPE(None,C.c_void_p,C.c_float)(address+meta['symbols']['live_frame'])
            frame(0x1234,0.125)
            memory[off:off+104]=header(1)
            frame(0x5678,0.25)
            state=struct.unpack('<10Q6I',memory[off:off+104])
            self.assertEqual(events,[(0x1234,0.125),(0x5678,0.25)])
            self.assertEqual(state[10],3)
            self.assertEqual(state[14],45950)
            memory[off:off+104]=header(1,46000)
            frame(0x9876,0.5)
            self.assertEqual(native,['construct','set','acquire','destroy'])
            self.assertEqual(events[-1],(0x9876,0.5))
            self.assertEqual(struct.unpack('<10Q6I',memory[off:off+104])[14],46000)

    def test_native_set_event_lifecycle_and_request_guards(self):
        fixture=r'''
#include "live_bridge.c"
#include <assert.h>
#include <string.h>
#include <stdlib.h>
static unsigned calls,sets,constructs,acquires,destroys,car_constructs,car_destroys;
static unsigned char object[0x200];
static uintptr_t root;
static unsigned char style[0x80],implementation[0x40];
static void frame(void *context,float dt){assert(context==(void *)0x1234&&dt==0.125f);calls++;}
static void construct(void *event,int delta,const GameString *a,const GameString *b){
    assert(delta==0&&a->size==10&&b->capacity==15);
    assert(!memcmp(a->text,"GT7Patcher",10));memset(event,0,0x160);
    constructs++;
}
static void setter(void *target,const uint32_t *amount){
    assert(target==object);memcpy(object+0x14c,amount,4);sets++;
}
static int acquire(void *event){assert(*(int *)((char *)event+0x30)==0);acquires++;return 0;}
static void destroy(void *event){(void)event;destroys++;}
static void car_construct(void *car){memset(car,0,0x30);*(uintptr_t *)car=(uintptr_t)implementation;car_constructs++;}
static int get_riding_car(void *car){assert(*(uintptr_t *)car==(uintptr_t)implementation);return 1;}
static void car_destroy(void *car){assert(*(uintptr_t *)car==(uintptr_t)implementation);car_destroys++;}
static void request(uint32_t amount){live_mailbox.amount=amount;live_mailbox.expected_amount=*(uint32_t *)(object+0x14c);live_mailbox.state=LIVE_QUEUED;}
int main(void){
    root=(uintptr_t)object;
    uintptr_t base=(uintptr_t)&root-0x066e9648;
    *(uintptr_t *)object=base+0x053edca8;
    *(uint32_t *)(object+0x14c)=45950;
    *(uintptr_t *)(implementation+0x18)=(uintptr_t)style;
    *(int8_t *)(style+0x26)=-2;*(int8_t *)(style+0x25)=3;*(uint32_t *)(style+0x68)=431;
    live_mailbox=(LiveMailbox){.magic=LIVE_MAGIC,.base=base,.original=(uintptr_t)frame,
        .make_event=(uintptr_t)construct,.acquire=(uintptr_t)acquire,.destroy_event=(uintptr_t)destroy,
        .set_credits=(uintptr_t)setter,.expected_object=(uintptr_t)object,.enabled=1,
        .car_ctor=(uintptr_t)car_construct,.get_riding_car=(uintptr_t)get_riding_car,
        .car_dtor=(uintptr_t)car_destroy,.car_state=CAR_QUEUED};
    live_frame((void *)0x1234,0.125f);assert(calls==1&&sets==0);
    assert(live_mailbox.car_state==CAR_READY&&live_mailbox.wheel_width_front==3&&live_mailbox.wheel_width_rear==-2);
    assert(live_mailbox.car_model_code==431&&live_mailbox.car_sequence==1&&car_constructs==1&&car_destroys==1);
    request(46000);live_frame((void *)0x1234,0.125f);
    assert(live_mailbox.state==LIVE_DONE&&live_mailbox.observed==46000);
    assert(sets==1&&constructs==1&&acquires==1&&destroys==1);
    live_frame((void *)0x1234,0.125f);assert(sets==1); /* exactly once */
    request(45000);live_frame((void *)0x1234,0.125f);assert(live_mailbox.observed==45000&&sets==2);
    request(45000);live_frame((void *)0x1234,0.125f);assert(sets==2&&live_mailbox.state==LIVE_DONE);
    request(1);live_mailbox.expected_amount++;live_frame((void *)0x1234,0.125f);assert(live_mailbox.state==LIVE_STALE&&sets==2);
    request(1);live_mailbox.expected_object++;live_frame((void *)0x1234,0.125f);assert(live_mailbox.state==LIVE_STALE&&sets==2);live_mailbox.expected_object--;
    request(100000001);live_frame((void *)0x1234,0.125f);assert(live_mailbox.state==LIVE_INVALID&&sets==2);
    request(1);live_mailbox.enabled=0;live_frame((void *)0x1234,0.125f);assert(live_mailbox.state==LIVE_CANCELLED&&sets==2);
    assert(calls==9&&live_mailbox.heartbeat==9&&car_constructs==1&&car_destroys==1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory);(path/'native.c').write_text(fixture)
            subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                '-I',str(ROOT/'src'),str(path/'native.c'),'-o',str(path/'native')],check=True)
            subprocess.run([str(path/'native')],check=True)

if __name__=='__main__':unittest.main(verbosity=2)
