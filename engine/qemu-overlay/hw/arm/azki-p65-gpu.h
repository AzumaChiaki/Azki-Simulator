/* SPDX-License-Identifier: GPL-2.0-or-later
 * Bounded software subset of the public VGLite command stream. This is not
 * a Vivante hardware renderer: curves use 32 segments and coverage uses a
 * 4 x 4 sample grid. Only the Q66 startup operations listed below are accepted.
 * All writes and persistent state remain private until the caller commits.
 */
#ifndef BEST1503_GPU_H
#define BEST1503_GPU_H
#include <inttypes.h>
#include <math.h>

#define BES_GPU_REGS 0x1000
#define BES_GPU_TARGETS 4
/* P65's 48-contour activity graph needs more than 4096 tessellated edges.
 * Cap the edge/cross scratch arrays at 384 KiB total; the independent
 * 32-Mi edge-work and 8-Mi pixel-work budgets still bound each submission. */
#define BES_GPU_EDGES 8192

typedef bool (*BesGpuRead)(uint32_t, void *, unsigned);
typedef struct {
    uint32_t address, canonical;
    unsigned width, height, stride, bytes, format, bpp;
    uint8_t *pixels;
} BesGpuTarget;
typedef struct {
    uint32_t regs[BES_GPU_REGS], event;
    BesGpuTarget targets[BES_GPU_TARGETS];
    unsigned target_count, draws, pixels;
    uint64_t budget, edge_budget;
    unsigned command_bytes;
    const char *error;
} BesGpuWork;
typedef struct { double x0, y0, x1, y1; } BesGpuEdge;
typedef struct { double x; int winding; } BesGpuCross;
typedef struct { unsigned r, g, b, a; } BesGpuColor;

/* Keep channels explicit inside the renderer. VGLite's constant color A02
 * is ABGR, but its index-image CLUT entries are ARGB. They are not the same
 * register format, even when both render to the same framebuffer. */
static BesGpuColor gpu_sw_color_abgr(uint32_t value)
{
    return (BesGpuColor) {value & 255, (value >> 8) & 255,
                         (value >> 16) & 255, value >> 24};
}

static BesGpuColor gpu_sw_color_argb(uint32_t value)
{
    return (BesGpuColor) {(value >> 16) & 255, (value >> 8) & 255,
                         value & 255, value >> 24};
}

static BesGpuColor gpu_sw_color_bgra(const uint8_t *pixel)
{
    return (BesGpuColor) {pixel[2], pixel[1], pixel[0], pixel[3]};
}

static BesGpuColor gpu_sw_color_565(unsigned value)
{
    return (BesGpuColor) {((value >> 11) & 31) * 255 / 31,
                         ((value >> 5) & 63) * 255 / 63,
                         (value & 31) * 255 / 31, 255};
}

/* SDK BGRA5551: little-endian B[15:11], G[10:6], R[5:1], A[0].
 * P65 uses it for the canvas created by swiping up from the watchface. */
static BesGpuColor gpu_sw_color_5551(unsigned value)
{
    return (BesGpuColor) {((value >> 1) & 31) * 255 / 31,
                         ((value >> 6) & 31) * 255 / 31,
                         ((value >> 11) & 31) * 255 / 31,
                         (value & 1) ? 255 : 0};
}

static BesGpuColor gpu_sw_target_load(const BesGpuTarget *t,
                                     const uint8_t *pixel)
{
    if (t->format == 0x100) {
        return (BesGpuColor) {255, 255, 255, pixel[0]};
    }
    if (t->format == 0x109) {
        return (BesGpuColor) {pixel[2], pixel[1], pixel[0], 255};
    }
    if (t->format == 0x105) {
        return gpu_sw_color_5551(lduw_le_p(pixel));
    }
    if (t->bpp == 4) {
        return gpu_sw_color_bgra(pixel);
    }
    /* SDK BGR565 (0x101) has red in bits 15:11; RGB565 (0x121)
     * reverses red/blue. BGRA5658 (0x10a) prepends an alpha byte. */
    BesGpuColor color = gpu_sw_color_565(lduw_le_p(pixel + (t->bpp == 3)));
    if (t->format == 0x121) {
        unsigned red = color.r; color.r = color.b; color.b = red;
    }
    if (t->bpp == 3) {
        color.a = pixel[0];
    }
    return color;
}

static void gpu_sw_target_store(const BesGpuTarget *t, uint8_t *pixel,
                                 BesGpuColor color)
{
    if (t->format == 0x100) {
        pixel[0] = color.a;
        return;
    }
    if (t->format == 0x109) {
        pixel[0] = color.b; pixel[1] = color.g; pixel[2] = color.r;
        return;
    }
    if (t->format == 0x105) {
        stw_le_p(pixel, ((color.b >> 3) << 11) | ((color.g >> 3) << 6) |
                       ((color.r >> 3) << 1) | (color.a >> 7));
        return;
    }
    if (t->bpp == 4) {
        pixel[0] = color.b; pixel[1] = color.g; pixel[2] = color.r;
        pixel[3] = color.a;
        return;
    }
    if (t->format == 0x121) {
        unsigned red = color.r; color.r = color.b; color.b = red;
    }
    if (t->bpp == 3) {
        pixel[0] = color.a;
    }
    stw_le_p(pixel + (t->bpp == 3),
             ((color.r >> 3) << 11) | ((color.g >> 2) << 5) | (color.b >> 3));
}

static uint32_t gpu_sw_canonical(uint32_t address, unsigned length)
{
    static const uint32_t bases[] = {0x20000000, 0x00200000, 0x38000000,
                                    0x18000000, 0x3c000000, 0x1c000000};
    for (unsigned i = 0; i < G_N_ELEMENTS(bases); i++) {
        unsigned size = i < 2 ? 0x1a0000 : 0x2000000;
        if (address >= bases[i] &&
            (uint64_t)address + length <= (uint64_t)bases[i] + size) {
            return (i < 2 ? 0x20000000 : 0x38000000) + address - bases[i];
        }
    }
    return 0;
}

static bool gpu_sw_fail(BesGpuWork *w, const char *reason)
{
    w->error = reason;
    return false;
}

static void gpu_sw_free(BesGpuWork *w)
{
    if (w) {
        for (unsigned i = 0; i < w->target_count; i++) {
            g_free(w->targets[i].pixels);
        }
        g_free(w);
    }
}

static double gpu_sw_float(uint32_t value)
{
    float f;
    memcpy(&f, &value, sizeof(f));
    return f;
}

static bool gpu_sw_number(double v)
{
    return isfinite(v) && v >= -65536 && v <= 65536;
}

static bool gpu_sw_state(unsigned r)
{
    if (r >= 0xb00 && r <= 0xbff) {
        return true; /* 256-entry indexed image CLUT. */
    }
    switch (r) {
    case 0xa00: case 0xa02:
    case 0xa10: case 0xa11: case 0xa12: case 0xa13:
    case 0xa18: case 0xa19: case 0xa1a: case 0xa1b:
    case 0xa1c: case 0xa1d: case 0xa1e: case 0xa1f:
    case 0xa20: case 0xa21: case 0xa22:
    case 0xa25: case 0xa27: case 0xa29: case 0xa2b:
    case 0xa2d: case 0xa2f: case 0xa34: case 0xa35:
    case 0xa39: case 0xa3a: case 0xa3b: case 0xa3c: case 0xa3d:
    case 0xa40: case 0xa41: case 0xa42: case 0xa43: case 0xa44: case 0xa45:
    case 0xa90: case 0xa91: case 0xa92: case 0xa93:
    case 0xa94: case 0xa95: case 0xa96: case 0xa97:
    case 0xac8: case 0xacb: case 0xacc: case 0xacd: case 0xace:
    case 0xad1: case 0xe02: case 0xf00:
        return true;
    default:
        return false;
    }
}

static BesGpuTarget *gpu_sw_target(BesGpuWork *w, BesGpuRead read)
{
    uint32_t *r = w->regs;
    unsigned width = r[0xa13] & 0xffff, height = r[0xa13] >> 16;
    unsigned stride = r[0xa12];
    unsigned format = r[0xa10];
    /* SDK convert_target_format(A8)=0, plus the linear-target flag 0x100.
     * Q65 launcher glyph caches use this alpha-only byte layout. */
    unsigned bpp = format == 0x100 ? 1 : (format == 0x109 || format == 0x10a) ? 3 :
                   format == 0x103 ? 4 : 2;
    /* Public VGLite converts 24-bit byte stride to a 32-bit hardware stride. */
    if (bpp == 3) {
        if (stride & 3) { gpu_sw_fail(w, "unaligned packed 24-bit stride"); return NULL; }
        stride = stride / 4 * 3;
    }
    uint64_t length = (uint64_t)stride * height;
    uint32_t canonical = gpu_sw_canonical(r[0xa11], length);
    bool bad_format = format != 0x100 && format != 0x101 && format != 0x121 &&
                      format != 0x109 && format != 0x10a && format != 0x103 &&
                      format != 0x105;
    bool bad_size = !width || !height || width > 1024 || height > 1024;
    bool bad_stride = stride < width * bpp || stride > 4096 || (bpp == 2 && (stride & 1));
    bool bad_address = (r[0xa11] & 1) != 0;
    bool bad_length = length > 4 * 1024 * 1024;
    if (bad_format || bad_size || bad_stride || bad_address || bad_length || !canonical) {
        fprintf(stderr, "BES GPU target check: fmt=%08x badfmt=%d w=%u h=%u badsize=%d stride=%u bpp=%u badstride=%d addr=%08x badaddr=%d len=%llu badlen=%d canonical=%08x\n",
                format, bad_format, width, height, bad_size, stride, bpp, bad_stride,
                r[0xa11], bad_address, (unsigned long long)length, bad_length, canonical);
        gpu_sw_fail(w, "unsupported or unsafe linear target");
        return NULL;
    }
    for (unsigned i = 0; i < w->target_count; i++) {
        BesGpuTarget *t = &w->targets[i];
        if (canonical == t->canonical && stride == t->stride &&
            format == t->format) {
            /* Q65 changes A13 from a full framebuffer to a smaller view
             * inside the same command buffer.  Width/height limit drawing;
             * they do not describe a new allocation.  Keep one backing store
             * so earlier draws and later source reads see command order.
            * bytes retains the largest initialized span for final commit. */
            if (length > t->bytes) {
                for (unsigned j = 0; j < w->target_count; j++) {
                    BesGpuTarget *other = &w->targets[j];
                    if (j != i &&
                        (uint64_t)canonical < (uint64_t)other->canonical + other->bytes &&
                        (uint64_t)other->canonical < (uint64_t)canonical + length) {
                        gpu_sw_fail(w, "overlapping target descriptions");
                        return NULL;
                    }
                }
                uint8_t *pixels = g_malloc(length);
                memcpy(pixels, t->pixels, t->bytes);
                if (!read(r[0xa11] + t->bytes, pixels + t->bytes,
                          length - t->bytes)) {
                    g_free(pixels);
                    gpu_sw_fail(w, "target RAM extension read failed");
                    return NULL;
                }
                g_free(t->pixels);
                t->pixels = pixels;
                t->bytes = length;
            }
            t->width = width;
            t->height = height;
            return t;
        }
        if ((uint64_t)canonical < (uint64_t)t->canonical + t->bytes &&
            (uint64_t)t->canonical < (uint64_t)canonical + length) {
            gpu_sw_fail(w, "overlapping target descriptions");
            return NULL;
        }
    }
    if (w->target_count == BES_GPU_TARGETS) {
        gpu_sw_fail(w, "too many render targets");
        return NULL;
    }
    BesGpuTarget *t = &w->targets[w->target_count++];
    *t = (BesGpuTarget) { .address = r[0xa11], .canonical = canonical,
        .width = width, .height = height, .stride = stride, .bytes = length,
        .format = format, .bpp = bpp,
        .pixels = g_malloc(length) };
    if (!read(t->address, t->pixels, t->bytes)) {
        gpu_sw_fail(w, "target RAM read failed");
        return NULL;
    }
    return t;
}

static bool gpu_sw_edge(BesGpuWork *w, BesGpuEdge *edges, unsigned *count,
                        double x0, double y0, double x1, double y1)
{
    if (!gpu_sw_number(x0) || !gpu_sw_number(y0) ||
        !gpu_sw_number(x1) || !gpu_sw_number(y1)) {
        return gpu_sw_fail(w, "path coordinate limit");
    }
    /* Horizontal edges do not participate in scan conversion and consume no
     * slot.  Check the capacity only for an edge that will be retained. */
    if (y0 == y1) {
        return true;
    }
    if (*count >= BES_GPU_EDGES) {
        return gpu_sw_fail(w, "path edge limit");
    }
    edges[(*count)++] = (BesGpuEdge) {x0, y0, x1, y1};
    return true;
}

static bool gpu_sw_path(BesGpuWork *w, const uint8_t *data, unsigned bytes,
                        BesGpuEdge *edges, unsigned *count)
{
    unsigned format = (w->regs[0xa34] >> 20) & 3;
    unsigned unit = format == 1 ? 2 : 4;
    double m[6], x = 0, y = 0, startx = 0, starty = 0;
    bool open = false;
    if ((w->regs[0xa34] & ~0x00300013u) != 0x03000000 ||
        (format != 1 && format != 3)) {
        return gpu_sw_fail(w, "unsupported path configuration");
    }
    for (unsigned i = 0; i < 6; i++) {
        m[i] = gpu_sw_float(w->regs[0xa40 + i]);
        if (!gpu_sw_number(m[i])) {
            return gpu_sw_fail(w, "invalid path transform");
        }
    }
    x = m[2]; y = m[5];
    for (unsigned p = 0; p + unit <= bytes; ) {
        unsigned op = unit == 2 ? lduw_le_p(data + p) : ldl_le_p(data + p);
        p += unit;
        if (op == 0) {
            if (open && !gpu_sw_edge(w, edges, count, x, y, startx, starty)) {
                return false;
            }
            /* Uploaded glyph DATA is rounded to 64 bits. Bytes after path END
             * are padding, and the original font uploader does not zero them. */
            return true;
        }
        if (op == 1) {
            if (!open || !gpu_sw_edge(w, edges, count, x, y, startx, starty)) {
                return gpu_sw_fail(w, "invalid path close");
            }
            x = startx; y = starty; open = false;
            continue;
        }
        unsigned absolute_op = op & ~1u;
        unsigned coords = absolute_op == 2 || absolute_op == 4 ? 2 :
                          absolute_op == 6 ? 4 : absolute_op == 8 ? 6 : 0;
        if (!coords || coords * unit > bytes - p || (absolute_op != 2 && !open)) {
            return gpu_sw_fail(w, "unsupported or truncated path opcode");
        }
        double v[6];
        for (unsigned i = 0; i < coords; i += 2) {
            double a = format == 1 ? (int16_t)lduw_le_p(data + p) :
                gpu_sw_float(ldl_le_p(data + p));
            double b = format == 1 ? (int16_t)lduw_le_p(data + p + unit) :
                gpu_sw_float(ldl_le_p(data + p + unit));
            p += unit * 2;
            if (!gpu_sw_number(a) || !gpu_sw_number(b)) {
                return gpu_sw_fail(w, "invalid path coordinate");
            }
            v[i] = m[0] * a + m[1] * b + ((op & 1) ? x : m[2]);
            v[i + 1] = m[3] * a + m[4] * b + ((op & 1) ? y : m[5]);
            if (!gpu_sw_number(v[i]) || !gpu_sw_number(v[i + 1])) {
                return gpu_sw_fail(w, "invalid transformed path coordinate");
            }
        }
        if (absolute_op == 2) {
            if (open && !gpu_sw_edge(w, edges, count, x, y, startx, starty)) {
                return false;
            }
            startx = x = v[0]; starty = y = v[1]; open = true;
        } else if (absolute_op == 4) {
            if (!gpu_sw_edge(w, edges, count, x, y, v[0], v[1])) {
                return false;
            }
            x = v[0]; y = v[1];
        } else {
            double ox = x, oy = y;
            for (unsigned i = 1; i <= 32; i++) {
                double t = i / 32.0, u = 1 - t;
                double nx = absolute_op == 6 ? u*u*ox + 2*u*t*v[0] + t*t*v[2] :
                    u*u*u*ox + 3*u*u*t*v[0] + 3*u*t*t*v[2] + t*t*t*v[4];
                double ny = absolute_op == 6 ? u*u*oy + 2*u*t*v[1] + t*t*v[3] :
                    u*u*u*oy + 3*u*u*t*v[1] + 3*u*t*t*v[3] + t*t*t*v[5];
                if (!gpu_sw_edge(w, edges, count, x, y, nx, ny)) {
                    return false;
                }
                x = nx; y = ny;
            }
        }
    }
    return gpu_sw_fail(w, "missing path END");
}

static int gpu_sw_cross_compare(const void *a, const void *b)
{
    double x = ((const BesGpuCross *)a)->x, y = ((const BesGpuCross *)b)->x;
    return (x > y) - (x < y);
}

static uint8_t *gpu_sw_coverage(BesGpuWork *w, BesGpuTarget *t,
                               const uint8_t *data, unsigned bytes, bool path,
                               unsigned bounds[4])
{
    unsigned left, top, right, bottom;
    if (!path) {
        if (bytes != 8) {
            gpu_sw_fail(w, "invalid rectangle DATA");
            return NULL;
        }
        int x = (int16_t)lduw_le_p(data), y = (int16_t)lduw_le_p(data + 2);
        left = CLAMP(x, 0, (int)t->width);
        top = CLAMP(y, 0, (int)t->height);
        right = MAX(left, (unsigned)CLAMP(x + (int)lduw_le_p(data + 4), 0, (int)t->width));
        bottom = MAX(top, (unsigned)CLAMP(y + (int)lduw_le_p(data + 6), 0, (int)t->height));
    } else {
        left = MIN(t->width, w->regs[0xa39] & 0xffff);
        top = MIN(t->height, w->regs[0xa39] >> 16);
        right = MIN(t->width, left + (w->regs[0xa3a] & 0xffff));
        bottom = MIN(t->height, top + (w->regs[0xa3a] >> 16));
    }
    g_autofree BesGpuEdge *edges = NULL;
    g_autofree BesGpuCross *cross = NULL;
    unsigned count = 0;
    if (path) {
        edges = g_new(BesGpuEdge, BES_GPU_EDGES);
        cross = g_new(BesGpuCross, BES_GPU_EDGES);
        if (!gpu_sw_path(w, data, bytes, edges, &count)) {
            return NULL;
        }
        /* A path clip commonly covers a full card while the glyph occupies
         * a small region. Restrict scan conversion to transformed edge bounds
         * with one-pixel padding for the 4x4 sample footprint. */
        if (count) {
            double min_x = edges[0].x0, max_x = edges[0].x0;
            double min_y = edges[0].y0, max_y = edges[0].y0;
            for (unsigned i = 0; i < count; i++) {
                min_x = MIN(min_x, MIN(edges[i].x0, edges[i].x1));
                max_x = MAX(max_x, MAX(edges[i].x0, edges[i].x1));
                min_y = MIN(min_y, MIN(edges[i].y0, edges[i].y1));
                max_y = MAX(max_y, MAX(edges[i].y0, edges[i].y1));
            }
            int edge_left = (int)floor(min_x) - 1;
            int edge_top = (int)floor(min_y) - 1;
            int edge_right = (int)ceil(max_x) + 1;
            int edge_bottom = (int)ceil(max_y) + 1;
            left = MAX(left, (unsigned)CLAMP(edge_left, 0, (int)t->width));
            top = MAX(top, (unsigned)CLAMP(edge_top, 0, (int)t->height));
            right = MAX(left, MIN(right, (unsigned)CLAMP(edge_right, 0, (int)t->width)));
            bottom = MAX(top, MIN(bottom, (unsigned)CLAMP(edge_bottom, 0, (int)t->height)));
        }
    }
    bounds[0] = left; bounds[1] = top;
    bounds[2] = right; bounds[3] = bottom;
    unsigned width = right - left, height = bottom - top;
    /* Both allocation and pixel traversal use the clipped draw extent.
     * Billing a small glyph as a full screen rejected ordinary UI batches.
     * Retain the bounded amount of real pixel work per command buffer. */
    uint64_t area = (uint64_t)width * height;
    if ((w->budget += area) > 8 * 1024 * 1024) {
        gpu_sw_fail(w, "draw work limit");
        return NULL;
    }
    uint8_t *coverage = g_malloc0(MAX(area, 1));
    if (!path) {
        memset(coverage, 16, area);
        return coverage;
    }
    if ((w->edge_budget += (uint64_t)count * height * 4) > 32 * 1024 * 1024) {
        gpu_sw_fail(w, "path work limit");
        g_free(coverage);
        return NULL;
    }
    for (unsigned y = top; y < bottom; y++) {
        for (unsigned sy = 0; sy < 4; sy++) {
            double yy = y + (sy + 0.5) / 4;
            unsigned n = 0;
            for (unsigned i = 0; i < count; i++) {
                BesGpuEdge *e = &edges[i];
                if (yy >= MIN(e->y0, e->y1) && yy < MAX(e->y0, e->y1)) {
                    cross[n++] = (BesGpuCross) {
                        e->x0 + (yy - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0),
                        e->y1 > e->y0 ? 1 : -1 };
                }
            }
            qsort(cross, n, sizeof(*cross), gpu_sw_cross_compare);
            int winding = 0;
            for (unsigned i = 0; i + 1 < n; i++) {
                winding += cross[i].winding;
                bool inside = w->regs[0xa34] & 0x10 ? (winding & 1) : winding != 0;
                if (!inside || cross[i].x == cross[i + 1].x) {
                    continue;
                }
                /* For each of the four horizontal sample positions, solve
                 * x + (sx + .5)/4 in [x0,x1) directly. This is equivalent
                 * to the old per-pixel floating comparison, while avoiding
                 * four doubles and branches for every candidate pixel. */
                double x0 = cross[i].x, x1 = cross[i + 1].x;
                for (unsigned sx = 0; sx < 4; sx++) {
                    double offset = (sx + 0.5) / 4;
                    int begin = MAX((int)left, (int)ceil(x0 - offset));
                    int end = MIN((int)right, (int)ceil(x1 - offset));
                    for (int x = begin; x < end; x++) {
                        coverage[(y - top) * width + x - left]++;
                    }
                }
            }
        }
    }
    return coverage;
}

static bool gpu_sw_draw(BesGpuWork *w, const uint8_t *data, unsigned bytes,
                        BesGpuRead read)
{
    uint32_t *r = w->regs;
    bool multiply_mode = r[0xa00] == 0x10002181 || r[0xa00] == 0x10002102;
    bool clear_rectangle = r[0xa00] == 0x10000001 && bytes == 8;
    /* A single DATA unit is x/y/width/height even for solid clears.
     * Longer solid DATA carries a path and keeps its tessellation state. */
    /* vg_lite_clear emits 0x10000001 for a regular solid rectangle;
     * path fills use 0x10000102. Both carry the same x/y/w/h DATA. */
    bool solid_rectangle = (r[0xa00] == 0x10000001 ||
                            r[0xa00] == 0x10000102) && bytes == 8;
    bool path = !solid_rectangle && r[0xa00] != 0x2181 && r[0xa00] != 0x10001181 &&
                r[0xa00] != 0x10002181;
    bool image = !solid_rectangle && r[0xa00] != 0x10000102;
    bool color_mode = r[0xa00] == 0x10001102 || r[0xa00] == 0x10001181;
    bool indexed = r[0xa25] == 0x01000900 || r[0xa25] == 0x01020900;
    bool packed_source = r[0xa25] == 0x41020100 || r[0xa25] == 0x41000100;
    /* SDK convert_source_format(BGR888)=0x20000000. P65 composites
     * its packed BGR888 transition canvas using this source format.
     * Like 5658, its hardware stride is expressed as four bytes/pixel,
     * but memory contains three; unlike 5658 it has no alpha byte. */
    bool bgr888_source = r[0xa25] == 0x21000100 ||
                         r[0xa25] == 0x21010100 || r[0xa25] == 0x21020100;
    bool rgb565_source = r[0xa25] == 0x01000105 ||
                         r[0xa25] == 0x01010105 || r[0xa25] == 0x01020105;
    /* Target format 5 and source format 4 refer to the same BGRA5551
     * storage. Accept the bounded POINT/LINEAR/BI_LINEAR COLOR samplers. */
    bool bgra5551_source = r[0xa25] == 0x01000104 ||
                           r[0xa25] == 0x01010104 || r[0xa25] == 0x01020104;
    /* Original Q65 launcher glyphs use POINT A8 (01000102), while
     * startup glyphs use LINEAR A8 (01010102). Both share the A8 layout. */
    bool a8_source = r[0xa25] == 0x01000102 || r[0xa25] == 0x01010102 ||
                     r[0xa25] == 0x01020102;
    /* Public convert_source_format: BGRA8888 = 7. Activity pages composite
     * their offscreen BGRA8888 canvas back into the RGB565 display target. */
    /* VGLite format 7 is BGRA8888.  The activity page uses the
     * 0x01011107 sampler variant in addition to the startup variants. */
    bool bgra8888_source = (r[0xa25] & 0xff) == 0x07 &&
                           (r[0xa25] >> 24) == 0x01 &&
                           ((r[0xa25] >> 16) & 0xff) <= 0x03 &&
                           (((r[0xa25] >> 8) & 0xff) == 0x01 ||
                            ((r[0xa25] >> 8) & 0xff) == 0x11 ||
                            ((r[0xa25] >> 8) & 0xff) == 0x21);
    /* The Rive paints in Q65's control center use LINEAR with PAD. Their
     * one-row BGRA ramps extend across the path in the other dimension. */
    bool linear_pad = r[0xa25] == 0x01011107;
    bool colored_source = indexed || packed_source || bgr888_source ||
                          rgb565_source || bgra8888_source || bgra5551_source;
    if ((r[0xa00] != 0x2181 && r[0xa00] != 0x2102 &&
         r[0xa00] != 0x10000001 && r[0xa00] != 0x10000102 &&
         !color_mode && !multiply_mode) ||
        (r[0xa00] == 0x10000001 && !clear_rectangle) ||
        (!path && !solid_rectangle && r[0xa34]) || r[0xad1]) {
        return gpu_sw_fail(w, "unsupported draw/blend/global-alpha mode");
    }
    for (unsigned i = 0xa90; i <= 0xa97; i++) {
        if (r[i]) {
            return gpu_sw_fail(w, "unsupported masking state");
        }
    }
    BesGpuTarget *t = gpu_sw_target(w, read);
    if (!t || ++w->draws > 256) {
        return gpu_sw_fail(w, w->error ? w->error : "draw work limit");
    }
    unsigned bounds[4];
    g_autofree uint8_t *coverage = gpu_sw_coverage(w, t, data, bytes, path, bounds);
    if (!coverage) {
        return false;
    }
    unsigned sw = r[0xa2f] & 0xffff, sh = r[0xa2f] >> 16, stride = r[0xa2b];
    unsigned source_bpp = bgra8888_source ? 4 : (packed_source || bgr888_source) ? 3 :
                          (rgb565_source || bgra5551_source) ? 2 : 1;
    if (image && (packed_source || bgr888_source)) {
        if (stride & 3) { return gpu_sw_fail(w, "unaligned packed source stride"); }
        stride = stride / 4 * 3;
    }
    g_autofree uint8_t *source = NULL;
    double m[9] = {0};
    if (image) {
        /* Captured linear sources, COLOR/PAD borders, no source origin. */
        uint64_t length = (uint64_t)stride * sh;
        uint32_t canonical;
        canonical = gpu_sw_canonical(r[0xa29], length);
        if ((!colored_source && !a8_source) ||
            r[0xa27] || r[0xa2d] ||
            !sw || !sh || sw > 1024 || sh > 1024 || stride < sw * source_bpp ||
            stride > 4096 || length > 4 * 1024 * 1024 || !canonical) {
            if (w->draws < 4) {
                fprintf(stderr,
                              "BES GPU image check: colored=%d fmt=%08x a27=%08x a2d=%08x sw=%u sh=%u stride=%u bpp=%u len=%" PRIu64 " canonical=%08x\n",
                              colored_source, r[0xa25], r[0xa27], r[0xa2d],
                              sw, sh, stride, source_bpp, length, canonical);
            }
            return gpu_sw_fail(w, "unsupported or unsafe image source");
        }
        source = g_malloc(length);
        if (!read(r[0xa29], source, length)) {
            return gpu_sw_fail(w, "source RAM read failed");
        }
        for (unsigned i = 0; i < w->target_count; i++) {
            BesGpuTarget *other = &w->targets[i];
            uint64_t lo = MAX(canonical, other->canonical);
            uint64_t hi = MIN((uint64_t)canonical + length,
                              (uint64_t)other->canonical + other->bytes);
            if (hi > lo) {
                memcpy(source + lo - canonical,
                       other->pixels + lo - other->canonical, hi - lo);
            }
        }
        static const unsigned mr[] = {0xa18, 0xa19, 0xa1a, 0xa1c, 0xa1d,
                                      0xa1e, 0xa20, 0xa21, 0xa22};
        for (unsigned i = 0; i < 9; i++) {
            m[i] = gpu_sw_float(r[mr[i]]);
            if (!gpu_sw_number(m[i])) {
                return gpu_sw_fail(w, "invalid image transform");
            }
        }
        if (m[2] != 1 || m[5] != 0 || m[8] != 0 || r[0xa1f] != 1) {
            return gpu_sw_fail(w, "unsupported perspective image");
        }
        if (g_getenv("BES_GPU_TRACE_SAMPLER")) {
            fprintf(stderr,
                    "BES GPU sampler: draw=%u mode=%08x src=%08x/%08x "
                    "size=%ux%u stride=%u target=%08x/%08x size=%ux%u "
                    "c=(%.9g,%.9g) x=(%.9g,%.9g) y=(%.9g,%.9g) "
                    "clip=%08x/%08x path=%08x path-m=(%.9g,%.9g,%.9g,%.9g,%.9g,%.9g)\n",
                    w->draws, r[0xa00], r[0xa25], r[0xa29], sw, sh, stride,
                    r[0xa10], r[0xa11], t->width, t->height,
                    m[0], m[1], m[3], m[4], m[6], m[7],
                    r[0xa39], r[0xa3a], r[0xa34],
                    gpu_sw_float(r[0xa40]), gpu_sw_float(r[0xa41]),
                    gpu_sw_float(r[0xa42]), gpu_sw_float(r[0xa43]),
                    gpu_sw_float(r[0xa44]), gpu_sw_float(r[0xa45]));
        }
    }
    BesGpuColor constant = gpu_sw_color_abgr(r[0xa02]);
    unsigned coverage_width = bounds[2] - bounds[0];
    for (unsigned y = bounds[1]; y < bounds[3]; y++) {
        for (unsigned x = bounds[0]; x < bounds[2]; x++) {
            unsigned cov = coverage[(y - bounds[1]) * coverage_width + x - bounds[0]];
            if (!cov) {
                continue;
            }
            unsigned a = (constant.a * cov + 8) / 16;
            unsigned sr = constant.r, sg = constant.g, sb = constant.b;
            if (image) {
                /* Q65 programs c_step/x_step/y_step in texel (pixel)
                 * coordinates.  The captured 14x20 glyphs use c=-43.5,
                 * x_step=1, y_step=1; multiplying by source dimensions (and
                 * subtracting another half texel) samples the wrong region.
                 * Filter-specific half-texel conventions are already encoded
                 * in the firmware's c_step values. */
                double u = m[0] + x*m[3] + y*m[6];
                double v = m[1] + x*m[4] + y*m[7];
                /* Image coordinates are texel centres. Linear hardware
                 * interpolation addresses taps around (coord - 0.5); Q65's
                 * c_step values (e.g. -43.5) already place the first target
                 * pixel at source texel centre 0. */
                if ((r[0xa25] & 0x30000) && !linear_pad) {
                    u -= 0.5;
                    v -= 0.5;
                }
                if (!isfinite(u) || !isfinite(v)) {
                    continue;
                }
                if (linear_pad) {
                    /* LINEAR paint already programs its sampling centre.
                     * PAD extends edge texels instead of returning COLOR;
                     * for a one-row gradient every finite v selects row 0. */
                    u = CLAMP(u, 0, (double)sw - 1);
                    v = CLAMP(v, 0, (double)sh - 1);
                } else if (u < -1 || v < -1 || u >= sw || v >= sh) {
                    continue;
                }
                int ix = (int)u, iy = (int)v;
                if (u < ix) { ix--; }
                if (v < iy) { iy--; }
                double fx = u - ix, fy = v - iy, sample = 0;
                double colors[3] = {0};
                /* VGLite's A8 LINEAR path is horizontally filtered while
                 * retaining POINT rows (the Q65 glyph path uses this mode).
                 * Do not vertically blend a neighboring row. */
                if (r[0xa25] == 0x01010102) {
                    fy = 0;
                }
                /* SDK FILTER_POINT=0, FILTER_LINEAR=0x10000 and
                 * FILTER_BI_LINEAR=0x20000. About uses point-filtered glyphs.
                 */
                bool filtered = (r[0xa25] & 0x30000) != 0;
                if (!filtered) {
                    /* POINT is by far the common path for Q65 glyphs. Avoid
                     * the generic 2x2 tap loop and floating weights: one
                     * bounded texel load has identical point semantics. */
                    ix = floor(u); iy = floor(v);
                    if (ix >= 0 && iy >= 0 && ix < (int)sw && iy < (int)sh) {
                        uint8_t *sample_pixel = source + iy * stride + ix * source_bpp;
                        unsigned texel = sample_pixel[0];
                        if (colored_source) {
                            BesGpuColor color;
                            if (indexed) {
                                color = gpu_sw_color_argb(r[0xb00 + texel]);
                            } else if (bgra8888_source) {
                                color = gpu_sw_color_bgra(sample_pixel);
                            } else if (bgr888_source) {
                                color = (BesGpuColor) {sample_pixel[2], sample_pixel[1], sample_pixel[0], 255};
                            } else if (bgra5551_source) {
                                color = gpu_sw_color_5551(lduw_le_p(sample_pixel));
                            } else {
                                color = gpu_sw_color_565(lduw_le_p(sample_pixel + packed_source));
                                if (packed_source) { color.a = texel; }
                            }
                            sample = color.a;
                            unsigned color_weight = packed_source ? 255 : color.a;
                            colors[0] = color.r * color_weight;
                            colors[1] = color.g * color_weight;
                            colors[2] = color.b * color_weight;
                        } else {
                            sample = texel;
                        }
                    }
                }
                if (filtered) for (unsigned dy = 0; dy < 2; dy++) {
                    for (unsigned dx = 0; dx < 2; dx++) {
                        int xx = ix + (int)dx, yy = iy + (int)dy;
                        if (linear_pad) {
                            xx = MIN(xx, (int)sw - 1);
                            yy = MIN(yy, (int)sh - 1);
                        }
                        if (xx >= 0 && yy >= 0 && xx < (int)sw && yy < (int)sh) {
                            uint8_t *sample_pixel = source + yy * stride + xx * source_bpp;
                            unsigned texel = sample_pixel[0];
                            double weight = (dx ? fx : 1-fx) * (dy ? fy : 1-fy);
                            if (colored_source) {
                                BesGpuColor color;
                                if (indexed) {
                                    color = gpu_sw_color_argb(r[0xb00 + texel]);
                                } else if (bgra8888_source) {
                                    color = gpu_sw_color_bgra(sample_pixel);
                                } else if (bgr888_source) {
                                    color = (BesGpuColor) {sample_pixel[2], sample_pixel[1], sample_pixel[0], 255};
                                } else if (bgra5551_source) {
                                    color = gpu_sw_color_5551(lduw_le_p(sample_pixel));
                                } else {
                                    color = gpu_sw_color_565(lduw_le_p(sample_pixel + packed_source));
                                    if (packed_source) { color.a = texel; }
                                }
                                double opacity = color.a * weight;
                                sample += opacity;
                                double color_weight = packed_source ? 255 * weight : opacity;
                                colors[0] += color.r * color_weight;
                                colors[1] += color.g * color_weight;
                                colors[2] += color.b * color_weight;
                            } else {
                                sample += texel * weight;
                            }
                        }
                    }
                }
                if (colored_source) {
                    a = ((unsigned)(sample + 0.5) * cov + 8) / 16;
                    if (sample > 0) {
                        sr = MIN(255, colors[0] / sample + 0.5);
                        sg = MIN(255, colors[1] / sample + 0.5);
                        sb = MIN(255, colors[2] / sample + 0.5);
                    }
                    /* VGLite MULTIPLY_IMAGE_MODE modulates the sampled RGBA
                     * by A02 before the source-over blend (e.g. dialog dimming).
                     */
                    if (multiply_mode) {
                        sr = (sr * constant.r + 127) / 255;
                        sg = (sg * constant.g + 127) / 255;
                        sb = (sb * constant.b + 127) / 255;
                        a = (a * constant.a + 127) / 255;
                    }
                } else {
                    a = (a * (unsigned)(sample + 0.5) + 127) / 255;
                }
            }
            if (clear_rectangle) {
                /* vg_lite_clear replaces all RGBA channels, including a
                 * transparent clear of an offscreen BGRA canvas. It is not
                 * a source-over fill and never samples the stale texture. */
                uint8_t *pixel = t->pixels + y * t->stride + x * t->bpp;
                gpu_sw_target_store(t, pixel, constant);
                w->pixels++;
                continue;
            }
            if (!a) {
                continue;
            }
            uint8_t *pixel = t->pixels + y * t->stride + x * t->bpp;
            BesGpuColor old = gpu_sw_target_load(t, pixel);
            BesGpuColor blended = {
                (sr * a + old.r * (255-a) + 127) / 255,
                (sg * a + old.g * (255-a) + 127) / 255,
                (sb * a + old.b * (255-a) + 127) / 255,
                a + (old.a * (255-a) + 127) / 255,
            };
            gpu_sw_target_store(t, pixel, blended);
            w->pixels++;
        }
    }
    return true;
}

static bool gpu_sw_execute(BesGpuWork *w, const uint8_t *data, unsigned bytes,
                           BesGpuRead read, unsigned depth)
{
    if (depth > 4 || bytes > 65536 || (w->command_bytes += bytes) > 512 * 1024) {
        return gpu_sw_fail(w, "command CALL depth or work limit");
    }
    for (unsigned p = 0; p + 8 <= bytes; ) {
        uint32_t command = ldl_le_p(data + p), op = command >> 28;
        unsigned step = 8;
        if (op == 0) {
            if (depth || (command & ~31u) || p + 8 != bytes || ldl_le_p(data + p + 4)) {
                gpu_sw_fail(w, "invalid command END");
                break;
            }
            w->event = 1u << command;
            return true;
        } else if (op == 7) {
            if (!depth || command != 0x70000000 || p + 8 != bytes ||
                ldl_le_p(data + p + 4)) {
                return gpu_sw_fail(w, "invalid command RETURN");
            }
            return true;
        } else if (op == 6) {
            unsigned count = command & 0xffff;
            uint32_t address = ldl_le_p(data + p + 4);
            if ((command & 0x0fff0000) || !count || count > 8192 ||
                (address & 7) || !gpu_sw_canonical(address, count * 8)) {
                return gpu_sw_fail(w, "invalid or unsafe command CALL");
            }
            g_autofree uint8_t *nested = g_malloc(count * 8);
            if (!read(address, nested, count * 8) ||
                !gpu_sw_execute(w, nested, count * 8, read, depth + 1)) {
                if (!w->error) { gpu_sw_fail(w, "command CALL RAM read failed"); }
                return false;
            }
        } else if (op == 3) {
            unsigned count = (command >> 16) & 0xfff, reg = command & 0xffff;
            step = ((count + 1) * 4 + 7) & ~7u;
            if (!count || step > bytes - p || reg + count > BES_GPU_REGS) {
                gpu_sw_fail(w, "invalid STATE framing");
                break;
            }
            for (unsigned i = 0; i < count; i++) {
                if (!gpu_sw_state(reg + i)) {
                    gpu_sw_fail(w, "unsupported STATE register");
                    break;
                }
                w->regs[reg + i] = ldl_le_p(data + p + 4 + i * 4);
            }
            if (w->error) { break; }
        } else if (op == 4) {
            step += (command & 0xffff) * 8;
            if ((command & 0x0fff0000) || ldl_le_p(data + p + 4) ||
                step > bytes - p || step == 8 ||
                !gpu_sw_draw(w, data + p + 8, step - 8, read)) {
                if (!w->error) { gpu_sw_fail(w, "invalid DATA framing"); }
                break;
            }
        } else if ((op == 1 || op == 2) &&
                   (command == ((op << 28) | 7) || command == ((op << 28) | 16)) &&
                   !ldl_le_p(data + p + 4)) {
            /* Single sequential queue: the public semaphore/stall pair is
             * ordered by this submission's completion. No external engines.
             */
        } else if (command != 0x80000000 || ldl_le_p(data + p + 4)) {
            gpu_sw_fail(w, "unsupported command opcode");
            break;
        }
        p += step;
    }
    if (!w->error) { gpu_sw_fail(w, depth ? "missing command RETURN" : "missing command END"); }
    return false;
}

static BesGpuWork *gpu_sw_prepare(const uint32_t *regs, const uint8_t *data,
                                  unsigned bytes, BesGpuRead read,
                                  const char **error)
{
    BesGpuWork *w = g_new0(BesGpuWork, 1);
    memcpy(w->regs, regs, sizeof(w->regs));
    if (gpu_sw_execute(w, data, bytes, read, 0)) { return w; }
    *error = w->error;
    gpu_sw_free(w);
    return NULL;
}
#endif
