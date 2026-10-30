/* SPDX-License-Identifier: GPL-3.0-only
 * Position-independent bridge executing within GT7's UI frame callback.
 * The original callback is always called. The payload publishes a request
 * only after its complete body has been written; CAS claims it exactly once. */
#include "live_bridge.h"
#include <stddef.h>
__attribute__((visibility("hidden"))) volatile LiveMailbox live_mailbox={.magic=LIVE_MAGIC};
typedef struct {char text[16];uint64_t size,capacity;} GameString;
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
__attribute__((visibility("hidden"))) void live_frame(void *context,float delta_time){
    __atomic_add_fetch(&live_mailbox.heartbeat,1,__ATOMIC_RELAXED);
    live_mailbox.context=(uintptr_t)context;
    /* Execute before entering the original callback's own locks. */
    set_wheel_width();
    set_wheel_geometry();
    set_engine_swap();
    sample_car();
    pump();
    ((void (*)(void *,float))(uintptr_t)live_mailbox.original)(context,delta_time);
}
