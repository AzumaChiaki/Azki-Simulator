/* SPDX-License-Identifier: GPL-2.0-or-later
 * Real production touch parser/gesture functions with host IRQ/timer stubs. */
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void QEMUPutMouseEntry;
#include "touch-state.inc"
typedef struct Best1503State {
    BesTouch touch;
    struct { unsigned frame_width, frame_height; } lcdc;
    struct { uint32_t input, pending; } gpio[4];
    unsigned touch_irq_pin, irq_updates;
    void *touch_irq_timer;
} Best1503State;

#define MOUSE_EVENT_LBUTTON 1
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define LOG_GUEST_ERROR 0
#define qemu_log_mask(...) ((void)0)
static bool touch_powered(Best1503State *s) { return s->touch.enabled; }
static void touch_irq(Best1503State *s) { s->irq_updates++; }
static void timer_del(void *timer) { (void)timer; }
#include "touch-functions.inc"

static void command(Best1503State *s, uint16_t reg)
{
    s->touch.tx[0] = reg >> 8; s->touch.tx[1] = reg;
    s->touch.tx_count = 2;
    assert(touch_write_packet(s));
}

static void tap(Best1503State *s)
{
    touch_mouse(s, 16000, 18000, 0, MOUSE_EVENT_LBUTTON);
    touch_mouse(s, 16000, 18000, 0, 0);
}

int main(void)
{
    Best1503State s = {0};
    s.touch.enabled = true;
    s.lcdc.frame_width = 212; s.lcdc.frame_height = 520;
    touch_reset(&s);
    command(&s, 0xd109);
    assert(s.touch.mode == 9 && s.touch.sleep_mode == 0);
    tap(&s);
    assert(s.touch.queue_count == 2 && s.touch.report[5] == 1);

    /* Actual firmware: sleep, verify reply, return to NORMAL report mode. */
    command(&s, 0xd104);
    assert(s.touch.queue_count == 0 && touch_gesture_mode(&s.touch));
    command(&s, 0xd11e); command(&s, 0xd11e); command(&s, 0x0002);
    command(&s, 0xd109);
    assert(s.touch.mode == 9 && s.touch.sleep_mode == 3);
    assert(s.touch.reply_mode == 9 && touch_reporting(&s.touch));
    tap(&s);
    assert(s.touch.queue_count == 1 && s.touch.report[4] == 0x10 && s.touch.report[5] == 0);

    command(&s, 0xd104); command(&s, 0xd109);
    touch_mouse(&s, 16000, 28000, 0, MOUSE_EVENT_LBUTTON);
    touch_mouse(&s, 16000, 4000, 0, 0);
    assert(s.touch.queue_count == 1 && s.touch.report[4] == 0x40);

    command(&s, 0xd104); command(&s, 0xd109);
    command(&s, 0xd11d);
    assert(s.touch.sleep_mode == 0 && !touch_gesture_mode(&s.touch));
    tap(&s);
    assert(s.touch.queue_count == 2 && s.touch.report[5] == 1);

    for (unsigned sleep = 1; sleep <= 2; sleep++) {
        command(&s, sleep == 1 ? 0xd105 : 0xd107);
        command(&s, 0xd109);
        assert(s.touch.sleep_mode == sleep && !touch_reporting(&s.touch));
        tap(&s); assert(s.touch.queue_count == 0);
        command(&s, 0xd11d); assert(touch_reporting(&s.touch));
    }
    command(&s, 0xd104); command(&s, 0xd109);
    s.touch.bootloader = true;
    assert(!touch_gesture_mode(&s.touch) && !touch_reporting(&s.touch));
    tap(&s); assert(s.touch.queue_count == 0);
    /* Existing soft bootloader exit and hardware reset still clear sleep. */
    s.touch.tx[0] = 0xa0; s.touch.tx[1] = 4; s.touch.tx[2] = 0xe8;
    s.touch.tx_count = 3; assert(touch_write_packet(&s));
    assert(!s.touch.bootloader && s.touch.sleep_mode == 0 && touch_reporting(&s.touch));
    command(&s, 0xd104); command(&s, 0xd109); touch_reset(&s);
    assert(s.touch.sleep_mode == 0 && s.touch.mode == 9 && !s.touch.gesture_down);
    puts("touch sleep production regression passed");
    return 0;
}
