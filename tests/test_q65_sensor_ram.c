/* SPDX-License-Identifier: GPL-2.0-or-later
 * Exercise the production sensor address-space configuration using a
 * minimal alias resolver. Actual ARM execution is covered by firmware runs. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct MemoryRegion MemoryRegion;
struct MemoryRegion {
    uint64_t size, offset, base;
    uint8_t *data;
    MemoryRegion *alias, *children[4];
    unsigned child_count;
    int priority;
};
typedef struct {
    MemoryRegion sensor_space, sensor_system, sensor_ram_exec, sensor_ram;
} Best1503State;
static MemoryRegion system_memory;
#define OBJECT(obj) (obj)

static MemoryRegion *get_system_memory(void) { return &system_memory; }
static uint64_t memory_region_size(MemoryRegion *mr) { return mr->size; }
static void memory_region_init(MemoryRegion *mr, void *owner,
                               const char *name, uint64_t size)
{
    (void)owner; (void)name;
    memset(mr, 0, sizeof(*mr));
    mr->size = size;
}
static void memory_region_init_alias(MemoryRegion *mr, void *owner,
                                     const char *name, MemoryRegion *target,
                                     uint64_t offset, uint64_t size)
{
    memory_region_init(mr, owner, name, size);
    mr->alias = target;
    mr->offset = offset;
}
static void memory_region_add_subregion_overlap(MemoryRegion *parent,
                                                uint64_t base,
                                                MemoryRegion *child,
                                                int priority)
{
    assert(parent->child_count < 4);
    child->base = base;
    child->priority = priority;
    parent->children[parent->child_count++] = child;
}
static void memory_region_add_subregion(MemoryRegion *parent, uint64_t base,
                                        MemoryRegion *child)
{
    memory_region_add_subregion_overlap(parent, base, child, 0);
}

#include "q65-sensor-ram.inc"

static uint8_t *resolve(MemoryRegion *mr, uint64_t addr)
{
    if (addr >= mr->size) { return NULL; }
    if (mr->alias) { return resolve(mr->alias, addr + mr->offset); }
    if (mr->data) { return mr->data + addr; }
    MemoryRegion *best = NULL;
    for (unsigned n = 0; n < mr->child_count; n++) {
        MemoryRegion *child = mr->children[n];
        if (addr >= child->base && addr - child->base < child->size &&
            (!best || child->priority > best->priority)) {
            best = child;
        }
    }
    return best ? resolve(best, addr - best->base) : NULL;
}

int main(void)
{
    Best1503State s = {0};
    MemoryRegion ap_exec = {0};
    const size_t size = 0x88000;
    s.sensor_ram.size = size;
    s.sensor_ram.data = calloc(1, size);
    ap_exec.size = size;
    ap_exec.data = calloc(1, size);
    assert(s.sensor_ram.data && ap_exec.data);
    memory_region_init(&system_memory, NULL, "system", UINT64_MAX);
    memory_region_add_subregion(&system_memory, 0x20300000, &s.sensor_ram);
    memory_region_add_subregion(&system_memory, 0x00300000, &ap_exec);
    sensor_map_exec_alias(&s);

    /* Both reset copy destinations must execute at the addresses used
     * by the unmodified firmware's call veneers, including their edges. */
    const uint32_t offsets[] = {0, 0x180, 0x1cfc, 0x202a, 0x204e, 0x209f,
                               0x2aa0, 0x2b50, 0x2bb4, 0x3744, 0x38ff,
                               0x87fff};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        uint32_t off = offsets[i];
        uint8_t *data = resolve(&s.sensor_space, 0x20300000 + off);
        uint8_t *exec = resolve(&s.sensor_space, 0x00500000 + off);
        assert(data && exec && data == exec);
        *data = (uint8_t)(i + 1);
        assert(*exec == i + 1);
        *exec = (uint8_t)(i + 81);
        assert(*data == i + 81);
    }
    assert(resolve(&s.sensor_space, 0x00588000) == NULL);
    assert(resolve(&s.sensor_space, 0x004fffff) == NULL);
    /* The old erroneous 0030xxxx mapping must leave the AP's real
     * executable SRAM visible through the shared system view. */
    assert(resolve(&s.sensor_space, 0x00301cfc) == ap_exec.data + 0x1cfc);
    assert(resolve(&s.sensor_space, 0x00301cfc) != s.sensor_ram.data + 0x1cfc);
    free(s.sensor_ram.data);
    free(ap_exec.data);
    puts("Q65 sensor RAMX: code/data coherence, bounds and AP alias passed");
    return 0;
}
