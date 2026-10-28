/* SPDX-License-Identifier: GPL-3.0-only */
#include "config.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef PS5
#include <sys/sysctl.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <ps5/kernel.h>
#include <pthread.h>
#include <stdatomic.h>
#include "engine.h"
#include "notify.h"

typedef struct {uint32_t app_id;uint64_t unknown1;char title_id[10];uint32_t apptype;char unknown2[0x36];} AppInfo;
extern int sceKernelGetAppInfo(pid_t,AppInfo *);
#endif
#define VERSION "0.23"
#define PORT 7210
static const char *root="/data/gt7p/patches";
static char token[33],message[256]="Save your choices, then start the engine before launching GT7.";
static Config config;

static int broken;
static volatile sig_atomic_t stopping;
#ifdef PS5
static atomic_int startup_pending;
static pthread_t startup_thread;
static int startup_thread_created;
#endif
static void stop(int sig){(void)sig;stopping=1;}
static int transfer(int fd,const void *data,size_t n){
    const char *p=data;while(n){ssize_t r=send(fd,p,n,0);if(r<0&&errno==EINTR)continue;if(r<=0)return -1;p+=r;n-=(size_t)r;}return 0;
}
static void response(int fd,int code,const char *type,const char *body){
    char h[512];size_t n=strlen(body);
    int z=snprintf(h,sizeof(h),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: frame-ancestors 'none'\r\nConnection: close\r\n\r\n",code,code==200?"OK":"Error",type,n);
    if(z>0&&(size_t)z<sizeof(h)&&!transfer(fd,h,(size_t)z))transfer(fd,body,n);
}
static int running_gt7(void){
#ifdef PS5
    int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_PROC,0};size_t n=0;
    if(sysctl(mib,4,NULL,&n,NULL,0))return -1;
    unsigned char *b=malloc(n);if(!b)return -1;
    if(sysctl(mib,4,b,&n,NULL,0)){free(b);return -1;}
    int found=0;
    for(size_t at=0;at+sizeof(struct kinfo_proc)<=n;){
        struct kinfo_proc *k=(void *)(b+at);if(k->ki_structsize<=0||(size_t)k->ki_structsize<sizeof(*k)||at+(size_t)k->ki_structsize>n){found=-1;break;}
        if(!strcmp(k->ki_comm,"eboot.bin")){
            AppInfo a={0};if(sceKernelGetAppInfo(k->ki_pid,&a)){found=-1;break;}
            if(!memcmp(a.title_id,"PPSA01317",9)||!memcmp(a.title_id,"PPSA01316",9)){found=1;break;}
        }
        at+=(size_t)k->ki_structsize;
    }free(b);return found;
#else
    return 0;
#endif
}
static int engine_ready(void){
#ifdef PS5
    if(atomic_load(&startup_pending))return 0;
    return gt7_engine_ready();
#else
    return 0;
#endif
}
static int port_in_use(unsigned port){
    int s=socket(AF_INET,SOCK_STREAM,0);if(s<0)return 1;
    struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=htons((uint16_t)port);
    int r=connect(s,(void *)&a,sizeof(a));close(s);return r==0;
}
#ifdef PS5
static int wait_for_gt7_launch(void){
    /* LaunchApp queues an asynchronous request and its return value is not a
     * boolean error status. Confirm the result by observing GT7 itself. */
    for(unsigned i=0;i<50;i++){
        if(running_gt7()>0)return 1;
        usleep(100000);
    }
    return 0;
}
#endif
static int start_engine(void){
    if(engine_ready())return 0;
    if(broken){strcpy(message,"Engine is unavailable. Reboot before retrying.");return -1;}
#ifdef PS5
    if(port_in_use(23916)||port_in_use(23900)||port_in_use(7211)){
        strcpy(message,"Another patch engine is running. Reboot and load only GT7Patcher.");return -1;
    }
    if(gt7_engine_start()){
        broken=1;strcpy(message,"Could not verify the GT7 launch hook. Reboot before retrying.");
        notify("GT7Patcher: engine startup failed. Reboot before retrying.");return -1;
    }
    strcpy(message,"GT7 launch hook is ready. You can launch GT7 now.");
    notify("GT7Patcher is ready. You can launch GT7 now.");return 0;
#else
    (void)port_in_use;
    strcpy(message,"Host preview only: the console patch engine is unavailable here.");return -1;
#endif
}
static int make_dirs(const char *path){
    char b[512];if(strlen(path)>=sizeof(b))return -1;strcpy(b,path);
    for(char *p=b+1;*p;p++)if(*p=='/'){*p=0;if(mkdir(b,0755)&&errno!=EEXIST)return -1;*p='/';}
    return mkdir(b,0755)&&errno!=EEXIST?-1:0;
}
static int initialize(void){
    char p[512];const char *dirs[]={"xml_prospero"};
    for(unsigned i=0;i<1;i++){snprintf(p,sizeof(p),"%s/%s",root,dirs[i]);if(make_dirs(p))return -1;}
    config=defaults();
    if(load_config(root,&config)){
        /* Never overwrite an existing malformed configuration with defaults. */
        snprintf(p,sizeof(p),"%s/xml_prospero/PPSA01317.xml",root);
        if(access(p,F_OK)==0)return -1;
        if(save_config(root,&config))return -1;
    }
    unsigned char random[16];FILE *f=fopen("/dev/urandom","rb");if(!f)return -1;
    size_t n=fread(random,1,sizeof(random),f);fclose(f);if(n!=sizeof(random))return -1;
    for(unsigned i=0;i<16;i++)snprintf(token+i*2,3,"%02x",random[i]);
    return 0;
}
#ifdef PS5
static void *startup(void *unused){
    (void)unused;
    int icon=gt7_install_icon();
    if(icon){
        strcpy(message,"GT7Patcher is running on port 7210. Home-screen installation failed; details are in /data/gt7p/startup.log. You can still start the engine here.");
        notify("GT7Patcher: shortcut installation failed. Open console port 7210.");
    }
    if(config.autostart){
        int game=running_gt7();
        if(game){strcpy(message,game>0?"Automatic start skipped: close GT7, then start the engine.":"Automatic start skipped: could not check whether GT7 is closed.");notify("GT7Patcher: automatic start skipped. Close GT7 and start the engine from the web interface.");}
        else start_engine();
    }else if(!icon)notify("GT7Patcher: open the home-screen shortcut or console port 7210.");
    atomic_store(&startup_pending,0);
    return NULL;
}
#endif
static void json_status(int fd){
    char body[4096];size_t n=0;
    const char *status_message=message;
#ifdef PS5
    if(atomic_load(&startup_pending))status_message="GT7Patcher is available on port 7210. Preparing the home-screen shortcut and saved startup option.";
#endif
    n+=(size_t)snprintf(body+n,sizeof(body)-n,"{\"version\":\"%s\",\"token\":\"%s\",\"engineRunning\":%s,\"gameRunning\":%d,\"message\":\"%s\",\"config\":{\"mask\":%u,\"preset\":%d,\"units\":%d,\"language\":%d,\"speed\":%d,\"mass\":%d,\"power\":%d,\"torque\":%d,\"temperature\":%d,\"autostart\":%d},\"features\":[",VERSION,token,engine_ready()?"true":"false",running_gt7(),status_message,config.mask,config.preset,config.units,config.language,config.speed,config.mass,config.power,config.torque,config.temperature,config.autostart);
    /* Original feature labels and notes contain no JSON quoting characters; verify at generation. */
    for(unsigned i=0;i<PATCH_COUNT;i++)n+=(size_t)snprintf(body+n,sizeof(body)-n,"%s{\"name\":\"%s\",\"note\":\"%s\",\"lines\":%u}",i?",":"",feature_name(i),feature_note(i),feature_count(i));
    snprintf(body+n,sizeof(body)-n,"]}");response(fd,200,"application/json",body);
}
static void serve(int fd){
    char req[4096],method[12],path[128],proto[16];size_t used=0;char *body=NULL;
    struct timeval tv={5,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
    while(used+1<sizeof(req)){
        ssize_t r=recv(fd,req+used,sizeof(req)-used-1,0);if(r<=0)return;
        used+=(size_t)r;req[used]=0;if((body=strstr(req,"\r\n\r\n")))break;
    }
    if(!body||sscanf(req,"%11s %127s %15s",method,path,proto)!=3){response(fd,400,"text/plain","Invalid request");return;}
    *body=0;body+=4;
    size_t length=0;int has_length=0,authorized=0;
    for(char *h=strstr(req,"\r\n");h&&h[2];){
        h+=2;char *e=strstr(h,"\r\n");if(e)*e=0;
        if(!strncasecmp(h,"Content-Length:",15)){
            char *end;unsigned long l=strtoul(h+15,&end,10);
            if(has_length||*end||l>511){response(fd,400,"text/plain","Invalid Content-Length");return;}length=(size_t)l;has_length=1;
        }
        if(!strncasecmp(h,"Transfer-Encoding:",18)){response(fd,400,"text/plain","Unsupported transfer encoding");return;}
        if(!strncasecmp(h,"X-GT7-Token: ",13)&&!strcmp(h+13,token))authorized=1;
        h=e;
    }
    if(!strcmp(method,"GET")){
        if(!strcmp(path,"/"))response(fd,200,"text/html; charset=utf-8",page_html());
        else if(!strcmp(path,"/api/status"))json_status(fd);
        else response(fd,404,"text/plain","Not found");
        return;
    }
    if(strcmp(method,"POST")){response(fd,405,"text/plain","Use POST");return;}
    if(!authorized){response(fd,403,"text/plain","Reload this page before saving.");return;}
    if(!has_length){response(fd,400,"text/plain","Content-Length required");return;}
    if((size_t)(body-req)+length>=sizeof(req)){response(fd,400,"text/plain","Request too large");return;}
    size_t received=used-(size_t)(body-req);
    if(received>length){response(fd,400,"text/plain","Invalid body length");return;}
    while(received<length){ssize_t r=recv(fd,body+received,length-received,0);if(r<=0)return;received+=(size_t)r;}body[length]=0;
    if(strcmp(path,"/api/config")&&strcmp(path,"/api/start")&&strcmp(path,"/api/game/start")&&strcmp(path,"/api/shortcut/reinstall")&&strcmp(path,"/api/engine/stop")&&strcmp(path,"/api/payload/stop")){response(fd,404,"text/plain","Not found");return;}
#ifdef PS5
    if(atomic_load(&startup_pending)){response(fd,409,"text/plain","Startup is still running. Please retry shortly; the browser page remains available.");return;}
#endif
    if(!strcmp(path,"/api/payload/stop")){
        if(length){response(fd,400,"text/plain","Unexpected request body");return;}
        strcpy(message,"GT7Patcher has stopped. Run the payload again to reconnect.");
        json_status(fd);stopping=1;return;
    }
    if(!strcmp(path,"/api/engine/stop")){
        if(length){response(fd,400,"text/plain","Unexpected request body");return;}
#ifdef PS5
        if(gt7_engine_stop()){broken=1;strcpy(message,"Could not safely stop the engine. Reboot before retrying.");response(fd,409,"text/plain",message);return;}
        notify("GT7Patcher: Patch engine stopped.");
#endif
        strcpy(message,"Patch engine stopped. You can change settings or start it again.");json_status(fd);return;
    }
    if(!strcmp(path,"/api/shortcut/reinstall")){
        if(length){response(fd,400,"text/plain","Unexpected request body");return;}
#ifdef PS5
        unlink("/data/gt7p/gt7patcher-icon-installed");
        if(gt7_install_icon()){strcpy(message,"Could not reinstall the home-screen shortcut. See /data/gt7p/startup.log.");response(fd,409,"text/plain",message);return;}
        strcpy(message,"Home-screen shortcut reinstalled.");notify("GT7Patcher: Home-screen shortcut reinstalled.");json_status(fd);return;
#else
        response(fd,409,"text/plain","Shortcut installation is only available on PS5.");return;
#endif
    }
    if(!strcmp(path,"/api/game/start")){
        if(length){response(fd,400,"text/plain","Unexpected request body");return;}
        int game=running_gt7();
        if(game){response(fd,409,"text/plain",game>0?"GT7 is already running.":"Cannot establish that GT7 is closed; launch was not requested.");return;}
#ifdef PS5
        if(!engine_ready()){response(fd,409,"text/plain","Start the patch engine before launching GT7.");return;}
        int launch_result=gt7_launch_game();
        if(!wait_for_gt7_launch()){
            snprintf(message,sizeof(message),"GT7 did not appear after the launch request (result 0x%08x).",(unsigned)launch_result);
            response(fd,409,"text/plain",message);return;
        }
        strcpy(message,"GT7 launch requested. The patch engine will apply saved patches as the game starts.");
        json_status(fd);return;
#else
        response(fd,409,"text/plain","GT7 launch is only available on PS5.");return;
#endif
    }
    int game=running_gt7();if(game){response(fd,409,"text/plain",game>0?"Fully close GT7 before saving or starting the engine.":"Cannot establish that GT7 is closed; no changes saved.");return;}
    if(!strcmp(path,"/api/start")){
        if(length){response(fd,400,"text/plain","Unexpected request body");return;}
        if(start_engine()){response(fd,409,"text/plain",message);return;}
    }else{
        Config next;if(parse_config(body,&next)){response(fd,400,"text/plain","Invalid settings. Quick Store Rotation requires Offline mode.");return;}
        if(save_config(root,&next)){response(fd,500,"text/plain","Could not save the patch snapshot; previous settings retained.");return;}
        config=next;strcpy(message,engine_ready()?"Choices saved. Launch GT7 to apply them.":"Choices saved. Start the engine, then launch GT7.");
    }
    json_status(fd);
}
int main(int argc,char **argv){
    unsigned port=PORT;
#ifndef PS5
    if(argc!=3){fprintf(stderr,"Usage: %s LOCAL_STATE_DIRECTORY PORT\n",argv[0]);return 1;}
    root=argv[1];port=(unsigned)strtoul(argv[2],NULL,10);
#else
    (void)argc;(void)argv;
    /* elfldr uses this syscall to assign payload.elf. Use the same mechanism
     * so process enumerators report the payload's real name. */
    syscall(SYS_thr_set_name,-1,"gt7patcher.elf");
    notify("GT7Patcher %s: starting on port 7210.",VERSION);
#endif
    signal(SIGPIPE,SIG_IGN);signal(SIGTERM,stop);signal(SIGINT,stop);
    int s=socket(AF_INET,SOCK_STREAM,0);if(s<0)return 1;
    int opt=1;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_port=htons((uint16_t)port);
#ifdef PS5
    a.sin_addr.s_addr=htonl(INADDR_ANY);
#else
    a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
#endif
    if(bind(s,(void *)&a,sizeof(a))||listen(s,8)){perror("GT7Patcher socket");close(s);return 1;}
    if(initialize()){perror("GT7Patcher initialization");close(s);return 1;}
#ifdef PS5
    FILE *log=fopen("/data/gt7p/startup.log","w");
    if(log){fprintf(log,"GT7Patcher %s main reached; firmware=%08x; web socket listening on 7210\n",VERSION,kernel_get_fw_version());fclose(log);}
    if(port_in_use(23916)||port_in_use(23900)||port_in_use(7211)){
        strcpy(message,"Engine startup requires no other patch system. Reboot and load only GT7Patcher.");
        notify("GT7Patcher: engine unavailable. Open console port 7210 for details.");
    }else{
        atomic_store(&startup_pending,1);
        if(pthread_create(&startup_thread,NULL,startup,NULL)){
            atomic_store(&startup_pending,0);strcpy(message,"Automatic setup could not start. GT7Patcher is available on port 7210.");
        }else startup_thread_created=1;
    }
#endif
    printf("GT7Patcher %s on port %u\n",VERSION,port);fflush(stdout);
    while(!stopping){fd_set set;FD_ZERO(&set);FD_SET(s,&set);struct timeval timeout={1,0};
        int r=select(s+1,&set,NULL,NULL,&timeout);if(r<0){if(errno==EINTR)continue;break;}if(!r)continue;
        int c=accept(s,NULL,NULL);if(c<0)continue;serve(c);close(c);
    }
    close(s);
#ifdef PS5
    if(startup_thread_created)pthread_join(startup_thread,NULL);
    (void)gt7_engine_stop();
#endif
    return 0;
}
