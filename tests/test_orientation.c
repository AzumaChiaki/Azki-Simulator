/* SPDX-License-Identifier: GPL-2.0-or-later
 * Test the production sample generators and actual chip register outputs. */
#include <glib.h>
#include <assert.h>
#include "../engine/qemu-overlay/hw/arm/best1503-sensors.h"

static int64_t clock_ns;
static int64_t now(void *opaque) { return clock_ns; }
static void schedule(void *opaque, int64_t when) { (void)opaque; (void)when; }
static void pin(void *opaque, int index, bool level) { (void)opaque; (void)index; (void)level; }
static void log_message(void *opaque, const char *message) { (void)opaque; (void)message; }

static void near(double actual, double expected, double tolerance)
{
    if (fabs(actual - expected) > tolerance) {
        fprintf(stderr, "actual %.8f expected %.8f tolerance %.8f\n", actual, expected, tolerance);
        abort();
    }
}

static void sample(BesSensorHub *h)
{
    /* Initialize schedule, then generate multiple real ODR samples. */
    bes_imu_update(h);
    clock_ns += 100000000;
    bes_imu_update(h);
}

int main(void)
{
    BesSensorHub h = {0};
    assert(bes_orientation_set(&h, "roll=45;pitch=-30"));
    const char *invalid[] = {"", "off", "roll=181;pitch=0", "roll=0;pitch=-181",
        "roll=nan;pitch=0", "roll=0;pitch=inf", "roll=0;pitch=0;extra=1",
        "roll=10", "roll=0;pitch=0\n", "roll=0;pitch=0x", "roll=1e999;pitch=0"};
    for (unsigned i = 0; i < G_N_ELEMENTS(invalid); i++) {
        assert(!bes_orientation_set(&h, invalid[i]));
        assert(h.orientation_enabled && h.roll_degrees == 45 && h.pitch_degrees == -30);
    }
    /* Six independent body axes, plus an oblique unit vector. */
    const double cases[][5] = {{0,0,0,0,1}, {0,90,-1,0,0}, {0,-90,1,0,0},
        {90,0,0,1,0}, {-90,0,0,-1,0}, {180,0,0,0,-1}, {45,30,-.5,sqrt(3.0/8),sqrt(3.0/8)}};
    for (unsigned c = 0; c < G_N_ELEMENTS(cases); c++) {
        char pose[80];
        snprintf(pose, sizeof(pose), "roll=%.17g;pitch=%.17g", cases[c][0], cases[c][1]);
        assert(bes_orientation_set(&h, pose));
        double a[3]; bes_orientation_gravity(&h, a);
        for (int i = 0; i < 3; i++) near(a[i], cases[c][i+2], 1e-12);
        near(a[0]*a[0]+a[1]*a[1]+a[2]*a[2], 1, 1e-12);
    }
    /* Both IMUs, including the N67/P67 +y+x-z physical installation. */
    for (int chip = BES_IMU_LSM6DSO; chip <= BES_IMU_ICM42607; chip++) {
        memset(&h, 0, sizeof(h)); clock_ns = 1000000;
        h.host = (BesSensorHost){.now_ns=now, .schedule=schedule, .set_input=pin, .log=log_message};
        h.imu.kind = chip; h.imu.port.irq_pin = -1;
        bes_sensors_reset(&h);
        double scale;
        if (chip == BES_IMU_LSM6DSO) {
            h.imu.regs[LSM_CTRL1_XL] = 0x30; /* 52 Hz, +/- 2g */
            h.imu.regs[LSM_FIFO_CTRL3] = 3;
            scale = 1000.0 / .061;
        } else {
            assert(bes_parse_mount("+y+x-z", h.imu.mount));
            h.imu.regs[ICM_PWR_MGMT0] = 3;
            h.imu.regs[ICM_ACCEL_CONFIG0] = 0x6a; /* 50 Hz, +/- 2g */
            scale = 16384;
        }
        for (unsigned c = 0; c < G_N_ELEMENTS(cases); c++) {
            char pose[80];
            snprintf(pose, sizeof(pose), "roll=%.17g;pitch=%.17g", cases[c][0], cases[c][1]);
            assert(bes_orientation_set(&h, pose)); sample(&h);
            double expected[3] = {cases[c][2], cases[c][3], cases[c][4]};
            if (chip == BES_IMU_ICM42607) {
                double x=expected[0]; expected[0]=expected[1]; expected[1]=x; expected[2]=-expected[2];
            }
            for (int i=0; i<3; i++) near(h.imu.accel_raw[i]/scale, expected[i], .011);
            assert(h.imu.samples > 0 && h.imu.xl_ready);
        }
    }
    /* Null/auto reset reproduces the old default bit-for-bit, including noise. */
    BesSensorHub original = {0}; original.noise = 0x1503;
    h = original; assert(bes_orientation_set(&h, "roll=90;pitch=0"));
    assert(bes_orientation_set(&h, "auto"));
    double a[3], g[3], expected_a[3], expected_g[3];
    bes_motion_sample(&h, .02, a, g); bes_motion_sample(&original, .02, expected_a, expected_g);
    assert(!memcmp(a, expected_a, sizeof(a)) && !memcmp(g, expected_g, sizeof(g)));
    h.health_enabled = true; h.cadence = 110; h.walk_target = 100; h.step_rate = 1;
    assert(bes_orientation_set(&h, "roll=0;pitch=0")); bes_motion_sample(&h, .02, a, g);
    assert(h.walk_phase == 0);
    assert(bes_orientation_set(&h, "auto")); bes_motion_sample(&h, .02, a, g);
    assert(h.walk_phase > 0);
    puts("orientation production IMU regression passed");
    return 0;
}
