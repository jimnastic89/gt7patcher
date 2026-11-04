/* SPDX-License-Identifier: GPL-3.0-only */
#include "config.h"
#include "live_bridge.h"
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
#define VERSION "0.3.18.26-11-04_1645"
#ifndef PORT
#define PORT 7210
#endif
static const char *root="/data/gt7p/patches";
static char token[33],message[256]="";
static Config config;
#ifndef PS5
/* Host preview: the live bridge and its pad shortcut exist only on PS5. */
static void gt7_live_pad_configure(int enabled){(void)enabled;}
static void gt7_live_pad_poll(void){}
static int gt7_live_credit_auto_poll(void){return -1;}
#endif

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
/* Persisted in-race HUD hide list, one entry per line in <root>/hud.txt. An
 * entry containing '/' is a widget path (RaceDisplay/Container/.../TCS) and
 * matches exactly that widget; an entry without one is a bare widget name and
 * matches every widget with that name. It is re-pushed to the live bridge
 * whenever the game is running so a selection survives races and game restarts.
 * Paths are what let a HUD element be hidden without also hiding a pause-menu
 * row that merely shares its name. */
static char hud_path[512];
/* Saved HUD group layout (<root>/hud_layout.json). The page owns the schema and
 * the payload stores it verbatim, so every browser that connects sees the same
 * grouping and the same custom item names. */
static char hud_layout_path[512];
static char hud_hidden[LIVE_HUD_MAX][LIVE_HUD_PATH];
static unsigned hud_hidden_count;
#ifdef PS5
static Gt7Hud hud_state; /* ~6 KB: kept off the request thread's stack */
#endif
static void hud_load(void){
    hud_hidden_count=0;
    FILE *f=fopen(hud_path,"rb");if(!f)return;
    char line[LIVE_HUD_PATH+8];
    while(hud_hidden_count<LIVE_HUD_MAX&&fgets(line,sizeof(line),f)){
        size_t n=strlen(line);
        while(n&&(line[n-1]=='\n'||line[n-1]=='\r'))line[--n]=0;
        if(!n||n>=LIVE_HUD_PATH)continue;
        memcpy(hud_hidden[hud_hidden_count],line,n+1);
        hud_hidden_count++;
    }
    fclose(f);
}
static int hud_save(void){
    FILE *f=fopen(hud_path,"wb");if(!f)return -1;
    for(unsigned i=0;i<hud_hidden_count;i++)fprintf(f,"%s\n",hud_hidden[i]);
    return fclose(f)?-1:0;
}
static void hud_push(void){
#ifdef PS5
    gt7_hud_set((const char (*)[LIVE_HUD_PATH])hud_hidden,hud_hidden_count);
#endif
}
static void hud_tick(void){
#ifdef PS5
    if(!(config.mask&LIVE_SETTINGS_BIT)||running_gt7()<=0)return;
    if(gt7_hud_read(&hud_state))return;
    int same=hud_state.count==hud_hidden_count;
    if(same)for(unsigned i=0;i<hud_hidden_count;i++)if(strncmp(hud_state.entries[i],hud_hidden[i],LIVE_HUD_PATH)){same=0;break;}
    if(!same)gt7_hud_set((const char (*)[LIVE_HUD_PATH])hud_hidden,hud_hidden_count);
#endif
}
#ifdef PS5
/* Serialiser for GET /api/live/hud/tree. Paths hold only printable ASCII, but
 * a quote or backslash still needs escaping to keep the JSON well formed. */
typedef struct {char *body;size_t cap,at;int first;} HudTreeJson;
static int hud_tree_emit(void *context,const Gt7HudNode *node){
    HudTreeJson *j=context;
    if(j->at+4u*LIVE_HUD_PATH+128u>j->cap)return 1; /* stop; the page shows what fits */
    size_t n=j->at;
    n+=(size_t)snprintf(j->body+n,j->cap-n,"%s{\"p\":\"",j->first?"":",");
    j->first=0;
    for(const char *c=node->path;*c;c++){
        if(*c=='"'||*c=='\\')j->body[n++]='\\';
        j->body[n++]=*c;
    }
    n+=(size_t)snprintf(j->body+n,j->cap-n,"\",\"d\":%u,\"h\":%d,\"a\":\"%#llx\"",node->depth,node->hidden,(unsigned long long)node->address);
    /* The pre-heap-name path, when it differs: the page treats it as an alias
     * so a group saved earlier still shows its widgets as present. */
    if(node->alt[0]){
        n+=(size_t)snprintf(j->body+n,j->cap-n,",\"q\":\"");
        for(const char *c=node->alt;*c;c++){
            if(*c=='"'||*c=='\\')j->body[n++]='\\';
            j->body[n++]=*c;
        }
        j->body[n++]='"';
    }
    j->body[n++]='}';
    j->body[n]=0;
    j->at=n;
    return 0;
}
#endif
static int engine_ready(void){
#ifdef PS5
    if(atomic_load(&startup_pending))return 0;
    return gt7_engine_ready();
#else
    return 0;
#endif
}
static int engine_active(void){
#ifdef PS5
    return gt7_engine_active();
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
    snprintf(hud_path,sizeof(hud_path),"%s/hud.txt",root);
    snprintf(hud_layout_path,sizeof(hud_layout_path),"%s/hud_layout.json",root);
    hud_load();
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
    n+=(size_t)snprintf(body+n,sizeof(body)-n,"{\"version\":\"%s\",\"token\":\"%s\",\"engineRunning\":%s,\"engineActive\":%s,\"gameRunning\":%d,\"message\":\"%s\",\"config\":{\"mask\":%u,\"preset\":%d,\"units\":%d,\"language\":%d,\"country\":%d,\"speed\":%d,\"mass\":%d,\"power\":%d,\"torque\":%d,\"temperature\":%d,\"autostart\":%d,\"aspect\":%d},\"features\":[",VERSION,token,engine_ready()?"true":"false",engine_active()?"true":"false",running_gt7(),status_message,config.mask,config.preset,config.units,config.language,config.country,config.speed,config.mass,config.power,config.torque,config.temperature,config.autostart,config.aspect);
    /* Original feature labels and notes contain no JSON quoting characters; verify at generation. */
    for(unsigned i=0;i<PATCH_COUNT;i++)n+=(size_t)snprintf(body+n,sizeof(body)-n,"%s{\"name\":\"%s\",\"note\":\"%s\",\"lines\":%u}",i?",":"",feature_name(i),feature_note(i),feature_count(i));
    n+=(size_t)snprintf(body+n,sizeof(body)-n,"],\"live\":{");
#ifdef PS5
    Gt7Credits credits;
    if((config.mask&LIVE_SETTINGS_BIT)&&!atomic_load(&startup_pending)&&gt7_live_ready()&&!gt7_credits_read(&credits))
        n+=(size_t)snprintf(body+n,sizeof(body)-n,"\"available\":true,\"credits\":%u,\"session\":\"%d:%llx\"",credits.amount,credits.pid,(unsigned long long)credits.object);
    else
#endif
        n+=(size_t)snprintf(body+n,sizeof(body)-n,"\"available\":false,\"credits\":null,\"session\":null");
    n+=(size_t)snprintf(body+n,sizeof(body)-n,",\"creditPrompt\":{");
#ifdef PS5
    Gt7CreditPrompt credit_prompt;
    if((config.mask&LIVE_SETTINGS_BIT)&&!atomic_load(&startup_pending)&&!gt7_live_credit_prompt_read(&credit_prompt))
        n+=(size_t)snprintf(body+n,sizeof(body)-n,"\"available\":true,\"state\":%u,\"result\":%u,\"amount\":%u,\"chars\":%u",credit_prompt.state,credit_prompt.result,credit_prompt.amount,credit_prompt.chars);
    else
#endif
        n+=(size_t)snprintf(body+n,sizeof(body)-n,"\"available\":false,\"state\":null,\"result\":null,\"amount\":null,\"chars\":null");
    n+=(size_t)snprintf(body+n,sizeof(body)-n,"}");
    n+=(size_t)snprintf(body+n,sizeof(body)-n,",\"wheelWidth\":{");
#ifdef PS5
    Gt7WheelWidth width;
    if((config.mask&LIVE_SETTINGS_BIT)&&!atomic_load(&startup_pending)&&!gt7_wheel_width_read(&width))
        n+=(size_t)snprintf(body+n,sizeof(body)-n,"\"available\":true,\"front\":%d,\"rear\":%d,\"modelCode\":%u,\"sequence\":%u",width.front,width.rear,width.model_code,width.sequence);
    else
#endif
        n+=(size_t)snprintf(body+n,sizeof(body)-n,"\"available\":false,\"front\":null,\"rear\":null,\"modelCode\":null,\"sequence\":null");
    n+=(size_t)snprintf(body+n,sizeof(body)-n,"},\"wheelGeometry\":{");
#ifdef PS5
    Gt7WheelGeometry geometry;
    if((config.mask&LIVE_SETTINGS_BIT)&&!atomic_load(&startup_pending)&&!gt7_wheel_geometry_read(&geometry))
        n+=(size_t)snprintf(body+n,sizeof(body)-n,"\"available\":true,\"inchUp\":%d,\"offsetFront\":%d,\"offsetRear\":%d,\"hubFront\":%d,\"hubRear\":%d,\"modelCode\":%u",geometry.inch_up,geometry.offset_front,geometry.offset_rear,geometry.hub_front,geometry.hub_rear,geometry.model_code);
    else
#endif
        n+=(size_t)snprintf(body+n,sizeof(body)-n,"\"available\":false,\"inchUp\":null,\"offsetFront\":null,\"offsetRear\":null,\"hubFront\":null,\"hubRear\":null,\"modelCode\":null");
    n+=(size_t)snprintf(body+n,sizeof(body)-n,"},\"engineSwap\":{");
#ifdef PS5
    Gt7EngineSwap swap;
    if((config.mask&LIVE_SETTINGS_BIT)&&!atomic_load(&startup_pending)&&!gt7_engine_swap_read(&swap))n+=(size_t)snprintf(body+n,sizeof(body)-n,"\"available\":true,\"category\":%u,\"modelCode\":%u",swap.category,swap.model_code);
    else
#endif
        n+=(size_t)snprintf(body+n,sizeof(body)-n,"\"available\":false,\"category\":null,\"modelCode\":null");
    snprintf(body+n,sizeof(body)-n,"}}}");
    response(fd,200,"application/json",body);
}
static void serve(int fd){
    /* Static: the saved HUD layout is a ~21 KB JSON document, far too big to
     * keep on a request-thread stack. serve() only ever runs on the accept
     * loop, so one buffer is safe. */
    static char req[262144+4096];char method[12],path[128],proto[16];size_t used=0;char *body=NULL;
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
            if(has_length||*end||l>262144){response(fd,400,"text/plain","Invalid Content-Length");return;}length=(size_t)l;has_length=1;
        }
        if(!strncasecmp(h,"Transfer-Encoding:",18)){response(fd,400,"text/plain","Unsupported transfer encoding");return;}
        if(!strncasecmp(h,"X-GT7-Token: ",13)&&!strcmp(h+13,token))authorized=1;
        h=e;
    }
    if(!strcmp(method,"GET")){
        if(!strcmp(path,"/"))response(fd,200,"text/html; charset=utf-8",page_html());
        else if(!strcmp(path,"/api/status"))json_status(fd);
        else if(!strcmp(path,"/api/live/credit-input")){
#ifdef PS5
            Gt7CreditPrompt prompt;
            if(gt7_live_credit_prompt_read(&prompt)){response(fd,409,"text/plain","Live numeric input is unavailable while supported GT7 is not running.");return;}
            char body[160];
            snprintf(body,sizeof(body),"{\"state\":%u,\"result\":%u,\"amount\":%u,\"chars\":%u,\"dbgGot\":%u,\"dbgEnd\":%u,\"dbgStatus\":%u,\"dbgUser\":%u}",prompt.state,prompt.result,prompt.amount,prompt.chars,prompt.dbg_got,prompt.dbg_end,prompt.dbg_status,prompt.dbg_user);
            response(fd,200,"application/json",body);return;
#else
            response(fd,409,"text/plain","Live numeric input is only available while supported GT7 is running on PS5.");return;
#endif
        }
        else if(!strcmp(path,"/api/live/hud")){
            /* In-race HUD hide list. Read-only here; the persisted selection is
             * returned along with whether the live bridge is reachable. */
            if(!authorized){response(fd,403,"text/plain","Reload this page before reading HUD state.");return;}
            static char body[LIVE_HUD_MAX*LIVE_HUD_PATH+512];int at;
#ifdef PS5
            int live=!gt7_hud_read(&hud_state);
            at=snprintf(body,sizeof(body),"{\"available\":%s,\"applied\":%u,\"sequence\":%u,\"hidden\":[",live?"true":"false",live?hud_state.applied:0,live?hud_state.sequence:0);
#else
            at=snprintf(body,sizeof(body),"{\"available\":false,\"applied\":0,\"sequence\":0,\"hidden\":[");
#endif
            for(unsigned i=0;i<hud_hidden_count&&at>0&&at<(int)sizeof(body)-128;i++)
                at+=snprintf(body+at,sizeof(body)-at,"%s\"%s\"",i?",":"",hud_hidden[i]);
            if(at>0&&at<(int)sizeof(body)-8)snprintf(body+at,sizeof(body)-at,"]}");
            response(fd,200,"application/json",body);return;
        }
        else if(!strcmp(path,"/api/live/hud/tree")){
            /* Live MENU widget tree for the UI's expandable groups. Every named
             * widget is reported with the full path the bridge matches on, its
             * depth and whether it is currently hidden. The in-car and bumper
             * root windows carry the same widgets, so duplicates are expected;
             * the page merges them by path. */
            if(!authorized){response(fd,403,"text/plain","Reload this page before reading HUD state.");return;}
#ifdef PS5
            size_t cap=512u*1024u;char *body=malloc(cap);
            if(!body){response(fd,500,"text/plain","Out of memory");return;}
            HudTreeJson j={body,cap,0,1};
            j.at=(size_t)snprintf(body,cap,"{\"available\":true,\"nodes\":[");
            if(gt7_hud_tree(hud_tree_emit,&j)){free(body);response(fd,409,"text/plain","The live widget tree is unavailable while supported GT7 is not running.");return;}
            snprintf(body+j.at,cap-j.at,"]}");
            response(fd,200,"application/json",body);free(body);return;
#else
            response(fd,409,"text/plain","The live widget tree is only available while supported GT7 is running on PS5.");return;
#endif
        }
        else if(!strcmp(path,"/api/live/hud/layout")){
            /* Saved HUD group layout. Returned verbatim so every browser shows
             * the same grouping; an empty object means nothing is saved yet and
             * the page should upload what it has. */
            if(!authorized){response(fd,403,"text/plain","Reload this page before reading the HUD layout.");return;}
            const char *empty="{\"groups\":[],\"names\":{}}";
            FILE *f=fopen(hud_layout_path,"rb");
            if(!f){response(fd,200,"application/json",empty);return;}
            long len=0;
            if(fseek(f,0,SEEK_END)||(len=ftell(f))<0||len>262144){fclose(f);response(fd,200,"application/json",empty);return;}
            rewind(f);
            char *body=malloc((size_t)len+1);
            if(!body){fclose(f);response(fd,500,"text/plain","Out of memory");return;}
            size_t n=fread(body,1,(size_t)len,f);fclose(f);body[n]=0;
            if(!n){free(body);response(fd,200,"application/json",empty);return;}
            response(fd,200,"application/json",body);free(body);return;
        }
        else if(!strncmp(path,"/api/live/dump",14)){
            /* Temporary read-only diagnostic: hex dump of a region (<=1 MiB). Remove before release. */
            if(!authorized){response(fd,403,"text/plain","Reload this page before reading live memory.");return;}
#ifdef PS5
            char *query=strchr(path,'?');unsigned long long address=0;unsigned length=0;
            if(!query||sscanf(query,"?addr=%llx&len=%u",&address,&length)!=2){response(fd,400,"text/plain","Use /api/live/dump?addr=0xSTART&len=N");return;}
            if(!length||length>1048576U){response(fd,400,"text/plain","Length must be from 1 to 1048576.");return;}
            unsigned char *raw=malloc(length);char *hex=malloc((size_t)length*2+1);static const char digits[]="0123456789abcdef";
            if(!raw||!hex){free(raw);free(hex);response(fd,500,"text/plain","Out of memory");return;}
            if(gt7_live_peek((uintptr_t)address,raw,length)){free(raw);free(hex);response(fd,409,"text/plain","Live memory is unavailable while supported GT7 is not running.");return;}
            for(unsigned i=0;i<length;i++){hex[i*2]=digits[raw[i]>>4];hex[i*2+1]=digits[raw[i]&15];}
            hex[length*2]=0;response(fd,200,"text/plain",hex);free(raw);free(hex);return;
#else
            response(fd,409,"text/plain","Live memory is only available while supported GT7 is running on PS5.");return;
#endif
        }
        else if(!strncmp(path,"/api/live/probe",15)){
            /* Temporary read-only diagnostic: last logged hook registers. Remove before release. */
            if(!authorized){response(fd,403,"text/plain","Reload this page before reading live memory.");return;}
#ifdef PS5
            uintptr_t words[128];char body[4096];int at;
            if(gt7_live_probe_read(words,128)){response(fd,409,"text/plain","Live memory is unavailable while supported GT7 is not running.");return;}
            at=snprintf(body,sizeof(body),"{\"rdi\":\"0x%llx\",\"rsi\":\"0x%llx\",\"rdx\":\"0x%llx\",\"rcx\":\"0x%llx\",\"r8\":\"0x%llx\",\"r9\":\"0x%llx\",\"rsp\":\"0x%llx\",\"count\":%llu,\"ret\":\"0x%llx\",\"rbp\":\"0x%llx\",\"stack\":[",
                (unsigned long long)words[0],(unsigned long long)words[1],(unsigned long long)words[2],(unsigned long long)words[3],(unsigned long long)words[4],(unsigned long long)words[5],(unsigned long long)words[6],(unsigned long long)words[7],(unsigned long long)words[8],(unsigned long long)words[9]);
            for(int i=0;i<48&&at>0&&at<(int)sizeof(body)-40;i++)at+=snprintf(body+at,sizeof(body)-at,"%s\"0x%llx\"",i?",":"",(unsigned long long)words[64+i]);
            if(at>0&&at<(int)sizeof(body)-40)at+=snprintf(body+at,sizeof(body)-at,"],\"snap\":[");
            for(int i=0;i<24&&at>0&&at<(int)sizeof(body)-40;i++)at+=snprintf(body+at,sizeof(body)-at,"%s\"0x%llx\"",i?",":"",(unsigned long long)words[32+i]);
            if(at>0&&at<(int)sizeof(body)-8)snprintf(body+at,sizeof(body)-at,"]}");
            response(fd,200,"application/json",body);return;
#else
            response(fd,409,"text/plain","Live memory is only available while supported GT7 is running on PS5.");return;
#endif
        }
        else if(!strcmp(path,"/api/live/maps")){
            /* Temporary read-only diagnostic: GT7 memory map. Remove before release. */
            if(!authorized){response(fd,403,"text/plain","Reload this page before reading live memory.");return;}
#ifdef PS5
            char *buffer=malloc(262144);
            if(!buffer){response(fd,500,"text/plain","Out of memory");return;}
            int n=gt7_live_mapinfo(buffer,262144);
            if(n<0){free(buffer);response(fd,409,"text/plain","Live memory is unavailable while supported GT7 is not running.");return;}
            response(fd,200,"text/plain",buffer);free(buffer);return;
#else
            response(fd,409,"text/plain","Live memory is only available while supported GT7 is running on PS5.");return;
#endif
        }
        else if(!strncmp(path,"/api/live/find",14)){
            /* Temporary read-only diagnostic: search GT7 memory for text.
             * Use /api/live/find?addr=0xSTART&len=N&text=STR&max=M
             * Remove before release. */
            if(!authorized){response(fd,403,"text/plain","Reload this page before reading live memory.");return;}
#ifdef PS5
            char *query=strchr(path,'?');unsigned long long address=0;unsigned length=0,maxhits=16;char text[128]="";
            if(!query||sscanf(query,"?addr=%llx&len=%u&text=%127[^&]&max=%u",&address,&length,text,&maxhits)!=4){response(fd,400,"text/plain","Use /api/live/find?addr=0xSTART&len=N&text=STR&max=M");return;}
            if(!length||length>67108864U||!text[0]||maxhits>64){response(fd,400,"text/plain","len must be 1..67108864, text must be non-empty, max 1..64.");return;}
            unsigned char pattern[64];uint32_t plen=0;
            if(!strncmp(text,"hex:",4)){const char *hp=text+4;for(uint32_t i=0;hp[i]&&hp[i+1]&&plen<sizeof(pattern);i+=2){unsigned v;if(sscanf(hp+i,"%2x",&v)!=1)break;pattern[plen++]=(unsigned char)v;}}
            else {plen=(uint32_t)strlen(text);memcpy(pattern,text,plen);}
            if(!plen){response(fd,400,"text/plain","Pattern must be non-empty.");return;}
            uintptr_t hits[64];uint32_t count=0;
            if(gt7_live_find((uintptr_t)address,length,(const char *)pattern,plen,hits,maxhits,&count)){response(fd,409,"text/plain","Live memory is unavailable while supported GT7 is not running.");return;}
            char reply[1200];int off=snprintf(reply,sizeof(reply),"{\"count\":%u,\"hits\":[",count);
            for(uint32_t i=0;i<count&&off<(int)sizeof(reply)-32;i++)off+=snprintf(reply+off,sizeof(reply)-off,"%s\"0x%llx\"",i?",":"",(unsigned long long)hits[i]);
            snprintf(reply+off,sizeof(reply)-off,"]}");
            response(fd,200,"application/json",reply);return;
#else
            response(fd,409,"text/plain","Live memory is only available while supported GT7 is running on PS5.");return;
#endif
        }
        else if(!strcmp(path,"/api/live/carptr")){
            /* Temporary read-only diagnostic: riding car object pointers. Remove before release. */
            if(!authorized){response(fd,403,"text/plain","Reload this page before reading live memory.");return;}
#ifdef PS5
            uintptr_t implementation=0,expanded=0,style=0;uint32_t model=0,sequence=0;
            if(gt7_live_car_pointers(&implementation,&expanded,&style,&model,&sequence)){response(fd,409,"text/plain","Live memory is unavailable while supported GT7 is not running.");return;}
            char reply[256];
            snprintf(reply,sizeof(reply),"{\"implementation\":\"0x%llx\",\"expanded\":\"0x%llx\",\"style\":\"0x%llx\",\"modelCode\":%u,\"sequence\":%u}",(unsigned long long)implementation,(unsigned long long)expanded,(unsigned long long)style,model,sequence);
            response(fd,200,"application/json",reply);return;
#else
            response(fd,409,"text/plain","Live memory is only available while supported GT7 is running on PS5.");return;
#endif
        }
        else if(!strncmp(path,"/api/live/diff",14)){
            /* Temporary read-only diagnostic: byte runs changed since the last snapshot. Remove before release. */
            if(!authorized){response(fd,403,"text/plain","Reload this page before reading live memory.");return;}
#ifdef PS5
            char *buffer=malloc(4194304);
            if(!buffer){response(fd,500,"text/plain","Out of memory");return;}
            unsigned bytes=0,runs=0;int n=gt7_live_snapshot_diff(buffer,4194304,&bytes,&runs);
            if(n<0){free(buffer);response(fd,409,"text/plain","No snapshot is stored, or live memory is unavailable.");return;}
            response(fd,200,"application/json",buffer);free(buffer);return;
#else
            response(fd,409,"text/plain","Live memory is only available while supported GT7 is running on PS5.");return;
#endif
        }
        else if(!strncmp(path,"/api/live/peek",14)){
            /* Temporary read-only diagnostic for mapping GT7 state from inside
             * the running game. Token-gated; remove before release. */
            if(!authorized){response(fd,403,"text/plain","Reload this page before reading live memory.");return;}
#ifdef PS5
            char *query=strchr(path,'?');unsigned long long address=0;unsigned len=8;
            if(!query||sscanf(query,"?addr=%llx&len=%u",&address,&len)!=2){response(fd,400,"text/plain","Use /api/live/peek?addr=0xADDR&len=N");return;}
            if(!len||len>256){response(fd,400,"text/plain","Length must be from 1 to 256.");return;}
            unsigned char buffer[256];static const char digits[]="0123456789abcdef";char hex[513];char reply[600];
            if(gt7_live_peek((uintptr_t)address,buffer,len)){response(fd,409,"text/plain","Live memory is unavailable while supported GT7 is not running.");return;}
            for(unsigned i=0;i<len;i++){hex[i*2]=digits[buffer[i]>>4];hex[i*2+1]=digits[buffer[i]&15];}
            hex[len*2]=0;
            snprintf(reply,sizeof(reply),"{\"address\":\"0x%llx\",\"length\":%u,\"hex\":\"%s\"}",address,len,hex);
            response(fd,200,"application/json",reply);return;
#else
            response(fd,409,"text/plain","Live memory reads are only available while supported GT7 is running on PS5.");return;
#endif
        }
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
    if(strncmp(path,"/api/live/probe",15)&&strcmp(path,"/api/live/snapshot")&&strcmp(path,"/api/live/snapshot/clear")&&strcmp(path,"/api/config")&&strcmp(path,"/api/live/credits")&&strcmp(path,"/api/live/credit-input")&&strcmp(path,"/api/live/wheel-width")&&strcmp(path,"/api/live/wheel-geometry")&&strcmp(path,"/api/live/engine-swap")&&strncmp(path,"/api/live/hud",13)&&strcmp(path,"/api/start")&&strcmp(path,"/api/game/start")&&strcmp(path,"/api/shortcut/reinstall")&&strcmp(path,"/api/engine/stop")&&strcmp(path,"/api/payload/stop")){response(fd,404,"text/plain","Not found");return;}
#ifdef PS5
    if(atomic_load(&startup_pending)){response(fd,409,"text/plain","Startup is still running. Please retry shortly; the browser page remains available.");return;}
#endif
    if(!strncmp(path,"/api/live/probe",15)){
        /* Temporary diagnostic: install/remove a logging hook. Remove before release. */
#ifdef PS5
        if(!strcmp(path,"/api/live/probe/remove")){
            if(length){response(fd,400,"text/plain","Unexpected request body");return;}
            if(gt7_live_probe_remove()){response(fd,409,"text/plain","Could not remove the probe; the bridge may be unavailable.");return;}
            response(fd,200,"application/json","{\"removed\":true}");return;
        }
        char *query=strchr(path,'?');unsigned long long address=0;unsigned displaced=0;unsigned mode=0;unsigned long long watch=0;
        if(!query||sscanf(query,"?addr=%llx&len=%u",&address,&displaced)!=2){response(fd,400,"text/plain","Use /api/live/probe?addr=0xADDR&len=N");return;}
        if(length){response(fd,400,"text/plain","Unexpected request body");return;}
        {const char *mp=strstr(query,"&mode=");if(mp)mode=(unsigned)strtoul(mp+6,NULL,10);}
        {const char *wp=strstr(query,"&watch=");if(wp)watch=strtoull(wp+7,NULL,0);}
        if(gt7_live_probe_install((uintptr_t)address,displaced,mode,(uintptr_t)watch)){response(fd,409,"text/plain","Could not install the probe at that address.");return;}
        response(fd,200,"application/json","{\"installed\":true}");return;
#else
        response(fd,409,"text/plain","Probes are only available while supported GT7 is running on PS5.");return;
#endif
    }
    if(!strcmp(path,"/api/live/snapshot/clear")){
        /* Temporary diagnostic: drop the stored snapshot. Remove before release. */
        if(length){response(fd,400,"text/plain","Unexpected request body");return;}
#ifdef PS5
        gt7_live_snapshot_clear();
        response(fd,200,"application/json","{\"cleared\":true}");return;
#else
        response(fd,409,"text/plain","Snapshots are only available while supported GT7 is running on PS5.");return;
#endif
    }
    if(!strcmp(path,"/api/live/snapshot")){
        /* Temporary diagnostic: store a memory snapshot in the payload. Remove before release. */
        if(length){response(fd,400,"text/plain","Unexpected request body");return;}
#ifdef PS5
        char *query=strchr(path,'?');unsigned long long address=0;unsigned len=0;
        if(!query||sscanf(query,"?addr=%llx&len=%u",&address,&len)!=2){response(fd,400,"text/plain","Use /api/live/snapshot?addr=0xSTART&len=N");return;}
        int result=gt7_live_snapshot((uintptr_t)address,len);
        if(result==-1){response(fd,500,"text/plain","Could not allocate the snapshot buffer.");return;}
        if(result){response(fd,409,"text/plain","Live memory is unavailable, or the length is out of range (max 536870912).");return;}
        char reply[128];snprintf(reply,sizeof(reply),"{\"stored\":true,\"addr\":\"0x%llx\",\"len\":%u}",address,len);
        response(fd,200,"application/json",reply);return;
#else
        response(fd,409,"text/plain","Snapshots are only available while supported GT7 is running on PS5.");return;
#endif
    }
    if(!strcmp(path,"/api/live/credits")){
        /* Fixed field order and canonical decimal values, with bounded input
         * before scanf, prevent signs, overflow, duplicate or extra fields. */
        int consumed=0;unsigned long long amount,expected,pid,object;
        char canonical[128];
        if(length>=sizeof(canonical)||sscanf(body,"amount=%10llu&expected=%10llu&session=%10llu:%16llx%n",&amount,&expected,&pid,&object,&consumed)!=4||consumed!=(int)length||
           amount>100000000U||expected>INT32_MAX||!pid||pid>INT32_MAX){response(fd,400,"text/plain","Use a whole credit amount from 0 to 100000000 and a valid game session.");return;}
        snprintf(canonical,sizeof(canonical),"amount=%llu&expected=%llu&session=%llu:%llx",amount,expected,pid,object);
        if(strcmp(body,canonical)){response(fd,400,"text/plain","Invalid credit amount or session.");return;}
        if(!(config.mask&LIVE_SETTINGS_BIT)){response(fd,409,"text/plain","Select and save the Live Settings patch first.");return;}
#ifdef PS5
        Gt7Credits before={(int)pid,object,(uint32_t)expected};int result=gt7_credits_set(&before,(uint32_t)amount);
        if(result){response(fd,409,"text/plain",result==-3?"The game has not acknowledged the credit request yet. Do not resubmit; wait for the live balance to refresh.":result==-2?"Credits changed since the last refresh. Refresh the balance and apply again.":"Live bridge is unavailable or the native credit update could not be verified. Start GT7 with the Live Settings patch selected.");return;}
        strcpy(message,"Live credit amount applied.");json_status(fd);return;
#else
        response(fd,409,"text/plain","Live credits are only available while supported GT7 is running on PS5.");return;
#endif
    }
    if(!strcmp(path,"/api/live/credit-input")){
        if(length){response(fd,400,"text/plain","Unexpected request body");return;}
        if(!(config.mask&LIVE_SETTINGS_BIT)){response(fd,409,"text/plain","Select and save the Live Settings patch first.");return;}
#ifdef PS5
        int result=gt7_live_credit_prompt_begin();
        if(result){response(fd,409,"text/plain","Live numeric input is unavailable while supported GT7 is not running.");return;}
        response(fd,200,"application/json","{\"opening\":true}");return;
#else
        response(fd,409,"text/plain","Live numeric input is only available while supported GT7 is running on PS5.");return;
#endif
    }
    if(!strcmp(path,"/api/live/wheel-width")){
        int consumed=0,front,rear,expected_front,expected_rear;
        unsigned model_code,sequence;
        char canonical[160];
        if(length>=sizeof(canonical)||sscanf(body,"front=%d&rear=%d&expectedFront=%d&expectedRear=%d&modelCode=%u&sequence=%u%n",&front,&rear,&expected_front,&expected_rear,&model_code,&sequence,&consumed)!=6||consumed!=(int)length||
           front<INT8_MIN||front>INT8_MAX||rear<INT8_MIN||rear>INT8_MAX||expected_front<INT8_MIN||expected_front>INT8_MAX||expected_rear<INT8_MIN||expected_rear>INT8_MAX||!model_code||!sequence){response(fd,400,"text/plain","Use signed wheel-width values from -128 to 127 and the current car sample.");return;}
        snprintf(canonical,sizeof(canonical),"front=%d&rear=%d&expectedFront=%d&expectedRear=%d&modelCode=%u&sequence=%u",front,rear,expected_front,expected_rear,model_code,sequence);
        if(strcmp(body,canonical)){response(fd,400,"text/plain","Invalid wheel-width request.");return;}
        if(!(config.mask&LIVE_SETTINGS_BIT)){response(fd,409,"text/plain","Select and save the Live Settings patch first.");return;}
#ifdef PS5
        Gt7WheelWidth before={0,model_code,sequence,(int8_t)expected_front,(int8_t)expected_rear};
        int result=gt7_wheel_width_set(&before,(int8_t)front,(int8_t)rear);
        if(result){response(fd,409,"text/plain",result==-3?"The game has not acknowledged the wheel-width request yet. Do not resubmit; wait for the values to refresh.":result==-2?"The selected car or its widths changed since the last refresh. Refresh and retry.":"Live bridge is unavailable or the native garage update could not be verified. Start GT7 with the Live Settings patch selected.");return;}
        strcpy(message,"Wheel widths applied through GT7 garage data.");json_status(fd);return;
#else
        response(fd,409,"text/plain","Wheel-width editing is only available while supported GT7 is running on PS5.");return;
#endif
    }
    if(!strcmp(path,"/api/live/wheel-geometry")){
        int inch,of,orr,hf,hr,einch,eof,eor,ehf,ehr,consumed=0;unsigned model;char canonical[256];
        if(length>=sizeof(canonical)||sscanf(body,"inch=%d&offsetFront=%d&offsetRear=%d&hubFront=%d&hubRear=%d&expectedInch=%d&expectedOffsetFront=%d&expectedOffsetRear=%d&expectedHubFront=%d&expectedHubRear=%d&modelCode=%u%n",&inch,&of,&orr,&hf,&hr,&einch,&eof,&eor,&ehf,&ehr,&model,&consumed)!=11||consumed!=(int)length||inch<INT8_MIN||inch>INT8_MAX||einch<INT8_MIN||einch>INT8_MAX||of<INT16_MIN||of>INT16_MAX||orr<INT16_MIN||orr>INT16_MAX||hf<INT16_MIN||hf>INT16_MAX||hr<INT16_MIN||hr>INT16_MAX||eof<INT16_MIN||eof>INT16_MAX||eor<INT16_MIN||eor>INT16_MAX||ehf<INT16_MIN||ehf>INT16_MAX||ehr<INT16_MIN||ehr>INT16_MAX||!model){response(fd,400,"text/plain","Use signed wheel geometry values within the displayed ranges.");return;}
        snprintf(canonical,sizeof(canonical),"inch=%d&offsetFront=%d&offsetRear=%d&hubFront=%d&hubRear=%d&expectedInch=%d&expectedOffsetFront=%d&expectedOffsetRear=%d&expectedHubFront=%d&expectedHubRear=%d&modelCode=%u",inch,of,orr,hf,hr,einch,eof,eor,ehf,ehr,model);
        if(strcmp(body,canonical)){response(fd,400,"text/plain","Invalid wheel-geometry request.");return;}
        if(!(config.mask&LIVE_SETTINGS_BIT)){response(fd,409,"text/plain","Select and save the Live Settings patch first.");return;}
#ifdef PS5
        Gt7WheelGeometry before={0,model,(int8_t)einch,(int16_t)eof,(int16_t)eor,(int16_t)ehf,(int16_t)ehr};int result=gt7_wheel_geometry_set(&before,(int8_t)inch,(int16_t)of,(int16_t)orr,(int16_t)hf,(int16_t)hr);
        if(result){response(fd,409,"text/plain",result==-3?"The game has not acknowledged the wheel-geometry request yet. Do not resubmit; wait for the values to refresh.":result==-2?"The selected car or its wheel geometry changed since the last refresh. Refresh and retry.":"The native garage update could not be verified. Wheel-offset changes are limited to 127 steps per apply.");return;}
        strcpy(message,"Wheel geometry applied through GT7 garage data.");json_status(fd);return;
#else
        response(fd,409,"text/plain","Wheel geometry editing is only available while supported GT7 is running on PS5.");return;
#endif
    }
    if(!strcmp(path,"/api/live/hud")){
        /* Body: newline-separated MENU widget paths or names to hide. An empty
         * body clears the list. The selection is persisted and pushed live. */
        static char names[LIVE_HUD_MAX][LIVE_HUD_PATH];unsigned count=0;int too_many=0;
        for(char *p=body;*p;){
            char *e=strchr(p,'\n');size_t n=e?(size_t)(e-p):strlen(p);
            while(n&&(p[n-1]=='\r'||p[n-1]==' '))n--;
            if(n&&n<LIVE_HUD_PATH){
                int ok=1;
                for(size_t i=0;i<n;i++){unsigned char c=(unsigned char)p[i];if(c<0x20||c>0x7e)ok=0;}
                if(ok){if(count<LIVE_HUD_MAX){memcpy(names[count],p,n);names[count][n]=0;count++;}else too_many=1;}
            }
            if(!e)break;
            p=e+1;
        }
        if(too_many){char m[80];snprintf(m,sizeof(m),"Too many HUD entries; at most %u can be hidden.",(unsigned)LIVE_HUD_MAX);response(fd,400,"text/plain",m);return;}
        memcpy(hud_hidden,names,sizeof(hud_hidden));
        hud_hidden_count=count;
        if(hud_save()){response(fd,500,"text/plain","Could not save the HUD selection.");return;}
        hud_push();
        response(fd,200,"text/plain","HUD selection saved.");return;
    }
    if(!strcmp(path,"/api/live/hud/layout")){
        /* Body: the JSON group layout the page keeps ({groups,names}). Stored
         * verbatim; independent of whether GT7 is running. */
        if(!length){response(fd,400,"text/plain","Expected a layout body");return;}
        if(body[0]!='{'){response(fd,400,"text/plain","Expected a JSON object");return;}
        FILE *f=fopen(hud_layout_path,"wb");
        if(!f){response(fd,500,"text/plain","Could not save the HUD layout.");return;}
        size_t written=fwrite(body,1,length,f);
        int failed=fclose(f)!=0||written!=length;
        if(failed){response(fd,500,"text/plain","Could not save the HUD layout.");return;}
        response(fd,200,"application/json","{\"saved\":true}");return;
    }
    if(!strcmp(path,"/api/live/engine-swap")){
        unsigned category,expected,unlock,model;int consumed=0;char canonical[96];
        if(length>=sizeof(canonical)||sscanf(body,"category=%u&expected=%u&unlock=%u&modelCode=%u%n",&category,&expected,&unlock,&model,&consumed)!=4||consumed!=(int)length||category>2||expected>2||unlock>1||!model){response(fd,400,"text/plain","Use an engine swap slot from 0 to 2.");return;}
        snprintf(canonical,sizeof(canonical),"category=%u&expected=%u&unlock=%u&modelCode=%u",category,expected,unlock,model);if(strcmp(body,canonical)){response(fd,400,"text/plain","Invalid engine-swap request.");return;}
        if(!(config.mask&LIVE_SETTINGS_BIT)){response(fd,409,"text/plain","Select and save the Live Settings patch first.");return;}
#ifdef PS5
        Gt7EngineSwap before={0,model,(uint8_t)expected};int result=gt7_engine_swap_set(&before,(uint8_t)category,(int)unlock);
        if(result){response(fd,409,"text/plain",result==-3?"The game has not acknowledged the engine request yet. Do not resubmit; wait for the value to refresh.":result==-2?"The selected car or engine swap changed since the last refresh. Refresh and retry.":"GT7 rejected that swap slot. The car may not support it.");return;}
        strcpy(message,"Engine swap applied through GT7 garage data.");json_status(fd);return;
#else
        response(fd,409,"text/plain","Engine swapping is only available while supported GT7 is running on PS5.");return;
#endif
    }
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
    if(!strcmp(path,"/api/start")){
        if(length){response(fd,400,"text/plain","Unexpected request body");return;}
        int game=running_gt7();if(game){response(fd,409,"text/plain",game>0?"Fully close GT7 before starting the engine.":"Cannot establish that GT7 is closed; engine was not started.");return;}
        if(start_engine()){response(fd,409,"text/plain",message);return;}
    }else{
        Config next;if(parse_config(body,&next)){response(fd,400,"text/plain","Invalid settings.");return;}
        if(save_config(root,&next)){response(fd,500,"text/plain","Could not save the patch snapshot; previous settings retained.");return;}
        config=next;
        int game=running_gt7();
        if(game>0)strcpy(message,"Choices saved. They will apply the next time GT7 starts.");
        else strcpy(message,engine_ready()?"Choices saved. Launch GT7 to apply them.":"Choices saved. Start the engine, then launch GT7.");
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
#ifdef GT7_LIVE_TEST
    /* Diagnostic build only: use a separate port and leave the running
     * payload's ShellCore launch hook and persisted choices untouched. */
    config.mask|=LIVE_SETTINGS_BIT;
    extern int gt7_live_test_attach(void);
    strcpy(message,gt7_live_test_attach()?"Live bridge test installation failed.":"Live bridge test installed. Waiting for the game UI callback.");
#else
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
#endif
    printf("GT7Patcher %s on port %u\n",VERSION,port);fflush(stdout);
    while(!stopping){gt7_live_pad_configure((config.mask&LIVE_SETTINGS_BIT)!=0);fd_set set;FD_ZERO(&set);FD_SET(s,&set);struct timeval timeout={1,0};
        int r=select(s+1,&set,NULL,NULL,&timeout);if(r<0){if(errno==EINTR)continue;break;}if(!r){gt7_live_pad_poll();hud_tick();gt7_live_credit_auto_poll();continue;}
        int c=accept(s,NULL,NULL);if(c<0)continue;serve(c);close(c);
    }
    close(s);
#ifdef PS5
    if(startup_thread_created)pthread_join(startup_thread,NULL);
    (void)gt7_engine_stop();
#endif
    return 0;
}
