/* Exercise production SPI and GPIO glue, including deferred host observation. */
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include "azki-q65-imu.h"
typedef int MemTxResult, MemTxAttrs, QemuMutex;
typedef uint64_t hwaddr;
typedef struct { uint8_t bytes[256]; } MemoryRegion;
typedef struct { int64_t deadline; } QEMUTimer;
typedef struct Best1503State Best1503State;
#define MEMTX_OK 0
#define MEMTX_ERROR 1
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define qemu_log_mask(...) ((void)0)
#define QEMU_CLOCK_VIRTUAL 0
static int64_t clock_ns;
static int64_t qemu_clock_get_ns(int c) { (void)c; return clock_ns; }
static void qemu_mutex_lock(QemuMutex *m) { (void)m; }
static void qemu_mutex_unlock(QemuMutex *m) { (void)m; }
static void qemu_set_irq(int *irq, bool value) { *irq=value; }
static void *memory_region_get_ram_ptr(MemoryRegion *m) { return m->bytes; }
static uint32_t ldl_le_p(void *p) { uint32_t v; memcpy(&v,p,4); return v; }
static void stl_le_p(void *p,uint32_t v) { memcpy(p,&v,4); }
static void timer_mod(QEMUTimer *t,int64_t n) { t->deadline=n; }
static void timer_del(QEMUTimer *t) { t->deadline=0; }
#include "q65-spi-state.inc"
typedef struct { uint32_t regs[32],input,pending; } BesGpio;
struct Best1503State {
    bool spi0_unconnected,gh3x2x_fixture,sensor_ready;
    BesSpiConfig sensor_spi[2]; BesGpio gpio[3]; MemoryRegion psc;
    int *sensor_gpio_irq,*panel_te_irq;
    Q65Imu imu; QemuMutex imu_lock; QEMUTimer *imu_timer;
    unsigned imu_trace_count;
};
static bool gh_transfer(BesSpiConfig *s,uint32_t v) { (void)s;(void)v;abort(); }
#include "q65-imu-glue.inc"
static Best1503State s;
static QEMUTimer timer;
static int sensor_irq,ap_irq;
static void tx(uint8_t value)
{ assert(spi0_write(&s.sensor_spi[1],8,value,1,0)==MEMTX_OK); }
static uint8_t rx(void)
{ uint64_t v; assert(spi0_read(&s.sensor_spi[1],8,&v,1,0)==MEMTX_OK); return v; }
static void select(bool active)
{
    s.gpio[0].regs[1]|=1u<<21;
    if(active) s.gpio[0].regs[0]&=~(1u<<21); else s.gpio[0].regs[0]|=1u<<21;
    sensor_imu_pins(&s);
}
static void write_reg(uint8_t reg,uint8_t value)
{ select(true);tx(reg);rx();tx(value);rx();select(false); }
int main(void)
{
    s.sensor_ready=true;s.imu.enabled=true;s.imu_timer=&timer;
    s.sensor_gpio_irq=&sensor_irq;s.panel_te_irq=&ap_irq;
    q65_imu_reset(&s.imu,0);
    BesSpiConfig *spi=&s.sensor_spi[1];spi->machine=&s;spi->unconnected_only=true;
    spi->regs[0]=0x70100;spi->regs[1]=2;
    s.gpio[2].regs[0x30/4]=1u<<7;s.gpio[2].regs[0x38/4]=1u<<7;
    s.gpio[2].regs[0x3c/4]=1u<<7;
    sensor_gpio_summary_write(&s,8,1u<<7,4,0);
    select(true);tx(0x8f);assert(rx()==0xff);tx(0);assert(rx()==0x6c);select(false);
    assert(!s.imu.fifo_read_count && !s.imu.output_read_count);
    q65_imu_input_set(&s.imu,"accel=17,-18,19;gyro=20,-21,22");
    write_reg(0x10,0x20);write_reg(9,2);write_reg(0x0a,6);
    write_reg(7,1);write_reg(0x0d,8);
    clock_ns=38461538;sensor_imu_schedule(&s);
    assert(sensor_irq && !ap_irq && s.imu.count==1);
    uint64_t value;sensor_gpio_summary_read(&s,20,&value,4,0);assert(value==(1u<<7));
    gpio_summary_read(&s,20,&value,4,0);assert(!value); /* AP cannot ACK sensor pin. */
    select(true);tx(0xf8);rx();
    for(unsigned i=0;i<7;i++) tx(0);
    assert(s.imu.fifo_pops==1 && !s.imu.fifo_read_count && !s.imu.last_read_version);
    uint8_t bytes[7];for(unsigned i=0;i<6;i++) bytes[i]=rx();
    assert(!s.imu.fifo_read_count);bytes[6]=rx();
    assert(bytes[0]>>3==2 && bytes[1]==17 && bytes[2]==0 && bytes[3]==0xee && bytes[4]==0xff);
    assert(s.imu.fifo_read_count==1 && s.imu.last_read_version==1);
    assert(rx()==0 && s.imu.fifo_read_count==1);select(false);
    s.gpio[2].pending=0;sensor_imu_schedule(&s);assert(!sensor_irq);
    /* Domain mask disables delivery without destroying pending edge. */
    clock_ns+=38461538;sensor_imu_schedule(&s);assert(sensor_irq);
    sensor_gpio_summary_write(&s,8,0,4,0);assert(!sensor_irq && s.gpio[2].pending);
    sensor_gpio_summary_write(&s,8,1u<<7,4,0);assert(sensor_irq && !ap_irq);
    /* RX overrun can drop the first output byte. A later last-byte dequeue
     * must not claim that the whole word was consumed. */
    select(true);tx(0x8f);rx();for(unsigned i=0;i<8;i++) tx(0);
    select(false);select(true);tx(0xa8);tx(0); /* command + X low both discarded */
    for(unsigned i=0;i<8;i++) rx();
    uint64_t words=s.imu.output_read_count;tx(0);rx(); /* X high survives */
    assert(s.imu.output_read_count==words);select(false);
    /* Default-disabled extension leaves the open bus and timer unchanged. */
    s.imu.enabled=false;sensor_imu_schedule(&s);
    select(true);tx(0x8f);assert(rx()==0xff);tx(0);assert(rx()==0xff);select(false);
    puts("Production IMU SPI dequeue and independent sensor GPIO IRQ passed");
}
