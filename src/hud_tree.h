/* SPDX-License-Identifier: GPL-3.0-only
 * Version-locked GT7 01.710.000 MENU widget-tree layout, shared by the live
 * bridge (which hides widgets inside the game) and the payload (which dumps the
 * tree for the web UI). Both must build widget paths identically or a selection
 * made in the UI will never match at runtime. */
#pragma once
#include <stdint.h>
#include "live_bridge.h"
#define HUD_OFF_NAME      0x28u
#define HUD_OFF_CHILDREN  0x160u
#define HUD_OFF_RECT      0x238u
#define HUD_RECT_LEN      0x20u
#define HUD_NLW_RVA       0x05183b20u
#define HUD_UPDATE_VT_RVA 0x051beb78u
#define HUD_RENDER_VT_RVA 0x051beff0u
#define HUD_ROOT_VT_RVA   0x0518de50u
#define HUD_MAX_DEPTH     24u
typedef struct {char buf[LIVE_HUD_PATH];unsigned len;} HudPath;
static inline int hud_ptr(uintptr_t p){return p>=0x10000u&&p<=UINT64_C(0x7fffffffffff);}
/* A widget name is a short printable ASCII identifier. Anything else - a root
 * window's name field holds pointer bytes, not a string - is rejected so it can
 * never enter a path. */
static inline int hud_name_ok(const char *s,unsigned n){
    if(!n||n>=LIVE_HUD_NAME)return 0;
    for(unsigned i=0;i<n;i++)if((unsigned char)s[i]<0x21||(unsigned char)s[i]>0x7e)return 0;
    return 1;
}
/* Append "[/]name". Returns 0 (leaving the path unchanged) when the name is
 * empty or the path is already at its limit; the caller then falls back to
 * matching the widget by its bare name. */
static inline int hud_path_push(HudPath *p,const char *name){
    unsigned n=0;while(name[n]&&n<LIVE_HUD_NAME-1)n++;
    if(!n)return 0;
    unsigned add=(p->len?1u:0u)+n;
    if(p->len+add>=LIVE_HUD_PATH)return 0;
    if(p->len)p->buf[p->len++]='/';
    for(unsigned i=0;i<n;i++)p->buf[p->len++]=name[i];
    p->buf[p->len]=0;
    return 1;
}
