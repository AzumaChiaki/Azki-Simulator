/* SPDX-License-Identifier: GPL-2.0-or-later
 * Production IMU bus contract only. No QEMU guest, QMP or process controller. */
#include <assert.h>
#include <glib.h>
#include "../engine/qemu-overlay/hw/arm/best1503-sensors.h"

static unsigned checks;
static int64_t clock_ns;
static int64_t now(void *p) { (void)p; return clock_ns; }
static void schedule(void *p, int64_t when) { (void)p; (void)when; }
static void pin(void *p, int i, bool high) { (void)p; (void)i; (void)high; }
static void message(void *p, const char *s) { (void)p; (void)s; }
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "motion-raw check failed at line %d: %s\n", __LINE__, #c); abort(); \
} } while (0)

static const char *raw_a = "ax=321;ay=-654;az=12000;gx=-77;gy=88;gz=-99";
static const char *raw_b = "ax=-32768;ay=32767;az=0;gx=32767;gy=-32768;gz=1";
static const int16_t axes_a[6] = {321, -654, 12000, -77, 88, -99};

static void setup(BesSensorHub *h, BesImuKind chip, bool gyro, bool fifo)
{
    memset(h, 0, sizeof(*h));
    clock_ns = 1000000;
    h->host = (BesSensorHost){.now_ns=now, .schedule=schedule,
        .set_input=pin, .log=message};
    h->imu.kind = chip;
    h->imu.port.irq_pin = -1;
    bes_sensors_reset(h);
    CHECK(bes_parse_mount("+y+x-z", h->imu.mount));
    if (chip == BES_IMU_LSM6DSO) {
        h->imu.regs[LSM_CTRL1_XL] = 0x30;
        h->imu.regs[LSM_CTRL2_G] = gyro ? 0x30 : 0;
        h->imu.regs[LSM_FIFO_CTRL3] = gyro ? 0x33 : 3;
        h->imu.regs[LSM_FIFO_CTRL4] = fifo ? 6 : 0;
    } else {
        h->imu.regs[ICM_PWR_MGMT0] = gyro ? 0x0f : 3;
        h->imu.regs[ICM_ACCEL_CONFIG0] = 0x6a;
        h->imu.regs[ICM_GYRO_CONFIG0] = 0x6a;
        h->imu.regs[ICM_FIFO_CONFIG1] = fifo ? 0 : 1;
        h->imu.mreg[1][1] = gyro ? 3 : 1;
    }
}

static void sample(BesSensorHub *h)
{
    bes_imu_update(h);
    clock_ns += 25000000;
    bes_imu_update(h);
}

static uint8_t direct_base(BesImuKind chip, unsigned component)
{
    return chip == BES_IMU_LSM6DSO ? (component ? 0x22 : 0x28) :
        (component ? 0x11 : 0x0b);
}

static void read_group(BesSensorHub *h, unsigned component, int16_t output[3])
{
    uint8_t bytes[6], base = direct_base(h->imu.kind, component);
    for (unsigned i=0; i<6; i++) bytes[i] = bes_imu_read(h, base+i);
    bool big = h->imu.kind == BES_IMU_ICM42607 && (h->imu.regs[ICM_INTF_CONFIG0] & 0x10);
    for (unsigned i=0; i<3; i++) output[i] = big ?
        (int16_t)((uint16_t)bytes[i*2]<<8 | bytes[i*2+1]) :
        (int16_t)((uint16_t)bytes[i*2+1]<<8 | bytes[i*2]);
}

static void input_contract(void)
{
    BesSensorHub h;
    setup(&h, BES_IMU_LSM6DSO, true, false);
    CHECK(h.motion_version == 0);
    CHECK(bes_motion_raw_set(&h, raw_b));
    CHECK(h.motion_version == 1 && h.raw_motion_enabled);
    CHECK(h.raw_accel[0] == -32768 && h.raw_accel[1] == 32767 && h.raw_accel[2] == 0);
    CHECK(h.raw_gyro[0] == 32767 && h.raw_gyro[1] == -32768 && h.raw_gyro[2] == 1);
    const char *bad[] = {NULL, "", "off", "AUTO", "auto ",
        "ax=32768;ay=0;az=0;gx=0;gy=0;gz=0",
        "ax=-32769;ay=0;az=0;gx=0;gy=0;gz=0",
        "ax=1;ay=2;az=3;gx=4;gy=5", "ax=1;ay=2;az=3;gx=4;gy=5;gz=6;gx=7",
        "ax=1.0;ay=2;az=3;gx=4;gy=5;gz=6", "ax=nan;ay=2;az=3;gx=4;gy=5;gz=6",
        "ax=+1;ay=2;az=3;gx=4;gy=5;gz=6", "ax= 1;ay=2;az=3;gx=4;gy=5;gz=6",
        "ax=0x10;ay=2;az=3;gx=4;gy=5;gz=6", "ax=1;ay=2;az=3;gx=4;gy=5;gz=6x",
        "ax=1;ay=2;az=3;gx=4;gy=5;gz=6\n", "ax=1;ay=2;az=3;gx=4;gy=5;gz=99999999999999999999999"};
    for (unsigned i=0; i<G_N_ELEMENTS(bad); i++) {
        CHECK(!bes_motion_raw_set(&h, bad[i]));
        CHECK(h.motion_version == 1 && h.raw_motion_enabled);
        CHECK(h.raw_accel[0] == -32768 && h.raw_gyro[2] == 1);
    }
    CHECK(bes_motion_raw_set(&h, "auto"));
    CHECK(h.motion_version == 2 && !h.raw_motion_enabled);
    h.motion_version = UINT64_MAX;
    CHECK(!bes_motion_raw_set(&h, raw_a));
    CHECK(h.motion_version == UINT64_MAX && !h.raw_motion_enabled);
}

static void direct_contract(BesImuKind chip)
{
    BesSensorHub h;
    setup(&h, chip, true, false);
    CHECK(bes_motion_raw_set(&h, raw_a));
    CHECK(!h.imu.sample_valid[0] && !h.imu.sample_valid[1]);
    sample(&h);
    CHECK(h.imu.sample_valid[0] && h.imu.sample_valid[1]);
    CHECK(h.imu.generated_version[0] == 1 && h.imu.generated_version[1] == 1);
    int16_t output[3];
    for (unsigned c=0; c<2; c++) {
        read_group(&h, c, output);
        CHECK(!memcmp(output, axes_a+c*3, sizeof(output)));
        CHECK(h.imu.reg_bus_reads[c] == 1 && h.imu.reg_bus_version[c] == 1);
    }
    uint8_t base = direct_base(chip, 0);
    for (unsigned i=0; i<3; i++) (void)bes_imu_read(&h, base+i);
    CHECK(h.imu.reg_bus_reads[0] == 1);
    sample(&h); /* Old prefix plus new suffix must not complete a sample. */
    for (unsigned i=3; i<6; i++) (void)bes_imu_read(&h, base+i);
    CHECK(h.imu.reg_bus_reads[0] == 1);
    for (unsigned i=0; i<3; i++) (void)bes_imu_read(&h, base+i);
    CHECK(h.imu.reg_bus_reads[0] == 2);
    uint64_t old = h.imu.reg_bus_reads[0];
    for (unsigned i=0; i<8; i++) (void)bes_imu_read(&h, base+5);
    CHECK(h.imu.reg_bus_reads[0] == old);
    CHECK(bes_motion_raw_set(&h, raw_b));
    read_group(&h, 0, output); /* Queued output is still the old conversion. */
    CHECK(h.imu.reg_bus_version[0] == 1);
    sample(&h);
    read_group(&h, 0, output);
    CHECK(h.imu.reg_bus_version[0] == 2 && output[0] == -32768 && output[1] == 32767);
    if (chip == BES_IMU_ICM42607) {
        h.imu.regs[ICM_INTF_CONFIG0] &= ~0x10;
        read_group(&h, 0, output);
        CHECK(output[0] == -32768 && output[1] == 32767);
    }
    setup(&h, chip, false, false);
    CHECK(bes_motion_raw_set(&h, raw_a)); sample(&h);
    read_group(&h, 1, output);
    CHECK(!h.imu.sample_valid[1] && h.imu.reg_bus_reads[1] == 0);
}

static void lsm_fifo_contract(void)
{
    BesSensorHub h;
    setup(&h, BES_IMU_LSM6DSO, false, true);
    CHECK(bes_motion_raw_set(&h, raw_a)); sample(&h);
    unsigned initial = h.imu.fifo_count;
    CHECK(initial > 0);
    (void)bes_imu_read(&h, LSM_FIFO_TAG+6);
    CHECK(h.imu.fifo_count == initial-1 && h.imu.fifo_bus_reads[0] == 0);
    sample(&h);
    for (unsigned i=0; i<3; i++) (void)bes_imu_read(&h, LSM_FIFO_TAG+i);
    CHECK(h.imu.fifo_bus_reads[0] == 0);
    for (unsigned i=3; i<7; i++) (void)bes_imu_read(&h, LSM_FIFO_TAG+i);
    CHECK(h.imu.fifo_bus_reads[0] == 1 && h.imu.fifo_bus_version[0] == 1);
    CHECK(h.imu.fifo_bus_reads[1] == 0);
    sample(&h);
    CHECK(bes_motion_raw_set(&h, raw_b));
    for (unsigned i=0; i<7; i++) (void)bes_imu_read(&h, LSM_FIFO_TAG+i);
    CHECK(h.imu.fifo_bus_version[0] == 1); /* Already queued before host set. */
    lsm_write(&h, LSM_FIFO_CTRL4, 0);
    CHECK(h.imu.fifo_count == 0);
    lsm_write(&h, LSM_FIFO_CTRL4, 6); sample(&h);
    for (unsigned i=0; i<7; i++) (void)bes_imu_read(&h, LSM_FIFO_TAG+i);
    CHECK(h.imu.fifo_bus_version[0] == 2);

    setup(&h, BES_IMU_LSM6DSO, false, true);
    CHECK(bes_motion_raw_set(&h, raw_a)); sample(&h);
    clock_ns += 12000000000LL; bes_imu_update(&h);
    CHECK(h.imu.fifo_count == BES_IMU_FIFO_WORDS);
    for (unsigned i=0; i<3; i++) (void)bes_imu_read(&h, LSM_FIFO_TAG+i);
    sample(&h); /* Stream overwrite invalidates the incomplete oldest word. */
    for (unsigned i=3; i<7; i++) (void)bes_imu_read(&h, LSM_FIFO_TAG+i);
    CHECK(h.imu.fifo_bus_reads[0] == 0);
    for (unsigned i=0; i<7; i++) (void)bes_imu_read(&h, LSM_FIFO_TAG+i);
    CHECK(h.imu.fifo_bus_reads[0] == 1);
}

static void icm_bytes(BesSensorHub *h, unsigned n)
{
    for (unsigned i=0; i<n; i++) (void)bes_imu_read(h, ICM_FIFO_DATA);
}

static void icm_fifo_contract(void)
{
    BesSensorHub h;
    setup(&h, BES_IMU_ICM42607, true, true);
    CHECK(bes_motion_raw_set(&h, raw_a)); sample(&h);
    icm_bytes(&h, 5);
    CHECK(h.imu.fifo_bus_reads[0] == 0 && h.imu.fifo_bus_reads[1] == 0);
    icm_bytes(&h, 10);
    CHECK(h.imu.fifo_bus_reads[0] == 0);
    icm_bytes(&h, 1);
    CHECK(h.imu.fifo_bus_reads[0] == 1 && h.imu.fifo_bus_reads[1] == 1);
    CHECK(h.imu.fifo_bus_version[0] == 1 && h.imu.fifo_bus_version[1] == 1);
    sample(&h); icm_bytes(&h, 7);
    icm_write(&h, 0x02, 4); /* FIFO flush must discard incomplete receipt. */
    sample(&h); icm_bytes(&h, 9);
    CHECK(h.imu.fifo_bus_reads[0] == 1);
    icm_bytes(&h, 7);
    CHECK(h.imu.fifo_bus_reads[0] == 2);
    setup(&h, BES_IMU_ICM42607, true, true);
    h.imu.mreg[1][1] = 1; /* Gyro runs, but FIFO only captures accel. */
    CHECK(bes_motion_raw_set(&h, raw_a)); sample(&h); icm_bytes(&h, 16);
    CHECK(h.imu.sample_valid[1]);
    CHECK(h.imu.fifo_bus_reads[0] == 1 && h.imu.fifo_bus_reads[1] == 0);

    setup(&h, BES_IMU_ICM42607, false, true);
    CHECK(bes_motion_raw_set(&h, raw_a)); sample(&h);
    clock_ns += 3000000000LL; bes_imu_update(&h);
    CHECK(h.imu.bfifo_count == sizeof(h.imu.bfifo));
    icm_bytes(&h, 5); sample(&h); /* Partial oldest packet is overwritten. */
    icm_bytes(&h, 11);
    CHECK(h.imu.fifo_bus_reads[0] == 0);
    icm_bytes(&h, 5);
    CHECK(h.imu.fifo_bus_reads[0] == 1);
}

static void reset_and_auto_contract(BesImuKind chip)
{
    BesSensorHub h, original;
    setup(&h, chip, true, true);
    CHECK(bes_motion_raw_set(&h, raw_a)); sample(&h);
    int16_t output[3]; read_group(&h, 0, output);
    uint64_t reads = h.imu.reg_bus_reads[0];
    (void)bes_imu_read(&h, direct_base(chip, 0));
    if (chip == BES_IMU_LSM6DSO) lsm_reset(&h.imu); else icm_reset(&h.imu);
    CHECK(h.imu.reg_bus_reads[0] == reads);
    CHECK(!h.imu.sample_valid[0] && !h.imu.sample_valid[1]);
    CHECK(h.imu.reg_read_mask[0] == 0 && h.imu.reg_read_mask[1] == 0);
    CHECK(h.motion_version == 1 && h.raw_motion_enabled);
    bes_sensors_reset(&h);
    CHECK(h.imu.reg_bus_reads[0] == 0 && h.imu.fifo_bus_reads[0] == 0);
    CHECK(h.motion_version == 1 && h.raw_motion_enabled);
    setup(&h, chip, true, false);
    original = h;
    CHECK(bes_motion_raw_set(&h, raw_a));
    CHECK(bes_motion_raw_set(&h, "auto"));
    bes_imu_update(&h); bes_imu_update(&original);
    clock_ns += 25000000;
    bes_imu_update(&h); bes_imu_update(&original);
    CHECK(!memcmp(h.imu.accel_raw, original.imu.accel_raw, sizeof(h.imu.accel_raw)));
    CHECK(!memcmp(h.imu.gyro_raw, original.imu.gyro_raw, sizeof(h.imu.gyro_raw)));
}

static void disable_after_conversion(BesImuKind chip)
{
    BesSensorHub h;
    setup(&h, chip, true, false);
    CHECK(bes_motion_raw_set(&h, raw_a)); sample(&h);
    CHECK(h.imu.sample_valid[0] && h.imu.sample_valid[1]);
    (void)bes_imu_read(&h, direct_base(chip, 1));
    bes_imu_write(&h, chip == BES_IMU_LSM6DSO ? LSM_CTRL2_G : ICM_PWR_MGMT0,
                  chip == BES_IMU_LSM6DSO ? 0 : 3);
    sample(&h);
    CHECK(h.imu.sample_valid[0] && !h.imu.sample_valid[1]);
    CHECK(h.imu.reg_read_mask[1] == 0);
    bes_imu_write(&h, chip == BES_IMU_LSM6DSO ? LSM_CTRL1_XL : ICM_PWR_MGMT0, 0);
    sample(&h);
    CHECK(!h.imu.sample_valid[0] && !h.imu.sample_valid[1]);
}

static void describe_contract(void)
{
    BesSensorHub h;
    memset(&h, 0, sizeof(h));
    char *text = bes_motion_raw_describe(&h);
    CHECK(!strcmp(text, "unsupported")); g_free(text);
    for (int chip=BES_IMU_LSM6DSO; chip<=BES_IMU_ICM42607; chip++) {
        setup(&h, chip, true, true);
        text = bes_motion_raw_describe(&h);
        CHECK(!strcmp(text, "auto")); g_free(text);
        CHECK(bes_motion_raw_set(&h, raw_a)); sample(&h);
        text = bes_motion_raw_describe(&h);
        CHECK(!strcmp(text, raw_a)); g_free(text);
        int16_t output[3]; read_group(&h, 0, output); read_group(&h, 1, output);
        if (chip == BES_IMU_LSM6DSO) {
            for (unsigned i=0; i<14; i++) (void)bes_imu_read(&h, LSM_FIFO_TAG+(i%7));
        } else {
            icm_bytes(&h, 16);
        }
        text = bes_sensor_observation(&h);
        printf("OBSERVATION_JSON %s\n", text); g_free(text);
    }
}

static void board_validation_contract(void)
{
    BesSensorHub h;
    setup(&h, BES_IMU_LSM6DSO, true, false);
    CHECK(bes_motion_raw_set(&h, raw_a)); sample(&h);
    const char *bad[] = {
        "imu:lsm6dso:spi0::71", "imu:lsm6dso:spi0:53:",
        "imu:lsm6dso:spi0: 53:71", "imu:lsm6dso:spi0:53: 71",
        "imu:lsm6dso:spi0:99999999999999999999999999:71",
        "imu:lsm6dso:spi0:53:99999999999999999999999999",
        "imu:lsm6dso:spi0:-1:71", "imu:lsm6dso:spi0:96:71",
        "imu:lsm6dso:spi0:53:-2", "imu:lsm6dso:spi0:53:96",
        "imu:lsm6dso:i2c:106:71", "imu:lsm6dso:i2c 1:106:71",
        "imu:lsm6dso:i2c999999999999999999999999:106:71",
        "imu:lsm6dso:i2c8:106:71", "imu:lsm6dso:i2c0:128:71",
        "imu:lsm6dso:i2c0:0:71", "imu:lsm6dso:i2c0::71",
        "imu:lsm6dso:spi0:53:71;imu:icm42607:spi1:13:10",
        "ppg:hx3695:spi0:50:14;ppg:hx3697:spi1:61:0",
        "imu:lsm6dso:spi0:53:71;ppg:hx3695:spi0:53:14",
        "imu:lsm6dso:i2c0:106:71;ppg:hx3697:i2c0:106:14",
    };
    BesSensorHub original = h;
    for (unsigned i=0; i<G_N_ELEMENTS(bad); i++) {
        char *error = NULL;
        CHECK(!bes_sensors_configure(&h, bad[i], &error));
        CHECK(!memcmp(&h, &original, sizeof(h)));
        g_free(error);
    }
    char *error = NULL;
    CHECK(bes_sensors_configure(&h, "imu:lsm6dso:spi0:53:71;ppg:hx3695:spi1:53:14", &error));
    CHECK(h.imu.kind == BES_IMU_LSM6DSO && h.ppg.kind == BES_PPG_HX3695);
    g_free(error); error = NULL;
    CHECK(bes_sensors_configure(&h, "imu:icm42607:i2c0:106:-1;ppg:hx3697:i2c1:106:-1", &error));
    CHECK(h.imu.kind == BES_IMU_ICM42607 && h.ppg.kind == BES_PPG_HX3697);
    g_free(error);
}

int main(void)
{
    input_contract();
    direct_contract(BES_IMU_LSM6DSO); direct_contract(BES_IMU_ICM42607);
    lsm_fifo_contract(); icm_fifo_contract();
    reset_and_auto_contract(BES_IMU_LSM6DSO); reset_and_auto_contract(BES_IMU_ICM42607);
    disable_after_conversion(BES_IMU_LSM6DSO); disable_after_conversion(BES_IMU_ICM42607);
    describe_contract();
    board_validation_contract();
    printf("motion raw sensor-bus contract: %u checks passed; no guest consumption asserted\n", checks);
    return 0;
}
