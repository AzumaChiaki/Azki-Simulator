/*
 * Partial BES1503 boot peripheral model for the Xiaomi Q66 Vela image.
 * Missing peripherals/commands produce observable bus faults.
 * Flash addresses come from the image's build_info, SRAM aliases from its
 * startup copy tables. SRAM size follows the SDK; PSRAM is provisional.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/units.h"
#include "hw/boards.h"
#include "hw/arm/armv7m.h"
#include "hw/arm/boot.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-clock.h"
#include "system/address-spaces.h"
#include "exec/cpu-common.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "hw/timer/cmsdk-apb-dualtimer.h"
#include "system/reset.h"
#include "system/system.h"
#include "hw/char/pl011.h"
#include "hw/dma/pl080.h"
#include "hw/watchdog/cmsdk-apb-watchdog.h"
#include "hw/irq.h"
#include "qemu/timer.h"
#include "qemu/atomic.h"
#include "ui/console.h"
#include "ui/input.h"
#include "chardev/char-fe.h"
#include "best1503-gpu.h"
#include "best1503-adc.h"
#include "qemu/thread.h"
#include "bes-nand-store.h"
#include "bes-nor-store.h"


typedef struct Best1503State Best1503State;

typedef struct {
    MemoryRegion mmio;
    hwaddr base;
    uint32_t regs[0x64 / 4];
    uint32_t input, pending;
    Best1503State *machine;
} BesGpio;

typedef struct {
    MemoryRegion mmio;
    uint32_t regs[0x2ac / 4];
    Best1503State *machine;
    QEMUTimer *spi_timer;
    QEMUTimer *frame_timer;
    qemu_irq irq;
    QemuConsole *console;
    uint8_t pixels[1024 * 1024 * 2];
    char *frame_file;
    uint8_t *shared_frame;
    uint32_t shared_sequence;
    uint32_t frame_address, frame_pitch;
    unsigned frame_width, frame_height, frame_count;
    bool frame_busy, frame_valid;
    uint32_t status, rx;
    uint8_t command, parameters[32];
    unsigned length;
    bool busy, reading, rx_valid;
} BesLcdc;

typedef struct {
    MemoryRegion mmio;
    qemu_irq irq;
    hwaddr base;
    uint32_t regs[0xac / 4];
    uint32_t pending, abort_source;
    Best1503State *machine;
    uint8_t rx[8];
    unsigned rx_head, rx_count;
} BesI2c;

typedef struct {
    bool enabled, bootloader, slave_reading;
    bool host_down;
    unsigned x, y;
    uint16_t pointer, mem_address;
    uint8_t tx[16], report[10], queue[32][10];
    unsigned tx_count, read_count, queue_head, queue_count;
    uint8_t mode, reply_mode, sleep_mode, mem_type, side_flags;
    uint32_t input_count, ack_count;
    uint16_t health;
    /* The AP acknowledges a report by clearing GPIO pending and briefly
     * reconfiguring GPIO27. A queued report must be presented after that
     * EOI/mask/enable sequence, not synchronously from the ACK. */
    bool irq_rearm_pending;
    QEMUPutMouseEntry *mouse;
} BesTouch;

typedef struct {
    MemoryRegion mmio;
    hwaddr base;
    const uint16_t *offsets;
    unsigned count;
    uint32_t regs[0x444 / 4];
    Best1503State *machine;
} BesBtConfig;

typedef struct {
    MemoryRegion mc_mmio, phy_mmio;
    MemoryRegion *memory;
    uint32_t mc[0x4a0 / 4], phy[0x6c / 4];
    uint32_t mr[256], tx[8], rx[8];
    unsigned tx_count, rx_pos, rx_count;
} BesPsram;

typedef struct {
    MemoryRegion mmio;
    uint32_t regs[0x8c / 4];
    const char *name;
} BesCache;

typedef struct {
    MemoryRegion mmio;
    unsigned controller;
} BesDma2d;

typedef struct {
    MemoryRegion mmio;
    uint32_t regs[0x200 / 4];
    bool aon;
    hwaddr base;
    Best1503State *machine;
} BesCmu;

typedef struct {
    MemoryRegion mmio;
    uint32_t regs[0x384 / 4];
    hwaddr base;
    MemoryRegion *backing;
    uint32_t write_min_offset, write_max_offset;
    uint32_t config_offset, config_bytes;
    uint8_t rx[8192], tx[8192], status[3];
    unsigned rx_pos, rx_len, tx_len;
    uint32_t program_address;
    unsigned program_remaining;
    uint32_t program_start;
    unsigned program_length;
    BesNorStore nor_store;
    bool write_enabled;
    uint8_t burst_wrap;
    uint8_t *nand_data, *nand_spare;
    BesNandStore nand_store;
    uint8_t nand_features[256];
    uint32_t nand_page;
    qemu_irq nand_drq;
    uint8_t nand_cache_data[2112];
    uint16_t nand_column;
    unsigned nand_remaining;
    bool nand_loading;
} BesNor;

typedef struct {
    MemoryRegion mmio;
    uint32_t regs[0x8c / 4];
    uint16_t analog[4][5][256];
    Best1503State *machine;
    uint8_t page[4];
    uint32_t rx[8];
    unsigned head, count;
    unsigned logged_reads;
    int8_t adc_channel[8];
    uint64_t adc_version[8];
} BesIspi;

typedef struct {
    MemoryRegion mmio;
    uint32_t regs[0x8c / 4];
    Best1503State *machine;
    uint8_t rx[8], packet[517];
    unsigned rx_head, rx_count, packet_len;
    uint16_t sensor[0x882 / 2], read_address;
    bool sensor_written[0x882 / 2];
    bool selected, reset_high, awake, read_latched, rx_overrun;
    int64_t ready_at;
    qemu_irq dma_rx, dma_tx;
    QEMUTimer *dma_timer;
} BesSpiConfig;

struct Best1503State {
    MachineState parent;
    ARMv7MState armv7m;
    MemoryRegion flash, flash_exec, flash_nc, flash_nc_exec;
    MemoryRegion sram, sram_exec, bt_ram, bt_ram_exec, bt_patch, bt_dump_ctrl;
    MemoryRegion psram, psram_exec, psram_cached, psram_cached_exec;
    MemoryRegion boot_vectors;
    uint32_t ap_offset, reset_sp, flash_bytes, factory_offset, nv_offset;
    uint32_t config_offset, config_bytes, nand_data_bytes;
    uint32_t nand_data_offset, resource_yaffs, pmu_calibration, panel_te_pin;
    uint32_t panel_power0, panel_power1, panel_power2, panel_id;
    uint32_t touch_i2c, touch_power_pin, touch_reset_pin, touch_irq_pin;
    uint32_t touch_module_id, touch_health_format;
    BesCmu cmu[2];
    BesCache cache[2];
    MemoryRegion iomux, psc;
    MemoryRegion gpio_summary;
    MemoryRegion gpu_probe_mmio;
    uint32_t gpu_probe_control, gpu_probe_mask;
    uint32_t gpu_command_address;
    bool gpu_probe;
    bool gpu_id_fixture;
    bool gpu_no_render;
    bool gpu_completion_fixture, gpu_busy;
    bool gpu_software_renderer;
    uint32_t gpu_sw_regs[BES_GPU_REGS];
    BesGpuWork *gpu_work;
    unsigned gpu_submission_id, gpu_pending_id;
    uint32_t gpu_status, gpu_pending_event;
    QEMUTimer *gpu_timer;
    qemu_irq gpu_irq;
    MemoryRegion uart_ext[4];
    PL080State dma[2];
    BesDma2d dma_2d[2];
    BesNor nor[2];
    MemoryRegion nand_cache, nand_cache_alias[3];
    char *resource_image;
    char *data_image;
    char *data_spare_image;
    bool data_writeback;
    char *factory_image;
    char *nor_image;
    bool nor_writeback, nor_exit_registered;
    Notifier nor_exit;
    BesIspi ispi;
    BesSpiConfig spi0, spi1;
    BesPsram psram_ctrl;
    Clock *cpuclk;
    Clock *slowclk, *fastclk;
    CMSDKAPBDualTimer timer[4];
    CMSDKAPBWatchdog watchdog[2];
    BesI2c i2c[8];
    BesTouch touch;
    BesBtConfig bt_config[4];
    BesGpio gpio[3];
    BesLcdc lcdc;
    bool panel_id_fixture;
    bool lcd_frame_capture;
    bool panel_te_fixture, panel_te_enabled, panel_awake, panel_display_on;
    bool panel_partial_mode, panel_idle_mode;
    bool panel_te_high;
    QEMUTimer *panel_te_timer;
    QEMUTimer *touch_irq_timer;
    qemu_irq panel_te_irq;
    bool gh3x2x_fixture;
    bool spi0_unconnected;
    bool power_adc_fixture;
    QemuMutex adc_lock;
    BesAdcInput adc_input;
    bool gh_reg0724_set;
    uint32_t gh_reg0724;
    bool bt_peer;
    bool bt_tx_capture;
    bool bt_hci_reset_fixture, bt_rx_pending, bt_rx_owned;
    bool bt_hci_reject_fixture, bt_hci_stack_started;
    uint16_t bt_response_queue[16];
    unsigned bt_response_head, bt_response_count;
    char *bt_hci_chardev;
    CharBackend bt_hci;
    uint8_t bt_h4_input[1029], bt_h4_queue[32][1029];
    unsigned bt_h4_used, bt_h4_length[32], bt_h4_head, bt_h4_count;
    uint32_t revision_id;
    QEMUTimer *bt_wake_timer;
    QEMUTimer *bt_tx_timer;
    QEMUTimer *bt_rx_timer;
    qemu_irq bt_tx_irq;
    qemu_irq bt_rx_irq;
    bool bt_tx_done;
    int32_t bt_iq_sample[2];
};

static void spi0_controller_reset(BesSpiConfig *spi);

/* Experimental peer wake protocol only; this does not execute controller ROM
 * or claim HCI/RF support. A 100 us delay is a model choice, not chip timing.
 */
static bool bt_peer_powered(Best1503State *s)
{
    /* Q66 enables bit 8 here; SDK's BTAON enum says bit 9. */
    return s->bt_peer &&
           (s->cmu[1].regs[0xcc / 4] & 0x48) == 0x48 &&
           (s->cmu[1].regs[0x04 / 4] & (1 << 8));
}

static bool lcdc_frame_range(uint32_t addr, uint64_t length);

static void bt_peer_wake(void *opaque)
{
    Best1503State *s = opaque;
    BesCmu *cmu = &s->cmu[0];
    if (bt_peer_powered(s) &&
        (s->bt_config[1].regs[0xa0 / 4] & (1 << 19)) &&
        (cmu->regs[0xa0 / 4] & 8)) {
        cmu->regs[0xa0 / 4] &= ~8u;
        cmu->regs[0xa4 / 4] = cmu->regs[0xa0 / 4];
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES experimental BT peer acknowledged wake (no ROM/HCI)\n");
    }
}

/* Capture-only transport: consume a validated H4 command list and return
 * buffer ownership through the original AP IRQ handler. Commands are logged,
 * not executed; no HCI Command Complete/Status event is generated.
 */
static void bt_tx_update_irq(Best1503State *s)
{
    qemu_set_irq(s->bt_tx_irq,
                 s->bt_tx_done && (s->cmu[0].regs[0xa0 / 4] & (1 << 15)));
}

static void bt_rx_update_irq(Best1503State *s)
{
    qemu_set_irq(s->bt_rx_irq,
                 s->bt_rx_pending && (s->cmu[0].regs[0xa0 / 4] & (1 << 13)));
}

/* Single-command endpoint fixture. The peer owns a descriptor and seven
 * event bytes in the tail of its emulated RAM until AP receive acknowledgement.
 * No AP instructions, return values or HCI state variables are modified.
 */
static void bt_peer_reset_response(void *opaque)
{
    Best1503State *s = opaque;
    if (s->bt_hci_chardev) {
        if (!bt_peer_powered(s) || s->bt_rx_owned || !s->bt_h4_count) { return; }
        uint32_t head;
        cpu_physical_memory_read(0xc0004000, &head, 4);
        if (head) { return; }
        unsigned index = s->bt_h4_head, length = s->bt_h4_length[index];
        uint32_t descriptor[] = {0, 0, cpu_to_le32(length), cpu_to_le32(0xc000f800)};
        cpu_physical_memory_write(0xc000ff00, descriptor, sizeof(descriptor));
        cpu_physical_memory_write(0xc000f800, s->bt_h4_queue[index], length);
        head = cpu_to_le32(0xc000ff00);
        cpu_physical_memory_write(0xc0004000, &head, 4);
        s->bt_h4_head = (index + 1) % 32; s->bt_h4_count--;
        s->bt_rx_owned = s->bt_rx_pending = true;
        bt_rx_update_irq(s);
        char hex[1029 * 3 + 1];
        for (unsigned i = 0; i < length; i++) {
            snprintf(hex + i * 3, 4, "%02x ", s->bt_h4_queue[index][i]);
        }
        qemu_log_mask(LOG_GUEST_ERROR, "BES backend H4 RX delivered: %s\n", hex);
        return;
    }
    uint8_t event[] = { 4, 0x0e, 4, 1, 3, 0x0c, 0 };
    if (s->bt_hci_reject_fixture && s->bt_response_count) {
        uint16_t opcode = s->bt_response_queue[s->bt_response_head];
        event[4] = opcode; event[5] = opcode >> 8;
        event[6] = opcode == 0x0c03 ? 0 : 1; /* Unknown HCI Command. */
    }
    uint32_t head, descriptor[] = {
        0, 0, cpu_to_le32(sizeof(event)), cpu_to_le32(0xc000ff20)
    };
    if (!s->bt_hci_reset_fixture || !bt_peer_powered(s) || s->bt_rx_owned) {
        return;
    }
    cpu_physical_memory_read(0xc0004000, &head, sizeof(head));
    if (head) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES BT Reset RX rejected: shared receive list already occupied\n");
        return;
    }
    cpu_physical_memory_write(0xc000ff00, descriptor, sizeof(descriptor));
    cpu_physical_memory_write(0xc000ff20, event, sizeof(event));
    head = cpu_to_le32(0xc000ff00);
    cpu_physical_memory_write(0xc0004000, &head, sizeof(head));
    s->bt_rx_owned = s->bt_rx_pending = true;
    if (s->bt_hci_reject_fixture && s->bt_response_count) {
        s->bt_response_head = (s->bt_response_head + 1) % 16;
        s->bt_response_count--;
    }
    bt_rx_update_irq(s);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "BES experimental BT RX event: 04 0e 04 01 %02x %02x %02x (%s fixture)\n",
                  event[4], event[5], event[6], event[6] ? "Reject" : "Reset");
}

static int bt_hci_can_read(void *opaque)
{
    Best1503State *s = opaque;
    return s->bt_h4_count < 32 ? 1 : 0;
}

static void bt_hci_receive(void *opaque, const uint8_t *data, int size)
{
    Best1503State *s = opaque;
    for (int i = 0; i < size; i++) {
        if (s->bt_h4_used == sizeof(s->bt_h4_input)) {
            error_report("BES Bluetooth H4 packet exceeds supported ACL length");
            exit(1);
        }
        s->bt_h4_input[s->bt_h4_used++] = data[i];
        unsigned header;
        if (s->bt_h4_input[0] == 4) { header = 3; }
        else if (s->bt_h4_input[0] == 2) { header = 5; }
        else {
            error_report("BES Bluetooth unsupported input H4 type %u", s->bt_h4_input[0]);
            exit(1);
        }
        if (s->bt_h4_used < header) { continue; }
        unsigned length = header + (header == 3 ? s->bt_h4_input[2] :
            s->bt_h4_input[3] | (s->bt_h4_input[4] << 8));
        if (length > sizeof(s->bt_h4_input)) {
            error_report("BES Bluetooth excessive input packet length %u", length);
            exit(1);
        }
        if (s->bt_h4_used == length) {
            unsigned tail = (s->bt_h4_head + s->bt_h4_count++) % 32;
            memcpy(s->bt_h4_queue[tail], s->bt_h4_input, length);
            s->bt_h4_length[tail] = length; s->bt_h4_used = 0;
            if (!s->bt_rx_owned && !timer_pending(s->bt_rx_timer)) {
                timer_mod(s->bt_rx_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
            }
        }
    }
}

static bool bt_capture_read(Best1503State *s, uint32_t addr, void *dest,
                            unsigned len, bool ram_only)
{
    /* No arbitrary MMIO reads from malformed guest pointers. */
    if (lcdc_frame_range(addr, len) ||
        (addr >= 0x20000000 && (uint64_t)addr + len <= 0x20160000) ||
        (addr >= 0x00200000 && (uint64_t)addr + len <= 0x00360000) ||
        (!ram_only && ((addr >= 0x0c000000 &&
                       (uint64_t)addr + len <= 0x0d000000) ||
                      (addr >= 0x2c000000 &&
                       (uint64_t)addr + len <= 0x2d000000)))) {
        cpu_physical_memory_read(addr, dest, len);
        return true;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "BES BT buffer rejected: addr=0x%08x len=%u ram=%d\n", addr, len, ram_only);
    return false;
}

static void bt_peer_capture_tx(void *opaque)
{
    Best1503State *s = opaque;
    uint32_t head_le, msg_le[4], seen[3];
    uint8_t packets[3][1029];
    unsigned lengths[3], count = 0;
    uint32_t head, message;

    if (!s->bt_tx_capture || !bt_peer_powered(s) ||
        !(s->cmu[0].regs[0xa0 / 4] & 4) || s->bt_tx_done) {
        return;
    }
    cpu_physical_memory_read(0xc0004004, &head_le, sizeof(head_le));
    head = le32_to_cpu(head_le);
    if ((head & 3) || !bt_capture_read(s, head, &head_le, 4, true)) {
        goto invalid;
    }
    message = le32_to_cpu(head_le);
    while (message) {
        uint32_t type, len, data;
        if (count == ARRAY_SIZE(packets) || (message & 3) ||
            !bt_capture_read(s, message, msg_le, sizeof(msg_le), true)) {
            goto invalid;
        }
        for (unsigned i = 0; i < count; i++) {
            if (seen[i] == message) {
                goto invalid;
            }
        }
        seen[count] = message;
        type = le32_to_cpu(msg_le[1]);
        len = le32_to_cpu(msg_le[2]);
        data = le32_to_cpu(msg_le[3]);
        if (type != 0 || len < 4 || len > sizeof(packets[0]) ||
            !bt_capture_read(s, data, packets[count], len, false) ||
            !((packets[count][0] == 1 && len == 4 + packets[count][3]) ||
              (s->bt_hci_chardev && len >= 5 && packets[count][0] == 2 &&
               len == 5 + packets[count][3] + (packets[count][4] << 8)))) {
            goto invalid;
        }
        lengths[count++] = len;
        message = le32_to_cpu(msg_le[0]);
    }
    if (!count) {
        goto invalid;
    }
    for (unsigned i = 0; i < count; i++) {
        char hex[1029 * 3 + 1];
        for (unsigned j = 0; j < lengths[i]; j++) {
            snprintf(hex + j * 3, 4, "%02x ", packets[i][j]);
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES experimental BT TX captured: %s(transport only)\n",
                      hex);
        if (s->bt_hci_chardev) {
            uint16_t opcode = packets[i][1] | (packets[i][2] << 8);
            s->bt_hci_stack_started |= packets[i][0] == 1 && opcode == 0x0c03;
            if (s->bt_hci_stack_started) {
                int sent = qemu_chr_fe_write(&s->bt_hci, packets[i], lengths[i]);
                if (sent != lengths[i]) {
                    error_report("BES H4 backend send failed: %d/%u", sent, lengths[i]);
                    exit(1);
                }
            }
        } else if (s->bt_hci_reject_fixture) {
            uint16_t opcode = packets[i][1] | (packets[i][2] << 8);
            s->bt_hci_stack_started |= opcode == 0x0c03;
            if (s->bt_hci_stack_started && s->bt_response_count < 16) {
                unsigned tail = (s->bt_response_head + s->bt_response_count++) % 16;
                s->bt_response_queue[tail] = opcode;
                if (!s->bt_rx_owned && !timer_pending(s->bt_rx_timer)) {
                    timer_mod(s->bt_rx_timer,
                              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
                }
            }
        } else if (s->bt_hci_reset_fixture && lengths[i] == 4 &&
            packets[i][1] == 3 && packets[i][2] == 0x0c) {
            if (s->bt_rx_owned || timer_pending(s->bt_rx_timer)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "BES BT Reset RX rejected: peer response still owned\n");
            } else {
                timer_mod(s->bt_rx_timer,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
            }
        }
    }
    s->cmu[0].regs[0xa0 / 4] &= ~4u;
    s->cmu[0].regs[0xa4 / 4] = s->cmu[0].regs[0xa0 / 4];
    s->bt_tx_done = true;
    bt_tx_update_irq(s);
    return;

invalid:
    qemu_log_mask(LOG_GUEST_ERROR,
                  "BES experimental BT TX rejected: invalid/unsupported command list\n");
}

static int32_t bt_signed14(uint32_t value)
{
    return (int32_t)((value & 0x3fff) ^ 0x2000) - 0x2000;
}

/* Ideal zero-offset I/Q fixture: residual follows the correction fields.
 * This exercises the original search algorithm, not real RF measurement.
 * The original routine consumes signed 22-bit samples and divides by eight.
 * Samples complete synchronously; physical acquisition timing is not modeled.
 */
static void bt_peer_iq_complete(void *opaque)
{
    Best1503State *s = opaque;
    for (unsigned i = 0; i < 2; i++) {
        s->bt_config[2].regs[(0x338 + i * 4) / 4] =
            (uint32_t)s->bt_iq_sample[i] & 0x3fffff;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "BES experimental ideal IQ sample: I=%d Q=%d (no RF)\n",
                  s->bt_iq_sample[0], s->bt_iq_sample[1]);
}

/* Configuration storage only. The SDK identifies digital/RF/modem banks;
 * Q66's original init at 0x0c92a194 and modem routine 0x0c92920c differ
 * from its BEST1306-derived implementation. Only inspected RMW/config
 * offsets are accepted. There is no radio, controller clock, ROM or CPU,
 * with optional experimental wake/IQ responses described above.
 * Reset defaults are provisional zero, not measured silicon values.
 */
static bool bt_config_supported(BesBtConfig *b, hwaddr offset)
{
    if (b->base == 0xd0330000 && offset == 0x54 &&
        (b->machine->bt_hci_reset_fixture || b->machine->bt_hci_chardev)) {
        return true;
    }
    /* Original 0x0c92b452..466 fills 79 channel gain/phase words. */
    if (b->base == 0xd0310000 && offset <= 78 * 4 && !(offset & 3)) {
        return true;
    }
    for (unsigned i = 0; i < b->count; i++) {
        if (offset == b->offsets[i]) {
            return true;
        }
    }
    return false;
}

static MemTxResult bt_config_read(void *opaque, hwaddr offset,
                                  uint64_t *value, unsigned size,
                                  MemTxAttrs attrs)
{
    BesBtConfig *b = opaque;
    if (!bt_config_supported(b, offset)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES unsupported BT config read at 0x%08" HWADDR_PRIx "\n",
                      b->base + offset);
        return MEMTX_ERROR;
    }
    /* The original wrapper saves result registers before acquisition.
     * Reset/stale values are readable, not evidence of a valid sample.
     */
    *value = b->regs[offset / 4];
    return MEMTX_OK;
}

static MemTxResult bt_config_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size,
                                   MemTxAttrs attrs)
{
    BesBtConfig *b = opaque;
    if (!bt_config_supported(b, offset)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES unsupported BT config write at 0x%08" HWADDR_PRIx "\n",
                      b->base + offset);
        return MEMTX_ERROR;
    }
    if (b->base == 0xd0330000 && offset == 0x54 && value != 4) {
        return MEMTX_ERROR;
    }
    if (b->base == 0xd0350000 && (offset == 0x338 || offset == 0x33c)) {
        return MEMTX_OK; /* Measurement results are read-only. */
    }
    if (b->base == 0xd0330000 && offset == 0x54 && (value & 4) &&
        b->machine->bt_rx_owned) {
        Best1503State *s = b->machine;
        uint32_t zero = 0;
        s->bt_rx_owned = s->bt_rx_pending = false;
        if (s->bt_response_count || s->bt_h4_count) {
            timer_mod(s->bt_rx_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
        }
        cpu_physical_memory_write(0xc0004000, &zero, sizeof(zero));
        bt_rx_update_irq(s);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES experimental BT RX acknowledged by AP\n");
        if (s->bt_hci_chardev) { qemu_chr_fe_accept_input(&s->bt_hci); }
    }
    if (b->base == 0xd0330000 && offset == 0x54) {
        return MEMTX_OK;
    }
    if (b->base == 0xd0350000 && offset == 0x334 &&
        !(b->regs[offset / 4] & (1 << 30)) && (value & (1 << 30)) &&
        bt_peer_powered(b->machine) &&
        (b->machine->bt_config[0].regs[0] & (1u << 31))) {
        Best1503State *s = b->machine;
        bool gain = b->regs[0x250 / 4] & (1 << 31);
        unsigned channel = s->bt_config[0].regs[0] & 0x7f;
        if (gain && channel > 78) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "BES unsupported IQ channel %u\n", channel);
            return MEMTX_ERROR;
        }
        uint32_t correction = gain ? s->bt_config[3].regs[channel] :
                                    b->regs[0x250 / 4];
        s->bt_iq_sample[0] = bt_signed14(correction) * 8;
        s->bt_iq_sample[1] = bt_signed14(correction >> (gain ? 16 : 14)) * 8;
        bt_peer_iq_complete(s);
    }
    if (b->base == 0xd0330000 && (offset == 0xa0 || offset == 0xa4)) {
        /* BT interrupt-mask SET/CLR in reg_btcmu_best1503.h. Q66 uses
         * the upper (SENS2BT) channel-1 mask, bit 19. Peer status is
         * read-only; no peer signal or IRQ is generated by this model.
         */
        uint32_t bits = value & 0x000f000f;
        b->regs[0xa0 / 4] = offset == 0xa0 ?
            b->regs[0xa0 / 4] | bits : b->regs[0xa0 / 4] & ~bits;
        b->regs[0xa4 / 4] = b->regs[0xa0 / 4];
    } else {
        b->regs[offset / 4] = value;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "BES BT config 0x%08" HWADDR_PRIx " = 0x%08x\n",
                  b->base + offset, b->regs[offset / 4]);
    return MEMTX_OK;
}

static const MemoryRegionOps bt_config_ops = {
    .read_with_attrs = bt_config_read,
    .write_with_attrs = bt_config_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
};

/* Register offsets from the BEST1503 CMU headers. Clocks/reset requests
 * settle immediately. This does not reproduce power islands or PLL timing.
 * RAM_CFG only sets electrical RAM timing; directly backed RAM is unaffected.
 * CHIP_ID/revision and register reset defaults are provisional zero values.
 */
static MemTxResult cmu_read(void *opaque, hwaddr offset, uint64_t *value,
                            unsigned size, MemTxAttrs attrs)
{
    BesCmu *cmu = opaque;
    hwaddr reg = offset & ~3;
    hwaddr sem_base = cmu->aon ? 0x24 : 0xc0;
    if (reg >= (cmu->aon ? 0x10c : 0x184)) {
        qemu_log_mask(LOG_GUEST_ERROR, "BES unsupported CMU read at 0x%08" HWADDR_PRIx "\n",
                      cmu->base + offset);
        return MEMTX_ERROR;
    }
    /* Hardware semaphores: a successful read atomically acquires one.
     * Q66's acquire/release functions are at 0x00275894/0x002758a8.
     * State stores ownership; MEMSC_STATUS exposes available semaphore bits.
     */
    if (reg >= sem_base && reg < sem_base + 16) {
        *value = !cmu->regs[reg / 4];
        cmu->regs[reg / 4] = 1;
        return MEMTX_OK;
    }
    if (reg == sem_base + 16) {
        *value = 0;
        for (unsigned i = 0; i < 4; i++) {
            *value |= (!cmu->regs[sem_base / 4 + i]) << i;
        }
        return MEMTX_OK;
    }
    uint32_t result = cmu->regs[reg / 4];
    if (!cmu->aon && (reg == 0xa0 || reg == 0xa4) &&
        cmu->machine->bt_rx_pending) {
        result |= 1 << 17;
        if (result & (1 << 13)) {
            result |= 1 << 21;
        }
    }
    if (!cmu->aon && (reg == 0xa0 || reg == 0xa4) &&
        cmu->machine->bt_tx_done) {
        result |= 1 << 19;
        if (result & (1 << 15)) {
            result |= 1 << 23;
        }
    }
    *value = (result >> ((offset & 3) * 8)) &
             (UINT32_MAX >> (32 - size * 8));
    return MEMTX_OK;
}

static MemTxResult cmu_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size, MemTxAttrs attrs)
{
    BesCmu *cmu = opaque;
    hwaddr reg = offset & ~3;
    hwaddr sem_base = cmu->aon ? 0x24 : 0xc0;
    uint32_t bits = value << ((offset & 3) * 8);
    uint32_t mask = (UINT32_MAX >> (32 - size * 8)) << ((offset & 3) * 8);
    if (reg >= (cmu->aon ? 0x10c : 0x184)) {
        qemu_log_mask(LOG_GUEST_ERROR, "BES unsupported CMU write at 0x%08" HWADDR_PRIx "\n",
                      cmu->base + offset);
        return MEMTX_ERROR;
    }
    if (reg >= sem_base && reg < sem_base + 16) {
        if (bits & 1) {
            cmu->regs[reg / 4] = 0;
        }
        return MEMTX_OK;
    }
    if (reg == sem_base + 16) {
        return MEMTX_OK;
    }
    /* Q66 .071's SPI module table at runtime 0x200778f8 gives SPI0 APB
     * reset ID 0x27. hal_cmu_reset_pulse() writes PRESET_PULSE bit 7;
     * PRESET_SET uses the same bit. The original receive driver pulses
     * this reset to discard stale RX bytes before starting paired DMA. */
    if (!cmu->aon && (reg == 0x2c || reg == 0x3c) && (bits & (1u << 7))) {
        spi0_controller_reset(&cmu->machine->spi0);
    }
    /* Main ISIRQ_SET/CLR are write-one operations, not plain storage.
     * Request bits 0..4 and masks 9..16/25 come from reg_cmu_best1503.h.
     * The optional wake peer acknowledges DATA1_IND (bit 3) asynchronously.
     * With it disabled, the request persists until software clears it.
     * Experimental capture-only TX-done status is read-only and IRQ-backed.
     */
    if (!cmu->aon && (reg == 0xa0 || reg == 0xa4)) {
        uint32_t writable = bits & 0x0201fe1f;
        cmu->regs[0xa0 / 4] = reg == 0xa0 ?
            cmu->regs[0xa0 / 4] | writable :
            cmu->regs[0xa0 / 4] & ~writable;
        cmu->regs[0xa4 / 4] = cmu->regs[0xa0 / 4];
        if (reg == 0xa4 && (bits & 1)) {
            cmu->machine->bt_tx_done = false;
        }
        bt_tx_update_irq(cmu->machine);
        bt_rx_update_irq(cmu->machine);
        if (reg == 0xa0 && (bits & 4) && cmu->machine->bt_tx_capture &&
            !timer_pending(cmu->machine->bt_tx_timer)) {
            timer_mod(cmu->machine->bt_tx_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
        }
        if (reg == 0xa0 && (bits & 8)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "BES MCU-to-BT wake request pending\n");
            if (cmu->machine->bt_peer &&
                !timer_pending(cmu->machine->bt_wake_timer)) {
                timer_mod(cmu->machine->bt_wake_timer,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
            }
        }
        return MEMTX_OK;
    }
    /* Paired enable/disable registers: write one sets/clears enable state. */
    const unsigned aon_enable[] = {0x04, 0x40, 0xa0, 0xe0};
    /* 0x168/16c and 0x174/178 are additional Q66 register banks absent
     * from the BEST1306-derived header, inferred from boot writes at
     * 0x0c0c1386 and 0x0c0c1390. Their interpretation is provisional.
     */
    const unsigned main_enable[] = {0x00, 0x08, 0x10, 0x60, 0x100, 0x140, 0x168};
    const unsigned *enable = cmu->aon ? aon_enable : main_enable;
    unsigned count = cmu->aon ? ARRAY_SIZE(aon_enable) : ARRAY_SIZE(main_enable);
    for (unsigned i = 0; i < count; i++) {
        unsigned en = enable[i];
        if (reg == en || reg == en + 4) {
            cmu->regs[en / 4] = reg == en ? cmu->regs[en / 4] | bits :
                                           cmu->regs[en / 4] & ~bits;
            cmu->regs[(en + 4) / 4] = cmu->regs[en / 4];
            return MEMTX_OK;
        }
    }
    /* Reset SET asserts reset, CLR releases it. Both read aliases report
     * released bits: hal_cmu_reset_get_status() reads SET and interprets
     * one as HAL_CMU_RST_CLR (Q66 .071 runtime 0x0026e938, also the SDK).
     * Inverting SET makes hal_cmu_reset_pulse() skip the actual pulse. */
    const unsigned aon_reset[] = {0x10, 0xc8, 0xd8};
    const unsigned main_reset[] = {0x34, 0x3c, 0x44, 0x150, 0x174};
    const unsigned *reset = cmu->aon ? aon_reset : main_reset;
    count = cmu->aon ? ARRAY_SIZE(aon_reset) : ARRAY_SIZE(main_reset);
    for (unsigned i = 0; i < count; i++) {
        unsigned set = reset[i];
        if (reg == set || reg == set + 4) {
            cmu->regs[(set + 4) / 4] = reg == set ? cmu->regs[(set + 4) / 4] & ~bits :
                                                  cmu->regs[(set + 4) / 4] | bits;
            cmu->regs[set / 4] = cmu->regs[(set + 4) / 4];
            return MEMTX_OK;
        }
    }
    if (cmu->aon && reg == 0) { /* CHIP_ID is read-only */
        return MEMTX_OK;
    }
    cmu->regs[reg / 4] = (cmu->regs[reg / 4] & ~mask) | (bits & mask);
    if (cmu->aon && reg == 0x5c) {
        qemu_log_mask(LOG_GUEST_ERROR, "BES RAM_CFG = 0x%08x (RAM_EMA=%u)\n",
                      cmu->regs[reg / 4], cmu->regs[reg / 4] & 7);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps cmu_ops = {
    .read_with_attrs = cmu_read,
    .write_with_attrs = cmu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* QEMU's directly backed memory is coherent. Cache maintenance therefore
 * completes synchronously; enable/configuration registers retain their state.
 * This models the software interface in reg_cache.h, not cache performance.
 */
static uint64_t cache_read(void *opaque, hwaddr offset, unsigned size)
{
    BesCache *cache = opaque;
    return cache->regs[offset / 4];
}

static void cache_write(void *opaque, hwaddr offset, uint64_t value,
                        unsigned size)
{
    BesCache *cache = opaque;
    switch (offset) {
    case 0x04: /* INVALID_ALL */
    case 0x0c: /* WRITE_BUFFER_FLUSH */
    case 0x18: /* CLEAN_INVLD_TRIG/LINE */
    case 0x34: /* CLEAN_ALL */
    case 0x38: /* CLEAN_INVLD_ALL */
    case 0x40: /* CLEAN_TRIG/SET */
    case 0x80: /* CLEAN_LINE */
    case 0x84: /* INVALID_LINE */
    case 0x88: /* INVALID_SET */
        cache->regs[offset / 4] = 0;
        break;
    case 0x30: /* STATUS: no outstanding transactions */
        break;
    default:
        cache->regs[offset / 4] = value;
        if (offset == 0) {
            qemu_log_mask(LOG_GUEST_ERROR, "BES %s CACHE_EN = 0x%08x\n",
                          cache->name, cache->regs[0]);
        }
    }
}

static const MemoryRegionOps cache_ops = {
    .read = cache_read,
    .write = cache_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* BES SDK platform/hal/reg_dma.h places eight 2D channel blocks at
 * 0x200 + channel * 0x20, after the standard PL080 channel bank. Each
 * block has SRCX/SRCY/DSTX/DSTY/CTRL/SRC_INC/DST_INC at offsets 0..0x18.
 * In hal_dma.c, hal_dma_sg_start() passes NULL for both 2D configurations,
 * clearing CTRL.SRC_EN and CTRL.DST_EN with separate read/modify/writes.
 * Support this disabled state and register clears without pretending to
 * implement 2D addressing. All reads stay zero; accepted writes cannot
 * change state, so this overlay needs no reset hook.
 */
static MemTxResult dma_2d_read(void *opaque, hwaddr offset, uint64_t *value,
                             unsigned size, MemTxAttrs attrs)
{
    BesDma2d *ext = opaque;

    if ((offset & 0x1f) == 0x1c) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES DMA%u 2D read: reserved offset=0x%03x\n",
                      ext->controller, (unsigned)offset + 0x200);
        return MEMTX_ERROR;
    }
    *value = 0;
    return MEMTX_OK;
}

static MemTxResult dma_2d_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size, MemTxAttrs attrs)
{
    BesDma2d *ext = opaque;
    unsigned reg = offset & 0x1f;

    if (reg == 0x10 && (value & 3)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES DMA%u 2D transfer unsupported: channel=%u "
                      "src_enable=%u dst_enable=%u CTRL=0x%08x\n",
                      ext->controller, (unsigned)(offset / 0x20),
                      (unsigned)value & 1, ((unsigned)value >> 1) & 1,
                      (unsigned)value);
        return MEMTX_ERROR;
    }
    if (reg == 0x1c || value != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES DMA%u 2D write unsupported: offset=0x%03x "
                      "value=0x%08x (only disabled register clears supported)\n",
                      ext->controller, (unsigned)offset + 0x200,
                      (unsigned)value);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps dma_2d_ops = {
    .read_with_attrs = dma_2d_read,
    .write_with_attrs = dma_2d_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
};

/* Synchronous boot-time NOR transactions. The synthetic GD25LQ64 identity
 * C8:60:17 occurs in the target image's supported-device table at 0xbcd0.
 * It represents an 8 MiB chip, or 16 MiB with dual-chip mode. This is a
 * modeled device choice, not identification of the watch's physical flash.
 * Only tail NV and an optional separate config area permit erase/program; application pages
 * and unimplemented commands deliberately bus-fault. NAND instance data may
 * opt into bounded data/OOB writeback; AP and resource files stay read-only.
 */
static void nand_update_drq(BesNor *nor)
{
    qemu_set_irq(nor->nand_drq, nor->rx_pos < nor->rx_len || nor->nand_remaining);
}

static bool nor_writable(BesNor *nor, uint32_t address, unsigned count)
{
    uint64_t end = (uint64_t)address + count;
    off_t compact_offset;
    return count && nor->backing && end <= memory_region_size(nor->backing) &&
        (!nor->nor_store.enabled ||
         bes_nor_layout_translate(&nor->nor_store.layout, address, count, &compact_offset)) &&
        ((address >= nor->write_min_offset && end <= nor->write_max_offset) ||
         (nor->config_bytes && address >= nor->config_offset &&
          end <= (uint64_t)nor->config_offset + nor->config_bytes));
}

/* Fatal storage failures have exactly one marker, including during atexit. */
static G_NORETURN void nor_store_failure(BesNor *nor, const char *reason)
{
    nor->nor_store.failed = true;
    error_report("NOR instance writeback failed: %s", reason);
    bes_nor_store_close(&nor->nor_store);
    fflush(NULL);
    _exit(1);
}

static void nor_flush(BesNor *nor, uint32_t address, unsigned count)
{
    if (!bes_nor_store_flush(&nor->nor_store, address, count,
                            (uint8_t *)memory_region_get_ram_ptr(nor->backing) + address)) {
        nor_store_failure(nor, "private NOR write or fsync failed");
    }
}

static void nor_check_complete(BesNor *nor)
{
    if (nor->nor_store.enabled && nor->program_remaining) {
        nor_store_failure(nor, "incomplete NOR program transaction");
    }
}

static void best1503_nor_exit(Notifier *notifier, void *unused)
{
    Best1503State *s = container_of(notifier, Best1503State, nor_exit);
    nor_check_complete(&s->nor[0]);
    bes_nor_store_close(&s->nor[0].nor_store);
}

static bool nor_command(BesNor *nor, uint32_t value)
{
    uint8_t cmd = value;
    unsigned len = (nor->regs[1] >> 12) & 0x1fff;
    bool extended = nor->regs[1] & 1;
    bool dual = nor->regs[0x204 / 4] & (1 << 19);
    bool quad_rx = ((value >> 11) & 3) == 2;
    uint8_t data[3] = {0};
    unsigned data_len = 1;
    if (extended) {
        len = value & (1 << 8) ? (value >> 13) & 0x1fff : 0;
    }
    if (nor->nand_data) {
        uint32_t address = (value >> 8) | (nor->regs[0x30 / 4] << 24);
        qemu_log_mask(CPU_LOG_INT,
                      "BES NAND command: cmd=%02x value=%08x address=%08x len=%u tx=%u\n",
                      cmd, value, address, len, nor->tx_len);
        nor->rx_pos = nor->rx_len = 0;
        nand_update_drq(nor);
        switch (cmd) {
        case 0x9f:
            if (len != 3) { return false; }
            /* GD5F4GQ4: an explicit 512 MiB device from the AP support table. */
            nor->rx[0] = 0xc8; nor->rx[1] = 0xf4; nor->rx[2] = 0xc8;
            nor->rx_len = 3;
            break;
        case 0xff:
            nor->nand_page = 0;
            nor->nand_loading = nor->write_enabled = false;
            nor->nand_remaining = 0;
            nand_update_drq(nor);
            break;
        case 0x06: nor->write_enabled = true; break;
        case 0x04: nor->write_enabled = false; break;
        case 0x0f:
            if (len != 1 || nor->tx_len < 1) { return false; }
            nor->rx[0] = nor->tx[0] == 0xc0 ? (nor->write_enabled ? 2 : 0) :
                         nor->nand_features[nor->tx[0]];
            nor->rx_len = 1;
            break;
        case 0x1f:
            if (nor->tx_len != 2 || len) { return false; }
            nor->nand_features[nor->tx[0]] = nor->tx[1];
            break;
        case 0x13:
            if (address >= 512 * MiB / 2048) { return false; }
            nor->nand_page = address;
            break;
        case 0x02:
        case 0x32:
            if (address >= 2112 || !len || address + len > 2112 || nor->tx_len > len) { return false; }
            memset(nor->nand_cache_data, 0xff, sizeof(nor->nand_cache_data));
            nor->nand_column = address; nor->nand_loading = true;
            memcpy(nor->nand_cache_data + address, nor->tx, nor->tx_len);
            nor->nand_column += nor->tx_len;
            nor->nand_remaining = len - nor->tx_len;
            break;
        case 0x10:
            if (!nor->write_enabled || !nor->nand_loading || nor->nand_remaining ||
                address < nor->write_min_offset / 2048 || address >= nor->write_max_offset / 2048) { return false; }
            for (unsigned i = 0; i < 2048; i++) {
                nor->nand_data[address * 2048 + i] &= nor->nand_cache_data[i];
            }
            for (unsigned i = 0; i < 64; i++) {
                nor->nand_spare[address * 64 + i] &= nor->nand_cache_data[2048 + i];
            }
            if (!bes_store_flush(&nor->nand_store, address, 1,
                                 nor->nand_data + address * 2048,
                                 nor->nand_spare + address * 64)) {
                error_report("NAND instance writeback failed; discard run transaction: %s", strerror(errno));
                exit(1);
            }
            nor->write_enabled = nor->nand_loading = false;
            nand_update_drq(nor);
            break;
        case 0xd8:
            if (!nor->write_enabled || address < nor->write_min_offset / 2048 ||
                address >= nor->write_max_offset / 2048) { return false; }
            memset(nor->nand_data + (address & ~63u) * 2048, 0xff, 131072);
            memset(nor->nand_spare + (address & ~63u) * 64, 0xff, 4096);
            if (!bes_store_flush(&nor->nand_store, address & ~63u, 64,
                                 nor->nand_data + (address & ~63u) * 2048,
                                 nor->nand_spare + (address & ~63u) * 64)) {
                error_report("NAND instance erase writeback failed; discard run transaction: %s", strerror(errno));
                exit(1);
            }
            nor->write_enabled = false;
            break;
        case 0x03:
        case 0x0b:
        case 0x3b:
        case 0x6b:
        case 0xeb: {
            /* Column may include the GD plane-select bit; page already selects it. */
            unsigned column = address & 0xfff;
            if (!len || len > sizeof(nor->rx) || column + len > 2112) { return false; }
            memset(nor->rx, 0xff, len);
            if (column < 2048) {
                memcpy(nor->rx, nor->nand_data + nor->nand_page * 2048 + column,
                       MIN(len, 2048 - column));
            }
            if (column + len > 2048) {
                unsigned start = MAX(column, 2048);
                memcpy(nor->rx + start - column,
                       nor->nand_spare + nor->nand_page * 64 + start - 2048,
                       column + len - start);
            }
            nor->rx_len = len;
            break;
        }
        default: return false;
        }
        nor->tx_len = 0;
        nand_update_drq(nor);
        return true;
    }
    switch (cmd) {
    case 0x02:
    case 0x32: {
        uint32_t address = (value >> 8) | (nor->regs[0x30 / 4] << 24);
        unsigned page = dual ? 512 : 256;
        if (extended || !nor->write_enabled || !nor->backing || !len ||
            !nor_writable(nor, address, len) ||
            (address & (page - 1)) + len > page || nor->program_remaining ||
            nor->tx_len > len) {
            return false;
        }
        uint8_t *flash = memory_region_get_ram_ptr(nor->backing);
        for (unsigned i = 0; i < nor->tx_len; i++) {
            flash[address + i] &= nor->tx[i]; /* NOR only programs 1 to 0. */
        }
        nor->program_start = address;
        nor->program_length = len;
        nor->program_address = address + nor->tx_len;
        nor->program_remaining = len - nor->tx_len;
        if (!nor->program_remaining) {
            nor_flush(nor, nor->program_start, nor->program_length);
        }
        nor->write_enabled = false;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES NOR NV program: address=0x%08x size=%u queued=%u\n",
                      address, len, nor->tx_len);
        len = 0;
        break;
    }
    case 0x20: /* 4 KiB sector erase */
    case 0x52: /* 32 KiB block erase, used by the P67 NOR driver */
    case 0xd8: { /* 64 KiB block erase; private writable extents only */
        uint32_t address = (value >> 8) | (nor->regs[0x30 / 4] << 24);
        unsigned count = cmd == 0x20 ? 4096 : cmd == 0x52 ? 32768 : 65536;
        if (dual) {
            count *= 2;
        }
        address &= ~(count - 1);
        if (extended || !nor->write_enabled || !nor->backing ||
            !nor_writable(nor, address, count) || nor->program_remaining) {
            return false;
        }
        memset((uint8_t *)memory_region_get_ram_ptr(nor->backing) + address,
               0xff, count);
        nor_flush(nor, address, count);
        qemu_log_mask(CPU_LOG_PLUGIN,
                      "NOR-ERASE command=0x%02x address=0x%08x bytes=%u\n",
                      cmd, address, count);
        nor->write_enabled = false;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES NOR NV erase: address=0x%08x size=%u\n",
                      address, count);
        len = 0;
        break;
    }
    case 0x03:
    case 0x0b:
    case 0xbb:
    case 0xeb: {
        uint32_t address = (value >> 8) | (nor->regs[0x30 / 4] << 24);
        if (extended || !nor->backing || !len || len > sizeof(nor->rx) ||
            (uint64_t)address + len > memory_region_size(nor->backing)) {
            return false;
        }
        uint8_t *flash = memory_region_get_ram_ptr(nor->backing);
        nor->rx_pos = 0;
        nor->rx_len = len;
        /* Legacy addressed reads return linear bytes, as norflash_read()
         * consumes them directly. JEDEC/status register packing is separate.
         */
        memcpy(nor->rx, flash + address, len);
        nor->tx_len = 0;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES NOR backing read: command=0x%02x address=0x%06x "
                      "rx_len=%u dual=%d\n", cmd, address, len, dual);
        return true;
    }
    case 0xab: /* Release deep power-down */
    case 0xb9: /* Deep power-down: no physical power transitions */
    case 0xff: /* Exit continuous read */
    case 0x66: /* Reset enable */
    case 0xe9: /* Exit four-byte address mode (backing range is <16 MiB/chip) */
    case 0xb7: /* Enter four-byte address mode */
        len = 0;
        break;
    case 0x99: /* Reset device */
        nor_check_complete(nor);
        nor->write_enabled = false;
        nor->program_remaining = nor->program_address = 0;
        nor->tx_len = 0;
        len = 0;
        break;
    case 0x06: /* Write enable */
    case 0x50: /* Volatile status-register write enable */
        nor->write_enabled = true;
        len = 0;
        break;
    case 0x04: /* Write disable */
        nor->write_enabled = false;
        len = 0;
        break;
    case 0x9f:
        data[0] = 0xc8; data[1] = 0x60;
        data[2] = nor->backing && memory_region_size(nor->backing) == 8 * MiB ? 0x16 : 0x17;
        data_len = 3;
        break;
    case 0x05:
    case 0x35:
    case 0x15:
        data[0] = nor->status[cmd == 5 ? 0 : cmd == 0x35 ? 1 : 2];
        if (cmd == 5) {
            data[0] = (data[0] & ~3) | (nor->write_enabled ? 2 : 0) |
                      (nor->program_remaining ? 1 : 0);
        }
        if (!extended) {
            len = 1;
        }
        break;
    case 0x77: /* GD25 set burst with wrap; SDK sends one TX parameter. */
        if (!nor->tx_len) {
            return false;
        }
        nor->burst_wrap = nor->tx[0];
        /* Direct XIP backing has no bus burst timing to configure. */
        len = 0;
        break;
    case 0x01:
    case 0x31:
    case 0x11:
        if (!nor->write_enabled) {
            return false;
        }
        for (unsigned i = 0; i < nor->tx_len && i < (cmd == 1 ? 3 : 1); i++) {
            unsigned index = cmd == 1 ? i : cmd == 0x31 ? 1 : 2;
            nor->status[index] = nor->tx[i];
        }
        nor->write_enabled = false;
        len = 0;
        break;
    default:
        return false;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "BES NOR command=0x%02x rx_len=%u"
                  " dual=%u quad_rx=%u\n", cmd, len, dual, quad_rx);
    nor->rx_pos = 0;
    nor->rx_len = MIN(len, sizeof(nor->rx));
    for (unsigned i = 0; i < nor->rx_len; i++) {
        if (dual && quad_rx) {
            /* Two identical chips sampled on lanes 1/5, packed as in the
             * SDK's norflash_dual_chip_read_reg_ex() for CTRL_VER <= 6.
             */
            unsigned byte = i / 8;
            unsigned bit = 7 - (i % 8);
            nor->rx[i] = data[byte % data_len] & (1 << bit) ? 0x22 : 0;
        } else if (dual) {
            /* CTRL_VER >= 7 returns high/low nibbles from both chips in
             * each byte; Q66 uses this path (six RX bytes for JEDEC ID).
             */
            uint8_t byte = data[(i / 2) % data_len];
            uint8_t nibble = i & 1 ? byte & 15 : byte >> 4;
            nor->rx[i] = nibble | (nibble << 4);
        } else {
            nor->rx[i] = data[i % data_len];
        }
    }
    /* The driver fills TX before WREN. Only payload-consuming commands
     * drain the FIFO; register reads and WREN leave queued bytes intact.
     */
    if (cmd == 0x02 || cmd == 0x32 || cmd == 0x01 || cmd == 0x31 ||
        cmd == 0x11 || cmd == 0x77) {
        nor->tx_len = 0;
    }
    return true;
}

static MemTxResult nor_read(void *opaque, hwaddr offset, uint64_t *value,
                            unsigned size, MemTxAttrs attrs)
{
    BesNor *nor = opaque;
    if (offset >= sizeof(nor->regs)) {
        return MEMTX_ERROR;
    }
    if (offset == 0x0c) {
        /* Q66 CTRL_VER >= 7: TX empty-count is at bit 9, RX at bit 4. */
        unsigned remaining = nor->rx_len - nor->rx_pos;
        *value = (16 << 9) | 2 | (nor->program_remaining ? 1 : 0) |
                 (nor->nand_remaining ? 1 : 0) | (remaining ? 0 : 8) |
                 (MIN(remaining, 31) << 4);
    } else if (offset == 0x10 || (offset == 8 && nor->nand_data)) {
        unsigned width = offset == 8 ? size :
                         1u << ((nor->regs[0x204 / 4] >> 17) & 3);
        *value = 0;
        for (unsigned i = 0; i < MIN(width, 4); i++) {
            if (nor->rx_pos < nor->rx_len) {
                *value |= (uint64_t)nor->rx[nor->rx_pos++] << (8 * i);
            }
        }
        if (nor->nand_data && nor->rx_pos == nor->rx_len) {
            nand_update_drq(nor);
        }
    } else {
        *value = nor->regs[offset / 4] >> ((offset & 3) * 8);
    }
    return MEMTX_OK;
}

static MemTxResult nor_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size, MemTxAttrs attrs)
{
    BesNor *nor = opaque;
    if (offset >= sizeof(nor->regs)) {
        return MEMTX_ERROR;
    }
    if ((offset & ~3) == 8) {
        if (nor->nand_data && nor->nand_loading) {
            if (size > nor->nand_remaining ||
                nor->nand_column + size > sizeof(nor->nand_cache_data)) { return MEMTX_ERROR; }
            for (unsigned i = 0; i < size; i++) {
                nor->nand_cache_data[nor->nand_column++] = value >> (8 * i);
            }
            nor->nand_remaining -= size;
            if (!nor->nand_remaining) { nand_update_drq(nor); }
            return MEMTX_OK;
        }
        if (nor->program_remaining) {
            if (size > nor->program_remaining) {
                return MEMTX_ERROR;
            }
            uint8_t *flash = memory_region_get_ram_ptr(nor->backing);
            for (unsigned i = 0; i < size; i++) {
                flash[nor->program_address++] &= value >> (8 * i);
            }
            nor->program_remaining -= size;
            if (!nor->program_remaining) {
                nor_flush(nor, nor->program_start, nor->program_length);
            }
            return MEMTX_OK;
        }
        for (unsigned i = 0; i < size && nor->tx_len < sizeof(nor->tx); i++) {
            nor->tx[nor->tx_len++] = value >> (8 * i);
        }
        return MEMTX_OK;
    }
    if ((offset & ~3) == 0 && !nor_command(nor, value)) {
        /* Private first-fault metadata only: never flash/HCI payload bytes. */
        qemu_log_mask(CPU_LOG_PLUGIN,
                      "NOR-REJECT base=0x%08" HWADDR_PRIx
                      " command=0x%02x value=0x%08" PRIx64
                      " ctrl=0x%08x addressHigh=0x%08x tx_len=%u"
                      " program_remaining=%u write_enabled=%u dual=%u"
                      " writable_min=0x%08x writable_max=0x%08x"
                      " config_offset=0x%08x config_bytes=0x%08x\n",
                      nor->base, (unsigned)(value & 0xff), value,
                      nor->regs[1], nor->regs[0x30 / 4], nor->tx_len,
                      nor->program_remaining, nor->write_enabled,
                      !!(nor->regs[0x204 / 4] & (1 << 19)),
                      nor->write_min_offset, nor->write_max_offset,
                      nor->config_offset, nor->config_bytes);
        qemu_log_mask(LOG_GUEST_ERROR, "BES NOR at 0x%08" HWADDR_PRIx
                      " unsupported transaction offset=0x%" HWADDR_PRIx
                      " value=0x%08" PRIx64 "\n", nor->base, offset, value);
        return MEMTX_ERROR;
    }
    if ((offset & ~3) == 0x18) {
        if (value & 1) {
            nor->rx_len = nor->rx_pos = 0;
        }
        if (value & 2) {
            nor->tx_len = 0;
        }
    }
    uint32_t mask = (UINT32_MAX >> (32 - size * 8)) << ((offset & 3) * 8);
    nor->regs[offset / 4] = (nor->regs[offset / 4] & ~mask) |
                           ((value << ((offset & 3) * 8)) & mask);
    return MEMTX_OK;
}

static const MemoryRegionOps nor_ops = {
    .read_with_attrs = nor_read,
    .write_with_attrs = nor_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/* GH3X2X functional register fixture, not an optical sampling model.
 * SPI framing is confirmed by the original F0/F1 helpers. Only registers
 * are retained for readback; unknown status and FIFO reads are rejected.
 */
static bool gh_register_address(unsigned address)
{
    return !(address & 1) && address <= 0x880;
}

static void gh_reset(BesSpiConfig *spi)
{
    memset(spi->sensor, 0, sizeof(spi->sensor));
    memset(spi->sensor_written, 0, sizeof(spi->sensor_written));
    spi->sensor[0x36 / 2] = 0xaa55; /* Documented communication ready code. */
    if (spi->machine->gh_reg0724_set) {
        /* User-selected hypothesis, not a measured revision/reset value. */
        spi->sensor[0x724 / 2] = spi->machine->gh_reg0724;
        spi->sensor_written[0x724 / 2] = true;
    }
    spi->awake = false;
    spi->read_latched = false;
    spi->rx_overrun = false;
    spi->packet_len = spi->rx_count = spi->rx_head = 0;
}

static void gh_pins_update(Best1503State *s)
{
    BesSpiConfig *spi = &s->spi0;
    if (!s->gh3x2x_fixture) {
        return;
    }
    bool reset = (s->gpio[0].regs[0] & s->gpio[0].regs[1] & (1 << 13)) != 0;
    reset &= (s->gpio[2].regs[0] & s->gpio[2].regs[1] & (1 << 8)) != 0;
    bool selected = (s->gpio[1].regs[1] & (1 << 18)) &&
                    !(s->gpio[1].regs[0] & (1 << 18));
    if (!reset && spi->reset_high) {
        gh_reset(spi);
    }
    if (reset && !spi->reset_high) {
        spi->ready_at = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 7000000;
    }
    if (!selected && spi->selected && spi->packet_len) {
        uint8_t *p = spi->packet;
        unsigned n = spi->packet_len;
        bool valid = false;
        if (n == 1 && (p[0] == 0xc2 || p[0] == 0xc3 || p[0] == 0xc4)) {
            spi->read_latched = false;
            if (p[0] == 0xc2) {
                gh_reset(spi);
            } else {
                spi->awake = p[0] == 0xc3;
            }
            if (p[0] == 0xc3) {
                spi->ready_at = MAX(spi->ready_at,
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 500000);
            }
            qemu_log_mask(LOG_GUEST_ERROR,
                          "BES experimental GH3X2X command: 0x%02x\n", p[0]);
            valid = true;
        } else if (p[0] == 0xf0 && n >= 3) {
            unsigned address = p[1] << 8 | p[2];
            if (n == 3) {
                spi->read_address = address;
                spi->read_latched = true;
                valid = true;
            } else if (n >= 7) {
                unsigned count = p[3] << 8 | p[4];
                /* Q66 encodes a byte count: its single-register write
                 * explicitly sends F0, address16, 0002, value16.
                 */
                valid = !(count & 1) && n == 5 + count && count <= 512;
                for (unsigned i = 0; valid && i < count / 2; i++) {
                    valid = gh_register_address(address + i * 2) &&
                            address + i * 2 != 0x36;
                }
                if (valid) {
                    for (unsigned i = 0; i < count / 2; i++) {
                        unsigned value = p[5 + i * 2] << 8 | p[6 + i * 2];
                        spi->sensor[address / 2 + i] = value;
                        spi->sensor_written[address / 2 + i] = true;
                        qemu_log_mask(LOG_GUEST_ERROR,
                                      "BES experimental GH3X2X write: register=0x%04x value=0x%04x\n",
                                      address + i * 2, value);
                    }
                }
            }
        } else if (p[0] == 0xf1 && n > 1 && (n & 1)) {
            valid = true;
            spi->read_latched = false;
        }
        if (!valid) {
            spi->read_latched = false;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "BES experimental GH3X2X cancelled malformed packet: command=0x%02x size=%u\n",
                          p[0], n);
        }
        spi->packet_len = 0;
    }
    if (selected && !spi->selected) {
        spi->packet_len = 0;
    }
    spi->selected = selected;
    spi->reset_high = reset;
}

static bool gh_transfer(BesSpiConfig *spi, uint32_t value)
{
    Best1503State *s = spi->machine;
    gh_pins_update(s);
    unsigned n = spi->packet_len;
    if (!s->gh3x2x_fixture || !spi->selected || !spi->reset_high ||
        spi->regs[0] != 0x70100 || spi->regs[1] != 2 ||
        spi->regs[0x24 / 4] || spi->regs[0x88 / 4] || value > 255 ||
        n >= sizeof(spi->packet)) {
        return false;
    }
    uint8_t response = 0;
    if (!n) {
        if (value != 0xf0 && value != 0xf1 && value != 0xc2 &&
            value != 0xc3 && value != 0xc4) {
            return false;
        }
        if ((value == 0xf0 || value == 0xf1) &&
            (!spi->awake || qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < spi->ready_at)) {
            return false;
        }
        if (value == 0xf1 && !spi->read_latched) {
            return false;
        }
        if (value == 0xf0) {
            spi->read_latched = false;
        }
    } else if (spi->packet[0] == 0xf1) {
        unsigned address = spi->read_address + ((n - 1) / 2) * 2;
        if (!gh_register_address(address) ||
            (address != 0 && address != 0x36 && address != 0x1ec &&
             !spi->sensor_written[address / 2])) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "BES experimental GH3X2X unsupported read: register=0x%04x\n", address);
            return false;
        }
        unsigned data = spi->sensor[address / 2];
        response = n & 1 ? data >> 8 : data & 255;
        if (!(n & 1)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "BES experimental GH3X2X read: register=0x%04x value=0x%04x\n",
                          address, data);
        }
    } else if (spi->packet[0] == 0xf0) {
        unsigned address = n >= 3 ? spi->packet[1] << 8 | spi->packet[2] : 0;
        if (n == 4) {
            unsigned count = spi->packet[3] << 8 | value;
            if (!count || (count & 1) || count > 512 ||
                !gh_register_address(address) || address + count > 0x882 ||
                (address <= 0x36 && address + count > 0x36)) {
                return false;
            }
        }
        if (n >= 5 && n >= 5 + (spi->packet[3] << 8 | spi->packet[4])) {
            return false;
        }
        if (n >= 5 && (!gh_register_address(address + ((n - 5) / 2) * 2) ||
                       address + ((n - 5) / 2) * 2 == 0x36)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "BES experimental GH3X2X unsupported write: register=0x%04x\n", address);
            return false;
        }
    } else {
        return false; /* A simple command has exactly one byte. */
    }
    spi->packet[spi->packet_len++] = value;
    if (spi->rx_count == 8) {
        /* Transmit-only callers ignore RX; retain the first FIFO bytes. */
        spi->rx_overrun = true;
        return true;
    }
    spi->rx[(spi->rx_head + spi->rx_count) % 8] = response;
    spi->rx_count++;
    return true;
}

static bool spi0_unconnected_config(BesSpiConfig *spi)
{
    return spi->machine->spi0_unconnected &&
           (spi->regs[0] & ~0xffc0U) == 0x70000 &&
           (spi->regs[1] & ~(3u << 7)) == 2 &&
           !(spi->regs[0x24 / 4] & ~3u) && !spi->regs[0x88 / 4];
}

static void spi0_dma_request(void *opaque)
{
    BesSpiConfig *spi = opaque;
    bool active = spi0_unconnected_config(spi);

    /* These are level requests, not completion IRQs. Drain available RX
     * before allowing the next transmitted byte. The transmit shifter is
     * independent of RX: a TX-only client may legitimately overrun RX. */
    qemu_set_irq(spi->dma_rx, active && (spi->regs[0x24 / 4] & 1) &&
                              spi->rx_count);
    qemu_set_irq(spi->dma_tx, active && (spi->regs[0x24 / 4] & 2));
}

static void spi0_dma_update(BesSpiConfig *spi)
{
    if (!spi->dma_timer) {
        return; /* No evidence for SPI1's DMA request routing yet. */
    }
    bool active = spi0_unconnected_config(spi);
    unsigned enable = active ? spi->regs[0x24 / 4] : 0;

    /* Withdraw requests immediately on RX empty, DMA disable or SSE
     * disable. Assert through the timer to avoid starting a new transfer
     * recursively from the FIFO access that completes the previous one.
     * A final timer callback also lets PL080 update its completion IRQ after
     * the channel-configuration write path has finished setting TC status. */
    if (!(enable & 2)) {
        qemu_set_irq(spi->dma_tx, 0);
    }
    if (!(enable & 1) || !spi->rx_count) {
        qemu_set_irq(spi->dma_rx, 0);
    }
    if (enable) {
        /* Functional scheduling only, not a claim about the SPI clock. */
        timer_mod(spi->dma_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
    } else {
        timer_del(spi->dma_timer);
    }
}

static void spi0_controller_reset(BesSpiConfig *spi)
{
    /* This is a controller reset, not a reset of the external sensor. */
    memset(spi->regs, 0, sizeof(spi->regs));
    memset(spi->rx, 0, sizeof(spi->rx));
    spi->rx_head = spi->rx_count = 0;
    spi->rx_overrun = false;
    spi0_dma_update(spi);
}

static bool spi0_unconnected_transfer(BesSpiConfig *spi, uint32_t value)
{
    if (!spi0_unconnected_config(spi) || value > 255) {
        return false;
    }
    /* An unconnected slave does not abort the host bus transaction.
     * Pull-high MISO is an explicit board-model choice.
     */
    if (spi->rx_count == ARRAY_SIZE(spi->rx)) {
        spi->rx_overrun = true;
    } else {
        spi->rx[(spi->rx_head + spi->rx_count) % ARRAY_SIZE(spi->rx)] = 0xff;
        spi->rx_count++;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "BES SPI0 unconnected transfer: tx=0x%02x rx=0xff\n", value);
    return true;
}

/* Eight-bit SPI with optional FIFO-driven DMA for the unconnected SPI0
 * fixture. Sensor protocol fixtures remain polled; no SPI interrupt model. */
static MemTxResult spi0_read(void *opaque, hwaddr offset, uint64_t *value,
                             unsigned size, MemTxAttrs attrs)
{
    BesSpiConfig *spi = opaque;
    switch (offset) {
    case 0x0c:
        *value = 3 | (spi->rx_count ? 4 : 0) | (spi->rx_count == 8 ? 8 : 0);
        return MEMTX_OK;
    case 0x08:
        if ((spi->machine->gh3x2x_fixture || spi->machine->spi0_unconnected) && spi->rx_count) {
            *value = spi->rx[spi->rx_head];
            spi->rx_head = (spi->rx_head + 1) % 8;
            spi->rx_count--;
            spi0_dma_update(spi);
            return MEMTX_OK;
        }
        return MEMTX_ERROR;
    case 0x18:
        *value = 8 | (spi->rx_count >= 4 ? 4 : 0) | spi->rx_overrun;
        return MEMTX_OK;
    case 0x1c: *value = 0; return MEMTX_OK; /* Interrupt mask must be zero. */
    case 0x00: case 0x04: case 0x10: case 0x14: case 0x24:
    case 0x28: case 0x2c: case 0x88:
        *value = spi->regs[offset / 4];
        return MEMTX_OK;
    default:
        return MEMTX_ERROR;
    }
}

static MemTxResult spi0_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size, MemTxAttrs attrs)
{
    BesSpiConfig *spi = opaque;
    switch (offset) {
    case 0x08:
        if (spi0_unconnected_transfer(spi, value) || gh_transfer(spi, value)) {
            /* Retire one SPI word before the next TX DMA request. Without
             * this scheduling PL080 drains all TX synchronously before RX
             * gets a turn, overflowing even a correctly configured duplex
             * transfer. TX-only operation instead records normal RX overrun. */
            if (spi->dma_timer) {
                qemu_set_irq(spi->dma_tx, 0);
            }
            spi0_dma_update(spi);
            return MEMTX_OK;
        }
        break;
    case 0x00: case 0x04: case 0x10: case 0x24:
    case 0x28: case 0x2c: case 0x88:
        spi->regs[offset / 4] = value;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES SPI0 configuration: offset=0x%02" HWADDR_PRIx
                      " value=0x%08" PRIx64 " (register configuration)\n", offset, value);
        spi0_dma_update(spi);
        return MEMTX_OK;
    case 0x14:
        if (!value) {
            spi->regs[offset / 4] = 0;
            return MEMTX_OK;
        }
        break;
    case 0x20: /* Clear latched receive overrun. */
        if (value & 1) {
            spi->rx_overrun = false;
        }
        return MEMTX_OK;
    case 0x0c: case 0x18: case 0x1c: /* Read-only status. */
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "BES SPI0 transfer unavailable: offset=0x%02" HWADDR_PRIx
                  " value=0x%08" PRIx64 " (no matching slave/IRQ backend)\n", offset, value);
    return MEMTX_ERROR;
}

static bool spi0_access_valid(void *opaque, hwaddr offset, unsigned size,
                              bool is_write, MemTxAttrs attrs)
{
    BesSpiConfig *spi = opaque;
    /* Q66 .071 configures byte-wide RX/TX DMA against DR. All other
     * registers keep their original aligned 32-bit access constraint. */
    return (size == 4 && !(offset & 3)) ||
           (offset == 0x08 && size == 1 && spi->dma_timer &&
            spi0_unconnected_config(spi));
}

static const MemoryRegionOps spi0_ops = {
    .read_with_attrs = spi0_read,
    .write_with_attrs = spi0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .valid.accepts = spi0_access_valid,
};

static void gpu_update_irq(Best1503State *s)
{
    qemu_set_irq(s->gpu_irq, !!(s->gpu_status & s->gpu_probe_mask));
}

static bool gpu_ram_read(uint32_t address, void *data, unsigned bytes)
{
    return lcdc_frame_range(address, bytes) &&
        address_space_read(&address_space_memory, address,
                           MEMTXATTRS_UNSPECIFIED, data, bytes) == MEMTX_OK;
}

static void gpu_complete(void *opaque)
{
    Best1503State *s = opaque;
    if (s->gpu_work) {
        BesGpuWork *w = s->gpu_work;
        for (unsigned i = 0; i < w->target_count; i++) {
            BesGpuTarget *t = &w->targets[i];
            address_space_write(&address_space_memory, t->address,
                                MEMTXATTRS_UNSPECIFIED, t->pixels, t->bytes);
        }
        memcpy(s->gpu_sw_regs, w->regs, sizeof(s->gpu_sw_regs));
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES GPU rendered: id=%u draws=%u pixels=%u targets=%u\n",
                      s->gpu_pending_id, w->draws, w->pixels, w->target_count);
        gpu_sw_free(w);
        s->gpu_work = NULL;
    }
    s->gpu_busy = false;
    s->gpu_status |= s->gpu_pending_event;
    s->gpu_pending_event = 0;
    gpu_update_irq(s);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "BES experimental GPU completion: status=0x%08x (%s)\n",
                  s->gpu_status, s->gpu_software_renderer ? "software renderer" : "no pixels");
}

/* Validate command framing before synthesizing an END event. This fixture
 * skips STATE and DATA payloads, never executes drawing or follows CALLs.
 * END's low five bits select the interrupt bit (public EVENT_END == 0).
 */
static bool gpu_decode_end(const uint8_t *commands, unsigned bytes,
                           uint32_t *event)
{
    for (unsigned pos = 0; pos + 8 <= bytes; ) {
        uint32_t command = ldl_le_p(commands + pos);
        unsigned opcode = command >> 28;
        unsigned step = 8;
        if (opcode == 0) {
            if ((command & ~31u) || pos + 8 != bytes ||
                ldl_le_p(commands + pos + 4)) {
                return false;
            }
            *event = 1u << command;
            return true;
        } else if (opcode == 3) {
            unsigned count = (command >> 16) & 0xfff;
            if (!count) {
                return false;
            }
            step = ((count + 1) * 4 + 7) & ~7u;
        } else if (opcode == 4) {
            step += (command & 0xffff) * 8;
        } else if (opcode != 1 && opcode != 2 && opcode != 8) {
            return false;
        }
        if (step > bytes - pos) {
            return false;
        }
        pos += step;
    }
    return false;
}

/* VGLite probe with optional Q66 identity and no-render scheduling fixture. */
static MemTxResult gpu_probe_read(void *opaque, hwaddr offset, uint64_t *value,
                                  unsigned size, MemTxAttrs attrs)
{
    Best1503State *s = opaque;
    switch (offset) {
    case 0: *value = s->gpu_probe_control; break;
    case 4: *value = s->gpu_busy ? 0 : 0xb05; break;
    case 0x10:
        *value = s->gpu_status;
        s->gpu_status = 0; /* Interrupt acknowledge on read. */
        gpu_update_irq(s);
        break;
    case 0x14: *value = s->gpu_probe_mask; break;
    case 0x500: *value = s->gpu_command_address; break;
    case 0x20: *value = s->gpu_id_fixture ? 0x265 : 0; break;
    case 0x24: *value = s->gpu_id_fixture ? 0x2002 : 0; break;
    case 0x30: *value = s->gpu_id_fixture ? 0x424 : 0; break;
    case 0x28: case 0x40: case 0xe8: *value = 0; break;
    default: return MEMTX_ERROR;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "BES GPU probe read: offset=0x%03" HWADDR_PRIx " value=0x%08" PRIx64 "\n",
                  offset, *value);
    return MEMTX_OK;
}

static MemTxResult gpu_probe_write(void *opaque, hwaddr offset, uint64_t value,
                                   unsigned size, MemTxAttrs attrs)
{
    Best1503State *s = opaque;
    if (offset == 0) {
        s->gpu_probe_control = value;
        if (value & (1 << 12)) {
            timer_del(s->gpu_timer);
            if (s->gpu_work) {
                qemu_log_mask(LOG_GUEST_ERROR, "BES GPU render cancelled: id=%u reset\n",
                              s->gpu_pending_id);
                gpu_sw_free(s->gpu_work);
                s->gpu_work = NULL;
            }
            memset(s->gpu_sw_regs, 0, sizeof(s->gpu_sw_regs));
            s->gpu_busy = false;
            s->gpu_status = s->gpu_pending_event = 0;
            gpu_update_irq(s);
        }
    } else if (offset == 0x14) {
        s->gpu_probe_mask = value;
        gpu_update_irq(s);
    } else if (offset == 0x500) {
        if (!lcdc_frame_range(value, 8) || (value & 7)) {
            return MEMTX_ERROR;
        }
        s->gpu_command_address = value;
    } else if (offset == 0x520 && value == 4) {
        /* Public gpu_enter_lpwr(0) resume write; no rendering side effect. */
    } else if (offset == 0x504 && value && value <= 8192 &&
               lcdc_frame_range(s->gpu_command_address, value * 8)) {
        g_autofree uint8_t *commands = g_malloc(value * 8);
        if (address_space_read(&address_space_memory, s->gpu_command_address,
                               MEMTXATTRS_UNSPECIFIED, commands, value * 8) != MEMTX_OK) {
            return MEMTX_ERROR;
        }
        unsigned id = s->gpu_submission_id++;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES GPU command submitted: id=%u address=0x%08x bytes=%u\n",
                      id, s->gpu_command_address, (unsigned)value * 8);
        if (qemu_loglevel_mask(CPU_LOG_INT)) {
            g_autofree char *hex = g_malloc(value * 16 + 1);
            static const char digits[] = "0123456789abcdef";
            for (unsigned i = 0; i < value * 8; i++) {
                hex[i * 2] = digits[commands[i] >> 4];
                hex[i * 2 + 1] = digits[commands[i] & 15];
            }
            hex[value * 16] = 0;
            qemu_log_mask(CPU_LOG_INT,
                          "BES GPU command captured: address=0x%08x bytes=%u data=%s (not executed)\n",
                          s->gpu_command_address, (unsigned)value * 8, hex);
        }
        if (s->gpu_software_renderer) {
            const char *error = "busy";
            if (s->gpu_busy ||
                !(s->gpu_work = gpu_sw_prepare(s->gpu_sw_regs, commands,
                                               value * 8, gpu_ram_read, &error))) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "BES GPU render rejected: id=%u reason=%s\n", id, error);
                return MEMTX_ERROR;
            }
            s->gpu_pending_id = id;
            s->gpu_pending_event = s->gpu_work->event;
            s->gpu_busy = true;
            timer_mod(s->gpu_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
            return MEMTX_OK;
        }
        if (s->gpu_completion_fixture) {
            if (s->gpu_busy || !gpu_decode_end(commands, value * 8,
                                               &s->gpu_pending_event)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "BES GPU completion rejected: busy or unsupported command framing\n");
                return MEMTX_ERROR;
            }
            s->gpu_busy = true;
            /* Scheduling fixture timing, not a measured rendering duration. */
            timer_mod(s->gpu_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
        }
        /* Experimental no-render path: the command is accepted and the
         * public idle value remains set, so the original wait can return.
         */
        return s->gpu_no_render ? MEMTX_OK : MEMTX_ERROR;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES GPU probe unsupported write: offset=0x%03" HWADDR_PRIx " value=0x%08" PRIx64 "\n",
                      offset, value);
        return MEMTX_ERROR;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "BES GPU probe write: offset=0x%03" HWADDR_PRIx " value=0x%08" PRIx64 "\n",
                  offset, value);
    return MEMTX_OK;
}

static const MemoryRegionOps gpu_probe_ops = {
    .read_with_attrs = gpu_probe_read,
    .write_with_attrs = gpu_probe_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* Internal SPI: register-level 25-bit BES analog transactions. FIFO and
 * page selection are modeled; analog devices currently retain register
 * writes rather than simulating voltage, charger, RF or ADC behavior.
 */
static uint64_t ispi_read(void *opaque, hwaddr offset, unsigned size)
{
    BesIspi *spi = opaque;
    if (offset == 0x0c) {
        return 3 | (spi->count ? 4 : 0) | (spi->count == 8 ? 8 : 0);
    }
    if (offset == 8) {
        if (!spi->count) {
            return 0;
        }
        uint32_t value = spi->rx[spi->head];
        int channel = spi->adc_channel[spi->head];
        if (channel >= 0 && channel < BES_ADC_CHANNELS) {
            qemu_mutex_lock(&spi->machine->adc_lock);
            bes_adc_observe(&spi->machine->adc_input, channel,
                            spi->adc_version[spi->head]);
            qemu_mutex_unlock(&spi->machine->adc_lock);
        }
        spi->head = (spi->head + 1) % 8;
        spi->count--;
        return value;
    }
    return spi->regs[offset / 4];
}

static void ispi_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    BesIspi *spi = opaque;
    if (offset != 8) {
        spi->regs[offset / 4] = value;
        return;
    }
    unsigned cs = (spi->regs[1] >> 7) & 3;
    unsigned bits = ((spi->regs[0] >> 16) & 31) + 1;
    unsigned padding = bits > 25 ? bits - 25 : 0;
    uint32_t cmd = value >> padding;
    unsigned reg = (cmd >> 16) & 255;
    uint16_t *cell = &spi->analog[cs][spi->page[cs]][reg];
    uint32_t response = 0;
    int adc_channel = -1;
    uint64_t adc_version = 0;
    if (cmd & (1 << 24)) {
        uint16_t data = *cell;
        if (spi->machine->power_adc_fixture && cs == 3 && !spi->page[cs] &&
            (spi->analog[3][0][0x4f] & 0x20)) {
            /* Honor guest channel enablement. Raw inputs have no inferred
             * voltage, temperature, battery or charging interpretation. */
            uint16_t channels = spi->analog[3][0][0x20] & 0xff;
            if (reg == 0x50) {
                data = channels;
            } else if (reg >= 0x56 && reg <= 0x5d && (channels & (1u << (reg - 0x56)))) {
                adc_channel = reg - 0x56;
                qemu_mutex_lock(&spi->machine->adc_lock);
                data = bes_adc_sample(&spi->machine->adc_input, adc_channel,
                                      &adc_version);
                qemu_mutex_unlock(&spi->machine->adc_lock);
            }
        }
        response = data << padding;
        /* The stock PMU worker polls continuously. Keep startup evidence
         * bounded in normal runs; --trace retains every transaction. */
        if (qemu_loglevel_mask(CPU_LOG_TB_IN_ASM) || spi->logged_reads < 256) {
            qemu_log_mask(LOG_GUEST_ERROR,
                      "BES ISPI read cs=%u page=%u reg=0x%02x -> 0x%04x"
                      " CR1=0x%08x RXCR=0x%08x bits=%u\n",
                      cs, spi->page[cs], reg, data, spi->regs[1],
                      spi->regs[0x88 / 4], bits);
            if (spi->logged_reads < 256 && ++spi->logged_reads == 256) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "BES ISPI read log limit reached; use --trace for all reads\n");
            }
        }
    } else if (reg == 0 && (cmd & 0xff0f) == 0xa000) {
        unsigned page = (cmd >> 4) & 15;
        if (page < 5) {
            spi->page[cs] = page;
        }
    } else {
        *cell = cmd;
        if (spi->machine->pmu_calibration && cs == 3 && !spi->page[cs] && reg == 0xf8) {
            /* O66 .28 SRAM 0x27574a polls FC bit11 after F8 bits14/3.
             * Functional completion only; no measured analog calibration. */
            spi->analog[3][0][0xfc] = (cmd & 0x4008) == 0x4008 ? 0x0800 : 0;
        }
    }
    /* RXCR.EN selects a distinct receive-frame format. Normal full-duplex
     * transfers also produce RX data when RXCR.EN is zero (Q66 uses this).
     */
    if (spi->count < 8) {
        unsigned slot = (spi->head + spi->count) % 8;
        spi->rx[slot] = response;
        spi->adc_channel[slot] = adc_channel;
        spi->adc_version[slot] = adc_version;
        spi->count++;
    }
}

static const MemoryRegionOps ispi_ops = {
    .read = ispi_read,
    .write = ispi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* Q66's PHY follows reg_psram_phy_v2.h (DLL status at 0x58), not v4.
 * Commands/FIFO/status follow hal_psram_v2.c. DLL locks synchronously once
 * powered and released; delay=16 is a functional choice, not measured timing.
 * MR1 vendor=0x0d selects the target's supported octal-device branch. It is
 * a synthetic identity, not a claim about the physical watch's PSRAM.
 * MC 0xc0/0xc4 are Q66 extensions retained as config; their semantics remain
 * unknown (v2 reserves them and v4 uses 0xc4 for timing, not IRQ clear).
 */
static bool psram_mc_valid(hwaddr offset)
{
    return offset <= 0xc4 || (offset >= 0x140 && offset <= 0x160) ||
           (offset >= 0x180 && offset <= 0x194) || offset == 0x200 ||
           offset == 0x400 || offset == 0x404 ||
           (offset >= 0x440 && offset <= 0x49c);
}

static MemTxResult psram_mc_read(void *opaque, hwaddr offset, uint64_t *value,
                                 unsigned size, MemTxAttrs attrs)
{
    BesPsram *p = opaque;
    if (!psram_mc_valid(offset)) {
        return MEMTX_ERROR;
    }
    if (offset == 0x404) {
        unsigned count = p->rx_count - p->rx_pos;
        *value = (4 << 15) | ((8 - p->tx_count) << 7) | (count << 2) |
                 (count ? 0 : 2) | (p->tx_count == 8 ? 64 : 0);
    } else if (offset == 0x18) {
        *value = p->rx_pos < p->rx_count ? p->rx[p->rx_pos++] : 0;
    } else {
        *value = p->mc[offset / 4];
    }
    return MEMTX_OK;
}

static MemTxResult psram_mc_write(void *opaque, hwaddr offset, uint64_t value,
                                  unsigned size, MemTxAttrs attrs)
{
    BesPsram *p = opaque;
    if (!psram_mc_valid(offset)) {
        return MEMTX_ERROR;
    }
    if (offset == 0x404 || offset == 0x18) {
        return MEMTX_OK;
    }
    if (offset == 0x14) {
        if (p->tx_count == 8) {
            return MEMTX_ERROR;
        }
        p->tx[p->tx_count++] = value;
    } else if (offset == 0x1c) {
        if (value & 1) {
            p->tx_count = 0;
        }
        if (value & 2) {
            p->rx_pos = p->rx_count = 0;
        }
    } else if (offset == 4) {
        unsigned addr = p->mc[2];
        unsigned len = p->mc[3] & 255;
        switch (value) {
        case 0xff: /* Device reset; restore synthetic mode-register defaults */
            memset(p->mr, 0, sizeof(p->mr));
            p->mr[1] = 0x0d;
            p->tx_count = p->rx_count = p->rx_pos = 0;
            break;
        case 0x20: /* NOP: clock runs, controller idle */
            break;
        case 0x01: /* Manual memory write, word length and byte mask */
        case 0x02: /* Manual memory read, same backing as all CPU aliases */
            if (!len || len > 8 ||
                (uint64_t)addr + len * 4 > memory_region_size(p->memory) ||
                (value == 1 && p->tx_count < len)) {
                return MEMTX_ERROR;
            }
            if (value == 2) {
                for (unsigned i = 0; i < len; i++) {
                    p->rx[i] = ldl_le_p((uint8_t *)memory_region_get_ram_ptr(
                                        p->memory) + addr + i * 4);
                }
                p->rx_pos = 0;
                p->rx_count = len;
            } else {
                for (unsigned i = 0; i < len; i++) {
                    for (unsigned j = 0; j < 4; j++) {
                        if (!(p->mc[0x10 / 4] & (1u << (i * 4 + j)))) {
                            uint8_t byte = p->tx[i] >> (j * 8);
                            cpu_physical_memory_write(0x38000000 + addr +
                                                      i * 4 + j, &byte, 1);
                        }
                    }
                }
                p->tx_count = 0;
            }
            break;
        case 0x05: /* Mode register write */
            if (addr >= 256 || len != 1 || !p->tx_count) {
                return MEMTX_ERROR;
            }
            p->mr[addr] = p->tx[0];
            p->tx_count = 0;
            break;
        case 0x06: /* Mode register read */
            if (addr >= 256 || len != 1) {
                return MEMTX_ERROR;
            }
            p->rx[0] = p->mr[addr];
            p->rx_pos = 0;
            p->rx_count = 1;
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "BES PSRAM unsupported command=0x%02" PRIx64 "\n",
                          value);
            return MEMTX_ERROR;
        }
        qemu_log_mask(value == 0x20 ? CPU_LOG_INT : LOG_GUEST_ERROR,
                      "BES PSRAM command=0x%02" PRIx64 " addr=0x%x len=%u\n",
                      value, addr, len);
    }
    p->mc[offset / 4] = value;
    return MEMTX_OK;
}

static uint64_t psram_phy_read(void *opaque, hwaddr offset, unsigned size)
{
    BesPsram *p = opaque;
    if (offset == 0x58) {
        uint32_t dll = p->phy[0x50 / 4];
        return (dll & 1) && (dll & (1 << 21)) ? 0x40 | 16 : 0;
    }
    return p->phy[offset / 4];
}

static void psram_phy_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    BesPsram *p = opaque;
    if (offset != 0x58) {
        p->phy[offset / 4] = value;
    }
}

static const MemoryRegionOps psram_mc_ops = {
    .read_with_attrs = psram_mc_read,
    .write_with_attrs = psram_mc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const MemoryRegionOps psram_phy_ops = {
    .read = psram_phy_read,
    .write = psram_phy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void panel_te_update_irq(Best1503State *s);

static bool touch_powered(Best1503State *s)
{
    unsigned pins[] = {s->touch_power_pin, s->touch_reset_pin};
    if (!s->touch.enabled) { return false; }
    for (unsigned i = 0; i < 2; i++) {
        BesGpio *g = &s->gpio[pins[i] / 32];
        if (!(g->regs[0] & g->regs[1] & (1u << (pins[i] % 32)))) {
            return false;
        }
    }
    return true;
}

static bool touch_irq_ready(Best1503State *s)
{
    BesGpio *g = &s->gpio[s->touch_irq_pin / 32];
    uint32_t bit = 1u << (s->touch_irq_pin % 32);
    uint32_t forwarding = ldl_le_p((uint8_t *)
        memory_region_get_ram_ptr(&s->psc) + 0x80 + (s->touch_irq_pin / 32) * 4);

    return !(g->regs[1] & bit) && (g->regs[0x30 / 4] & bit) &&
           !(g->regs[0x34 / 4] & bit) && (forwarding & bit);
}

static void touch_irq(Best1503State *s)
{
    BesGpio *g = &s->gpio[s->touch_irq_pin / 32];
    uint32_t bit = 1u << (s->touch_irq_pin % 32);
    bool available = touch_powered(s) && s->touch.queue_count &&
                     s->touch.mode == 9 && !s->touch.sleep_mode &&
                     !s->touch.bootloader;
    bool low = available && !s->touch.irq_rearm_pending;
    bool old = !!(g->input & bit), level = !low;
    g->input = (g->input & ~bit) | (level ? bit : 0);
    if (!(g->regs[1] & bit) && (old != level) &&
        (g->regs[0x38 / 4] & bit) &&
        !!(g->regs[0x3c / 4] & bit) == level) {
        g->pending |= bit;
    }
    panel_te_update_irq(s);
    if (available && s->touch.irq_rearm_pending && touch_irq_ready(s) &&
        !timer_pending(s->touch_irq_timer)) {
        /* Functional sample interval, not measured controller timing. */
        timer_mod(s->touch_irq_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
    }
}

static void touch_irq_rearm(void *opaque)
{
    Best1503State *s = opaque;
    BesTouch *t = &s->touch;
    uint32_t bit = 1u << (s->touch_irq_pin % 32);

    if (!touch_powered(s) || !t->queue_count || t->mode != 9 ||
        t->sleep_mode || t->bootloader) {
        touch_irq(s);
        return;
    }
    if (!touch_irq_ready(s)) {
        /* GPIO/PSC writes will schedule again once delivery is enabled. */
        return;
    }

    /* ACK released the data-ready line high. Recompute it only now, after
     * the AP has had time to finish EOI/mask/enable, so the falling edge is
     * latched into GPIO pending and forwarded through the PSC summary. */
    s->gpio[s->touch_irq_pin / 32].input |= bit;
    t->irq_rearm_pending = false;
    touch_irq(s);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "BES touch IRQ rearmed: queued=%u pending=%u\n",
                  t->queue_count, !!(s->gpio[s->touch_irq_pin / 32].pending & bit));
}

static void touch_reset(Best1503State *s)
{
    BesTouch *t = &s->touch;
    t->bootloader = t->slave_reading = false;
    t->irq_rearm_pending = false;
    if (s->touch_irq_timer) {
        timer_del(s->touch_irq_timer);
    }
    t->host_down = false;
    t->tx_count = t->read_count = t->queue_head = t->queue_count = 0;
    t->pointer = t->mem_address = 0;
    t->mode = t->reply_mode = 9;
    t->sleep_mode = t->mem_type = 0;
    t->side_flags = 0;
    t->health = 0;
    s->gpio[s->touch_irq_pin / 32].pending &= ~(1u << (s->touch_irq_pin % 32));
    memset(t->report, 0, sizeof(t->report));
    t->report[6] = 0xab;
    touch_irq(s);
}

static void touch_queue_remove(BesTouch *t, unsigned offset)
{
    for (unsigned i = offset; i + 1 < t->queue_count; i++) {
        memcpy(t->queue[(t->queue_head + i) % ARRAY_SIZE(t->queue)],
               t->queue[(t->queue_head + i + 1) % ARRAY_SIZE(t->queue)],
               sizeof(t->report));
    }
    t->queue_count--;
}

static void touch_mouse(void *opaque, int x, int y, int z, int buttons)
{
    Best1503State *s = opaque;
    BesTouch *t = &s->touch;
    bool down = buttons & MOUSE_EVENT_LBUTTON;
    unsigned width = s->lcdc.frame_width ? s->lcdc.frame_width : 212;
    unsigned height = s->lcdc.frame_height ? s->lcdc.frame_height : 520;
    unsigned px = CLAMP(x, 0, 32767) * (width - 1) / 32767;
    unsigned py = CLAMP(y, 0, 32767) * (height - 1) / 32767;
    if (!touch_powered(s) || t->mode != 9 || t->sleep_mode || t->bootloader ||
        (!down && !t->host_down) ||
        (down == t->host_down && px == t->x && py == t->y)) {
        if (down && (!touch_powered(s) || t->mode != 9 || t->sleep_mode || t->bootloader)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "BES touch input inactive: powered=%u mode=%u sleep=%u bootloader=%u\n",
                          touch_powered(s), t->mode, t->sleep_mode, t->bootloader);
        }
        return;
    }
    if (t->queue_count == ARRAY_SIZE(t->queue)) {
        /* Keep the report currently exposed over I2C stable and preserve
         * press/release boundaries. Overwriting the last slot would lose a
         * queued release if the next host event starts another gesture. */
        unsigned drop = 1;
        while (drop < t->queue_count &&
               t->queue[(t->queue_head + drop) % ARRAY_SIZE(t->queue)][0] != 7) {
            drop++;
        }
        if (drop < t->queue_count) {
            touch_queue_remove(t, drop);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "BES touch queue coalesced: pending move removed\n");
        } else {
            /* All remaining reports are boundaries. Remove one complete
             * gesture the guest has not seen, never the current release. */
            for (drop = 1; drop + 1 < t->queue_count; drop++) {
                if (t->queue[(t->queue_head + drop) % ARRAY_SIZE(t->queue)][0] == 6 &&
                    t->queue[(t->queue_head + drop + 1) % ARRAY_SIZE(t->queue)][0] == 0) {
                    break;
                }
            }
            if (drop + 1 == t->queue_count) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "BES touch queue rejected: invalid gesture ordering\n");
                return;
            }
            touch_queue_remove(t, drop);
            touch_queue_remove(t, drop);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "BES touch queue coalesced: unreported gesture removed\n");
        }
    }
    bool was_down = t->host_down;
    t->x = px; t->y = py; t->host_down = down;
    unsigned tail = (t->queue_head + t->queue_count++) % ARRAY_SIZE(t->queue);
    uint8_t *r = t->queue[tail];
    memset(r, 0, 10);
    r[0] = down ? (was_down ? 7 : 6) : 0; /* finger 0: down/move/up */
    r[1] = px >> 4; r[2] = py >> 4;
    r[3] = (px << 4) | (py & 15);
    r[5] = 1; r[6] = 0xab;
    if (t->queue_count == 1) {
        memcpy(t->report, r, 10);
        /* A new event can also arrive between the previous ACK and the
         * driver's GPIO re-enable. Use the same delayed publication path. */
        t->irq_rearm_pending = true;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "BES touch input: index=%u down=%u x=%u y=%u queued=%u\n",
                  ++t->input_count, down, px, py, t->queue_count);
    touch_irq(s);
}

/* Explicit CST9217 model identity; these values describe our attached device,
 * not a measurement of a particular watch's module/firmware revision. */
static bool touch_read_byte(Best1503State *s, uint16_t reg, uint8_t *value)
{
    BesTouch *t = &s->touch;
    uint32_t v;
    unsigned shift;
    if (reg >= 0xd000 && reg < 0xd00a) {
        *value = t->report[reg - 0xd000];
        return true;
    }
    if (reg == 0xd00a || reg == 0xd00b) {
        /* O66 reads a 12-byte report; key data is unused when report[5].7=0. */
        *value = 0;
        return true;
    }
    if (reg >= 0xa002 && reg < 0xa004 && t->bootloader) {
        *value = reg == 0xa002 ? 0x55 : 0xb0;
        return true;
    }
    if (reg == 0xa004 && t->mem_address) { *value = 0; return true; }
    if (reg >= 0xa018 && reg < 0xa01c && t->mem_address) {
        v = t->mem_address == 0x077c ? 0xcaca9217 :
            t->mem_address == 0x7fc0 ? s->touch_module_id : 0;
        if (!v) { return false; }
        *value = v >> ((reg - 0xa018) * 8);
        return true;
    }
    if (reg == 0x0002 || reg == 0x0003) {
        *value = reg == 0x0002 ? t->sleep_mode : t->reply_mode;
        return true;
    }
    if (reg >= 0xd228 && reg <= 0xd23c) {
        /* O66 get_firmware_info (0x0c10ab04) reads 30 bytes from D21F.
         * Its unused auxiliary information tail is empty on this virtual IC. */
        *value = 0;
        return true;
    }
    switch (reg & ~3u) {
    case 0xd1fc: v = 0; break;
    case 0xd200: v = 0; break;
    case 0xd204: v = 0x92170001; break; /* IC type / project */
    case 0xd208: v = 1; break; /* version */
    case 0xd21c: v = 0x92170002; break; /* sum of the four info words */
    case 0xd220: v = 1; break;
    case 0xd224: v = 0xcaca9217; break; /* CST9217 identity used by P67 */
    case 0xd20c: v = 1; break;
    case 0xd1f4: v = 0x0014000a; break; /* 10 TX, 20 RX, no keys */
    case 0xd048:
        if (!(reg & 3)) { t->health++; }
        v = (s->touch_health_format ? (t->sleep_mode == 3 ? 0xa0 : 0x20) :
             0x20 | (t->health % 10)) | (t->side_flags << 8); break;
    default: return false;
    }
    shift = (reg & 3) * 8;
    *value = v >> shift;
    return true;
}

static bool touch_write_packet(Best1503State *s)
{
    BesTouch *t = &s->touch;
    if (!t->tx_count) { return true; }
    if (t->tx_count == 1 && t->tx[0] == 0xd0) {
        t->tx_count = 0; /* Address probe. */
        return true;
    }
    if (t->tx_count < 2) { return false; }
    uint16_t reg = (t->tx[0] << 8) | t->tx[1];
    unsigned count = t->tx_count - 2;
    t->pointer = reg;
    if (!count) {
        switch (reg) {
        case 0xd109: t->mode = 9; t->sleep_mode = 0; break;
        case 0xd11e: break; /* Enter command interface; scan mode is unchanged. */
        case 0xd11f: case 0xd120: /* Charger state */
        case 0xd123: case 0xd124: /* Wear state */
        case 0xd125: case 0xd126: /* Gyroscope state */
            t->reply_mode = reg & 0xff;
            {
                unsigned bit = reg <= 0xd120 ? 4 : reg <= 0xd124 ? 1 : 2;
                t->side_flags = (reg & 1) ? t->side_flags | bit : t->side_flags & ~bit;
            }
            break;
        case 0xd101: t->mode = 1; t->sleep_mode = 0; break;
        case 0xd105: t->mode = 5; t->sleep_mode = 1; break;
        case 0xd107: t->mode = 7; t->sleep_mode = 2; break;
        case 0xd104: t->mode = 4; t->sleep_mode = 3; break;
        default: break; /* Two-byte register-pointer selection. */
        }
        if (reg >= 0xd100 && reg <= 0xd11e) {
            t->reply_mode = reg & 0xff;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "BES touch mode command: reg=0x%04x mode=%u sleep=%u\n",
                          reg, t->mode, t->sleep_mode);
        }
        t->tx_count = 0; touch_irq(s); return true;
    }
    uint8_t v = t->tx[2];
    bool ok = false;
    if (reg == 0xa001 && count == 1 && (v == 0xaa || v == 0)) {
        t->bootloader = v == 0xaa; ok = true;
    } else if (reg == 0xa004 && count == 1 && v == 0xe8 && t->bootloader) {
        /* O66 .28 soft_exit_bootloader at 0x0c106f74 sends A0 04 E8. */
        t->bootloader = false;
        t->mode = t->reply_mode = 9;
        t->sleep_mode = 0;
        t->mem_address = 0;
        ok = true;
    } else if (reg == 0xa010 && count == 1 && (v == 1 || v == 0)) {
        t->mem_type = v; t->mem_address = 0; ok = true;
    } else if (reg == 0xa00c && count == 2) {
        uint16_t address = v | (t->tx[3] << 8);
        ok = (t->mem_type == 1 && address == 0x077c) ||
             (t->mem_type == 0 && address == 0x7fc0);
        t->mem_address = ok ? address : 0;
    } else if (reg == 0xa004 && count == 1 && v == 0xe4 && t->mem_address) {
        ok = true;
    } else if ((reg == 0x0005 && v == 0) || (reg == 0xd000 && v == 0xab)) {
        if (count != 1) { return false; }
        if (t->queue_count) {
            t->queue_count--;
            t->queue_head = (t->queue_head + 1) % ARRAY_SIZE(t->queue);
            qemu_log_mask(LOG_GUEST_ERROR, "BES touch report acknowledged: index=%u\n",
                          ++t->ack_count);
            /* ACK releases the data-ready line. If another report is queued,
             * publish it after the AP's GPIO EOI/mask/enable sequence. */
            s->gpio[s->touch_irq_pin / 32].input |= 1u << (s->touch_irq_pin % 32);
            if (t->queue_count) {
                memcpy(t->report, t->queue[t->queue_head], 10);
                t->irq_rearm_pending = true;
            } else {
                t->report[5] = 0;
                t->irq_rearm_pending = false;
                timer_del(s->touch_irq_timer);
            }
        }
        ok = true;
    } else if ((reg == 0xd109 || reg == 0xd119) && count == 1 && v == 0) {
        t->mode = t->reply_mode = t->sleep_mode = 0; ok = true;
    } else if ((reg == 0xd160 || reg == 0xd161) && count == 1) {
        ok = true;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "BES touch write: reg=0x%04x count=%u value=0x%02x %s\n",
                  reg, count, v, ok ? "accepted" : "unsupported");
    t->tx_count = 0;
    touch_irq(s);
    return ok;
}

/* DesignWare-style I2C interface from reg_i2cip.h. Only the optional
 * CST9217 at I2C3/0x5a is attached; other targets NACK. Empty TX FIFO is
 * a level interrupt. Slave transfers are synchronous, with a bounded RX FIFO.
 */
static uint32_t i2c_raw(BesI2c *i2c)
{
    return i2c->pending | (i2c->regs[0x6c / 4] & 1 ? 1 << 4 : 0) |
        (i2c->rx_count > i2c->regs[0x38 / 4] ? 1 << 2 : 0);
}

static void i2c_update(BesI2c *i2c)
{
    qemu_set_irq(i2c->irq, !!(i2c_raw(i2c) & i2c->regs[0x30 / 4]));
}

static bool i2c_valid(hwaddr offset)
{
    return (offset <= 0xa8 && offset != 0xc) ||
           offset == 0xf4 || offset == 0xf8 || offset == 0xfc;
}

static MemTxResult i2c_read(void *opaque, hwaddr offset, uint64_t *value,
                           unsigned size, MemTxAttrs attrs)
{
    BesI2c *i2c = opaque;
    if (!i2c_valid(offset)) {
        return MEMTX_ERROR;
    }
    *value = 0;
    if (offset >= 0x40 && offset <= 0x68) {
        if (offset == 0x40) {
            i2c->pending = i2c->abort_source = 0;
        } else {
            static const unsigned bits[] = {0, 1, 3, 5, 6, 7, 8, 9, 10, 11};
            i2c->pending &= ~(1u << bits[(offset - 0x44) / 4]);
            if (offset == 0x54) {
                i2c->abort_source = 0;
            }
        }
        i2c_update(i2c);
        return MEMTX_OK;
    }
    switch (offset) {
    case 0x10: /* Reading an empty RX FIFO latches RX_UNDER. */
        if (i2c->rx_count) {
            *value = i2c->rx[i2c->rx_head++ % ARRAY_SIZE(i2c->rx)];
            i2c->rx_count--;
            i2c_update(i2c);
            break;
        }
        if (i2c->regs[0x6c / 4] & 1) {
            i2c->pending |= 1;
            i2c_update(i2c);
        }
        break;
    case 0x2c: *value = i2c_raw(i2c) & i2c->regs[0x30 / 4]; break;
    case 0x34: *value = i2c_raw(i2c); break;
    case 0x70: *value = 6 | (i2c->rx_count ? 8 : 0) |
                            (i2c->rx_count == ARRAY_SIZE(i2c->rx) ? 16 : 0); break;
    case 0x74: /* TXFLR */
        break;
    case 0x78: *value = i2c->rx_count; break;
    case 0x80: *value = i2c->abort_source; break;
    case 0x9c: *value = i2c->regs[0x6c / 4] & 1; break;
    case 0xf4: *value = (7 << 16) | (7 << 8) | 0xee; break;
    case 0xf8: /* Controller revision unknown. */ break;
    case 0xfc: *value = 0x44570140; break; /* DesignWare component signature */
    default: *value = i2c->regs[offset / 4]; break;
    }
    return MEMTX_OK;
}

static MemTxResult i2c_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size, MemTxAttrs attrs)
{
    BesI2c *i2c = opaque;
    if (!i2c_valid(offset)) {
        return MEMTX_ERROR;
    }
    if (offset == 0x10) {
        if (!(i2c->regs[0x6c / 4] & 1)) {
            return MEMTX_OK; /* Disabled controller does not issue commands. */
        }
        Best1503State *s = i2c->machine;
        BesTouch *t = &s->touch;
        if (i2c == &s->i2c[s->touch_i2c] && (i2c->regs[1] & 0x3ff) == 0x5a &&
            touch_powered(s)) {
            bool ok = true;
            if (value & 0x100) {
                if (!t->slave_reading) {
                    ok = touch_write_packet(s);
                    t->slave_reading = true;
                    t->read_count = 0;
                    qemu_log_mask(LOG_GUEST_ERROR, "BES touch read: reg=0x%04x\n", t->pointer);
                }
                uint8_t byte;
                if (!ok || i2c->rx_count == ARRAY_SIZE(i2c->rx) ||
                    !touch_read_byte(s, t->pointer + t->read_count, &byte)) {
                    ok = false;
                } else {
                    t->read_count++;
                    i2c->rx[(i2c->rx_head + i2c->rx_count++) % ARRAY_SIZE(i2c->rx)] = byte;
                }
            } else {
                if (t->slave_reading) {
                    t->slave_reading = false;
                    t->tx_count = 0;
                }
                if (t->tx_count == ARRAY_SIZE(t->tx)) {
                    ok = false;
                } else {
                    t->tx[t->tx_count++] = value;
                }
            }
            if (value & 0x200) {
                if (!(value & 0x100)) { ok = ok && touch_write_packet(s); }
                t->slave_reading = false;
                i2c->pending |= 1 << 9;
            }
            if (!ok) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "BES touch I2C rejected: reg=0x%04x read=%u rx=%u command=0x%03" PRIx64 "\n",
                              t->pointer, t->read_count, i2c->rx_count, value);
                i2c->abort_source = 1 << 3;
                i2c->pending |= (1 << 6) | (1 << 9);
                t->tx_count = 0;
            }
            i2c_update(i2c);
            return MEMTX_OK;
        }
        i2c->abort_source = i2c->regs[0] & 1 ?
            (i2c->regs[1] & (1 << 12) ? 2 : 1) : 1 << 11;
        i2c->pending |= (1 << 6) | (1 << 9);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES I2C at 0x%08" HWADDR_PRIx
                      " target=0x%03x command=0x%03" PRIx64
                      " NACK (no attached device)\n",
                      i2c->base, i2c->regs[1] & 0x3ff, value);
    } else if (offset == 0x6c) {
        i2c->regs[offset / 4] = value & 1;
        if (!(value & 1)) {
            i2c->pending = i2c->abort_source = 0;
            i2c->rx_head = i2c->rx_count = 0;
            if (i2c == &i2c->machine->i2c[i2c->machine->touch_i2c]) {
                i2c->machine->touch.tx_count = 0;
                i2c->machine->touch.slave_reading = false;
            }
        } else if (value & 2) {
            i2c->abort_source = 1 << 16; /* Software ABORT */
            i2c->pending |= 1 << 6;
        }
    } else if (offset == 0x2c || offset == 0x34 ||
               (offset >= 0x40 && offset <= 0x80 && offset != 0x7c) ||
               offset == 0x9c || offset >= 0xf4) {
        return MEMTX_OK; /* Read-only status/clear-on-read registers. */
    } else {
        i2c->regs[offset / 4] = value;
    }
    i2c_update(i2c);
    return MEMTX_OK;
}

static const MemoryRegionOps i2c_ops = {
    .read_with_attrs = i2c_read,
    .write_with_attrs = i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static bool lcdc_panel_powered(BesLcdc *l);

static void panel_te_update_irq(Best1503State *s)
{
    BesGpio *g = &s->gpio[s->panel_te_pin / 32];
    uint32_t mask = ldl_le_p((uint8_t *)memory_region_get_ram_ptr(&s->psc) + 0x80 + (s->panel_te_pin / 32) * 4);
    bool active = !!(g->pending & g->regs[0x30 / 4] & ~g->regs[0x34 / 4] & mask);
    if (s->touch.enabled) {
        g = &s->gpio[s->touch_irq_pin / 32];
        mask = ldl_le_p((uint8_t *)memory_region_get_ram_ptr(&s->psc) + 0x80 + (s->touch_irq_pin / 32) * 4);
        active |= !!(g->pending & g->regs[0x30 / 4] & ~g->regs[0x34 / 4] & mask & (1u << (s->touch_irq_pin % 32)));
    }
    qemu_set_irq(s->panel_te_irq,
                 active);
}

/* Q66 hal_gpio_get_irq_status copies three PSC GPIO summary words into
 * its ISR before GPIO EOI. Original 0c92e96c/e9b8 enable/disable these
 * masks, and the pointer table at2cc20fdc identifies the status words.
 */
static MemTxResult gpio_summary_read(void *opaque, hwaddr offset, uint64_t *value,
                                     unsigned size, MemTxAttrs attrs)
{
    Best1503State *s = opaque;
    if (offset < 12) {
        *value = ldl_le_p((uint8_t *)memory_region_get_ram_ptr(&s->psc) + 0x80 + offset);
    } else {
        *value = s->gpio[(offset - 12) / 4].pending;
    }
    return MEMTX_OK;
}

static MemTxResult gpio_summary_write(void *opaque, hwaddr offset, uint64_t value,
                                      unsigned size, MemTxAttrs attrs)
{
    Best1503State *s = opaque;
    if (offset < 12) {
        stl_le_p((uint8_t *)memory_region_get_ram_ptr(&s->psc) + 0x80 + offset, value);
        if (s->touch.enabled && offset == (s->touch_irq_pin / 32) * 4) {
            touch_irq(s);
        }
        panel_te_update_irq(s);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps gpio_summary_ops = {
    .read_with_attrs = gpio_summary_read,
    .write_with_attrs = gpio_summary_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
};

static void panel_te_update(Best1503State *s)
{
    if (!lcdc_panel_powered(&s->lcdc)) {
        s->panel_te_enabled = s->panel_awake = s->panel_display_on = false;
        s->panel_partial_mode = s->panel_idle_mode = false;
    }
    if (!s->panel_te_fixture) {
        return;
    }
    if (s->panel_te_enabled && s->panel_awake && s->panel_display_on) {
        if (!timer_pending(s->panel_te_timer)) {
            timer_mod(s->panel_te_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 8333333);
        }
    } else {
        timer_del(s->panel_te_timer);
        s->panel_te_high = false;
        s->gpio[s->panel_te_pin / 32].input &= ~(1u << (s->panel_te_pin % 32));
        s->gpio[s->panel_te_pin / 32].pending &= ~(1u << (s->panel_te_pin % 32));
        panel_te_update_irq(s);
    }
}

/* Original TE pin getter returns 65; Q66 GPIO dispatch uses IRQ44, unlike
 * the public SDK's IRQ9. This fixture toggles a provisional 60 Hz signal;
 * only configured input/edge/polarity determines its latched raw status.
 * Panel timings and GPU pixel generation are not simulated.
 */
static void panel_te_edge(void *opaque)
{
    Best1503State *s = opaque;
    BesGpio *g = &s->gpio[s->panel_te_pin / 32];
    uint32_t bit = 1u << (s->panel_te_pin % 32);
    if (!s->panel_te_fixture || !lcdc_panel_powered(&s->lcdc) ||
        !s->panel_te_enabled || !s->panel_awake || !s->panel_display_on) {
        panel_te_update(s);
        return;
    }
    s->panel_te_high = !s->panel_te_high;
    g->input = (g->input & ~bit) | (s->panel_te_high ? bit : 0);
    if (!(g->regs[1] & bit) && (g->regs[0x38 / 4] & bit) &&
        !!(g->regs[0x3c / 4] & bit) == s->panel_te_high) {
        g->pending |= bit;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES experimental panel TE edge: pin=%u level=%u (60 Hz fixture)\n",
                      s->panel_te_pin, s->panel_te_high);
    }
    panel_te_update_irq(s);
    panel_te_update(s);
}

/* GPIO v2 atomic SET/CLR register bank from reg_gpio_v2.h. Inputs read low
 * except for the explicitly enabled panel TE fixture on pin65.
 * Output EXT_PORT bits reflect DR when DDR marks the pin as an output.
 * GPIO_CTL alternate routing and debounce timing are configuration only.
 */
static int gpio_config_slot(hwaddr offset, bool *clear)
{
    static const unsigned set[] = {0x00, 0x04, 0x08, 0x30, 0x34,
                                   0x38, 0x3c, 0x48};
    static const unsigned clr[] = {0x0c, 0x10, 0x14, 0x18, 0x1c,
                                   0x20, 0x24, 0x28};
    for (unsigned i = 0; i < ARRAY_SIZE(set); i++) {
        if (offset == set[i] || offset == clr[i]) {
            *clear = offset == clr[i];
            return set[i] / 4;
        }
    }
    return -1;
}

static MemTxResult gpio_read(void *opaque, hwaddr offset, uint64_t *value,
                             unsigned size, MemTxAttrs attrs)
{
    BesGpio *g = opaque;
    bool clear;
    int slot = gpio_config_slot(offset, &clear);
    if (slot >= 0) {
        *value = g->regs[slot];
    } else if (offset == 0x50) {
        *value = (g->regs[0] & g->regs[1]) | (g->input & ~g->regs[1]);
    } else if (offset == 0x40) {
        *value = g->pending & g->regs[0x30 / 4] & ~g->regs[0x34 / 4];
    } else if (offset == 0x44) {
        *value = g->pending;
    } else if (offset == 0x4c) {
        *value = 0;
    } else if (offset == 0x60) {
        *value = g->regs[offset / 4];
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES unsupported GPIO read at 0x%08" HWADDR_PRIx "\n",
                      g->base + offset);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult gpio_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size, MemTxAttrs attrs)
{
    BesGpio *g = opaque;
    Best1503State *s = g->machine;
    uint32_t touch_bit = 1u << (s->touch_irq_pin % 32);
    bool touch_rearm = s->touch.enabled && g == &s->gpio[s->touch_irq_pin / 32] &&
        offset == 0x4c && (value & touch_bit) && s->touch.queue_count &&
        !touch_irq_ready(s);
    bool clear;
    int slot = gpio_config_slot(offset, &clear);
    if (slot >= 0) {
        g->regs[slot] = clear ? g->regs[slot] & ~value :
                               g->regs[slot] | value;
    } else if (offset == 0x4c) {
        g->pending &= ~value;
        if (touch_rearm) {
            /* The driver's setup_irq() clears pending before unmask/enable.
             * If it erased an already published edge, release the pin and
             * sample again after configuration is complete. ISR EOI while
             * delivery is enabled must still wait for the report's ACK. */
            s->touch.irq_rearm_pending = true;
            timer_del(s->touch_irq_timer);
        }
    } else if (offset == 0x60) {
        g->regs[offset / 4] = value;
    } else if (offset != 0x40 && offset != 0x44 && offset != 0x4c &&
               offset != 0x50) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES unsupported GPIO write at 0x%08" HWADDR_PRIx "\n",
                      g->base + offset);
        return MEMTX_ERROR;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "BES GPIO 0x%08" HWADDR_PRIx " write 0x%08x\n",
                  g->base + offset, (uint32_t)value);
    gh_pins_update(g->machine);
    if (g->machine->touch.enabled) {
        if (!touch_powered(g->machine)) { touch_reset(g->machine); }
        else { touch_irq(g->machine); }
    }
    panel_te_update(g->machine);
    panel_te_update_irq(g->machine);
    return MEMTX_OK;
}

static const MemoryRegionOps gpio_ops = {
    .read_with_attrs = gpio_read,
    .write_with_attrs = gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
};

/* Original Q66 configuration and optional bounded command-engine fixture.
 * The fixture supplies the original RM690C0_XINLI ID tuple and records writes;
 * Separate optional frame capture supports a bounded RGB565 GRA layer.
 * Vendor register effects and other layers are not modeled. The separate
 * TE fixture tracks only the original TE/awake/display-on command sequence.
 * REG_2A8 is a Q66 configuration word absent from the downloaded SDK header;
 * its storage semantics follow the original RMW, not an assumed bit meaning.
 */
static bool lcdc_config_offset(hwaddr offset)
{
    /* Stock post-ID layer size/position, SMPN porches and TE setup.
     * GEN_FRAME at 0x210 bit29 is handled separately in the write path.
     */
    static const unsigned config[] = {0xc0, 0xe8, 0xec, 0xf0, 0xf4, 0xfc,
                                      0x100, 0x104, 0x108,
                                      0x114, 0x118, 0x11c, 0x120, 0x124,
                                      0x180, 0x188, 0x194, 0x1a8, 0x1c0,
                                      0x1dc, 0x1e8, 0x1ec, 0x1f8, 0x200, 0x210,
                                      0x214, 0x21c, 0x220, 0x228, 0x22c, 0x230,
                                      0x264, 0x27c, 0x284, 0x2a8};
    for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
        if (offset == config[i]) {
            return true;
        }
    }
    return false;
}

static bool lcdc_panel_powered(BesLcdc *l)
{
    Best1503State *s = l->machine;
    uint32_t masks[] = {s->panel_power0, s->panel_power1, s->panel_power2};
    for (unsigned i = 0; i < 3; i++) {
        if ((s->gpio[i].regs[0] & s->gpio[i].regs[1] & masks[i]) != masks[i]) {
            return false;
        }
    }
    return true;
}

static bool lcdc_panel_selected(BesLcdc *l)
{
    BesGpio *g = &l->machine->gpio[1];
    /* Stock GPIO41 is a manual, active-low chip select. */
    return (g->regs[1] & (1 << 9)) && !(g->regs[0] & (1 << 9));
}

static void lcdc_update_irq(BesLcdc *l)
{
    qemu_set_irq(l->irq, (l->status & l->regs[0x1c0 / 4]) != 0);
}

/* Completed LCD snapshots for local viewers, independent of QEMU's optional
 * pixman/screendump support. The little-endian 32-byte header is followed by
 * packed RGB565LE pixels: R[15:11], G[10:5], B[4:0]. This matches the
 * two-byte /dev/fb0 screenshot path in smash.lua.
 * Readers retry if the sequence is odd or changes. */
#define LCD_SHARED_HEADER 32
#define LCD_SHARED_SIZE (LCD_SHARED_HEADER + 1024 * 1024 * 2)
#define LCD_SHARED_RGB565LE 1

static void lcdc_shared_init(BesLcdc *l)
{
    if (!l->frame_file) {
        return;
    }
#ifndef _WIN32
    int fd = open(l->frame_file, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || ftruncate(fd, LCD_SHARED_SIZE) < 0) {
        error_report("Cannot create LCD framebuffer %s: %s",
                     l->frame_file, strerror(errno));
        if (fd >= 0) { close(fd); }
        exit(1);
    }
    void *mapped = mmap(NULL, LCD_SHARED_SIZE, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
    close(fd);
    if (mapped == MAP_FAILED) {
        error_report("Cannot map LCD framebuffer %s: %s",
                     l->frame_file, strerror(errno));
        exit(1);
    }
    l->shared_frame = mapped;
    memset(l->shared_frame, 0, LCD_SHARED_SIZE);
    memcpy(l->shared_frame, "Q66FRAME", 8);
    stl_le_p(l->shared_frame + 8, 1);
    stl_le_p(l->shared_frame + 28, LCD_SHARED_RGB565LE);
#else
    error_report("lcd-frame-file requires a POSIX host");
    exit(1);
#endif
}

static void lcdc_shared_publish(BesLcdc *l)
{
    uint8_t *frame = l->shared_frame;
    if (!frame) {
        return;
    }
    uint32_t *sequence = (uint32_t *)(frame + 12);
    uint32_t next = l->shared_sequence + 2;
    if (!next) { next = 2; }
    qatomic_set(sequence, cpu_to_le32(next - 1));
    smp_wmb();
    stl_le_p(frame + 16, l->frame_width);
    stl_le_p(frame + 20, l->frame_height);
    stl_le_p(frame + 24, l->frame_width * 2);
    memcpy(frame + LCD_SHARED_HEADER, l->pixels,
           l->frame_width * l->frame_height * 2);
    smp_wmb();
    qatomic_set(sequence, cpu_to_le32(next));
    l->shared_sequence = next;
}

static void lcdc_draw(void *opaque)
{
    BesLcdc *l = opaque;
    if (!l->frame_valid) {
        return;
    }
    DisplaySurface *surface = qemu_console_surface(l->console);
    for (unsigned y = 0; y < l->frame_height; y++) {
        uint32_t *dest = (uint32_t *)(surface_data(surface) +
                                     y * surface_stride(surface));
        for (unsigned x = 0; x < l->frame_width; x++) {
            unsigned off = (y * l->frame_width + x) * 2;
            unsigned p = l->pixels[off] | l->pixels[off + 1] << 8;
            /* Match smash.lua's RGB565 screenshot conversion exactly:
             * left-align each field, with no channel swap or low-bit fill. */
            unsigned r = ((p >> 11) & 31) << 3;
            unsigned g = ((p >> 5) & 63) << 2;
            unsigned b = (p & 31) << 3;
            dest[x] = (r << 16) | (g << 8) | b;
        }
    }
    dpy_gfx_update(l->console, 0, 0, l->frame_width, l->frame_height);
}

static const GraphicHwOps lcdc_gfx_ops = {
    .invalidate = lcdc_draw,
    .gfx_update = lcdc_draw,
};

static void lcdc_frame_cancel(BesLcdc *l)
{
    timer_del(l->frame_timer);
    l->frame_busy = false;
    l->regs[0x224 / 4] = 0;
}

static bool lcdc_frame_range(uint32_t addr, uint64_t length)
{
    /* Validate the entire pitched span before any read. No MMIO or Flash. */
    static const uint32_t bases[] = {0x20000000, 0x00200000, 0x38000000,
                                     0x18000000, 0x3c000000, 0x1c000000};
    for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
        uint64_t limit = (uint64_t)bases[i] + (i < 2 ? 0x160000 : 32 * MiB);
        if (addr >= bases[i] && (uint64_t)addr + length <= limit) {
            return true;
        }
    }
    return false;
}

static void lcdc_frame_complete(void *opaque)
{
    BesLcdc *l = opaque;
    lcdc_frame_cancel(l);
    if (!lcdc_panel_powered(l) || !lcdc_panel_selected(l) ||
        !(l->regs[0x210 / 4] & (1U << 29)) ||
        !(l->regs[0x264 / 4] & 1)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES experimental LCD frame cancelled: power/reset/CS/enable\n");
        return;
    }
    l->frame_valid = true;
    l->frame_count++;
    lcdc_shared_publish(l);
    /* Resizing the Cocoa surface every frame is extremely expensive. The
     * panel dimensions are fixed for this device, so resize only when they
     * actually change. */
    if (qemu_console_surface(l->console) == NULL ||
        surface_width(qemu_console_surface(l->console)) != l->frame_width ||
        surface_height(qemu_console_surface(l->console)) != l->frame_height) {
        qemu_console_resize(l->console, l->frame_width, l->frame_height);
    }
    lcdc_draw(l);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "BES experimental LCD frame captured: index=%u address=0x%08x "
                  "width=%u height=%u pitch=%u (RGB565 snapshot)\n",
                  l->frame_count, l->frame_address, l->frame_width,
                  l->frame_height, l->frame_pitch);
    /* Original handler checks TXC bit21 and GRA frame bit27. Only set after
     * the bounded snapshot finishes; no periodic or synthetic TE events.
     */
    l->status |= (1U << 21) | (1U << 27);
    lcdc_update_irq(l);
}

static MemTxResult lcdc_frame_start(BesLcdc *l, uint32_t value)
{
    uint32_t dimensions = l->regs[0x104 / 4];
    unsigned width = dimensions & 0xfff, height = (dimensions >> 16) & 0xfff;
    unsigned format = (l->regs[0x264 / 4] >> 8) & 15;
    unsigned bpp = format == 2 ? 3 : 2;
    unsigned pitch = l->regs[0xfc / 4] & 0xffff;
    uint32_t address = l->regs[0xf4 / 4];
    uint64_t length = height ? (uint64_t)(height - 1) * pitch + width * bpp : 0;
    if (!l->machine->lcd_frame_capture || value != 1 || l->frame_busy || l->busy ||
        !lcdc_panel_powered(l) || !lcdc_panel_selected(l) ||
        !(l->regs[0x210 / 4] & (1U << 29)) ||
        !(l->regs[0x264 / 4] & 1) ||
        (l->regs[0x264 / 4] & 0xfe) || (format != 0 && format != 2) ||
        (l->regs[0x100 / 4] & 0x0fff0fff) || /* Position origin only. */
        dimensions != l->regs[0x108 / 4] || /* No scaling. */
        dimensions != l->regs[0x118 / 4] ||
        !width || width > 1024 || !height || height > 1024 ||
        pitch < width * bpp || (format == 0 && ((pitch | address) & 1)) ||
        !lcdc_frame_range(address, length)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES experimental LCD frame rejected: address=0x%08x "
                      "width=%u height=%u pitch=%u gra=0x%08x busy=%d CS=%d\n",
                      address, width, height, pitch, l->regs[0x264 / 4],
                      l->frame_busy, lcdc_panel_selected(l));
        return MEMTX_ERROR;
    }
    for (unsigned y = 0; y < height; y++) {
        if (format == 0) {
            cpu_physical_memory_read(address + y * pitch, l->pixels + y * width * 2, width * 2);
        } else {
            uint8_t row[1024 * 3];
            cpu_physical_memory_read(address + y * pitch, row, width * 3);
            for (unsigned x = 0; x < width; x++) {
                uint16_t rgb = ((row[3*x+2] >> 3) << 11) | ((row[3*x+1] >> 2) << 5) | (row[3*x] >> 3);
                stw_le_p(l->pixels + (y * width + x) * 2, rgb);
            }
        }
    }
    l->frame_address = address;
    l->frame_pitch = pitch;
    l->frame_width = width;
    l->frame_height = height;
    l->frame_valid = false;
    l->frame_busy = true;
    l->regs[0x224 / 4] = 1;
    /* One millisecond functional transfer delay; not measured panel timing. */
    timer_mod(l->frame_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
    return MEMTX_OK;
}

static void lcdc_spi_complete(void *opaque)
{
    BesLcdc *l = opaque;
    uint32_t id = l->machine->panel_id;
    const uint8_t ids[] = {id >> 16, id >> 8, id};
    l->busy = false;
    l->regs[0x280 / 4] = 0;
    if (!lcdc_panel_powered(l) || !lcdc_panel_selected(l)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES experimental LCD SPI cancelled: panel power/reset/CS\n");
        return;
    }
    if (l->reading) {
        if (l->command == 0x0a) {
            Best1503State *s = l->machine;
            /* MIPI DCS GET_POWER_MODE: booster (7), idle (6), sleep-out
             * (4), normal mode (3), display-on (2). Derive from completed
             * panel commands, including the stock wake/partial sequence.
             */
            l->rx = (s->panel_awake ? 0x90 : 0) |
                    (s->panel_idle_mode ? 0x40 : 0) |
                    (s->panel_partial_mode ? 0 : 8) |
                    (s->panel_display_on ? 4 : 0);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "BES panel DCS power mode: command=0x0a value=0x%02x\n", l->rx);
        } else if (l->command == 0x00) {
            l->rx = 0; /* DCS NOP has no identification data. */
        } else if (l->command == 0x04) {
            l->rx = (ids[2] << 16) | (ids[1] << 8) | ids[0];
            qemu_log_mask(LOG_GUEST_ERROR, "BES panel DCS ID: 0x%06x\n", id);
        } else {
            l->rx = ids[l->command - 0xda];
            qemu_log_mask(LOG_GUEST_ERROR,
                          "BES experimental panel ID: command=0x%02x value=0x%02x "
                          "(RM690C0_XINLI fixture)\n", l->command, l->rx);
        }
        l->rx_valid = true;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES experimental panel TX: command=0x%02x length=%u data=",
                      l->command, l->length);
        for (unsigned i = 0; i < l->length; i++) {
            qemu_log_mask(LOG_GUEST_ERROR, "%02x ", l->parameters[i]);
        }
        qemu_log_mask(LOG_GUEST_ERROR, "(capture only)\n");
        {
            Best1503State *s = l->machine;
            bool no_parameter = !l->length || (l->length == 1 && l->parameters[0] == 0);
            if (l->command == 0x01 && no_parameter) {
                s->panel_te_enabled = s->panel_awake = s->panel_display_on = false;
                s->panel_partial_mode = s->panel_idle_mode = false;
            } else if (l->command == 0x35 && l->length == 1 &&
                       (l->parameters[0] == 0 || l->parameters[0] == 2)) {
                /* DCS V-blank TE (N66) or the Q66 vendor's TE setting. */
                s->panel_te_enabled = true;
            } else if (l->command == 0x34 && no_parameter) {
                s->panel_te_enabled = false;
            } else if (l->command == 0x11 && no_parameter) {
                s->panel_awake = true;
            } else if (l->command == 0x10 && no_parameter) {
                s->panel_awake = false;
            } else if (l->command == 0x29 && no_parameter) {
                s->panel_display_on = true;
            } else if (l->command == 0x28 && no_parameter) {
                s->panel_display_on = false;
            } else if (l->command == 0x12 && no_parameter) {
                s->panel_partial_mode = true;
            } else if (l->command == 0x13 && no_parameter) {
                s->panel_partial_mode = false;
            } else if (l->command == 0x38 && no_parameter) {
                s->panel_idle_mode = false;
            } else if (l->command == 0x39 && no_parameter) {
                s->panel_idle_mode = true;
            }
            panel_te_update(s);
        }
    }
    l->status |= 1 << 18;
    lcdc_update_irq(l);
}

static void lcdc_spi_sync_due(BesLcdc *l)
{
    /* Firmware polls SPI completion against the free-running system timer.
     * That counter advances on reads even when the main-loop callback has
     * not run yet. Keep the observable SPI state on the same virtual clock,
     * without shortening the transfer or accepting an early busy retrigger.
     */
    if (l->busy && timer_pending(l->spi_timer) &&
        timer_expired(l->spi_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
        timer_del(l->spi_timer);
        lcdc_spi_complete(l);
    }
}

static MemTxResult lcdc_spi_start(BesLcdc *l, uint32_t control)
{
    uint32_t header = l->regs[0x284 / 4];
    unsigned instruction = header >> 24;
    unsigned length = control & 2 ? ((control & 0x1ff0) + 16) >> 7 : 0;
    unsigned rxbits = ((l->regs[0x180 / 4] >> 16) & 0xff) + 1;
    uint8_t command = header >> 8;
    if (!l->machine->panel_id_fixture) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES LCDC SPI transfer unavailable: control=0x%08x "
                      "header=0x%08x (no panel/SPI backend)\n", control, header);
        return MEMTX_ERROR;
    }
    /* Older drivers assert START for the header, then append payload length
     * and data before waiting. This is one transfer, not a busy retrigger. */
    bool append_data = l->busy && !l->reading && !l->length &&
        l->regs[0x280 / 4] == 1 && instruction == 2 &&
        command == l->command && length > 0;
    if ((l->busy && !append_data) || l->frame_busy || !lcdc_panel_powered(l) || !lcdc_panel_selected(l) ||
        !(l->regs[0x180 / 4] & 8) || !(control & 1) ||
        control & ~0x1ff3 || header & 0x00ff00ff ||
        (instruction != 2 && instruction != 3) || length > 32 ||
        (instruction == 2 && control != (length ? ((length << 7) - 16) | 3 : 1)) ||
        (instruction == 3 && (control != 1 || rxbits != (command == 0x04 ? 24 : 8) ||
                             l->regs[0x27c / 4] != 0 ||
                             (command != 0x00 && command != 0x04 && command != 0x0a && (command < 0xda || command > 0xdc))))) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES experimental LCD SPI rejected: control=0x%08x "
                      "header=0x%08x busy=%d powered=%d rxbits=%u\n",
                      control, header, l->busy, lcdc_panel_powered(l), rxbits);
        return MEMTX_ERROR;
    }
    l->command = command;
    l->length = length;
    l->reading = instruction == 3;
    l->rx_valid = false;
    for (unsigned i = 0; i < length; i++) {
        /* Stock packs four parameter bytes MSB first in successive words. */
        l->parameters[i] = l->regs[(0x288 / 4) + i / 4] >> (24 - 8 * (i % 4));
    }
    l->busy = true;
    l->regs[0x280 / 4] = control;
    timer_mod(l->spi_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 20 * 1000);
    return MEMTX_OK;
}

static MemTxResult lcdc_read(void *opaque, hwaddr offset, uint64_t *value,
                             unsigned size, MemTxAttrs attrs)
{
    BesLcdc *l = opaque;
    lcdc_spi_sync_due(l);
    if (lcdc_config_offset(offset)) {
        *value = l->regs[offset / 4];
        return MEMTX_OK;
    }
    if (offset == 0x1c4) {
        *value = l->status;
        return MEMTX_OK;
    }
    if (offset == 0x280 && l->machine->panel_id_fixture) {
        *value = l->regs[offset / 4];
        return MEMTX_OK;
    }
    if (offset == 0x224 && l->machine->lcd_frame_capture) {
        *value = l->regs[offset / 4];
        return MEMTX_OK;
    }
    if (offset == 0x140 && l->machine->panel_id_fixture && l->rx_valid) {
        *value = l->rx;
        l->rx_valid = false;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "BES unsupported LCDC read at 0x%08" HWADDR_PRIx "\n",
                  0x40200000 + offset);
    return MEMTX_ERROR;
}

static MemTxResult lcdc_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size, MemTxAttrs attrs)
{
    BesLcdc *l = opaque;
    lcdc_spi_sync_due(l);
    if (offset == 0x210 && (value & (1U << 29)) &&
        !l->machine->lcd_frame_capture) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES LCDC frame generator unavailable: control=0x%08x "
                      "(no frame DMA/pixel backend)\n", (uint32_t)value);
        return MEMTX_ERROR;
    }
    if (offset == 0x224 && l->machine->lcd_frame_capture) {
        if (!(value & 1)) {
            lcdc_frame_cancel(l);
            return MEMTX_OK;
        }
        return lcdc_frame_start(l, value);
    }
    if (lcdc_config_offset(offset)) {
        l->regs[offset / 4] = value;
        if ((offset == 0x210 && !(value & (1U << 29))) ||
            (offset == 0x264 && !(value & 1))) {
            lcdc_frame_cancel(l);
        }
        if (offset == 0x1c0) {
            lcdc_update_irq(l);
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES LCDC config 0x%08" HWADDR_PRIx " = 0x%08x\n",
                      0x40200000 + offset, (uint32_t)value);
        return MEMTX_OK;
    }
    if (offset == 0x1c4) {
        l->status &= value; /* Original Q66 clears bit18 by read/AND/write. */
        lcdc_update_irq(l);
        return MEMTX_OK;
    }
    if (offset == 0x1c8) {
        return MEMTX_OK; /* No display IRQ routing modeled. */
    }
    if (offset == 0x280) {
        /* Drivers may program the length/enables before asserting START.
         * This is register configuration, not a second SPI transfer. */
        if (!(value & 1) && !(value & ~0x1ff3u) && !l->busy) {
            l->regs[offset / 4] = value;
            return MEMTX_OK;
        }
        return lcdc_spi_start(l, value);
    }
    if (offset >= 0x288 && offset < 0x2a8 && l->machine->panel_id_fixture) {
        l->regs[offset / 4] = value;
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "BES unsupported LCDC write at 0x%08" HWADDR_PRIx "\n",
                  0x40200000 + offset);
    return MEMTX_ERROR;
}

static const MemoryRegionOps lcdc_ops = {
    .read_with_attrs = lcdc_read,
    .write_with_attrs = lcdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
};

static void best1503_peripheral_reset(void *opaque)
{
    Best1503State *s = opaque;
    timer_del(s->bt_wake_timer);
    timer_del(s->bt_tx_timer);
    timer_del(s->bt_rx_timer);
    timer_del(s->gpu_timer);
    timer_del(s->touch_irq_timer);
    if (s->gpu_work) {
        qemu_log_mask(LOG_GUEST_ERROR, "BES GPU render cancelled: id=%u system-reset\n",
                      s->gpu_pending_id);
        gpu_sw_free(s->gpu_work);
        s->gpu_work = NULL;
    }
    memset(s->gpu_sw_regs, 0, sizeof(s->gpu_sw_regs));
    timer_del(s->panel_te_timer);
    s->panel_te_enabled = s->panel_awake = s->panel_display_on = false;
    s->panel_partial_mode = s->panel_idle_mode = false;
    s->panel_te_high = false;
    timer_del(s->lcdc.spi_timer);
    lcdc_frame_cancel(&s->lcdc);
    s->lcdc.frame_valid = false;
    s->lcdc.frame_count = 0;
    if (s->lcdc.shared_frame) {
        /* Do not show an old frame while a reset is still booting. Keep the
         * producer counter monotonic so a reader cannot reuse stale pixels. */
        qatomic_set((uint32_t *)(s->lcdc.shared_frame + 12), 0);
    }
    s->lcdc.busy = s->lcdc.reading = s->lcdc.rx_valid = false;
    s->lcdc.status = s->lcdc.rx = s->lcdc.length = 0;
    s->lcdc.command = 0;
    memset(s->lcdc.parameters, 0, sizeof(s->lcdc.parameters));
    s->bt_tx_done = false;
    s->bt_rx_pending = s->bt_rx_owned = false;
    s->bt_h4_head = s->bt_h4_count = s->bt_h4_used = 0;
    s->bt_response_head = s->bt_response_count = 0;
    s->bt_hci_stack_started = false;
    if (s->bt_hci_reset_fixture) {
        uint32_t zero = 0;
        cpu_physical_memory_write(0xc0004000, &zero, sizeof(zero));
    }
    bt_rx_update_irq(s);
    bt_tx_update_irq(s);
    memset(s->bt_iq_sample, 0, sizeof(s->bt_iq_sample));
    for (unsigned i = 0; i < 2; i++) {
        memset(s->cmu[i].regs, 0, sizeof(s->cmu[i].regs));
        memset(s->cache[i].regs, 0, sizeof(s->cache[i].regs));
        memset(s->nor[i].regs, 0, sizeof(s->nor[i].regs));
        memset(s->nor[i].status, 0, sizeof(s->nor[i].status));
        nor_check_complete(&s->nor[i]);
        s->nor[i].write_enabled = false;
        s->nor[i].program_remaining = s->nor[i].program_address = 0;
        s->nor[i].burst_wrap = 0x10;
        s->nor[i].rx_pos = s->nor[i].rx_len = s->nor[i].tx_len = 0;
        s->nor[i].nand_loading = false;
        s->nor[i].nand_remaining = s->nor[i].nand_page = s->nor[i].nand_column = 0;
        memset(s->nor[i].nand_features, 0, sizeof(s->nor[i].nand_features));
        if (s->nor[i].nand_data) {
            nand_update_drq(&s->nor[i]);
        }
    }
    s->cmu[1].regs[0] = s->revision_id << 16;
    memset(s->ispi.regs, 0, sizeof(s->ispi.regs));
    memset(s->ispi.analog, 0, sizeof(s->ispi.analog));
    memset(s->ispi.page, 0, sizeof(s->ispi.page));
    s->ispi.head = s->ispi.count = 0;
    s->ispi.logged_reads = 0;
    memset(s->spi0.regs, 0, sizeof(s->spi0.regs));
    gh_reset(&s->spi0);
    spi0_dma_update(&s->spi0);
    s->spi0.selected = s->spi0.reset_high = false;
    memset(s->spi1.regs, 0, sizeof(s->spi1.regs));
    gh_reset(&s->spi1);
    s->spi1.selected = s->spi1.reset_high = false;
    s->ispi.analog[3][0][0] = 0x1530;
    BesPsram *p = &s->psram_ctrl;
    memset(p->mc, 0, sizeof(p->mc));
    memset(p->phy, 0, sizeof(p->phy));
    memset(p->mr, 0, sizeof(p->mr));
    p->mr[1] = 0x0d;
    p->tx_count = p->rx_count = p->rx_pos = 0;
    memset(memory_region_get_ram_ptr(&s->iomux), 0, 0xd0);
    memset(memory_region_get_ram_ptr(&s->psc), 0, 0x100);
    for (unsigned i = 0; i < ARRAY_SIZE(s->uart_ext); i++) {
        memset(memory_region_get_ram_ptr(&s->uart_ext[i]), 0, 0xc);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->i2c); i++) {
        memset(s->i2c[i].regs, 0, sizeof(s->i2c[i].regs));
        s->i2c[i].pending = s->i2c[i].abort_source = 0;
        s->i2c[i].rx_head = s->i2c[i].rx_count = 0;
        i2c_update(&s->i2c[i]);
    }
    memset(memory_region_get_ram_ptr(&s->bt_patch), 0, 0x200);
    memset(memory_region_get_ram_ptr(&s->bt_dump_ctrl), 0, 8);
    for (unsigned i = 0; i < ARRAY_SIZE(s->bt_config); i++) {
        memset(s->bt_config[i].regs, 0, sizeof(s->bt_config[i].regs));
    }
    for (unsigned i = 0; i < ARRAY_SIZE(s->gpio); i++) {
        memset(s->gpio[i].regs, 0, sizeof(s->gpio[i].regs));
        s->gpio[i].input = s->gpio[i].pending = 0;
    }
    panel_te_update_irq(s);
    if (s->touch.enabled) { touch_reset(s); }
    memset(s->lcdc.regs, 0, sizeof(s->lcdc.regs));
    s->gpu_probe_control = s->gpu_probe_mask = 0;
    s->gpu_command_address = 0;
    s->gpu_busy = false;
    s->gpu_status = s->gpu_pending_event = 0;
    s->touch.irq_rearm_pending = false;
    gpu_update_irq(s);
    lcdc_update_irq(&s->lcdc);
}

static void map_ram(Best1503State *s, MemoryRegion *mr, const char *name,
                    hwaddr base, uint64_t size)
{
    memory_region_init_ram(mr, NULL, name, size, &error_fatal);
    memory_region_add_subregion(get_system_memory(), base, mr);
}

static void map_alias(Best1503State *s, MemoryRegion *mr, const char *name,
                      MemoryRegion *target, hwaddr base)
{
    memory_region_init_alias(mr, OBJECT(s), name, target, 0,
                             memory_region_size(target));
    memory_region_add_subregion(get_system_memory(), base, mr);
}

static MemTxResult nand_cache_read(void *opaque, hwaddr offset, uint64_t *value,
                                   unsigned size, MemTxAttrs attrs)
{
    BesNor *nor = opaque;
    if (offset + size > 2112) { return MEMTX_ERROR; }
    *value = 0;
    for (unsigned i = 0; i < size; i++) {
        uint8_t byte = offset + i < 2048 ?
            nor->nand_data[nor->nand_page * 2048 + offset + i] : 0xff;
        *value |= (uint64_t)byte << (8 * i);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps nand_cache_ops = {
    .read_with_attrs = nand_cache_read,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void best1503_init(MachineState *machine)
{
    Best1503State *s = (Best1503State *)machine;
    DeviceState *cpu;
    BesNorLayout nor_layout = { 0 };
    if (s->nor_image || s->nor_writeback) {
        if (!s->nor_image || !s->nor_writeback || s->factory_image ||
            !bes_nor_layout_init(&nor_layout, s->flash_bytes, s->ap_offset,
                                 s->nv_offset, s->factory_offset,
                                 s->config_offset, s->config_bytes)) {
            nor_store_failure(&s->nor[0], "NOR writeback requires an exact catalog geometry, nor-image and no factory-image");
        }
    }
    if ((s->flash_bytes != 8 * MiB && s->flash_bytes != 16 * MiB) ||
        s->ap_offset >= s->flash_bytes || (s->ap_offset & 0xfff) ||
        s->reset_sp < 0x20000000 || s->reset_sp > 0x20160000 ||
        (s->reset_sp & 7) || (s->factory_image && s->factory_offset > s->flash_bytes - 128 * KiB) ||
        s->nv_offset > s->flash_bytes || s->nand_data_offset > 512 * MiB ||
        (uint64_t)s->config_offset + s->config_bytes > s->ap_offset ||
        ((s->config_offset | s->config_bytes) & (8192 - 1)) ||
        (uint64_t)s->nand_data_offset + s->nand_data_bytes > 512 * MiB ||
        !s->nand_data_bytes || (s->nand_data_bytes & (128 * KiB - 1)) ||
        (s->data_image && !s->resource_image) ||
        (s->data_writeback && (!s->data_image || !s->data_spare_image)) ||
        (s->data_spare_image && !s->data_writeback) ||
        (s->nand_data_offset & (128 * KiB - 1)) || s->panel_te_pin >= 96 ||
        s->resource_yaffs > 1 || s->pmu_calibration > 1 || s->touch_health_format > 1 || s->panel_id > 0xffffff ||
        s->touch_i2c >= 8 || s->touch_power_pin >= 96 ||
        s->touch_reset_pin >= 96 || s->touch_irq_pin >= 96) {
        error_report("Invalid BES1503 firmware memory geometry");
        exit(1);
    }
    s->nor[0].write_min_offset = s->nv_offset;
    s->nor[0].write_max_offset = s->flash_bytes;
    s->nor[0].config_offset = s->config_offset;
    s->nor[0].config_bytes = s->config_bytes;
    s->nor[1].write_min_offset = s->nand_data_offset;
    s->nor[1].write_max_offset = s->nand_data_offset + s->nand_data_bytes;
    if (s->bt_hci_chardev) {
        Chardev *backend = qemu_chr_find(s->bt_hci_chardev);
        if (!backend || !s->bt_tx_capture || s->bt_hci_reset_fixture ||
            s->bt_hci_reject_fixture) {
            error_report("bt-hci-chardev requires TX capture and excludes response fixtures");
            exit(1);
        }
        qemu_chr_fe_init(&s->bt_hci, backend, &error_fatal);
        qemu_chr_fe_set_handlers(&s->bt_hci, bt_hci_can_read, bt_hci_receive,
                                  NULL, NULL, s, NULL, true);
    }
    if (s->resource_image) {
        g_autofree char *contents = NULL;
        gsize length;
        if (!g_file_get_contents(s->resource_image, &contents, &length, NULL) ||
            length < 16 || length > 512 * MiB || (!s->resource_yaffs && memcmp(contents, "-rom1fs-", 8))) {
            error_report("Invalid ROMFS resource image: %s", s->resource_image);
            exit(1);
        }
        s->nor[1].nand_data = g_malloc(512 * MiB);
        memset(s->nor[1].nand_data, 0xff, 512 * MiB);
        if (length > s->nand_data_offset) { error_report("Resource exceeds NAND partition"); exit(1); }
        /* AP table at file 0xb8e740: resource starts at page 0, 65536 pages.
         * nand_data at 0xb8e658 starts at page 65536, spans 196608 pages. */
        memcpy(s->nor[1].nand_data, contents, length);
        if (s->data_image && !s->data_writeback) {
            g_autofree char *data_contents = NULL;
            gsize data_length;
            if (!g_file_get_contents(s->data_image, &data_contents, &data_length, NULL) ||
                data_length != s->nand_data_bytes) {
                error_report("Invalid NAND data partition image: %s", s->data_image);
                exit(1);
            }
            /* Copy-on-start: guest changes never alter the source image. */
            memcpy(s->nor[1].nand_data + s->nand_data_offset, data_contents, data_length);
        }
        s->nor[1].nand_spare = g_malloc(16 * MiB);
        memset(s->nor[1].nand_spare, 0xff, 16 * MiB);
        if (s->data_writeback) {
            BesNandStore *store = &s->nor[1].nand_store;
            if (!bes_store_open(store, s->data_image, s->data_spare_image,
                                s->nand_data_offset, s->nand_data_bytes) ||
                !bes_store_read(store->data_fd,
                    s->nor[1].nand_data + s->nand_data_offset, s->nand_data_bytes) ||
                !bes_store_read(store->spare_fd,
                    s->nor[1].nand_spare + s->nand_data_offset / 2048 * 64,
                    s->nand_data_bytes / 2048 * 64)) {
                error_report("Cannot open private NAND transaction: %s", strerror(errno));
                bes_store_close(store);
                exit(1);
            }
        }
        memory_region_init_io(&s->nand_cache, OBJECT(s), &nand_cache_ops, &s->nor[1],
                              "bes.nand-page-cache", 4096);
        memory_region_add_subregion(get_system_memory(), 0x30000000, &s->nand_cache);
        static const hwaddr aliases[] = {0x34000000, 0x10000000, 0x14000000};
        for (unsigned i = 0; i < 3; i++) {
            g_autofree char *name = g_strdup_printf("bes.nand-cache-alias%u", i);
            map_alias(s, &s->nand_cache_alias[i], name, &s->nand_cache, aliases[i]);
        }
    }
    if ((s->spi0_unconnected && s->gh3x2x_fixture) ||
        (s->gpu_id_fixture && !s->gpu_probe) ||
        (s->gpu_no_render && !s->gpu_id_fixture) ||
        (s->gpu_completion_fixture && !s->gpu_no_render) ||
        (s->gpu_software_renderer && (!s->gpu_id_fixture ||
          s->gpu_no_render || s->gpu_completion_fixture)) ||
        (s->bt_hci_reset_fixture && (!s->bt_peer || !s->bt_tx_capture)) ||
        (s->bt_hci_reject_fixture && !s->bt_hci_reset_fixture)) {
        error_report("Incompatible SPI slave modes or missing GPU/BT fixture prerequisite");
        exit(1);
    }
    if (s->panel_te_fixture && (!s->panel_id_fixture || !s->lcd_frame_capture)) {
        error_report("panel-te-fixture requires panel-id-fixture and lcd-frame-capture");
        exit(1);
    }
    if (s->gpu_probe) {
        memory_region_init_io(&s->gpu_probe_mmio, OBJECT(s), &gpu_probe_ops, s,
                              "bes.gpu-probe", 0x1000);
        memory_region_add_subregion(get_system_memory(), 0x40220000, &s->gpu_probe_mmio);
    }
    uint32_t vectors[2] = {
        cpu_to_le32(s->reset_sp), cpu_to_le32(0x0c000011 + s->ap_offset)
    };
    s->bt_wake_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bt_peer_wake, s);
    s->bt_tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bt_peer_capture_tx, s);
    s->bt_rx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, bt_peer_reset_response, s);
    s->gpu_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gpu_complete, s);
    s->panel_te_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, panel_te_edge, s);
    s->touch_irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, touch_irq_rearm, s);
    s->lcdc.machine = s;
    if (s->touch.enabled) {
        if (!s->lcd_frame_capture) {
            error_report("touch requires lcd-frame-capture");
            exit(1);
        }
        s->touch.mouse = qemu_add_mouse_event_handler(touch_mouse, s, 1,
                                                      "BES CST9217 touchscreen");
        qemu_activate_mouse_event_handler(s->touch.mouse);
    }
    s->spi0.machine = s;
    s->spi1.machine = s;
    gh_reset(&s->spi0);
    gh_reset(&s->spi1);
    s->lcdc.spi_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lcdc_spi_complete,
                                    &s->lcdc);
    s->lcdc.frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lcdc_frame_complete,
                                     &s->lcdc);
    if (s->lcd_frame_capture) {
        if (!s->panel_id_fixture) {
            error_report("lcd-frame-capture requires panel-id-fixture=on");
            exit(1);
        }
        s->lcdc.console = graphic_console_init(NULL, 0, &lcdc_gfx_ops, &s->lcdc);
        lcdc_shared_init(&s->lcdc);
    } else if (s->lcdc.frame_file) {
        error_report("lcd-frame-file requires lcd-frame-capture");
        exit(1);
    }
    s->cmu[1].regs[0] = s->revision_id << 16;

    map_ram(s, &s->flash, "bes.flash", 0x2c000000, s->flash_bytes);
    memset(memory_region_get_ram_ptr(&s->flash), 0xff, s->flash_bytes);
    if (s->nor_writeback) {
        uint8_t *flash = memory_region_get_ram_ptr(&s->flash);
        if (!bes_nor_store_open(&s->nor[0].nor_store, s->nor_image,
                                &nor_layout, flash)) {
            nor_store_failure(&s->nor[0], "NOR image must be an exclusive owner-0600 regular file of the exact compact layout size");
        }
        s->nor_exit.notify = best1503_nor_exit;
        qemu_add_exit_notifier(&s->nor_exit);
        s->nor_exit_registered = true;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES NOR instance storage loaded: offset=0x%08x tail-size=%u config-offset=0x%08x config-size=%u compact-size=%u\n",
                      nor_layout.tail_offset, nor_layout.flash_bytes - nor_layout.tail_offset,
                      nor_layout.config_offset, nor_layout.config_bytes,
                      bes_nor_layout_bytes(&nor_layout));
    }
    if (s->factory_image) {
        g_autofree char *contents = NULL;
        gsize length;
        /* The stock /dev/facmode MTD observed at littlefs_bind uses
         * sector 0x7e00, 512-byte sectors, 16 x 8192-byte erase blocks.
         * Supply storage bytes, leaving AP code and property APIs intact. */
        if (!g_file_get_contents(s->factory_image, &contents, &length, NULL) ||
            length != 128 * KiB) {
            error_report("factory-image must be a 128 KiB Q66 LittleFS partition");
            exit(1);
        }
        memcpy((uint8_t *)memory_region_get_ram_ptr(&s->flash) + s->factory_offset,
               contents, length);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BES factory storage loaded: offset=0x%08x size=%zu\n",
                      s->factory_offset, length);
    }
    s->nor[0].backing = &s->flash;
    map_alias(s, &s->flash_exec, "bes.flash.exec", &s->flash, 0x0c000000);
    map_alias(s, &s->flash_nc, "bes.flash.nc", &s->flash, 0x28000000);
    map_alias(s, &s->flash_nc_exec, "bes.flash.nc.exec", &s->flash, 0x08000000);
    map_ram(s, &s->sram, "bes.sram", 0x20000000, 0x160000);
    map_alias(s, &s->sram_exec, "bes.sram.exec", &s->sram, 0x00200000);
    /* SDK BT_RAM_BASE/BT_RAM_SIZE: patch data only, no Bluetooth CPU. */
    map_ram(s, &s->bt_ram, "bes.bt-ram", 0xc0000000, 0x10000);
    map_alias(s, &s->bt_ram_exec, "bes.bt-ram.exec", &s->bt_ram, 0xa0200000);
    /* Comparator/remap table from bt_1306_reg_map.h, config storage only.
     * No BT ROM replacement or controller CPU execution is implemented.
     */
    map_ram(s, &s->bt_patch, "bes.bt-patch-state", 0xa01ffe00, 0x200);
    /* Enable/start/stop config from bt_drv_cpudump.c. No trace entries
     * exist without an emulated Bluetooth CPU; its trace RAM stays unmapped.
     */
    map_ram(s, &s->bt_dump_ctrl, "bes.bt-cpudump-control", 0xd0702000, 8);
    {
        /* TX power/SWAGC force; Q66 modem final config (bits 31:30). */
        /* Original btdrv clock-selection RMW at 0x0c92cbd2..de. */
        /* N66 .32 at 0x0c52d694 configures D0220D90[19:14]. */
        static const uint16_t digital[] = {0x00, 0x04, 0x3c, 0x7c, 0x190};
        /* CMU_CLKREG and BT peer interrupt-mask SET/CLR configuration. */
        static const uint16_t clock[] = {0x38, 0x58, 0x60, 0x64, 0xa0, 0xa4};
        /* All RMW fields in original Q66 modem init 0x0c92920c..d3c.
         * Some differ from SDK bt_drv_config.c; names/side effects beyond
         * configuration remain unverified. Status offsets are not mapped.
         */
        static const uint16_t modem[] = {
            0x004, 0x00c, 0x018, 0x028, 0x02c, 0x040, 0x05c, 0x0c0, 0x0d0, 0x0d4,
            0x0e0, 0x0e4, 0x0ec, 0x0f4, 0x0f8, 0x104, 0x108, 0x120,
            0x134, 0x140, 0x14c, 0x150, 0x154, 0x15c, 0x160, 0x168,
            0x174, 0x178, 0x17c, 0x1a8, 0x1ac, 0x1b0, 0x1b4, 0x1bc,
            0x1c0, 0x1c4, 0x1cc, 0x1d0, 0x1e8, 0x1ec, 0x1f0, 0x20c,
            0x210, 0x214, 0x230, 0x234, 0x238, 0x240, 0x248, 0x250, 0x254,
            0x25c, 0x2c0, 0x2c4, 0x300, 0x308, 0x324, 0x334, 0x338,
            0x33c, 0x340, 0x344,
            0x36c, 0x370, 0x374, 0x378, 0x400, 0x414, 0x438, 0x440,
        };
        static const uint16_t iq_correction[] = {0x34};
        static const hwaddr bases[] = {
            0xd0220c00, 0xd0330000, 0xd0350000, 0xd0310000
        };
        const uint16_t *offsets[] = {digital, clock, modem, iq_correction};
        const unsigned counts[] = {
            ARRAY_SIZE(digital), ARRAY_SIZE(clock), ARRAY_SIZE(modem),
            ARRAY_SIZE(iq_correction)
        };
        for (unsigned i = 0; i < ARRAY_SIZE(s->bt_config); i++) {
            BesBtConfig *b = &s->bt_config[i];
            g_autofree char *name = g_strdup_printf("bes.bt-config%u", i);
            b->base = bases[i];
            b->machine = s;
            b->offsets = offsets[i];
            b->count = counts[i];
            memory_region_init_io(&b->mmio, OBJECT(s), &bt_config_ops, b,
                                  name, i == 2 ? 0x444 : i == 1 ? 0xa8 :
                                        i == 3 ? 79 * 4 : 0x194);
            memory_region_add_subregion(get_system_memory(), b->base, &b->mmio);
        }
    }
    map_ram(s, &s->psram, "bes.psram", 0x38000000, 32 * MiB);
    map_alias(s, &s->psram_exec, "bes.psram.exec", &s->psram, 0x18000000);
    map_alias(s, &s->psram_cached, "bes.psram.cached", &s->psram, 0x3c000000);
    map_alias(s, &s->psram_cached_exec, "bes.psram.cached.exec",
              &s->psram, 0x1c000000);

    /* Synthetic reset vectors stand in for the missing mask-ROM bootloader.
     * The firmware itself, including its 16-byte BES header, is unmodified.
     */
    map_ram(s, &s->boot_vectors, "bes.synthetic-boot-vectors", 0, 0x1000);
    cpu_physical_memory_write(0, vectors, sizeof(vectors));

    for (unsigned i = 0; i < 2; i++) {
        s->cmu[i].machine = s;
        s->cmu[i].aon = i != 0;
        s->cmu[i].base = i ? 0x40080000 : 0x40000000;
        memory_region_init_io(&s->cmu[i].mmio, OBJECT(s), &cmu_ops,
                              &s->cmu[i], i ? "bes.aon-cmu" : "bes.cmu", 0x1000);
        memory_region_add_subregion(get_system_memory(), s->cmu[i].base,
                                    &s->cmu[i].mmio);
    }
    for (unsigned i = 0; i < 2; i++) {
        s->cache[i].name = i ? "dcache" : "icache";
        memory_region_init_io(&s->cache[i].mmio, OBJECT(s), &cache_ops,
                              &s->cache[i], s->cache[i].name, 0x8c);
        memory_region_add_subregion(get_system_memory(),
                                    i ? 0x07ffc000 : 0x07ffa000,
                                    &s->cache[i].mmio);
    }
    /* Pin multiplexing state only: no physical GPIO/peripheral routing yet.
     * reg_iomux_best1503.h defines the 0x000..0x0cc register bank.
     */
    map_ram(s, &s->iomux, "bes.iomux-state", 0x40086000, 0xd0);
    /* Power-sequencer configuration storage only; no power transitions. */
    map_ram(s, &s->psc, "bes.psc-state", 0x40085000, 0x100);
    if (s->panel_te_fixture) {
        memory_region_init_io(&s->gpio_summary, OBJECT(s), &gpio_summary_ops, s,
                              "bes.gpio-summary", 24);
        memory_region_add_subregion_overlap(get_system_memory(), 0x40085080,
                                           &s->gpio_summary, 1);
    }
    memory_region_init_io(&s->lcdc.mmio, OBJECT(s), &lcdc_ops, &s->lcdc,
                          "bes.lcdc-config", 0x2ac);
    memory_region_add_subregion(get_system_memory(), 0x40200000,
                                &s->lcdc.mmio);
    for (unsigned i = 0; i < ARRAY_SIZE(s->gpio); i++) {
        /* Original Q66 bank table 0x2cc2166c differs from SDK map:
         * SDK GPIO1=0x40088000, 0x4008a000=PWM1; do not copy its bases.
         */
        static const hwaddr bases[] = {0x40081000, 0x40089000, 0x4008a000};
        BesGpio *g = &s->gpio[i];
        g_autofree char *name = g_strdup_printf("bes.gpio%u", i);
        g->base = bases[i];
        g->machine = s;
        memory_region_init_io(&g->mmio, OBJECT(s), &gpio_ops, g, name, 0x64);
        memory_region_add_subregion(get_system_memory(), g->base, &g->mmio);
    }
    /* atc1441 reports a real PMU_REG(0) read of 0x1530 on BEST1503. */
    s->ispi.analog[3][0][0] = 0x1530;
    s->ispi.machine = s;
    memory_region_init_io(&s->ispi.mmio, OBJECT(s), &ispi_ops, &s->ispi,
                          "bes.ispi", 0x8c);
    memory_region_add_subregion(get_system_memory(), 0x40009000,
                                &s->ispi.mmio);
    memory_region_init_io(&s->spi0.mmio, OBJECT(s), &spi0_ops, &s->spi0,
                          "bes.spi0-config", 0x8c);
    memory_region_add_subregion(get_system_memory(), 0x40007000,
                                &s->spi0.mmio);
    /* P67 SRAM SPI instance table includes 0x40068000, polled SSP bank. */
    memory_region_init_io(&s->spi1.mmio, OBJECT(s), &spi0_ops, &s->spi1,
                          "bes.spi1-config", 0x8c);
    memory_region_add_subregion(get_system_memory(), 0x40068000, &s->spi1.mmio);
    for (unsigned i = 0; i < 2; i++) {
        s->nor[i].base = i ? 0x40500000 : 0x40140000;
        memory_region_init_io(&s->nor[i].mmio, OBJECT(s), &nor_ops,
                              &s->nor[i], i ? "bes.nor1" : "bes.nor0", 0x1000);
        memory_region_add_subregion(get_system_memory(), s->nor[i].base,
                                    &s->nor[i].mmio);
    }

    s->psram_ctrl.memory = &s->psram;
    memory_region_init_io(&s->psram_ctrl.mc_mmio, OBJECT(s), &psram_mc_ops,
                          &s->psram_ctrl, "bes.psram-mc", 0x1000);
    memory_region_add_subregion(get_system_memory(), 0x40160000,
                                &s->psram_ctrl.mc_mmio);
    memory_region_init_io(&s->psram_ctrl.phy_mmio, OBJECT(s), &psram_phy_ops,
                          &s->psram_ctrl, "bes.psram-phy", 0x6c);
    memory_region_add_subregion(get_system_memory(), 0x40168000,
                                &s->psram_ctrl.phy_mmio);
    qemu_register_reset(best1503_peripheral_reset, s);

    s->cpuclk = clock_new(OBJECT(s), "cpuclk");
    clock_set_hz(s->cpuclk, 208000000);
    object_initialize_child(OBJECT(s), "armv7m", &s->armv7m, TYPE_ARMV7M);
    cpu = DEVICE(&s->armv7m);
    qdev_prop_set_string(cpu, "cpu-type", machine->cpu_type);
    qdev_prop_set_uint32(cpu, "num-irq", 78);
    qdev_connect_clock_in(cpu, "cpuclk", s->cpuclk);
    object_property_set_link(OBJECT(cpu), "memory", OBJECT(get_system_memory()),
                             &error_fatal);
    sysbus_realize(SYS_BUS_DEVICE(cpu), &error_fatal);
    s->bt_tx_irq = qdev_get_gpio_in(cpu, 32);
    s->bt_rx_irq = qdev_get_gpio_in(cpu, 34);
    s->gpu_irq = qdev_get_gpio_in(cpu, 70);
    s->panel_te_irq = qdev_get_gpio_in(cpu, 44);
    /* Original Q66 getter 0xc93448c returns67, unlike SDK LCD_FRAME_DONE=68. */
    s->lcdc.irq = qdev_get_gpio_in(cpu, 67);
    /* P67 3.101.043 AP table at file 0xc1ac18 confirms all eight bases
     * and IRQs (12-byte records); I2C6/7 are needed during board bringup. */
    for (unsigned i = 0; i < ARRAY_SIZE(s->i2c); i++) {
        static const hwaddr bases[] = {
            0x40005000, 0x40006000, 0x40060000,
            0x40061000, 0x40062000, 0x40063000, 0x40064000, 0x40065000
        };
        static const unsigned irqs[] = {14, 29, 64, 65, 53, 54, 55, 56};
        g_autofree char *name = g_strdup_printf("bes.i2c%u", i);
        s->i2c[i].base = bases[i];
        s->i2c[i].machine = s;
        s->i2c[i].irq = qdev_get_gpio_in(cpu, irqs[i]);
        memory_region_init_io(&s->i2c[i].mmio, OBJECT(s), &i2c_ops,
                              &s->i2c[i], name, 0x1000);
        memory_region_add_subregion(get_system_memory(), bases[i],
                                    &s->i2c[i].mmio);
    }
    /* BES UART's standard PL011 register bank, IRQs from best1503.h.
     * Oversampling/RX extension registers retain config only; host serial
     * has no physical bit timing. DMA requests are not connected yet.
     */
    for (unsigned i = 0; i < ARRAY_SIZE(s->uart_ext); i++) {
        /* The original shared ISR walks four UARTs (P67 table 0xc1b008). */
        static const hwaddr bases[] = {0x4000b000, 0x4000c000, 0x4000d000, 0x40066000};
        static const int irq[] = {17, 18, 26, 2};
        g_autofree char *name = g_strdup_printf("bes.uart%u-ext", i);
        pl011_create(bases[i],
                     qdev_get_gpio_in(cpu, irq[i]), i < 3 ? serial_hd(i) : NULL);
        memory_region_init_ram(&s->uart_ext[i], NULL, name, 0xc, &error_fatal);
        memory_region_add_subregion_overlap(get_system_memory(),
                                            bases[i] + 0x4c,
                                            &s->uart_ext[i], 1);
    }
    /* SDK reg_dma.h matches PL080 for core registers/eight channels.
     * The BES 2D extension supports disabled state only; clock gating and
     * UART DMA handshake are not modeled.
     * Memory-to-memory transfers and software DMA requests use QEMU's engine.
     */
    for (unsigned i = 0; i < 2; i++) {
        g_autofree char *name = g_strdup_printf("dma%u", i);
        object_initialize_child(OBJECT(s), name, &s->dma[i], TYPE_PL080);
        object_property_set_link(OBJECT(&s->dma[i]), "downstream",
                                 OBJECT(get_system_memory()), &error_fatal);
        sysbus_realize(SYS_BUS_DEVICE(&s->dma[i]), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->dma[i]), 0, 0x40120000 + i * 0x10000);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->dma[i]), 0,
                            qdev_get_gpio_in(cpu, i ? 4 : 3));
        s->dma_2d[i].controller = i;
        g_autofree char *ext_name = g_strdup_printf("bes.dma%u-2d", i);
        memory_region_init_io(&s->dma_2d[i].mmio, OBJECT(s), &dma_2d_ops,
                              &s->dma_2d[i], ext_name, 0x100);
        memory_region_add_subregion_overlap(get_system_memory(),
                                            0x40120200 + i * 0x10000,
                                            &s->dma_2d[i].mmio, 1);
    }
    /* Original HAL request 28 maps both RX source and TX destination to
     * GPDMA selector 6 (observed TX channel config 0x981). */
    s->nor[1].nand_drq = qdev_get_gpio_in(DEVICE(&s->dma[1]), 6);
    /* Q66 .071 original SPI DMA descriptors: RX config 0xd009 selects
     * source 4; TX config 0x941 selects destination 5, both on GPDMA.
     * DR is 0x40007008. Do not apply this routing to the other SPI bank. */
    s->spi0.dma_rx = qdev_get_gpio_in(DEVICE(&s->dma[1]), 4);
    s->spi0.dma_tx = qdev_get_gpio_in(DEVICE(&s->dma[1]), 5);
    s->spi0.dma_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, spi0_dma_request,
                                    &s->spi0);
    s->slowclk = clock_new(OBJECT(s), "slowclk");
    s->fastclk = clock_new(OBJECT(s), "fastclk");
    /* Nominal BES slow system tick is 16 kHz, fast tick is 24 MHz / 4.
     * Clock-tree divider writes are currently stored, not wired to these.
     */
    clock_set_hz(s->slowclk, 16000);
    clock_set_hz(s->fastclk, 6000000);
    /* hal_wdt.c implements the standard ARM watchdog register interface.
     * AON uses nominal slow ticks; MCU uses fast ticks. CMU source/divider
     * changes are not wired yet, as with the existing system timers.
     */
    for (unsigned i = 0; i < 2; i++) {
        g_autofree char *name = g_strdup_printf("watchdog%u", i);
        object_initialize_child(OBJECT(s), name, &s->watchdog[i],
                                TYPE_CMSDK_APB_WATCHDOG);
        qdev_connect_clock_in(DEVICE(&s->watchdog[i]), "WDOGCLK",
                              i ? s->slowclk : s->fastclk);
        sysbus_realize(SYS_BUS_DEVICE(&s->watchdog[i]), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->watchdog[i]), 0,
                        i ? 0x40082000 : 0x40001000);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->watchdog[i]), 0,
                            qdev_get_gpio_in(cpu, i ? 5 : 10));
    }
    /* SDK's extra AON timer at 0x40089000 conflicts with original Q66
     * GPIO1. Its actual Q66 address is unknown; do not map that timer.
     */
    for (unsigned i = 0; i < ARRAY_SIZE(s->timer); i++) {
        static const hwaddr bases[] = {
            0x40002000, 0x40003000, 0x40004000, 0x40084000
        };
        static const int irqs[][2] = {{12, 13}, {27, 28}, {62, 62},
                                      {45, 46}};
        g_autofree char *name = g_strdup_printf("timer%u", i);
        object_initialize_child(OBJECT(s), name, &s->timer[i],
                                TYPE_CMSDK_APB_DUALTIMER);
        qdev_connect_clock_in(DEVICE(&s->timer[i]), "TIMCLK",
                              i >= 3 ? s->slowclk : s->fastclk);
        sysbus_realize(SYS_BUS_DEVICE(&s->timer[i]), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->timer[i]), 0, bases[i]);
        if (i == 2) {
            sysbus_connect_irq(SYS_BUS_DEVICE(&s->timer[i]), 0,
                                qdev_get_gpio_in(cpu, 62));
        }
        for (unsigned j = 0; j < 2 && i != 2; j++) {
            if (irqs[i][j] >= 0) {
                sysbus_connect_irq(SYS_BUS_DEVICE(&s->timer[i]), j + 1,
                                    qdev_get_gpio_in(cpu, irqs[i][j]));
            }
        }
    }
    /* Neither compact extent may be overlaid by kernel input (config ends
     * at/before ap-offset, tail begins at nv-offset). In addition,
     * direct guest stores must not bypass the NOR command write boundary. */
    if (s->nor_writeback) {
        memory_region_set_readonly(&s->flash, true);
    }
    armv7m_load_kernel(s->armv7m.cpu, machine->kernel_filename,
                       0x2c000000 + s->ap_offset,
                       (s->nor_writeback ? s->nv_offset : s->flash_bytes) - s->ap_offset);
}

static bool best1503_get_bt_peer(Object *obj, Error **errp)
{
    return ((Best1503State *)obj)->bt_peer;
}

static void best1503_set_bt_peer(Object *obj, bool value, Error **errp)
{
    ((Best1503State *)obj)->bt_peer = value;
}

static bool best1503_get_bt_tx_capture(Object *obj, Error **errp)
{
    return ((Best1503State *)obj)->bt_tx_capture;
}

static bool best1503_get_panel_id_fixture(Object *obj, Error **errp)
{
    return ((Best1503State *)obj)->panel_id_fixture;
}

static bool best1503_get_gh3x2x_fixture(Object *obj, Error **errp)
{
    return ((Best1503State *)obj)->gh3x2x_fixture;
}

static bool best1503_get_spi0_unconnected(Object *obj, Error **errp)
{
    return ((Best1503State *)obj)->spi0_unconnected;
}

static bool best1503_get_gpu_probe(Object *obj, Error **errp)
{
    return ((Best1503State *)obj)->gpu_probe;
}

static void best1503_set_gpu_probe(Object *obj, bool value, Error **errp)
{
    ((Best1503State *)obj)->gpu_probe = value;
}

static bool best1503_get_gpu_id_fixture(Object *obj, Error **errp)
{
    return ((Best1503State *)obj)->gpu_id_fixture;
}

static void best1503_set_gpu_id_fixture(Object *obj, bool value, Error **errp)
{
    ((Best1503State *)obj)->gpu_id_fixture = value;
}

static bool best1503_get_gpu_no_render(Object *obj, Error **errp)
{
    return ((Best1503State *)obj)->gpu_no_render;
}

static bool best1503_get_touch(Object *obj, Error **errp)
{
    return ((Best1503State *)obj)->touch.enabled;
}

static bool best1503_get_bt_hci_reject_fixture(Object *obj, Error **errp)
{
    return ((Best1503State *)obj)->bt_hci_reject_fixture;
}

static void best1503_set_bt_hci_reject_fixture(Object *obj, bool value, Error **errp)
{
    ((Best1503State *)obj)->bt_hci_reject_fixture = value;
}

static bool best1503_get_power_adc_fixture(Object *obj, Error **errp)
{
    return ((Best1503State *)obj)->power_adc_fixture;
}

static void best1503_set_power_adc_fixture(Object *obj, bool value, Error **errp)
{
    ((Best1503State *)obj)->power_adc_fixture = value;
}

/* The runner owns QMP and exposes only pmu.adcRaw.set/get. Do not surface
 * caller-selected QOM paths, properties or arbitrary monitor commands. */
static void best1503_set_adc_raw(Object *obj, const char *value, Error **errp)
{
    Best1503State *s = (Best1503State *)obj;
    uint16_t next[BES_ADC_CHANNELS];
    if (!s->power_adc_fixture) {
        error_setg(errp, "pmu.adcRaw requires power-adc-fixture");
        return;
    }
    if (!bes_adc_parse(value, next)) {
        error_setg(errp, "pmu.adcRaw requires eight decimal values in 0..65535");
        return;
    }
    qemu_mutex_lock(&s->adc_lock);
    bool applied = bes_adc_replace(&s->adc_input, next);
    qemu_mutex_unlock(&s->adc_lock);
    if (!applied) {
        error_setg(errp, "pmu.adcRaw version exhausted");
    }
}

static char *best1503_get_adc_state(Object *obj, Error **errp)
{
    Best1503State *s = (Best1503State *)obj;
    BesAdcInput snapshot;
    qemu_mutex_lock(&s->adc_lock);
    snapshot = s->adc_input;
    qemu_mutex_unlock(&s->adc_lock);
    /* Fixed eight-element arrays: bounded response, no host-controlled size.
     * The version marks vector replacement, not a guest calibration epoch. */
    GString *result = g_string_new(NULL);
    g_string_append_printf(result,
        "{\"schema\":1,\"fixtureEnabled\":%s,\"version\":%" PRIu64,
        s->power_adc_fixture ? "true" : "false", snapshot.version);
    g_string_append(result, ",\"values\":[");
    for (unsigned i = 0; i < BES_ADC_CHANNELS; i++) {
        g_string_append_printf(result, "%s%u", i ? "," : "", snapshot.values[i]);
    }
    g_string_append(result, "],\"readCount\":[");
    for (unsigned i = 0; i < BES_ADC_CHANNELS; i++) {
        g_string_append_printf(result, "%s%" PRIu64, i ? "," : "", snapshot.reads[i]);
    }
    g_string_append(result, "],\"lastReadVersion\":[");
    for (unsigned i = 0; i < BES_ADC_CHANNELS; i++) {
        g_string_append_printf(result, "%s%" PRIu64, i ? "," : "",
                               snapshot.last_read_version[i]);
    }
    g_string_append(result, "]}");
    return g_string_free(result, false);
}

static void best1503_set_touch(Object *obj, bool value, Error **errp)
{
    ((Best1503State *)obj)->touch.enabled = value;
}

static bool best1503_get_gpu_software_renderer(Object *obj, Error **errp)
{
    return ((Best1503State *)obj)->gpu_software_renderer;
}

static void best1503_set_gpu_software_renderer(Object *obj, bool value, Error **errp)
{
    ((Best1503State *)obj)->gpu_software_renderer = value;
}

static bool best1503_get_panel_te_fixture(Object *obj, Error **errp)
{
    return ((Best1503State *)obj)->panel_te_fixture;
}

static void best1503_set_panel_te_fixture(Object *obj, bool value, Error **errp)
{
    ((Best1503State *)obj)->panel_te_fixture = value;
}

static bool best1503_get_bt_hci_reset_fixture(Object *obj, Error **errp)
{
    return ((Best1503State *)obj)->bt_hci_reset_fixture;
}

static void best1503_set_bt_hci_reset_fixture(Object *obj, bool value, Error **errp)
{
    ((Best1503State *)obj)->bt_hci_reset_fixture = value;
}

static bool best1503_get_gpu_completion_fixture(Object *obj, Error **errp)
{
    return ((Best1503State *)obj)->gpu_completion_fixture;
}

static void best1503_set_gpu_completion_fixture(Object *obj, bool value, Error **errp)
{
    ((Best1503State *)obj)->gpu_completion_fixture = value;
}

static void best1503_set_gpu_no_render(Object *obj, bool value, Error **errp)
{
    ((Best1503State *)obj)->gpu_no_render = value;
}

static void best1503_set_spi0_unconnected(Object *obj, bool value, Error **errp)
{
    ((Best1503State *)obj)->spi0_unconnected = value;
}

static void best1503_set_gh3x2x_fixture(Object *obj, bool value, Error **errp)
{
    ((Best1503State *)obj)->gh3x2x_fixture = value;
}

static bool best1503_get_lcd_frame_capture(Object *obj, Error **errp)
{
    return ((Best1503State *)obj)->lcd_frame_capture;
}

static void best1503_set_lcd_frame_capture(Object *obj, bool value, Error **errp)
{
    ((Best1503State *)obj)->lcd_frame_capture = value;
}

static void best1503_set_panel_id_fixture(Object *obj, bool value, Error **errp)
{
    ((Best1503State *)obj)->panel_id_fixture = value;
}

static void best1503_set_bt_tx_capture(Object *obj, bool value, Error **errp)
{
    ((Best1503State *)obj)->bt_tx_capture = value;
}

static void best1503_get_revision(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    uint32_t value = ((Best1503State *)obj)->revision_id;
    visit_type_uint32(v, name, &value, errp);
}

static void best1503_set_revision(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    uint32_t value;
    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value > 15) {
        error_setg(errp, "revision-id must be between 0 and 15");
        return;
    }
    ((Best1503State *)obj)->revision_id = value;
}

static void best1503_get_gh_reg0724(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    Best1503State *s = (Best1503State *)obj;
    uint32_t value = s->gh_reg0724_set ? s->gh_reg0724 : 0x10000;
    visit_type_uint32(v, name, &value, errp);
}

static void best1503_set_gh_reg0724(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    uint32_t value;
    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value > 0x10000) {
        error_setg(errp, "gh3x2x-reg0724 must be 0..65535, or 65536 for unknown");
        return;
    }
    Best1503State *s = (Best1503State *)obj;
    s->gh_reg0724_set = value != 0x10000;
    s->gh_reg0724 = value;
}

static char *best1503_get_resource_image(Object *obj, Error **errp)
{
    return g_strdup(((Best1503State *)obj)->resource_image);
}

static void best1503_set_resource_image(Object *obj, const char *value, Error **errp)
{
    Best1503State *s = (Best1503State *)obj;
    g_free(s->resource_image);
    s->resource_image = g_strdup(value);
}

static char *best1503_get_data_spare_image(Object *obj, Error **errp)
{
    return g_strdup(((Best1503State *)obj)->data_spare_image);
}

static void best1503_set_data_spare_image(Object *obj, const char *value, Error **errp)
{
    Best1503State *s = (Best1503State *)obj;
    g_free(s->data_spare_image);
    s->data_spare_image = g_strdup(value);
}

static bool best1503_get_data_writeback(Object *obj, Error **errp)
{
    return ((Best1503State *)obj)->data_writeback;
}

static void best1503_set_data_writeback(Object *obj, bool value, Error **errp)
{
    ((Best1503State *)obj)->data_writeback = value;
}

static char *best1503_get_data_image(Object *obj, Error **errp)
{
    return g_strdup(((Best1503State *)obj)->data_image);
}

static void best1503_set_data_image(Object *obj, const char *value, Error **errp)
{
    Best1503State *s = (Best1503State *)obj;
    g_free(s->data_image);
    s->data_image = g_strdup(value);
}

static char *best1503_get_bt_hci_chardev(Object *obj, Error **errp)
{
    return g_strdup(((Best1503State *)obj)->bt_hci_chardev);
}

static char *best1503_get_factory_image(Object *obj, Error **errp)
{
    return g_strdup(((Best1503State *)obj)->factory_image);
}

static void best1503_set_factory_image(Object *obj, const char *value, Error **errp)
{
    Best1503State *s = (Best1503State *)obj;
    g_free(s->factory_image);
    s->factory_image = g_strdup(value);
}

static void best1503_set_bt_hci_chardev(Object *obj, const char *value, Error **errp)
{
    Best1503State *s = (Best1503State *)obj;
    g_free(s->bt_hci_chardev); s->bt_hci_chardev = g_strdup(value);
}

static char *best1503_get_lcd_frame_file(Object *obj, Error **errp)
{
    return g_strdup(((Best1503State *)obj)->lcdc.frame_file);
}

static void best1503_set_lcd_frame_file(Object *obj, const char *value, Error **errp)
{
    BesLcdc *l = &((Best1503State *)obj)->lcdc;
    g_free(l->frame_file);
    l->frame_file = g_strdup(value);
}

static char *best1503_get_nor_image(Object *obj, Error **errp)
{
    return g_strdup(((Best1503State *)obj)->nor_image);
}

static void best1503_set_nor_image(Object *obj, const char *value, Error **errp)
{
    Best1503State *s = (Best1503State *)obj;
    g_free(s->nor_image);
    s->nor_image = g_strdup(value);
}

static bool best1503_get_nor_writeback(Object *obj, Error **errp)
{
    return ((Best1503State *)obj)->nor_writeback;
}

static void best1503_set_nor_writeback(Object *obj, bool value, Error **errp)
{
    ((Best1503State *)obj)->nor_writeback = value;
}

static void best1503_finalize(Object *obj)
{
    Best1503State *s = (Best1503State *)obj;
    nor_check_complete(&s->nor[0]);
    if (s->nor_exit_registered) {
        qemu_remove_exit_notifier(&s->nor_exit);
    }
    bes_nor_store_close(&s->nor[0].nor_store);
    g_free(s->nor_image);
    BesLcdc *l = &((Best1503State *)obj)->lcdc;
#ifndef _WIN32
    if (l->shared_frame) {
        munmap(l->shared_frame, LCD_SHARED_SIZE);
    }
#endif
    g_free(l->frame_file);
    bes_store_close(&((Best1503State *)obj)->nor[1].nand_store);
    qemu_mutex_destroy(&((Best1503State *)obj)->adc_lock);
}

#define GEOMETRY_ACCESSOR(field) \
static void best1503_get_##field(Object *obj, Visitor *v, const char *name, \
                                void *opaque, Error **errp) \
{ \
    uint32_t value = ((Best1503State *)obj)->field; \
    visit_type_uint32(v, name, &value, errp); \
} \
static void best1503_set_##field(Object *obj, Visitor *v, const char *name, \
                                void *opaque, Error **errp) \
{ \
    uint32_t value; \
    if (visit_type_uint32(v, name, &value, errp)) { \
        ((Best1503State *)obj)->field = value; \
    } \
}

GEOMETRY_ACCESSOR(ap_offset)
GEOMETRY_ACCESSOR(reset_sp)
GEOMETRY_ACCESSOR(flash_bytes)
GEOMETRY_ACCESSOR(factory_offset)
GEOMETRY_ACCESSOR(nv_offset)
GEOMETRY_ACCESSOR(config_offset)
GEOMETRY_ACCESSOR(config_bytes)
GEOMETRY_ACCESSOR(nand_data_offset)
GEOMETRY_ACCESSOR(nand_data_bytes)
GEOMETRY_ACCESSOR(resource_yaffs)
GEOMETRY_ACCESSOR(pmu_calibration)
GEOMETRY_ACCESSOR(panel_te_pin)
GEOMETRY_ACCESSOR(panel_power0)
GEOMETRY_ACCESSOR(panel_power1)
GEOMETRY_ACCESSOR(panel_power2)
GEOMETRY_ACCESSOR(panel_id)
GEOMETRY_ACCESSOR(touch_i2c)
GEOMETRY_ACCESSOR(touch_power_pin)
GEOMETRY_ACCESSOR(touch_reset_pin)
GEOMETRY_ACCESSOR(touch_irq_pin)
GEOMETRY_ACCESSOR(touch_module_id)
GEOMETRY_ACCESSOR(touch_health_format)

static void best1503_instance_init(Object *obj)
{
    Best1503State *s = (Best1503State *)obj;
    qemu_mutex_init(&s->adc_lock);
    bes_adc_init(&s->adc_input);
    s->ap_offset = 0xc0000;
    s->reset_sp = 0x2015fec0;
    s->flash_bytes = 16 * MiB;
    s->factory_offset = 0xfc0000;
    s->nv_offset = 0xfe0000;
    s->nand_data_offset = 128 * MiB;
    s->nand_data_bytes = 384 * MiB;
    s->panel_power0 = 0x01800000;
    s->panel_power1 = 0;
    s->panel_power2 = 0x1000;
    s->panel_id = 0x460420;
    s->panel_te_pin = 65;
    s->touch_i2c = 3;
    s->touch_power_pin = 74;
    s->touch_reset_pin = 28;
    s->touch_irq_pin = 27;
    s->touch_module_id = 1;
}

static void best1503_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "BES1503 Q66 partial boot peripheral model";
    mc->init = best1503_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m33");
    mc->max_cpus = 1;
    mc->default_ram_size = 0;
#define ADD_GEOMETRY(field, name) \
    object_class_property_add(oc, name, "uint32", best1503_get_##field, \
                              best1503_set_##field, NULL, NULL)
    ADD_GEOMETRY(ap_offset, "ap-offset");
    ADD_GEOMETRY(reset_sp, "reset-sp");
    ADD_GEOMETRY(flash_bytes, "flash-bytes");
    ADD_GEOMETRY(factory_offset, "factory-offset");
    ADD_GEOMETRY(nv_offset, "nv-offset");
    ADD_GEOMETRY(config_offset, "config-offset");
    ADD_GEOMETRY(config_bytes, "config-bytes");
    ADD_GEOMETRY(nand_data_offset, "nand-data-offset");
    ADD_GEOMETRY(nand_data_bytes, "nand-data-bytes");
    ADD_GEOMETRY(resource_yaffs, "resource-yaffs");
    ADD_GEOMETRY(pmu_calibration, "pmu-calibration");
    ADD_GEOMETRY(panel_te_pin, "panel-te-pin");
    ADD_GEOMETRY(panel_power0, "panel-power0");
    ADD_GEOMETRY(panel_power1, "panel-power1");
    ADD_GEOMETRY(panel_power2, "panel-power2");
    ADD_GEOMETRY(panel_id, "panel-id");
    ADD_GEOMETRY(touch_i2c, "touch-i2c");
    ADD_GEOMETRY(touch_power_pin, "touch-power-pin");
    ADD_GEOMETRY(touch_reset_pin, "touch-reset-pin");
    ADD_GEOMETRY(touch_irq_pin, "touch-irq-pin");
    ADD_GEOMETRY(touch_module_id, "touch-module-id");
    ADD_GEOMETRY(touch_health_format, "touch-health-format");
    object_class_property_add_str(oc, "lcd-frame-file",
        best1503_get_lcd_frame_file, best1503_set_lcd_frame_file);
    object_class_property_set_description(oc, "lcd-frame-file",
        "Shared completed RGB565 LCD snapshots for local viewers");
    object_class_property_add_str(oc, "bt-hci-chardev", best1503_get_bt_hci_chardev,
                                  best1503_set_bt_hci_chardev);
    object_class_property_add_str(oc, "resource-image", best1503_get_resource_image,
                                  best1503_set_resource_image);
    object_class_property_add_str(oc, "data-image", best1503_get_data_image,
                                  best1503_set_data_image);
    object_class_property_add_str(oc, "data-spare-image", best1503_get_data_spare_image,
                                  best1503_set_data_spare_image);
    object_class_property_add_bool(oc, "data-writeback", best1503_get_data_writeback,
                                   best1503_set_data_writeback);
    object_class_property_set_description(oc, "data-writeback",
        "Persist private instance NAND data and OOB; caller owns rollback transaction");
    object_class_property_add_str(oc, "nor-image", best1503_get_nor_image,
                                  best1503_set_nor_image);
    object_class_property_add_bool(oc, "nor-writeback", best1503_get_nor_writeback,
                                   best1503_set_nor_writeback);
    object_class_property_set_description(oc, "nor-writeback",
        "Persist exclusive private compact catalog NOR; caller owns rollback transaction");
    object_class_property_add_str(oc, "factory-image", best1503_get_factory_image,
                                  best1503_set_factory_image);
    object_class_property_add_bool(oc, "touch", best1503_get_touch, best1503_set_touch);
    object_class_property_add_bool(oc, "bt-hci-reject-fixture",
        best1503_get_bt_hci_reject_fixture, best1503_set_bt_hci_reject_fixture);
    object_class_property_set_description(oc, "bt-hci-reject-fixture",
        "Unknown HCI Command responses after Reset; requires Reset fixture");
    object_class_property_add_bool(oc, "power-adc-fixture",
        best1503_get_power_adc_fixture, best1503_set_power_adc_fixture);
    object_class_property_set_description(oc, "power-adc-fixture",
        "Uncalibrated raw ADC fixture for enabled PMU channels 0..7");
    object_class_property_add_str(oc, "pmu-adc-raw", NULL, best1503_set_adc_raw);
    object_class_property_set_description(oc, "pmu-adc-raw",
        "Atomically replace eight raw ADC words; dedicated runner input only");
    object_class_property_add_str(oc, "pmu-adc-state", best1503_get_adc_state, NULL);
    object_class_property_set_description(oc, "pmu-adc-state",
        "Raw vector version and actual guest ISPI FIFO read counters");
    object_class_property_set_description(oc, "touch", "CST9217 reports on configured I2C/GPIO from absolute pointer input");
    object_class_property_add_bool(oc, "gpu-software-renderer",
        best1503_get_gpu_software_renderer, best1503_set_gpu_software_renderer);
    object_class_property_set_description(oc, "gpu-software-renderer",
        "Bounded VGLite A8/path software renderer with delayed END IRQ70; requires GPU ID fixture");
    object_class_property_add_bool(oc, "panel-te-fixture",
        best1503_get_panel_te_fixture, best1503_set_panel_te_fixture);
    object_class_property_set_description(oc, "panel-te-fixture",
        "Experimental 60 Hz TE signal on GPIO65 with IRQ44; no GPU pixels");
    object_class_property_add_bool(oc, "bt-hci-reset-fixture",
        best1503_get_bt_hci_reset_fixture, best1503_set_bt_hci_reset_fixture);
    object_class_property_set_description(oc, "bt-hci-reset-fixture",
        "Experimental HCI Reset response through shared memory and IRQ34; no RF/controller ROM");
    object_class_property_add_bool(oc, "gpu-completion-fixture",
        best1503_get_gpu_completion_fixture, best1503_set_gpu_completion_fixture);
    object_class_property_set_description(oc, "gpu-completion-fixture",
        "Experimental bounded END completion and IRQ70; requires gpu-no-render; no pixels");
    object_class_property_add_bool(oc, "gpu-no-render", best1503_get_gpu_no_render,
                                   best1503_set_gpu_no_render);
    object_class_property_set_description(oc, "gpu-no-render",
        "Experimental accept-and-ignore GPU commands; pixels and IRQ not generated");
    object_class_property_add_bool(oc, "gpu-id-fixture", best1503_get_gpu_id_fixture,
                                   best1503_set_gpu_id_fixture);
    object_class_property_set_description(oc, "gpu-id-fixture",
        "Q66 driver identity fixture 265/2002/424; requires gpu-probe; no rendering");
    object_class_property_add_bool(oc, "gpu-probe", best1503_get_gpu_probe,
                                   best1503_set_gpu_probe);
    object_class_property_set_description(oc, "gpu-probe",
        "VGLite reset/idle probe with unsupported identity; no GPU commands or pixels");
    object_class_property_add_bool(oc, "spi0-unconnected",
        best1503_get_spi0_unconnected, best1503_set_spi0_unconnected);
    object_class_property_set_description(oc, "spi0-unconnected",
        "Polled SPI0 with no slave and pull-high MISO; no sensor registers");
    object_class_property_add_bool(oc, "bt-peer", best1503_get_bt_peer,
                                   best1503_set_bt_peer);
    object_class_property_set_description(oc, "bt-peer",
        "Experimental Bluetooth wake and ideal IQ peer (no controller ROM/HCI)");
    object_class_property_add_bool(oc, "bt-tx-capture", best1503_get_bt_tx_capture,
                                   best1503_set_bt_tx_capture);
    object_class_property_set_description(oc, "bt-tx-capture",
        "Capture H4 commands and signal transport TX done; no HCI responses");
    object_class_property_add_bool(oc, "panel-id-fixture",
        best1503_get_panel_id_fixture, best1503_set_panel_id_fixture);
    object_class_property_set_description(oc, "panel-id-fixture",
        "Experimental RM690C0_XINLI ID reads and command capture; no frame output");
    object_class_property_add_bool(oc, "lcd-frame-capture",
        best1503_get_lcd_frame_capture, best1503_set_lcd_frame_capture);
    object_class_property_set_description(oc, "lcd-frame-capture",
        "Experimental bounded RGB565 GRA snapshot and IRQ67; requires panel-id-fixture");
    object_class_property_add_bool(oc, "gh3x2x-fixture",
        best1503_get_gh3x2x_fixture, best1503_set_gh3x2x_fixture);
    object_class_property_set_description(oc, "gh3x2x-fixture",
        "Experimental GH3X2X SPI register fixture; no optical samples or sensor IRQ");
    object_class_property_add(oc, "gh3x2x-reg0724", "uint32",
        best1503_get_gh_reg0724, best1503_set_gh_reg0724, NULL, NULL);
    object_class_property_set_description(oc, "gh3x2x-reg0724",
        "Hypothetical register0724 reset value; 65536 means unknown (default)");
    object_class_property_add(oc, "revision-id", "uint32",
                              best1503_get_revision, best1503_set_revision,
                              NULL, NULL);
    object_class_property_set_description(oc, "revision-id",
        "Provisional AON revision ID (0..15), default 0; not a silicon measurement");
}

static const TypeInfo best1503_type = {
    .name = MACHINE_TYPE_NAME("best1503"),
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(Best1503State),
    .instance_finalize = best1503_finalize,
    .instance_init = best1503_instance_init,
    .class_init = best1503_class_init,
};

static void best1503_register_types(void)
{
    type_register_static(&best1503_type);
}
type_init(best1503_register_types)
