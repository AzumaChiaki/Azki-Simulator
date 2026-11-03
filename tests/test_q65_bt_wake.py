"""Device wake completion must honor virtual deadlines under delayed dispatch."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>
#include <stdio.h>
typedef uint64_t hwaddr;
typedef int MemTxAttrs;
typedef int MemTxResult;
#define MEMTX_OK 0
#define MEMTX_ERROR 1
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
typedef struct Best1503State Best1503State;
typedef struct { uint32_t regs[128]; bool aon; hwaddr base; Best1503State *machine; } BesCmu;
typedef struct { bool pending; uint64_t deadline; unsigned deletes; } QEMUTimer;
struct Best1503State {
    bool bt_peer, bt_tx_done, bt_tx_capture;
    BesCmu cmu[2], bt_config[2]; QEMUTimer *bt_wake_timer, *bt_tx_timer;
};
static uint64_t now;
#define QEMU_CLOCK_VIRTUAL 0
#define LOG_GUEST_ERROR 1
#define qemu_log_mask(...) ((void)0)
static const char *g_getenv(const char *key) { (void)key; return NULL; }
static uint64_t qemu_clock_get_ns(int clock) { (void)clock; return now; }
static bool timer_pending(QEMUTimer *timer) { return timer->pending; }
static uint64_t timer_expire_time_ns(QEMUTimer *timer) { return timer->deadline; }
static void timer_del(QEMUTimer *timer) { timer->pending = false; timer->deletes++; }
static void timer_mod(QEMUTimer *timer, uint64_t deadline) {
    timer->pending = true; timer->deadline = deadline;
}
static void sensor_mailbox_write(Best1503State *s, int side, bool set, uint32_t bits) {
    (void)s; (void)side; (void)set; (void)bits;
}
static void sensor_set_reset(Best1503State *s, bool reset) { (void)s; (void)reset; }
static void bt_tx_update_irq(Best1503State *s) { (void)s; }
static void bt_rx_update_irq(Best1503State *s) { (void)s; }
'''
MAIN = r'''
int main(void) {
    QEMUTimer timer = {true, 100000, 0};
    Best1503State s = {.bt_peer = true, .bt_wake_timer = &timer};
    s.cmu[0].machine = s.cmu[1].machine = &s;
    s.cmu[1].aon = true;
    s.cmu[1].regs[0xcc/4] = 0x48;
    s.cmu[1].regs[0x04/4] = 1 << 8;
    s.cmu[0].regs[0xa0/4] = 0x14008;
    now = 99999; bt_peer_complete_due_wake(&s);
    assert(timer.pending && !timer.deletes && s.cmu[0].regs[0xa0/4] == 0x14008);
    now = 100000; bt_peer_complete_due_wake(&s);
    assert(!timer.pending && timer.deletes == 1);
    assert(s.cmu[0].regs[0xa0/4] == 0x14000 && s.cmu[0].regs[0xa4/4] == 0x14000);
    bt_peer_complete_due_wake(&s); assert(timer.deletes == 1);
    /* A late host callback must not expose stale hardware to the AP. */
    timer.pending = true; timer.deadline = 200000;
    s.cmu[0].regs[0xa0/4] |= 8;
    now = 20000000; bt_peer_complete_due_wake(&s);
    assert(!(s.cmu[0].regs[0xa0/4] & 8) && timer.deletes == 2);
    /* Every power prerequisite is real; expiry alone cannot grant an ACK. */
    for (unsigned condition = 0; condition < 3; condition++) {
        s.bt_peer = condition != 0;
        s.cmu[1].regs[0xcc/4] = condition == 1 ? 0x40 : 0x48;
        s.cmu[1].regs[0x04/4] = condition == 2 ? 0 : 1 << 8;
        s.cmu[0].regs[0xa0/4] = 0x14008;
        timer.pending = true;
        bt_peer_complete_due_wake(&s);
        assert(!timer.pending && s.cmu[0].regs[0xa0/4] == 0x14008);
    }
    /* A cancelled request/reset never resurrects a pending event. */
    s.bt_peer = true; s.cmu[1].regs[0x04/4] = 1 << 8;
    s.cmu[0].regs[0xa0/4] = 0x14000;
    timer.pending = true; bt_peer_complete_due_wake(&s);
    assert(s.cmu[0].regs[0xa0/4] == 0x14000);
    unsigned deleted = timer.deletes;
    timer.pending = false; s.cmu[0].regs[0xa0/4] = 0;
    bt_peer_complete_due_wake(&s);
    assert(timer.deletes == deleted && s.cmu[0].regs[0xa0/4] == 0);
    /* SET, CLR, SET represent distinct requests, never the old deadline. */
    now = 0;
    assert(cmu_write(&s.cmu[0], 0xa0, 8, 4, 0) == MEMTX_OK);
    assert(timer.pending && timer.deadline == 100000);
    now = 50000; assert(cmu_write(&s.cmu[0], 0xa4, 8, 4, 0) == MEMTX_OK);
    assert(!timer.pending);
    now = 80000; assert(cmu_write(&s.cmu[0], 0xa0, 8, 4, 0) == MEMTX_OK);
    assert(timer.pending && timer.deadline == 180000);
    now = 90000; assert(cmu_write(&s.cmu[0], 0xa0, 8, 4, 0) == MEMTX_OK);
    assert(timer.deadline == 180000); /* W1S does not extend an active request. */
    now = 100000; bt_peer_complete_due_wake(&s);
    assert(timer.pending && s.cmu[0].regs[0xa0/4] == 8);
    now = 180000; bt_peer_complete_due_wake(&s);
    assert(!timer.pending && s.cmu[0].regs[0xa0/4] == 0);
    /* CMU writes first synchronize expiry using the OLD power state. */
    now = 0;
    assert(cmu_write(&s.cmu[1], 0x08, 1 << 8, 4, 0) == MEMTX_OK);
    assert(cmu_write(&s.cmu[0], 0xa0, 8, 4, 0) == MEMTX_OK);
    now = 150000;
    assert(cmu_write(&s.cmu[1], 0x04, 1 << 8, 4, 0) == MEMTX_OK);
    now = 160000; bt_peer_complete_due_wake(&s);
    assert(!timer.pending && s.cmu[0].regs[0xa0/4] == 8);
    assert(cmu_write(&s.cmu[0], 0xa4, 8, 4, 0) == MEMTX_OK);
    now = 200000; assert(cmu_write(&s.cmu[0], 0xa0, 8, 4, 0) == MEMTX_OK);
    now = 350000; assert(cmu_write(&s.cmu[1], 0x08, 1 << 8, 4, 0) == MEMTX_OK);
    assert(!timer.pending && s.cmu[0].regs[0xa0/4] == 0);
    puts("Q65 wake: deadline, delayed dispatch, power gating and cancellation passed");
}
'''


class Q65WakeTest(unittest.TestCase):
    def test_device_deadline_without_early_or_unpowered_ack(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler required")
        source = (ROOT / "engine/qemu-overlay/hw/arm/azki-q65.c").read_text()
        functions = []
        for name in ("bt_peer_powered", "bt_peer_wake", "bt_peer_complete_due_wake",
                     "bt_peer_request_changed", "cmu_reg_supported", "cmu_write"):
            match = re.search(r"^static (?:bool|void|MemTxResult) " + name + r"\([^;{}]*\)\n\{.*?^\}\n",
                              source, re.M | re.S)
            self.assertIsNotNone(match, name)
            functions.append(match.group())
        (ROOT / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="q65-wake-", dir=ROOT / "build") as temp:
            source_file = Path(temp) / "wake.c"
            source_file.write_text(PRELUDE + "\n".join(functions) + MAIN)
            binary = Path(temp) / "wake"
            subprocess.run([compiler, "-O1", "-g", "-fsanitize=address,undefined",
                            str(source_file), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("deadline, delayed dispatch, power gating and cancellation passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
