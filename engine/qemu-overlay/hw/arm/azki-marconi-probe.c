/*
 * O62M Marconi AP reset probe. This is NOT a functioning watch machine.
 *
 * Evidence: O62M 3.1.115 AP SHA256
 * c60ec97e12de3959b03a5963f05ade29f7843af835e83a528d4ef1b55f3d6428.
 * vela_mmap.bin declares AP 0x605c0000+0x3333c00. The unmodified reset
 * entry writes VTOR=0x605c0000 and sets MSP=0x61745d00. There are 165
 * vector words before the first instruction at file offset 0x294.
 *
 * AP RAM and the architectural Cortex-M55 core are implemented. An opt-in
 * counter32k model provides only the verified 32-bit read at 0x4a000090.
 * uart16550 enables the DW UART's basic initialization registers and FIFO
 * reset using QEMU's real serial core. uart-dw-config adds documented DW
 * shadow configuration and PTIME while the FIFOs are empty. FIFO capacity,
 * clock control, fractional divisor and data transfer remain unsupported.
 * Every other bus access is a transaction error and ends this probe with
 * exit status 86. No peripheral status, interrupt, or success is invented.
 * No code bytes, vectors or guest checks are patched. The ROM/secure boot
 * chain is not executed. sbl-reset selects the unmodified SBL reset entry
 * instead of AP; its 4 KiB container header stays at 0x60001000 and its
 * vector payload is at 0x60002000, within mmap's ap_cp_sm RAM region.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/boards.h"
#include "hw/arm/armv7m.h"
#include "hw/arm/boot.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-clock.h"
#include "system/address-spaces.h"
#include "system/runstate.h"
#include "hw/core/cpu.h"
#include "hw/char/serial.h"
#include "hw/irq.h"
#include "hw/loader.h"
#include "system/system.h"
#include "qom/object.h"

#define MARCONI_AP_BASE 0x605c0000u
#define MARCONI_AP_BYTES 0x03333c00u
#define MARCONI_SBL_BASE 0x60001000u
#define MARCONI_SBL_BYTES 0x0023f000u
#define MARCONI_SBL_VTOR 0x60002000u
#define MARCONI_PROBE_EXIT 86

typedef struct MarconiProbeState {
    MachineState parent_obj;
    ARMv7MState armv7m;
    MemoryRegion ap_ram;
    MemoryRegion mmap_ram;
    MemoryRegion missing;
    Clock *cpuclk;
    bool reported;
    bool sbl_reset;
    char *mmap_image;
    bool counter32k;
    unsigned counter_reads;
    bool uart16550;
    bool uart_dw_config;
    SerialState uart;
    unsigned uart_accesses;
    uint8_t uart_ier_extra;
    uint8_t uart_stet;
} MarconiProbeState;

/*
 * AP 0x60c36826 extends the low word on wrap and computes
 * (extended_ticks * 10000) >> 15; 0x605cc5da uses a 32768-tick interval.
 * This provides the observed low word only. The counter's power/reset
 * controls, high word, interrupt routing and retention remain unknown.
 * The epoch is virtual machine time zero. Splitting seconds avoids an
 * overflowing nanosecond product; the final cast supplies modulo 2^32.
 */
static uint32_t marconi_counter_ticks(uint64_t ns)
{
    return (uint32_t)((ns / NANOSECONDS_PER_SECOND) * 32768 +
                      ((ns % NANOSECONDS_PER_SECOND) * 32768) /
                      NANOSECONDS_PER_SECOND);
}

static MemTxResult marconi_missing(MarconiProbeState *s, hwaddr address,
                                  unsigned size, bool write, uint64_t value)
{
    if (!s->reported) {
        s->reported = true;
        error_report("MARCONI_PROBE_UNIMPLEMENTED access=%s address=0x%08"
                     HWADDR_PRIx " size=%u value=0x%016" PRIx64
                     " pc=0x%08" PRIx64
                     " transaction=MEMTX_ERROR stop=first-unimplemented-access",
                     write ? "write" : "read", address, size, value,
                     current_cpu ? CPU_GET_CLASS(current_cpu)->get_pc(current_cpu)
                                 : UINT64_MAX);
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC,
                                              MARCONI_PROBE_EXIT);
        if (current_cpu) {
            cpu_exit(current_cpu);
        }
    }
    return MEMTX_ERROR;
}

static bool marconi_uart_ptime(MarconiProbeState *s)
{
    return s->uart_dw_config && (s->uart_ier_extra & 0x80) &&
           (s->uart.fcr & 1);
}

static void marconi_uart_irq(void *opaque, int n, int level)
{
    MarconiProbeState *s = opaque;

    if (marconi_uart_ptime(s) && (s->uart.ier & 2) &&
        fifo8_is_empty(&s->uart.xmit_fifo)) {
        /* Empty is at or below all four documented STET thresholds.
         * Nonempty transfers remain barred pending FIFO-capacity evidence. */
        level = 1;
    }
    /* AP private+0xe4 is exception 73. 0x60c373e2 subtracts 16 before
     * programming NVIC ISER1 bit25, establishing external interrupt 57. */
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->armv7m), 57), level);
}

static void marconi_uart_refresh_irq(MarconiProbeState *s)
{
    marconi_uart_irq(s, 0, (s->uart.iir & 0xf) != 1);
}

static int marconi_uart_can_receive(void *opaque)
{
    /* The actual DW FIFO depth and receive timing have not been established.
     * Do not let the 16550's internal storage imply a 16-byte hardware FIFO. */
    return 0;
}

static void marconi_uart_log(MarconiProbeState *s, hwaddr address,
                             uint64_t value, bool write)
{
    if (s->uart_accesses++ < 64) {
        error_report("MARCONI_PROBE_UART access=%s address=0x%08"
                     HWADDR_PRIx " value=0x%08" PRIx64,
                     write ? "write" : "read", address, value);
    }
}

static bool marconi_uart_read(MarconiProbeState *s, hwaddr address,
                              uint64_t *value, unsigned size)
{
    hwaddr offset = address - 0x4a030000;
    SerialState *uart = &s->uart;

    if (!s->uart16550 || size != 4 || (offset & 3)) {
        return false;
    }
    if (offset < 0x20 && offset != 0x18) {
        /* The modem input wiring is unknown, so MSR is deliberately absent. */
        *value = serial_io_ops.read(uart, offset >> 2, 1);
        if (s->uart_dw_config && offset == 4 && !(uart->lcr & 0x80)) {
            *value |= s->uart_ier_extra;
        }
        if (marconi_uart_ptime(s)) {
            assert(fifo8_is_empty(&uart->xmit_fifo));
            if (offset == 0x14) {
                /* DW PTIME changes LSR.THRE from empty to FIFO full. */
                *value &= ~UINT64_C(0x20);
            } else if (offset == 8 && ((*value & 0xf) == 1 ||
                                       (*value & 0xf) == 2)) {
                *value = (*value & ~UINT64_C(0xf)) | ((uart->ier & 2) ? 2 : 1);
                marconi_uart_refresh_irq(s);
            }
        }
    } else if (offset == 0x7c) {
        /* Synopsys USR: BUSY, TFNF, TFE, RFNE, RFF. Data transfer is
         * stopped below until clock/FIFO configuration is implemented. */
        *value = ((uart->lsr & 0x40) ? 0 : 1) |
                 (fifo8_is_full(&uart->xmit_fifo) ? 0 : 2) |
                 (fifo8_is_empty(&uart->xmit_fifo) ? 4 : 0) |
                 ((uart->lsr & 1) ? 8 : 0) |
                 (fifo8_is_full(&uart->recv_fifo) ? 16 : 0);
    } else if (s->uart_dw_config && offset == 0x98) {
        *value = uart->fcr & 1;
    } else if (s->uart_dw_config && offset == 0x9c) {
        *value = uart->fcr >> 6;
    } else if (s->uart_dw_config && offset == 0xa0) {
        *value = s->uart_stet;
    } else {
        return false;
    }
    marconi_uart_log(s, address, *value, false);
    return true;
}

static bool marconi_uart_write(MarconiProbeState *s, hwaddr address,
                               uint64_t value, unsigned size)
{
    hwaddr offset = address - 0x4a030000;
    SerialState *uart = &s->uart;

    if (!s->uart16550 || size != 4 || (offset & 3) || value > 255) {
        return false;
    }
    if (offset == 0x88 && !(value & ~6)) {
        /* SRR.RFR/XFR reset the corresponding real FIFO and its status.
         * Keep FCR configuration; SRR=6 must not reset LCR/divisor/IER.
         * Synopsys dw_uart_hal.h defines RFR=2 and XFR=4. */
        serial_io_ops.write(uart, 2, uart->fcr | value, 1);
    } else if (s->uart_dw_config && offset == 0xa0 && value <= 3) {
        s->uart_stet = value;
    } else if (s->uart_dw_config && offset == 0x9c && value <= 3) {
        serial_io_ops.write(uart, 2, (uart->fcr & ~0xc0) | (value << 6), 1);
    } else if (s->uart_dw_config && offset == 0x98 && value <= 1) {
        serial_io_ops.write(uart, 2, (uart->fcr & ~1) | value, 1);
    } else if (offset < 0x20 && offset != 0x14 && offset != 0x18) {
        if ((offset == 0 && !(uart->lcr & 0x80)) ||
            (offset == 4 && !(uart->lcr & 0x80) &&
             (value & ~(s->uart_dw_config ? 0x8f : 0x0f))) ||
            (offset == 8 && (value & 1) && !s->uart_dw_config) ||
            (offset == 0x10 && (value & ~0x1f))) {
            /* No output at an invented baud rate, no silently dropped PTIME
             * bit, no substitution of a 16-byte FIFO for the DW FIFO, and
             * no fake auto-flow control. */
            return false;
        }
        if (s->uart_dw_config && offset == 4 && !(uart->lcr & 0x80)) {
            s->uart_ier_extra = value & 0x80;
            serial_io_ops.write(uart, 1, value & 0x0f, 1);
        } else {
            if (s->uart_dw_config && offset == 8) {
                s->uart_stet = (value >> 4) & 3;
            }
            serial_io_ops.write(uart, offset >> 2, value, 1);
        }
    } else {
        return false;
    }
    marconi_uart_log(s, address, value, true);
    marconi_uart_refresh_irq(s);
    return true;
}

static MemTxResult marconi_missing_read(void *opaque, hwaddr address,
                                       uint64_t *value, unsigned size,
                                       MemTxAttrs attrs)
{
    MarconiProbeState *s = opaque;

    if (marconi_uart_read(s, address, value, size)) {
        return MEMTX_OK;
    }
    if (s->counter32k && address == 0x4a000090 && size == 4) {
        uint64_t ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        *value = marconi_counter_ticks(ns);
        if (s->counter_reads++ < 4) {
            error_report("MARCONI_PROBE_COUNTER virtual-ns=%" PRIu64
                         " ticks=%" PRIu64 " hz=32768 width=32",
                         ns, *value);
        }
        return MEMTX_OK;
    }
    /* A value is deliberately not produced on the rejected transaction. */
    return marconi_missing(s, address, size, false, 0);
}

static MemTxResult marconi_missing_write(void *opaque, hwaddr address,
                                        uint64_t value, unsigned size,
                                        MemTxAttrs attrs)
{
    MarconiProbeState *s = opaque;

    if (marconi_uart_write(s, address, value, size)) {
        return MEMTX_OK;
    }
    return marconi_missing(s, address, size, true, value);
}

static const MemoryRegionOps marconi_missing_ops = {
    .read_with_attrs = marconi_missing_read,
    .write_with_attrs = marconi_missing_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static void marconi_probe_init(MachineState *machine)
{
    MarconiProbeState *s = (MarconiProbeState *)machine;
    DeviceState *cpu;
    g_autofree gchar *ap = NULL;
    gsize bytes = 0;
    hwaddr load_base = s->sbl_reset ? MARCONI_SBL_BASE : MARCONI_AP_BASE;
    hwaddr ram_bytes = s->sbl_reset ? MARCONI_SBL_BYTES : MARCONI_AP_BYTES;
    uint32_t vtor = s->sbl_reset ? MARCONI_SBL_VTOR : MARCONI_AP_BASE;
    bool layout_matches;

    if (s->uart_dw_config && !s->uart16550) {
        error_report("uart-dw-config requires uart16550=on");
        exit(EXIT_FAILURE);
    }

    if (!machine->kernel_filename ||
        !g_file_get_contents(machine->kernel_filename, &ap, &bytes, NULL)) {
        error_report("Marconi probe requires an O62M firmware image");
        exit(EXIT_FAILURE);
    }
    if (s->sbl_reset) {
        layout_matches = bytes == 733568 &&
            ldl_le_p(ap + 0x1000) == 0x600c8b30 &&
            ldl_le_p(ap + 0x1004) == 0x60013d71 &&
            ldl_le_p(ap + 0x12d9c) == MARCONI_SBL_VTOR;
    } else {
        layout_matches = bytes >= 0x83221c && bytes <= MARCONI_AP_BYTES &&
            ldl_le_p(ap) == 0x61745d00 && ldl_le_p(ap + 4) == 0x60df21e1 &&
            ldl_le_p(ap + 0x83220c) == MARCONI_AP_BASE;
    }
    if (!layout_matches) {
        error_report("Marconi probe requires the O62M %s vector/VTOR layout",
                     s->sbl_reset ? "SBL" : "AP");
        exit(EXIT_FAILURE);
    }

    memory_region_init_io(&s->missing, OBJECT(machine), &marconi_missing_ops,
                          s, "marconi.unimplemented", UINT64_C(0x100000000));
    memory_region_add_subregion_overlap(get_system_memory(), 0,
                                        &s->missing, -10);
    memory_region_init_ram(&s->ap_ram, NULL,
                           s->sbl_reset ? "marconi.sbl-ram" : "marconi.ap-ram",
                           ram_bytes, &error_fatal);
    memory_region_add_subregion(get_system_memory(), load_base,
                                &s->ap_ram);
    if (s->mmap_image) {
        g_autofree gchar *map = NULL;
        gsize map_bytes = 0;

        if (!g_file_get_contents(s->mmap_image, &map, &map_bytes, NULL) ||
            map_bytes != 1568 || memcmp(map, "tee", 4) ||
            ldl_le_p(map + 20) != 0x63e00000 ||
            memcmp(map + 32, "ap", 3) ||
            ldl_le_p(map + 52) != MARCONI_AP_BASE) {
            error_report("mmap-image requires the original O62M vela_mmap.bin");
            exit(EXIT_FAILURE);
        }
        /* The image itself declares mmap at 0x60000000+0x1000. The
         * remainder is RAM, not invented peripheral register contents. */
        memory_region_init_ram(&s->mmap_ram, NULL, "marconi.mmap-ram",
                               0x1000, &error_fatal);
        memory_region_add_subregion(get_system_memory(), 0x60000000,
                                    &s->mmap_ram);
        rom_add_blob_fixed("marconi.mmap", map, map_bytes, 0x60000000);
        error_report("MARCONI_PROBE_IMAGE mmap=0x60000000 size=%zu",
                     map_bytes);
    }

    s->cpuclk = clock_new(OBJECT(machine), "cpuclk");
    /* Early init stores 500000000 at 0x6153d4b4; no timing claim is made. */
    clock_set_hz(s->cpuclk, 500000000);
    object_initialize_child(OBJECT(machine), "armv7m", &s->armv7m, TYPE_ARMV7M);
    cpu = DEVICE(&s->armv7m);
    qdev_prop_set_string(cpu, "cpu-type", ARM_CPU_TYPE_NAME("cortex-m55"));
    qdev_prop_set_uint32(cpu, "num-irq", 149);
    qdev_prop_set_uint32(cpu, "init-svtor", vtor);
    qdev_prop_set_uint32(cpu, "init-nsvtor", vtor);
    qdev_connect_clock_in(cpu, "cpuclk", s->cpuclk);
    object_property_set_link(OBJECT(cpu), "memory", OBJECT(get_system_memory()),
                             &error_fatal);
    sysbus_realize(SYS_BUS_DEVICE(cpu), &error_fatal);
    armv7m_load_kernel(s->armv7m.cpu, machine->kernel_filename,
                       load_base, ram_bytes);
    if (s->uart16550) {
        object_initialize_child(OBJECT(machine), "uart", &s->uart, TYPE_SERIAL);
        /* AP table 0x6155a948 and 0x60d30f26 select 167 MHz / 2 for the
         * intended 2 Mbps console. This is only a serial-core parameter:
         * the board clock's current state is unknown; TX is rejected. */
        qdev_prop_set_uint32(DEVICE(&s->uart), "baudbase", 83500000 / 16);
        s->uart.irq = qemu_allocate_irq(marconi_uart_irq, s, 0);
        qdev_prop_set_chr(DEVICE(&s->uart), "chardev", serial_hd(0));
        qdev_realize(DEVICE(&s->uart), NULL, &error_fatal);
        qemu_chr_fe_set_handlers(&s->uart.chr, marconi_uart_can_receive, NULL,
                                 NULL, NULL, s, NULL, true);
    }
    error_report("MARCONI_PROBE_RESET core=cortex-m55 stage=%s vtor=0x%08x "
                 "ram=0x%08" HWADDR_PRIx "+0x%08" HWADDR_PRIx " irq-lines=149 "
                 "counter32k=%s uart16550=%s uart-dw-config=%s "
                 "uart-external-irq=57 uart-data-transfer=unsupported "
                 "other-peripherals=unimplemented "
                 "full-watch=false", s->sbl_reset ? "sbl" : "ap", vtor,
                 load_base, ram_bytes, s->counter32k ? "on" : "off",
                 s->uart16550 ? "initialization-subset" : "off",
                 s->uart_dw_config ? "on" : "off");
}

static bool marconi_get_sbl_reset(Object *obj, Error **errp)
{
    return ((MarconiProbeState *)obj)->sbl_reset;
}

static char *marconi_get_mmap_image(Object *obj, Error **errp)
{
    return g_strdup(((MarconiProbeState *)obj)->mmap_image);
}

static void marconi_set_mmap_image(Object *obj, const char *value, Error **errp)
{
    MarconiProbeState *s = (MarconiProbeState *)obj;
    g_free(s->mmap_image);
    s->mmap_image = g_strdup(value);
}

static void marconi_set_sbl_reset(Object *obj, bool value, Error **errp)
{
    ((MarconiProbeState *)obj)->sbl_reset = value;
}

static bool marconi_get_counter32k(Object *obj, Error **errp)
{
    return ((MarconiProbeState *)obj)->counter32k;
}

static void marconi_set_counter32k(Object *obj, bool value, Error **errp)
{
    ((MarconiProbeState *)obj)->counter32k = value;
}

static bool marconi_get_uart16550(Object *obj, Error **errp)
{
    return ((MarconiProbeState *)obj)->uart16550;
}

static void marconi_set_uart16550(Object *obj, bool value, Error **errp)
{
    ((MarconiProbeState *)obj)->uart16550 = value;
}

static bool marconi_get_uart_dw_config(Object *obj, Error **errp)
{
    return ((MarconiProbeState *)obj)->uart_dw_config;
}

static void marconi_set_uart_dw_config(Object *obj, bool value, Error **errp)
{
    ((MarconiProbeState *)obj)->uart_dw_config = value;
}

static void marconi_probe_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "Experimental O62M Marconi AP reset probe (stops on missing MMIO)";
    mc->init = marconi_probe_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m55");
    mc->max_cpus = 1;
    mc->ignore_memory_transaction_failures = false;
    object_class_property_add_bool(oc, "sbl-reset", marconi_get_sbl_reset,
                                   marconi_set_sbl_reset);
    object_class_property_set_description(oc, "sbl-reset",
        "Probe unmodified SBL reset using its verified container/vector mapping");
    object_class_property_add_str(oc, "mmap-image", marconi_get_mmap_image,
                                  marconi_set_mmap_image);
    object_class_property_set_description(oc, "mmap-image",
        "Load the original O62M vela_mmap.bin at its declared RAM address");
    object_class_property_add_bool(oc, "counter32k", marconi_get_counter32k,
                                   marconi_set_counter32k);
    object_class_property_set_description(oc, "counter32k",
        "Enable the observed 32-bit 32768 Hz counter read; all other MMIO fails");
    object_class_property_add_bool(oc, "uart16550", marconi_get_uart16550,
                                   marconi_set_uart16550);
    object_class_property_set_description(oc, "uart16550",
        "Enable basic DW UART initialization; clock and data transfer fail");
    object_class_property_add_bool(oc, "uart-dw-config", marconi_get_uart_dw_config,
                                   marconi_set_uart_dw_config);
    object_class_property_set_description(oc, "uart-dw-config",
        "Enable DW shadow/PTIME configuration with empty FIFOs; data transfer fails");
}

static const TypeInfo marconi_probe_type = {
    .name = MACHINE_TYPE_NAME("azki-marconi-probe"),
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(MarconiProbeState),
    .class_init = marconi_probe_class_init,
};

static void marconi_probe_register_types(void)
{
    type_register_static(&marconi_probe_type);
}
type_init(marconi_probe_register_types)
