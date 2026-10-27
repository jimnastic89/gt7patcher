/* SPDX-License-Identifier: GPL-3.0-only
 * Position-independent ShellCore bridge. No imports or relocations at runtime.
 * Only PPSA01317 launch events reach GT7Patcher's loopback patch service.
 */
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <stddef.h>
typedef struct {uint32_t app_id;uint64_t unknown1;char title_id[10];uint32_t apptype;char unknown2[0x36];} AppInfo;
typedef struct __attribute__((packed)){int pid;unsigned char final_attempt;} PatchRequest;
struct Imports {
    int (*original)(void *,void *,void *);
    int (*appinfo)(int,AppInfo *);
    int (*socket)(int,int,int);
    int (*connect)(int,const struct sockaddr *,unsigned);
    int (*setsockopt)(int,int,int,const void *,unsigned);
    long (*send)(int,const void *,size_t,int);
    long (*recv)(int,void *,size_t,int);
    int (*close)(int);
    int (*install)(const char *,const char *,int,int);
};
__attribute__((visibility("hidden"))) volatile struct Imports imports={0};
static int patch_process(void *c,int final_attempt){
    AppInfo info={0};
    if(!c||imports.appinfo(*(int *)c,&info))return 0;
    const char *id=info.title_id;
    if(id[0]!='P'||id[1]!='P'||id[2]!='S'||id[3]!='A'||id[4]!='0'||id[5]!='1'||id[6]!='3'||id[7]!='1'||id[8]!='7'||id[9])return 0;
    int fd=imports.socket(AF_INET,SOCK_STREAM,0);
    if(fd<0)return 0;
    struct timeval timeout={10,0};
    imports.setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    imports.setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    struct sockaddr_in addr={0};addr.sin_len=sizeof(addr);addr.sin_family=AF_INET;
    addr.sin_port=0x2b1c; /* htons(7211) */
    addr.sin_addr.s_addr=0x0100007f; /* loopback */
    int patched=0;
    if(!imports.connect(fd,(void *)&addr,sizeof(addr))){
        PatchRequest request={*(int *)c,(unsigned char)final_attempt};unsigned char *p=(void *)&request;size_t n=sizeof(request);
        while(n){long r=imports.send(fd,p,n,0);if(r<=0)break;p+=r;n-=(size_t)r;}
        if(!n){unsigned char ack=0;if(imports.recv(fd,&ack,1,0)==1&&ack==1)patched=1;}
    }
    imports.close(fd);return patched;
}
__attribute__((visibility("hidden"))) int launch_hook(void *a,void *b,void *c){
    /* Apply before the original callback so launch-time feature checks see
     * the modified bytes. Retry afterward for callbacks that expose the PID
     * before the process address space is ready. */
    int patched=patch_process(c,0);
    int result=imports.original(a,b,c);
    if(!patched)patch_process(c,1);
    return result;
}
/* Called only during first-run home-screen registration, through a temporary
 * AppInstallAll redirect. The caller restores the original bytes afterwards. */
__attribute__((visibility("hidden"))) int icon_hook(void){
    return imports.install("GTPS00001","/user/app/",1,0);
}
