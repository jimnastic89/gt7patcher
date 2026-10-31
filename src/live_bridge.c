/* SPDX-License-Identifier: GPL-3.0-only
 * Position-independent bridge executing within GT7's UI frame callback.
 * The original callback is always called. The payload publishes a request
 * only after its complete body has been written; CAS claims it exactly once. */
#include "live_bridge.h"
#include <stddef.h>
__attribute__((visibility("hidden"))) volatile LiveMailbox live_mailbox={.magic=LIVE_MAGIC};
typedef struct {char text[16];uint64_t size,capacity;} GameString;
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
 * The platform delivers OrbisIme events to OrbisImeParam::handler. The bridge
 * opens GT7's own sceImeOpen in Number mode on request, captures the resulting
 * digits here, and applies them through the same native credit path used by
 * the live settings. All scratch memory lives at fixed offsets inside this
 * allocation, so the packed bridge bytes stay identical and are re-verified
 * by the payload on every attach. */
enum {ORBIS_IME_EVENT_UPDATE_TEXT=1,ORBIS_IME_EVENT_PRESS_CLOSE=4,ORBIS_IME_EVENT_PRESS_ENTER=5,ORBIS_IME_EVENT_ABORT=6};
__attribute__((visibility("hidden"))) void credit_ime_handler(void *arg,const void *event){
    volatile LiveMailbox *m=&live_mailbox;(void)arg;
    uint32_t id=*(const uint32_t *)event;
    if(id==ORBIS_IME_EVENT_UPDATE_TEXT){
        /* OrbisImeEvent is {id, padding, union param}; OrbisImeEditText::str is
         * the first member of the union, eight bytes into the event. */
        const uint16_t *src=*(const uint16_t *const *)((const unsigned char *)event+8);
        if(!src)return;
        uint16_t *dst=(uint16_t *)(uintptr_t)(m->allocation+LIVE_PROMPT_TEXT);
        uint32_t n=0;
        while(n<LIVE_PROMPT_TEXT_CHARS-1&&src[n]){dst[n]=src[n];n++;}
        dst[n]=0;m->prompt_chars=n;
    }else if(id==ORBIS_IME_EVENT_PRESS_ENTER){
        if(m->prompt_state==PROMPT_OPEN)m->prompt_state=PROMPT_ENTERED;
    }else if(id==ORBIS_IME_EVENT_PRESS_CLOSE||id==ORBIS_IME_EVENT_ABORT){
        if(m->prompt_state==PROMPT_OPEN)m->prompt_state=PROMPT_CANCELLED;
    }
}
static void prompt(void){
    volatile LiveMailbox *m=&live_mailbox;
    uint32_t state=m->prompt_state;
    if(state==PROMPT_QUEUED){
        uint32_t queued=PROMPT_QUEUED;
        if(!__atomic_compare_exchange_n(&m->prompt_state,&queued,PROMPT_RUNNING,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED))return;
        if(!m->enabled||!m->ime_original||!m->allocation){m->prompt_result=1;m->prompt_state=PROMPT_FAILED;return;}
        /* GT7 rebuilds its own OrbisImeParam on every open, so ours is
         * independent. type is OrbisImeType::Number (4) at offset 0x04;
         * maxTextLength is nine digits because 100000000 is the native cap. */
        unsigned char *param=(unsigned char *)(uintptr_t)(m->allocation+LIVE_PROMPT_PARAM);
        for(unsigned i=0;i<0x60;i++)param[i]=0;
        *(uint32_t *)(param+0x00)=m->ime_user_id;
        *(uint32_t *)(param+0x04)=4;
        *(uint32_t *)(param+0x24)=9;
        *(uintptr_t *)(param+0x28)=m->allocation+LIVE_PROMPT_TEXT;
        *(float *)(param+0x30)=960.0f;
        *(float *)(param+0x34)=540.0f;
        *(uintptr_t *)(param+0x40)=m->allocation+LIVE_PROMPT_WORK;
        *(uintptr_t *)(param+0x48)=(uintptr_t)&live_mailbox;
        *(uintptr_t *)(param+0x50)=m->allocation+(uintptr_t)&credit_ime_handler;
        /* The platform zeroes this scratch buffer on open; do the same so a
         * stale buffer from an earlier prompt can never leak into the pad. */
        {unsigned char *w=(unsigned char *)(uintptr_t)(m->allocation+LIVE_PROMPT_WORK);
         for(unsigned i=0;i<LIVE_PROMPT_WORK_SIZE;i++)w[i]=0;}
        *(uint16_t *)(uintptr_t)(m->allocation+LIVE_PROMPT_TEXT)=0;
        m->prompt_chars=0;m->prompt_amount=0;
        /* rdi is the OrbisImeParam, rsi the optional extended parameter. */
        int result=((int (*)(void *,void *))(uintptr_t)m->ime_original)(param,0);
        if(result){m->prompt_result=(uint32_t)result;m->prompt_state=PROMPT_FAILED;return;}
        m->prompt_open=1;m->prompt_result=0;m->prompt_state=PROMPT_OPEN;
        return;
    }
    if(state==PROMPT_OPEN){
        /* Pump queued events when the platform expects it; a no-op otherwise. */
        if(m->ime_update)((void (*)(void *))(uintptr_t)m->ime_update)((void *)(uintptr_t)(m->allocation+(uintptr_t)&credit_ime_handler));
        return;
    }
    if(state==PROMPT_ENTERED||state==PROMPT_CANCELLED){
        if(m->prompt_open&&m->ime_close)((int (*)(void))(uintptr_t)m->ime_close)();
        m->prompt_open=0;
        if(state==PROMPT_CANCELLED)return;
        const uint16_t *text=(const uint16_t *)(uintptr_t)(m->allocation+LIVE_PROMPT_TEXT);
        uint32_t amount=0;int valid=0;
        for(unsigned i=0;i<LIVE_PROMPT_TEXT_CHARS&&text[i];i++){
            uint16_t c=text[i];
            if(c<'0'||c>'9'){valid=0;break;}
            if(amount>(LIVE_MAX_CREDITS-(uint32_t)(c-'0'))/10){valid=0;break;}
            amount=amount*10+(uint32_t)(c-'0');valid=1;
        }
        if(!valid){m->prompt_result=2;m->prompt_state=PROMPT_FAILED;return;}
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
}
__attribute__((visibility("hidden"))) void live_frame(void *context,float delta_time){
    __atomic_add_fetch(&live_mailbox.heartbeat,1,__ATOMIC_RELAXED);
    live_mailbox.context=(uintptr_t)context;
    /* Execute before entering the original callback's own locks. */
    set_wheel_width();
    set_wheel_geometry();
    set_engine_swap();
    sample_car();
    prompt();
    pump();
    ((void (*)(void *,float))(uintptr_t)live_mailbox.original)(context,delta_time);
}
