/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../engine/qemu-overlay/hw/arm/azki-q65-display.h"

int main(void)
{
    /* Every intensity survives the native BGR888 path, including the dark
     * values that RGB565 previously rounded down in blurred backgrounds. */
    uint8_t bgr[256 * 3], rgb[256 * 3];
    for (unsigned i = 0; i < 256; i++) {
        bgr[3*i] = i; bgr[3*i+1] = 255-i; bgr[3*i+2] = i ^ 0x55;
    }
    q65_lcd_unpack_rgb(bgr, rgb, 256, 3);
    for (unsigned i = 0; i < 256; i++) {
        assert(rgb[3*i] == (i ^ 0x55));
        assert(rgb[3*i+1] == 255-i);
        assert(rgb[3*i+2] == i);
    }
    /* All 65536 legacy 565 colors retain their original channel values. */
    for (unsigned i = 0; i < 65536; i++) {
        uint8_t source[] = {i & 255, i >> 8};
        uint8_t dest[5] = {0xcc, 0, 0, 0, 0xdd};
        q65_lcd_unpack_rgb(source, dest+1, 1, 2);
        assert(dest[0] == 0xcc && dest[4] == 0xdd);
        assert(dest[1] == (i >> 11) * 8);
        assert(dest[2] == ((i >> 5) & 63) * 4);
        assert(dest[3] == (i & 31) * 8);
    }
    uint8_t sentinel = 0x72;
    q65_lcd_unpack_rgb(NULL, &sentinel, 0, 3);
    assert(sentinel == 0x72);
    puts("Q65 display: 256 native intensities and 65536 legacy colors preserved");
    return 0;
}
