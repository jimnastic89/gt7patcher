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
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>
typedef struct {uint32_t app_id;uint64_t unknown1;char title_id[10];uint32_t apptype;char unknown2[0x36];} AppInfo;
extern int sceKernelGetAppInfo(pid_t,AppInfo *);
extern int sceShellCoreUtilGetAppLaunchedParamString(uint32_t,const char *,char *,size_t);
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
    uint64_t paging[2];if(kernel_copyout(vm+0x2e8+32,paging,sizeof(paging)))return -1;
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
static int bridge_prepare(void){
    engine_log("Bridge: locating ShellCore and main module");
    if(bridge)return damaged?-1:0;
    if((kernel_get_fw_version()>>16)!=0x1240)return -1;
    shell=find_shell();if(shell<0||main_module(shell,&module))return -1;
    engine_log("Bridge: scanning OnNewProcess and callback pointer");
    /* OnNewProcess 12.xx signature; require exactly one match and pointer. */
    const unsigned char signature[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18,0x49,0x89,0xfd,0xbf,0x18,0,0,0,0x49,0x89,0xd6,0x49,0x89,0xf4};
    if(!module.mapsize||module.mapsize>128*1024*1024||module.eh_frame_size<sizeof(hook_blob))return -1;
    unsigned char *image=malloc(module.mapsize);if(!image)return -1;
    if(read_mem(shell,module.mapbase,image,module.mapsize)){free(image);return -1;}
    unsigned matches=0,pointers=0;
    for(size_t i=0;i+sizeof(signature)<=module.mapsize;i++)if(!memcmp(image+i,signature,sizeof(signature))){original=module.mapbase+i;matches++;}
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
static void apply_gt7(pid_t pid){
    AppInfo app={0};char version[32]={0};Module game;
    if(sceKernelGetAppInfo(pid,&app)||strcmp(app.title_id,"PPSA01317"))return;
    if(sceShellCoreUtilGetAppLaunchedParamString(app.app_id,"CONTENT_VERSION",version,sizeof(version))||strcmp(version,"01.710.000")){
        notify("GT7: patches skipped. This payload requires version 01.710.000.");return;
    }
    if(main_module(pid,&game)||!game.mapbase){notify("GT7: could not locate the game executable. Close GT7 and retry.");return;}
    Config config;if(load_config(state_root,&config)){notify("GT7: could not read saved patch choices. Close GT7 and save again.");return;}
    /* Generate from validated settings and built-in patch data. Never execute
     * arbitrary XML supplied on disk; only the config comment is consumed. */
    FILE *f=fopen("/data/gt7p/launch.tmp","w+");
    if(f)unlink("/data/gt7p/launch.tmp");
    if(!f||emit_xml(f,&config)){if(f)fclose(f);notify("GT7: could not prepare patches.");return;}
    rewind(f);char line[16384],hex[8193];unsigned address,count=0;int failed=0;
    while(fgets(line,sizeof(line),f)){
        if(strncmp(line,"<Line ",6))continue;
        if(sscanf(line,"<Line Type=\"bytes\" Address=\"0x%x\" Value=\"%8192[0123456789abcdefABCDEF]\"",&address,hex)!=2||apply_line(pid,game.mapbase,address,hex)){failed=1;break;}
        count++;
    }
    fclose(f);
    if(failed)notify("GT7: patching failed. Close the game before retrying.");
    else if(count)notify("GT7Patcher: Saved patches applied.");
    else notify("GT7: all patches are disabled. Launching without changes.");
}
static void *patch_service(void *unused){
    (void)unused;atomic_store(&live,1);
    while(atomic_load(&live)){
        int fd=accept(service,NULL,NULL);if(fd<0){if(errno==EINTR)continue;break;}
        struct timeval timeout={10,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        pid_t pid=0;size_t n=0;
        while(n<sizeof(pid)){ssize_t r=recv(fd,(char *)&pid+n,sizeof(pid)-n,0);if(r<=0)break;n+=(size_t)r;}
        if(n==sizeof(pid)&&pid>0){apply_gt7(pid);unsigned char ack=1;send(fd,&ack,1,0);}
        close(fd);
    }atomic_store(&live,0);return NULL;
}
int gt7_engine_ready(void){
    uintptr_t value=0;unsigned char code[sizeof(hook_blob)];
    return active&&!damaged&&atomic_load(&live)&&!read_mem(shell,slot,&value,8)&&value==bridge+HOOK_LAUNCH&&
        !read_mem(shell,bridge,code,sizeof(code))&&!memcmp(code,expected_bridge,sizeof(code));
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
    /* Leave the bridge memory in place: another ShellCore thread may still
     * be returning through it. The restored pointer prevents new calls. */
    return 0;
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
    int (*uninstall)(const char *)=(int (*)(const char *))dlsym(install_library,"sceAppInstUtilAppUnInstall");
    if(!initialize||!install_all){engine_log("Icon: registration API resolution failed");return -1;}
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
