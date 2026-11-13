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

int live_menu_close(const uint64_t *registers, volatile LiveMailbox *mailbox)
{
    uintptr_t option = registers[LIVE_SAVE_RDI];
    if ((uint32_t)registers[LIVE_SAVE_RDX] || !option ||
        option != mailbox->menu_active_option)
        return 0;
    uint32_t selected = *(const uint32_t *)(option + 0x388);
    uint32_t count = mailbox->menu_active_count;
    if (selected < LIVE_MENU_STOCK_ROWS || selected - LIVE_MENU_STOCK_ROWS >= count)
        return 0;
    uintptr_t box = *(const uintptr_t *)(option + 0x3c8);
    if (!box || box != mailbox->menu_active_box ||
        *(const uintptr_t *)(box + 0x2d0) != mailbox->menu_active_row)
        return 0;

    /* Consume the confirmation before the native close and its stock
     * value callback. Keep the popup open at its current row and scroll. */
    if (!mailbox->enabled || !mailbox->allocation)
        return 1;
    volatile LiveMenuControl *control =
        (volatile LiveMenuControl *)(mailbox->allocation + LIVE_MENU_CONTROL);
    uint32_t sequence = mailbox->menu_toggle_sequence;
    if (sequence - __atomic_load_n(&control->acknowledged, __ATOMIC_ACQUIRE) >= LIVE_MENU_EVENTS)
        return 1;
    control->events[sequence % LIVE_MENU_EVENTS] = (uint8_t)(selected - LIVE_MENU_STOCK_ROWS);
    __atomic_store_n(&mailbox->menu_toggle_sequence, sequence + 1, __ATOMIC_RELEASE);
    return 1;
}
