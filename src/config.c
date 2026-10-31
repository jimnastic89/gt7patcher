/* SPDX-License-Identifier: GPL-3.0-only */
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include "assets.h"
#include "live_bridge.h"

static pthread_mutex_t config_io_lock=PTHREAD_MUTEX_INITIALIZER;

/* The country patch needs a code cave in executable padding. That padding
 * is shared with the payload's live-settings bootstrap, so the cave must
 * sit beyond the reserved bootstrap region. */
enum {COUNTRY_CAVE=0x430ea10};
_Static_assert(COUNTRY_CAVE>=LIVE_IMAGE_BASE+LIVE_BOOT_RVA+LIVE_BOOT_RESERVED,
               "country code cave overlaps the live-settings bootstrap region");

/* Player-country table. Order MUST match the <select id="country"> options in
 * web/index.html. Each entry drives the in-race flag override. */
static const char *const country_codes[COUNTRY_COUNT]={
    "AU","AT","BE","BR","CA","CH","CL","CN","CZ","DE","DK","ES","FI","FR","GB",
    "GR","HK","HR","HU","ID","IE","IL","IN","IT","JP","KR","MX","MY","NL","NO",
    "NZ","PH","PL","PT","RO","RU","SE","SG","SK","TH","TR","TW","UA","US","ZA","AR"};
const char *country_code(int i){
    return (i>=0&&i<COUNTRY_COUNT)?country_codes[i]:"";
}

Config defaults(void){return (Config){1,1,1,2,-1,0,0,2,2,1,0};}
int valid_config(const Config *c){
    return c->mask<=(1U<<PATCH_COUNT)-1 && (!(c->mask&4)||(c->mask&1)) &&
        c->preset>=0&&c->preset<=5&&c->units>=0&&c->units<=1&&
        c->language>=-1&&c->language<=26&&c->country>=-1&&c->country<COUNTRY_COUNT&&
        c->speed>=0&&c->speed<=1&&
        c->mass>=0&&c->mass<=1&&c->power>=0&&c->power<=2&&
        c->torque>=0&&c->torque<=2&&c->temperature>=1&&c->temperature<=2&&c->autostart>=0&&c->autostart<=1;
}
int parse_config(const char *body,Config *c){
    unsigned seen=0; char copy[512];
    if(strlen(body)>=sizeof(copy))return -1;
    strcpy(copy,body);memset(c,0,sizeof(*c));c->country=-1;
    for(char *item=strtok(copy,"&");item;item=strtok(NULL,"&")){
        char *eq=strchr(item,'=');if(!eq)return -1;*eq++=0;
        if(!*eq)return -1;
        char *end;long v=strtol(eq,&end,10);
        if(*end||v < -1||v>255)return -1;
        const char *keys[]={"mask","preset","units","language","speed","mass","power","torque","temperature","autostart","country"};
        int k;for(k=0;k<11;k++)if(!strcmp(item,keys[k]))break;
        if(k==11||(seen&(1U<<k)))return -1;
        seen|=1U<<k;
        switch(k){
          case 0:c->mask=(unsigned)v;break;case 1:c->preset=(int)v;break;
          case 2:c->units=(int)v;break;case 3:c->language=(int)v;break;
          case 4:c->speed=(int)v;break;case 5:c->mass=(int)v;break;
          case 6:c->power=(int)v;break;case 7:c->torque=(int)v;break;
          case 8:c->temperature=(int)v;break;case 9:c->autostart=(int)v;break;
          case 10:c->country=(int)v;break;
        }
    }
    /* Accept the full 11-key payload, plus the 9/10-key payloads written by
     * older builds (which simply had no country field). */
    unsigned full=(1U<<11)-1;
    return (seen==full||seen==((1U<<10)-1)||seen==((1U<<9)-1))&&valid_config(c)?0:-1;
}
static void line(FILE *f,uint32_t a,const unsigned char *b,size_t n){
    fprintf(f,"<Line Type=\"bytes\" Address=\"0x%08x\" Value=\"",a);
    for(size_t i=0;i<n;i++)fprintf(f,"%02x",b[i]);
    fputs("\"/>\n",f);
}
static void store(unsigned char *b,uint32_t a,uint32_t target,uint32_t value){
    b[0]=0xc7;b[1]=5;int32_t displacement=(int32_t)(target-(a+10));
    memcpy(b+2,&displacement,4);memcpy(b+6,&value,4);
}
static void put32(unsigned char *b,int32_t v){memcpy(b,&v,4);}
int emit_xml(FILE *f,const Config *c){
    if(!valid_config(c))return -1;
    fputs("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<Patch><TitleID><ID>PPSA01317</ID></TitleID>\n"
        "<Metadata Title=\"Gran Turismo 7\" Name=\"GT7 selected patches\" Author=\"AI-generated; based on High-Speed007 and PSVIETHOA; GT7Patcher\" PatchVer=\"1.0\" AppVer=\"01.710.000\" ImageBase=\"0x00400000\" AppElf=\"eboot.bin\"><PatchList>\n",f);
    for(unsigned i=0;i<PATCH_COUNT;i++)if(c->mask&(1U<<i)){
        fprintf(f,"<!-- GT7FEATURE %u -->\n",i);fputs(patch_lines[i],f);
    }
    if(c->mask&LIVE_SETTINGS_BIT)fputs("<!-- GT7LIVEHOOK 1 -->\n",f);
    if(c->units){
        fputs("<!-- GT7FEATURE units -->\n",f);
        unsigned char b[71];uint32_t values[]={c->speed,c->power,c->torque,3,c->temperature,0,c->mass};
        for(unsigned i=0;i<7;i++)store(b+i*10,0xc22060+i*10,0x611aecc+i*4,values[i]);
        b[70]=0xc3;line(f,0xc22060,b,sizeof(b));
        const uint32_t addresses[]={0x1c4e7f0,0x1c4e800,0x1c4e810,0x1c4e820};
        const uint32_t targets[]={0x611aed0,0x611aedc,0x611aed4,0x611aecc};
        const uint32_t v[]={c->power,c->temperature,c->torque,c->speed};
        for(unsigned i=0;i<4;i++){store(b,addresses[i],targets[i],v[i]);b[10]=0xc3;line(f,addresses[i],b,11);}
    }
    if(c->language>=0){
        fputs("<!-- GT7FEATURE language -->\n",f);
        unsigned char b[]={0xbf,0,0,0,0};uint32_t lang=(uint32_t)c->language;memcpy(b+1,&lang,4);
        line(f,0xc21e93,b,5);b[0]=0xeb;b[1]=3;line(f,0xc21e98,b,2);
    }
    if(c->country>=0){
        const char *cc=country_code(c->country);
        fputs("<!-- GT7FEATURE country -->\n",f);
        /* Offline the local row's nationalFlag_ is "UN" (little-endian
         * 0x4e55): there is no service-account country. The typed
         * Dashboard::InGarageRank move assignment at 0x0117bfd0 copies that
         * 0x20-byte field from its source to the HUD row. Redirect the
         * instruction immediately after the nationalFlag_ copy, substitute
         * only the "UN" fallback in the destination, then execute the
         * displaced manufLogo_ copy. This is the race-entry leaderboard sink;
         * real AI country keys remain untouched. */
        const uint32_t site=0x117c098,cave=COUNTRY_CAVE,back=0x117c0a0;
        unsigned char jump[8];jump[0]=0xe9;put32(jump+1,(int32_t)(cave-(site+5)));
        memset(jump+5,0x90,sizeof(jump)-5);line(f,site,jump,sizeof(jump));
        unsigned char s[48];size_t n=0;
        s[n++]=0x66;s[n++]=0x81;s[n++]=0xb9;s[n++]=0x80;s[n++]=0;s[n++]=0;s[n++]=0;s[n++]=0x55;s[n++]=0x4e; /* cmp word [rcx+0x80],0x4e55 */
        s[n++]=0x75;s[n++]=0x09;                                                                             /* jne +9 (keep real code) */
        s[n++]=0x66;s[n++]=0xc7;s[n++]=0x81;s[n++]=0x80;s[n++]=0;s[n++]=0;s[n++]=0;                         /* mov word [rcx+0x80],imm16 */
        s[n++]=(unsigned char)cc[0];s[n++]=(unsigned char)cc[1];
        s[n++]=0xc5;s[n++]=0xfc;s[n++]=0x11;s[n++]=0x81;s[n++]=0xa0;s[n++]=0;s[n++]=0;s[n++]=0;             /* vmovups ymmword [rcx+0xa0],ymm0 */
        size_t off=n;s[n++]=0xe9;put32(s+n,(int32_t)(back-(cave+off+5)));n+=4;                              /* jmp 0x0117c0a0 */
        line(f,cave,s,n);
    }
    fputs("</PatchList></Metadata>\n",f);
    fprintf(f,"<!-- GT7CONFIG %u %d %d %d %d %d %d %d %d %d %d -->\n",c->mask,c->preset,c->units,c->language,c->speed,c->mass,c->power,c->torque,c->temperature,c->autostart,c->country);
    fputs("</Patch>\n",f);
    return ferror(f)?-1:0;
}
static int save_config_unlocked(const char *root,const Config *c){
    char path[512],temp[520];
    snprintf(path,sizeof(path),"%s/xml_prospero/PPSA01317.xml",root);
    snprintf(temp,sizeof(temp),"%s.tmp",path);
    FILE *f=fopen(temp,"wb");if(!f)return -1;
    int r=emit_xml(f,c);if(fflush(f)||fsync(fileno(f)))r=-1;if(fclose(f))r=-1;
    if(r||rename(temp,path)){unlink(temp);return -1;}
    /* Persist the UI state inside the same atomic XML snapshot. The engine ignores this comment. */
    return 0;
}
int save_config(const char *root,const Config *c){
    int r;pthread_mutex_lock(&config_io_lock);r=save_config_unlocked(root,c);pthread_mutex_unlock(&config_io_lock);return r;
}
static int load_config_unlocked(const char *root,Config *c){
    char path[512],buf[512];snprintf(path,sizeof(path),"%s/xml_prospero/PPSA01317.xml",root);
    FILE *f=fopen(path,"rb");if(!f)return -1;
    int r=-1,has_race_info=0,has_debug_environment=0,has_live_hook=0,has_opening_movie_skip=0;Config loaded={0};unsigned saved_mask=0;
    while(fgets(buf,sizeof(buf),f)){
        if(strstr(buf,"<!-- GT7LIVEHOOK 1 -->"))has_live_hook=1;
        if(strstr(buf,"<!-- GT7FEATURE 7 -->"))has_opening_movie_skip=1;
        if(strstr(buf,"Name=\"Race Info fifth blank option (experimental)\""))has_race_info=1;
        if(strstr(buf,"Address=\"0x060ec3b0\"")||strstr(buf,"Address=\"0x060e83b0\"")||strstr(buf,"Address=\"0x05ce83b0\""))has_debug_environment=1;
        unsigned mask;Config x={0};x.country=-1;
        int fields=sscanf(buf,"<!-- GT7CONFIG %u %d %d %d %d %d %d %d %d %d %d",&mask,&x.preset,&x.units,&x.language,&x.speed,&x.mass,&x.power,&x.torque,&x.temperature,&x.autostart,&x.country);
        if(fields>=9&&fields<=11){
            /* HUD builds may save bit 5 for their experimental Race Info
             * option and bit 6 for Wheel Offset. Preserve the supported
             * Wheel Offset choice, discard the unsupported experiment. */
            if(mask<=0xffU){x.mask=0;if(valid_config(&x)){loaded=x;saved_mask=mask;r=0;}}
        }
    }
    fclose(f);
    if(r)return r;
    unsigned normalized=saved_mask&0x1fU;
    /* Bit 6 is Wheel Offset in HUD snapshots, but was Debug environment in
     * native snapshots. Debug environment remains in source only, so discard
     * that saved selection while preserving the independent wheel option. */
    if((saved_mask&0x40U)&&has_live_hook)normalized|=LIVE_SETTINGS_BIT;
    else if((saved_mask&0x40U)&&!has_live_hook&&!has_debug_environment)normalized|=0x20U;
    if((saved_mask&0x20U)&&(!has_race_info||has_debug_environment))normalized|=0x20U;
    if((saved_mask&OPENING_MOVIE_SKIP_BIT)&&has_opening_movie_skip)normalized|=OPENING_MOVIE_SKIP_BIT;
    loaded.mask=normalized;
    if(!valid_config(&loaded))return -1;
    *c=loaded;return 0;
}
int load_config(const char *root,Config *c){
    int r;pthread_mutex_lock(&config_io_lock);r=load_config_unlocked(root,c);pthread_mutex_unlock(&config_io_lock);return r;
}
const char *feature_name(unsigned i){return patch_names[i];}
const char *feature_note(unsigned i){return patch_notes[i];}
unsigned feature_count(unsigned i){return patch_counts[i];}
const char *page_html(void){return web_page;}
