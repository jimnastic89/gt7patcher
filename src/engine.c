/* SPDX-License-Identifier: GPL-3.0-only
 * GT7-only launch engine. The physical copy method was adapted and
 * substantially modified from Apache-2.0 ps-patch-system/shared/proc_rw.c
 * (illusionyy and contributors).
 * FW12.40 icon registration offsets follow GPL-3.0 ShadowMountPlus.
 */
#include "engine.h"
#include "config.h"
#include "notify.h"
#include "hook_blob.h"
#include "icon.h"
#include "live_bridge.h"
#include "live_blob.h"
#include <ps5/kernel.h>
#include <ps5/mdbg.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>
typedef struct {uint32_t app_id;uint64_t unknown1;char title_id[10];uint32_t apptype;char unknown2[0x36];} AppInfo;
typedef struct __attribute__((packed)){int pid;unsigned char final_attempt;} PatchRequest;
extern int sceKernelGetAppInfo(pid_t,AppInfo *);
extern int sceShellCoreUtilGetAppLaunchedParamString(uint32_t,const char *,char *,size_t);
extern int sceUserServiceInitialize(void *);
extern int sceUserServiceGetForegroundUser(uint32_t *);
typedef struct {uint32_t structsize,user_id,app_opt;uint64_t crash_report;uint32_t check_flag;} Gt7LaunchContext;
extern int sceSystemServiceLaunchApp(const char *,const char **,Gt7LaunchContext *);
/* SDK-private dynlib layout, same as ps-patch-system/shared/dynlib.h. */
typedef struct {
    uintptr_t next,path,unknown0[2];unsigned refcount;uintptr_t handle;
    uintptr_t mapbase,mapsize,textsize,database,datasize,unknown1,unknown1size;
    uintptr_t entry,unknown2,vaddrbase;unsigned tlsindex;
    uintptr_t tlsinit,tlsinitsize,tlssize,tlsoffset,tlsalign,pltgot,unknown3[6];
    uintptr_t init,fini,eh_frame_hdr,eh_frame_hdr_size,eh_frame,eh_frame_size;
} Module;
static pid_t shell=-1;
static Module module;
static uintptr_t bridge,slot,original;
static unsigned char *saved_frame;
static int service=-1,active,damaged;
static int user_service_ready;
static atomic_int live;
static pthread_t thread;
static unsigned char expected_bridge[sizeof(hook_blob)];
static const char *state_root="/data/gt7p/patches";
static void engine_log(const char *stage){
    FILE *f=fopen("/data/gt7p/startup.log","a");
    if(f){fprintf(f,"%s\n",stage);fclose(f);}
}
static void engine_log_result(const char *stage,long result){
    FILE *f=fopen("/data/gt7p/startup.log","a");
    if(f){fprintf(f,"%s: %ld (0x%lx)\n",stage,result,(unsigned long)result);fclose(f);}
}
static int verified_write(pid_t pid,uintptr_t address,const void *data,size_t size);
static unsigned firmware_version(void){return kernel_get_fw_version()>>16;}
/* Stop ShellCore while replacing a multi-byte dispatcher instruction. Restore
 * our credentials on every path; no permanent debugger capability changes. */
static int trace_call(int request,pid_t pid){
    unsigned char saved[16],caps[16];memset(caps,0xff,sizeof(caps));
    pid_t self=getpid();uint64_t auth=kernel_get_ucred_authid(self);
    if(!auth||kernel_get_ucred_caps(self,saved))return -1;
    int r=-1;
    if(!kernel_set_ucred_authid(self,0x4800000000010003ULL)&&!kernel_set_ucred_caps(self,caps))
        r=ptrace(request,pid,NULL,0);
    int restored=kernel_set_ucred_authid(self,auth)==0;
    restored=kernel_set_ucred_caps(self,saved)==0&&restored;
    return restored?r:-1;
}
static int stopped_swap(uintptr_t address,const void *expected,const void *data,size_t size){
    if(trace_call(PT_ATTACH,shell))return -1;
    int status=0;pid_t waited;
    do{waited=waitpid(shell,&status,0);}while(waited<0&&errno==EINTR);
    int r=-1;
    unsigned char current[12];
    if(size<=sizeof(current)&&waited==shell&&WIFSTOPPED(status)&&!mdbg_copyout(shell,address,current,size)&&!memcmp(current,expected,size)){
        r=verified_write(shell,address,data,size);
        if(r&&verified_write(shell,address,expected,size))damaged=1;
    }
    if(trace_call(PT_DETACH,shell)){damaged=1;r=-1;}
    return r;
}
static int read_mem(pid_t pid,uintptr_t address,void *data,size_t size){return mdbg_copyout(pid,address,data,size);}
/* Fault target pages in, then copy through the direct map; mdbg_copyin cannot
 * write executable pages on this firmware. Handle large pages and boundaries. */
static int write_mem(pid_t pid,uintptr_t address,const void *data,size_t size){
    unsigned char *probe=malloc(size);if(!probe)return -1;
    int r=read_mem(pid,address,probe,size);free(probe);if(r)return -1;
    uintptr_t proc=kernel_get_proc(pid);if(!proc)return -1;
    uintptr_t vm=kernel_getlong(proc+KERNEL_OFFSET_PROC_P_VMSPACE);if(!vm)return -1;
    /* The pmap member moved between the early and later PS5 kernels.  The
     * old fixed 0x2e8 offset made all writes fail on 5.00-5.50. */
    unsigned fw=kernel_get_fw_version()>>16;
    uintptr_t pmap_offset=(fw>=0x0100&&fw<=0x0102)?0x2c0:(fw<=0x0550?0x2e0:0x2e8);
    uint64_t paging[2];if(kernel_copyout(vm+pmap_offset+32,paging,sizeof(paging)))return -1;
    if(!paging[1]||paging[0]<=paging[1])return -1;
    uint64_t dmap=paging[0]-paging[1];const unsigned char *p=data;
    while(size){
        uint64_t table=paging[1]&0x000ffffffffff000ULL,physical=0,limit=0;
        for(int shift=39;shift>=12;shift-=9){
            uint64_t entry=kernel_getlong(dmap+table+((address>>shift)&511)*8);
            if(!(entry&1))return -1;
            if((entry&128)||shift==12){
                uint64_t mask=(1ULL<<shift)-1;
                physical=(entry&(((1ULL<<52)-1)&~mask))|(address&mask);
                limit=(physical|mask)+1;break;
            }table=entry&0x000ffffffffff000ULL;
        }
        size_t n=limit-physical;if(n>size)n=size;if(!n)return -1;
        if(kernel_copyin(p,dmap+physical,n))return -1;
        address+=n;p+=n;size-=n;
    }return 0;
}
static int verified_write(pid_t pid,uintptr_t address,const void *data,size_t size){
    unsigned char *check=malloc(size);if(!check)return -1;
    int r=write_mem(pid,address,data,size)||read_mem(pid,address,check,size)||memcmp(data,check,size);
    free(check);return r?-1:0;
}
static pid_t find_shell(void){
    int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PROC,0};size_t n=0;
    if(sysctl(mib,4,NULL,&n,NULL,0))return -1;
    unsigned char *b=malloc(n);if(!b)return -1;
    if(sysctl(mib,4,b,&n,NULL,0)){free(b);return -1;}
    pid_t result=-1;
    for(size_t at=0;at+sizeof(struct kinfo_proc)<=n;){
        struct kinfo_proc *k=(void *)(b+at);
        if(k->ki_structsize<(int)sizeof(*k)||at+(size_t)k->ki_structsize>n)break;
        if(!strcmp(k->ki_comm,"SceShellCore")){result=k->ki_pid;break;}
        at+=k->ki_structsize;
    }free(b);return result;
}
static int main_module(pid_t pid,Module *out){
    uintptr_t proc=kernel_get_proc(pid),node=0;if(!proc)return -1;
    if(kernel_copyout(proc+0x3e8,&node,sizeof(node)))return -1;
    for(unsigned i=0;i<512&&node;i++){
        if(kernel_copyout(node,&node,sizeof(node))||!node)return -1;
        if(kernel_copyout(node,out,sizeof(*out)))return -1;
        if(!out->handle)return 0;
    }return -1;
}
/* 01.710.000 ProgressData singleton. Always resolve the object afresh: heap
 * addresses change between sessions. RTTI and three live balances confirmed
 * this member's identity; native copy methods access it as a separate dword. */
static int credits_game(pid_t *pid,uintptr_t *base){
    int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PROC,0};size_t n=0;
    if(sysctl(mib,4,NULL,&n,NULL,0))return -1;
    unsigned char *data=malloc(n);if(!data)return -1;
    if(sysctl(mib,4,data,&n,NULL,0)){free(data);return -1;}
    int result=-1;
    for(size_t at=0;at+sizeof(struct kinfo_proc)<=n;){
        struct kinfo_proc *k=(void *)(data+at);
        if(k->ki_structsize<(int)sizeof(*k)||at+(size_t)k->ki_structsize>n)break;
        if(!strcmp(k->ki_comm,"eboot.bin")){
            AppInfo app={0};Module game;char version[32]={0};
            if(!sceKernelGetAppInfo(k->ki_pid,&app)&&!strcmp(app.title_id,"PPSA01317")&&
               !sceShellCoreUtilGetAppLaunchedParamString(app.app_id,"CONTENT_VERSION",version,sizeof(version))&&
               !strcmp(version,"01.710.000")&&!main_module(k->ki_pid,&game)&&game.mapbase&&game.mapsize>0x066e9650){
                *pid=k->ki_pid;*base=game.mapbase;result=0;break;
            }
        }
        at+=(size_t)k->ki_structsize;
    }
    free(data);return result;
}
typedef struct {uint64_t magic,original,map,allocation,heartbeat;uint32_t state,reserved;uint64_t entry;} LiveBoot;
_Static_assert(sizeof(LiveBoot)==56,"bootstrap ABI");
_Static_assert(sizeof(LiveMailbox)==280,"mailbox ABI");
enum {LIVE_SLOT_RVA=0x051bee08,LIVE_ORIGINAL_RVA=0x0028bb20,LIVE_BOOT_RVA=0x03f0e940};
static int live_boot_read(pid_t pid,uintptr_t base,LiveBoot *boot){
    return read_mem(pid,base+LIVE_BOOT_RVA+LIVE_BOOT_CONTROL,boot,sizeof(*boot))||boot->magic!=UINT64_C(0x31544f4f424c3747)?-1:0;
}
static int live_install(pid_t pid,uintptr_t base){
    uintptr_t current=0;LiveBoot existing;
    if(read_mem(pid,base+LIVE_SLOT_RVA,&current,8))return -1;
    if(current!=base+LIVE_ORIGINAL_RVA){
        if(live_boot_read(pid,base,&existing))return -1;
        return current==base+LIVE_BOOT_RVA+LIVE_BOOT_ENTRY||
            (existing.allocation&&current==existing.allocation+LIVE_FRAME)?0:-1;
    }
    unsigned char blank[0xf0],code[16];
    static const unsigned char expected[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x83,0xec,0x10};
    if(read_mem(pid,base+LIVE_ORIGINAL_RVA,code,sizeof(expected))||memcmp(code,expected,sizeof(expected))||
       read_mem(pid,base+LIVE_BOOT_RVA,blank,sizeof(blank)))return -1;
    for(unsigned i=0;i<sizeof(blank);i++)if(blank[i])return -1;
    uintptr_t map=kernel_dynlib_dlsym(pid,0x2001,"mmap");if(!map)return -1;
    LiveBoot boot={UINT64_C(0x31544f4f424c3747),base+LIVE_ORIGINAL_RVA,map,0,0,0,0,0};
    unsigned char blob[sizeof(live_boot_blob)];memcpy(blob,live_boot_blob,sizeof(blob));
    memcpy(blob+LIVE_BOOT_CONTROL,&boot,sizeof(boot));
    /* The bootstrap state is in executable padding; its page must also be
     * writable by the game. Never replace instructions in a running thread. */
    if(kernel_mprotect(pid,(base+LIVE_BOOT_RVA)&~(uintptr_t)0x3fff,0x4000,PROT_READ|PROT_WRITE|PROT_EXEC)||
       verified_write(pid,base+LIVE_BOOT_RVA,blob,sizeof(blob)))return -1;
    uintptr_t hook=base+LIVE_BOOT_RVA+LIVE_BOOT_ENTRY;
    return verified_write(pid,base+LIVE_SLOT_RVA,&hook,sizeof(hook));
}
static int live_resolve(pid_t pid,uintptr_t base,uintptr_t *address,LiveMailbox *state){
    LiveBoot boot;uintptr_t slot=0;
    if(live_boot_read(pid,base,&boot)||boot.original!=base+LIVE_ORIGINAL_RVA||boot.state<2||boot.state>4||
       boot.allocation<0x10000||boot.allocation>0x00007fffffff8000ULL||(boot.allocation&0x3fff)||
       read_mem(pid,base+LIVE_SLOT_RVA,&slot,8))return -1;
    uintptr_t mailbox=boot.allocation+LIVE_MAILBOX;
    if(slot!=base+LIVE_BOOT_RVA+LIVE_BOOT_ENTRY)return -1;
    if(boot.state==2){
        unsigned char blob[sizeof(live_bridge_blob)];memcpy(blob,live_bridge_blob,sizeof(blob));
        LiveMailbox initial={0};initial.magic=LIVE_MAGIC;initial.base=base;initial.original=boot.original;
        initial.make_event=base+0x035db2e0;initial.acquire=base+0x035dbcc0;
        initial.destroy_event=base+0x0097b3c0;initial.set_credits=base+0x0250e420;
        initial.car_ctor=base+0x003f4dc0;initial.get_riding_car=base+0x00447870;
        initial.car_dtor=base+0x003fa810;initial.car_state=CAR_QUEUED;initial.enabled=1;
        initial.garage_root=base+0x0044bf50;initial.garage_lock=base+0x03f0a460;
        initial.garage_unlock=base+0x03f0a470;initial.garage_riding_id=base+0x00447930;
        initial.garage_get_car=base+0x00447f50;initial.garage_set_car=base+0x024bd120;
        initial.set_wheel_width=base+0x01af0a20;
        initial.set_wheel_inch_up=base+0x01af0980;initial.add_wheel_offset=base+0x007ac980;
        initial.set_hub_offset=base+0x01aef670;
        memcpy(blob+LIVE_MAILBOX,&initial,sizeof(initial));
        if(kernel_mprotect(pid,boot.allocation,0x4000,PROT_READ|PROT_WRITE|PROT_EXEC)||
           (kernel_get_vmem_protection(pid,boot.allocation,0x4000)&7)!=7||
           verified_write(pid,boot.allocation,blob,sizeof(blob)))return -1;
        uintptr_t entry=boot.allocation+LIVE_FRAME;
        uintptr_t control=base+LIVE_BOOT_RVA+LIVE_BOOT_CONTROL;
        if(verified_write(pid,control+offsetof(LiveBoot,entry),&entry,8))return -1;
        uint32_t published=3;
        if(verified_write(pid,control+offsetof(LiveBoot,state),&published,4))return -1;
    }else if(boot.entry!=boot.allocation+LIVE_FRAME)return -1;
    if(read_mem(pid,mailbox,state,sizeof(*state))||state->magic!=LIVE_MAGIC||state->base!=base||state->original!=boot.original)return -1;
    /* Verify executable bytes too, excluding the mutable mailbox. */
    unsigned char code[LIVE_MAILBOX];
    if(read_mem(pid,boot.allocation,code,sizeof(code))||memcmp(code,live_bridge_blob,sizeof(code)))return -1;
    *address=mailbox;return 0;
}
int gt7_live_ready(void){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    return !credits_game(&pid,&base)&&!live_resolve(pid,base,&address,&state)&&state.enabled&&state.heartbeat>0&&state.state!=LIVE_QUEUED&&state.state!=LIVE_RUNNING;
}
void gt7_live_configure(int enabled){
    pid_t pid;uintptr_t base;LiveBoot boot;LiveMailbox state;
    if(credits_game(&pid,&base)||live_boot_read(pid,base,&boot)||boot.state<3||boot.state>4||
       boot.allocation<0x10000||boot.allocation>0x00007fffffff8000ULL)return;
    uintptr_t address=boot.allocation+LIVE_MAILBOX;
    if(read_mem(pid,address,&state,sizeof(state))||state.magic!=LIVE_MAGIC||state.base!=base)return;
    uint32_t value=enabled?1U:0U;
    (void)verified_write(pid,address+offsetof(LiveMailbox,enabled),&value,sizeof(value));
}
#ifdef GT7_LIVE_TEST
int gt7_live_test_attach(void){
    pid_t pid;uintptr_t base;
    return credits_game(&pid,&base)?-1:live_install(pid,base);
}
#endif
static int live_submit(const Gt7Credits *expected,uint32_t amount){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(amount>LIVE_MAX_CREDITS||credits_game(&pid,&base)||pid!=expected->pid||live_resolve(pid,base,&address,&state)||!state.heartbeat)return -1;
    if(state.state==LIVE_QUEUED||state.state==LIVE_RUNNING)return -3;
    state.expected_object=expected->object;state.expected_amount=expected->amount;state.amount=amount;
    state.observed=0;state.sequence++;
    /* Leave state unchanged until the entire request is published. The game
     * never claims a partially written request. Only the game writes RUNNING. */
    if(verified_write(pid,address+offsetof(LiveMailbox,expected_object),&state.expected_object,8)||
       verified_write(pid,address+offsetof(LiveMailbox,expected_amount),&state.expected_amount,16))return -1;
    uint32_t queued=LIVE_QUEUED;
    if(verified_write(pid,address+offsetof(LiveMailbox,state),&queued,4))return -1;
    for(unsigned i=0;i<150;i++){
        usleep(10000);
        if(read_mem(pid,address,&state,sizeof(state)))return -3;
        if(state.state==LIVE_DONE)return state.observed==amount?0:-1;
        if(state.state==LIVE_STALE)return -2;
        if(state.state==LIVE_INVALID||state.state==LIVE_CANCELLED)return -1;
    }
    /* Do not overwrite a possibly running request or fall back to raw writes.
     * Subsequent submissions remain blocked until the game acknowledges it. */
    return -3;
}
int gt7_credits_read(Gt7Credits *out){
    pid_t pid;uintptr_t base,object=0,vtable=0,again=0;uint32_t amount=0;
    if(credits_game(&pid,&base)||read_mem(pid,base+0x066e9648,&object,sizeof(object))||
       object<0x10000||object>0x00007ffffffffe00ULL||(object&7)||
       read_mem(pid,object,&vtable,sizeof(vtable))||vtable!=base+0x053edca8||
       read_mem(pid,object+0x14c,&amount,sizeof(amount))||amount>INT32_MAX||
       read_mem(pid,base+0x066e9648,&again,sizeof(again))||again!=object)return -1;
    *out=(Gt7Credits){pid,object,amount};return 0;
}
int gt7_credits_set(const Gt7Credits *expected,uint32_t amount){
    Gt7Credits current,after;
    if(amount>LIVE_MAX_CREDITS||gt7_credits_read(&current))return -1;
    /* Reject stale screens/sessions. This is a best-effort stale-value guard,
     * not an atomic transaction with the game's own currency updates. */
    if(current.pid!=expected->pid||current.object!=expected->object||current.amount!=expected->amount)return -2;
    int result=live_submit(&current,amount);if(result)return result;
    if(gt7_credits_read(&after)||after.pid!=current.pid||after.object!=current.object||after.amount!=amount)return -1;
    return 0;
}
int gt7_wheel_width_read(Gt7WheelWidth *out){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(!out||credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled||!state.heartbeat)return -1;
    int ready=state.car_state==CAR_READY;
    if(ready)*out=(Gt7WheelWidth){pid,state.car_model_code,state.car_sequence,state.wheel_width_front,state.wheel_width_rear};
    if(state.car_state!=CAR_QUEUED&&state.car_state!=CAR_RUNNING){
        uint32_t queued=CAR_QUEUED;
        if(verified_write(pid,address+offsetof(LiveMailbox,car_state),&queued,sizeof(queued))&&!ready)return -1;
    }
    return ready?0:-1;
}
static int wheel_width_submit(const Gt7WheelWidth *expected,int8_t front,int8_t rear){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(!expected||credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled||!state.heartbeat)return -1;
    if(state.wheel_state==WHEEL_QUEUED||state.wheel_state==WHEEL_RUNNING)return -3;
    /* car_sequence is a read-refresh counter, not a car revision.  The
     * values below are rechecked again by the game UI thread before writing. */
    if((state.car_state!=CAR_READY&&state.car_state!=CAR_QUEUED)||state.car_model_code!=expected->model_code||
       state.wheel_width_front!=expected->front||state.wheel_width_rear!=expected->rear)return -2;
    state.wheel_expected_sequence=expected->sequence;state.wheel_expected_model_code=expected->model_code;
    state.wheel_expected_front=expected->front;state.wheel_expected_rear=expected->rear;
    state.wheel_requested_front=front;state.wheel_requested_rear=rear;
    if(verified_write(pid,address+offsetof(LiveMailbox,wheel_expected_sequence),&state.wheel_expected_sequence,12)||
       verified_write(pid,address+offsetof(LiveMailbox,wheel_expected_front),&state.wheel_expected_front,4))return -1;
    uint32_t queued=WHEEL_QUEUED;
    if(verified_write(pid,address+offsetof(LiveMailbox,wheel_state),&queued,4))return -1;
    for(unsigned i=0;i<150;i++){
        usleep(10000);
        if(read_mem(pid,address,&state,sizeof(state)))return -3;
        if(state.wheel_state==WHEEL_DONE)return state.car_model_code==expected->model_code&&state.wheel_width_front==front&&state.wheel_width_rear==rear?0:-1;
        if(state.wheel_state==WHEEL_STALE)return -2;
        if(state.wheel_state==WHEEL_INVALID||state.wheel_state==WHEEL_UNAVAILABLE)return -1;
    }
    return -3;
}
int gt7_wheel_width_set(const Gt7WheelWidth *expected,int8_t front,int8_t rear){
    return wheel_width_submit(expected,front,rear);
}
int gt7_wheel_geometry_read(Gt7WheelGeometry *out){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(!out||credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled||!state.heartbeat||state.car_state!=CAR_READY)return -1;
    *out=(Gt7WheelGeometry){pid,state.car_model_code,state.wheel_inch_up,state.wheel_offset_front,state.wheel_offset_rear,state.hub_offset_front,state.hub_offset_rear};return 0;
}
int gt7_wheel_geometry_set(const Gt7WheelGeometry *expected,int8_t inch_up,int16_t offset_front,int16_t offset_rear,int16_t hub_front,int16_t hub_rear){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(!expected||credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled||!state.heartbeat)return -1;
    if(state.geometry_state==GEOMETRY_QUEUED||state.geometry_state==GEOMETRY_RUNNING)return -3;
    if((state.car_state!=CAR_READY&&state.car_state!=CAR_QUEUED)||state.car_model_code!=expected->model_code||state.wheel_inch_up!=expected->inch_up||
       state.wheel_offset_front!=expected->offset_front||state.wheel_offset_rear!=expected->offset_rear||state.hub_offset_front!=expected->hub_front||state.hub_offset_rear!=expected->hub_rear)return -2;
    if((int)offset_front-expected->offset_front<INT8_MIN||(int)offset_front-expected->offset_front>INT8_MAX||
       (int)offset_rear-expected->offset_rear<INT8_MIN||(int)offset_rear-expected->offset_rear>INT8_MAX)return -1;
    state.geometry_expected_model_code=expected->model_code;state.geometry_expected_inch_up=expected->inch_up;state.geometry_requested_inch_up=inch_up;
    state.geometry_expected_offset_front=expected->offset_front;state.geometry_expected_offset_rear=expected->offset_rear;state.geometry_expected_hub_front=expected->hub_front;state.geometry_expected_hub_rear=expected->hub_rear;
    state.geometry_requested_offset_front=offset_front;state.geometry_requested_offset_rear=offset_rear;state.geometry_requested_hub_front=hub_front;state.geometry_requested_hub_rear=hub_rear;
    if(verified_write(pid,address+offsetof(LiveMailbox,geometry_expected_model_code),&state.geometry_expected_model_code,sizeof(state)-offsetof(LiveMailbox,geometry_expected_model_code)))return -1;
    uint32_t queued=GEOMETRY_QUEUED;
    if(verified_write(pid,address+offsetof(LiveMailbox,geometry_state),&queued,4))return -1;
    for(unsigned i=0;i<150;i++){usleep(10000);if(read_mem(pid,address,&state,sizeof(state)))return -3;
        if(state.geometry_state==GEOMETRY_DONE)return state.wheel_inch_up==inch_up&&state.wheel_offset_front==offset_front&&state.wheel_offset_rear==offset_rear&&state.hub_offset_front==hub_front&&state.hub_offset_rear==hub_rear?0:-1;
        if(state.geometry_state==GEOMETRY_STALE)return -2;
        if(state.geometry_state==GEOMETRY_INVALID||state.geometry_state==GEOMETRY_UNAVAILABLE)return -1;}
    return -3;
}
static int bridge_prepare(void){
    engine_log("Bridge: locating ShellCore and main module");
    if(bridge)return damaged?-1:0;
    shell=find_shell();if(shell<0||main_module(shell,&module))return -1;
    engine_log("Bridge: scanning OnNewProcess and callback pointer");
    if(!module.mapsize||module.mapsize>128*1024*1024||module.eh_frame_size<sizeof(hook_blob))return -1;
    unsigned char *image=malloc(module.mapsize);if(!image)return -1;
    if(read_mem(shell,module.mapbase,image,module.mapsize)){free(image);return -1;}
    /* ps-patch-system identifies three ShellCore layouts: 4.00-10.00,
     * 11.xx, and 12.00+.  The old implementation only recognized the last
     * one, so 5.00 could never start the engine.  A zero mask entry is a
     * wildcard for firmware-dependent register encodings. */
    static const unsigned char old_signature[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18,0x49,0x89,0x00,0xbf,0x18,0,0,0,0x49,0x89,0xd6,0x49,0x89,0xf5};
    static const unsigned char mid_signature[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50,0x49,0x89,0xfd,0xbf,0x18,0,0,0,0x49,0x89,0xd7,0x49,0x89,0xf4,0xe8,0,0,0,0};
    static const unsigned char new_signature[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18,0x49,0x89,0xfd,0xbf,0x18,0,0,0,0x49,0x89,0xd6,0x49,0x89,0xf4,0xe8,0,0,0,0};
    const unsigned char *signature=old_signature;size_t signature_size=sizeof(old_signature),wildcard_start=19,wildcard_size=1;
    unsigned fw=firmware_version();
    if(fw>=0x0b00&&fw<0x0c00){signature=mid_signature;signature_size=sizeof(mid_signature);wildcard_start=29;wildcard_size=4;engine_log("Bridge: using 11.xx signature");}
    else if(fw>=0x0c00){signature=new_signature;signature_size=sizeof(new_signature);wildcard_start=32;wildcard_size=4;engine_log("Bridge: using 12.xx signature");}
    else engine_log("Bridge: using 4.00-10.00 signature");
    unsigned matches=0,pointers=0;
    for(size_t i=0;i+signature_size<=module.mapsize;i++){
        if((wildcard_start==0||!memcmp(image+i,signature,wildcard_start))&&
           (wildcard_start+wildcard_size==signature_size||!memcmp(image+i+wildcard_start+wildcard_size,signature+wildcard_start+wildcard_size,signature_size-wildcard_start-wildcard_size))){original=module.mapbase+i;matches++;}
    }
    /* A stopped prior payload intentionally leaves its inert bridge in
     * eh_frame. Exclude that region so the bridge's saved function address is
     * not mistaken for the live callback slot during restart or reinjection. */
    if(matches==1)for(size_t i=0;i+8<=module.mapsize;i+=8){
        uintptr_t address=module.mapbase+i;
        int in_frame=address>=module.eh_frame&&address-module.eh_frame<module.eh_frame_size;
        if(!in_frame&&!memcmp(image+i,&original,8)){slot=address;pointers++;}
    }
    free(image);if(matches!=1||pointers!=1)return -1;
    engine_log("Bridge: resolving imports");
    unsigned char blob[sizeof(hook_blob)];memcpy(blob,hook_blob,sizeof(blob));
    const char *names[]={"sceKernelGetAppInfo","socket","connect","setsockopt","send","recv","close"};
    uintptr_t imports[9]={original};
    for(unsigned i=0;i<7;i++){imports[i+1]=kernel_dynlib_dlsym(shell,0x2001,names[i]);if(!imports[i+1])return -1;}
    imports[8]=module.mapbase+0x2e0050; /* AppInstallTitleDirMain, 12.40 */
    memcpy(blob+HOOK_IMPORTS,imports,sizeof(imports));
    memcpy(expected_bridge,blob,sizeof(blob));
    saved_frame=malloc(sizeof(blob));if(!saved_frame)return -1;
    bridge=module.eh_frame;
    if(!bridge||read_mem(shell,bridge,saved_frame,sizeof(blob))){
        free(saved_frame);saved_frame=NULL;bridge=0;return -1;
    }
    engine_log("Bridge: writing and verifying code");
    int protect_result=kernel_mprotect(shell,bridge&~(uintptr_t)0x3fff,((bridge+sizeof(blob)+0x3fff)&~(uintptr_t)0x3fff)-(bridge&~(uintptr_t)0x3fff),PROT_READ|PROT_WRITE|PROT_EXEC);
    engine_log_result("Bridge: mprotect result",protect_result);
    int write_result=verified_write(shell,bridge,blob,sizeof(blob));
    engine_log_result("Bridge: verified write result",write_result);
    if(protect_result||write_result){
        if(verified_write(shell,bridge,saved_frame,sizeof(blob)))damaged=1;
        free(saved_frame);saved_frame=NULL;bridge=0;return -1;
    }return 0;
}
static int apply_line(pid_t pid,uintptr_t base,unsigned address,const char *hex){
    size_t len=strlen(hex);if(!len||(len&1)||len>8192||address<0x400000)return -1;
    unsigned char bytes[4096];
    for(size_t i=0;i<len/2;i++){unsigned v;if(sscanf(hex+i*2,"%2x",&v)!=1)return -1;bytes[i]=(unsigned char)v;}
    return verified_write(pid,base+(address-0x400000),bytes,len/2);
}
static int apply_debug_environment(pid_t pid,const Module *game){
    static const unsigned char pattern[]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,'r','e','l','e','a','s','e',0,0,0,0,0,0,0,0};
    static const unsigned char replacement[]={'d','e','b','u','g',0,0};
    const size_t chunk=1024*1024,overlap=sizeof(pattern)-1;
    if(!game->mapsize||game->mapsize>128*1024*1024)return -1;
    unsigned char *buf=malloc(chunk+overlap);if(!buf)return -1;
    size_t offset=0,carry=0,matches=0;uintptr_t target=0;
    while(offset<game->mapsize){
        size_t n=game->mapsize-offset;if(n>chunk)n=chunk;
        if(read_mem(pid,game->mapbase+offset,buf+carry,n)){free(buf);return -1;}
        size_t total=carry+n;
        for(size_t i=0;i+sizeof(pattern)<=total;i++)if(!memcmp(buf+i,pattern,sizeof(pattern))){
            target=game->mapbase+offset-carry+i+16;matches++;
        }
        carry=total<overlap?total:overlap;memmove(buf,buf+total-carry,carry);offset+=n;
    }
    free(buf);
    return matches==1?verified_write(pid,target,replacement,sizeof(replacement)):-1;
}
static int apply_gt7(pid_t pid,int final_attempt){
    AppInfo app={0};char version[32]={0};Module game;
    if(sceKernelGetAppInfo(pid,&app)||strcmp(app.title_id,"PPSA01317"))return 0;
    if(sceShellCoreUtilGetAppLaunchedParamString(app.app_id,"CONTENT_VERSION",version,sizeof(version))||strcmp(version,"01.710.000")){
        if(final_attempt)notify("GT7: patches skipped. This payload requires version 01.710.000.");return 0;
    }
    if(main_module(pid,&game)||!game.mapbase){if(final_attempt)notify("GT7: could not locate the game executable. Close GT7 and retry.");return 0;}
    Config config;if(load_config(state_root,&config)){if(final_attempt)notify("GT7: could not read saved patch choices. Close GT7 and save again.");return 0;}
    /* Generate from validated settings and built-in patch data. Never execute
     * arbitrary XML supplied on disk; only the config comment is consumed. */
    FILE *f=fopen("/data/gt7p/launch.tmp","w+");
    if(f)unlink("/data/gt7p/launch.tmp");
    if(!f||emit_xml(f,&config)){if(f)fclose(f);if(final_attempt)notify("GT7: could not prepare patches.");return 0;}
    rewind(f);char line[16384],hex[8193];unsigned address;int failed=0,patched=0;
    const char *current_name="Unknown patch";
    while(fgets(line,sizeof(line),f)){
        unsigned feature;
        if(sscanf(line,"<!-- GT7FEATURE %u -->",&feature)==1){
            current_name=feature<PATCH_COUNT?feature_name(feature):"Unknown patch";
            continue;
        }
        if(strstr(line,"<!-- GT7FEATURE units -->")){
            current_name="Display units";continue;
        }
        if(strstr(line,"<!-- GT7FEATURE language -->")){
            current_name="Game language";continue;
        }
        if(strncmp(line,"<Line ",6))continue;
        if(sscanf(line,"<Line Type=\"bytes\" Address=\"0x%x\" Value=\"%8192[0123456789abcdefABCDEF]\"",&address,hex)!=2||
           (address==0x060e83b0?apply_debug_environment(pid,&game):apply_line(pid,game.mapbase,address,hex))){failed=1;break;}
        patched=1;
    }
    fclose(f);
    if(!failed&&(config.mask&LIVE_SETTINGS_BIT)&&live_install(pid,game.mapbase)){
        if(final_attempt)notify("GT7: could not install the optional Live Settings bridge. Close GT7 before retrying.");
        return 0;
    }
    if(!failed&&(config.mask&LIVE_SETTINGS_BIT))patched=1;
    if(failed){if(final_attempt)notify("GT7: Patch failed: %s. Close the game before retrying.",current_name);return 0;}
    else if(patched)notify("GT7Patcher: Saved patches applied.");
    else notify("GT7: all patches are disabled. Launching without changes.");
    return 1;
}
static void *patch_service(void *unused){
    (void)unused;atomic_store(&live,1);
    while(atomic_load(&live)){
        int fd=accept(service,NULL,NULL);if(fd<0){if(errno==EINTR)continue;break;}
        struct timeval timeout={10,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        PatchRequest request={0};size_t n=0;
        while(n<sizeof(request)){ssize_t r=recv(fd,(char *)&request+n,sizeof(request)-n,0);if(r<=0)break;n+=(size_t)r;}
        if(n==sizeof(request)&&request.pid>0){unsigned char ack=apply_gt7(request.pid,request.final_attempt)?1:0;send(fd,&ack,1,0);}
        close(fd);
    }atomic_store(&live,0);return NULL;
}
int gt7_engine_ready(void){
    uintptr_t value=0;unsigned char code[sizeof(hook_blob)];
    return active&&!damaged&&atomic_load(&live)&&!read_mem(shell,slot,&value,8)&&value==bridge+HOOK_LAUNCH&&
        !read_mem(shell,bridge,code,sizeof(code))&&!memcmp(code,expected_bridge,sizeof(code));
}
int gt7_engine_active(void){return active&&!damaged;}
int gt7_engine_start(void){
    if(active)return gt7_engine_ready()?0:-1;
    if(damaged||bridge_prepare())return -1;
    service=socket(AF_INET,SOCK_STREAM,0);if(service<0)return -1;
    struct sockaddr_in addr={0};addr.sin_len=sizeof(addr);addr.sin_family=AF_INET;addr.sin_port=htons(7211);addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    int opt=1;setsockopt(service,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    if(bind(service,(void *)&addr,sizeof(addr))||listen(service,4)){close(service);service=-1;return -1;}
    atomic_store(&live,1);
    if(pthread_create(&thread,NULL,patch_service,NULL)){close(service);service=-1;atomic_store(&live,0);return -1;}
    uintptr_t entry=bridge+HOOK_LAUNCH;
    if(verified_write(shell,slot,&entry,8)){
        damaged=1;verified_write(shell,slot,&original,8);shutdown(service,SHUT_RDWR);pthread_join(thread,NULL);close(service);service=-1;return -1;
    }active=1;return 0;
}
int gt7_engine_stop(void){
    if(active){
        if(!gt7_engine_ready()||verified_write(shell,slot,&original,8)){damaged=1;return -1;}
        active=0;shutdown(service,SHUT_RDWR);pthread_join(thread,NULL);close(service);service=-1;
    }
    /* Leave the bridge memory in place: another ShellCore thread may still
     * be returning through it. The restored pointer prevents new calls. */
    return 0;
}
int gt7_launch_game(void){
    uint32_t user=0xffffffffU;const char *argv[]={NULL};Gt7LaunchContext context={0};
    if(!user_service_ready){int result=sceUserServiceInitialize(NULL);if(result)return result;user_service_ready=1;}
    if(!sceUserServiceGetForegroundUser(&user)&&user!=0xffffffffU&&(int32_t)user!=-1){
        context.structsize=sizeof(context);context.user_id=user;
        return sceSystemServiceLaunchApp("PPSA01317",argv,&context);
    }
    return sceSystemServiceLaunchApp("PPSA01317",argv,NULL);
}
/* ShadowMountPlus may already own the 12.40 AppInstallAll hook. Its bridge
 * deliberately exposes one-shot title/base-directory immediates. Reuse that
 * verified bridge instead of replacing its live hook. */
static int install_with_shadowmount_bridge(int (*install_all)(void *),int *handled){
    *handled=0;
    const uintptr_t cave=module.mapbase+0x1887200;
    const uintptr_t hook=cave+0x260;
    const uintptr_t target=module.mapbase+0x2dd100;
    unsigned char target_bytes[12],marker[2],hook_head[4];
    if(read_mem(shell,target,target_bytes,sizeof(target_bytes))||read_mem(shell,cave,marker,sizeof(marker))||read_mem(shell,hook,hook_head,sizeof(hook_head)))return -1;
    uintptr_t destination=0;memcpy(&destination,target_bytes+2,sizeof(destination));
    if(target_bytes[0]!=0x48||target_bytes[1]!=0xb8||target_bytes[10]!=0xff||target_bytes[11]!=0xe0||destination!=hook||marker[0]!=0x66||marker[1]!=0x90||hook_head[0]!=0xb0||hook_head[2]!=0x84||hook_head[3]!=0xc0)return 0;
    *handled=1;
    engine_log("Icon: reusing existing ShadowMountPlus registration bridge");
    unsigned char title[16]={0},directory[16]={0},armed=1,disarmed=0;
    memcpy(title,"GTPS00001",9);memcpy(directory,"/user/app/",10);
    if(verified_write(shell,cave+0x26c,title,8)||verified_write(shell,cave+0x27a,title+8,8)||
       verified_write(shell,cave+0x289,directory,8)||verified_write(shell,cave+0x298,directory+8,8)||
       verified_write(shell,cave+0x261,&armed,1))return -1;
    int result=install_all(NULL);
    if(verified_write(shell,cave+0x261,&disarmed,1)){damaged=1;return -1;}
    engine_log_result("Icon: ShadowMountPlus registration result",result);
    return (result==0||(uint32_t)result==0x80990002U)?1:-1;
}
static void mark_icon_installed(void){
    FILE *f=fopen("/data/gt7p/gt7patcher-icon-installed","w");
    if(f){fputs("GTPS00001\n",f);fflush(f);fsync(fileno(f));fclose(f);}
}
int gt7_install_icon(void){
    engine_log("Icon: checking installation");
    const char *marker="/data/gt7p/gt7patcher-icon-installed";
    const char *metadata="/user/app/GTPS00001/sce_sys/param.json";
    if(access(marker,F_OK)==0){
        if(access(metadata,F_OK)==0)return 0;
        engine_log("Icon: stale installation marker removed");
        unlink(marker);
    }
    /* AppInstUtil requires UserService to be loaded first on 12.40. Keep both
     * optional so shortcut setup cannot prevent the payload from starting. */
    engine_log("Icon: loading UserService prerequisite");
    static void *user_library,*install_library;
    user_library=dlopen("/system/common/lib/libSceUserService.sprx",RTLD_LAZY);
    if(!user_library){engine_log("Icon: UserService load failed");return -1;}
    engine_log("Icon: loading AppInstUtil");
    install_library=dlopen("/system/common/lib/libSceAppInstUtil.sprx",RTLD_LAZY);
    if(!install_library){engine_log("Icon: AppInstUtil load failed");return -1;}
    int (*initialize)(void)=(int (*)(void))dlsym(install_library,"sceAppInstUtilInitialize");
    int (*install_all)(void *)=(int (*)(void *))dlsym(install_library,"sceAppInstUtilAppInstallAll");
    int (*install_title_dir)(const char *,const char *,void *)=(int (*)(const char *,const char *,void *))dlsym(install_library,"sceAppInstUtilAppInstallTitleDir");
    int (*uninstall)(const char *)=(int (*)(const char *))dlsym(install_library,"sceAppInstUtilAppUnInstall");
    if(!initialize||(!install_all&&firmware_version()>=0x1200)||(!install_title_dir&&firmware_version()<0x1200)){engine_log("Icon: registration API resolution failed");return -1;}
    if(initialize()){engine_log("Icon: AppInstUtil initialization failed");return -1;}
    /* Refresh the existing GTPS00001 registration once when adopting the new
     * GT7Patcher name and icon. The registration call does not replace cached
     * metadata for an already-installed title. */
    if(access("/user/app/GTPS00001/sce_sys/param.json",F_OK)==0&&uninstall){
        int result=uninstall("GTPS00001");engine_log_result("Icon: previous registration removal result",result);
    }
    if(mkdir("/user/app/GTPS00001",0755)&&errno!=EEXIST)return -1;
    if(mkdir("/user/app/GTPS00001/sce_sys",0755)&&errno!=EEXIST)return -1;
    const char *paths[]={"/user/app/GTPS00001/sce_sys/param.json","/user/app/GTPS00001/sce_sys/icon0.png"};
    const unsigned char *assets[]={icon_param,icon_png};size_t sizes[]={sizeof(icon_param)-1,sizeof(icon_png)};
    for(unsigned i=0;i<2;i++){
        FILE *f=fopen(paths[i],"wb");if(!f)return -1;
        int bad=fwrite(assets[i],1,sizes[i],f)!=sizes[i];if(fflush(f)||fsync(fileno(f)))bad=1;if(fclose(f))bad=1;if(bad)return -1;
    }
    /* Firmware before 12.00 still exposes the public TitleDir API. */
    if(firmware_version()<0x1200){
        if(!install_title_dir){engine_log("Icon: TitleDir registration API unavailable");return -1;}
        int result=install_title_dir("GTPS00001","/user/app/GTPS00001",NULL);
        engine_log_result("Icon: TitleDir registration result",result);
        if(result==0||(uint32_t)result==0x80990002U){mark_icon_installed();return 0;}
        return -1;
    }
    shell=find_shell();
    if(shell<0||main_module(shell,&module)){engine_log("Icon: ShellCore lookup failed");return -1;}
    int handled=0,shared=install_with_shadowmount_bridge(install_all,&handled);
    if(handled){if(shared>0){mark_icon_installed();return 0;}return -1;}
    if(bridge_prepare()){engine_log("Icon: bridge preparation failed");return -1;}
    /* Firmware 12 removed the public TitleDir RPC. Temporarily redirect the
     * internal AppInstallAll dispatcher to our fixed-title registration call.
     * Restore and verify its bytes regardless of registration result. */
    uintptr_t target=module.mapbase+0x2dd100,destination=bridge+HOOK_ICON;
    unsigned char original_bytes[12],jump[12]={0x48,0xb8};
    const unsigned char prologue[]={0x55,0x48,0x89,0xe5};
    if(read_mem(shell,target,original_bytes,sizeof(original_bytes))||memcmp(original_bytes,prologue,4)){engine_log("Icon: AppInstallAll target prologue mismatch");return -1;}
    unsigned char install_prologue[4];
    if(read_mem(shell,module.mapbase+0x2e0050,install_prologue,4)||memcmp(install_prologue,prologue,4)){engine_log("Icon: TitleDir target prologue mismatch");return -1;}
    memcpy(jump+2,&destination,8);jump[10]=0xff;jump[11]=0xe0;
    engine_log("Icon: redirecting fixed-title registration call");
    if(stopped_swap(target,original_bytes,jump,sizeof(jump))){engine_log("Icon: redirect swap failed");return -1;}
    int result=install_all(NULL);
    if(stopped_swap(target,jump,original_bytes,sizeof(original_bytes))){damaged=1;return -1;}
    FILE *log=fopen("/data/gt7p/startup.log","a");if(log){fprintf(log,"Icon: registration result 0x%08x\n",(unsigned)result);fclose(log);}
    if(result==0||(uint32_t)result==0x80990002U){mark_icon_installed();return 0;}
    return -1;
}
