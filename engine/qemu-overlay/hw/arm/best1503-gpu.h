/* SPDX-License-Identifier: GPL-2.0-or-later
 * Bounded software subset of the public VGLite command stream. This is not
 * a Vivante hardware renderer: curves use 32 segments and coverage uses a
 * 4 x 4 sample grid. Only the Q66 startup operations listed below are accepted.
 * All writes and persistent state remain private until the caller commits.
 */
#ifndef BEST1503_GPU_H
#define BEST1503_GPU_H
#include <math.h>

#define BES_GPU_REGS 0x1000
#define BES_GPU_TARGETS 4
#define BES_GPU_EDGES 16384

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
typedef struct { unsigned left, top, right, bottom; } BesGpuBounds;

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

static BesGpuColor gpu_sw_target_load(const BesGpuTarget *t,
                                     const uint8_t *pixel)
{
    if (t->format == 0x109 || t->format == 0x129) {
        return (BesGpuColor) {pixel[t->format == 0x109 ? 2 : 0],
                             pixel[1], pixel[t->format == 0x109 ? 0 : 2], 255};
    }
    if (t->bpp == 4) {
        return gpu_sw_color_bgra(pixel);
    }
    /* SDK BGR565 (0x101) has red in bits 15:11; RGB565 (0x121)
     * reverses red/blue. BGRA5658 (0x10a) stores alpha after the two
     * little-endian BGR565 bytes; alpha-first is ABGR8565 (0x11a). */
    BesGpuColor color = gpu_sw_color_565(lduw_le_p(pixel));
    if (t->format == 0x121) {
        unsigned red = color.r; color.r = color.b; color.b = red;
    }
    if (t->bpp == 3) {
        color.a = pixel[2];
    }
    return color;
}

static void gpu_sw_target_store(const BesGpuTarget *t, uint8_t *pixel,
                                 BesGpuColor color)
{
    if (t->format == 0x109 || t->format == 0x129) {
        pixel[t->format == 0x109 ? 2 : 0] = color.r;
        pixel[1] = color.g;
        pixel[t->format == 0x109 ? 0 : 2] = color.b;
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
        pixel[2] = color.a;
    }
    stw_le_p(pixel,
             ((color.r >> 3) << 11) | ((color.g >> 2) << 5) | (color.b >> 3));
}

static uint32_t gpu_sw_canonical(uint32_t address, unsigned length)
{
    static const uint32_t bases[] = {0x20000000, 0x00200000, 0x38000000,
                                    0x18000000, 0x3c000000, 0x1c000000};
    for (unsigned i = 0; i < G_N_ELEMENTS(bases); i++) {
        unsigned size = i < 2 ? 0x160000 : 0x2000000;
        if (address >= bases[i] &&
            (uint64_t)address + length <= (uint64_t)bases[i] + size) {
            return (i < 2 ? 0x20000000 : 0x38000000) + address - bases[i];
        }
    }
    return 0;
}

static bool gpu_sw_fail(BesGpuWork *w, const char *reason)
{
    qemu_log_mask(LOG_GUEST_ERROR,
        "BES GPU rejected state: mode=%08x target=%08x source=%08x source_addr=%08x source_stride=%08x\n",
        w->regs[0xa00], w->regs[0xa10], w->regs[0xa25], w->regs[0xa29], w->regs[0xa2b]);
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
    unsigned bpp = (format == 0x10a || format == 0x109 || format == 0x129) ? 3 :
                   format == 0x103 ? 4 : 2;
    /* Public VGLite converts 24-bit byte stride to a 32-bit hardware stride. */
    if (bpp == 3) {
        if (stride & 3) { gpu_sw_fail(w, "unaligned packed 24-bit stride"); return NULL; }
        stride = stride / 4 * 3;
    }
    /* Some stock targets round width to 16 pixels while keeping the panel's
     * packed row pitch. Only accept that description when the explicit
     * scissor lies wholly inside the real rows; render into the physical
     * row width so no coverage or store can cross into the next row. */
    if (width <= 1024 && width * bpp > stride) {
        unsigned left = r[0xa39] & 0xffff, top = r[0xa39] >> 16;
        unsigned clip_width = r[0xa3a] & 0xffff, clip_height = r[0xa3a] >> 16;
        if (clip_width && clip_height && left + clip_width <= stride / bpp &&
            top + clip_height <= height) {
            width = stride / bpp;
        }
    }
    uint64_t length = (uint64_t)stride * height;
    uint32_t canonical;
    if ((format != 0x101 && format != 0x121 && format != 0x10a && format != 0x103 &&
         format != 0x109 && format != 0x129) ||
        !width || !height || width > 1024 || height > 1024 ||
        stride < width * bpp || stride > 4096 || (bpp == 2 && (stride & 1)) ||
        (r[0xa11] & 1) || length > 4 * 1024 * 1024 ||
        !(canonical = gpu_sw_canonical(r[0xa11], length))) {
        qemu_log_mask(LOG_GUEST_ERROR,
            "BES GPU target rejected: format=0x%x address=0x%08x width=%u height=%u stride=%u raw_stride=%u\n",
            format, r[0xa11], width, height, stride, r[0xa12]);
        gpu_sw_fail(w, "unsupported or unsafe linear target");
        return NULL;
    }
    for (unsigned i = 0; i < w->target_count; i++) {
        BesGpuTarget *t = &w->targets[i];
        if (canonical == t->canonical && width == t->width &&
            height == t->height && stride == t->stride && format == t->format) {
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

static bool gpu_sw_edge_add(BesGpuWork *w, BesGpuEdge *edges, unsigned *count,
                            double x0, double y0, double x1, double y1)
{
    if (y0 == y1) {
        return true;
    }
    if (*count == BES_GPU_EDGES) {
        return gpu_sw_fail(w, "path edge limit");
    }
    edges[(*count)++] = (BesGpuEdge) {x0, y0, x1, y1};
    return true;
}

/* Edges are clipped to the +-65536 box around every target. The parts
 * above or below it cover no target row; parts left or right of it become
 * vertical edges on the box side, which keeps the winding of every point
 * inside the box. */
static bool gpu_sw_edge(BesGpuWork *w, BesGpuEdge *edges, unsigned *count,
                        double x0, double y0, double x1, double y1)
{
    const double box = 65536;
    if (y0 == y1) {
        return true;
    }
    double t0 = (-box - y0) / (y1 - y0), t1 = (box - y0) / (y1 - y0);
    double ta = MAX(0, MIN(t0, t1)), tb = MIN(1, MAX(t0, t1));
    if (ta >= tb) {
        return true;
    }
    double ax = x0 + ta * (x1 - x0), ay = y0 + ta * (y1 - y0);
    double bx = x0 + tb * (x1 - x0), by = y0 + tb * (y1 - y0);
    /* Split where the edge crosses the box sides, in edge order. */
    double cut[4] = {0};
    unsigned cuts = 1;
    for (int side = -1; side <= 1; side += 2) {
        double t = bx != ax ? (side * box - ax) / (bx - ax) : -1;
        if (t > 0 && t < 1) {
            cut[cuts++] = t;
        }
    }
    if (cuts == 3 && cut[1] > cut[2]) {
        double t = cut[1]; cut[1] = cut[2]; cut[2] = t;
    }
    cut[cuts++] = 1;
    for (unsigned i = 0; i + 1 < cuts; i++) {
        double sx = ax + cut[i] * (bx - ax), sy = ay + cut[i] * (by - ay);
        double ex = ax + cut[i + 1] * (bx - ax), ey = ay + cut[i + 1] * (by - ay);
        if (!gpu_sw_edge_add(w, edges, count, CLAMP(sx, -box, box), sy,
                             CLAMP(ex, -box, box), ey)) {
            return false;
        }
    }
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
        if (!isfinite(m[i])) {
            goto empty;
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
            v[i] = m[0] * a + m[1] * b + ((op & 1) ? x : m[2]);
            v[i + 1] = m[3] * a + m[4] * b + ((op & 1) ? y : m[5]);
            if (!isfinite(v[i]) || !isfinite(v[i + 1])) {
                goto empty;
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
empty:
    /* The firmware builds some paths from values it has not computed yet
     * (a chart with no samples hands over NaN corners). Such a path has no
     * area: it covers no pixel and the rest of the command buffer renders. */
    qemu_log_mask(LOG_GUEST_ERROR, "BES GPU path with non-finite coordinates drawn empty\n");
    *count = 0;
    return true;
}

static int gpu_sw_cross_compare(const void *a, const void *b)
{
    double x = ((const BesGpuCross *)a)->x, y = ((const BesGpuCross *)b)->x;
    return (x > y) - (x < y);
}

static bool gpu_sw_bounds(BesGpuWork *w, BesGpuTarget *t,
                          const uint8_t *data, unsigned bytes, bool path,
                          BesGpuBounds *b)
{
    if (!path && bytes != 8) {
        return gpu_sw_fail(w, "invalid rectangle DATA");
    }
    /* Rectangle DATA is already the firmware's clipped rectangle. Paths
     * use the explicit tessellation/scissor bounds. Keep those semantics
     * while avoiding a whole-target allocation and scan for every glyph. */
    unsigned x = path ? w->regs[0xa39] & 0xffff : lduw_le_p(data);
    unsigned y = path ? w->regs[0xa39] >> 16 : lduw_le_p(data + 2);
    unsigned width = path ? w->regs[0xa3a] & 0xffff : lduw_le_p(data + 4);
    unsigned height = path ? w->regs[0xa3a] >> 16 : lduw_le_p(data + 6);
    *b = (BesGpuBounds) {MIN(t->width, x), MIN(t->height, y),
        MIN(t->width, x + width), MIN(t->height, y + height)};
    return true;
}

static uint8_t *gpu_sw_coverage(BesGpuWork *w, const BesGpuBounds *b,
                               const uint8_t *data, unsigned bytes, bool path)
{
    unsigned width = b->right - b->left, height = b->bottom - b->top;
    uint8_t *coverage = g_malloc0(MAX(1, width * height));
    if (!path) {
        memset(coverage, 16, width * height);
        return coverage;
    }
    g_autofree BesGpuEdge *edges = g_new(BesGpuEdge, BES_GPU_EDGES);
    g_autofree BesGpuCross *cross = g_new(BesGpuCross, BES_GPU_EDGES);
    unsigned count = 0;
    if (!gpu_sw_path(w, data, bytes, edges, &count)) {
        g_free(coverage);
        return NULL;
    }
    if ((w->edge_budget += (uint64_t)count * height * 4) > 32 * 1024 * 1024) {
        gpu_sw_fail(w, "path work limit");
        g_free(coverage);
        return NULL;
    }
    unsigned left = b->left, top = b->top, right = b->right, bottom = b->bottom;
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
                int begin = MAX((int)left, (int)cross[i].x - 1);
                int end = MIN((int)right, (int)cross[i + 1].x + 2);
                for (int x = begin; x < end; x++) {
                    for (unsigned sx = 0; sx < 4; sx++) {
                        double xx = x + (sx + 0.5) / 4;
                        if (xx >= cross[i].x && xx < cross[i + 1].x) {
                            coverage[(y - top) * width + x - left]++;
                        }
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
    /* Observed P67 solid-path DST_IN on a premultiplied BGRA8888 layer.
     * Public vg_lite_draw emits A02 paint and no imageMode bits; A25 and
     * the source registers can still describe an earlier image draw. */
    bool dst_in_mode = r[0xa00] == 0x10000402;
    bool multiply_mode = r[0xa00] == 0x10002181 || r[0xa00] == 0x10002102;
    bool path = r[0xa00] != 0x2181 && r[0xa00] != 0x10001181 &&
                r[0xa00] != 0x10002181;
    bool image = r[0xa00] != 0x10000102 && !dst_in_mode;
    bool color_mode = r[0xa00] == 0x10001102 || r[0xa00] == 0x10001181;
    /* Public VG_LITE_PATTERN_PAD clamps texture samples to the image edge. */
    bool pad_source = (r[0xa25] & 0x1000) != 0;
    uint32_t source_format = r[0xa25] & ~0x1000u;
    bool indexed = source_format == 0x01000900 || source_format == 0x01020900;
    bool packed_source = source_format == 0x41020100 || source_format == 0x41000100;
    bool rgb565_source = source_format == 0x01000105 ||
                         source_format == 0x01010105 || source_format == 0x01020105;
    /* Public convert_source_format: BGRA8888 = 7. Activity pages composite
     * their offscreen BGRA8888 canvas back into the RGB565 display target. */
    bool bgra8888_source = source_format == 0x01000107 ||
                           source_format == 0x01010107 || source_format == 0x01020107;
    bool rgb888_source = (source_format & ~0x30020u) == 0x21000100 &&
                         (source_format & 0x30000) != 0x30000;
    bool colored_source = indexed || packed_source || rgb565_source || bgra8888_source || rgb888_source;
    /* Stock BES/LVGL normal/multiply image draws set A00 bit 28 and pass
     * premultiplied BGRA texels (for example 7f000080 for half-red). Preserve
     * their RGB contribution across filtering; multiplying by alpha again
     * darkens both imported images and offscreen system layers. The legacy
     * A00=0x2181 straight-alpha path and ARGB CLUT remain unchanged. */
    bool premultiplied_source = packed_source ||
        (bgra8888_source && (r[0xa00] & 0x10000000));
    if ((r[0xa00] != 0x2181 && r[0xa00] != 0x2102 &&
         r[0xa00] != 0x10000102 && !color_mode && !multiply_mode && !dst_in_mode) ||
        (dst_in_mode && r[0xa10] != 0x103) ||
        (!path && r[0xa34]) || r[0xad1]) {
        return gpu_sw_fail(w, "unsupported draw/blend/global-alpha mode");
    }
    for (unsigned i = 0xa90; i <= 0xa97; i++) {
        if (r[i]) {
            return gpu_sw_fail(w, "unsupported masking state");
        }
    }
    BesGpuTarget *t = gpu_sw_target(w, read);
    BesGpuBounds bounds;
    if (!t || ++w->draws > 256) {
        return gpu_sw_fail(w, w->error ? w->error : "draw work limit");
    }
    if (!gpu_sw_bounds(w, t, data, bytes, path, &bounds)) {
        return false;
    }
    unsigned coverage_width = bounds.right - bounds.left;
    if ((w->budget += (uint64_t)coverage_width * (bounds.bottom - bounds.top)) > 8 * 1024 * 1024) {
        return gpu_sw_fail(w, "draw work limit");
    }
    g_autofree uint8_t *coverage = gpu_sw_coverage(w, &bounds, data, bytes, path);
    if (!coverage) {
        return false;
    }
    unsigned sw = r[0xa2f] & 0xffff, sh = r[0xa2f] >> 16, stride = r[0xa2b];
    unsigned source_bpp = bgra8888_source ? 4 : (packed_source || rgb888_source) ? 3 : rgb565_source ? 2 : 1;
    if (image && (packed_source || rgb888_source)) {
        if (stride & 3) { return gpu_sw_fail(w, "unaligned packed source stride"); }
        stride = stride / 4 * 3;
    }
    g_autofree uint8_t *source = NULL;
    double m[9] = {0};
    if (image) {
        /* Captured linear sources, transparent border, no source origin. */
        uint64_t length = (uint64_t)stride * sh;
        uint32_t canonical;
        if ((!colored_source && source_format != 0x01010102) ||
            r[0xa27] || r[0xa2d] ||
            !sw || !sh || sw > 1024 || sh > 1024 || stride < sw * source_bpp ||
            stride > 4096 || length > 4 * 1024 * 1024 ||
            !(canonical = gpu_sw_canonical(r[0xa29], length))) {
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
    }
    BesGpuColor constant = gpu_sw_color_abgr(r[0xa02]);
    for (unsigned y = bounds.top; y < bounds.bottom; y++) {
        for (unsigned x = bounds.left; x < bounds.right; x++) {
            unsigned cov = coverage[(y - bounds.top) * coverage_width + x - bounds.left];
            if (!cov) {
                continue;
            }
            if (dst_in_mode) {
                /* DST_IN: premultiplied destination RGBA * paint alpha.
                 * Coverage interpolates that blend with the old pixel:
                 * factor = (1 - coverage) + coverage * paint_alpha.
                 * In particular, zero alpha erases a fully covered pixel;
                 * it must not take the source-over transparent early exit.
                 * Keep 16-sample precision until the final rounded store. */
                unsigned factor = (16 - cov) * 255 + cov * constant.a;
                uint8_t *pixel = t->pixels + y * t->stride + x * t->bpp;
                BesGpuColor old = gpu_sw_target_load(t, pixel);
                BesGpuColor masked = {
                    (old.r * factor + 2040) / 4080,
                    (old.g * factor + 2040) / 4080,
                    (old.b * factor + 2040) / 4080,
                    (old.a * factor + 2040) / 4080,
                };
                gpu_sw_target_store(t, pixel, masked);
                w->pixels++;
                continue;
            }
            unsigned a = (constant.a * cov + 8) / 16;
            unsigned sr = constant.r, sg = constant.g, sb = constant.b;
            if (image) {
                /* Linear normalized coordinates; c_step includes pixel-center
                 * adjustment in public VGLite. Out-of-image samples are zero.
                 */
                double u = (m[0] + x*m[3] + y*m[6]) * sw - 0.5;
                double v = (m[1] + x*m[4] + y*m[7]) * sh - 0.5;
                if (!isfinite(u) || !isfinite(v)) {
                    continue;
                }
                if (pad_source) {
                    u = CLAMP(u, 0, sw - 1);
                    v = CLAMP(v, 0, sh - 1);
                }
                if (u < -1 || v < -1 || u >= sw || v >= sh) {
                    continue;
                }
                int ix = (int)u, iy = (int)v;
                if (u < ix) { ix--; }
                if (v < iy) { iy--; }
                double fx = u - ix, fy = v - iy, sample = 0;
                /* SDK FILTER_POINT=0, FILTER_LINEAR=0x10000 and
                 * FILTER_BI_LINEAR=0x20000. About uses point-filtered glyphs.
                 */
                if (!(r[0xa25] & 0x30000)) {
                    ix = floor(u + 0.5); iy = floor(v + 0.5);
                    fx = fy = 0;
                }
                double colors[3] = {0};
                for (unsigned dy = 0; dy < 2; dy++) {
                    for (unsigned dx = 0; dx < 2; dx++) {
                        int xx = ix + dx, yy = iy + dy;
                        if (xx >= 0 && yy >= 0 && (unsigned)xx < sw && (unsigned)yy < sh) {
                            uint8_t *sample_pixel = source + yy * stride + xx * source_bpp;
                            unsigned texel = sample_pixel[0];
                            double weight = (dx ? fx : 1-fx) * (dy ? fy : 1-fy);
                            if (colored_source) {
                                BesGpuColor color;
                                if (indexed) {
                                    color = gpu_sw_color_argb(r[0xb00 + texel]);
                                } else if (bgra8888_source) {
                                    color = gpu_sw_color_bgra(sample_pixel);
                                } else if (rgb888_source) {
                                    bool rgb_order = source_format & 0x20;
                                    color = (BesGpuColor) {sample_pixel[rgb_order ? 0 : 2],
                                        sample_pixel[1], sample_pixel[rgb_order ? 2 : 0], 255};
                                } else {
                                    color = gpu_sw_color_565(lduw_le_p(sample_pixel));
                                    if (packed_source) { color.a = sample_pixel[2]; }
                                }
                                double opacity = color.a * weight;
                                sample += opacity;
                                double color_weight = premultiplied_source ? 255 * weight : opacity;
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
