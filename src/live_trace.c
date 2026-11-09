/* SPDX-License-Identifier: GPL-3.0-only */
#include "live_bridge.h"

static uint64_t trace_tsc(void){
    uint32_t lo,hi;
    __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));
    return ((uint64_t)hi<<32)|lo;
}

void live_trace(void *save,uint64_t cfg,uint64_t allocation){
    volatile LiveTraceHeader *header=(volatile LiveTraceHeader *)(allocation+LIVE_TRACE_HEADER);
    uint32_t site=(uint32_t)cfg&0xffu;
    uint32_t deref=((uint32_t)(cfg>>8))&1u;
    uint32_t shift=((uint32_t)(cfg>>16))&0xffu;
    if(header->magic!=LIVE_TRACE_MAGIC||site>=32u)return;
    volatile uint32_t *sample=(volatile uint32_t *)(allocation+LIVE_TRACE_SAMPLE+site*4u);
    uint32_t seen=__atomic_fetch_add(sample,1,__ATOMIC_RELAXED);
    if(shift&&((seen&((1u<<shift)-1u))!=0))return;
    uint64_t *regs=(uint64_t *)save;
    uint64_t rdi=regs[LIVE_SAVE_RDI],rsi=regs[LIVE_SAVE_RSI];
    uint64_t sym=0,native=0;
    if(deref&&rdi>=LIVE_ARENA_LOW&&rdi<LIVE_ARENA_HIGH){
        sym=*(volatile uint64_t *)(rdi+0x28);
        native=*(volatile uint64_t *)(rdi+0x20);
    }
    uint64_t seq=__atomic_fetch_add(&header->count,1,__ATOMIC_RELAXED)+1;
    volatile LiveTraceRecord *record=(volatile LiveTraceRecord *)
        (allocation+LIVE_TRACE_RING+((seq-1)&LIVE_TRACE_MASK)*sizeof(LiveTraceRecord));
    record->tsc=trace_tsc();
    record->site=site;
    record->epoch=header->epoch;
    record->sym=sym;
    record->native=native;
    record->rdi=rdi;
    record->rsi=rsi;
    record->ret=regs[LIVE_SAVE_RET];
    __atomic_store_n(&record->seq,seq,__ATOMIC_RELEASE);
}
