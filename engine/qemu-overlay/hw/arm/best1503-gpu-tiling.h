/* SPDX-License-Identifier: GPL-2.0-or-later
 * VGLite IMAGE_CONFIG[13:12] border addressing, independent of filtering.
 * Keep each renderer's coordinate and pixel-centre conventions at the caller.
 */
#ifndef BEST1503_GPU_TILING_H
#define BEST1503_GPU_TILING_H
#include <limits.h>
#include <math.h>
#include <stdbool.h>

static double gpu_sw_tile_coordinate(double value, unsigned extent, unsigned tile)
{
    if (tile >= 2 && tile <= 3 && extent && isfinite(value)) {
        double period = (double)extent * (tile == 3 ? 2 : 1);
        value = fmod(value, period);
        if (value < 0) {
            value += period;
        }
    }
    return value;
}

static bool gpu_sw_tile_index(int *index, unsigned extent, unsigned tile)
{
    if (!extent || extent > INT_MAX / 2 || tile > 3) {
        return false;
    }
    int size = extent;
    if (tile == 0) {
        return *index >= 0 && *index < size;
    }
    if (tile == 1) {
        *index = *index < 0 ? 0 : *index >= size ? size - 1 : *index;
        return true;
    }
    int period = tile == 3 ? size * 2 : size;
    int mapped = *index % period;
    if (mapped < 0) {
        mapped += period;
    }
    *index = mapped < size ? mapped : period - 1 - mapped;
    return true;
}
#endif
