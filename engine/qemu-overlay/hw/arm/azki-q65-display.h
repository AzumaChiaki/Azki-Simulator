/* Preserve the Q65 panel's RGB888 input precision. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef AZKI_Q65_DISPLAY_H
#define AZKI_Q65_DISPLAY_H
#include <stdint.h>

/* The caller validates the source format, full pitched guest span and width.
 * RGB565 keeps the legacy left-aligned channel convention. The native
 * BGR888 framebuffer must not be quantized to RGB565 on its way to a viewer. */
static inline void q65_lcd_unpack_rgb(const uint8_t *source, uint8_t *dest,
                                      unsigned width, unsigned bytes)
{
    for (unsigned x = 0; x < width; x++) {
        const uint8_t *p = source + x * bytes;
        if (bytes == 2) {
            unsigned v = p[0] | ((unsigned)p[1] << 8);
            dest[x * 3] = (v >> 11) << 3;
            dest[x * 3 + 1] = ((v >> 5) & 63) << 2;
            dest[x * 3 + 2] = (v & 31) << 3;
        } else {
            dest[x * 3] = p[2];
            dest[x * 3 + 1] = p[1];
            dest[x * 3 + 2] = p[0];
        }
    }
}
#endif
