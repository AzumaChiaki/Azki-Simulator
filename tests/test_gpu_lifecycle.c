/* SPDX-License-Identifier: GPL-2.0-or-later
 * Production GPU MMIO and renderer, with deterministic host RAM/timer/IRQ.
 * IRQ observation is at the device output; this is not an NVIC/guest test. */
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned lduw_le_p(const void *v) { const uint8_t *p=v; return p[0]|p[1]<<8; }
static uint32_t ldl_le_p(const void *v) { const uint8_t *p=v; return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void stw_le_p(void *v,unsigned n) { uint8_t *p=v; p[0]=n; p[1]=n>>8; }
static void stl_le_p(void *v,uint32_t n) { uint8_t *p=v; p[0]=n; p[1]=n>>8; p[2]=n>>16; p[3]=n>>24; }
#define LOG_GUEST_ERROR 0
#define CPU_LOG_INT 1
#define qemu_log_mask(...) ((void)0)
#include "best1503-gpu.h"
typedef uint64_t hwaddr;
typedef unsigned MemTxAttrs;
typedef int MemTxResult;
typedef int qemu_irq;
typedef struct { bool pending; int64_t deadline; unsigned schedules, cancels; } QEMUTimer;
typedef struct Best1503State {
#include "gpu-fields.inc"
} Best1503State;
#define MEMTX_OK 0
#define MEMTX_ERROR 1
#define MEMTXATTRS_UNSPECIFIED 0
#define QEMU_CLOCK_VIRTUAL 0
#define RAM_BASE 0x20000000u
static uint8_t ram[1024];
static int address_space_memory, irq_level;
static unsigned irq_rises, memory_writes;
static int64_t virtual_ns;
static void (*interrupt_observer)(void);
static bool lcdc_frame_range(uint64_t address, unsigned bytes) {
    return address >= RAM_BASE && address + bytes <= (uint64_t)RAM_BASE + sizeof(ram);
}
static int address_space_read(void *space, uint64_t address, unsigned attrs, void *out, unsigned bytes) {
    (void)space; (void)attrs;
    if (!lcdc_frame_range(address, bytes)) { return MEMTX_ERROR; }
    memcpy(out, ram + (address - RAM_BASE), bytes); return MEMTX_OK;
}
static int address_space_write(void *space, uint64_t address, unsigned attrs, const void *in, unsigned bytes) {
    (void)space; (void)attrs;
    if (!lcdc_frame_range(address, bytes)) { return MEMTX_ERROR; }
    memcpy(ram + (address - RAM_BASE), in, bytes); memory_writes++; return MEMTX_OK;
}
static bool qemu_loglevel_mask(unsigned mask) { (void)mask; return false; }
static int64_t qemu_clock_get_ns(unsigned clock) { (void)clock; return virtual_ns; }
static void timer_mod(QEMUTimer *timer, int64_t deadline) { timer->pending=true; timer->deadline=deadline; timer->schedules++; }
static void timer_del(QEMUTimer *timer) { timer->pending=false; timer->cancels++; }
static void qemu_set_irq(qemu_irq irq, int level) {
    (void)irq;
    if (level && !irq_level) { irq_rises++; }
    irq_level=level;
    if (level && interrupt_observer) { interrupt_observer(); }
}
/* Parent optional diagnostics have no role in the device lifecycle contract. */
static void gpu_trace_sync(Best1503State *s, const char *operation, hwaddr offset, uint64_t value) {
    (void)s; (void)operation; (void)offset; (void)value;
}
#include "gpu-lifecycle-functions.inc"
static unsigned checks;
#define CHECK(condition) do { checks++; if (!(condition)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#condition); exit(1); } } while (0)
static uint32_t bits(float f) { uint32_t u; memcpy(&u,&f,4); return u; }
static MemTxResult write_reg(Best1503State *s, unsigned address, unsigned value) {
    return gpu_probe_write(s,address,value,4,MEMTXATTRS_UNSPECIFIED);
}
static uint64_t read_reg(Best1503State *s, unsigned address) {
    uint64_t result=UINT64_MAX;
    CHECK(gpu_probe_read(s,address,&result,4,MEMTXATTRS_UNSPECIFIED)==MEMTX_OK);
    return result;
}
static void start(Best1503State *s, QEMUTimer *timer) {
    memset(s,0,sizeof(*s)); memset(timer,0,sizeof(*timer)); memset(ram,0,sizeof(ram));
    s->gpu_timer=timer; s->gpu_software_renderer=true; s->gpu_id_fixture=true;
    virtual_ns=1000; irq_level=0; irq_rises=0; memory_writes=0; interrupt_observer=NULL;
    s->gpu_sw_regs[0xa00]=0x10000102; s->gpu_sw_regs[0xa10]=0x103;
    s->gpu_sw_regs[0xa11]=RAM_BASE; s->gpu_sw_regs[0xa12]=4; s->gpu_sw_regs[0xa13]=0x10001;
    s->gpu_sw_regs[0xa34]=0x03300000; s->gpu_sw_regs[0xa3a]=0x10001;
    s->gpu_sw_regs[0xa40]=s->gpu_sw_regs[0xa44]=bits(1);
}
static unsigned make_draw(unsigned location, uint32_t paint, unsigned event) {
    uint8_t *command=ram+location; memset(command,0,80);
    stl_le_p(command,0x30010a02); stl_le_p(command+4,paint);
    uint32_t path[]={2,0,0,4,bits(1),0,4,bits(1),bits(1),4,0,bits(1),1,0};
    stl_le_p(command+8,0x40000000|sizeof(path)/8);
    for (unsigned i=0;i<G_N_ELEMENTS(path);i++) { stl_le_p(command+16+i*4,path[i]); }
    stl_le_p(command+72,event); return 80;
}
static void advance(Best1503State *s, int64_t now) {
    virtual_ns=now;
    if (s->gpu_timer->pending && now>=s->gpu_timer->deadline) {
        s->gpu_timer->pending=false; gpu_complete(s);
    }
}
static void deferred_commit_and_busy_preserves_first(void) {
    Best1503State s; QEMUTimer timer; start(&s,&timer);
    unsigned bytes=make_draw(256,0xff204080,0); make_draw(512,0xff112233,1);
    CHECK(write_reg(&s,0x14,3)==MEMTX_OK);
    CHECK(write_reg(&s,0x500,RAM_BASE+256)==MEMTX_OK);
    CHECK(write_reg(&s,0x504,bytes/8)==MEMTX_OK);
    CHECK(s.gpu_busy && s.gpu_work && s.gpu_pending_event==1 && !irq_level);
    CHECK(read_reg(&s,4)==0 && read_reg(&s,0x10)==0);
    CHECK(memory_writes==0 && ldl_le_p(ram)==0 && s.gpu_sw_regs[0xa02]==0);
    BesGpuWork *first=s.gpu_work; unsigned first_id=s.gpu_pending_id;
    int64_t deadline=timer.deadline; CHECK(deadline==101000);
    CHECK(write_reg(&s,0x500,RAM_BASE+512)==MEMTX_OK); /* Address staging isn't execution. */
    CHECK(s.gpu_work==first && s.gpu_pending_id==first_id && timer.schedules==1);
    CHECK(write_reg(&s,0x504,bytes/8)==MEMTX_ERROR);
    CHECK(s.gpu_work==first && s.gpu_pending_id==first_id && s.gpu_pending_event==1);
    CHECK(timer.schedules==1 && timer.deadline==deadline && s.gpu_status==0 && !irq_level);
    memset(ram+256,0xff,bytes); /* Active work owns a decoded snapshot. */
    advance(&s,deadline-1); CHECK(s.gpu_busy && memory_writes==0 && !irq_level);
    advance(&s,deadline); CHECK(!s.gpu_busy && !s.gpu_work && memory_writes==1);
    CHECK(ldl_le_p(ram)==0xff804020 && s.gpu_sw_regs[0xa02]==0xff204080);
    CHECK(s.gpu_status==1 && s.gpu_pending_event==0 && irq_level==1 && irq_rises==1);
    CHECK(read_reg(&s,4)==0xb05 && read_reg(&s,0x14)==3);
    CHECK(read_reg(&s,0x10)==1 && !irq_level && s.gpu_status==0);
    CHECK(read_reg(&s,0x10)==0 && irq_rises==1);
}
static void masked_completion_and_unacknowledged_status(void) {
    Best1503State s; QEMUTimer timer; start(&s,&timer);
    memset(ram+256,0,8); stl_le_p(ram+256,1); /* END event bit 1. */
    CHECK(write_reg(&s,0x500,RAM_BASE+256)==MEMTX_OK);
    CHECK(write_reg(&s,0x14,1)==MEMTX_OK);
    CHECK(write_reg(&s,0x504,1)==MEMTX_OK);
    advance(&s,timer.deadline);
    CHECK(s.gpu_status==2 && !s.gpu_busy && !irq_level);
    CHECK(write_reg(&s,0x14,2)==MEMTX_OK && irq_level==1);
    CHECK(write_reg(&s,0x504,1)==MEMTX_OK && s.gpu_busy);
    /* No guessed submission-time ACK: old completion remains until status read. */
    CHECK(s.gpu_status==2 && irq_level==1);
    CHECK(read_reg(&s,0x10)==2 && s.gpu_busy && !irq_level && s.gpu_pending_event==2);
    CHECK(read_reg(&s,0x10)==0 && s.gpu_busy);
    advance(&s,timer.deadline); CHECK(!s.gpu_busy && irq_level==1 && irq_rises==2);
    CHECK(read_reg(&s,0x10)==2 && !irq_level);
    CHECK(write_reg(&s,0x14,0)==MEMTX_OK);
    CHECK(write_reg(&s,0x504,1)==MEMTX_OK);
    advance(&s,timer.deadline); CHECK(s.gpu_status==2 && !irq_level);
    CHECK(read_reg(&s,0x10)==2 && !irq_level);
    CHECK(write_reg(&s,0x14,2)==MEMTX_OK && !irq_level); /* ACK while masked stays acknowledged. */
}
static Best1503State *observed_state;
static void acknowledge_and_resubmit_at_irq(void) {
    interrupt_observer=NULL; Best1503State *s=observed_state;
    CHECK(!s->gpu_busy && !s->gpu_work && memory_writes==1);
    CHECK(ldl_le_p(ram)==0xff804020 && s->gpu_sw_regs[0xa02]==0xff204080);
    CHECK(read_reg(s,0x10)==1 && !irq_level);
    CHECK(write_reg(s,0x500,RAM_BASE+512)==MEMTX_OK);
    CHECK(write_reg(s,0x504,10)==MEMTX_OK && s->gpu_busy && s->gpu_work);
}
static void completion_is_reentrant_after_irq_ack(void) {
    Best1503State s; QEMUTimer timer; start(&s,&timer);
    make_draw(256,0xff204080,0); make_draw(512,0xff112233,1);
    CHECK(write_reg(&s,0x14,3)==MEMTX_OK);
    CHECK(write_reg(&s,0x500,RAM_BASE+256)==MEMTX_OK);
    CHECK(write_reg(&s,0x504,10)==MEMTX_OK);
    observed_state=&s; interrupt_observer=acknowledge_and_resubmit_at_irq;
    advance(&s,timer.deadline);
    CHECK(s.gpu_busy && s.gpu_work && s.gpu_pending_event==2 && s.gpu_status==0);
    CHECK(timer.pending && timer.schedules==2 && memory_writes==1 && !irq_level);
    advance(&s,timer.deadline);
    CHECK(!s.gpu_busy && !s.gpu_work && memory_writes==2 && ldl_le_p(ram)==0xff332211);
    CHECK(read_reg(&s,0x10)==2 && !irq_level && irq_rises==2);
}
static void resets_cancel_uncommitted_work(void) {
    for (unsigned system=0;system<2;system++) {
        Best1503State s; QEMUTimer timer; start(&s,&timer);
        make_draw(256,0xff204080,0);
        CHECK(write_reg(&s,0x14,1)==MEMTX_OK);
        CHECK(write_reg(&s,0x500,RAM_BASE+256)==MEMTX_OK);
        CHECK(write_reg(&s,0x504,10)==MEMTX_OK && s.gpu_work);
        int64_t deadline=timer.deadline;
        s.gpu_status=1; gpu_update_irq(&s); CHECK(irq_level==1);
        if (system) { production_gpu_reset(&s); }
        else { CHECK(write_reg(&s,0,1u<<12)==MEMTX_OK); }
        CHECK(!timer.pending && timer.cancels==1 && !s.gpu_work && !s.gpu_busy);
        CHECK(!s.gpu_status && !s.gpu_pending_event && !irq_level && memory_writes==0);
        bool registers_clear=true;
        for (unsigned i=0;i<BES_GPU_REGS;i++) { registers_clear &= s.gpu_sw_regs[i]==0; }
        CHECK(registers_clear);
        CHECK(ldl_le_p(ram)==0 && s.gpu_probe_mask==(system?0:1));
        advance(&s,deadline+1000000); CHECK(!s.gpu_status && !irq_level && memory_writes==0);
        CHECK(read_reg(&s,4)==0xb05 && read_reg(&s,0x10)==0);
        memset(ram+256,0,8);
        CHECK(write_reg(&s,0x500,RAM_BASE+256)==MEMTX_OK);
        CHECK(write_reg(&s,0x14,1)==MEMTX_OK);
        CHECK(write_reg(&s,0x504,1)==MEMTX_OK && s.gpu_busy);
        advance(&s,timer.deadline); CHECK(read_reg(&s,0x10)==1 && !s.gpu_busy && !irq_level);
    }
}
static void invalid_commands_do_not_schedule_an_end(void) {
    Best1503State s; QEMUTimer timer; start(&s,&timer);
    CHECK(write_reg(&s,0x500,RAM_BASE+257)==MEMTX_ERROR);
    CHECK(write_reg(&s,0x500,RAM_BASE+256)==MEMTX_OK);
    stl_le_p(ram+256,0xf0000000);
    CHECK(write_reg(&s,0x504,1)==MEMTX_ERROR);
    CHECK(!s.gpu_busy && !s.gpu_work && !timer.pending && !s.gpu_status && !s.gpu_pending_event);
    CHECK(!irq_level && memory_writes==0 && timer.schedules==0);
    CHECK(write_reg(&s,0x504,0)==MEMTX_ERROR);
    CHECK(write_reg(&s,0x504,8193)==MEMTX_ERROR);
    CHECK(write_reg(&s,0x520,4)==MEMTX_OK && !timer.pending && !s.gpu_busy);
}
int main(void) {
    deferred_commit_and_busy_preserves_first();
    masked_completion_and_unacknowledged_status();
    completion_is_reentrant_after_irq_ack();
    resets_cancel_uncommitted_work();
    invalid_commands_do_not_schedule_an_end();
    printf("GPU lifecycle production contract: %u checks passed; host output IRQ only, no guest/NVIC\n",checks);
    return 0;
}
