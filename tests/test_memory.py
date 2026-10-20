"""Exercise the production copy functions against synthetic PS5 page tables.

The exact functions are compiled from engine.c, with only SDK calls mocked.
This is not a firmware emulator or a console compatibility test.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
PREFIX=r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#define KERNEL_OFFSET_PROC_P_VMSPACE 0x200
static unsigned char ram[0x400000];
static const uint64_t dmap=0x100000000ULL;
static int fault_fail,kernel_fail,copy_fail,readback_bad,reads;
static int large;
static uint64_t fixture_phys(uint64_t address){
    size_t offset=address-0x400000;
    return large?0x200000+offset:(offset<4096?0x100000+offset:0x110000+offset-4096);
}
static uintptr_t kernel_get_proc(pid_t pid){(void)pid;return kernel_fail?0:0x1000;}
static uint64_t kernel_getlong(uintptr_t address){
    if(address==0x1200)return 0x2000;
    assert(address>=dmap&&address-dmap+8<=sizeof(ram));
    uint64_t value;memcpy(&value,ram+address-dmap,8);return value;
}
static int kernel_copyout(uintptr_t address,void *data,size_t size){
    assert(address==0x2000+0x2e8+32&&size==16);
    uint64_t paging[]={dmap+0x4000,0x4000};memcpy(data,paging,size);return 0;
}
static int kernel_copyin(const void *data,uintptr_t address,size_t size){
    if(copy_fail)return -1;
    assert(address>=dmap&&address-dmap+size<=sizeof(ram));
    memcpy(ram+address-dmap,data,size);return 0;
}
static int mdbg_copyout(pid_t pid,uintptr_t address,void *data,size_t size){
    (void)pid;if(fault_fail)return -1;
    for(size_t i=0;i<size;i++)((unsigned char *)data)[i]=ram[fixture_phys(address+i)];
    if(readback_bad&&reads++)((unsigned char *)data)[0]^=1;
    return 0;
}
static void table(size_t address,uint64_t value){memcpy(ram+address,&value,8);}
static void reset(void){
    memset(ram,0,sizeof(ram));fault_fail=kernel_fail=copy_fail=readback_bad=reads=large=0;
    table(0x4000,0x5001);table(0x5000,0x6001);table(0x6010,0x7001);
    table(0x7000,0x100001);table(0x7008,0x110001);
}
'''
SUFFIX=r'''
int main(void){
    unsigned char data[32];for(unsigned i=0;i<sizeof(data);i++)data[i]=(unsigned char)(i+1);
    reset();assert(verified_write(42,0x400ffa,data,32)==0);
    assert(memcmp(ram+0x100ffa,data,6)==0&&memcmp(ram+0x110000,data+6,26)==0);
    reset();large=1;table(0x6010,0x200081);
    assert(verified_write(42,0x400ffa,data,32)==0&&memcmp(ram+0x200ffa,data,32)==0);
    reset();fault_fail=1;assert(write_mem(42,0x400000,data,32)==-1&&ram[0x100000]==0);
    reset();kernel_fail=1;assert(write_mem(42,0x400000,data,32)==-1&&ram[0x100000]==0);
    reset();copy_fail=1;assert(write_mem(42,0x400000,data,32)==-1&&ram[0x100000]==0);
    reset();table(0x7000,0);assert(write_mem(42,0x400000,data,32)==-1&&ram[0x100000]==0);
    reset();readback_bad=1;assert(verified_write(42,0x400000,data,32)==-1);
    reset();table(0x7008,0);assert(write_mem(42,0x400ffa,data,32)==-1);
    assert(memcmp(ram+0x100ffa,data,6)==0&&ram[0x110000]==0);
    puts("8 production-memory scenarios passed");return 0;
}
'''

class MemoryTests(unittest.TestCase):
    def test_page_boundaries_large_pages_and_failures(self):
        source=(ROOT/'src/engine.c').read_text()
        functions=source[source.index('static int read_mem('):source.index('static pid_t find_shell(')]
        with tempfile.TemporaryDirectory() as temp:
            c=Path(temp)/'memory.c';binary=Path(temp)/'memory'
            c.write_text(PREFIX+functions+SUFFIX)
            subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

if __name__=='__main__':unittest.main(verbosity=2)
