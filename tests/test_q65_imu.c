/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "azki-q65-imu.h"
static Q65Imu m;
static unsigned checks;
#define CHECK(x) do { checks++; assert(x); } while (0)
static void init(void)
{ memset(&m,0,sizeof(m)); m.enabled=true; q65_imu_reset(&m,0); }
static void put(unsigned reg,unsigned val) { q65_imu_write(&m,reg,val); }
static unsigned get(unsigned reg) { return q65_imu_read(&m,reg).value; }
static Q65ImuByte byte(unsigned tx) { return q65_imu_transfer(&m,tx); }
static void select(bool on) { q65_imu_select(&m,on); }
static void consume_record(uint8_t out[7])
{
    select(true); byte(0xf8);
    for(unsigned i=0;i<7;i++) { Q65ImuByte b=byte(0xff); out[i]=b.value; q65_imu_observe(&m,b); }
    select(false);
}
int main(void)
{
    init(); CHECK(get(0x0f)==0x6c && get(0x12)==4);
    CHECK(byte(0x8f).value==0xff); /* No chip select, no transaction. */
    select(true); CHECK(byte(0x8f).value==0xff && byte(0).value==0x6c); select(false);
    CHECK(q65_imu_input_set(&m,"accel=-32768,0,32767;gyro=123,-456,789"));
    CHECK(m.input_version==1 && m.input[0]==-32768 && m.input[5]==789);
    const char *bad[]={"accel=1,2,3;gyro=4,5,32768","accel=1,2,3;gyro=4,5,-32769",
        "accel=1,2,3;gyro=4,5,99999999999999999999999999","accel=1,2,3;gyro=4,5,6junk",
        "accel=1,2,3;gyro=4,5,", "accel=+1,2,3;gyro=4,5,6"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) {
        CHECK(!q65_imu_input_set(&m,bad[i])); CHECK(m.input_version==1 && m.input[0]==-32768);
    }
    q65_imu_advance(&m,1000000000); CHECK(!m.samples[0] && !m.samples[1]);
    put(0x10,0x20); put(0x11,0x30); put(9,0x32); put(0x0a,6); put(7,3); put(0x0d,8);
    q65_imu_advance(&m,1000000000);
    q65_imu_advance(&m,1000000000+38461538);
    CHECK(m.samples[0]==2 && m.samples[1]==1 && m.count==3);
    CHECK(q65_imu_irq(&m)); CHECK(get(0x3a)==3 && (get(0x3b)&0x80));
    uint8_t data[7]; consume_record(data);
    CHECK(data[0]>>3==1 && data[1]==123 && data[2]==0);
    CHECK(__builtin_parity((unsigned)data[0])==0);
    CHECK(m.fifo_read_count==1 && m.last_read_version==1 && m.fifo_pops==1);
    CHECK(!q65_imu_irq(&m));
    /* Host update cannot relabel old queued FIFO words. */
    CHECK(q65_imu_input_set(&m,"accel=10,20,30;gyro=40,50,60"));
    consume_record(data); CHECK(m.last_read_version==1);
    consume_record(data); CHECK(data[0]>>3==2 && data[1]==0 && data[2]==0x80);
    CHECK(m.last_read_version==1);
    q65_imu_advance(&m,1000000000+57692307);
    select(true); byte(0xf8);
    Q65ImuByte pending[7];
    for(unsigned i=0;i<7;i++) pending[i]=byte(0);
    Q65ImuByte completion=pending[6];
    select(false);
    CHECK(completion.observation==2 && completion.version==2);
    CHECK(m.fifo_read_count==3 && m.last_read_version==1); /* Not guest-observed yet. */
    for(unsigned i=0;i<7;i++) q65_imu_observe(&m,pending[i]);
    CHECK(m.fifo_read_count==4 && m.last_read_version==2);
    /* Reset retains accepted host input, but clears hardware FIFO/config. */
    put(0x12,1); CHECK(!m.count && !m.regs[0x10] && m.input_version==2 && get(0x12)==5);
    q65_imu_advance(&m,m.now_ns+49999); CHECK(get(0x12)==5);
    q65_imu_advance(&m,m.now_ns+1); CHECK(get(0x12)==4);
    /* Pair-specific BDU freezes an initiated pair, not the entire vector. */
    put(0x12,0x44); put(0x10,0x30); put(0x0d,1);
    int64_t t=m.now_ns; q65_imu_advance(&m,t); q65_imu_advance(&m,t+19230769);
    Q65ImuByte lo=q65_imu_read(&m,0x28);
    CHECK(lo.value==10 && q65_imu_irq(&m));q65_imu_observe(&m,lo);
    CHECK(q65_imu_input_set(&m,"accel=257,514,771;gyro=0,0,0"));
    q65_imu_advance(&m,t+38461538);
    Q65ImuByte hi=q65_imu_read(&m,0x29);
    CHECK(hi.value==0 && hi.version==2 && hi.observation==1);
    CHECK(get(0x2a)==2 && get(0x2b)==2); /* Y refreshed while X pair was held. */
    q65_imu_observe(&m,hi); CHECK(m.output_read_count==1 && m.last_read_version==2);
    CHECK(get(0x2c)==3 && get(0x2d)==3 && !q65_imu_irq(&m));
    q65_imu_advance(&m,t+57692307); CHECK(get(0x28)==1 && get(0x29)==1);
    /* 512 complete tagged records; continuous mode replaces oldest. */
    init(); q65_imu_input_set(&m,"accel=1,2,3;gyro=4,5,6");
    put(0x10,0x40); put(9,4); put(0x0a,6); q65_imu_advance(&m,0);
    q65_imu_advance(&m,9615385LL*513);
    CHECK(m.count==512 && m.overrun && m.dropped==1);
    CHECK(get(0x3a)==0 && (get(0x3b)&0x6a)==0x6a);
    CHECK(!(get(0x3b)&8));
    /* One SPI burst wraps7e->78 and pops exactly two records. */
    select(true); byte(0xf8);
    for(unsigned i=0;i<14;i++) { Q65ImuByte b=byte(0); q65_imu_observe(&m,b); }
    select(false); CHECK(m.count==510 && m.fifo_read_count==2);
    put(0x0a,0); CHECK(!m.count && !m.overrun);
    put(0x0a,1); q65_imu_advance(&m,m.now_ns); q65_imu_advance(&m,9615385LL*1026);
    CHECK(m.count==512 && !m.overrun); /* FIFO mode stops at capacity. */
    put(0x0a,0); put(0x12,0); /* IF_INC disabled, repeated address remains stable. */
    select(true); byte(0x8f); CHECK(byte(0).value==0x6c && byte(0).value==0x6c); select(false);
    /* Timestamp only advances with TIMESTAMP_EN; explicit reset is supported. */
    init(); q65_imu_advance(&m,1000000); CHECK(get(0x40)==0);
    put(0x19,0x20); q65_imu_advance(&m,1250000); CHECK(get(0x40)==10);
    put(0x19,0); q65_imu_advance(&m,2250000); CHECK(get(0x40)==10);
    put(0x42,0xaa); CHECK(get(0x40)==0);
    /* BOOT reload keeps configuration; SW_RESET clears it. */
    put(7,23); put(0x0d,4); put(0x12,0x84);
    CHECK(q65_imu_irq(&m) && get(7)==23 && (get(0x12)&0x80));
    q65_imu_advance(&m,m.now_ns+10000000);
    CHECK(!q65_imu_irq(&m) && get(7)==23 && get(0x12)==4);
    /* Read-only timestamp and unsupported engines have explicit diagnostics. */
    put(0x40,0xff); CHECK(get(0x40)==0 && !m.regs[0x40]);
    uint64_t unsupported=m.unsupported_count;
    put(0x56,1); CHECK(m.unsupported_count==unsupported+1);
    CHECK(get(0x56)==1 && m.unsupported_count==unsupported+2);
    put(1,0x80);put(0x10,3);CHECK(get(0x10)==3);
    CHECK(m.unsupported_count==unsupported+4);put(1,0);
    put(0x0d,0);put(0x0e,4);put(0x13,0x20);put(0x12,0x84);
    CHECK(!q65_imu_irq(&m)); /* INT2 temperature route cannot synthesize BOOT. */
    /* FIFO FULL means the next record fills the selected capacity. */
    init(); put(0x10,0x20);put(9,2);put(0x0a,1);q65_imu_advance(&m,0);
    q65_imu_advance(&m,38461538LL*511);CHECK(m.count==511 && (get(0x3b)&32));
    q65_imu_advance(&m,38461538LL*512);CHECK(m.count==512 && m.fifo_stopped);
    consume_record(data);q65_imu_advance(&m,38461538LL*513);
    CHECK(m.count==511 && m.fifo_stopped); /* A read cannot restart stopped FIFO mode. */
    put(0x0a,0);put(0x0a,1);q65_imu_advance(&m,m.now_ns);
    q65_imu_advance(&m,m.now_ns+38461538);CHECK(m.count==1 && !m.fifo_stopped);
    /* STOP_ON_WTM changes capacity; continuous mode still overwrites. */
    init();put(7,3);put(8,0x80);put(0x10,0x20);put(9,2);put(0x0a,6);
    q65_imu_advance(&m,0);q65_imu_advance(&m,38461538LL*2);
    CHECK(m.count==2 && (get(0x3b)&32));
    q65_imu_advance(&m,38461538LL*4);CHECK(m.count==3 && m.overrun && m.dropped==1);
    /* Batching has an independent phase: 26Hz output / 12.5Hz FIFO. */
    init();put(0x10,0x20);put(9,1);put(0x0a,6);q65_imu_advance(&m,0);
    for(int64_t now=1000000;now<=1000000000;now+=1000000) q65_imu_advance(&m,now);
    CHECK(m.samples[1]==26 && m.count==12 && !m.dropped);
    /* Large gaps retain BDR records, not only the last512 high-rate ODR samples. */
    init();put(0x10,0xa0);put(9,1);put(0x0a,6);q65_imu_advance(&m,0);
    q65_imu_advance(&m,1000000000);CHECK(m.samples[1]==6666 && m.count==12 && !m.dropped);
    put(0x0a,0);q65_imu_advance(&m,2000000000);CHECK(!m.count && !m.dropped);
    /* Gyro BDR code11 is6.5Hz; accel code11 is1.6Hz. */
    init();put(0x11,0x20);put(9,0xb0);put(0x0a,6);q65_imu_advance(&m,0);
    q65_imu_advance(&m,1000000000);CHECK(m.count==6 && m.samples[0]==26);
    /* A single long virtual step and frequent callbacks retain identical
     * chronological FIFO suffixes and drop totals, even with two BDRs. */
    static Q65Imu frequent;
    init();q65_imu_input_set(&m,"accel=1,2,3;gyro=-4,-5,-6");
    put(0x10,0x50);put(0x11,0x50);put(9,0x21);put(0x0a,6);
    q65_imu_advance(&m,0);frequent=m;
    for(int64_t now=10000000;now<=80000000000LL;now+=10000000) q65_imu_advance(&frequent,now);
    q65_imu_advance(&m,80000000000LL);
    CHECK(m.count==frequent.count && m.dropped==frequent.dropped);
    CHECK(m.samples[0]==frequent.samples[0] && m.samples[1]==frequent.samples[1]);
    for(unsigned i=0;i<m.count;i++) {
        CHECK(!memcmp(m.fifo[(m.head+i)%512].bytes,frequent.fifo[(frequent.head+i)%512].bytes,7));
    }
    /* Missing any RX byte is not a complete FIFO or output sample. */
    Q65ImuByte fragment[7];
    for(unsigned i=0;i<7;i++) fragment[i]=q65_imu_read(&m,0x78+i);
    for(unsigned i=1;i<7;i++) q65_imu_observe(&m,fragment[i]);
    CHECK(!m.fifo_read_count && !m.axis_read_count[3]);
    q65_imu_observe(&m,fragment[0]);CHECK(m.fifo_read_count==1 && m.axis_read_count[3]==1);
    q65_imu_observe(&m,fragment[6]);CHECK(m.fifo_read_count==1); /* No duplicate count. */
    lo=q65_imu_read(&m,0x22);hi=q65_imu_read(&m,0x23);
    q65_imu_observe(&m,hi);CHECK(!m.output_read_count);
    q65_imu_observe(&m,lo);CHECK(m.output_read_count==1);
    printf("Q65 raw IMU: %u checks passed\n",checks); return 0;
}
