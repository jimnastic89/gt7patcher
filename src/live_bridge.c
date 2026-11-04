/* SPDX-License-Identifier: GPL-3.0-only
 * Position-independent bridge executing within GT7's UI frame callback.
 * The original callback is always called. The payload publishes a request
 * only after its complete body has been written; CAS claims it exactly once. */
#include "live_bridge.h"
#include "hud_tree.h"
#include <stddef.h>
/* Must stay static: a global symbol would be addressed through the GOT, which
 * GNU ld relaxes to an absolute (link-time VMA) address for the `add` form.
 * See the note in live_bridge.h. */
static volatile LiveMailbox live_mailbox={.magic=LIVE_MAGIC};
typedef struct {char text[16];uint64_t size,capacity;} GameString;
/* ------------------------------------------------------------------------
 * In-race HUD visibility.
 * The MENU widget tree is rebuilt at every race start and process restart, so
 * widgets are matched by *path*: the chain of named ancestors, for example
 * "RaceDisplay/Container/subjective/TCS". The layout rect at node+0x238 (32
 * bytes, eight floats) is the draw gate: zeroing it removes the widget and
 * every one of its children. The original rect is cached the first time a key
 * is hidden so the widget can be shown again.
 *
 * The payload publishes hide *entries*. An entry containing '/' is a path and
 * matches exactly that widget; an entry without '/' is a bare widget name and
 * matches every widget with that name (the original behaviour). Paths exist so
 * a HUD element cannot collide with a menu row that merely shares its name -
 * the in-race TCS indicator and the Assistance Settings "Traction control" row
 * are different widgets on different paths.
 *
 * The tree is reached from the update context handed to this callback each
 * frame: context+0x698..0x6c0 are MENU::mRenderContextPS4 pointers, each
 * holding a vector of MENU::mRootWindow at +0x1e8/+0x1f0. A root window's own
 * name field holds pointer bytes rather than a string, so a path starts at its
 * children and only printable ASCII names are ever pushed. The offsets live in
 * hud_tree.h because the payload walks the same tree for the UI.
 */
/* Restore cache. Bigger than the selection list because hiding a group
 * also hides every layer inside it: one selection can zero many widgets,
 * and each one needs its original rect kept so it can be shown again.
 * The cache lives in the bridge mapping, not in the mailbox, so this does
 * not change the payload ABI. The blob must stay below LIVE_PROMPT_PARAM
 * (0x10000); 1024 entries of 48 bytes keep it near 0xea00.
 *
 * Keyed by a 64-bit hash of the widget path rather than by the path itself:
 * a single selection cascades over everything below it, so the cache has to
 * hold a rect for every widget in the tree (about 700), and 96-byte keys
 * would not fit. A collision would restore one widget's rect onto another;
 * with ~700 live paths over 64 bits that is about 1e-14. */
#define HUD_SAVED_MAX 1024u
typedef struct {unsigned long long hash;unsigned char rect[HUD_RECT_LEN];unsigned char have,seen,used;} HudSaved;
static HudSaved hud_saved[HUD_SAVED_MAX];
static unsigned long long hud_hash(const char *s){
    unsigned long long h=14695981039346656037ull; /* FNV-1a 64 */
    for(;*s;s++){h^=(unsigned char)*s;h*=1099511628211ull;}
    return h;
}
static void hud_fill(void *d,unsigned char v,unsigned n){unsigned char *a=d;for(unsigned i=0;i<n;i++)a[i]=v;}
static void hud_copy(void *d,const void *s,unsigned n){unsigned char *a=d;const unsigned char *b=s;for(unsigned i=0;i<n;i++)a[i]=b[i];}
static int hud_same(const char *a,const char *b){for(;;a++,b++){if(*a!=*b)return 0;if(!*a)return 1;}}
static int hud_is_path(const char *e){for(;*e;e++)if(*e=='/')return 1;return 0;}
/* Read a widget name: 0 when there is none, 1 for an inline name, 2 for a
 * heap-allocated one. The name object is a small-string-optimised string, so a
 * long name is reached through the pointer the object holds rather than sitting
 * at the object itself. The payload's hud_read_name decodes it the same way: a
 * widget the two disagree about can never be hidden from the UI. */
static int hud_name(uintptr_t node,char *out){
    uintptr_t p=*(uintptr_t *)(node+HUD_OFF_NAME);
    if(!hud_ptr(p))return 0;
    const unsigned char *raw=(const unsigned char *)p;
    uint64_t len=*(const uint64_t *)(p+HUD_OFF_NAME_LEN),cap=*(const uint64_t *)(p+HUD_OFF_NAME_CAP);
    if(len>HUD_NAME_INLINE&&cap>=len&&len<LIVE_HUD_NAME){
        uintptr_t text=*(const uintptr_t *)p;
        if(!hud_ptr(text))return 0;
        raw=(const unsigned char *)text;
    }
    unsigned i=0;for(;i<LIVE_HUD_NAME-1;i++){if(!raw[i])break;out[i]=(char)raw[i];}
    out[i]=0;
    if(!hud_name_ok(out,i))return 0;
    return (len>HUD_NAME_INLINE)?2:1;
}
/* A widget matches by its path, or - so a selection made before heap-allocated
 * widget names were decoded keeps working - by `alias`, the path the old reader
 * would have built for it. */
static int hud_match(volatile LiveMailbox *m,uint32_t count,const char *name,const char *path,const char *alias){
    for(uint32_t i=0;i<count;i++){
        const char *e=(const char *)m->hud_entries[i];
        if(!e[0])continue;
        if(hud_is_path(e)){if(path[0]&&(hud_same(e,path)||hud_same(e,alias)))return 1;}
        else if(hud_same(e,name))return 1;
    }
    return 0;
}
static void hud_children(uintptr_t node,volatile LiveMailbox *m,uintptr_t base,unsigned depth,HudPath *path,HudPath *alias,int hidden);
static void hud_walk(uintptr_t node,volatile LiveMailbox *m,uintptr_t base,unsigned depth,HudPath *path,HudPath *alias,int hidden){
    if(depth>HUD_MAX_DEPTH)return;
    char nm[LIVE_HUD_NAME];
    int named=hud_name(node,nm);
    unsigned save=path->len,asave=alias->len;
    int pushed=named?hud_path_push(path,nm):0;
    if(named&&!pushed)return; /* path limit reached: skip the subtree so no widget is given a path missing an ancestor */
    /* Only names the old reader could see go into the compatibility path. */
    if(named==1&&!hud_path_push(alias,nm)){alias->len=asave;alias->buf[asave]=0;}
    if(named){
        uint32_t count=__atomic_load_n(&m->hud_count,__ATOMIC_RELAXED);
        if(count>LIVE_HUD_MAX)count=LIVE_HUD_MAX;
        const char *key=pushed?path->buf:nm;
        unsigned long long key_hash=hud_hash(key);
        /* A widget is hidden when it is selected itself or when an ancestor
         * is. Hiding a group therefore hides every layer drawn inside it, so
         * one tick covers an element built from several child layers (a panel
         * background plus its indicator). Selecting a layer on its own still
         * hides only that layer, which is what lets a background be dropped
         * while the indicator stays. */
        if(!hidden&&hud_match(m,count,nm,pushed?path->buf:"",alias->buf))hidden=1;
        if(hidden){
            int slot=-1,spare=-1;
            for(unsigned i=0;i<HUD_SAVED_MAX;i++){
                if(hud_saved[i].have){if(hud_saved[i].hash==key_hash){slot=(int)i;break;}}
                else if(spare<0)spare=(int)i;
            }
            if(slot<0&&spare>=0){
                slot=spare;
                hud_fill(&hud_saved[slot],0,sizeof(HudSaved));
                hud_saved[slot].hash=key_hash;
                hud_copy(hud_saved[slot].rect,(void *)(node+HUD_OFF_RECT),HUD_RECT_LEN);
                hud_saved[slot].have=1;
            }
            /* Only hide what can be restored. When the cache is full the
             * widget is left visible rather than hidden for good. */
            if(slot>=0){
                hud_saved[slot].seen=1;hud_saved[slot].used=1;
                hud_fill((void *)(node+HUD_OFF_RECT),0,HUD_RECT_LEN);
            }
        }else{
            /* Restore every instance of the key. The cache entry is dropped by
             * apply_hud once the whole tree has been visited, so duplicates (the
             * in-car and bumper root windows carry the same names) are all
             * restored. Clearing `have` here would leave the second instance
             * hidden and would later re-cache its zeroed rect, making the widget
             * permanently un-restorable. */
            for(unsigned i=0;i<HUD_SAVED_MAX;i++)if(hud_saved[i].have&&hud_saved[i].hash==key_hash){
                hud_copy((void *)(node+HUD_OFF_RECT),hud_saved[i].rect,HUD_RECT_LEN);
                hud_saved[i].seen=1;break;
            }
        }
    }
    hud_children(node,m,base,depth+1,path,alias,hidden);
    path->len=save;path->buf[save]=0;
    alias->len=asave;alias->buf[asave]=0;
}
static void hud_children(uintptr_t node,volatile LiveMailbox *m,uintptr_t base,unsigned depth,HudPath *path,HudPath *alias,int hidden){
    uintptr_t nl=*(uintptr_t *)(node+HUD_OFF_CHILDREN);
    if(!hud_ptr(nl)||*(uintptr_t *)nl!=base+HUD_NLW_RVA)return;
    uintptr_t vb=*(uintptr_t *)(nl+0x38),ve=*(uintptr_t *)(nl+0x40);
    if(!hud_ptr(vb)||ve<vb||(ve-vb)>8u*4096u)return;
    for(uintptr_t v=vb;v<ve;v+=8){uintptr_t c=*(uintptr_t *)v;if(hud_ptr(c))hud_walk(c,m,base,depth,path,alias,hidden);}
}
static void apply_hud(void){
    volatile LiveMailbox *m=&live_mailbox;
    uint32_t count=__atomic_load_n(&m->hud_count,__ATOMIC_RELAXED);
    if(count>LIVE_HUD_MAX)count=LIVE_HUD_MAX;
    int any=0;
    for(unsigned i=0;i<HUD_SAVED_MAX;i++)if(hud_saved[i].have){any=1;break;}
    if(!count&&!any){__atomic_store_n(&m->hud_applied,0,__ATOMIC_RELAXED);return;}
    uintptr_t base=m->base,ctx=m->context;
    if(!hud_ptr(base)||!hud_ptr(ctx)||*(uintptr_t *)ctx!=base+HUD_UPDATE_VT_RVA)return;
    HudPath path;path.len=0;path.buf[0]=0;
    HudPath alias;alias.len=0;alias.buf[0]=0;
    for(unsigned i=0;i<HUD_SAVED_MAX;i++){hud_saved[i].seen=0;hud_saved[i].used=0;}
    for(unsigned i=0;i<6;i++){
        uintptr_t rc=*(uintptr_t *)(ctx+0x698+8*i);
        if(!hud_ptr(rc)||*(uintptr_t *)rc!=base+HUD_RENDER_VT_RVA)continue;
        uintptr_t rb=*(uintptr_t *)(rc+0x1e8),re=*(uintptr_t *)(rc+0x1f0);
        if(!hud_ptr(rb)||re<rb||(re-rb)>8u*64u)continue;
        for(uintptr_t v=rb;v<re;v+=8){uintptr_t rw=*(uintptr_t *)v;if(hud_ptr(rw)&&*(uintptr_t *)rw==base+HUD_ROOT_VT_RVA)hud_children(rw,m,base,0,&path,&alias,0);}
    }
    /* Drop caches that were restored this pass: seen in the tree but no longer
     * hidden (their rects have just been put back). A cache whose widget is
     * absent this frame - for example between races - is kept, so a selection
     * survives a race change or a game restart. A cascade entry stays while
     * `used` is set, even though its own path is not in the list. */
    for(unsigned i=0;i<HUD_SAVED_MAX;i++)if(hud_saved[i].have&&hud_saved[i].seen&&!hud_saved[i].used)hud_saved[i].have=0;
    uint32_t applied=0;
    for(unsigned i=0;i<HUD_SAVED_MAX;i++)if(hud_saved[i].have&&hud_saved[i].used)applied++;
    __atomic_store_n(&m->hud_applied,applied,__ATOMIC_RELAXED);
}

/* ---- Credits popup auto-trigger ---------------------------------------- *
 * The world-map (and several other menu) status bars carry a Credits button.
 * Pressing it makes the game spawn a widget named "GTCredit" under one of the
 * menu root windows. When that widget appears we ask the payload to open the
 * credit number pad, so the button press shows the pad directly. The request
 * is edge-triggered (once per opening) so the pad is not restarted every frame
 * while the popup stays open; the prompt state is reset to idle once the popup
 * closes so the next press can arm again. */
static int credit_popup_prev;
/* Only the credits *view* popup may open the numpad - not the purchase
 * confirmation shown by the tuning shop and GT Auto. The two are the same
 * template: both render a widget named "GTCredit", so matching that name alone
 * fired on purchases too. They differ in how the balance row is built:
 *
 *   view      GTCreditChargeDialogRoot   .../GTCredit/SBox/HBox      (OK only)
 *   purchase  GTCreditPurchaseDialogRoot .../GTCredit/SBox/Balance/HBox +
 *                                          Payment + BodyText + ButtonComposite/Cancel
 *
 * The view popup is therefore the one with an HBox directly under GTCredit/SBox;
 * the purchase popup nests its HBox one level deeper under Balance, so it is
 * excluded. Track the two enclosing widget names to tell the shapes apart. */
static int credit_find(uintptr_t node,uintptr_t base,unsigned depth,const char *parent,const char *grand){
    if(depth>HUD_MAX_DEPTH)return 0;
    char nm[LIVE_HUD_NAME];
    int named=hud_name(node,nm)==1; /* inline names only: this walk predates heap-name decoding */
    const char *name=named?nm:"";
    if(named&&hud_same(nm,"HBox")&&hud_same(parent,"SBox")&&hud_same(grand,"GTCredit"))return 1;
    uintptr_t nl=*(uintptr_t *)(node+HUD_OFF_CHILDREN);
    if(!hud_ptr(nl)||*(uintptr_t *)nl!=base+HUD_NLW_RVA)return 0;
    uintptr_t vb=*(uintptr_t *)(nl+0x38),ve=*(uintptr_t *)(nl+0x40);
    if(!hud_ptr(vb)||ve<vb||(ve-vb)>8u*4096u)return 0;
    for(uintptr_t v=vb;v<ve;v+=8){uintptr_t c=*(uintptr_t *)v;if(hud_ptr(c)&&credit_find(c,base,depth+1,name,parent))return 1;}
    return 0;
}
static int credit_visible(void){
    volatile LiveMailbox *m=&live_mailbox;
    uintptr_t base=m->base,ctx=m->context;
    if(!hud_ptr(base)||!hud_ptr(ctx)||*(uintptr_t *)ctx!=base+HUD_UPDATE_VT_RVA)return 0;
    for(unsigned i=0;i<6;i++){
        uintptr_t rc=*(uintptr_t *)(ctx+0x698+8*i);
        if(!hud_ptr(rc)||*(uintptr_t *)rc!=base+HUD_RENDER_VT_RVA)continue;
        uintptr_t rb=*(uintptr_t *)(rc+0x1e8),re=*(uintptr_t *)(rc+0x1f0);
        if(!hud_ptr(rb)||re<rb||(re-rb)>8u*64u)continue;
        for(uintptr_t v=rb;v<re;v+=8){uintptr_t rw=*(uintptr_t *)v;if(hud_ptr(rw)&&*(uintptr_t *)rw==base+HUD_ROOT_VT_RVA&&credit_find(rw,base,0,"",""))return 1;}
    }
    return 0;
}
static void credit_watch(void){
    volatile LiveMailbox *m=&live_mailbox;
    int visible=credit_visible();
    uint32_t st=m->prompt_state;
    if(visible&&!credit_popup_prev&&st==PROMPT_IDLE)m->credit_request=1;
    if(!visible&&st!=PROMPT_QUEUED&&st!=PROMPT_RUNNING&&st!=PROMPT_OPEN&&st!=PROMPT_IDLE)m->prompt_state=PROMPT_IDLE;
    credit_popup_prev=visible;
}
/* GT7 passes its OrbisImeParam in rdi and the extended-parameter pointer in
 * rsi at its sceImeOpen call site (the game rebuilds the struct on every
 * open).  OrbisImeParam::type lives at +0x04; OrbisImeType::Number is 4. */
__attribute__((visibility("hidden"))) int ime_open_hook(void *param, void *work){
    volatile LiveMailbox *m=&live_mailbox;
    uint32_t *type=(uint32_t *)((unsigned char *)param+0x04);
    uint32_t saved=*type;
    int armed=m->ime_hook_enabled&&__atomic_exchange_n(&m->credit_input_pending,0,__ATOMIC_ACQ_REL);
    if(armed)*type=4;
    int result=((int (*)(void *,void *))(uintptr_t)m->ime_original)(param,work);
    if(armed)*type=saved;
    return result;
}
static void sample_car(void){
    volatile LiveMailbox *m=&live_mailbox;
    uint32_t queued=CAR_QUEUED;
    if(!__atomic_compare_exchange_n(&m->car_state,&queued,CAR_RUNNING,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return;
    uint32_t result=CAR_UNAVAILABLE;
    m->car_object=0;m->car_implementation=0;m->car_expanded=0;m->car_style=0;
    _Alignas(16) unsigned char car[0x30];
    if(!m->enabled||!m->car_ctor||!m->get_riding_car||!m->car_dtor)goto finish;
    ((void (*)(void *))(uintptr_t)m->car_ctor)(car);
    if(((int (*)(void *))(uintptr_t)m->get_riding_car)(car)){
        uintptr_t implementation=*(uintptr_t *)car;
        uintptr_t style=implementation?*(uintptr_t *)(implementation+0x18):0;
        if(style){
            /* GT7's axle argument is rear=0, front=1. */
            m->wheel_width_front=*(int8_t *)(style+0x25);
            m->wheel_width_rear=*(int8_t *)(style+0x26);
            m->wheel_inch_up=*(int8_t *)(style+0x24);
            m->wheel_offset_front=*(int16_t *)(style+0x20);m->wheel_offset_rear=*(int16_t *)(style+0x22);
            m->hub_offset_front=*(int16_t *)(style+0x28);m->hub_offset_rear=*(int16_t *)(style+0x2a);
            uintptr_t expanded=*(uintptr_t *)(implementation+0x10);
            m->engine_swap_category=expanded?*(uint8_t *)(expanded+0x0c):0;
            m->car_implementation=implementation;m->car_expanded=expanded;m->car_style=style;
            m->car_model_code=*(uint32_t *)(style+0x68);
            m->car_sequence++;
            result=CAR_READY;
        }
    }
    ((void (*)(void *))(uintptr_t)m->car_dtor)(car);
finish:
    __atomic_store_n(&m->car_state,result,__ATOMIC_RELEASE);
}
typedef struct {void *car;const uint8_t *value;} WheelInchArgs;
typedef struct {void *car;const uint8_t *axle;const int16_t *value;} HubOffsetArgs;
static void set_wheel_geometry(void){
    volatile LiveMailbox *m=&live_mailbox;uint32_t queued=GEOMETRY_QUEUED;
    if(!__atomic_compare_exchange_n(&m->geometry_state,&queued,GEOMETRY_RUNNING,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return;
    uint32_t result=GEOMETRY_UNAVAILABLE;_Alignas(16) unsigned char car[0x30];int constructed=0,locked=0;uintptr_t root=0,garage=0,implementation=0,style=0;uint16_t index=0;
    if(!m->enabled||!m->garage_root||!m->garage_lock||!m->garage_unlock||!m->garage_riding_id||!m->garage_get_car||!m->garage_set_car||!m->set_wheel_inch_up||!m->add_wheel_offset||!m->set_hub_offset)goto finish;
    root=((uintptr_t (*)(void))(uintptr_t)m->garage_root)();if(!root||((int (*)(void *))(uintptr_t)m->garage_lock)((void *)(root+0x68)))goto finish;locked=1;garage=*(uintptr_t *)(root+0x70);if(!garage)goto finish;
    ((void (*)(void *,uint16_t *))(uintptr_t)m->garage_riding_id)((void *)garage,&index);((void (*)(void *))(uintptr_t)m->car_ctor)(car);constructed=1;
    if(!((int (*)(void *,unsigned,void *,void *))(uintptr_t)m->garage_get_car)((void *)garage,index,car,0))goto finish;
    implementation=*(uintptr_t *)car;style=implementation?*(uintptr_t *)(implementation+0x18):0;if(!style)goto finish;
    if(m->car_model_code!=m->geometry_expected_model_code||*(int8_t *)(style+0x24)!=m->geometry_expected_inch_up||*(int16_t *)(style+0x20)!=m->geometry_expected_offset_front||*(int16_t *)(style+0x22)!=m->geometry_expected_offset_rear||*(int16_t *)(style+0x28)!=m->geometry_expected_hub_front||*(int16_t *)(style+0x2a)!=m->geometry_expected_hub_rear){result=GEOMETRY_STALE;goto finish;}
    uint8_t inch=(uint8_t)m->geometry_requested_inch_up,front_axle=1,rear_axle=0;int16_t front_hub_value=m->geometry_requested_hub_front,rear_hub_value=m->geometry_requested_hub_rear;WheelInchArgs inch_args={car,&inch};HubOffsetArgs front_hub={car,&front_axle,&front_hub_value},rear_hub={car,&rear_axle,&rear_hub_value};
    ((void (*)(void *,const WheelInchArgs *))(uintptr_t)m->set_wheel_inch_up)(0,&inch_args);
    ((void (*)(void *,unsigned,int))(uintptr_t)m->add_wheel_offset)(car,1,(int)m->geometry_requested_offset_front-*(int16_t *)(style+0x20));
    ((void (*)(void *,unsigned,int))(uintptr_t)m->add_wheel_offset)(car,0,(int)m->geometry_requested_offset_rear-*(int16_t *)(style+0x22));
    ((void (*)(void *,const HubOffsetArgs *))(uintptr_t)m->set_hub_offset)(0,&front_hub);((void (*)(void *,const HubOffsetArgs *))(uintptr_t)m->set_hub_offset)(0,&rear_hub);
    if(!((int (*)(void *,unsigned,void *))(uintptr_t)m->garage_set_car)((void *)garage,index,car))goto finish;
    m->wheel_inch_up=*(int8_t *)(style+0x24);m->wheel_offset_front=*(int16_t *)(style+0x20);m->wheel_offset_rear=*(int16_t *)(style+0x22);m->hub_offset_front=*(int16_t *)(style+0x28);m->hub_offset_rear=*(int16_t *)(style+0x2a);m->car_model_code=*(uint32_t *)(style+0x68);m->car_sequence++;result=GEOMETRY_DONE;
finish:if(constructed)((void (*)(void *))(uintptr_t)m->car_dtor)(car);if(locked)((void (*)(void *))(uintptr_t)m->garage_unlock)((void *)(root+0x68));__atomic_store_n(&m->geometry_state,result,__ATOMIC_RELEASE);
}
static void set_engine_swap(void){
    volatile LiveMailbox *m=&live_mailbox;uint32_t queued=ENGINE_QUEUED;
    if(!__atomic_compare_exchange_n(&m->engine_state,&queued,ENGINE_RUNNING,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return;
    uint32_t result=ENGINE_UNAVAILABLE;_Alignas(16) unsigned char car[0x30];int constructed=0,locked=0;uintptr_t root=0,garage=0,implementation=0,style=0;uint16_t index=0;
    if(!m->enabled||!m->garage_root||!m->garage_lock||!m->garage_unlock||!m->garage_riding_id||!m->garage_get_car||!m->garage_set_car||!m->get_engine_swap||!m->add_engine_swap||!m->set_engine_swap||!m->reset_engine_swap)goto finish;
    root=((uintptr_t (*)(void))(uintptr_t)m->garage_root)();if(!root||((int (*)(void *))(uintptr_t)m->garage_lock)((void *)(root+0x68)))goto finish;locked=1;garage=*(uintptr_t *)(root+0x70);if(!garage)goto finish;
    ((void (*)(void *,uint16_t *))(uintptr_t)m->garage_riding_id)((void *)garage,&index);((void (*)(void *))(uintptr_t)m->car_ctor)(car);constructed=1;
    if(!((int (*)(void *,unsigned,void *,void *))(uintptr_t)m->garage_get_car)((void *)garage,index,car,0))goto finish;
    implementation=*(uintptr_t *)car;style=implementation?*(uintptr_t *)(implementation+0x18):0;if(!style||m->car_model_code!=m->engine_expected_model_code)goto stale;
    uint8_t current=((uint8_t (*)(void *))(uintptr_t)m->get_engine_swap)(car);
    if(current!=m->engine_expected_category)goto stale;
    if(m->engine_requested_category==0){((void (*)(void *))(uintptr_t)m->reset_engine_swap)(car);}
    else {if(current)((void (*)(void *))(uintptr_t)m->reset_engine_swap)(car);if(m->engine_unlock)((void (*)(void *,unsigned))(uintptr_t)m->add_engine_swap)(car,m->engine_requested_category);if(!((int (*)(void *,unsigned))(uintptr_t)m->set_engine_swap)(car,m->engine_requested_category))goto finish;}
    if(!((int (*)(void *,unsigned,void *))(uintptr_t)m->garage_set_car)((void *)garage,index,car))goto finish;
    m->engine_swap_category=((uint8_t (*)(void *))(uintptr_t)m->get_engine_swap)(car);m->car_sequence++;result=ENGINE_DONE;goto finish;
stale:result=ENGINE_STALE;
finish:if(constructed)((void (*)(void *))(uintptr_t)m->car_dtor)(car);if(locked)((void (*)(void *))(uintptr_t)m->garage_unlock)((void *)(root+0x68));__atomic_store_n(&m->engine_state,result,__ATOMIC_RELEASE);
}
typedef struct {void *car;const uint8_t *axle;const uint8_t *value;} WheelWidthArgs;
static void set_wheel_width(void){
    volatile LiveMailbox *m=&live_mailbox;
    uint32_t queued=WHEEL_QUEUED;
    if(!__atomic_compare_exchange_n(&m->wheel_state,&queued,WHEEL_RUNNING,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return;
    uint32_t result=WHEEL_UNAVAILABLE;
    _Alignas(16) unsigned char car[0x30];
    int constructed=0,locked=0;
    uintptr_t root=0,garage=0,implementation=0,style=0;
    uint16_t index=0;
    if(!m->enabled||!m->garage_root||!m->garage_lock||!m->garage_unlock||!m->garage_riding_id||!m->garage_get_car||!m->garage_set_car||!m->set_wheel_width)goto finish;
    root=((uintptr_t (*)(void))(uintptr_t)m->garage_root)();
    if(!root||((int (*)(void *))(uintptr_t)m->garage_lock)((void *)(root+0x68)))goto finish;
    locked=1;garage=*(uintptr_t *)(root+0x70);if(!garage)goto finish;
    ((void (*)(void *,uint16_t *))(uintptr_t)m->garage_riding_id)((void *)garage,&index);
    ((void (*)(void *))(uintptr_t)m->car_ctor)(car);constructed=1;
    if(!((int (*)(void *,unsigned,void *,void *))(uintptr_t)m->garage_get_car)((void *)garage,index,car,0))goto finish;
    implementation=*(uintptr_t *)car;style=implementation?*(uintptr_t *)(implementation+0x18):0;
    if(!style)goto finish;
    if(m->car_model_code!=m->wheel_expected_model_code||
       *(int8_t *)(style+0x26)!=m->wheel_expected_front||*(int8_t *)(style+0x25)!=m->wheel_expected_rear){result=WHEEL_STALE;goto finish;}
    uint8_t front_axle=1,rear_axle=0,front=(uint8_t)m->wheel_requested_front,rear=(uint8_t)m->wheel_requested_rear;
    WheelWidthArgs front_args={car,&front_axle,&front},rear_args={car,&rear_axle,&rear};
    ((void (*)(void *,const WheelWidthArgs *))(uintptr_t)m->set_wheel_width)(0,&front_args);
    ((void (*)(void *,const WheelWidthArgs *))(uintptr_t)m->set_wheel_width)(0,&rear_args);
    if(!((int (*)(void *,unsigned,void *))(uintptr_t)m->garage_set_car)((void *)garage,index,car))goto finish;
    m->wheel_width_front=*(int8_t *)(style+0x25);m->wheel_width_rear=*(int8_t *)(style+0x26);
    m->car_model_code=*(uint32_t *)(style+0x68);m->car_sequence++;
    result=WHEEL_DONE;
finish:
    if(constructed)((void (*)(void *))(uintptr_t)m->car_dtor)(car);
    if(locked)((void (*)(void *))(uintptr_t)m->garage_unlock)((void *)(root+0x68));
    __atomic_store_n(&m->wheel_state,result,__ATOMIC_RELEASE);
}
static void pump(void){
    volatile LiveMailbox *m=&live_mailbox;
    uint32_t queued=LIVE_QUEUED;
    if(!__atomic_compare_exchange_n(&m->state,&queued,LIVE_RUNNING,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return;
    uint32_t result=LIVE_INVALID;
    if(!m->enabled){result=LIVE_CANCELLED;goto finish;}
    if(m->magic!=LIVE_MAGIC||m->amount>LIVE_MAX_CREDITS)goto finish;
    uintptr_t object=*(volatile uintptr_t *)(m->base+0x066e9648);
    if(!object||object!=m->expected_object||*(volatile uintptr_t *)object!=m->base+0x053edca8){result=LIVE_STALE;goto finish;}
    uint32_t before=*(volatile uint32_t *)(object+0x14c);
    if(before!=m->expected_amount){result=LIVE_STALE;goto finish;}
    if(before!=m->amount){
        /* An administrative set is not earned/spent currency: set through the
         * native accessor, then submit a zero-delta native event to refresh
         * observers without adding the requested balance to earned totals. */
        _Alignas(32) unsigned char event[0x160];
        const GameString label={{'G','T','7','P','a','t','c','h','e','r',0},10,15};
        ((void (*)(void *,int,const GameString *,const GameString *))(uintptr_t)m->make_event)(event,0,&label,&label);
        uint32_t amount=m->amount;
        ((void (*)(void *,const uint32_t *))(uintptr_t)m->set_credits)((void *)object,&amount);
        ((int (*)(void *))(uintptr_t)m->acquire)(event);
        ((void (*)(void *))(uintptr_t)m->destroy_event)(event);
    }
    if(*(volatile uintptr_t *)(m->base+0x066e9648)!=object){result=LIVE_STALE;goto finish;}
    m->observed=*(volatile uint32_t *)(object+0x14c);
    result=m->observed==m->amount?LIVE_DONE:LIVE_INVALID;
finish:
    __atomic_store_n(&m->state,result,__ATOMIC_RELEASE);
}
/* ------------------------------------------------------------------------
 * Credit number-input prompt.
 * sceImeOpen opens only the on-screen keyboard panel (the game is expected to
 * draw the text itself), so it appears as a bare number pad. The system
 * dialogue opened through sceImeDialogInit draws its own text entry field.
 * The payload resolves GT7's imported sceImeDialog functions; the bridge opens
 * the number dialogue, polls its status, reads the accepted digits from
 * inputTextBuffer, and applies them through the same native credit path used
 * by the live settings. All scratch memory lives at fixed offsets inside this
 * allocation, so the packed bridge bytes stay identical and are re-verified
 * by the payload on every attach. */
enum {ORBIS_DIALOG_STATUS_NONE,ORBIS_DIALOG_STATUS_RUNNING,ORBIS_DIALOG_STATUS_FINISHED};
static void prompt(void){
    volatile LiveMailbox *m=&live_mailbox;
    uint32_t state=m->prompt_state;
    if(state==PROMPT_QUEUED){
        uint32_t queued=PROMPT_QUEUED;
        if(!__atomic_compare_exchange_n(&m->prompt_state,&queued,PROMPT_RUNNING,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED))return;
        if(!m->enabled||!m->ime_dialog_init||!m->ime_dialog_status||!m->ime_dialog_result||!m->ime_dialog_term||!m->allocation){m->prompt_result=1;m->prompt_state=PROMPT_FAILED;return;}
        /* GT7 rebuilds its own parameters on every open, so ours is
         * independent. OrbisImeDialogSetting: type is OrbisImeType::Number (4)
         * at 0x04, maxTextLength at 0x24 is nine digits (100000000 is the
         * native cap), and the text field buffer is at 0x28. */
        unsigned char *param=(unsigned char *)(uintptr_t)(m->allocation+LIVE_PROMPT_PARAM);
        for(unsigned i=0;i<0x80;i++)param[i]=0;
        *(uint32_t *)(param+0x00)=m->ime_user_id;
        *(uint32_t *)(param+0x04)=4;
        *(uint32_t *)(param+0x24)=9;
        *(uintptr_t *)(param+0x28)=m->allocation+LIVE_PROMPT_TEXT;
        *(float *)(param+0x30)=960.0f;
        *(float *)(param+0x34)=540.0f;
        /* 0x38 horizontal / 0x3C vertical alignment: leave posx/posy at the
         * screen centre but ask the system to centre the dialogue on that
         * point instead of anchoring its top-left corner there. */
        *(uint32_t *)(param+0x38)=1;
        *(uint32_t *)(param+0x3C)=1;
        /* Start from an empty field so a stale buffer can never leak in. */
        *(uint16_t *)(uintptr_t)(m->allocation+LIVE_PROMPT_TEXT)=0;
        m->prompt_chars=0;m->prompt_amount=0;
        /* rdi is the OrbisImeDialogSetting, rsi the optional extended one. */
        int result=((int (*)(void *,void *))(uintptr_t)m->ime_dialog_init)(param,0);
        if(result){m->prompt_result=(uint32_t)result;m->prompt_state=PROMPT_FAILED;return;}
        m->prompt_open=1;m->prompt_result=0;m->prompt_state=PROMPT_OPEN;
        return;
    }
    if(state==PROMPT_OPEN){
        int status=((int (*)(void))(uintptr_t)m->ime_dialog_status)();
        m->prompt_dbg_status=(uint32_t)status;
        if(status==ORBIS_DIALOG_STATUS_RUNNING)return;
        if(status==ORBIS_DIALOG_STATUS_FINISHED){
            /* The real libSceImeDialog validates the caller's OrbisImeDialogResult
             * before doing anything: if result bytes +4..+0xF are not zero it
             * returns DIALOG_NOT_FINISHED (0x80BC0032). An uninitialised 16-byte
             * buffer therefore failed on every single accepted entry. */
            unsigned char result[16];
            for(unsigned i=0;i<16;i++)result[i]=0;
            int got=status==ORBIS_DIALOG_STATUS_FINISHED?((int (*)(void *))(uintptr_t)m->ime_dialog_result)(result):-1;
            /* The console can still report Finished a frame before the result is
             * ready; poll again instead of treating DIALOG_NOT_FINISHED as a
             * cancel. The retry budget reuses prompt_amount, zeroed on open. */
            if((uint32_t)got==0x80bc0032u&&m->prompt_amount++<120)return;
            ((int (*)(void))(uintptr_t)m->ime_dialog_term)();
            m->prompt_open=0;
            m->prompt_reserved=(uint32_t)got;
            m->prompt_dbg_end=*(uint32_t *)result;
            /* Decide from the text the IME committed into our buffer, not the
             * end status: GT7's own wrapper only reads offset 0 and the library
             * cycles several internal states. An empty buffer means cancelled. */
            const uint16_t *text=(const uint16_t *)(uintptr_t)(m->allocation+LIVE_PROMPT_TEXT);
            uint32_t amount=0;int valid=0,n=0;
            for(unsigned i=0;i<LIVE_PROMPT_TEXT_CHARS&&text[i];i++){
                uint16_t c=text[i];
                if(c<'0'||c>'9'){valid=0;break;}
                if(amount>(LIVE_MAX_CREDITS-(uint32_t)(c-'0'))/10){valid=0;break;}
                amount=amount*10+(uint32_t)(c-'0');valid=1;n=i+1;
            }
            m->prompt_chars=n;
            if(!valid){m->prompt_state=PROMPT_CANCELLED;return;}
            m->prompt_amount=amount;
            /* Queue the value through the same native credit path as the live
             * settings; pump() applies it later in this same frame. */
            uintptr_t object=*(volatile uintptr_t *)(m->base+0x066e9648);
            if(!object||*(volatile uintptr_t *)object!=m->base+0x053edca8||m->state==LIVE_QUEUED||m->state==LIVE_RUNNING){
                m->prompt_result=3;m->prompt_state=PROMPT_FAILED;return;
            }
            m->expected_object=object;
            m->expected_amount=*(volatile uint32_t *)(object+0x14c);
            m->amount=amount;m->observed=0;m->sequence++;
            m->prompt_result=0;m->prompt_state=PROMPT_APPLIED;m->state=LIVE_QUEUED;
            return;
        }
        /* No dialog is running (it never opened or was closed externally). */
        m->prompt_open=0;
        m->prompt_state=PROMPT_CANCELLED;
        return;
    }
}
__attribute__((visibility("hidden"))) void live_frame(void *context,float delta_time){
    __atomic_add_fetch(&live_mailbox.heartbeat,1,__ATOMIC_RELAXED);
    live_mailbox.context=(uintptr_t)context;
    apply_hud();
    /* Execute before entering the original callback's own locks. */
    set_wheel_width();
    set_wheel_geometry();
    set_engine_swap();
    sample_car();
    credit_watch();
    prompt();
    pump();
    ((void (*)(void *,float))(uintptr_t)live_mailbox.original)(context,delta_time);
}
