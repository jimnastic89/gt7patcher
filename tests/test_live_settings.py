"""Live credit guards and host HTTP behavior; never connects to a console."""
from pathlib import Path
import json
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


class LiveSettingsTests(unittest.TestCase):
    def test_production_credit_guards(self):
        source = (ROOT / 'src/engine.c').read_text()
        functions = source[source.index('int gt7_credits_read('):source.index('static int bridge_prepare(')]
        fixture = r'''
#include "engine.h"
#include "live_bridge.h"
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
extern int usleep(unsigned int);
static unsigned char object[0x200];
static uintptr_t pointer=0xe415c45160ULL;
static int reads,writes,bad_type,bad_game,changed_root,fail_write;
static int credits_game(pid_t *pid,uintptr_t *base){*pid=105;*base=0x400000;return bad_game?-1:0;}
static int live_resolve(pid_t pid,uintptr_t base,uintptr_t *address,LiveMailbox *state){
    (void)pid;(void)base;(void)address;(void)state;return -1;
}
static int read_mem(pid_t pid,uintptr_t address,void *out,size_t size){
    assert(pid==105);
    if(address==0x6ae9648){
        uintptr_t p=changed_root&&reads++?pointer+8:pointer;
        memcpy(out,&p,size);return 0;
    }
    if(address==pointer){uintptr_t v=bad_type?0:0x57edca8;memcpy(out,&v,size);return 0;}
    assert(address>=pointer&&address+size<=pointer+sizeof(object));
    memcpy(out,object+address-pointer,size);return 0;
}
static int verified_write(pid_t pid,uintptr_t address,const void *data,size_t size){
    assert(pid==105&&address==pointer+0x14c&&size==4);writes++;
    if(fail_write)return -1;
    memcpy(object+0x14c,data,size);return 0;
}
static int live_submit(const Gt7Credits *before,uint32_t amount){return amount==before->amount?0:verified_write(before->pid,before->object+0x14c,&amount,4);}
'''
        checks = r'''
int main(void){
    uint32_t amount=43700;memset(object,0xa5,sizeof(object));memcpy(object+0x14c,&amount,4);
    Gt7Credits state,stale;
    assert(!gt7_credits_read(&state)&&state.amount==43700&&state.pid==105&&state.object==pointer);
    stale=state;stale.amount++;assert(gt7_credits_set(&stale,1)==-2&&!writes);
    stale=state;stale.pid++;assert(gt7_credits_set(&stale,1)==-2&&!writes);
    stale=state;stale.object+=8;assert(gt7_credits_set(&stale,1)==-2&&!writes);
    assert(gt7_credits_set(&state,UINT32_MAX)==-1&&!writes);
    bad_type=1;assert(gt7_credits_read(&stale)==-1);assert(gt7_credits_set(&state,1)==-1&&!writes);bad_type=0;
    bad_game=1;assert(gt7_credits_read(&stale)==-1);bad_game=0;
    changed_root=1;reads=0;assert(gt7_credits_read(&stale)==-1);changed_root=0;
    assert(!gt7_credits_set(&state,43700)&&!writes);
    fail_write=1;assert(gt7_credits_set(&state,50000)==-1);fail_write=0;
    assert(!gt7_credits_set(&state,50000));assert(!gt7_credits_read(&state)&&state.amount==50000);
    for(unsigned i=0;i<sizeof(object);i++)if(i<0x14c||i>=0x150)assert(object[i]==0xa5);
    assert(!gt7_credits_set(&state,0));assert(!gt7_credits_read(&state)&&!state.amount);
    assert(!gt7_credits_set(&state,LIVE_MAX_CREDITS));assert(!gt7_credits_read(&state)&&state.amount==LIVE_MAX_CREDITS);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'credits.c').write_text(fixture + functions + checks)
            subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I', str(ROOT / 'src'), str(path / 'credits.c'), '-o', str(path / 'credits')], check=True)
            subprocess.run([str(path / 'credits')], check=True)

    def test_host_api_validation_and_availability(self):
        with tempfile.TemporaryDirectory() as directory:
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0))
                port = sock.getsockname()[1]
            process = subprocess.Popen([str(ROOT / 'build/gt7patcher-host'), directory, str(port)], stdout=subprocess.DEVNULL)
            base = f'http://127.0.0.1:{port}'
            try:
                for _ in range(60):
                    try:
                        with urllib.request.urlopen(base + '/api/status', timeout=1) as response:
                            state = json.load(response)
                        break
                    except OSError:
                        time.sleep(.05)
                else:
                    self.fail('Host did not start')
                self.assertEqual(state['live'], dict(available=False, credits=None, session=None,
                    wheelWidth=dict(available=False, front=None, rear=None, modelCode=None, sequence=None),
                    wheelGeometry=dict(available=False, inchUp=None, offsetFront=None, offsetRear=None,
                                       hubFront=None, hubRear=None, modelCode=None)))
                def request(body=None, token=True, endpoint='/api/live/credits'):
                    headers = {'X-GT7-Token': state['token']} if token else {}
                    req = urllib.request.Request(base + endpoint, data=body, headers=headers)
                    try:
                        response = urllib.request.urlopen(req, timeout=2)
                    except urllib.error.HTTPError as error:
                        response = error
                    with response:
                        return response.status
                self.assertEqual(request(), 404)
                self.assertEqual(request(b'amount=50000&expected=43700&session=105:e415c45160', False), 403)
                for body in [b'', b'amount=-1&expected=43700&session=105:e415c45160',
                             b'amount=2147483648&expected=43700&session=105:e415c45160',
                             b'amount=999999999999999999999999999999999999&expected=43700&session=105:abc',
                             b'amount=1.5&expected=43700&session=105:e415c45160',
                             b'amount=1&expected=43700&session=0:e415c45160',
                             b'amount=1&expected=43700&session=105:e415c45160&amount=2']:
                    self.assertEqual(request(body), 400, body)
                selected = dict(state['config'], mask=64)
                req = urllib.request.Request(base + '/api/config', data=urllib.parse.urlencode(selected).encode(), headers={'X-GT7-Token': state['token']})
                with self.assertRaises(urllib.error.HTTPError) as rejected:
                    urllib.request.urlopen(req)
                self.assertEqual(rejected.exception.code, 400)
                self.assertNotIn('Live Settings', [f['name'] for f in state['features']])
                for amount in (0, 50000, 100000000):
                    self.assertEqual(request(f'amount={amount}&expected=43700&session=105:e415c45160'.encode()), 409)
                wheel = b'front=7&rear=-4&expectedFront=0&expectedRear=10&modelCode=431&sequence=7'
                self.assertEqual(request(wheel, endpoint='/api/live/wheel-width'), 409)
                for body in [b'front=128&rear=0&expectedFront=0&expectedRear=10&modelCode=431&sequence=7',
                             b'front=+7&rear=0&expectedFront=0&expectedRear=10&modelCode=431&sequence=7',
                             b'front=7&rear=0&expectedFront=0&expectedRear=10&modelCode=0&sequence=7',
                             b'front=7&rear=0&expectedFront=0&expectedRear=10&modelCode=431&sequence=0']:
                    self.assertEqual(request(body, endpoint='/api/live/wheel-width'), 400, body)
            finally:
                process.terminate()
                process.wait(timeout=5)


if __name__ == '__main__':
    unittest.main(verbosity=2)
