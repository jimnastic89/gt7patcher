/* SPDX-License-Identifier: GPL-3.0-only */
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "assets.h"

Config defaults(void){return (Config){1,1,1,2,0,0,2,2,1,0};}
int valid_config(const Config *c){
    return c->mask<=31 && (!(c->mask&4)||(c->mask&1)) &&
        c->preset>=0&&c->preset<=5&&c->units>=0&&c->units<=1&&
        c->language>=-1&&c->language<=26&&c->speed>=0&&c->speed<=1&&
        c->mass>=0&&c->mass<=1&&c->power>=0&&c->power<=2&&
        c->torque>=0&&c->torque<=2&&c->temperature>=1&&c->temperature<=2&&c->autostart>=0&&c->autostart<=1;
}
int parse_config(const char *body,Config *c){
    unsigned seen=0; char copy[512];
    if(strlen(body)>=sizeof(copy))return -1;
    strcpy(copy,body);memset(c,0,sizeof(*c));
    for(char *item=strtok(copy,"&");item;item=strtok(NULL,"&")){
        char *eq=strchr(item,'=');if(!eq)return -1;*eq++=0;
        if(!*eq)return -1;
        char *end;long v=strtol(eq,&end,10);
        if(*end||v < -1||v>31)return -1;
        const char *keys[]={"mask","preset","units","language","speed","mass","power","torque","temperature","autostart"};
        int k;for(k=0;k<10;k++)if(!strcmp(item,keys[k]))break;
        if(k==10||(seen&(1U<<k)))return -1;
        seen|=1U<<k;
        switch(k){
          case 0:c->mask=(unsigned)v;break;case 1:c->preset=(int)v;break;
          case 2:c->units=(int)v;break;case 3:c->language=(int)v;break;
          case 4:c->speed=(int)v;break;case 5:c->mass=(int)v;break;
          case 6:c->power=(int)v;break;case 7:c->torque=(int)v;break;
          case 8:c->temperature=(int)v;break;
          case 9:c->autostart=(int)v;break;
        }
    }
    return (seen==511||seen==1023)&&valid_config(c)?0:-1;
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
int emit_xml(FILE *f,const Config *c){
    if(!valid_config(c))return -1;
    fputs("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<Patch><TitleID><ID>PPSA01317</ID></TitleID>\n"
        "<Metadata Title=\"Gran Turismo 7\" Name=\"GT7 selected patches\" Author=\"AI-generated; based on High-Speed007 and PSVIETHOA; GT7Patcher\" PatchVer=\"1.0\" AppVer=\"01.710.000\" ImageBase=\"0x00400000\" AppElf=\"eboot.bin\"><PatchList>\n",f);
    for(unsigned i=0;i<5;i++)if(c->mask&(1U<<i))fputs(patch_lines[i],f);
    if(c->units){
        unsigned char b[71];uint32_t values[]={c->speed,c->power,c->torque,3,c->temperature,0,c->mass};
        for(unsigned i=0;i<7;i++)store(b+i*10,0xc22060+i*10,0x611aecc+i*4,values[i]);
        b[70]=0xc3;line(f,0xc22060,b,sizeof(b));
        const uint32_t addresses[]={0x1c4e7f0,0x1c4e800,0x1c4e810,0x1c4e820};
        const uint32_t targets[]={0x611aed0,0x611aedc,0x611aed4,0x611aecc};
        const uint32_t v[]={c->power,c->temperature,c->torque,c->speed};
        for(unsigned i=0;i<4;i++){store(b,addresses[i],targets[i],v[i]);b[10]=0xc3;line(f,addresses[i],b,11);}
    }
    if(c->language>=0){
        unsigned char b[]={0xbf,0,0,0,0};uint32_t lang=(uint32_t)c->language;memcpy(b+1,&lang,4);
        line(f,0xc21e93,b,5);b[0]=0xeb;b[1]=3;line(f,0xc21e98,b,2);
    }
    fputs("</PatchList></Metadata>\n",f);
    fprintf(f,"<!-- GT7CONFIG %u %d %d %d %d %d %d %d %d %d -->\n",c->mask,c->preset,c->units,c->language,c->speed,c->mass,c->power,c->torque,c->temperature,c->autostart);
    fputs("</Patch>\n",f);
    return ferror(f)?-1:0;
}
int save_config(const char *root,const Config *c){
    char path[512],temp[520];
    snprintf(path,sizeof(path),"%s/xml_prospero/PPSA01317.xml",root);
    snprintf(temp,sizeof(temp),"%s.tmp",path);
    FILE *f=fopen(temp,"wb");if(!f)return -1;
    int r=emit_xml(f,c);if(fflush(f)||fsync(fileno(f)))r=-1;if(fclose(f))r=-1;
    if(r||rename(temp,path)){unlink(temp);return -1;}
    /* Persist the UI state inside the same atomic XML snapshot. The engine ignores this comment. */
    return 0;
}
int load_config(const char *root,Config *c){
    char path[512],buf[512];snprintf(path,sizeof(path),"%s/xml_prospero/PPSA01317.xml",root);
    FILE *f=fopen(path,"rb");if(!f)return -1;
    int r=-1;while(fgets(buf,sizeof(buf),f)){
        unsigned mask;Config x={0};
        int fields=sscanf(buf,"<!-- GT7CONFIG %u %d %d %d %d %d %d %d %d %d",&mask,&x.preset,&x.units,&x.language,&x.speed,&x.mass,&x.power,&x.torque,&x.temperature,&x.autostart);
        if(fields==9||fields==10){
            x.mask=mask;if(valid_config(&x)){*c=x;r=0;}break;
        }
    }fclose(f);return r;
}
const char *feature_name(unsigned i){return patch_names[i];}
const char *feature_note(unsigned i){return patch_notes[i];}
unsigned feature_count(unsigned i){return patch_counts[i];}
const char *page_html(void){return web_page;}
