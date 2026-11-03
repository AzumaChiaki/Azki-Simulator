/* SPDX-License-Identifier: GPL-2.0-or-later
 * Q65 production decoder: transparent NONE replacement and PAD sampling.
 *
 * NXP VGLite/vg_lite_path.c independently encodes PATTERN_PAD as 0x1000,
 * FILTER_POINT as 0, and FILTER_BI_LINEAR as 0x20000. PAD extends edge
 * texels regardless of filter. NONE replaces the covered destination;
 * SRC_OVER leaves it unchanged for a transparent source.
 *
 * These vectors deliberately do not settle Q65's half-texel convention:
 * every selected in-range footprint is within a constant-color region,
 * and border footprints have the same expected color after clamping.
 * Opaque/zero-alpha colors and translucent black also avoid assuming how
 * the firmware's premultiplication control bits describe colored texels.
 */
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned lduw_le_p(const void *v)
{
    const uint8_t *p = v;
    return p[0] | p[1] << 8;
}

static uint32_t ldl_le_p(const void *v)
{
    const uint8_t *p = v;
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static void stw_le_p(void *v, unsigned n)
{
    uint8_t *p = v;
    p[0] = n;
    p[1] = n >> 8;
}

static void stl_le_p(void *v, uint32_t n)
{
    uint8_t *p = v;
    p[0] = n;
    p[1] = n >> 8;
    p[2] = n >> 16;
    p[3] = n >> 24;
}

#include "../engine/qemu-overlay/hw/arm/azki-q65-gpu.h"

#define RAM_BASE 0x20000000u
#define SOURCE_OFFSET 256
#define SOURCE_WIDTH 8
#define SOURCE_HEIGHT 8

static uint8_t ram[1024];
static unsigned checks;
static unsigned failures;

static bool read_ram(uint32_t address, void *out, unsigned bytes)
{
    if (address < RAM_BASE ||
        (uint64_t)address + bytes > (uint64_t)RAM_BASE + sizeof(ram)) {
        return false;
    }
    memcpy(out, ram + (address - RAM_BASE), bytes);
    return true;
}

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void expect(bool condition, const char *name, const char *detail)
{
    checks++;
    if (!condition) {
        failures++;
        fprintf(stderr, "FAIL %s: %s\n", name, detail);
    }
}

static void fill_source(const uint8_t bgra[4])
{
    for (unsigned i = 0; i < SOURCE_WIDTH * SOURCE_HEIGHT; i++) {
        memcpy(ram + SOURCE_OFFSET + i * 4, bgra, 4);
    }
}

static void draw_vector(const char *name, uint32_t mode, uint32_t sampler,
                        float u, float v, const uint8_t expected[4])
{
    uint32_t regs[BES_GPU_REGS] = {0};
    uint8_t command[24] = {0};
    uint8_t before[sizeof(ram)];
    memcpy(before, ram, sizeof(ram));
    regs[0xa00] = mode;
    regs[0xa10] = 0x103;
    regs[0xa11] = RAM_BASE;
    regs[0xa12] = 4;
    regs[0xa13] = 0x00010001;
    regs[0xa25] = sampler;
    regs[0xa29] = RAM_BASE + SOURCE_OFFSET;
    regs[0xa2b] = SOURCE_WIDTH * 4;
    regs[0xa2f] = (SOURCE_HEIGHT << 16) | SOURCE_WIDTH;
    regs[0xa18] = float_bits(u);
    regs[0xa19] = float_bits(v);
    regs[0xa1a] = float_bits(1);
    regs[0xa1f] = 1;
    stl_le_p(command, 0x40000001);
    stw_le_p(command + 12, 1);
    stw_le_p(command + 14, 1);
    const char *error = NULL;
    BesGpuWork *work = gpu_sw_prepare(regs, command, sizeof(command),
                                      read_ram, &error);
    expect(work != NULL, name, error ? error : "decoder rejected vector");
    expect(memcmp(ram, before, sizeof(ram)) == 0, name,
           "preparation mutated guest RAM");
    if (!work) {
        return;
    }
    expect(work->target_count == 1, name, "unexpected target count");
    if (work->target_count == 1) {
        const uint8_t *got = work->targets[0].pixels;
        char message[160];
        snprintf(message, sizeof(message),
                 "BGRA actual=%u,%u,%u,%u expected=%u,%u,%u,%u",
                 got[0], got[1], got[2], got[3],
                 expected[0], expected[1], expected[2], expected[3]);
        expect(memcmp(got, expected, 4) == 0, name, message);
    }
    gpu_sw_free(work);
}

static void transparent_none(void)
{
    const uint8_t zero[4] = {0, 0, 0, 0};
    for (unsigned filter = 0; filter < 2; filter++) {
        memset(ram, 0, sizeof(ram));
        memcpy(ram, (const uint8_t[]){13, 27, 49, 200}, 4);
        fill_source(zero);
        draw_vector(filter ? "NONE transparent BILINEAR" : "NONE transparent POINT",
                    0x10001081, 0x01000107 | (filter ? 0x20000 : 0),
                    4.25f, 4.25f, zero);
    }
}

static const uint8_t pad_palette[3][3][4] = {
    {{16, 32, 48, 255}, {64, 80, 96, 255}, {112, 128, 144, 255}},
    {{24, 40, 56, 255}, {72, 88, 104, 255}, {120, 136, 152, 255}},
    {{32, 48, 64, 255}, {80, 96, 112, 255}, {128, 144, 160, 255}}
};

static unsigned region(unsigned coordinate)
{
    return coordinate < 2 ? 0 : coordinate < 6 ? 1 : 2;
}

static void pad_vectors(void)
{
    static const struct {
        const char *name;
        float u, v;
        unsigned col, row;
    } samples[] = {
        {"interior", 4.25f, 4.25f, 1, 1},
        {"left near edge", 0.125f, 4.25f, 0, 1},
        {"right near edge", 7.875f, 4.25f, 2, 1},
        {"top near edge", 4.25f, 0.125f, 1, 0},
        {"bottom near edge", 4.25f, 7.875f, 1, 2},
        {"left outside", -4.0f, 4.25f, 0, 1},
        {"right outside", 20.0f, 4.25f, 2, 1},
        {"top outside", 4.25f, -4.0f, 1, 0},
        {"bottom outside", 4.25f, 20.0f, 1, 2},
        {"top-left outside", -4.0f, -4.0f, 0, 0},
        {"top-right outside", 20.0f, -4.0f, 2, 0},
        {"bottom-left outside", -4.0f, 20.0f, 0, 2},
        {"bottom-right outside", 20.0f, 20.0f, 2, 2},
    };
    for (unsigned filter = 0; filter < 2; filter++) {
        for (unsigned i = 0; i < G_N_ELEMENTS(samples); i++) {
            memset(ram, 0, sizeof(ram));
            memcpy(ram, (const uint8_t[]){3, 5, 7, 255}, 4);
            for (unsigned y = 0; y < SOURCE_HEIGHT; y++) {
                for (unsigned x = 0; x < SOURCE_WIDTH; x++) {
                    memcpy(ram + SOURCE_OFFSET + (y * SOURCE_WIDTH + x) * 4,
                           pad_palette[region(y)][region(x)], 4);
                }
            }
            char name[100];
            snprintf(name, sizeof(name), "%s PAD %s",
                     filter ? "BILINEAR" : "POINT", samples[i].name);
            draw_vector(name, 0x10001181,
                        0x01001107 | (filter ? 0x20000 : 0),
                        samples[i].u, samples[i].v,
                        pad_palette[samples[i].row][samples[i].col]);
        }
    }
}

static void source_over_vectors(void)
{
    static const struct {
        const char *name;
        uint8_t source[4], dest[4], expected[4];
    } vectors[] = {
        {"transparent preserves", {0, 0, 0, 0},
            {64, 96, 128, 255}, {64, 96, 128, 255}},
        {"opaque replaces", {16, 40, 80, 255},
            {64, 96, 128, 255}, {16, 40, 80, 255}},
        {"half black over opaque", {0, 0, 0, 128},
            {64, 96, 128, 255}, {32, 48, 64, 255}},
        {"half black over translucent", {0, 0, 0, 128},
            {32, 48, 64, 128}, {16, 24, 32, 192}},
    };
    for (unsigned filter = 0; filter < 2; filter++) {
        for (unsigned i = 0; i < G_N_ELEMENTS(vectors); i++) {
            memset(ram, 0, sizeof(ram));
            memcpy(ram, vectors[i].dest, 4);
            fill_source(vectors[i].source);
            char name[100];
            snprintf(name, sizeof(name), "%s SRC_OVER %s",
                     filter ? "BILINEAR" : "POINT", vectors[i].name);
            draw_vector(name, 0x10001181,
                        0x01000107 | (filter ? 0x20000 : 0),
                        4.25f, 4.25f, vectors[i].expected);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "expected group: none, pad, or over\n");
        return 2;
    }
    if (!strcmp(argv[1], "none")) {
        transparent_none();
    } else if (!strcmp(argv[1], "pad")) {
        pad_vectors();
    } else if (!strcmp(argv[1], "over")) {
        source_over_vectors();
    } else {
        return 2;
    }
    printf("Q65 %s: %u checks, %u failures\n", argv[1], checks, failures);
    return failures ? 1 : 0;
}
