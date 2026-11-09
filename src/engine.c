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
#include "hud_tree.h"
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
#include <unistd.h>
#include <fcntl.h>
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
static uintptr_t bridge,slot,original,ime_slot,ime_original,ime_hook;
static unsigned char *saved_frame;
static int service=-1,active,damaged;
static int user_service_ready;
static atomic_int live;
static atomic_uint launch_requests;
static atomic_int launch_pid;
static atomic_int launch_stage;
static pthread_t thread;
static unsigned char expected_bridge[sizeof(hook_blob)];
static const char *state_root="/data/gt7p/patches";
enum {LAUNCH_NONE,LAUNCH_REQUESTED,LAUNCH_NOT_GT7,LAUNCH_VERSION,LAUNCH_MODULE,
      LAUNCH_CONFIG,LAUNCH_PREPARE,LAUNCH_PATCH,LAUNCH_LIVE,LAUNCH_APPLIED,
      LAUNCH_UNCHANGED};
static const char *const launch_stages[]={
    "waiting for a launch request","launch request received","not GT7",
    "unsupported game version","game module unavailable","saved configuration unavailable",
    "patch generation failed","patch write failed","live bridge install failed",
    "saved patches applied","all patches disabled"
};
static unsigned firmware_version(void);
/* The public TitleDir registration entry point is available before firmware
 * 12.  On 12+ AppInstallAll must be bridged to ShellCore's internal TitleDir
 * handler.  These verified RVAs are from ShadowMountPlus's generated
 * per-firmware table through 13.60. Keep the firmware exact: similar
 * ShellCore revisions can move the cave or dispatcher by a few hundred bytes. */
typedef struct {unsigned firmware;uintptr_t title_dir,install_all,smp_cave;} IconProfile;
static const IconProfile icon_profiles[]={
    {0x1200,0x2e0050,0x2dd100,0x1886360}, /* 12.00 */
    {0x1202,0x2e0050,0x2dd100,0x1886360}, /* 12.02 */
    {0x1220,0x2e0050,0x2dd100,0x1887200}, /* 12.20 */
    {0x1240,0x2e0050,0x2dd100,0x1887200}, /* 12.40 */
    {0x1260,0x2e0320,0x2dd3d0,0x188dfa0}, /* 12.60 */
    {0x1270,0x2e0320,0x2dd3d0,0x188dfa0}, /* 12.70 */
    {0x1300,0x31f6e0,0x31c790,0x1958090}, /* 13.00 */
    {0x1320,0x31ff20,0x31cfd0,0x19619f0}, /* 13.20 */
    {0x1340,0x3200a0,0x31d150,0x1961b70}, /* 13.40 */
    {0x1342,0x3200a0,0x31d150,0x1961b70}, /* 13.42 */
    {0x1360,0x320c40,0x31dcf0,0x1962700}, /* 13.60 */
};
static const IconProfile *icon_profile(void){
    unsigned firmware=firmware_version();
    for(unsigned i=0;i<sizeof(icon_profiles)/sizeof(icon_profiles[0]);i++)
        if(icon_profiles[i].firmware==firmware)return &icon_profiles[i];
    return NULL;
}
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
/* Same module list as main_module, matched by path substring. Used to reach a
 * system library (libSceRtc) rather than the game executable. */
static int find_module(pid_t pid,const char *needle,Module *out){
    uintptr_t proc=kernel_get_proc(pid),node=0;if(!proc)return -1;
    if(kernel_copyout(proc+0x3e8,&node,sizeof(node)))return -1;
    for(unsigned i=0;i<512&&node;i++){
        if(kernel_copyout(node,&node,sizeof(node))||!node)return -1;
        if(kernel_copyout(node,out,sizeof(*out)))return -1;
        if(out->path){
            char path[256]={0};
            if(!kernel_copyout(out->path,path,sizeof(path)-1)&&strstr(path,needle))return 0;
        }
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
    int result=-1,procs=0,ebs=0;char diag[112]="";
    for(size_t at=0;at+sizeof(struct kinfo_proc)<=n;){
        struct kinfo_proc *k=(void *)(data+at);
        if(k->ki_structsize<(int)sizeof(*k)||at+(size_t)k->ki_structsize>n)break;
        procs++;
        if(!strcmp(k->ki_comm,"eboot.bin")){
            AppInfo app={0};Module game;char version[32]={0};
            ebs++;
            int gai=sceKernelGetAppInfo(k->ki_pid,&app);
            int pvs=gai?1:sceShellCoreUtilGetAppLaunchedParamString(app.app_id,"CONTENT_VERSION",version,sizeof(version));
            int mm=gai?1:main_module(k->ki_pid,&game);
            snprintf(diag,sizeof(diag),"pid=%d gai=%d title=%.9s pvs=%d ver=%.9s mm=%d base=%#lx size=%#lx",
                     (int)k->ki_pid,gai,gai?"?":app.title_id,pvs,pvs?"?":version,mm,
                     mm?0UL:(unsigned long)game.mapbase,mm?0UL:(unsigned long)game.mapsize);
            if(!gai&&!strcmp(app.title_id,"PPSA01317")&&!pvs&&!strcmp(version,"01.710.000")&&
               !mm&&game.mapbase&&game.mapsize>0x066e9650){
                *pid=k->ki_pid;*base=game.mapbase;result=0;break;
            }
        }
        at+=(size_t)k->ki_structsize;
    }
    if(result){static unsigned calls;if((calls++&15)==0){engine_log_result("credits procs",procs);engine_log_result("credits eboot",ebs);if(ebs)engine_log(diag);}}
    free(data);return result;
}
typedef struct {uint64_t magic,original,map,allocation,heartbeat;uint32_t state,reserved;uint64_t entry;} LiveBoot;
_Static_assert(sizeof(LiveBoot)==56,"bootstrap ABI");
_Static_assert(sizeof(LiveMailbox)==6712,"mailbox ABI");
_Static_assert(sizeof(live_boot_blob)<=LIVE_BOOT_RESERVED,"live bootstrap exceeds reserved region");
/* Import jump-table slots (see the module's GOT). The IME slots sit next to
 * one another: sceImeOpen, sceImeUpdate and sceImeClose. */
enum {LIVE_SLOT_RVA=0x051bee08,LIVE_ORIGINAL_RVA=0x0028bb20,
      IME_GOT_RVA=0x055df2a8,IME_UPDATE_GOT_RVA=0x055df288,IME_CLOSE_GOT_RVA=0x055df2b8,
      /* libSceIme/libSceImeDialog import slots share one stride-8 jump
       * table, ordered as GT7's import name table lists them: after
       * sceImeUpdate come DialogGetStatus/Abort/GetResult, then sceImeOpen,
       * then DialogInit, then sceImeClose, then DialogTerm. */
      IME_DIALOG_STATUS_GOT_RVA=0x055df290,IME_DIALOG_RESULT_GOT_RVA=0x055df2a0,
      IME_DIALOG_INIT_GOT_RVA=0x055df2b0,IME_DIALOG_TERM_GOT_RVA=0x055df2c0};
_Static_assert(LIVE_ALLOCATION>=LIVE_PROMPT_WORK+LIVE_PROMPT_WORK_SIZE,"prompt scratch exceeds the live allocation");
static uint32_t foreground_user(void){
    uint32_t user=0xffffffffU;
    if(!user_service_ready){if(sceUserServiceInitialize(NULL))return 0xffffffffU;user_service_ready=1;}
    if(sceUserServiceGetForegroundUser(&user))return 0xffffffffU;
    return user;
}
static int live_boot_read(pid_t pid,uintptr_t base,LiveBoot *boot){
    return read_mem(pid,base+LIVE_BOOT_RVA+LIVE_BOOT_CONTROL,boot,sizeof(*boot))||boot->magic!=UINT64_C(0x31544f4f424c3747)?-1:0;
}
static int live_install(pid_t pid,uintptr_t base){
    uintptr_t current=0;LiveBoot existing;
    if(read_mem(pid,base+LIVE_SLOT_RVA,&current,8)){engine_log("Live: slot read failed");return -1;}
    engine_log_result("Live: slot current",(long)current);
    engine_log_result("Live: slot expected",(long)(base+LIVE_ORIGINAL_RVA));
    if(current!=base+LIVE_ORIGINAL_RVA){
        if(live_boot_read(pid,base,&existing)){engine_log("Live: reattach boot read failed");return -1;}
        if(current==base+LIVE_BOOT_RVA+LIVE_BOOT_ENTRY){engine_log("Live: reattach ok (boot entry)");return 0;}
        if(existing.allocation&&current==existing.allocation+LIVE_FRAME){engine_log("Live: reattach ok (frame)");return 0;}
        engine_log("Live: reattach slot mismatch");return -1;
    }
    unsigned char blank[LIVE_BOOT_RESERVED],code[16];
    static const unsigned char expected[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x83,0xec,0x10};
    if(read_mem(pid,base+LIVE_ORIGINAL_RVA,code,sizeof(expected))){engine_log("Live: prologue read failed");return -1;}
    if(memcmp(code,expected,sizeof(expected))){
        uint64_t head=0;memcpy(&head,code,8);engine_log_result("Live: prologue mismatch head",(long)head);return -1;
    }
    if(read_mem(pid,base+LIVE_BOOT_RVA,blank,sizeof(blank))){engine_log("Live: padding read failed");return -1;}
    for(unsigned i=0;i<sizeof(blank);i++)if(blank[i]){engine_log_result("Live: padding not blank at",(long)i);return -1;}
    uintptr_t map=kernel_dynlib_dlsym(pid,0x2001,"mmap");if(!map){engine_log("Live: mmap dlsym failed");return -1;}
    LiveBoot boot={UINT64_C(0x31544f4f424c3747),base+LIVE_ORIGINAL_RVA,map,0,0,0,0,0};
    unsigned char blob[sizeof(live_boot_blob)];memcpy(blob,live_boot_blob,sizeof(blob));
    memcpy(blob+LIVE_BOOT_CONTROL,&boot,sizeof(boot));
    if(kernel_mprotect(pid,(base+LIVE_BOOT_RVA)&~(uintptr_t)0x3fff,0x4000,PROT_READ|PROT_WRITE|PROT_EXEC)){engine_log("Live: boot mprotect failed");return -1;}
    if(verified_write(pid,base+LIVE_BOOT_RVA,blob,sizeof(blob))){engine_log("Live: boot write failed");return -1;}
    uintptr_t hook=base+LIVE_BOOT_RVA+LIVE_BOOT_ENTRY;
    if(verified_write(pid,base+LIVE_SLOT_RVA,&hook,sizeof(hook))){engine_log("Live: slot write failed");return -1;}
    engine_log("Live: install complete");
    return 0;
}
static int live_resolve(pid_t pid,uintptr_t base,uintptr_t *address,LiveMailbox *state){
    LiveBoot boot;uintptr_t slot=0;
    if(live_boot_read(pid,base,&boot)){engine_log("Resolve: boot read failed");return -1;}
    if(boot.original!=base+LIVE_ORIGINAL_RVA||boot.state<2||boot.state>4||
       boot.allocation<0x10000||boot.allocation>0x00007fffffff8000ULL||(boot.allocation&0x3fff)){engine_log("Resolve: boot fields invalid");return -1;}
    if(read_mem(pid,base+LIVE_SLOT_RVA,&slot,8)){engine_log("Resolve: slot read failed");return -1;}
    uintptr_t mailbox=boot.allocation+LIVE_MAILBOX;
    if(slot!=base+LIVE_BOOT_RVA+LIVE_BOOT_ENTRY){engine_log("Resolve: slot != boot entry");return -1;}
    if(boot.state==2){
        LiveMailbox initial={0};initial.magic=LIVE_MAGIC;initial.base=base;initial.original=boot.original;
        uintptr_t ime_target=0;
        if(read_mem(pid,base+IME_GOT_RVA,&ime_target,8)||!ime_target){engine_log("Resolve: ime got read failed");return -1;}
        initial.ime_original=ime_target;initial.ime_hook_enabled=1;
        initial.allocation=boot.allocation;
        uint32_t user=foreground_user();
        if(user!=0xffffffffU)initial.ime_user_id=user;
        uintptr_t ime_close=0,ime_update=0;
        if(!read_mem(pid,base+IME_CLOSE_GOT_RVA,&ime_close,8)&&ime_close)initial.ime_close=ime_close;
        if(!read_mem(pid,base+IME_UPDATE_GOT_RVA,&ime_update,8)&&ime_update)initial.ime_update=ime_update;
        uintptr_t dialog_init=0,dialog_status=0,dialog_result=0,dialog_term=0;
        if(read_mem(pid,base+IME_DIALOG_INIT_GOT_RVA,&dialog_init,8))dialog_init=0;
        if(read_mem(pid,base+IME_DIALOG_STATUS_GOT_RVA,&dialog_status,8))dialog_status=0;
        if(read_mem(pid,base+IME_DIALOG_RESULT_GOT_RVA,&dialog_result,8))dialog_result=0;
        if(read_mem(pid,base+IME_DIALOG_TERM_GOT_RVA,&dialog_term,8))dialog_term=0;
        initial.ime_dialog_init=dialog_init;initial.ime_dialog_status=dialog_status;
        initial.ime_dialog_result=dialog_result;initial.ime_dialog_term=dialog_term;
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
        initial.get_engine_swap=base+0x02c401b0;initial.add_engine_swap=base+0x02c41800;
        initial.set_engine_swap=base+0x02c41900;initial.reset_engine_swap=base+0x02c421d0;
        if(kernel_mprotect(pid,boot.allocation,LIVE_ALLOCATION,PROT_READ|PROT_WRITE|PROT_EXEC)){engine_log("Resolve: mailbox mprotect failed");return -1;}
        if((kernel_get_vmem_protection(pid,boot.allocation,LIVE_ALLOCATION)&7)!=7){engine_log("Resolve: mailbox protection check failed");return -1;}
        /* Written in three parts straight from the static image. Copying the
         * whole 62 KB bridge onto the stack first would not fit a 64 KB
         * worker-thread stack, and live_resolve is reachable from the pad and
         * patch-service threads. */
        {
            size_t head=LIVE_MAILBOX;
            size_t tail=sizeof(live_bridge_blob)-LIVE_MAILBOX-sizeof(LiveMailbox);
            if(head+sizeof(LiveMailbox)+tail!=sizeof(live_bridge_blob)){engine_log("Resolve: bridge layout mismatch");return -1;}
            if(verified_write(pid,boot.allocation,live_bridge_blob,head)){engine_log("Resolve: bridge write failed");return -1;}
            if(verified_write(pid,boot.allocation+head,&initial,sizeof(initial))){engine_log("Resolve: mailbox write failed");return -1;}
            if(tail&&verified_write(pid,boot.allocation+head+sizeof(LiveMailbox),live_bridge_blob+head+sizeof(LiveMailbox),tail)){engine_log("Resolve: bridge cache write failed");return -1;}
        }
        uintptr_t entry=boot.allocation+LIVE_FRAME;
        uintptr_t control=base+LIVE_BOOT_RVA+LIVE_BOOT_CONTROL;
        if(verified_write(pid,control+offsetof(LiveBoot,entry),&entry,8))return -1;
        uint32_t published=3;
        if(verified_write(pid,control+offsetof(LiveBoot,state),&published,4))return -1;
        ime_hook=boot.allocation+IME_OPEN_HOOK;
        if(verified_write(pid,base+IME_GOT_RVA,&ime_hook,8))return -1;
        ime_slot=base+IME_GOT_RVA;ime_original=ime_target;
    }else if(boot.entry!=boot.allocation+LIVE_FRAME)return -1;
    if(read_mem(pid,mailbox,state,sizeof(*state))||state->magic!=LIVE_MAGIC||state->base!=base||state->original!=boot.original){engine_log("Resolve: mailbox verify failed");return -1;}
    if(state->ime_hook_enabled&&state->ime_original){
        uintptr_t current=0;
        if(read_mem(pid,base+IME_GOT_RVA,&current,8)||current!=boot.allocation+IME_OPEN_HOOK){engine_log("Resolve: ime hook verify failed");return -1;}
        ime_slot=base+IME_GOT_RVA;ime_original=state->ime_original;ime_hook=current;
    }
    /* Verify executable bytes too, excluding the mutable mailbox. Read in
     * windows: the whole region does not fit a worker-thread stack. */
    {
        unsigned char code[1024];
        for(size_t at=0;at<LIVE_MAILBOX;at+=sizeof(code)){
            size_t n=LIVE_MAILBOX-at;if(n>sizeof(code))n=sizeof(code);
            if(read_mem(pid,boot.allocation+at,code,n)||memcmp(code,live_bridge_blob+at,n)){engine_log("Resolve: bridge code verify failed");return -1;}
        }
    }
    *address=mailbox;return 0;
}
int gt7_live_arm_credit_input(void){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled||!state.ime_hook_enabled)return -1;
    uint32_t armed=1;
    return verified_write(pid,address+offsetof(LiveMailbox,credit_input_pending),&armed,sizeof(armed));
}
int gt7_live_credit_prompt_begin(void){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled||!state.ime_hook_enabled||!state.allocation)return -1;
    /* A prompt that is already opening or open is not restarted. */
    if(state.prompt_state==PROMPT_QUEUED||state.prompt_state==PROMPT_RUNNING||state.prompt_state==PROMPT_OPEN)return 0;
    uint32_t user=foreground_user();
    if(user!=0xffffffffU&&verified_write(pid,address+offsetof(LiveMailbox,ime_user_id),&user,sizeof(user)))return -1;
    uint32_t queued=PROMPT_QUEUED;
    if(verified_write(pid,address+offsetof(LiveMailbox,prompt_state),&queued,sizeof(queued)))return -1;
    return 0;
}
/* Consume the bridge's credits-popup request and arm the number pad. The
 * bridge sets LiveMailbox::credit_request the frame the game's "GTCredit"
 * widget first appears (the user pressed the status-bar Credits button).
 * Running here, on the payload's idle tick, reuses
 * gt7_live_credit_prompt_begin so the user id and the existing prompt guards
 * still apply. */
int gt7_live_credit_auto_poll(void){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled)return -1;
    if(!state.credit_request)return 0;
    uint32_t clear=0;
    if(verified_write(pid,address+offsetof(LiveMailbox,credit_request),&clear,sizeof(clear)))return -1;
    return gt7_live_credit_prompt_begin();
}
/* Diagnostic pad reader. An earlier handoff claimed a Square-button shortcut
 * opened the number pad; that is WRONG - the shortcut never worked on hardware
 * and is not a usable credit trigger. The thread below only samples pad state
 * and logs it. Square is 0x8000 in OrbisPadButtonDataOffset. */
static int pad_shortcut_enabled;
/* GT7 keeps two UI depth counters (version-locked to 01.710.000) that read
 * 1/1 on the plain world map and rise to 2/3 once a status overlay (GT Menu,
 * credits dialog) opens. They are the only deterministic world-map signal found
 * so far, but they are generic (any overlay raises them) and are NOT a working
 * credit trigger. Failing safe: an unreadable counter is not treated as the
 * world map. */
#define LIVE_WORLD_MAP_DEPTH_A 0x0000000e400de3c78ULL
#define LIVE_WORLD_MAP_DEPTH_B 0x0000000e400dd58d8ULL
static int live_world_map_active(pid_t pid){
    uint32_t a=0,b=0;
    if(read_mem(pid,(uintptr_t)LIVE_WORLD_MAP_DEPTH_A,&a,4))return 0;
    if(read_mem(pid,(uintptr_t)LIVE_WORLD_MAP_DEPTH_B,&b,4))return 0;
    return a==1&&b==1;
}
void gt7_live_pad_configure(int enabled){pad_shortcut_enabled=enabled;}
/* Diagnostic pad reader, run on a dedicated thread so a blocking pad call can
 * never stall the web UI. This is NOT a working credit trigger (see above); it
 * only logs pad state. The pad handle table is per-process: only the process
 * that called scePadOpen owns a usable handle, so we open our own. */
static volatile int pad_thread_started;
static void *live_pad_thread(void *arg){
#ifdef PS5
    (void)arg;
    int (*read_state)(int,void*)=NULL;
    int (*get_handle)(uint32_t,int,int)=NULL;
    int (*open_pad)(uint32_t,int,int,const void*)=NULL;
    int (*init_pad)(void)=NULL;
    void *library=dlopen("/system/common/lib/libScePad.sprx",RTLD_LAZY);
    if(library){
        read_state=(int (*)(int,void*))dlsym(library,"scePadReadState");
        get_handle=(int (*)(uint32_t,int,int))dlsym(library,"scePadGetHandle");
        open_pad=(int (*)(uint32_t,int,int,const void*))dlsym(library,"scePadOpen");
        init_pad=(int (*)(void))dlsym(library,"scePadInit");
    }
    uint32_t user=foreground_user();
    char msg[192];
    snprintf(msg,sizeof(msg),"Pad: thread read=%d get=%d open=%d init=%d user=0x%x",read_state?1:0,get_handle?1:0,open_pad?1:0,init_pad?1:0,user);
    engine_log(msg);
    int handle=-1;
    if(open_pad&&user!=0xffffffffU){
        handle=open_pad(user,0,0,NULL);
        snprintf(msg,sizeof(msg),"Pad: open handle=%d (0x%x)",handle,handle);
        engine_log(msg);
    }
    if(handle<0&&get_handle&&user!=0xffffffffU){
        handle=get_handle(user,0,0);
        snprintf(msg,sizeof(msg),"Pad: gethandle handle=%d (0x%x)",handle,handle);
        engine_log(msg);
    }
    if(!read_state||handle<0)return NULL;
    int previous=0;
    for(;;){
        _Alignas(16) unsigned char data[0x100];
        memset(data,0,sizeof(data));
        int rc=read_state(handle,data);
        if(rc){static int logged=0;if(!logged){logged=1;snprintf(msg,sizeof(msg),"Pad: read rc=0x%x",(unsigned)rc);engine_log(msg);}usleep(200000);continue;}
        static int first=1;
        if(first){first=0;snprintf(msg,sizeof(msg),"Pad: buttons=0x%x",*(uint32_t*)data);engine_log(msg);}
        int square=(*(uint32_t*)data&0x8000U)!=0;
        if(square&&!previous){
            pid_t pid=0;uintptr_t base=0;uint32_t a=0,b=0;int cg,ra,rb,gate,begin;
            cg=credits_game(&pid,&base);
            ra=cg?1:read_mem(pid,(uintptr_t)LIVE_WORLD_MAP_DEPTH_A,&a,4);
            rb=cg?1:read_mem(pid,(uintptr_t)LIVE_WORLD_MAP_DEPTH_B,&b,4);
            gate=(!cg&&live_world_map_active(pid));
            begin=gate?gt7_live_credit_prompt_begin():(-99);
            snprintf(msg,sizeof(msg),"Pad: square cg=%d a=0x%x b=0x%x ra=%d rb=%d gate=%d begin=%d",cg,a,b,ra,rb,gate,begin);
            engine_log(msg);
        }
        previous=square;
        usleep(20000);
    }
#else
    (void)arg;return NULL;
#endif
}
void gt7_live_pad_poll(void){
#ifdef PS5
    if(!pad_shortcut_enabled||pad_thread_started)return;
    pad_thread_started=1;
    pthread_t t;
    if(pthread_create(&t,NULL,live_pad_thread,NULL))pad_thread_started=0;
    else pthread_detach(t);
#endif
}
int gt7_live_credit_prompt_read(Gt7CreditPrompt *out){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(!out||credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled)return -1;
    *out=(Gt7CreditPrompt){pid,state.prompt_state,state.prompt_result,state.prompt_amount,state.prompt_chars,state.prompt_reserved,state.prompt_dbg_end,state.prompt_dbg_status,state.ime_user_id};
    return 0;
}
/* Temporary read-only diagnostics. Remove before release. */
/* ------------------------------------------------------------------------
 * Hook slots.
 *
 * Each slot owns one snapshot record and one detour stub inside the live
 * allocation, so several game functions can be observed at once. Slot 0 also
 * owns the shared log block used by the census modes (0,3,4,5,6,7,8) and keeps
 * the historical offsets, so the existing diagnostics keep working.
 *
 * Mode 9 is the observation trace: the stub saves the integer argument
 * registers, calls live_trace() in the bridge and appends one fixed 64-byte
 * record to the trace ring. Unlike the census modes it never blocks and never
 * allocates, so it is safe to leave installed while the game runs.
 *
 * A stub replays the displaced prologue at its own address, so the displaced
 * byte range must contain no RIP-relative instruction: its disp32 would
 * resolve from the stub, and the stub sits several GiB away. work/sitelint.py
 * validates candidate sites offline; every default site below passes.
 *
 * Remove before release.
 * ---------------------------------------------------------------------- */
#define LIVE_PROBE_LOG    0x00008000u   /* census log block (slot 0 only)    */
#define LIVE_PROBE_ADDR   0x00008800u   /* snapshot records                  */
#define LIVE_PROBE_RING   0x00000100u
#define LIVE_PROBE_RING_COUNT 64u
#define LIVE_PROBE_RING_WORDS 26u
#define LIVE_PROBE_RECORD_STRIDE 0x00000080u
#define LIVE_PROBE_STUB   0x00009000u   /* detour stubs                      */
#define LIVE_PROBE_CODE_STRIDE   0x00000400u
#define LIVE_PROBE_SLOTS  8u
#define LIVE_PROBE_MODES  9
typedef struct {
    uintptr_t target;
    uint32_t displaced;
    uint32_t mode;
    uint32_t active;
    uint32_t slot;
    uint64_t watch;
    unsigned char original[0x40];
} LiveProbeRecord;
_Static_assert(sizeof(LiveProbeRecord)<=LIVE_PROBE_RECORD_STRIDE,"probe record exceeds slot stride");
static uintptr_t probe_stub(uintptr_t allocation,uint32_t slot){return allocation+LIVE_PROBE_STUB+(uintptr_t)slot*LIVE_PROBE_CODE_STRIDE;}
static uintptr_t probe_record(uintptr_t allocation,uint32_t slot){return allocation+LIVE_PROBE_ADDR+(uintptr_t)slot*LIVE_PROBE_RECORD_STRIDE;}
/* Default observation sites, as RVAs from the module image base. Each is the
 * smallest whole-instruction boundary >= 14 bytes that contains no
 * RIP-relative instruction, which is what a replayed prologue requires.
 *
 *   vm.invoke             script VM call chokepoint. rdi is the invoked
 *                         object, so the record carries its interned symbol
 *                         and bound native: the full script activity log.
 *   ui.pushFunctionEvent  UI event queue push
 *   ui.pushEvent          UI event queue push
 *   ui.filterEvent        UI event filter
 *
 * The event funnels run tens of thousands of times a second, so they are
 * recorded through a sample divider (one record in every 2^sample calls) and
 * without the rdi dereference. */
typedef struct {uint32_t rva;uint32_t displaced;uint32_t slot;uint32_t sample;int deref;const char *label;} LiveTraceSite;
static const LiveTraceSite trace_sites[]={
    {0x0026d00u,17u,0,0,1,"vm.invoke"},
    {0x189b0c0u,17u,1,6,0,"ui.pushFunctionEvent"},
    {0x189ac50u,15u,2,6,0,"ui.pushEvent"},
    {0x18db940u,17u,3,6,0,"ui.filterEvent"},
};
#define LIVE_TRACE_SITE_COUNT ((uint32_t)(sizeof(trace_sites)/sizeof(trace_sites[0])))
int gt7_live_probe_install_slot(uintptr_t target,uint32_t displaced,int mode,uintptr_t watch,uint32_t slot,uint64_t cfg){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled||!state.allocation)return -1;
    if(displaced<14||displaced>0x40||mode<0||mode>LIVE_PROBE_MODES||slot>=LIVE_PROBE_SLOTS)return -1;
    uintptr_t alloc=state.allocation;
    uintptr_t stub=probe_stub(alloc,slot),record=probe_record(alloc,slot),log=alloc+LIVE_PROBE_LOG;
    unsigned char orig[0x40];
    if(read_mem(pid,target,orig,displaced))return -1;
    if(orig[0]==0xFF&&orig[1]==0x25&&*(uintptr_t *)(orig+6)==stub)return -1;/* already hooked here */
    unsigned char code[0x800];size_t n=0;
    if(mode==9){
    /* Observation trace detour. Every integer register the hooked function may
     * be reading is pushed, the block is handed to live_trace() together with
     * the allocated base, and the registers are restored before the displaced
     * prologue runs. live_trace is built with -mgeneral-regs-only, so incoming
     * xmm argument values are left exactly as the caller set them. */
    {
        static const unsigned char pushes[]={0x9C,0x50,0x51,0x52,0x56,0x57,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53};
        static const unsigned char pops[]={0x41,0x5B,0x41,0x5A,0x41,0x59,0x41,0x58,0x5F,0x5E,0x5A,0x59,0x58,0x9D};
        uint64_t fn=alloc+LIVE_TRACE;
        code[n++]=0x48;code[n++]=0x83;code[n++]=0xEC;code[n++]=0x08;   /* sub rsp,8: keeps the call 16-byte aligned */
        memcpy(code+n,pushes,sizeof(pushes));n+=sizeof(pushes);
        code[n++]=0x48;code[n++]=0x89;code[n++]=0xE7;                   /* mov rdi,rsp */
        code[n++]=0x48;code[n++]=0xBE;memcpy(code+n,&cfg,8);n+=8;       /* mov rsi,cfg */
        code[n++]=0x48;code[n++]=0xBA;memcpy(code+n,&alloc,8);n+=8;     /* mov rdx,allocation */
        code[n++]=0x48;code[n++]=0xB8;memcpy(code+n,&fn,8);n+=8;        /* mov rax,live_trace */
        code[n++]=0xFF;code[n++]=0xD0;                                  /* call rax */
        memcpy(code+n,pops,sizeof(pops));n+=sizeof(pops);
        code[n++]=0x48;code[n++]=0x83;code[n++]=0xC4;code[n++]=0x08;    /* add rsp,8 */
    }
    }else{
    if(mode!=1){
    code[n++]=0x49;code[n++]=0xBB;memcpy(code+n,&log,8);n+=8;
    static const unsigned char stores[7][3]={{0x49,0x89,0x7B},{0x49,0x89,0x73},{0x49,0x89,0x53},{0x49,0x89,0x4B},{0x4D,0x89,0x43},{0x4D,0x89,0x4B},{0x49,0x89,0x63}};
    for(unsigned i=0;i<7;i++){code[n++]=stores[i][0];code[n++]=stores[i][1];code[n++]=stores[i][2];code[n++]=(unsigned char)(i*8);}
    code[n++]=0x4C;code[n++]=0x8B;code[n++]=0x14;code[n++]=0x24;              /* mov r10,[rsp] */
    code[n++]=0x4D;code[n++]=0x89;code[n++]=0x53;code[n++]=0x40;              /* mov [r11+0x40],r10 */
    code[n++]=0x49;code[n++]=0xFF;code[n++]=0x43;code[n++]=0x38;              /* inc qword [r11+0x38] */
    code[n++]=0x49;code[n++]=0x89;code[n++]=0x6B;code[n++]=0x48;              /* mov [r11+0x48],rbp */
    /* No memory dereferences in this stub: at the VM invoke chokepoint the
     * argument registers (notably rsi) may hold non-pointer values and a
     * stray dereference faults. Object identity is captured from the rdi
     * value alone (trace table below); contents are read later via peek. */
    if(mode==0){
    /* VM invoke trace: hash rdi into a 1024-bucket object table at log+0x4000. */
    {
        uint32_t table=0x4000u;
        code[n++]=0x50;                                                            /* push rax */
        code[n++]=0x49;code[n++]=0x8B;code[n++]=0x03;                             /* mov rax,[r11]     (rdi) */
        code[n++]=0x48;code[n++]=0xC1;code[n++]=0xE8;code[n++]=0x04;              /* shr rax,4 */
        code[n++]=0x48;code[n++]=0x25;code[n++]=0xFF;code[n++]=0x03;code[n++]=0x00;code[n++]=0x00; /* and rax,0x3ff */
        code[n++]=0x48;code[n++]=0xC1;code[n++]=0xE0;code[n++]=0x04;              /* shl rax,4 */
        code[n++]=0x4D;code[n++]=0x8D;code[n++]=0x93;memcpy(code+n,&table,4);n+=4;/* lea r10,[r11+0x4000] */
        code[n++]=0x49;code[n++]=0x01;code[n++]=0xC2;                             /* add r10,rax */
        code[n++]=0x49;code[n++]=0x8B;code[n++]=0x03;                             /* mov rax,[r11] */
        code[n++]=0x49;code[n++]=0x89;code[n++]=0x02;                             /* mov [r10],rax */
        code[n++]=0x49;code[n++]=0xFF;code[n++]=0x42;code[n++]=0x08;              /* inc qword [r10+8] */
        code[n++]=0x58;                                                            /* pop rax */
    }
    }
    if(mode==5){
    /* Symbol-keyed trace: hash the interned symbol at [rdi+0x28] (whose
     * content is the invoked method/class path) into the 1024-bucket table.
     * Interned symbols are stable, so a RARE method invocation keeps its own
     * bucket instead of being overwritten by the ambient ~10k/s stream --
     * unlike mode 0, which keys by the per-frame instance pointer rdi and
     * therefore only ever remembers frequent occupants. The deref of rdi is
     * exactly as safe as the target's own first instruction (mov rax,[rdi+0x20]).
     * Bucket = {symbol, count, native}: we also record the native bound at
     * [rdi+0x20] (stride 0x20), turning this into a symbol->native map -- the
     * output of the load-time registration pass, readable at any time. */
    {
        code[n++]=0x50;                                                  /* push rax */
        code[n++]=0x52;                                                  /* push rdx */
        code[n++]=0x51;                                                  /* push rcx */
        code[n++]=0x49;code[n++]=0x8B;code[n++]=0x03;                    /* mov rax,[r11] (rdi) */
        code[n++]=0x48;code[n++]=0x85;code[n++]=0xC0;                    /* test rax,rax */
        code[n++]=0x74;code[n++]=0x37;                                   /* jz skip */
        code[n++]=0x48;code[n++]=0x8B;code[n++]=0x40;code[n++]=0x28;     /* mov rax,[rax+0x28] */
        code[n++]=0x48;code[n++]=0x85;code[n++]=0xC0;                    /* test rax,rax */
        code[n++]=0x74;code[n++]=0x2E;                                   /* jz skip */
        code[n++]=0x48;code[n++]=0x89;code[n++]=0xC2;                    /* mov rdx,rax */
        code[n++]=0x48;code[n++]=0xC1;code[n++]=0xEA;code[n++]=0x04;     /* shr rdx,4 */
        code[n++]=0x48;code[n++]=0x81;code[n++]=0xE2;code[n++]=0xFF;code[n++]=0x03;code[n++]=0x00;code[n++]=0x00;/* and rdx,0x3ff */
        code[n++]=0x48;code[n++]=0xC1;code[n++]=0xE2;code[n++]=0x05;     /* shl rdx,5 (stride 0x20) */
        {uint32_t table5=0x4000u;
        code[n++]=0x49;code[n++]=0x8D;code[n++]=0x8B;memcpy(code+n,&table5,4);n+=4;/* lea rcx,[r11+0x4000] */}
        code[n++]=0x48;code[n++]=0x01;code[n++]=0xD1;                    /* add rcx,rdx */
        code[n++]=0x48;code[n++]=0x89;code[n++]=0x01;                    /* mov [rcx],rax (symbol) */
        code[n++]=0x48;code[n++]=0xFF;code[n++]=0x41;code[n++]=0x08;     /* inc qword [rcx+8] */
        code[n++]=0x49;code[n++]=0x8B;code[n++]=0x03;                    /* mov rax,[r11] (rdi=record) */
        code[n++]=0x48;code[n++]=0x8B;code[n++]=0x40;code[n++]=0x20;     /* mov rax,[rax+0x20] (native) */
        code[n++]=0x48;code[n++]=0x89;code[n++]=0x41;code[n++]=0x10;     /* mov [rcx+0x10],rax */
        code[n++]=0x59;                                                  /* skip: pop rcx */
        code[n++]=0x5A;                                                  /* pop rdx */
        code[n++]=0x58;                                                  /* pop rax */
    }
    }
    if(mode==6){
    /* Symbol-watch: bump log+0x100 whenever the invoked object's interned
     * symbol [rdi+0x28] equals the configured watch value. Used to detect a
     * specific script method invocation (e.g. pushPage) by its stable symbol
     * identity, independent of the per-frame instance pointer. rdi is
     * dereferenced by the target itself, so the extra read is safe. */
    {
        code[n++]=0x50;                                                  /* push rax */
        code[n++]=0x51;                                                  /* push rcx */
        code[n++]=0x49;code[n++]=0x8B;code[n++]=0x03;                    /* mov rax,[r11] (rdi) */
        code[n++]=0x48;code[n++]=0x85;code[n++]=0xC0;                    /* test rax,rax */
        code[n++]=0x74;code[n++]=0x1A;                                   /* jz skip */
        code[n++]=0x48;code[n++]=0x8B;code[n++]=0x40;code[n++]=0x28;     /* mov rax,[rax+0x28] */
        code[n++]=0x48;code[n++]=0xB9;memcpy(code+n,&watch,8);n+=8;      /* mov rcx,watch */
        code[n++]=0x48;code[n++]=0x39;code[n++]=0xC8;                    /* cmp rax,rcx */
        code[n++]=0x75;code[n++]=0x07;                                   /* jne skip */
        code[n++]=0x49;code[n++]=0xFF;code[n++]=0x83;code[n++]=0x00;code[n++]=0x01;code[n++]=0x00;code[n++]=0x00;/* inc qword [r11+0x100] */
        code[n++]=0x59;                                                  /* skip: pop rcx */
        code[n++]=0x58;                                                  /* pop rax */
    }
    }
    }
    if(mode==3){
    /* Argument snapshot (crash-safe): copy the rcx argument array, which
     * holds the pushed page object and the event-source string pointers,
     * into the log buffer while it is still live. rcx is only read after
     * a bounds check, and nothing it points to is dereferenced here. */
    {
        code[n++]=0x4C;code[n++]=0x8B;code[n++]=0xD1;  /* mov r10,rcx */
        code[n++]=0x4C;code[n++]=0x2B;code[n++]=0xD4;  /* sub r10,rsp */
        code[n++]=0x49;code[n++]=0x81;code[n++]=0xFA;code[n++]=0x00;code[n++]=0x00;code[n++]=0x01;code[n++]=0x00;  /* cmp r10,0x10000 */
        code[n++]=0x72;code[n++]=0x05;  /* jb have */
        code[n++]=0xE9;code[n++]=0xAF;code[n++]=0x00;code[n++]=0x00;code[n++]=0x00;  /* jmp END */
        code[n++]=0x4C;code[n++]=0x8B;code[n++]=0x11;  /* mov r10,[rcx+0x0] */
        code[n++]=0x4D;code[n++]=0x89;code[n++]=0x93;code[n++]=0x00;code[n++]=0x01;code[n++]=0x00;code[n++]=0x00;  /* rcx[0] -> log+0x100 */
        code[n++]=0x4C;code[n++]=0x8B;code[n++]=0x51;code[n++]=0x08;  /* mov r10,[rcx+0x8] */
        code[n++]=0x4D;code[n++]=0x89;code[n++]=0x93;code[n++]=0x08;code[n++]=0x01;code[n++]=0x00;code[n++]=0x00;  /* rcx[1] -> log+0x108 */
        code[n++]=0x4C;code[n++]=0x8B;code[n++]=0x51;code[n++]=0x10;  /* mov r10,[rcx+0x10] */
        code[n++]=0x4D;code[n++]=0x89;code[n++]=0x93;code[n++]=0x10;code[n++]=0x01;code[n++]=0x00;code[n++]=0x00;  /* rcx[2] -> log+0x110 */
        code[n++]=0x4C;code[n++]=0x8B;code[n++]=0x51;code[n++]=0x18;  /* mov r10,[rcx+0x18] */
        code[n++]=0x4D;code[n++]=0x89;code[n++]=0x93;code[n++]=0x18;code[n++]=0x01;code[n++]=0x00;code[n++]=0x00;  /* rcx[3] -> log+0x118 */
        code[n++]=0x4C;code[n++]=0x8B;code[n++]=0x51;code[n++]=0x20;  /* mov r10,[rcx+0x20] */
        code[n++]=0x4D;code[n++]=0x89;code[n++]=0x93;code[n++]=0x20;code[n++]=0x01;code[n++]=0x00;code[n++]=0x00;  /* rcx[4] -> log+0x120 */
        code[n++]=0x4C;code[n++]=0x8B;code[n++]=0x51;code[n++]=0x28;  /* mov r10,[rcx+0x28] */
        code[n++]=0x4D;code[n++]=0x89;code[n++]=0x93;code[n++]=0x28;code[n++]=0x01;code[n++]=0x00;code[n++]=0x00;  /* rcx[5] -> log+0x128 */
        code[n++]=0x4C;code[n++]=0x8B;code[n++]=0x51;code[n++]=0x30;  /* mov r10,[rcx+0x30] */
        code[n++]=0x4D;code[n++]=0x89;code[n++]=0x93;code[n++]=0x30;code[n++]=0x01;code[n++]=0x00;code[n++]=0x00;  /* rcx[6] -> log+0x130 */
        code[n++]=0x4C;code[n++]=0x8B;code[n++]=0x51;code[n++]=0x38;  /* mov r10,[rcx+0x38] */
        code[n++]=0x4D;code[n++]=0x89;code[n++]=0x93;code[n++]=0x38;code[n++]=0x01;code[n++]=0x00;code[n++]=0x00;  /* rcx[7] -> log+0x138 */
        code[n++]=0x4C;code[n++]=0x8B;code[n++]=0x51;code[n++]=0x40;  /* mov r10,[rcx+0x40] */
        code[n++]=0x4D;code[n++]=0x89;code[n++]=0x93;code[n++]=0x40;code[n++]=0x01;code[n++]=0x00;code[n++]=0x00;  /* rcx[8] -> log+0x140 */
        code[n++]=0x4C;code[n++]=0x8B;code[n++]=0x51;code[n++]=0x48;  /* mov r10,[rcx+0x48] */
        code[n++]=0x4D;code[n++]=0x89;code[n++]=0x93;code[n++]=0x48;code[n++]=0x01;code[n++]=0x00;code[n++]=0x00;  /* rcx[9] -> log+0x148 */
        code[n++]=0x4C;code[n++]=0x8B;code[n++]=0x51;code[n++]=0x50;  /* mov r10,[rcx+0x50] */
        code[n++]=0x4D;code[n++]=0x89;code[n++]=0x93;code[n++]=0x50;code[n++]=0x01;code[n++]=0x00;code[n++]=0x00;  /* rcx[10] -> log+0x150 */
        code[n++]=0x4C;code[n++]=0x8B;code[n++]=0x51;code[n++]=0x58;  /* mov r10,[rcx+0x58] */
        code[n++]=0x4D;code[n++]=0x89;code[n++]=0x93;code[n++]=0x58;code[n++]=0x01;code[n++]=0x00;code[n++]=0x00;  /* rcx[11] -> log+0x158 */
        code[n++]=0x4C;code[n++]=0x8B;code[n++]=0x51;code[n++]=0x60;  /* mov r10,[rcx+0x60] */
        code[n++]=0x4D;code[n++]=0x89;code[n++]=0x93;code[n++]=0x60;code[n++]=0x01;code[n++]=0x00;code[n++]=0x00;  /* rcx[12] -> log+0x160 */
        code[n++]=0x4C;code[n++]=0x8B;code[n++]=0x51;code[n++]=0x68;  /* mov r10,[rcx+0x68] */
        code[n++]=0x4D;code[n++]=0x89;code[n++]=0x93;code[n++]=0x68;code[n++]=0x01;code[n++]=0x00;code[n++]=0x00;  /* rcx[13] -> log+0x168 */
        code[n++]=0x4C;code[n++]=0x8B;code[n++]=0x51;code[n++]=0x70;  /* mov r10,[rcx+0x70] */
        code[n++]=0x4D;code[n++]=0x89;code[n++]=0x93;code[n++]=0x70;code[n++]=0x01;code[n++]=0x00;code[n++]=0x00;  /* rcx[14] -> log+0x170 */
        code[n++]=0x4C;code[n++]=0x8B;code[n++]=0x51;code[n++]=0x78;  /* mov r10,[rcx+0x78] */
        code[n++]=0x4D;code[n++]=0x89;code[n++]=0x93;code[n++]=0x78;code[n++]=0x01;code[n++]=0x00;code[n++]=0x00;  /* rcx[15] -> log+0x178 */
    }
    }
    if(mode==4){
    /* Invoke watch: bump log+0x100 whenever the invoked rdi equals the
     * configured watch address. Detects a specific VM object being invoked
     * at the chokepoint regardless of how its instance address changes. */
    {
        uintptr_t wv=watch;
        code[n++]=0x50;                                                          /* push rax (preserve) */
        code[n++]=0x49;code[n++]=0xBA;memcpy(code+n,&wv,8);n+=8;                  /* mov r10,watch */
        code[n++]=0x4D;code[n++]=0x39;code[n++]=0x13;                            /* cmp [r11],r10 (rdi) */
        code[n++]=0x0F;code[n++]=0x94;code[n++]=0xC0;                            /* sete al */
        code[n++]=0x0F;code[n++]=0xB6;code[n++]=0xC0;                            /* movzx eax,al */
        code[n++]=0x49;code[n++]=0x01;code[n++]=0x83;code[n++]=0x00;code[n++]=0x01;code[n++]=0x00;code[n++]=0x00; /* add [r11+0x100],rax */
        code[n++]=0x58;                                                          /* pop rax */
    }
    }
    if(mode==7){
    /* Bind census: at the bind-native API the name arrives in rsi (log[8]).
     * Hash rsi into the 1024-bucket table so every distinct interned symbol
     * bound through this API is recorded, independent of the context pointer
     * rdi (constant here). Lets us prove whether the credit-open symbol is
     * ever bound to a native. rsi is only stored, never dereferenced. */
    {
        uint32_t table7=0x4000u;
        code[n++]=0x50;                                                          /* push rax */
        code[n++]=0x49;code[n++]=0x8B;code[n++]=0x43;code[n++]=0x08;             /* mov rax,[r11+8]  (rsi=name) */
        code[n++]=0x48;code[n++]=0xC1;code[n++]=0xE8;code[n++]=0x04;             /* shr rax,4 */
        code[n++]=0x48;code[n++]=0x25;code[n++]=0xFF;code[n++]=0x03;code[n++]=0x00;code[n++]=0x00; /* and rax,0x3ff */
        code[n++]=0x48;code[n++]=0xC1;code[n++]=0xE0;code[n++]=0x04;             /* shl rax,4 */
        code[n++]=0x4D;code[n++]=0x8D;code[n++]=0x93;memcpy(code+n,&table7,4);n+=4;/* lea r10,[r11+0x4000] */
        code[n++]=0x49;code[n++]=0x01;code[n++]=0xC2;                            /* add r10,rax */
        code[n++]=0x49;code[n++]=0x8B;code[n++]=0x43;code[n++]=0x08;             /* mov rax,[r11+8] */
        code[n++]=0x49;code[n++]=0x89;code[n++]=0x02;                            /* mov [r10],rax */
        code[n++]=0x49;code[n++]=0xFF;code[n++]=0x42;code[n++]=0x08;             /* inc qword [r10+8] */
        code[n++]=0x58;                                                          /* pop rax */
    }
    }
    if(mode==8){
    /* Override (PoC): when the invoked object's interned symbol [rdi+0x28]
     * equals the configured watch value (the credits-icon key handler), queue
     * the native number pad by writing PROMPT_QUEUED into the live mailbox --
     * but only when the prompt is idle, so an in-progress prompt is never
     * clobbered. Fires synchronously on the press, in-process, no polling.
     * The original handler still runs (suppression is a later step). */
    {
        int32_t disp=(int32_t)offsetof(LiveMailbox,prompt_state)-(int32_t)0x7000;
        code[n++]=0x50;                                                  /* push rax */
        code[n++]=0x51;                                                  /* push rcx */
        code[n++]=0x49;code[n++]=0x8B;code[n++]=0x03;                    /* mov rax,[r11] (rdi) */
        code[n++]=0x48;code[n++]=0x85;code[n++]=0xC0;                    /* test rax,rax */
        code[n++]=0x74;code[n++]=0x2F;                                   /* jz skip */
        code[n++]=0x48;code[n++]=0x8B;code[n++]=0x40;code[n++]=0x28;     /* mov rax,[rax+0x28] */
        code[n++]=0x48;code[n++]=0xB9;memcpy(code+n,&watch,8);n+=8;      /* mov rcx,watch */
        code[n++]=0x48;code[n++]=0x39;code[n++]=0xC8;                    /* cmp rax,rcx */
        code[n++]=0x75;code[n++]=0x1C;                                   /* jne skip */
        code[n++]=0x41;code[n++]=0x83;code[n++]=0xBB;memcpy(code+n,&disp,4);n+=4;code[n++]=0x00;/* cmp dword [r11+disp],0 */
        code[n++]=0x75;code[n++]=0x12;                                   /* jne skip */
        code[n++]=0x41;code[n++]=0xC7;code[n++]=0x83;memcpy(code+n,&disp,4);n+=4;code[n++]=0x01;code[n++]=0x00;code[n++]=0x00;code[n++]=0x00;/* mov dword [r11+disp],1 (PROMPT_QUEUED) */
        code[n++]=0x49;code[n++]=0xFF;code[n++]=0x83;code[n++]=0x00;code[n++]=0x01;code[n++]=0x00;code[n++]=0x00;/* inc qword [r11+0x100] */
        code[n++]=0x59;                                                  /* skip: pop rcx */
        code[n++]=0x58;                                                  /* pop rax */
    }
    }
    }
    memcpy(code+n,orig,displaced);n+=displaced;
    code[n++]=0x49;code[n++]=0xBB;uintptr_t resume=target+displaced;memcpy(code+n,&resume,8);n+=8;
    code[n++]=0x41;code[n++]=0xFF;code[n++]=0xE3;                             /* jmp r11 */
    if(n>LIVE_PROBE_CODE_STRIDE)return -1;
    LiveProbeRecord snap;memset(&snap,0,sizeof(snap));
    snap.target=target;snap.displaced=displaced;snap.mode=(uint32_t)mode;snap.slot=slot;snap.watch=watch;snap.active=1;
    memcpy(snap.original,orig,displaced);
    unsigned char raw[LIVE_PROBE_RECORD_STRIDE];memset(raw,0,sizeof(raw));memcpy(raw,&snap,sizeof(snap));
    if(verified_write(pid,record,raw,sizeof(raw)))return -1;
    if(verified_write(pid,stub,code,n))return -1;
    if(slot==0&&mode!=9){
        unsigned char zero[0x400];memset(zero,0,sizeof(zero));if(verified_write(pid,log,zero,sizeof(zero)))return -1;
        static unsigned char zobj[0x8000];if(verified_write(pid,log+0x4000,zobj,sizeof(zobj)))return -1;
    }
    unsigned char patch[0x40];
    patch[0]=0xFF;patch[1]=0x25;patch[2]=0;patch[3]=0;patch[4]=0;patch[5]=0;memcpy(patch+6,&stub,8);
    for(uint32_t i=14;i<displaced;i++)patch[i]=0x90;
    if(kernel_mprotect(pid,target&~(uintptr_t)0x3fff,0x4000,PROT_READ|PROT_WRITE|PROT_EXEC))return -1;
    if(verified_write(pid,target,patch,displaced))return -1;
    return 0;
}
int gt7_live_probe_install(uintptr_t target,uint32_t displaced,int mode,uintptr_t watch){
    return gt7_live_probe_install_slot(target,displaced,mode,watch,0,LIVE_TRACE_CFG(0,1,0));
}
int gt7_live_probe_remove_slot(uint32_t slot){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(slot>=LIVE_PROBE_SLOTS)return -1;
    if(credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled||!state.allocation)return -1;
    LiveProbeRecord snap;
    if(read_mem(pid,probe_record(state.allocation,slot),&snap,sizeof(snap)))return -1;
    if(!snap.active||!snap.target||snap.displaced<14||snap.displaced>0x40)return -1;
    if(kernel_mprotect(pid,snap.target&~(uintptr_t)0x3fff,0x4000,PROT_READ|PROT_WRITE|PROT_EXEC))return -1;
    if(verified_write(pid,snap.target,snap.original,snap.displaced))return -1;
    uint64_t zero=0;
    if(verified_write(pid,probe_record(state.allocation,slot),&zero,8))return -1;
    return 0;
}
int gt7_live_probe_remove(void){
    unsigned removed=0;
    for(uint32_t slot=0;slot<LIVE_PROBE_SLOTS;slot++)if(!gt7_live_probe_remove_slot(slot))removed++;
    return removed?0:-1;
}
/* Symbol names for the pointers the trace records reference. The chain is
 * symbol record +0x10 -> name record +0x00 -> NUL-terminated text. Pointers
 * are stable for the life of a game process, so the cache is keyed by pointer
 * and dropped when the game process changes. */
#define LIVE_NAME_CACHE 256
typedef struct {uintptr_t ptr;char name[48];} LiveNameEntry;
static LiveNameEntry live_name_cache[LIVE_NAME_CACHE];
static pid_t live_name_pid;
static const char *live_name(pid_t pid,uintptr_t sym){
    if(!sym)return "";
    if(live_name_pid!=pid){memset(live_name_cache,0,sizeof(live_name_cache));live_name_pid=pid;}
    uint32_t hash=(uint32_t)((sym>>4)&(LIVE_NAME_CACHE-1));
    for(uint32_t i=0;i<LIVE_NAME_CACHE;i++){
        LiveNameEntry *e=&live_name_cache[(hash+i)&(LIVE_NAME_CACHE-1)];
        if(e->ptr==sym)return e->name;
        if(!e->ptr){
            uintptr_t name_record=0,string_ptr=0;
            e->ptr=sym;e->name[0]=0;
            if(!read_mem(pid,sym+0x10,&name_record,8)&&name_record&&
               !read_mem(pid,name_record,&string_ptr,8)&&string_ptr){
                unsigned char raw[47];
                if(!read_mem(pid,string_ptr,raw,sizeof(raw))){
                    unsigned j=0;
                    for(;j<sizeof(raw)&&raw[j];j++)e->name[j]=(raw[j]>=32&&raw[j]<127)?(char)raw[j]:'?';
                    e->name[j]=0;
                }
            }
            return e->name;
        }
    }
    return "";
}
/* Initialise (or re-initialise) the trace ring. The epoch is bumped so records
 * still sitting in the ring from an earlier run are rejected instead of being
 * reported as new. */
int gt7_live_trace_init(void){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled||!state.allocation)return -1;
    uintptr_t hdr=state.allocation+LIVE_TRACE_HEADER;
    LiveTraceHeader old;
    uint32_t epoch=0;
    if(!read_mem(pid,hdr,&old,sizeof(old))&&old.magic==LIVE_TRACE_MAGIC)epoch=old.epoch;
    LiveTraceHeader next;memset(&next,0,sizeof(next));
    next.magic=LIVE_TRACE_MAGIC;next.mask=LIVE_TRACE_MASK;next.sites=LIVE_PROBE_SLOTS;next.epoch=epoch+1;
    if(verified_write(pid,hdr,&next,sizeof(next)))return -1;
    return 0;
}
/* Compact JSON for every record written since *since. A symbol dictionary is
 * emitted alongside so a recurring pointer costs one string, not one per
 * event. *since is advanced past the batch. */
/* Copy a symbol name into a JSON string body. Names come from the game's
 * interned string table and are normally plain identifiers, but a stray
 * quote or control byte must not break the document. */
static uint32_t trace_json_name(char *dst,uint32_t size,const char *src){
    uint32_t at=0;
    for(;src&&*src;src++){
        unsigned char c=(unsigned char)*src;
        const char *esc=0;
        if(c=='"')esc="\\\"";
        else if(c=='\\')esc="\\\\";
        else if(c=='\n')esc="\\n";
        else if(c=='\r')esc="\\r";
        else if(c=='\t')esc="\\t";
        else if(c<32)esc="?";
        if(esc){
            uint32_t n=(uint32_t)strlen(esc);
            if(at+n>=size)break;
            memcpy(dst+at,esc,n);at+=n;
        }else{
            if(at+1>=size)break;
            dst[at++]=(char)c;
        }
    }
    if(size)dst[at<size?at:size-1]=0;
    return at;
}
int gt7_live_trace_report(char *out,uint32_t size,uint64_t *since,uint32_t max){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(!out||!since||size<512)return -1;
    if(credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled||!state.allocation)return -1;
    if(!max||max>LIVE_TRACE_MAX)max=LIVE_TRACE_MAX;
    uintptr_t hdr=state.allocation+LIVE_TRACE_HEADER;
    LiveTraceHeader h;
    if(read_mem(pid,hdr,&h,sizeof(h)))return -1;
    if(h.magic!=LIVE_TRACE_MAGIC){
        snprintf(out,size,"{\"ready\":false,\"count\":0,\"since\":%llu,\"next\":%llu,\"lost\":0,\"syms\":{},\"events\":[]}",
            (unsigned long long)*since,(unsigned long long)*since);
        return 0;
    }
    uint64_t count=h.count,cur=*since,lost=0;
    if(cur>count)cur=count;
    if(count-cur>LIVE_TRACE_COUNT){lost=count-cur-LIVE_TRACE_COUNT;cur=count-LIVE_TRACE_COUNT;}
    uint64_t available=count-cur;
    uint32_t take=(uint32_t)(available<max?available:max);
    static LiveTraceRecord records[LIVE_TRACE_MAX];
    if(take){
        uintptr_t ring=state.allocation+LIVE_TRACE_RING;
        uint32_t first=(uint32_t)(cur&LIVE_TRACE_MASK);
        uint32_t head=(first+take>LIVE_TRACE_COUNT)?(LIVE_TRACE_COUNT-first):take;
        if(read_mem(pid,ring+(uintptr_t)first*sizeof(LiveTraceRecord),records,(size_t)head*sizeof(LiveTraceRecord)))return -1;
        if(take>head&&read_mem(pid,ring,records+head,(size_t)(take-head)*sizeof(LiveTraceRecord)))return -1;
    }
    uintptr_t syms[128];uint32_t nsym=0;
    for(uint32_t i=0;i<take;i++){
        const LiveTraceRecord *rec=&records[i];
        if(rec->seq!=cur+i+1||rec->epoch!=h.epoch){lost++;continue;}
        if(!rec->sym)continue;
        int seen=0;
        for(uint32_t k=0;k<nsym;k++)if(syms[k]==rec->sym){seen=1;break;}
        if(!seen&&nsym<128)syms[nsym++]=rec->sym;
    }
    int at=snprintf(out,size,"{\"ready\":true,\"count\":%llu,\"since\":%llu,\"next\":%llu,\"lost\":%llu,\"epoch\":%u,\"syms\":{",
        (unsigned long long)count,(unsigned long long)cur,(unsigned long long)(cur+take),
        (unsigned long long)lost,(unsigned)h.epoch);
    for(uint32_t i=0;i<nsym;i++){
        if(at<0||(uint32_t)at>=size)break;
        char name[96];trace_json_name(name,sizeof(name),live_name(pid,syms[i]));
        at+=snprintf(out+at,(size_t)(size-(uint32_t)at),"%s\"%s\"",i?",":"",name);
    }
    if(at>=0&&(uint32_t)at<size)at+=snprintf(out+at,(size_t)(size-(uint32_t)at),"},");
    if(at>=0&&(uint32_t)at<size)at+=snprintf(out+at,(size_t)(size-(uint32_t)at),"\"events\":[");
    uint32_t emitted=0;
    for(uint32_t i=0;i<take;i++){
        const LiveTraceRecord *rec=&records[i];
        if(rec->seq!=cur+i+1||rec->epoch!=h.epoch)continue;
        if(at<0||(uint32_t)at>=size-192)break;
        int idx=-1;
        if(rec->sym)for(uint32_t k=0;k<nsym;k++)if(syms[k]==rec->sym){idx=(int)k;break;}
        at+=snprintf(out+at,(size_t)(size-(uint32_t)at),"%s[%u,%llu,%d,\"0x%llx\",\"0x%llx\",\"0x%llx\",\"0x%llx\"]",
            emitted?",":"",(unsigned)rec->site,(unsigned long long)rec->tsc,idx,
            (unsigned long long)rec->rdi,(unsigned long long)rec->rsi,(unsigned long long)rec->ret,
            (unsigned long long)rec->native);
        emitted++;
    }
    if(at>=0&&(uint32_t)at<size)at+=snprintf(out+at,(size_t)(size-(uint32_t)at),"],\"emitted\":%u}",emitted);
    *since=cur+take;
    return 0;
}
/* Install the default observation set and initialise the ring. Sites are
 * addressed as RVAs from the module image base so the set survives a moving
 * base. A site whose slot is already hooked is restored first. */
int gt7_live_hooks_auto(void){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled||!state.allocation)return -1;
    if(gt7_live_trace_init())return -1;
    unsigned installed=0;
    for(uint32_t i=0;i<LIVE_TRACE_SITE_COUNT;i++){
        const LiveTraceSite *site=&trace_sites[i];
        uint64_t cfg=LIVE_TRACE_CFG(site->slot,site->deref,site->sample);
        gt7_live_probe_remove_slot(site->slot);
        if(!gt7_live_probe_install_slot(base+site->rva,site->displaced,9,0,site->slot,cfg))installed++;
    }
    return installed?0:-1;
}
int gt7_live_hooks_list(char *out,uint32_t size){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(!out||size<128)return -1;
    if(credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled||!state.allocation)return -1;
    LiveTraceHeader h;
    int trace_ready=!read_mem(pid,state.allocation+LIVE_TRACE_HEADER,&h,sizeof(h))&&h.magic==LIVE_TRACE_MAGIC;
    int at=snprintf(out,size,"{\"traceReady\":%s,\"count\":%llu,\"slots\":[",
        trace_ready?"true":"false",(unsigned long long)(trace_ready?h.count:0));
    unsigned shown=0;
    for(uint32_t slot=0;slot<LIVE_PROBE_SLOTS;slot++){
        LiveProbeRecord snap;
        if(read_mem(pid,probe_record(state.allocation,slot),&snap,sizeof(snap)))continue;
        if(!snap.active||!snap.target)continue;
        const char *label="";
        uint32_t rva=(uint32_t)(snap.target-base);
        for(uint32_t i=0;i<LIVE_TRACE_SITE_COUNT;i++)if(trace_sites[i].rva==rva)label=trace_sites[i].label;
        if(at<0||(uint32_t)at>=size-192)break;
        at+=snprintf(out+at,(size_t)(size-(uint32_t)at),
            "%s{\"slot\":%u,\"rva\":\"0x%x\",\"target\":\"0x%llx\",\"len\":%u,\"mode\":%u,\"label\":\"%s\"}",
            shown?",":"",(unsigned)slot,(unsigned)rva,(unsigned long long)snap.target,
            (unsigned)snap.displaced,(unsigned)snap.mode,label);
        shown++;
    }
    if(at>=0&&(uint32_t)at<size)at+=snprintf(out+at,(size_t)(size-(uint32_t)at),"],\"installed\":%u}",shown);
    return 0;
}
int gt7_live_probe_read(uintptr_t *words,uint32_t count){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(!words||count<9||credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled||!state.allocation)return -1;
    return read_mem(pid,state.allocation+LIVE_PROBE_LOG,words,count*8)?-1:0;
}
int gt7_live_probe_history(uintptr_t *words,uint32_t count,uint32_t *next){
    pid_t pid;uintptr_t base,address;LiveMailbox state;uint32_t cursor=0;
    if(!words||count<LIVE_PROBE_RING_COUNT*LIVE_PROBE_RING_WORDS||credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled||!state.allocation)return -1;
    if(read_mem(pid,state.allocation+LIVE_PROBE_LOG+0x80,&cursor,sizeof(cursor))||
       read_mem(pid,state.allocation+LIVE_PROBE_LOG+LIVE_PROBE_RING,words,LIVE_PROBE_RING_COUNT*LIVE_PROBE_RING_WORDS*8))return -1;
    if(next)*next=cursor;
    return 0;
}
int gt7_live_menu_call(uintptr_t target,uintptr_t receiver,unsigned argc,uintptr_t args,unsigned *result){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(result)*result=0;
    if(argc!=0||target!=UINT64_C(0x001d63ce0)||receiver<0x10000||receiver>UINT64_C(0x7fffffffffff))return -1;
    if(credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled)return -1;
    if(state.menu_call_state==1||state.menu_call_state==2)return -2;
    uint32_t zero=0,queued=1;
    if(verified_write(pid,address+offsetof(LiveMailbox,menu_call_target),&target,8)||
       verified_write(pid,address+offsetof(LiveMailbox,menu_call_receiver),&receiver,8)||
       verified_write(pid,address+offsetof(LiveMailbox,menu_call_argv),&args,8)||
       verified_write(pid,address+offsetof(LiveMailbox,menu_call_argc),&argc,4)||
       verified_write(pid,address+offsetof(LiveMailbox,menu_call_result),&zero,4)||
       verified_write(pid,address+offsetof(LiveMailbox,menu_call_state),&queued,4))return -1;
    for(unsigned i=0;i<150;i++){
        usleep(10000);
        if(read_mem(pid,address,&state,sizeof(state)))return -3;
        if(state.menu_call_state==3||state.menu_call_state==4||state.menu_call_state==5){if(result)*result=state.menu_call_result;return state.menu_call_state==3?0:-1;}
    }
    return -3;
}
int gt7_live_car_pointers(uintptr_t *implementation,uintptr_t *expanded,uintptr_t *style,uint32_t *model_code,uint32_t *sequence){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled)return -1;
    if(implementation)*implementation=(uintptr_t)state.car_implementation;
    if(expanded)*expanded=(uintptr_t)state.car_expanded;
    if(style)*style=(uintptr_t)state.car_style;
    if(model_code)*model_code=state.car_model_code;
    if(sequence)*sequence=state.car_sequence;
    return 0;
}
#define GT7_SNAPSHOT_MAX 0x20000000u
static unsigned char *snapshot_data;
static uintptr_t snapshot_address;
static unsigned snapshot_length;
int gt7_live_snapshot(uintptr_t address,unsigned length){
    pid_t pid;uintptr_t base;
    if(!length||length>GT7_SNAPSHOT_MAX||credits_game(&pid,&base))return -2;
    unsigned char *buffer=malloc(length);if(!buffer)return -1;
    /* The save region is a set of mappings with holes; read in chunks and
     * zero-fill anything unmapped so the snapshot never spans a fault. */
    unsigned read_ok=0;
    for(unsigned off=0;off<length;){
        unsigned want=length-off;if(want>0x100000)want=0x100000;
        if(read_mem(pid,address+off,buffer+off,want))memset(buffer+off,0,want);else read_ok+=want;
        off+=want;
    }
    if(!read_ok){free(buffer);return -2;}
    free(snapshot_data);snapshot_data=buffer;snapshot_address=address;snapshot_length=length;
    return 0;
}
void gt7_live_snapshot_clear(void){free(snapshot_data);snapshot_data=NULL;snapshot_address=0;snapshot_length=0;}
static int snapshot_emit(pid_t pid,char *out,unsigned *used,unsigned size,uintptr_t base,const unsigned char *old,unsigned start,unsigned end,unsigned *runs,unsigned *bytes){
    static const char d[]="0123456789abcdef";
    while(start<end){
        unsigned len=end-start;if(len>256)len=256;
        unsigned need=len*4+80;
        if(*used+need>=size)return -1;
        unsigned char fresh[256];
        if(read_mem(pid,base+start,fresh,len))return -1;
        *used+=(unsigned)snprintf(out+*used,size-*used,"%s{\"off\":\"0x%x\",\"len\":%u,\"old\":\"",(*runs?",":""),start,len);
        for(unsigned i=0;i<len;i++){out[(*used)++]=d[old[start+i]>>4];out[(*used)++]=d[old[start+i]&15];}
        memcpy(out+*used,"\",\"new\":\"",9);*used+=9;
        for(unsigned i=0;i<len;i++){out[(*used)++]=d[fresh[i]>>4];out[(*used)++]=d[fresh[i]&15];}
        memcpy(out+*used,"\"}",2);*used+=2;
        (*runs)++;*bytes+=len;start+=len;
    }
    return 0;
}
int gt7_live_snapshot_diff(char *out,unsigned size,unsigned *changed_bytes,unsigned *changed_runs){
    pid_t pid;uintptr_t base;
    if(!out||!size||!snapshot_data||credits_game(&pid,&base))return -2;
    unsigned used=0,runs=0,bytes=0;int truncated=0;
    int n=snprintf(out,size,"{\"addr\":\"0x%llx\",\"len\":%u,\"windows\":[",(unsigned long long)snapshot_address,snapshot_length);
    if(n<0||(unsigned)n>=size)return -2;used=(unsigned)n;
    unsigned char chunk[65536];const unsigned GAP=8,MAXWINDOWS=2048;
    unsigned run_start=0,run_end=0;int in_run=0;
    for(unsigned off=0;off<snapshot_length&&!truncated;){
        unsigned want=snapshot_length-off;if(want>sizeof(chunk))want=(unsigned)sizeof(chunk);
        if(read_mem(pid,snapshot_address+off,chunk,want)){
            if(in_run){if(runs>=MAXWINDOWS||snapshot_emit(pid,out,&used,size,snapshot_address,snapshot_data,run_start,run_end,&runs,&bytes))truncated=1;in_run=0;}
            off+=want;continue;
        }
        for(unsigned i=0;i<want;i++){
            unsigned p=off+i;
            if(chunk[i]!=snapshot_data[p]){if(!in_run){run_start=p;in_run=1;}run_end=p+1;}
            else if(in_run&&p-run_end>=GAP){
                if(runs>=MAXWINDOWS||snapshot_emit(pid,out,&used,size,snapshot_address,snapshot_data,run_start,run_end,&runs,&bytes)){truncated=1;in_run=0;break;}
                in_run=0;
            }
        }
        off+=want;
    }
    if(!truncated&&in_run&&(runs>=MAXWINDOWS||snapshot_emit(pid,out,&used,size,snapshot_address,snapshot_data,run_start,run_end,&runs,&bytes)))truncated=1;
    if(changed_runs)*changed_runs=runs;
    if(changed_bytes)*changed_bytes=bytes;
    if(used<size)used+=(unsigned)snprintf(out+used,size-used,",\"runs\":%u,\"changed\":%u,\"truncated\":%s}",runs,bytes,truncated?"true":"false");
    return (int)used;
}
int gt7_live_mapinfo(char *out,uint32_t size){
    pid_t pid;uintptr_t base;
    if(!out||!size||credits_game(&pid,&base))return -1;
    int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_VMMAP,(int)pid};
    size_t need=0;
    if(sysctl(mib,4,NULL,&need,NULL,0)||!need)return -2;
    unsigned char *data=malloc(need);if(!data)return -2;
    if(sysctl(mib,4,data,&need,NULL,0)){free(data);return -2;}
    uint32_t used=0;
    for(size_t at=0;at+offsetof(struct kinfo_vmentry,kve_path)<=need;){
        struct kinfo_vmentry *v=(void *)(data+at);
        int record=v->kve_structsize;
        if(record<(int)offsetof(struct kinfo_vmentry,kve_path)||record<=0||at+(size_t)record>need)break;
        const char *path=(record>(int)offsetof(struct kinfo_vmentry,kve_path))?(const char *)v->kve_path:"";
        int n=snprintf(out+used,size-used,"%llx-%llx %c%c%c%s%s\n",
            (unsigned long long)v->kve_start,(unsigned long long)v->kve_end,
            (v->kve_protection&1)?'r':'-',(v->kve_protection&2)?'w':'-',(v->kve_protection&4)?'x':'-',
            path[0]?" ":"",path);
        if(n<0||(uint32_t)n>=size-used)break;
        used+=(uint32_t)n;
        at+=(size_t)record;
    }
    out[used]=0;
    return used?(int)used:-1;
}
int gt7_live_find(uintptr_t address,uint32_t length,const char *needle,uint32_t needle_length,uintptr_t *hits,uint32_t max_hits,uint32_t *out_count){
    pid_t pid;uintptr_t base;
    if(!needle||!needle_length||!hits||!out_count||!length||credits_game(&pid,&base))return -1;
    uint32_t remaining=length,count=0;uintptr_t a=address;unsigned char chunk[4096];
    while(remaining&&count<max_hits){
        uint32_t want=remaining<sizeof(chunk)?remaining:(uint32_t)sizeof(chunk);
        if(read_mem(pid,a,chunk,want)){uintptr_t step=0x1000-(a&0xfff);if(step>remaining)break;a+=step;remaining-=(uint32_t)step;continue;}
        for(uint32_t i=0;i+needle_length<=want&&count<max_hits;i++)
            if(!memcmp(chunk+i,needle,needle_length))hits[count++]=a+i;
        a+=want;remaining-=want;
    }
    *out_count=count;return 0;
}
int gt7_live_peek(uintptr_t address,void *buffer,uint32_t length){
    pid_t pid;uintptr_t base;
    if(!buffer||!length||credits_game(&pid,&base))return -1;
    return read_mem(pid,address,buffer,length)?-1:0;
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
int gt7_engine_swap_read(Gt7EngineSwap *out){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(!out||credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled||!state.heartbeat||state.car_state!=CAR_READY)return -1;
    *out=(Gt7EngineSwap){pid,state.car_model_code,state.engine_swap_category};return 0;
}
int gt7_engine_swap_set(const Gt7EngineSwap *expected,uint8_t category,int unlock){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(!expected||category>2||credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled||!state.heartbeat)return -1;
    if(state.engine_state==ENGINE_QUEUED||state.engine_state==ENGINE_RUNNING)return -3;
    if((state.car_state!=CAR_READY&&state.car_state!=CAR_QUEUED)||state.car_model_code!=expected->model_code||state.engine_swap_category!=expected->category)return -2;
    state.engine_expected_model_code=expected->model_code;state.engine_expected_category=expected->category;state.engine_requested_category=category;state.engine_unlock=unlock?1:0;
    if(verified_write(pid,address+offsetof(LiveMailbox,engine_expected_model_code),&state.engine_expected_model_code,8))return -1;
    uint32_t queued=ENGINE_QUEUED;if(verified_write(pid,address+offsetof(LiveMailbox,engine_state),&queued,4))return -1;
    for(unsigned i=0;i<150;i++){usleep(10000);if(read_mem(pid,address,&state,sizeof(state)))return -3;
        if(state.engine_state==ENGINE_DONE)return state.engine_swap_category==category?0:-1;
        if(state.engine_state==ENGINE_STALE)return -2;
        if(state.engine_state==ENGINE_INVALID||state.engine_state==ENGINE_UNAVAILABLE)return -1;}
    return -3;
}
/* HUD visibility: publish the set of MENU widget keys the bridge should hide.
 * The keys are written before hud_count is raised so the bridge never reads a
 * partial list, and hud_sequence is bumped so the caller can confirm the
 * generation that was applied. */
int gt7_hud_read(Gt7Hud *out){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(!out||credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled||!state.heartbeat)return -1;
    memset(out,0,sizeof(*out));
    out->pid=pid;out->count=state.hud_count;out->applied=state.hud_applied;out->sequence=state.hud_sequence;
    if(out->count>LIVE_HUD_MAX)out->count=LIVE_HUD_MAX;
    memcpy(out->entries,state.hud_entries,sizeof(out->entries));
    return 0;
}
int gt7_hud_set(const char (*entries)[LIVE_HUD_PATH],uint32_t count){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(count>LIVE_HUD_MAX||(!entries&&count))return -1;
    if(credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled||!state.heartbeat)return -1;
    /* Write the keys first and raise the count last: the bridge never reads a
     * half-updated list, and a transient count/key mismatch only delays a
     * change by one frame. */
    for(uint32_t i=0;i<count;i++)
        if(verified_write(pid,address+offsetof(LiveMailbox,hud_entries)+i*LIVE_HUD_PATH,entries[i],LIVE_HUD_PATH))return -1;
    uint32_t sequence=state.hud_sequence+1;
    if(verified_write(pid,address+offsetof(LiveMailbox,hud_sequence),&sequence,4))return -1;
    if(verified_write(pid,address+offsetof(LiveMailbox,hud_count),&count,4))return -1;
    return 0;
}
/* ---------------------------------------------------------------------------
 * Live widget-tree dump for the web UI. The payload walks the game's MENU tree
 * through the same offsets the bridge uses (hud_tree.h) and reports every named
 * widget with its path, depth and current hidden state. Paths are built exactly
 * as the bridge builds them, so a path selected in the UI matches at runtime.
 */
#define HUD_TREE_MAX_NODES 2048
#define HUD_TREE_MAX_VISITS 20000
typedef struct {
    pid_t pid;uintptr_t base;
    int (*emit)(void *context,const Gt7HudNode *node);
    void *context;int count,visited;
} HudTreeWalk;
/* Read a widget name. Returns 0 when there is none, 1 for an inline name and 2
 * for a heap-allocated one: the caller needs the distinction to keep building
 * the compatibility path (see hud_tree_node). The name object is a
 * small-string-optimised string, so a long name is reached through the pointer
 * the object holds rather than sitting at the object itself. Reading only the
 * inline form is what made the SteerAssist_icon / BrakeAssist_icon widgets look
 * unnamed and their layers collide. */
static int hud_read_name(pid_t pid,uintptr_t p,char *out){
    if(!hud_ptr(p))return 0;
    unsigned char blk[HUD_OFF_NAME_CAP+8];
    if(read_mem(pid,p,blk,sizeof(blk)))return 0;
    const unsigned char *raw=blk;
    unsigned char heap[LIVE_HUD_NAME];
    uint64_t len=*(uint64_t *)(blk+HUD_OFF_NAME_LEN),cap=*(uint64_t *)(blk+HUD_OFF_NAME_CAP);
    if(len>HUD_NAME_INLINE&&cap>=len&&len<LIVE_HUD_NAME){
        uintptr_t text=*(uintptr_t *)blk;
        if(!hud_ptr(text))return 0;
        if(read_mem(pid,text,heap,sizeof(heap)))return 0;
        raw=heap;
    }
    unsigned i=0;for(;i<LIVE_HUD_NAME-1;i++){if(!raw[i])break;out[i]=(char)raw[i];}
    out[i]=0;
    if(!hud_name_ok(out,i))return 0;
    return (len>HUD_NAME_INLINE)?2:1;
}
static void hud_tree_children(HudTreeWalk *w,uintptr_t node,unsigned depth,HudPath *path,HudPath *alias);
/* `alias` is the path the old reader would have built: the same path with the
 * heap-allocated widget names left out. It is published alongside the real path
 * so a group saved before those names were decoded still recognises its
 * widgets; the bridge matches on it too. */
static void hud_tree_node(HudTreeWalk *w,uintptr_t node,unsigned depth,HudPath *path,HudPath *alias){
    if(depth>HUD_MAX_DEPTH||w->count>=HUD_TREE_MAX_NODES||++w->visited>HUD_TREE_MAX_VISITS)return;
    unsigned char blk[HUD_OFF_RECT+HUD_RECT_LEN];
    if(read_mem(w->pid,node,blk,sizeof(blk)))return;
    unsigned save=path->len,asave=alias->len;
    char nm[LIVE_HUD_NAME];
    int kind=hud_read_name(w->pid,*(uintptr_t *)(blk+HUD_OFF_NAME),nm);
    if(kind){
        /* The bridge skips a subtree whose path will not fit, so the dump must
         * skip it too or the UI would offer unselectable paths. */
        if(!hud_path_push(path,nm))return;
        if(kind==1&&!hud_path_push(alias,nm)){alias->len=asave;alias->buf[asave]=0;}
        Gt7HudNode out;
        memset(&out,0,sizeof(out));
        memcpy(out.path,path->buf,LIVE_HUD_PATH);
        if(alias->len!=path->len)memcpy(out.alt,alias->buf,LIVE_HUD_PATH);
        out.depth=depth;out.address=node;out.vtable=*(uintptr_t *)blk;out.children=*(uintptr_t *)(blk+HUD_OFF_CHILDREN);
        memcpy(out.rect,blk+HUD_OFF_RECT,sizeof(out.rect));
        int hidden=1;
        for(unsigned i=0;i<8;i++){float v;memcpy(&v,blk+HUD_OFF_RECT+4*i,4);if(v!=0.0f){hidden=0;break;}}
        out.hidden=hidden;
        w->count++;
        if(w->emit(w->context,&out)){path->len=save;path->buf[save]=0;alias->len=asave;alias->buf[asave]=0;return;}
    }
    hud_tree_children(w,node,depth+1,path,alias);
    path->len=save;path->buf[save]=0;alias->len=asave;alias->buf[asave]=0;
}
static void hud_tree_children(HudTreeWalk *w,uintptr_t node,unsigned depth,HudPath *path,HudPath *alias){
    unsigned char blk[HUD_OFF_CHILDREN+8];
    if(read_mem(w->pid,node,blk,sizeof(blk)))return;
    uintptr_t nl=*(uintptr_t *)(blk+HUD_OFF_CHILDREN);
    if(!hud_ptr(nl))return;
    unsigned char head[0x48];
    if(read_mem(w->pid,nl,head,sizeof(head)))return;
    if(*(uintptr_t *)head!=w->base+HUD_NLW_RVA)return;
    uintptr_t vb=*(uintptr_t *)(head+0x38),ve=*(uintptr_t *)(head+0x40);
    if(!hud_ptr(vb)||ve<vb||(ve-vb)>8u*4096u)return;
    uintptr_t n=(ve-vb)/8;
    uintptr_t *kids=malloc(n*8);
    if(!kids)return;
    if(read_mem(w->pid,vb,kids,n*8)){free(kids);return;}
    for(uintptr_t i=0;i<n;i++){uintptr_t c=kids[i];if(hud_ptr(c))hud_tree_node(w,c,depth,path,alias);}
    free(kids);
}
int gt7_hud_tree(int (*emit)(void *context,const Gt7HudNode *node),void *context){
    pid_t pid;uintptr_t base,address;LiveMailbox state;
    if(!emit||credits_game(&pid,&base)||live_resolve(pid,base,&address,&state)||!state.enabled||!state.heartbeat)return -1;
    uintptr_t ctx=state.context,ctx_vt;
    if(!hud_ptr(base)||!hud_ptr(ctx))return -1;
    if(read_mem(pid,ctx,&ctx_vt,8)||ctx_vt!=base+HUD_UPDATE_VT_RVA)return -1;
    HudTreeWalk w={pid,base,emit,context,0,0};
    HudPath path;path.len=0;path.buf[0]=0;
    HudPath alias;alias.len=0;alias.buf[0]=0;
    unsigned char rcs[6*8];
    if(read_mem(pid,ctx+0x698,rcs,sizeof(rcs)))return -1;
    for(unsigned i=0;i<6;i++){
        uintptr_t rc=*(uintptr_t *)(rcs+8*i);
        if(!hud_ptr(rc))continue;
        uintptr_t vt;
        if(read_mem(pid,rc,&vt,8))continue;
        if(vt!=base+HUD_RENDER_VT_RVA)continue;
        unsigned char vec[0x10];
        if(read_mem(pid,rc+0x1e8,vec,sizeof(vec)))continue;
        uintptr_t rb=*(uintptr_t *)vec,re=*(uintptr_t *)(vec+8);
        if(!hud_ptr(rb)||re<rb||(re-rb)>8u*64u)continue;
        uintptr_t n=(re-rb)/8;
        uintptr_t *rws=malloc(n*8);
        if(!rws)continue;
        if(read_mem(pid,rb,rws,n*8)){free(rws);continue;}
        for(uintptr_t k=0;k<n;k++){
            uintptr_t rw=rws[k];
            if(!hud_ptr(rw))continue;
            uintptr_t rvt;
            if(read_mem(pid,rw,&rvt,8))continue;
            if(rvt!=base+HUD_ROOT_VT_RVA)continue;
            hud_tree_children(&w,rw,0,&path,&alias);
        }
        free(rws);
    }
    return 0;
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
    if(fw>=0x1100&&fw<0x1200){signature=mid_signature;signature_size=sizeof(mid_signature);wildcard_start=29;wildcard_size=4;engine_log("Bridge: using 11.xx signature");}
    else if(fw>=0x1200){signature=new_signature;signature_size=sizeof(new_signature);wildcard_start=32;wildcard_size=4;engine_log("Bridge: using 12.00+ signature");}
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
    /* The launch hook never calls this import, but the icon hook in the same
     * bridge does.  Select it by exact firmware when the profile is known. */
    const IconProfile *profile=icon_profile();
    imports[8]=module.mapbase+(profile?profile->title_dir:0x2e0050);
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
/* The build-environment string ("release") is not at a fixed address and, on
 * 01.710.000, is no longer preceded by the 16 zero bytes older builds used.
 * It lives in the module's string table as an entry {len,strptr} immediately
 * followed by {entryptr,hash}; the record that owns it therefore
 * self-references the string pointer at P+0x28, while the string itself sits in
 * zero padding. Older images instead padded 16 zero bytes directly before the
 * string. Accept either fingerprint, restricted to the module image, and
 * require exactly one unambiguous hit before rewriting it to "debug". */
static int apply_debug_environment(pid_t pid,const Module *game){
    static const unsigned char replacement[]={'d','e','b','u','g',0,0};
    if(!game->mapsize||game->mapsize>128u*1024*1024)return -1;
    unsigned char *image=malloc(game->mapsize);if(!image)return -1;
    for(size_t off=0;off<game->mapsize;){
        size_t n=game->mapsize-off;if(n>1024*1024)n=1024*1024;
        if(read_mem(pid,game->mapbase+off,image+off,n)){free(image);return -1;}
        off+=n;
    }
    size_t matches=0;uintptr_t target=0;
    for(size_t i=8;i+0x38<=game->mapsize;i++){
        if(memcmp(image+i,"release\x00",8))continue;
        uint64_t owned_by_table=0;memcpy(&owned_by_table,image+i+0x28,8);
        int owned=owned_by_table==(uint64_t)(game->mapbase+i);
        int padded=1;
        for(size_t z=i-8;z<i&&padded;z++)padded=!image[z];
        for(size_t z=i+8;z<i+0x18&&padded;z++)padded=!image[z];
        if(owned||padded){target=game->mapbase+i;matches++;}
    }
    free(image);
    return matches==1?verified_write(pid,target,replacement,sizeof(replacement)):-1;
}
/* GT7 draws the world-map clock from sceRtcGetCurrentTick, which libSceRtc
 * implements with clock_gettime(clock id 0x10). On this console that clock runs
 * ahead of the realtime clock the OS, gettimeofday and libSceRtc's own other
 * time getters use (+8m37s observed), so the in-game clock -- and every other
 * consumer of the tick -- reads the wrong time. The sibling getters pass clock
 * id 0 (CLOCK_REALTIME); retarget this one to match. The instruction
 * `bf 10 00 00 00` (mov edi,0x10) is at libSceRtc+0x5b5; a signature around it
 * is verified so an unexpected firmware layout is skipped, never corrupted.
 * The syscall itself is fine, so no import or GOT surgery is needed. */
static int apply_clock_fix(pid_t pid){
    static const unsigned char signature[]={
        0x48,0x89,0x45,0xe8,0x48,0x85,0xff,0x74,0x35,0x48,0x89,0xfb,0x48,0x8d,0x75,0xd8,
        0xbf,0x10,0x00,0x00,0x00,0xe8};
    const unsigned clock_id=0x5b5; /* mov edi,0x10 at signature[16] */
    /* The library may still be loading when the launch request arrives. */
    for(unsigned attempt=0;attempt<40;attempt++){
        Module rtc;
        if(!find_module(pid,"libSceRtc",&rtc)&&rtc.mapbase){
            unsigned char bytes[sizeof(signature)];
            if(!read_mem(pid,rtc.mapbase+0x5a5,bytes,sizeof(bytes))){
                if(memcmp(bytes,signature,sizeof(signature)))return -1;
                unsigned char insn[5]={0xbf,0x00,0x00,0x00,0x00}; /* mov edi,0 */
                return verified_write(pid,rtc.mapbase+clock_id,insn,sizeof(insn));
            }
        }
        usleep(50000);
    }
    return -1;
}
static int apply_gt7(pid_t pid,int final_attempt){
    AppInfo app={0};char version[32]={0};Module game;
    if(sceKernelGetAppInfo(pid,&app)||strcmp(app.title_id,"PPSA01317")){atomic_store(&launch_stage,LAUNCH_NOT_GT7);return 0;}
    if(sceShellCoreUtilGetAppLaunchedParamString(app.app_id,"CONTENT_VERSION",version,sizeof(version))||strcmp(version,"01.710.000")){
        atomic_store(&launch_stage,LAUNCH_VERSION);if(final_attempt)notify("GT7: patches skipped. This payload requires version 01.710.000.");return 0;
    }
    if(main_module(pid,&game)||!game.mapbase){atomic_store(&launch_stage,LAUNCH_MODULE);if(final_attempt)notify("GT7: could not locate the game executable. Close GT7 and retry.");return 0;}
    Config config;if(load_config(state_root,&config)){atomic_store(&launch_stage,LAUNCH_CONFIG);if(final_attempt)notify("GT7: could not read saved patch choices. Close GT7 and save again.");return 0;}
    /* Generate from validated settings and built-in patch data. Never execute
     * arbitrary XML supplied on disk; only the config comment is consumed. */
    FILE *f=fopen("/data/gt7p/launch.tmp","w+");
    if(f)unlink("/data/gt7p/launch.tmp");
    if(!f||emit_xml(f,&config)){atomic_store(&launch_stage,LAUNCH_PREPARE);if(f)fclose(f);if(final_attempt)notify("GT7: could not prepare patches.");return 0;}
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
        if(strstr(line,"<!-- GT7FEATURE country -->")){
            current_name="Player country";continue;
        }
        if(strstr(line,"<!-- GT7FEATURE aspect -->")){
            current_name="Video aspect ratio";continue;
        }
        if(strncmp(line,"<Line ",6))continue;
        if(sscanf(line,"<Line Type=\"bytes\" Address=\"0x%x\" Value=\"%8192[0123456789abcdefABCDEF]\"",&address,hex)!=2){failed=1;break;}
        if(address==0x060e83b0){
            if(apply_debug_environment(pid,&game)){failed=1;break;}
        }else if(address==0x060e83c0){
            /* Not a byte patch: resolve libSceRtc and retarget its clock source.
             * Its failure is non-fatal so a firmware with an unexpected layout
             * cannot stop the byte patches from applying. */
            engine_log_result("Clock fix result",apply_clock_fix(pid));
        }else if(apply_line(pid,game.mapbase,address,hex)){failed=1;break;}
        patched=1;
    }
    fclose(f);
    if(!failed&&(config.mask&LIVE_SETTINGS_BIT)&&live_install(pid,game.mapbase)){
        atomic_store(&launch_stage,LAUNCH_LIVE);
        if(final_attempt)notify("GT7: could not install the optional Live Settings bridge. Close GT7 before retrying.");
        return 0;
    }
    if(!failed&&(config.mask&LIVE_SETTINGS_BIT))patched=1;
    if(failed){atomic_store(&launch_stage,LAUNCH_PATCH);if(final_attempt)notify("GT7: Patch failed: %s. Close the game before retrying.",current_name);return 0;}
    else if(patched){atomic_store(&launch_stage,LAUNCH_APPLIED);notify("GT7Patcher: Saved patches applied.");}
    else {atomic_store(&launch_stage,LAUNCH_UNCHANGED);notify("GT7: all patches are disabled. Launching without changes.");}
    return 1;
}
static void *patch_service(void *unused){
    (void)unused;atomic_store(&live,1);
    while(atomic_load(&live)){
        int fd=accept(service,NULL,NULL);if(fd<0){if(errno==EINTR)continue;break;}
        struct timeval timeout={10,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        PatchRequest request={0};size_t n=0;
        while(n<sizeof(request)){ssize_t r=recv(fd,(char *)&request+n,sizeof(request)-n,0);if(r<=0)break;n+=(size_t)r;}
        if(n==sizeof(request)&&request.pid>0){
            atomic_fetch_add(&launch_requests,1);atomic_store(&launch_pid,request.pid);atomic_store(&launch_stage,LAUNCH_REQUESTED);
            unsigned char ack=apply_gt7(request.pid,request.final_attempt)?1:0;send(fd,&ack,1,0);
        }
        close(fd);
    }atomic_store(&live,0);return NULL;
}
int gt7_engine_ready(void){
    uintptr_t value=0;unsigned char code[sizeof(hook_blob)];
    return active&&!damaged&&atomic_load(&live)&&!read_mem(shell,slot,&value,8)&&value==bridge+HOOK_LAUNCH&&
        !read_mem(shell,bridge,code,sizeof(code))&&!memcmp(code,expected_bridge,sizeof(code));
}
int gt7_engine_active(void){return active&&!damaged;}
void gt7_engine_launch_status(unsigned *requests,int *pid,const char **stage){
    unsigned state=(unsigned)atomic_load(&launch_stage);
    if(state>=sizeof(launch_stages)/sizeof(launch_stages[0]))state=LAUNCH_NONE;
    if(requests)*requests=atomic_load(&launch_requests);
    if(pid)*pid=atomic_load(&launch_pid);
    if(stage)*stage=launch_stages[state];
}
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
    if(ime_slot&&ime_original){
        uintptr_t current=0;
        if(!read_mem(shell,ime_slot,&current,8) && current==ime_hook && verified_write(shell,ime_slot,&ime_original,8)){
            damaged=1;return -1;
        }
        ime_slot=ime_original=ime_hook=0;
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
/* ShadowMountPlus may already own AppInstallAll. Its bridge deliberately
 * exposes one-shot title/base-directory immediates. Reuse it only after
 * verifying the exact firmware cave and bridge ABI; otherwise leave another
 * payload's hook untouched. */
static int install_with_shadowmount_bridge(const IconProfile *profile,int (*install_all)(void *),int *handled){
    *handled=0;
    const uintptr_t cave=module.mapbase+profile->smp_cave;
    const uintptr_t hook=cave+0x260;
    const uintptr_t target=module.mapbase+profile->install_all;
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
    const IconProfile *profile=icon_profile();
    if(!profile){engine_log("Icon: unsupported firmware profile");return -1;}
    shell=find_shell();
    if(shell<0||main_module(shell,&module)){engine_log("Icon: ShellCore lookup failed");return -1;}
    int handled=0,shared=install_with_shadowmount_bridge(profile,install_all,&handled);
    if(handled){if(shared>0){mark_icon_installed();return 0;}return -1;}
    if(bridge_prepare()){engine_log("Icon: bridge preparation failed");return -1;}
    /* Firmware 12 removed the public TitleDir RPC. Temporarily redirect the
     * internal AppInstallAll dispatcher to our fixed-title registration call.
     * Restore and verify its bytes regardless of registration result. */
    uintptr_t target=module.mapbase+profile->install_all,destination=bridge+HOOK_ICON;
    unsigned char original_bytes[12],jump[12]={0x48,0xb8};
    const unsigned char prologue[]={0x55,0x48,0x89,0xe5};
    if(read_mem(shell,target,original_bytes,sizeof(original_bytes))||memcmp(original_bytes,prologue,4)){engine_log("Icon: AppInstallAll target prologue mismatch");return -1;}
    unsigned char install_prologue[4];
    if(read_mem(shell,module.mapbase+profile->title_dir,install_prologue,4)||memcmp(install_prologue,prologue,4)){engine_log("Icon: TitleDir target prologue mismatch");return -1;}
    memcpy(jump+2,&destination,8);jump[10]=0xff;jump[11]=0xe0;
    engine_log("Icon: redirecting fixed-title registration call");
    if(stopped_swap(target,original_bytes,jump,sizeof(jump))){engine_log("Icon: redirect swap failed");return -1;}
    int result=install_all(NULL);
    if(stopped_swap(target,jump,original_bytes,sizeof(original_bytes))){damaged=1;return -1;}
    FILE *log=fopen("/data/gt7p/startup.log","a");if(log){fprintf(log,"Icon: registration result 0x%08x\n",(unsigned)result);fclose(log);}
    if(result==0||(uint32_t)result==0x80990002U){mark_icon_installed();return 0;}
    return -1;
}
