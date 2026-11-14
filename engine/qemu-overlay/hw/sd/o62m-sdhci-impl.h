/* Included once by the full copied sdhci.c, after its standard backend helpers.
 * v10 adds actual opt-in eMMC CMD21 data; the card alone owns its enable.
 * v9 adds only current bounded command/data-contract metadata, never addresses.
 * v8 adds optional declared embedded-card VCC for actual selector0 requests.
 * v7 adds only bounded functional-gate metadata and accepted-submission count.
 * v5 adds explicit functional finite PHY backing and actual timed apply.
 * v4 adds opt-in independent HRS01/BOOT access during real card-line jobs.
 * HRS06 v3 derived from frozen, parent-compiled v2 adapter.
 * HRS06 retained timing field is an explicit virtual profile, not a PHY job.
 * Existing generic types leave SDHCIState.o62m_profile NULL. */
#ifndef O62M_SDHCI_IMPL_H
#define O62M_SDHCI_IMPL_H

typedef struct O62MSDHCIState {
    SDHCIState parent_obj;
    O62MSdhciConfig config;
    O62MSdhciObservation observation;
    O62MHrsV1 hrs;
    O62MPhyV1 phy;
    uint32_t hrs06_fullword;
    O62MV4DataV1 data;
    MemoryRegion outer;
    QEMUTimer *reset_timer;
    QEMUTimer *phy_timer;
    DeviceState *card;
    bool configured, realized, card_bound, card_held, terminal_fault;
    bool clock_available, command_pending;
    bool descriptor_active;
    O62MAdma2DecodedV1 descriptor;
    uint32_t descriptor_remaining;
    uint64_t descriptor_data_address, descriptor_total;
    uint64_t session_generation;
} O62MSDHCIState;

DECLARE_INSTANCE_CHECKER(O62MSDHCIState, O62M_SDHCI, TYPE_O62M_SDHCI)

static void o62m_sdhci_transfer_step(void *opaque);
static void o62m_sdhci_reset_step(void *opaque);
static void o62m_sdhci_phy_step(void *opaque);
static bool o62m_sdhci_arm_phy(O62MSDHCIState *p);
static bool o62m_sdhci_send_command(O62MSDHCIState *p);
static bool o62m_sdhci_card_reset_actual(O62MSDHCIState *p);
static bool o62m_sdhci_boot_empty(O62MSDHCIState *p);
static bool o62m_sdhci_pio_read(O62MSDHCIState *p, unsigned size,
                               uint64_t *value);
static bool o62m_sdhci_pio_write(O62MSDHCIState *p, unsigned size,
                                uint64_t value);
static bool o62m_sdhci_srs_write(O62MSDHCIState *p, hwaddr offset,
                                uint64_t value, unsigned size);

static bool o62m_sdhci_fail(O62MSDHCIState *p, const char *reason,
                           uint64_t offset, unsigned size, bool write,
                           uint64_t value)
{
    if (!p->terminal_fault) {
        if (p->observation.strict_faults != UINT64_MAX) {
            p->observation.strict_faults++;
        }
        p->terminal_fault = true;
        timer_del(p->parent_obj.transfer_timer);
        timer_del(p->reset_timer);
        timer_del(p->phy_timer);
    }
    /* This callback must stop the owned CPU/run. Outer accesses also return
     * MEMTX_ERROR; no rejected transaction becomes a successful zero read. */
    if (p->config.fault) {
        p->config.fault(p->config.opaque, reason, offset, size, write, value);
    }
    return false;
}

static bool o62m_sdhci_increment(O62MSDHCIState *p, uint64_t *counter,
                                uint64_t amount, const char *name)
{
    if (*counter > UINT64_MAX - amount) {
        return o62m_sdhci_fail(p, name, 0, 0, false, 0);
    }
    *counter += amount;
    return true;
}

static bool o62m_sdhci_clock_decode(O62MSDHCIState *p)
{
    bool available = false;
    if (p->terminal_fault) {
        return false;
    }
    if (!p->configured || !p->config.clock ||
        !p->config.clock(p->config.opaque, &available)) {
        return o62m_sdhci_fail(p, "unknown-functional-clock", 0x22c,
                               4, false, 0);
    }
    p->clock_available = available;
    if (available && (p->parent_obj.clkcon & SDHC_CLOCK_INT_EN)) {
        p->parent_obj.clkcon |= SDHC_CLOCK_INT_STABLE;
    } else {
        p->parent_obj.clkcon &= ~SDHC_CLOCK_INT_STABLE;
    }
    return true;
}

/* A fixed external VCC is a declared virtual-board input. It is neither
 * controller voltage-selector readback nor the separate signaling VCCQ bit.
 * It never turns a software POWER_ON=0 request into a powered card. */
static bool o62m_sdhci_fixed_supply_valid(const O62MSdhciConfig *config)
{
    uint64_t voltage_cap;
    if (!config->embedded_fixed_supply_enable ||
        FIELD_EX64(config->capabilities_fullword, SDHC_CAPAB, SLOT_TYPE) != 1) {
        return false;
    }
    if (config->embedded_fixed_supply_mv == 1800) {
        voltage_cap = R_SDHC_CAPAB_V18_MASK;
    } else if (config->embedded_fixed_supply_mv == 3300) {
        voltage_cap = R_SDHC_CAPAB_V33_MASK;
    } else {
        return false;
    }
    return (config->capabilities_fullword & voltage_cap) != 0;
}

static bool o62m_sdhci_power_available(O62MSDHCIState *p)
{
    SDHCIState *s = &p->parent_obj;
    unsigned code = (s->pwrcon >> 1) & 7;
    if (!(s->pwrcon & SDHC_POWER_ON) || !sdbus_get_inserted(&s->sdbus)) {
        return false;
    }
    if (code == 0) {
        return o62m_sdhci_fixed_supply_valid(&p->config);
    }
    /* All nonzero selector encodings retain the old controller contract. */
    return code >= 5 &&
           (s->capareg & (UINT64_C(1) << (31 - code))) != 0;
}

static bool o62m_sdhci_functional(O62MSDHCIState *p)
{
    return (!p->config.phy.explicit_enable || o62m_phy_ready_v1(&p->phy)) &&
           p->clock_available && !p->card_held && p->card_bound &&
           p->hrs.phase == O62M_HRS_IDLE_V1 &&
           SDHC_CLOCK_IS_ON(p->parent_obj.clkcon) &&
           o62m_sdhci_power_available(p);
}

static bool o62m_sdhci_schedule_work(O62MSDHCIState *p)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (p->terminal_fault) {
        return false;
    }
    if (!o62m_sdhci_functional(p)) {
        timer_del(p->parent_obj.transfer_timer);
        return true; /* Known gated/held/request-pending state: no progress. */
    }
    if (now < 0 || p->config.work_latency_ns > (uint64_t)(INT64_MAX - now)) {
        return o62m_sdhci_fail(p, "transfer-deadline-overflow", 0, 0, false, 0);
    }
    if (p->command_pending ||
        (p->data.phase == O62M_V4_PHASE_RUNNING_V1 &&
         p->data.config.engine != O62M_V4_ENGINE_PIO_V1)) {
        timer_mod(p->parent_obj.transfer_timer,
                   now + (int64_t)p->config.work_latency_ns);
    }
    return true;
}

/* The PHY reference clock is independent of SDCLK output, card reset,
 * card power and the host controller reset job. Its owner is the board's actual
 * canonical clock callback. Completion only applies real captured parameters. */
static bool o62m_sdhci_arm_phy(O62MSDHCIState *p)
{
    O62MPhyJobV1 job;
    bool due;
    O62MPhyResultV1 result;
    if (!p->config.phy.explicit_enable) {
        return true;
    }
    if (p->terminal_fault) {
        return false;
    }
    result = o62m_phy_job_due_v1(&p->phy,
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), &job, &due);
    if (result != O62M_PHY_OK_V1) {
        return o62m_sdhci_fail(p, o62m_phy_reason_v1(result), 0x24, 4, false, 0);
    }
    (void)due;
    if (job.generation && p->clock_available) {
        timer_mod(p->phy_timer, job.deadline_ns);
    } else {
        timer_del(p->phy_timer);
    }
    return true;
}

static void o62m_sdhci_phy_step(void *opaque)
{
    O62MSDHCIState *p = opaque;
    O62MPhyJobV1 job;
    bool due;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    O62MPhyResultV1 result;
    if (p->terminal_fault || !p->config.phy.explicit_enable) {
        return;
    }
    result = o62m_phy_job_due_v1(&p->phy, now, &job, &due);
    if (result != O62M_PHY_OK_V1) {
        o62m_sdhci_fail(p, o62m_phy_reason_v1(result), 0x24, 4, false, 0);
        return;
    }
    if (!job.generation) {
        return;
    }
    /* Query the canonical reference-clock provider at the actual apply point;
     * the cached notification value alone never grants initialization. */
    if (!o62m_sdhci_clock_decode(p) || !p->clock_available) {
        return; /* Real clock commit notification re-arms an unchanged job. */
    }
    if (!due) {
        o62m_sdhci_arm_phy(p);
        return;
    }
    /* This function actually validates and copies the captured finite bank
     * into the PHY's private active backing. It cannot accept a caller-supplied
     * success/ready constant. Only its successful commit projects bit1. */
    result = o62m_phy_apply_due_v1(&p->phy, now, &job, true);
    if (result != O62M_PHY_OK_V1) {
        o62m_sdhci_fail(p, o62m_phy_reason_v1(result), 0x24, 4, false, 0);
        return;
    }
    o62m_sdhci_schedule_work(p);
}

static bool o62m_sdhci_phy_port(hwaddr offset)
{
    return offset == 0x10 || offset == 0x14 || offset == 0x1c ||
           offset == 0x24 || offset == 0x28 || offset == 0x40;
}

static bool o62m_sdhci_ram_valid(O62MSDHCIState *p, uint64_t address,
                                uint32_t bytes, bool write)
{
    if (!bytes || address > UINT64_MAX - bytes ||
        !p->config.ram_range ||
        !p->config.ram_range(p->config.opaque, address, bytes, write)) {
        return o62m_sdhci_fail(p, "unproved-dma-ram-range", address,
                               bytes, write, 0);
    }
    return true;
}

static bool o62m_sdhci_raise_error(O62MSDHCIState *p, uint16_t error)
{
    SDHCIState *s = &p->parent_obj;
    if (error & s->errintstsen) {
        s->errintsts |= error & s->errintstsen;
        s->norintsts |= SDHC_NIS_ERR;
    }
    sdhci_update_irq(s);
    return true;
}

static bool o62m_sdhci_count_result(O62MSDHCIState *p,
                                   O62MV4DataResultV1 result)
{
    if (result != O62M_V4_DATA_OK_V1) {
        return o62m_sdhci_fail(p, o62m_v4_data_reason_v1(result),
                               0, 0, false, 0);
    }
    return true;
}

static bool o62m_sdhci_arm_reset(O62MSDHCIState *p)
{
    O62MHrsJobV1 job;
    bool due;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    O62MHrsResultV1 r = o62m_hrs_job_due_v1(&p->hrs, now, &job, &due);
    if (r != O62M_HRS_OK_V1) {
        return o62m_sdhci_fail(p, o62m_hrs_reason_v1(r), 0, 4, false, 0);
    }
    if (p->hrs.phase == O62M_HRS_PENDING_V1) {
        timer_mod(p->reset_timer, due ? now : job.deadline_ns);
    }
    return true;
}

static void o62m_sdhci_reset_step(void *opaque)
{
    O62MSDHCIState *p = opaque;
    O62MHrsV1 hrs_trial = p->hrs;
    O62MV4DataV1 data_trial = p->data;
    O62MHrsJobV1 job;
    O62MHrsCompletionV1 completed = { 0 };
    O62MV4DataBankV1 reset_bank = { 0 };
    O62MHrsResultV1 r;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool due;

    if (p->terminal_fault || !o62m_sdhci_clock_decode(p)) {
        return;
    }
    r = o62m_hrs_job_due_v1(&p->hrs, now, &job, &due);
    if (r != O62M_HRS_OK_V1 || !due) {
        o62m_sdhci_fail(p, r == O62M_HRS_OK_V1 ? "reset-callback-not-due" :
                         o62m_hrs_reason_v1(r), 0, 4, false, 0);
        return;
    }
    if (job.kind == O62M_HRS_JOB_SRS_RESET_ALL_V1 &&
        !p->clock_available) {
        return; /* Pending reset is retained; actual clock change re-arms it. */
    }
    completed.kind = job.kind;
    completed.success = true;
    if (job.kind == O62M_HRS_JOB_SRS_RESET_ALL_V1) {
        completed.completed_srs_mask = O62M_HRS_SRS_RESET_ALL_V1;
        if (o62m_v4_data_active_v1(&data_trial) &&
            !o62m_sdhci_count_result(p, o62m_v4_data_abort_v1(&data_trial))) {
            return;
        }
        if (!o62m_sdhci_count_result(p,
                o62m_v4_data_program_v1(&data_trial, &reset_bank)) ||
            p->observation.controller_resets == UINT64_MAX) {
            if (!p->terminal_fault) {
                o62m_sdhci_fail(p, "controller-reset-observation-overflow",
                                 0, 0, false, 0);
            }
            return;
        }
    } else if (job.kind == O62M_HRS_JOB_CARD_RESET_LINE_V1) {
        completed.applied_card_level = job.logical_card_level;
        if (!p->card_bound ||
            (job.logical_card_level == p->config.card_reset_assert_level &&
             (o62m_v4_data_active_v1(&p->data) || p->command_pending ||
              p->descriptor_active)) ||
            (job.logical_card_level == p->config.card_reset_assert_level ?
             p->observation.card_assertions == UINT64_MAX :
             p->observation.card_deassertions == UINT64_MAX)) {
            o62m_sdhci_fail(p, "card-line-owner-or-counter-invalid",
                             0x2c, 4, true, job.logical_card_level);
            return;
        }
    } else {
        o62m_sdhci_fail(p, "unknown-reset-job", 0, 0, false, 0);
        return;
    }
    /* Pure completion is preflighted on a copy, BEFORE external work. The
     * copy is published only after the actual backend operation succeeds. */
    r = o62m_hrs_complete_v1(&hrs_trial, now, &job, &completed);
    if (r != O62M_HRS_OK_V1) {
        o62m_sdhci_fail(p, o62m_hrs_reason_v1(r), 0, 4, false, 0);
        return;
    }
    if (job.kind == O62M_HRS_JOB_SRS_RESET_ALL_V1) {
        /* Controller reset deliberately does NOT reset or power-cycle card. */
        sdhci_reset(&p->parent_obj);
        sdhci_update_irq(&p->parent_obj);
        p->data = data_trial;
        p->command_pending = false;
        p->descriptor_active = false;
        p->descriptor_remaining = 0;
        p->descriptor_total = 0;
        p->observation.controller_resets++;
    } else if (job.logical_card_level == p->config.card_reset_assert_level) {
        if (!o62m_sdhci_card_reset_actual(p)) {
            return; /* Real card may have changed; never roll it back. */
        }
        p->card_held = true;
        timer_del(p->parent_obj.transfer_timer);
        p->observation.card_assertions++;
    } else {
        /* This is an actual transition of the adapter's explicit virtual
         * wire. It is not a second card cold reset or readiness projection. */
        p->card_held = false;
        p->observation.card_deassertions++;
    }
    p->hrs = hrs_trial;
    o62m_sdhci_schedule_work(p);
}

static bool o62m_sdhci_srs_readable(hwaddr offset)
{
    switch (offset & ~3u) {
    case SDHC_SYSAD:
    case SDHC_BLKSIZE:
    case SDHC_ARGUMENT:
    case SDHC_TRNMOD:
    case SDHC_RSPREG0 ... SDHC_RSPREG3:
    case SDHC_BDATA:
    case SDHC_PRNSTS:
    case SDHC_HOSTCTL:
    case SDHC_CLKCON:
    case SDHC_NORINTSTS:
    case SDHC_NORINTSTSEN:
    case SDHC_NORINTSIGEN:
    case SDHC_ACMD12ERRSTS:
    case SDHC_CAPAB:
    case SDHC_CAPAB + 4:
    case SDHC_MAXCURR:
    case SDHC_MAXCURR + 4:
    case SDHC_ADMAERR:
    case SDHC_ADMASYSADDR:
    case SDHC_ADMASYSADDR + 4:
    case SDHC_SLOT_INT_STATUS:
        return true;
    default:
        return false;
    }
}

static bool o62m_sdhci_srs_read(O62MSDHCIState *p, hwaddr offset,
                               unsigned size, uint64_t *value)
{
    uint32_t full;
    if (!o62m_sdhci_srs_readable(offset)) {
        return o62m_sdhci_fail(p, "unknown-standard-register-read",
                               offset + 0x200, size, false, 0);
    }
    if (!o62m_v4_data_valid_v1(&p->data)) {
        return o62m_sdhci_fail(p, "invalid-canonical-data-bank",
                               offset + 0x200, size, false, 0);
    }
    switch (offset & ~3u) {
    case SDHC_SYSAD:
        full = p->data.bank.saar;
        break;
    case SDHC_BLKSIZE:
        full = p->parent_obj.blksize | ((uint32_t)p->data.bank.bcct16 << 16);
        break;
    case SDHC_ADMASYSADDR:
        full = (uint32_t)p->data.bank.dma58;
        break;
    case SDHC_ADMASYSADDR + 4:
        full = (uint32_t)(p->data.bank.dma58 >> 32);
        break;
    case SDHC_CLKCON:
        if (!o62m_sdhci_clock_decode(p)) {
            return false;
        }
        full = p->parent_obj.clkcon |
               ((uint32_t)p->parent_obj.timeoutcon << 16) |
               ((uint32_t)p->hrs.swr << 24);
        break;
    case SDHC_BDATA:
        if ((p->data.block_offset & 3) != (offset & 3)) {
            return o62m_sdhci_fail(p, "nonsequential-pio-read",
                                   offset + 0x200, size, false, 0);
        }
        return o62m_sdhci_pio_read(p, size, value);
    default:
        /* No canonical address/count/data access can reach this legacy
         * reader, and the two eager timer triggers were disabled by profile. */
        *value = sdhci_read(&p->parent_obj, offset, size);
        return !p->terminal_fault;
    }
    *value = (full >> ((offset & 3) * 8)) &
             ((UINT64_C(1) << (size * 8)) - 1);
    return true;
}

static bool o62m_sdhci_settle_due_work(O62MSDHCIState *p)
{
    QEMUTimer *timer=p->parent_obj.transfer_timer;
    if(timer_pending(timer)&&timer_expire_time_ns(timer)<=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) {
        timer_del(timer);
        o62m_sdhci_transfer_step(p);
    }
    return !p->terminal_fault;
}

static MemTxResult o62m_sdhci_outer_read(void *opaque, hwaddr offset,
                                        uint64_t *value, unsigned size,
                                        MemTxAttrs attrs)
{
    O62MSDHCIState *p = opaque;
    uint64_t result = 0;
    O62MHrsResultV1 r;
    (void)attrs;
    if (p->terminal_fault || !p->card_bound ||
        p->observation.mmio_reads == UINT64_MAX ||
        (size != 1 && size != 2 && size != 4) ||
        (offset & (size - 1)) || (offset & 3) + size > 4) {
        o62m_sdhci_fail(p, "invalid-or-unbound-controller-read", offset,
                         size, false, 0);
        return MEMTX_ERROR;
    }
    if(!o62m_sdhci_settle_due_work(p))return MEMTX_ERROR;
    if (p->config.phy.explicit_enable && o62m_sdhci_phy_port(offset)) {
        O62MPhyResultV1 phy_result = o62m_phy_access_v1(&p->phy,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), offset, size, false, &result);
        if (phy_result != O62M_PHY_OK_V1) {
            o62m_sdhci_fail(p, o62m_phy_reason_v1(phy_result), offset, size, false, 0);
            return MEMTX_ERROR;
        }
    } else if (offset == 0x18 && p->config.hrs06_timing_enable) {
        if (size != 4) {
            o62m_sdhci_fail(p, "hrs06-timing-width-unsupported", offset,
                             size, false, 0);
            return MEMTX_ERROR;
        }
        result = p->hrs06_fullword;
    } else if (offset < 0x200) {
        if (offset == 0x90 && !o62m_sdhci_boot_empty(p)) {
            return MEMTX_ERROR;
        }
        r = o62m_hrs_access_independent_config_v1(&p->hrs,
            p->config.independent_cardline_host_config_enable,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), offset, size, false, &result);
        if (r != O62M_HRS_OK_V1) {
            o62m_sdhci_fail(p, o62m_hrs_reason_v1(r), offset, size, false, 0);
            return MEMTX_ERROR;
        }
    } else if (offset < 0x300) {
        if (!o62m_sdhci_srs_read(p, offset - 0x200, size, &result)) {
            return MEMTX_ERROR;
        }
    } else {
        o62m_sdhci_fail(p, "controller-read-out-of-range", offset,
                         size, false, 0);
        return MEMTX_ERROR;
    }
    if (!o62m_sdhci_increment(p, &p->observation.mmio_reads, 1,
                             "read-observation-overflow")) {
        return MEMTX_ERROR;
    }
    *value = result;
    return MEMTX_OK;
}

static MemTxResult o62m_sdhci_outer_write(void *opaque, hwaddr offset,
                                         uint64_t value, unsigned size,
                                         MemTxAttrs attrs)
{
    O62MSDHCIState *p = opaque;
    O62MHrsResultV1 r;
    (void)attrs;
    if (p->terminal_fault || !p->card_bound ||
        p->observation.mmio_writes == UINT64_MAX ||
        (size != 1 && size != 2 && size != 4) ||
        (offset & (size - 1)) || (offset & 3) + size > 4 ||
        value > ((UINT64_C(1) << (size * 8)) - 1)) {
        o62m_sdhci_fail(p, "invalid-or-unbound-controller-write", offset,
                         size, true, value);
        return MEMTX_ERROR;
    }
    if(!o62m_sdhci_settle_due_work(p))return MEMTX_ERROR;
    if (p->config.phy.explicit_enable && o62m_sdhci_phy_port(offset)) {
        uint64_t input = value;
        O62MPhyResultV1 phy_result;
        if (o62m_v4_data_active_v1(&p->data) || p->command_pending ||
            p->descriptor_active) {
            o62m_sdhci_fail(p, "phy-config-during-active-work-unsupported",
                             offset, size, true, value);
            return MEMTX_ERROR;
        }
        phy_result = o62m_phy_access_v1(&p->phy,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), offset, size, true, &input);
        if (phy_result != O62M_PHY_OK_V1 || !o62m_sdhci_arm_phy(p)) {
            if (!p->terminal_fault) {
                o62m_sdhci_fail(p, o62m_phy_reason_v1(phy_result),
                                 offset, size, true, value);
            }
            return MEMTX_ERROR;
        }
    } else if (offset == 0x18 && p->config.hrs06_timing_enable) {
        if (size != 4 || ((value ^ p->hrs06_fullword) & ~UINT32_C(7))) {
            o62m_sdhci_fail(p, "hrs06-timing-width-or-opaque-change", offset,
                             size, true, value);
            return MEMTX_ERROR;
        }
        /* Configuration retained from the actual host write. No PHY work,
         * busy/ready, event or completion is implied by this selector. */
        p->hrs06_fullword = value;
    } else if (offset < 0x200) {
        uint64_t input = value;
        if (offset == 0x2c &&
            (value & 1) == p->config.card_reset_assert_level &&
            (o62m_v4_data_active_v1(&p->data) || p->command_pending ||
             p->descriptor_active)) {
            o62m_sdhci_fail(p, "card-assert-during-active-work-unsupported",
                             offset, size, true, value);
            return MEMTX_ERROR;
        }
        r = o62m_hrs_access_independent_config_v1(&p->hrs,
            p->config.independent_cardline_host_config_enable,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), offset, size, true, &input);
        if (r != O62M_HRS_OK_V1 || !o62m_sdhci_arm_reset(p)) {
            if (!p->terminal_fault) {
                o62m_sdhci_fail(p, o62m_hrs_reason_v1(r), offset,
                                 size, true, value);
            }
            return MEMTX_ERROR;
        }
    } else if (offset < 0x300) {
        if (!o62m_sdhci_srs_write(p, offset - 0x200, value, size)) {
            return MEMTX_ERROR;
        }
    } else {
        o62m_sdhci_fail(p, "controller-write-out-of-range", offset,
                         size, true, value);
        return MEMTX_ERROR;
    }
    if (!o62m_sdhci_increment(p, &p->observation.mmio_writes, 1,
                             "write-observation-overflow")) {
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps o62m_sdhci_outer_ops = {
    .read_with_attrs = o62m_sdhci_outer_read,
    .write_with_attrs = o62m_sdhci_outer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4,
               .unaligned = false },
    .impl = { .min_access_size = 1, .max_access_size = 4,
              .unaligned = false },
};

static bool o62m_sdhci_card_snapshot(O62MSDHCIState *p,
                                    O62MSdObserverSnapshotV1 *out)
{
    O62MSdObserverResultV1 r;
    if (!p->card_bound || !p->card) {
        return o62m_sdhci_fail(p, "no-bound-real-emmc", 0, 0, false, 0);
    }
    r = o62m_sd_observer_snapshot_v1(p->card, out);
    if (r != O62M_SD_OBSERVER_OK_V1 || out->counter_overflow ||
        out->session != p->config.hrs.instance_id) {
        return o62m_sdhci_fail(p, "invalid-real-card-observation",
                               0, 0, false, 0);
    }
    return true;
}

static bool o62m_sdhci_boot_empty(O62MSDHCIState *p)
{
    O62MSdObserverSnapshotV1 state;
    if (!o62m_sdhci_card_snapshot(p, &state)) {
        return false;
    }
    if (!p->config.hrs.virtual_card_auto_boot_off ||
        p->config.hrs.media_bootcfg != 0 || state.active_part_config != 0) {
        return o62m_sdhci_fail(p, "actual-card-boot-configuration-unknown",
                               0x90, 4, false, 0);
    }
    return true; /* No BOOT request was accepted by this adapter. */
}

static bool o62m_sdhci_card_begin(O62MSDHCIState *p,
                                 O62MSdObserverKindV1 kind,
                                 O62MSdObserverTokenV1 *token)
{
    O62MSdObserverResultV1 r;
    if (!p->card_bound || p->terminal_fault) {
        return o62m_sdhci_fail(p, "real-card-owner-unavailable",
                               0, 0, false, 0);
    }
    r = o62m_sd_observer_begin_v1(p->card, p->config.hrs.instance_id,
                                 kind, token);
    if (r != O62M_SD_OBSERVER_OK_V1) {
        return o62m_sdhci_fail(p, o62m_sd_observer_reason_v1(r),
                               0, 0, false, 0);
    }
    return true;
}

static bool o62m_sdhci_card_finish_count(O62MSDHCIState *p,
                                  const O62MSdObserverTokenV1 *token,
                                  O62MSdObserverReceiptV1 *receipt,
                                  uint32_t expected_calls)
{
    O62MSdObserverResultV1 r =
        o62m_sd_observer_finish_v1(p->card, token, receipt);
    if (r != O62M_SD_OBSERVER_OK_V1) {
        return o62m_sdhci_fail(p, o62m_sd_observer_reason_v1(r),
                               0, 0, false, 0);
    }
    /* Observer success means a receipt exists, never successful card I/O.
     * Keep actual consumed-byte observations even on a reported host error.
     * No card/RAM side effect is restored from a pre-operation snapshot. */
    if (!o62m_sdhci_increment(p, &p->observation.card_bytes_read,
            receipt->read_bytes_consumed, "card-read-observation-overflow") ||
        !o62m_sdhci_increment(p, &p->observation.card_bytes_written,
            receipt->write_bytes_accepted, "card-write-observation-overflow")) {
        return false;
    }
    if (receipt->flags || receipt->actual_calls != expected_calls ||
        receipt->returned_calls != expected_calls) {
        return o62m_sdhci_fail(p,
            (receipt->flags & O62M_SD_OBSERVER_PARTIAL_UNKNOWN_V1) ?
                "real-card-io-error-unknown-partial-effects" :
                "real-card-operation-failed-or-incomplete",
            0, 0, false, 0);
    }
    return true;
}

static bool o62m_sdhci_card_finish(O62MSDHCIState *p,
                                  const O62MSdObserverTokenV1 *token,
                                  O62MSdObserverReceiptV1 *receipt)
{
    return o62m_sdhci_card_finish_count(p, token, receipt, 1);
}

static bool o62m_sdhci_card_bytes(O62MSDHCIState *p, bool write,
                                 uint8_t *buffer, uint32_t bytes,
                                 uint32_t *successful_bytes)
{
    O62MSdObserverTokenV1 token;
    O62MSdObserverReceiptV1 receipt;
    O62MSdObserverResultV1 result;
    *successful_bytes = 0;
    if (!p->card_bound || p->terminal_fault ||
        (write ? p->observation.card_bytes_written :
                 p->observation.card_bytes_read) > UINT64_MAX - bytes) {
        return o62m_sdhci_fail(p, "card-chunk-owner-or-counter-invalid",
                              0x220, 1, write, 0);
    }
    result = o62m_sd_observer_begin_bytes_v1(p->card,
        p->config.hrs.instance_id, write, bytes, &token);
    if (result != O62M_SD_OBSERVER_OK_V1) {
        return o62m_sdhci_fail(p, o62m_sd_observer_reason_v1(result),
                              0x220, 1, write, 0);
    }
    for (uint32_t i = 0; i < bytes; ++i) {
        if (p->card_held || !o62m_sdhci_functional(p) ||
            (write ? !sdbus_receive_ready(&p->parent_obj.sdbus) :
                     !sdbus_data_ready(&p->parent_obj.sdbus))) {
            o62m_sdhci_fail(p, "real-card-data-not-available", 0x220, 1, write, 0);
            o62m_sdhci_card_finish_count(p, &token, &receipt, bytes);
            return false;
        }
        if (write) {
            sdbus_write_byte(&p->parent_obj.sdbus, buffer[i]);
        } else {
            buffer[i] = sdbus_read_byte(&p->parent_obj.sdbus);
        }
        if (!o62m_sd_observer_byte_progress_v1(p->card, &token, i + 1)) {
            /* Finish preserves the actual consumed count, even when the last
             * byte's backend write failed after entering the card buffer. */
            o62m_sdhci_card_finish_count(p, &token, &receipt, bytes);
            return o62m_sdhci_fail(p, "card-chunk-byte-failed-or-incomplete",
                                  0x220, 1, write, 0);
        }
        *successful_bytes = i + 1;
    }
    return o62m_sdhci_card_finish_count(p, &token, &receipt, bytes);
}

static bool o62m_sdhci_card_byte(O62MSDHCIState *p, bool write,
                                uint8_t *value)
{
    O62MSdObserverTokenV1 token;
    O62MSdObserverReceiptV1 receipt;
    O62MSdObserverKindV1 kind = write ? O62M_SD_OBSERVER_WRITE_BYTE_V1 :
                                      O62M_SD_OBSERVER_READ_BYTE_V1;
    if (p->card_held || !o62m_sdhci_functional(p) ||
        (write ? !sdbus_receive_ready(&p->parent_obj.sdbus) :
                 !sdbus_data_ready(&p->parent_obj.sdbus))) {
        return o62m_sdhci_fail(p, "real-card-data-not-available",
                               0x220, 1, write, 0);
    }
    if ((write ? p->observation.card_bytes_written :
                 p->observation.card_bytes_read) == UINT64_MAX) {
        return o62m_sdhci_fail(p, "card-byte-observation-overflow",
                               0x220, 1, write, 0);
    }
    if (!o62m_sdhci_card_begin(p, kind, &token)) {
        return false;
    }
    if (write) {
        sdbus_write_byte(&p->parent_obj.sdbus, *value);
    } else {
        *value = sdbus_read_byte(&p->parent_obj.sdbus);
    }
    if (!o62m_sdhci_card_finish(p, &token, &receipt)) {
        return false;
    }
    if (receipt.actual_consumed_bytes != 1 ||
        (write ? receipt.write_bytes_accepted != 1 :
                 receipt.read_bytes_consumed != 1)) {
        return o62m_sdhci_fail(p, "card-did-not-consume-one-real-byte",
                               0x220, 1, write, 0);
    }
    return true;
}

static bool o62m_sdhci_card_reset_actual(O62MSDHCIState *p)
{
    O62MSdObserverTokenV1 token;
    O62MSdObserverReceiptV1 receipt;
    if (!o62m_sdhci_card_begin(p, O62M_SD_OBSERVER_RESET_V1, &token)) {
        return false;
    }
    /* Actual wire assertion precedes the real backend reset. On a later
     * failure this held state is retained; no fictitious rollback occurs. */
    p->card_held = true;
    timer_del(p->parent_obj.transfer_timer);
    device_cold_reset(p->card);
    if (!o62m_sdhci_card_finish(p, &token, &receipt)) {
        return false;
    }
    if (receipt.before.completed_resets == UINT64_MAX ||
        receipt.after.completed_resets != receipt.before.completed_resets + 1) {
        return o62m_sdhci_fail(p, "no-actual-card-reset-receipt",
                               0x2c, 4, true, 0);
    }
    return true;
}

static bool o62m_sdhci_data_config(O62MSDHCIState *p,
                                  O62MV4DataConfigV1 *config)
{
    SDHCIState *s = &p->parent_obj;
    memset(config, 0, sizeof(*config));
    config->hv4e = FIELD_EX32(s->hostctl2, SDHC_HOSTCTL2, VERSION4);
    config->a64b = (s->hostctl2 & (1u << 13)) != 0;
    config->bce = (s->trnmod & SDHC_TRNS_BLK_CNT_EN) != 0;
    config->multiblock = (s->trnmod & SDHC_TRNS_MULTI) != 0;
    config->block_size = s->blksize & BLOCK_SIZE_MASK;
    config->boundary_code = (s->blksize >> 12) & 7;
    if (config->a64b || (s->trnmod & SDHC_TRNS_ACMD23) ||
        FIELD_EX32(s->hostctl2, SDHC_HOSTCTL2, EXECUTE_TUNING)) {
        return o62m_sdhci_fail(p, "unsupported-64bit-autocmd23-or-tuning",
                               0x23c, 4, true, s->hostctl2);
    }
    if (!(s->trnmod & SDHC_TRNS_DMA)) {
        config->engine = O62M_V4_ENGINE_PIO_V1;
    } else if (SDHC_DMA_TYPE(s->hostctl1) == SDHC_CTRL_SDMA &&
               (s->capareg & R_SDHC_CAPAB_SDMA_MASK)) {
        config->engine = O62M_V4_ENGINE_SDMA_V1;
    } else if (SDHC_DMA_TYPE(s->hostctl1) == SDHC_CTRL_ADMA2_32 &&
               (s->capareg & R_SDHC_CAPAB_ADMA2_MASK)) {
        config->engine = O62M_V4_ENGINE_ADMA_V1;
    } else {
        return o62m_sdhci_fail(p, "unsupported-dma-engine-mode",
                               0x228, 1, true, s->hostctl1);
    }
    return true;
}

static bool o62m_sdhci_command_boot_guard(O62MSDHCIState *p,
                                         const SDRequest *request)
{
    O62MSdObserverSnapshotV1 state;
    if (!o62m_sdhci_card_snapshot(p, &state)) {
        return false;
    }
    if (request->cmd == 0 && request->arg != 0) {
        return o62m_sdhci_fail(p, "unsupported-cmd0-boot-or-preidle-request",
                               0x208, 4, true, request->arg);
    }
    if (request->cmd == 0 && state.reset_default_part_config != 0) {
        return o62m_sdhci_fail(p, "cmd0-would-restore-unmodeled-boot-config",
                               0x208, 4, true, request->arg);
    }
    if (request->cmd == 6) {
        uint8_t index = request->arg >> 16;
        uint8_t access = request->arg >> 24;
        uint8_t value = request->arg >> 8;
        uint8_t next = state.active_part_config;
        if (index == 177) {
            return o62m_sdhci_fail(p, "unimplemented-boot-bus-configuration",
                                   0x208, 4, true, request->arg);
        }
        if (index == 179) {
            switch (access & 3) {
            case 1: next |= value; break;
            case 2: next &= ~value; break;
            case 3: next = value; break;
            default: break;
            }
            if ((next & 0xf8) || (state.active_part_config & 0xf8)) {
                return o62m_sdhci_fail(p, "unimplemented-active-boot-config",
                                       0x208, 4, true, request->arg);
            }
        }
    }
    return true;
}

static bool o62m_sdhci_send_command(O62MSDHCIState *p)
{
    SDHCIState *s = &p->parent_obj;
    SDRequest request = { .cmd = s->cmdreg >> 8, .arg = s->argument };
    uint8_t response[16] = { 0 };
    size_t rlen, expected;
    O62MSdObserverTokenV1 token;
    O62MSdObserverReceiptV1 receipt;
    O62MV4DataV1 trial = p->data;
    O62MV4DataConfigV1 config;
    bool data_command = (s->cmdreg & SDHC_CMD_DATA_PRESENT) != 0;
    bool active = o62m_v4_data_active_v1(&p->data);

    if (!o62m_sdhci_functional(p) || !p->command_pending) {
        return o62m_sdhci_fail(p, "command-callback-without-functional-owner",
                               0x20c, 4, false, 0);
    }
    /* CMD21 is always ADTC. Reject a host request without its data phase
     * before calling the real card, otherwise the card would start sending
     * 128 bytes with no corresponding host data-job owner. */
    if (request.cmd == 21 && !data_command) {
        return o62m_sdhci_fail(p, "cmd21-requires-host-data-present",
                               0x20c, 4, false, 0);
    }
    if (p->observation.commands == UINT64_MAX ||
        p->observation.command_failures == UINT64_MAX ||
        (active && (data_command ||
                    (request.cmd != 12 && request.cmd != 13)))) {
        return o62m_sdhci_fail(p, "unsupported-concurrent-command-or-limit",
                               0x20c, 4, true, s->cmdreg);
    }
    if (!o62m_sdhci_command_boot_guard(p, &request)) {
        return false;
    }
    if (data_command) {
        bool read = (s->trnmod & SDHC_TRNS_READ) != 0;
        bool multi = (s->trnmod & SDHC_TRNS_MULTI) != 0;
        /* These real eMMC commands are the supported data contract. The
         * pure accounting core cannot infer single/multi-block from BCE. */
        if ((request.cmd == 21 && (!read || multi ||
                                  (s->blksize & BLOCK_SIZE_MASK) != 128)) ||
            (request.cmd == 8 && (!read || multi ||
                                 (s->blksize & BLOCK_SIZE_MASK) != 512)) ||
            (request.cmd == 17 && (!read || multi)) ||
            (request.cmd == 18 && (!read || !multi)) ||
            (request.cmd == 24 && (read || multi)) ||
            (request.cmd == 25 && (read || !multi)) ||
            (request.cmd != 8 && request.cmd != 17 && request.cmd != 18 &&
             request.cmd != 21 && request.cmd != 24 && request.cmd != 25)) {
            return o62m_sdhci_fail(p, "unimplemented-or-inconsistent-data-command",
                                   0x20c, 4, true, s->cmdreg);
        }
        if (request.cmd == 21) {
            O62MSdObserverSnapshotV1 card_mode;
            if (!o62m_sdhci_card_snapshot(p, &card_mode)) {
                return false;
            }
            if (!card_mode.cmd21_tuning_enabled || !card_mode.in_transfer_state ||
                card_mode.bus_width_config != 2 ||
                (card_mode.hs_timing & 15) != 2 || request.arg != 0) {
                return o62m_sdhci_fail(p, "cmd21-card-extension-or-actual-mode-unavailable",
                                       0x20c, 4, false, 0);
            }
        }
        if (!o62m_sdhci_data_config(p, &config) ||
            !o62m_sdhci_count_result(p, o62m_v4_data_begin_v1(&trial, &config))) {
            return false;
        }
    }
    if (!o62m_sdhci_card_begin(p, O62M_SD_OBSERVER_COMMAND_V1, &token)) {
        return false;
    }
    /* Standard error/status words stay sticky until their explicit W1C.
     * A new accepted command does not erase an earlier failure. */
    p->command_pending = false;
    p->observation.commands++;
    rlen = sdbus_do_command(&s->sdbus, &request, response, sizeof(response));
    if (!o62m_sdhci_card_finish(p, &token, &receipt)) {
        return false;
    }
    if (receipt.command_response_bytes != rlen) {
        return o62m_sdhci_fail(p, "command-response-receipt-mismatch",
                               0x20c, 4, false, 0);
    }
    expected = (s->cmdreg & SDHC_CMD_RESPONSE) == 0 ? 0 :
               (s->cmdreg & SDHC_CMD_RESPONSE) == 1 ? 16 : 4;
    s->prnsts &= ~SDHC_CMD_INHIBIT;
    if (rlen != expected) {
        p->observation.command_failures++;
        return o62m_sdhci_raise_error(p, SDHC_EIS_CMDTIMEOUT);
    }
    if (rlen == 4) {
        s->rspreg[0] = ldl_be_p(response);
        s->rspreg[1] = s->rspreg[2] = s->rspreg[3] = 0;
    } else if (rlen == 16) {
        s->rspreg[0] = ldl_be_p(&response[11]);
        s->rspreg[1] = ldl_be_p(&response[7]);
        s->rspreg[2] = ldl_be_p(&response[3]);
        s->rspreg[3] = (response[0] << 16) | (response[1] << 8) | response[2];
    }
    if (s->norintstsen & SDHC_NISEN_CMDCMP) {
        s->norintsts |= SDHC_NIS_CMDCMP;
    }
    if (request.cmd == 12 && active) {
        if ((ldl_be_p(response) | receipt.after.card_status) &
            (OUT_OF_RANGE | ADDRESS_ERROR | BLOCK_LEN_ERROR | COM_CRC_ERROR |
             ILLEGAL_COMMAND | CARD_ECC_FAILED | CC_ERROR | SD_ERROR)) {
            return o62m_sdhci_fail(p, "actual-manual-stop-returned-card-error",
                                   0x20c, 4, false, 0);
        }
        if (!o62m_sdhci_count_result(p, o62m_v4_data_abort_v1(&p->data))) {
            return false;
        }
        p->descriptor_active = false;
        s->prnsts &= ~(SDHC_DATA_INHIBIT | SDHC_DAT_LINE_ACTIVE |
                       SDHC_DOING_READ | SDHC_DOING_WRITE |
                       SDHC_SPACE_AVAILABLE | SDHC_DATA_AVAILABLE);
    }
    if (data_command) {
        uint32_t errors = OUT_OF_RANGE | ADDRESS_ERROR | BLOCK_LEN_ERROR |
            ERASE_SEQ_ERROR | ERASE_PARAM | WP_VIOLATION | LOCK_UNLOCK_FAILED |
            COM_CRC_ERROR | ILLEGAL_COMMAND | CARD_ECC_FAILED | CC_ERROR |
            SD_ERROR | CID_CSD_OVERWRITE | WP_ERASE_SKIP | AKE_SEQ_ERROR;
        if ((receipt.after.card_status | ldl_be_p(response)) & errors) {
            /* A real error response is delivered, not turned into data ready. */
            p->observation.command_failures++;
            sdhci_update_irq(s);
            return true;
        }
        p->data = trial;
        p->descriptor_active = false;
        p->descriptor_remaining = 0;
        p->descriptor_total = 0;
        if (p->data.phase == O62M_V4_PHASE_EMPTY_V1) {
            return o62m_sdhci_fail(p, "zero-count-data-command-no-work",
                                   0x204, 4, false, 0);
        }
        s->prnsts |= SDHC_DATA_INHIBIT | SDHC_DAT_LINE_ACTIVE;
        if (s->trnmod & SDHC_TRNS_READ) {
            s->prnsts |= SDHC_DOING_READ;
            if (p->data.config.engine == O62M_V4_ENGINE_PIO_V1 &&
                sdbus_data_ready(&s->sdbus)) {
                s->prnsts |= SDHC_DATA_AVAILABLE;
                if (s->norintstsen & SDHC_NISEN_RBUFRDY) {
                    s->norintsts |= SDHC_NIS_RBUFRDY;
                }
            }
        } else {
            s->prnsts |= SDHC_DOING_WRITE;
            if (p->data.config.engine == O62M_V4_ENGINE_PIO_V1 &&
                sdbus_receive_ready(&s->sdbus)) {
                s->prnsts |= SDHC_SPACE_AVAILABLE;
                if (s->norintstsen & SDHC_NISEN_WBUFRDY) {
                    s->norintsts |= SDHC_NIS_WBUFRDY;
                }
            }
        }
    } else if ((s->cmdreg & SDHC_CMD_RESPONSE) == SDHC_CMD_RSP_WITH_BUSY) {
        if (!(sdbus_get_dat_lines(&s->sdbus) & 1)) {
            return o62m_sdhci_fail(p, "unimplemented-real-card-busy-duration",
                                   0x224, 4, false, 0);
        }
        if (s->norintstsen & SDHC_NISEN_TRSCMP) {
            s->norintsts |= SDHC_NIS_TRSCMP;
        }
    }
    sdhci_update_irq(s);
    return true;
}

static bool o62m_sdhci_finish_data(O62MSDHCIState *p)
{
    SDHCIState *s = &p->parent_obj;
    if (p->data.phase != O62M_V4_PHASE_DONE_V1 ||
        p->observation.completed_data_jobs == UINT64_MAX) {
        return o62m_sdhci_fail(p, "data-end-without-real-exhaustion",
                               0x230, 4, false, 0);
    }
    if (s->trnmod & SDHC_TRNS_ACMD12) {
        SDRequest request = { .cmd = 12, .arg = 0 };
        uint8_t response[16] = { 0 };
        size_t rlen;
        O62MSdObserverTokenV1 token;
        O62MSdObserverReceiptV1 receipt;
        if (p->observation.actual_auto_cmd12 == UINT64_MAX ||
            !o62m_sdhci_card_begin(p, O62M_SD_OBSERVER_COMMAND_V1, &token)) {
            return false;
        }
        rlen = sdbus_do_command(&s->sdbus, &request, response, sizeof(response));
        p->observation.actual_auto_cmd12++;
        if (!o62m_sdhci_card_finish(p, &token, &receipt)) {
            return false;
        }
        if (rlen != 4 || receipt.command_response_bytes != rlen ||
            (rlen == 4 && ((ldl_be_p(response) | receipt.after.card_status) &
                          (OUT_OF_RANGE | ADDRESS_ERROR | BLOCK_LEN_ERROR |
                           COM_CRC_ERROR | ILLEGAL_COMMAND | CARD_ECC_FAILED |
                           CC_ERROR | SD_ERROR)))) {
            s->acmd12errsts |= R_SDHC_ACMD12ERRSTS_TIMEOUT_ERR_MASK;
            o62m_sdhci_raise_error(p, SDHC_EIS_CMD12ERR);
            return o62m_sdhci_fail(p, "actual-auto-cmd12-failed",
                                   0x23c, 4, false, 0);
        }
        if (!(sdbus_get_dat_lines(&s->sdbus) & 1)) {
            return o62m_sdhci_fail(p, "unimplemented-auto-cmd12-busy-duration",
                                   0x224, 4, false, 0);
        }
        s->rspreg[3] = ldl_be_p(response);
    }
    p->observation.completed_data_jobs++;
    s->prnsts &= ~(SDHC_DOING_READ | SDHC_DOING_WRITE | SDHC_DAT_LINE_ACTIVE |
                   SDHC_DATA_INHIBIT | SDHC_SPACE_AVAILABLE |
                   SDHC_DATA_AVAILABLE);
    if (s->norintstsen & SDHC_NISEN_TRSCMP) {
        s->norintsts |= SDHC_NIS_TRSCMP;
    }
    sdhci_update_irq(s);
    return true;
}

static bool o62m_sdhci_trial_chunk(O62MSDHCIState *p,
                                  const O62MV4DataPlanV1 *plan, uint32_t bytes,
                                  O62MV4DataV1 *trial,
                                  O62MV4DataFactsV1 *facts)
{
    *trial = p->data;
    if (!o62m_sdhci_count_result(p, o62m_v4_data_commit_v1(
            trial, plan, bytes, o62m_sdhci_functional(p), facts))) {
        return false;
    }
    if (facts->boundary_hit && facts->data_exhausted &&
        p->config.terminal_boundary_policy == O62M_SDHCI_TERMINAL_BOUNDARY_STRICT) {
        return o62m_sdhci_fail(p, "terminal-boundary-irq-policy-required",
                               0x230, 4, false, 0);
    }
    if ((facts->boundary_hit && p->observation.boundary_events == UINT64_MAX) ||
        (facts->data_exhausted &&
         p->observation.completed_data_jobs == UINT64_MAX)) {
        return o62m_sdhci_fail(p, "data-fact-observation-overflow",
                               0, 0, false, 0);
    }
    return true;
}

static bool o62m_sdhci_publish_chunk(O62MSDHCIState *p,
                                    const O62MV4DataV1 *trial,
                                    const O62MV4DataFactsV1 *facts,
                                    bool defer_end)
{
    SDHCIState *s = &p->parent_obj;
    p->data = *trial; /* Only called after every actual corresponding byte. */
    if (facts->boundary_hit) {
        p->observation.boundary_events++;
        if ((!facts->data_exhausted ||
             p->config.terminal_boundary_policy == O62M_SDHCI_TERMINAL_BOUNDARY_IRQ) &&
            (s->norintstsen & SDHC_NISEN_DMA)) {
            s->norintsts |= SDHC_NIS_DMA;
        }
    }
    if (facts->data_exhausted && !defer_end) {
        return o62m_sdhci_finish_data(p);
    }
    sdhci_update_irq(s);
    return true;
}

/* A failed card write can leave a positively acknowledged preceding prefix.
 * Retire only those bytes into the canonical accounting owner, with no IRQ or
 * completion publication. The failed byte and any uncertain host persistence
 * remain described by the observer receipt and the terminal strict fault. */
static void o62m_sdhci_failed_write_prefix(O62MSDHCIState *p,
                                          const O62MV4DataPlanV1 *plan,
                                          uint32_t successful_bytes)
{
    O62MV4DataV1 trial = p->data;
    O62MV4DataFactsV1 facts;
    O62MV4DataResultV1 result;
    if (!successful_bytes) {
        return;
    }
    /* Serialized actual I/O began with a valid functional clock grant. A
     * strict host-I/O failure does not retrospectively revoke that grant. */
    result = o62m_v4_data_commit_v1(&trial, plan, successful_bytes, true, &facts);
    if (result != O62M_V4_DATA_OK_V1 || facts.data_exhausted ||
        facts.boundary_hit || facts.completed_blocks) {
        o62m_sdhci_fail(p, "failed-write-prefix-accounting-inconsistent",
                         0, 0, false, 0);
        return;
    }
    p->data = trial;
}

static bool o62m_sdhci_dma_chunk(O62MSDHCIState *p,
                                const O62MV4DataPlanV1 *plan,
                                uint64_t address, uint32_t bytes,
                                bool defer_end)
{
    SDHCIState *s = &p->parent_obj;
    const MemTxAttrs attrs = { .memory = true,
                               .secure = p->config.dma_secure };
    bool to_ram = (s->trnmod & SDHC_TRNS_READ) != 0;
    O62MV4DataV1 trial;
    O62MV4DataFactsV1 facts;
    MemTxResult memory_result;
    uint64_t *ram_observation = to_ram ? &p->observation.ram_bytes_written :
                                         &p->observation.ram_bytes_read;

    if (bytes > s->buf_maxsz || *ram_observation > UINT64_MAX - bytes ||
        !o62m_sdhci_trial_chunk(p, plan, bytes, &trial, &facts) ||
        !o62m_sdhci_ram_valid(p, address, bytes, to_ram)) {
        if (!p->terminal_fault) {
            o62m_sdhci_fail(p, "dma-chunk-or-counter-invalid", address,
                             bytes, to_ram, 0);
        }
        return false;
    }
    if (!address_space_access_valid(s->dma_as, address, bytes, to_ram, attrs)) {
        return o62m_sdhci_fail(p, "actual-dma-address-space-rejected",
                               address, bytes, to_ram, 0);
    }
    if (to_ram) {
        uint32_t successful_bytes;
        if (!o62m_sdhci_card_bytes(p, false, s->fifo_buffer, bytes,
                                   &successful_bytes)) {
            return false; /* Actual consumed prefix stays in receipt stats. */
        }
        memory_result = dma_memory_write(s->dma_as, address, s->fifo_buffer,
                                          bytes, attrs);
    } else {
        memory_result = dma_memory_read(s->dma_as, address, s->fifo_buffer,
                                         bytes, attrs);
    }
    if (memory_result != MEMTX_OK) {
        /* A failed memory transaction need not be atomic in the AddressSpace.
         * Do not invent a successful prefix or roll RAM/card effects back. */
        return o62m_sdhci_fail(p, "actual-dma-error-unknown-partial-effects",
                               address, bytes, to_ram, 0);
    }
    *ram_observation += bytes;
    if (!to_ram) {
        uint32_t successful_bytes;
        if (!o62m_sdhci_card_bytes(p, true, s->fifo_buffer, bytes,
                                   &successful_bytes)) {
            o62m_sdhci_failed_write_prefix(p, plan, successful_bytes);
            return false;
        }
    }
    return o62m_sdhci_publish_chunk(p, &trial, &facts, defer_end);
}

static bool o62m_sdhci_pio_read(O62MSDHCIState *p, unsigned size,
                               uint64_t *value)
{
    O62MV4DataPlanV1 plan;
    O62MV4DataV1 trial;
    O62MV4DataFactsV1 facts;
    uint64_t result = 0;
    if (!o62m_sdhci_clock_decode(p) ||
        p->data.config.engine != O62M_V4_ENGINE_PIO_V1 ||
        !(p->parent_obj.trnmod & SDHC_TRNS_READ) ||
        !o62m_sdhci_count_result(p, o62m_v4_data_plan_v1(
            &p->data, size, o62m_sdhci_functional(p), &plan))) {
        return p->terminal_fault ? false :
            o62m_sdhci_fail(p, "pio-read-without-matching-job",
                             0x220, size, false, 0);
    }
    if (plan.bytes != size ||
        !o62m_sdhci_trial_chunk(p, &plan, size, &trial, &facts)) {
        return p->terminal_fault ? false :
            o62m_sdhci_fail(p, "pio-read-crosses-block", 0x220, size, false, 0);
    }
    for (unsigned i = 0; i < size; i++) {
        uint8_t byte;
        if (!o62m_sdhci_card_byte(p, false, &byte)) {
            return false;
        }
        result |= (uint64_t)byte << (i * 8);
    }
    if (!o62m_sdhci_publish_chunk(p, &trial, &facts, false)) {
        return false;
    }
    if (facts.completed_blocks && !facts.data_exhausted &&
        (p->parent_obj.norintstsen & SDHC_NISEN_RBUFRDY)) {
        p->parent_obj.norintsts |= SDHC_NIS_RBUFRDY;
        sdhci_update_irq(&p->parent_obj);
    }
    *value = result;
    return true;
}

static bool o62m_sdhci_pio_write(O62MSDHCIState *p, unsigned size,
                                uint64_t value)
{
    O62MV4DataPlanV1 plan;
    O62MV4DataV1 trial;
    O62MV4DataFactsV1 facts;
    if (!o62m_sdhci_clock_decode(p) ||
        p->data.config.engine != O62M_V4_ENGINE_PIO_V1 ||
        (p->parent_obj.trnmod & SDHC_TRNS_READ) ||
        !o62m_sdhci_count_result(p, o62m_v4_data_plan_v1(
            &p->data, size, o62m_sdhci_functional(p), &plan))) {
        return p->terminal_fault ? false :
            o62m_sdhci_fail(p, "pio-write-without-matching-job",
                             0x220, size, true, 0);
    }
    if (plan.bytes != size ||
        !o62m_sdhci_trial_chunk(p, &plan, size, &trial, &facts)) {
        return p->terminal_fault ? false :
            o62m_sdhci_fail(p, "pio-write-crosses-block", 0x220, size, true, 0);
    }
    for (unsigned i = 0; i < size; i++) {
        uint8_t byte = value >> (i * 8);
        if (!o62m_sdhci_card_byte(p, true, &byte)) {
            o62m_sdhci_failed_write_prefix(p, &plan, i);
            return false;
        }
    }
    if (!o62m_sdhci_publish_chunk(p, &trial, &facts, false)) {
        return false;
    }
    if (facts.completed_blocks && !facts.data_exhausted &&
        (p->parent_obj.norintstsen & SDHC_NISEN_WBUFRDY)) {
        p->parent_obj.norintsts |= SDHC_NIS_WBUFRDY;
        sdhci_update_irq(&p->parent_obj);
    }
    return true;
}

static bool o62m_sdhci_adma_fetch(O62MSDHCIState *p)
{
    SDHCIState *s = &p->parent_obj;
    uint8_t bytes[16];
    O62MAdma2ModeV1 mode = { .address_bits = 32,
        .v4 = FIELD_EX32(s->hostctl2, SDHC_HOSTCTL2, VERSION4),
        .length26 = FIELD_EX32(s->hostctl2, SDHC_HOSTCTL2, ADMA2_LENGTH) };
    uint8_t length = o62m_adma2_stride_v1(&mode);
    uint64_t address = (uint32_t)p->data.bank.dma58;
    const MemTxAttrs attrs = { .memory = true,
                               .secure = p->config.dma_secure };
    O62MAdma2ResultV1 result;
    if (!length || p->descriptor_total >= p->config.total_descriptor_limit ||
        p->observation.descriptor_fetches == UINT64_MAX ||
        p->observation.ram_bytes_read > UINT64_MAX - length ||
        (address & 3) ||
        address > UINT32_MAX - length ||
        !o62m_sdhci_ram_valid(p, address, length, false)) {
        return p->terminal_fault ? false :
            o62m_sdhci_fail(p, "adma-descriptor-bound-or-limit",
                             address, length, false, 0);
    }
    if (!address_space_access_valid(s->dma_as, address, length, false, attrs) ||
        dma_memory_read(s->dma_as, address, bytes, length, attrs) != MEMTX_OK) {
        return o62m_sdhci_fail(p, "actual-adma-descriptor-read-failed",
                               address, length, false, 0);
    }
    p->observation.descriptor_fetches++;
    p->observation.ram_bytes_read += length;
    p->descriptor_total++;
    result = o62m_adma2_decode_v1(bytes, length, &mode, &p->descriptor);
    if (result != O62M_ADMA2_V1_OK) {
        return o62m_sdhci_fail(p, o62m_adma2_reason_v1(result),
                               address, length, false, 0);
    }
    p->descriptor_remaining = p->descriptor.transfer_bytes;
    p->descriptor_data_address = p->descriptor.address;
    p->descriptor_active = true;
    return true;
}

static bool o62m_sdhci_adma_finish_descriptor(O62MSDHCIState *p)
{
    SDHCIState *s = &p->parent_obj;
    uint64_t next = (uint32_t)p->data.bank.dma58;
    if (p->observation.descriptors_completed == UINT64_MAX) {
        return o62m_sdhci_fail(p, "descriptor-completion-counter-overflow",
                               0x258, 4, false, 0);
    }
    if (p->descriptor.action == 3) {
        next = p->descriptor.address;
    } else {
        next += p->descriptor.descriptor_bytes;
    }
    if (next > UINT32_MAX) {
        return o62m_sdhci_fail(p, "descriptor-pointer-overflow",
                               0x258, 4, false, 0);
    }
    p->data.bank.dma58 = (p->data.bank.dma58 & UINT64_C(0xffffffff00000000)) |
                         (uint32_t)next;
    p->observation.descriptors_completed++;
    p->descriptor_active = false;
    if (p->descriptor.interrupt && (s->norintstsen & SDHC_NISEN_DMA)) {
        s->norintsts |= SDHC_NIS_DMA;
        sdhci_update_irq(s);
    }
    if (p->descriptor.end) {
        if (p->data.phase != O62M_V4_PHASE_DONE_V1) {
            s->admaerr = SDHC_ADMAERR_LENGTH_MISMATCH | SDHC_ADMAERR_STATE_ST_TFR;
            o62m_sdhci_raise_error(p, SDHC_EIS_ADMAERR);
            return o62m_sdhci_fail(p, "actual-adma-end-before-real-count",
                                   0x258, 4, false, 0);
        }
        return o62m_sdhci_finish_data(p);
    }
    if (p->data.phase == O62M_V4_PHASE_DONE_V1) {
        /* Count-derived completion does not need an invented END descriptor. */
        return o62m_sdhci_finish_data(p);
    }
    return true;
}

static void o62m_sdhci_transfer_step(void *opaque)
{
    O62MSDHCIState *p = opaque;
    uint32_t budget = p->config.bytes_per_tick;
    uint32_t descriptor_budget = p->config.descriptors_per_tick;
    if (p->terminal_fault || !o62m_sdhci_clock_decode(p) ||
        !o62m_sdhci_functional(p)) {
        return;
    }
    if (p->command_pending && !o62m_sdhci_send_command(p)) {
        return;
    }
    while (!p->terminal_fault && budget &&
           p->data.phase == O62M_V4_PHASE_RUNNING_V1 &&
           p->data.config.engine != O62M_V4_ENGINE_PIO_V1) {
        O62MV4DataPlanV1 plan;
        uint32_t bytes;
        uint64_t address;
        bool adma = p->data.config.engine == O62M_V4_ENGINE_ADMA_V1;
        if (adma && !p->descriptor_active) {
            if (!descriptor_budget) {
                break;
            }
            descriptor_budget--;
            if (!o62m_sdhci_adma_fetch(p)) {
                return;
            }
        }
        if (adma && p->descriptor.action != 2) {
            if (!o62m_sdhci_adma_finish_descriptor(p)) {
                return;
            }
            continue;
        }
        bytes = adma ? MIN(budget, p->descriptor_remaining) : budget;
        if (!o62m_sdhci_count_result(p, o62m_v4_data_plan_v1(
                &p->data, bytes, true, &plan))) {
            return;
        }
        bytes = plan.bytes;
        address = adma ? p->descriptor_data_address : plan.address;
        if (adma && address > UINT32_MAX - bytes) {
            o62m_sdhci_fail(p, "adma-data-address-overflow", address,
                             bytes, false, 0);
            return;
        }
        if (!o62m_sdhci_dma_chunk(p, &plan, address, bytes, adma)) {
            return;
        }
        budget -= bytes;
        if (adma) {
            p->descriptor_data_address += bytes;
            p->descriptor_remaining -= bytes;
            if (p->data.phase == O62M_V4_PHASE_DONE_V1 &&
                p->descriptor_remaining) {
                p->parent_obj.admaerr = SDHC_ADMAERR_LENGTH_MISMATCH |
                                       SDHC_ADMAERR_STATE_ST_TFR;
                o62m_sdhci_raise_error(p, SDHC_EIS_ADMAERR);
                o62m_sdhci_fail(p, "adma-length-exceeds-real-count",
                                 0x258, 4, false, 0);
                return;
            }
            if (!p->descriptor_remaining &&
                !o62m_sdhci_adma_finish_descriptor(p)) {
                return;
            }
        }
    }
    o62m_sdhci_schedule_work(p);
}

static uint32_t o62m_sdhci_merge32(uint32_t old, hwaddr offset,
                                  unsigned size, uint64_t value)
{
    unsigned shift = (offset & 3) * 8;
    uint32_t bits = (uint32_t)(((UINT64_C(1) << (size * 8)) - 1) << shift);
    return (old & ~bits) | ((uint32_t)value << shift);
}

static bool o62m_sdhci_program_bank(O62MSDHCIState *p,
                                   const O62MV4DataBankV1 *bank)
{
    O62MV4DataV1 trial = p->data;
    if (!o62m_sdhci_count_result(p, o62m_v4_data_program_v1(&trial, bank))) {
        return false;
    }
    p->data = trial;
    return true;
}

static bool o62m_sdhci_address_write(O62MSDHCIState *p, hwaddr offset,
                                    unsigned size, uint64_t value)
{
    O62MV4DataBankV1 bank = p->data.bank;
    O62MV4DataV1 trial = p->data;
    bool active = o62m_v4_data_active_v1(&p->data);
    bool ignored;
    uint32_t full;

    if ((offset & ~3u) == SDHC_SYSAD) {
        full = o62m_sdhci_merge32(bank.saar, offset, size, value);
        if (!active) {
            bank.saar = full;
            return o62m_sdhci_program_bank(p, &bank);
        }
        if (p->data.count_source == O62M_V4_COUNT32_V1) {
            return o62m_sdhci_count_result(p,
                o62m_v4_data_count_write_v1(&p->data, true, full, &ignored));
        }
        if (p->data.phase != O62M_V4_PHASE_WAIT_BOUNDARY_V1 ||
            p->data.config.engine != O62M_V4_ENGINE_SDMA_V1 ||
            p->data.config.hv4e) {
            return o62m_sdhci_fail(p, "active-SAAR-not-count-or-legacy-resume",
                                   0x200 + offset, size, true, value);
        }
        /* One canonical address word. Partial writes at the stopped boundary
         * update that same word; its highest byte is the legacy resume strobe. */
        if ((offset & 3) + size != 4) {
            p->data.bank.saar = full;
            return true;
        }
        if (!o62m_sdhci_count_result(p,
                o62m_v4_data_resume_v1(&trial, full))) {
            return false;
        }
        p->data = trial;
        return o62m_sdhci_schedule_work(p);
    }

    full = o62m_sdhci_merge32((offset & ~3u) == SDHC_ADMASYSADDR ?
                              (uint32_t)bank.dma58 : (uint32_t)(bank.dma58 >> 32),
                              offset, size, value);
    if (!active) {
        if ((offset & ~3u) == SDHC_ADMASYSADDR) {
            bank.dma58 = (bank.dma58 & UINT64_C(0xffffffff00000000)) | full;
        } else {
            bank.dma58 = (bank.dma58 & UINT64_C(0xffffffff)) |
                         ((uint64_t)full << 32);
        }
        return o62m_sdhci_program_bank(p, &bank);
    }
    if (p->data.phase != O62M_V4_PHASE_WAIT_BOUNDARY_V1 ||
        p->data.config.engine != O62M_V4_ENGINE_SDMA_V1 ||
        !p->data.config.hv4e || p->data.config.a64b ||
        !p->config.allow_v4_32_resume || offset != SDHC_ADMASYSADDR || size != 4) {
        error_report("MARCONI_EMMC_ACTIVE_ADDRESS_CLASS phase=%u engine=%u hv4e=%u a64=%u allow-resume=%u offset=%x width=%u command-pending=%u timer-pending=%u timer-due=%u no-address-or-data-output=true",
            p->data.phase,p->data.config.engine,p->data.config.hv4e,p->data.config.a64b,
            p->config.allow_v4_32_resume,(unsigned)offset,size,p->command_pending,
            timer_pending(p->parent_obj.transfer_timer),
            timer_pending(p->parent_obj.transfer_timer)&&timer_expire_time_ns(p->parent_obj.transfer_timer)<=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        return o62m_sdhci_fail(p, "active-DMA58-write-needs-proven-v4-32-resume",
                               0x200 + offset, size, true, value);
    }
    if (!o62m_sdhci_count_result(p,
            o62m_v4_data_resume_v1(&trial, full))) {
        return false;
    }
    p->data = trial;
    return o62m_sdhci_schedule_work(p);
}

static bool o62m_sdhci_srs_write(O62MSDHCIState *p, hwaddr offset,
                                uint64_t value, unsigned size)
{
    SDHCIState *s = &p->parent_obj;
    uint32_t full, old;
    bool active = o62m_v4_data_active_v1(&p->data);
    unsigned shift = (offset & 3) * 8;
    uint32_t touched = (uint32_t)(((UINT64_C(1) << (size * 8)) - 1) << shift);

    if (!o62m_sdhci_clock_decode(p)) {
        return false;
    }
    if (!o62m_v4_data_valid_v1(&p->data)) {
        return o62m_sdhci_fail(p, "invalid-canonical-data-write",
                               0x200 + offset, size, true, value);
    }
    if (p->hrs.phase != O62M_HRS_IDLE_V1 &&
        (offset & ~3u) != SDHC_CLKCON &&
        !o62m_hrs_independent_srs_config_access_v1(&p->hrs,
            p->config.independent_cardline_host_config_enable, offset)) {
        return o62m_sdhci_fail(p, "standard-write-during-reset-job",
                               0x200 + offset, size, true, value);
    }
    switch (offset & ~3u) {
    case SDHC_SYSAD:
    case SDHC_ADMASYSADDR:
    case SDHC_ADMASYSADDR + 4:
        return o62m_sdhci_address_write(p, offset, size, value);
    case SDHC_BLKSIZE:
        old = s->blksize | ((uint32_t)p->data.bank.bcct16 << 16);
        full = o62m_sdhci_merge32(old, offset, size, value);
        if (active) {
            bool ignored;
            /* SDHCI inhibits block-size programming and ignores active count
             * writes. No hidden copy of the block count is updated. */
            if ((touched & UINT32_C(0xffff0000)) &&
                !o62m_sdhci_count_result(p, o62m_v4_data_count_write_v1(
                    &p->data, false, full >> 16, &ignored))) {
                return false;
            }
            return true;
        } else {
            O62MV4DataBankV1 bank = p->data.bank;
            if ((full & BLOCK_SIZE_MASK) > s->buf_maxsz) {
                return o62m_sdhci_fail(p, "block-size-exceeds-actual-buffer",
                                       0x200 + offset, size, true, value);
            }
            bank.bcct16 = full >> 16;
            if (!o62m_sdhci_program_bank(p, &bank)) {
                return false;
            }
            s->blksize = full & UINT32_C(0x7fff);
            return true;
        }
    case SDHC_ARGUMENT:
        if (p->command_pending) {
            return o62m_sdhci_fail(p, "argument-write-during-pending-command",
                                   0x200 + offset, size, true, value);
        }
        s->argument = o62m_sdhci_merge32(s->argument, offset, size, value);
        return true;
    case SDHC_TRNMOD: {
        uint16_t trnmod, command;
        old = s->trnmod | ((uint32_t)s->cmdreg << 16);
        full = o62m_sdhci_merge32(old, offset, size, value);
        trnmod = full & SDHC_TRNMOD_MASK;
        command = full >> 16;
        if ((full & SDHC_TRNS_ACMD23) || (command & UINT16_C(0xc000)) ||
            ((SDHC_COMMAND_TYPE(command) != 0) &&
             SDHC_COMMAND_TYPE(command) != SDHC_CMD_ABORT)) {
            return o62m_sdhci_fail(p, "unimplemented-transfer-or-command-mode",
                                   0x200 + offset, size, true, value);
        }
        if (p->command_pending || (active && (touched & UINT32_C(0xffff)) &&
                                   trnmod != s->trnmod)) {
            return o62m_sdhci_fail(p, "concurrent-transfer-register-change",
                                   0x200 + offset, size, true, value);
        }
        if (!(touched & UINT32_C(0xff000000))) {
            s->trnmod = trnmod;
            s->cmdreg = command;
            return true;
        }
        if (p->card_held || !p->card_bound ||
            (active && ((command & SDHC_CMD_DATA_PRESENT) ||
                        ((command >> 8) != 12 && (command >> 8) != 13)))) {
            return o62m_sdhci_fail(p, "command-submit-with-held-or-busy-card",
                                   0x200 + offset, size, true, value);
        }
        if (p->observation.command_submissions == UINT64_MAX) {
            return o62m_sdhci_fail(p, "command-submission-counter-overflow",
                                   0x200 + offset, size, true, value);
        }
        p->observation.command_submissions++;
        s->trnmod = trnmod;
        s->cmdreg = command;
        s->prnsts |= SDHC_CMD_INHIBIT;
        p->command_pending = true;
        return o62m_sdhci_clock_decode(p) && o62m_sdhci_schedule_work(p);
    }
    case SDHC_BDATA:
        if ((p->data.block_offset & 3) != (offset & 3)) {
            return o62m_sdhci_fail(p, "nonsequential-pio-write",
                                   0x200 + offset, size, true, value);
        }
        return o62m_sdhci_pio_write(p, size, value);
    case SDHC_HOSTCTL:
        old = s->hostctl1 | ((uint32_t)s->pwrcon << 8) |
              ((uint32_t)s->blkgap << 16) | ((uint32_t)s->wakcon << 24);
        full = o62m_sdhci_merge32(old, offset, size, value);
        if (((full >> 16) & 0xff) != s->blkgap ||
            (full & (SDHC_CTRL_CDTEST_INS | SDHC_CTRL_CDTEST_EN)) ||
            (active && ((full ^ old) & UINT32_C(0xffff)))) {
            return o62m_sdhci_fail(p, "unimplemented-block-gap-test-or-active-mode-change",
                                   0x200 + offset, size, true, value);
        }
        s->hostctl1 = full;
        s->pwrcon = full >> 8;
        s->wakcon = full >> 24;
        /* Actual card presence and the declared voltage capability determine
         * whether the standard power-enable request can be held. */
        if (!o62m_sdhci_power_available(p)) {
            s->pwrcon &= ~SDHC_POWER_ON;
        }
        sdhci_update_irq(s); /* WAKCON may expose an already pending card event. */
        return o62m_sdhci_clock_decode(p) && o62m_sdhci_schedule_work(p);
    case SDHC_CLKCON: {
        O62MHrsV1 trial = p->hrs;
        uint8_t reset;
        uint64_t request = 1;
        O62MHrsResultV1 result;
        old = s->clkcon | ((uint32_t)s->timeoutcon << 16);
        full = o62m_sdhci_merge32(old, offset, size, value);
        reset = (touched & UINT32_C(0xff000000)) ? full >> 24 : 0;
        if (reset && reset != SDHC_RESET_ALL) {
            return o62m_sdhci_fail(p, "partial-SRS-reset-not-yet-implemented",
                                   0x200 + offset, size, true, value);
        }
        if (reset) {
            result = o62m_hrs_access_v1(&trial,
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), 0, 4, true, &request);
            if (result != O62M_HRS_OK_V1) {
                return o62m_sdhci_fail(p, o62m_hrs_reason_v1(result),
                                       0x200 + offset, size, true, value);
            }
        }
        s->clkcon = full & ~SDHC_CLOCK_INT_STABLE;
        s->timeoutcon = full >> 16;
        if (reset) {
            p->hrs = trial;
        }
        return o62m_sdhci_clock_decode(p) && o62m_sdhci_arm_reset(p) &&
               o62m_sdhci_schedule_work(p);
    }
    case SDHC_ACMD12ERRSTS: {
        uint16_t host;
        old = s->acmd12errsts | ((uint32_t)s->hostctl2 << 16);
        full = o62m_sdhci_merge32(old, offset, size, value);
        host = full >> 16;
        if ((host & ((1u << 13) | R_SDHC_HOSTCTL2_EXECUTE_TUNING_MASK)) ||
            (active && host != s->hostctl2)) {
            return o62m_sdhci_fail(p, "unimplemented-64bit-tuning-or-active-hostctl2-change",
                                   0x200 + offset, size, true, value);
        }
        /* This profile has no proven write-clear protocol for low16 AutoCMD
         * errors. Preserve same-value/no-op writes; other changes stay strict.
         * The actual HostControl2 halfword is independently writable. */
        if (((uint16_t)full ^ s->acmd12errsts) & (uint16_t)touched) {
            return o62m_sdhci_fail(p, "unimplemented-auto-cmd-error-write-protocol",
                                   0x200 + offset, size, true, value);
        }
        if ((host ^ s->hostctl2) & R_SDHC_HOSTCTL2_V18_ENA_MASK) {
            sdbus_set_voltage(&s->sdbus, (host & R_SDHC_HOSTCTL2_V18_ENA_MASK) ?
                              SD_VOLTAGE_1_8V : SD_VOLTAGE_3_3V);
        }
        s->hostctl2 = host;
        return true;
    }
    case SDHC_NORINTSTS:
    case SDHC_NORINTSTSEN:
    case SDHC_NORINTSIGEN:
    case SDHC_ADMAERR:
        /* These exact handlers have no count/address/data or command side
         * effects. The new profile disables their old eager transfer timer. */
        sdhci_write(s, offset, value, size);
        return true;
    default:
        return o62m_sdhci_fail(p, "unknown-or-readonly-standard-register-write",
                               0x200 + offset, size, true, value);
    }
}

bool o62m_sdhci_configure(DeviceState *dev, const O62MSdhciConfig *config,
                          Error **errp)
{
    O62MSDHCIState *p;
    O62MHrsV1 hrs;
    O62MV4DataV1 data;
    O62MPhyV1 phy;
    uint32_t max_block;
    if (!dev || !config || !object_dynamic_cast(OBJECT(dev), TYPE_O62M_SDHCI)) {
        error_setg(errp, "O62M SDHCI configure requires the explicit new device type");
        return false;
    }
    p = O62M_SDHCI(dev);
    if (o62m_v4_data_overlap_v1(p, sizeof(*p), config, sizeof(*config)) ||
        p->configured || qdev_is_realized(dev) ||
        !config->explicit_research_profile || !config->clock ||
        (config->embedded_fixed_supply_enable ?
            !o62m_sdhci_fixed_supply_valid(config) :
            config->embedded_fixed_supply_mv != 0) ||
        (!config->hrs06_timing_enable && config->hrs06_initial_fullword) ||
        !config->ram_range || !config->fault ||
        !config->hrs.explicit_enable || !config->hrs.instance_id ||
        !config->hrs.virtual_card_auto_boot_off || config->hrs.media_bootcfg ||
        config->card_reset_assert_level > 1 || !config->work_latency_ns ||
        config->work_latency_ns > INT64_MAX ||
        !config->bytes_per_tick || config->bytes_per_tick > (1u << 20) ||
        !config->descriptors_per_tick || config->descriptors_per_tick > 4096 ||
        !config->total_descriptor_limit ||
        config->terminal_boundary_policy > O62M_SDHCI_TERMINAL_BOUNDARY_NO_IRQ ||
        (config->standard_version_fullword & 0xff) < 3 ||
        (config->standard_version_fullword & 0xff) > 5 ||
        FIELD_EX64(config->capabilities_fullword, SDHC_CAPAB, MAXBLOCKLENGTH) >= 3) {
        error_setg(errp, "O62M SDHCI explicit geometry, policies or callbacks invalid");
        return false;
    }
    max_block = 1u << (9 + FIELD_EX64(config->capabilities_fullword,
                                     SDHC_CAPAB, MAXBLOCKLENGTH));
    if (o62m_hrs_init_v1(&hrs, &config->hrs) != O62M_HRS_OK_V1 ||
        o62m_v4_data_init_v1(&data, &config->initial_data_bank, max_block,
                             false, config->hrs.instance_id) != O62M_V4_DATA_OK_V1) {
        error_setg(errp, "O62M SDHCI pure initial state rejected");
        return false;
    }
    if (config->phy.explicit_enable &&
        o62m_phy_init_v1(&phy, &config->phy) != O62M_PHY_OK_V1) {
        error_setg(errp, "O62M SDHCI explicit functional PHY profile invalid");
        return false;
    }
    p->config = *config;
    p->hrs = hrs;
    if (config->phy.explicit_enable) {
        p->phy = phy;
    }
    p->hrs06_fullword = config->hrs06_initial_fullword;
    /* The real canonical object, not the temporary validation copy, owns
     * every later pure plan token and survives for the whole device life. */
    assert(o62m_v4_data_init_v1(&p->data, &config->initial_data_bank, max_block,
                                false, config->hrs.instance_id) == O62M_V4_DATA_OK_V1);
    p->parent_obj.sd_spec_version = 4;
    p->parent_obj.capareg = config->capabilities_fullword;
    p->parent_obj.maxcurr = config->max_current_fullword;
    p->configured = true;
    return true;
}

bool o62m_sdhci_bind_card(DeviceState *dev, DeviceState *card, Error **errp)
{
    O62MSDHCIState *p;
    O62MSdObserverSnapshotV1 state;
    O62MSdObserverResultV1 result;
    if (!dev || !card || !object_dynamic_cast(OBJECT(dev), TYPE_O62M_SDHCI) ||
        !object_dynamic_cast(OBJECT(card), TYPE_EMMC)) {
        error_setg(errp, "O62M SDHCI bind requires its actual realized eMMC");
        return false;
    }
    p = O62M_SDHCI(dev);
    if (!p->realized || p->card_bound || !qdev_is_realized(card) ||
        qdev_get_parent_bus(card) != BUS(&p->parent_obj.sdbus)) {
        error_setg(errp, "O62M SDHCI card owner, bus or lifecycle invalid");
        return false;
    }
    result = o62m_sd_observer_enable_v1(card, p->config.hrs.instance_id);
    if (result != O62M_SD_OBSERVER_OK_V1) {
        error_setg(errp, "O62M SDHCI actual card observer rejected enable: %s",
                   o62m_sd_observer_reason_v1(result));
        return false;
    }
    p->card = card;
    p->card_bound = true;
    if (!o62m_sdhci_card_snapshot(p, &state) || !state.inserted ||
        state.active_part_config || state.reset_default_part_config) {
        error_setg(errp, "O62M SDHCI actual fresh eMMC boot configuration unsupported");
        return false;
    }
    if (p->config.hrs.card_initial_applied_level ==
        p->config.card_reset_assert_level) {
        if (!o62m_sdhci_card_reset_actual(p)) {
            error_setg(errp, "O62M SDHCI actual initial card reset failed");
            return false;
        }
    } else {
        p->card_held = false;
    }
    sdhci_set_inserted(dev, state.inserted);
    sdhci_set_readonly(dev, state.readonly);
    return o62m_sdhci_clock_changed(dev, errp);
}

bool o62m_sdhci_clock_changed(DeviceState *dev, Error **errp)
{
    O62MSDHCIState *p;
    if (!dev || !object_dynamic_cast(OBJECT(dev), TYPE_O62M_SDHCI)) {
        error_setg(errp, "O62M SDHCI clock hook requires its explicit device");
        return false;
    }
    p = O62M_SDHCI(dev);
    if (!p->realized || !o62m_sdhci_clock_decode(p) ||
        !o62m_sdhci_arm_phy(p) || !o62m_sdhci_arm_reset(p) ||
        !o62m_sdhci_schedule_work(p)) {
        error_setg(errp, "O62M SDHCI functional clock/state unavailable");
        return false;
    }
    return true;
}

bool o62m_sdhci_observe(DeviceState *dev, O62MSdhciObservation *out,
                        Error **errp)
{
    O62MSDHCIState *p;
    O62MSdhciObservation result;
    if (!dev || !out || !object_dynamic_cast(OBJECT(dev), TYPE_O62M_SDHCI)) {
        error_setg(errp, "O62M SDHCI observation requires valid device/output");
        return false;
    }
    p = O62M_SDHCI(dev);
    if (o62m_v4_data_overlap_v1(p, sizeof(*p), out, sizeof(*out)) ||
        !p->configured || !o62m_v4_data_valid_v1(&p->data)) {
        error_setg(errp, "O62M SDHCI observation state/output invalid");
        return false;
    }
    if (p->card_bound) {
        O62MSdObserverSnapshotV1 card_mode;
        O62MSdObserverResultV1 card_result =
            o62m_sd_observer_snapshot_v1(p->card, &card_mode);
        if (card_result != O62M_SD_OBSERVER_OK_V1 || card_mode.counter_overflow ||
            card_mode.session != p->config.hrs.instance_id) {
            error_setg(errp, "O62M SDHCI finite actual card-mode snapshot unavailable");
            return false;
        }
        result = p->observation;
        result.card_state = card_mode.card_state;
        result.card_cmd21_tuning_enabled = card_mode.cmd21_tuning_enabled;
        result.card_in_transfer_state = card_mode.in_transfer_state;
        result.card_bus_width_config = card_mode.bus_width_config;
        result.card_hs_timing = card_mode.hs_timing;
        result.card_ext_csd_revision = card_mode.ext_csd_revision;
        result.card_type = card_mode.card_type;
    } else {
        result = p->observation;
    }
    result.data_bank = p->data.bank;
    result.data_phase = p->data.phase;
    result.block_offset = p->data.block_offset;
    result.phy_enabled = p->config.phy.explicit_enable;
    result.phy_ready = result.phy_enabled && o62m_phy_ready_v1(&p->phy);
    if (result.phy_enabled) {
        result.phy_jobs_started = p->phy.jobs_started;
        result.phy_jobs_completed = p->phy.jobs_completed;
        result.phy_jobs_cancelled = p->phy.jobs_cancelled;
        result.phy_parameter_writes = p->phy.parameter_writes;
        result.phy_reset_assertions = p->phy.held_assertions;
    }
    /* Snapshot the same actual state the worker tests. No read-side clock
     * decode, timer progress, submission, state clearing or IRQ is performed. */
    result.command_index = (p->parent_obj.cmdreg >> 8) & 63;
    result.data_present = (p->parent_obj.cmdreg & SDHC_CMD_DATA_PRESENT) != 0;
    result.transfer_read = (p->parent_obj.trnmod & SDHC_TRNS_READ) != 0;
    result.transfer_multi = (p->parent_obj.trnmod & SDHC_TRNS_MULTI) != 0;
    result.transfer_bce = (p->parent_obj.trnmod & SDHC_TRNS_BLK_CNT_EN) != 0;
    result.transfer_dma = (p->parent_obj.trnmod & SDHC_TRNS_DMA) != 0;
    result.host_v4_enabled = FIELD_EX32(p->parent_obj.hostctl2,
                                       SDHC_HOSTCTL2, VERSION4) != 0;
    result.transfer_block_size = p->parent_obj.blksize & BLOCK_SIZE_MASK;
    result.legacy_block_count = p->data.bank.bcct16;
    result.v4_block_count = 0;
    result.canonical_block_count = 0;
    result.count_source = O62M_SDHCI_OBS_COUNT_IGNORED;
    if (result.transfer_bce) {
        if (result.host_v4_enabled) {
            result.count_source = O62M_SDHCI_OBS_COUNT_V4_SAAR32;
            result.v4_block_count = p->data.bank.saar;
            result.canonical_block_count = result.v4_block_count;
        } else {
            result.count_source = O62M_SDHCI_OBS_COUNT_LEGACY16;
            result.canonical_block_count = result.legacy_block_count;
        }
    }
    result.command_pending = p->command_pending;
    result.command_inhibit = (p->parent_obj.prnsts & SDHC_CMD_INHIBIT) != 0;
    result.hrs_phase = p->hrs.phase;
    result.internal_clock_enabled = (p->parent_obj.clkcon & SDHC_CLOCK_INT_EN) != 0;
    result.internal_clock_stable = (p->parent_obj.clkcon & SDHC_CLOCK_INT_STABLE) != 0;
    result.sd_clock_enabled = (p->parent_obj.clkcon & SDHC_CLOCK_SDCLK_EN) != 0;
    result.power_on = (p->parent_obj.pwrcon & SDHC_POWER_ON) != 0;
    result.power_voltage_code = (p->parent_obj.pwrcon >> 1) & 7;
    result.embedded_fixed_supply_enabled = p->config.embedded_fixed_supply_enable;
    result.embedded_fixed_supply_mv = p->config.embedded_fixed_supply_mv;
    result.embedded_fixed_supply_active = result.power_on &&
        result.power_voltage_code == 0 && o62m_sdhci_power_available(p);
    result.functional = o62m_sdhci_functional(p);
    result.blocked_conditions = 0;
    if (!p->card_bound) { result.blocked_conditions |= O62M_SDHCI_BLOCK_NO_CARD; }
    if (p->card_held) { result.blocked_conditions |= O62M_SDHCI_BLOCK_CARD_HELD; }
    if (p->hrs.phase != O62M_HRS_IDLE_V1) {
        result.blocked_conditions |= O62M_SDHCI_BLOCK_HOST_RESET;
    }
    if (result.phy_enabled && !result.phy_ready) {
        result.blocked_conditions |= O62M_SDHCI_BLOCK_PHY_NOT_READY;
    }
    if (!p->clock_available) { result.blocked_conditions |= O62M_SDHCI_BLOCK_CLOCK_OFF; }
    if (!result.internal_clock_enabled) {
        result.blocked_conditions |= O62M_SDHCI_BLOCK_INTCLK_OFF;
    }
    if (!result.internal_clock_stable) {
        result.blocked_conditions |= O62M_SDHCI_BLOCK_INTCLK_UNSTABLE;
    }
    if (!result.sd_clock_enabled) { result.blocked_conditions |= O62M_SDHCI_BLOCK_SDCLK_OFF; }
    if (!o62m_sdhci_power_available(p)) {
        result.blocked_conditions |= O62M_SDHCI_BLOCK_POWER_OFF;
    }
    result.card_bound = p->card_bound;
    result.card_held = p->card_held;
    result.clock_available = p->clock_available;
    result.terminal_fault = p->terminal_fault;
    *out = result;
    return true;
}

static void o62m_sdhci_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    O62MSDHCIState *p = O62M_SDHCI(dev);
    if (!p->configured || p->parent_obj.sd_spec_version != 4 ||
        p->parent_obj.capareg != p->config.capabilities_fullword ||
        p->parent_obj.maxcurr != p->config.max_current_fullword ||
        p->parent_obj.endianness != DEVICE_LITTLE_ENDIAN ||
        p->parent_obj.vendor != SDHCI_VENDOR_NONE ||
        p->parent_obj.pending_insert_quirk) {
        error_setg(errp, "O62M SDHCI configure or conflicting legacy properties invalid");
        return;
    }
    sdhci_sysbus_realize(dev, errp);
    if (*errp) {
        return;
    }
    p->parent_obj.version = p->config.standard_version_fullword;
    memory_region_init_io(&p->outer, OBJECT(p), &o62m_sdhci_outer_ops, p,
                           "o62m-cadence-research", O62M_SDHCI_OUTER_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &p->outer);
    p->realized = true;
}

static void o62m_sdhci_unrealize(DeviceState *dev)
{
    O62MSDHCIState *p = O62M_SDHCI(dev);
    timer_del(p->reset_timer);
    timer_del(p->phy_timer);
    timer_del(p->parent_obj.transfer_timer);
    p->realized = false;
    sdhci_sysbus_unrealize(dev);
}

static void o62m_sdhci_instance_init(Object *obj)
{
    O62MSDHCIState *p = O62M_SDHCI(obj);
    p->parent_obj.o62m_profile = p;
    timer_free(p->parent_obj.transfer_timer);
    p->parent_obj.transfer_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                                o62m_sdhci_transfer_step, p);
    p->reset_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, o62m_sdhci_reset_step, p);
    p->phy_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, o62m_sdhci_phy_step, p);
}

static void o62m_sdhci_instance_finalize(Object *obj)
{
    O62MSDHCIState *p = O62M_SDHCI(obj);
    timer_free(p->reset_timer);
    timer_free(p->phy_timer);
    p->reset_timer = NULL;
    p->phy_timer = NULL;
}

static void o62m_sdhci_machine_reset(DeviceState *dev)
{
    O62MSDHCIState *p = O62M_SDHCI(dev);
    O62MHrsSpecV1 spec;
    O62MHrsV1 hrs;
    O62MV4DataV1 data;
    O62MPhySpecV1 phy_spec;
    O62MPhyV1 phy;
    uint64_t epoch;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!p->configured) {
        sdhci_poweron_reset(dev);
        return;
    }
    if (p->terminal_fault) {
        return; /* No implicit recovery of unknown external partial effects. */
    }
    if (p->session_generation == UINT64_MAX ||
        p->config.hrs.instance_id > UINT64_MAX - p->session_generation - 1 ||
        !p->hrs.card_level_known || now < 0 ||
        p->observation.controller_resets == UINT64_MAX) {
        o62m_sdhci_fail(p, "whole-controller-reset-lifecycle-unavailable",
                         0, 0, false, 0);
        return;
    }
    epoch = p->config.hrs.instance_id + p->session_generation + 1;
    spec = p->config.hrs;
    spec.instance_id = epoch;
    spec.now_ns = now;
    /* A QOM controller reset cancels its pending requests but does not move
     * the independently owned card wire or cold-reset the actual card. */
    spec.hrs11_initial_fullword = p->hrs.hrs11_opaque |
                                 p->hrs.applied_card_level;
    spec.card_initial_applied_level = p->hrs.applied_card_level;
    if (o62m_hrs_init_v1(&hrs, &spec) != O62M_HRS_OK_V1 ||
        o62m_v4_data_init_v1(&data, &p->config.initial_data_bank,
                             p->data.max_block_size, false, epoch) !=
                                O62M_V4_DATA_OK_V1) {
        o62m_sdhci_fail(p, "whole-controller-reset-preflight-rejected",
                         0, 0, false, 0);
        return;
    }
    if (p->config.phy.explicit_enable) {
        if (p->config.phy.instance_id > UINT64_MAX - p->session_generation - 1) {
            o62m_sdhci_fail(p, "phy-reset-epoch-overflow", 0x24, 4, false, 0);
            return;
        }
        phy_spec = p->config.phy;
        phy_spec.now_ns = now;
        phy_spec.instance_id += p->session_generation + 1;
        if (o62m_phy_init_v1(&phy, &phy_spec) != O62M_PHY_OK_V1) {
            o62m_sdhci_fail(p, "whole-phy-reset-preflight-rejected", 0x24, 4, false, 0);
            return;
        }
    }
    timer_del(p->reset_timer);
    timer_del(p->phy_timer);
    sdhci_poweron_reset(dev); /* Actual standard controller backend reset. */
    if (p->config.phy.explicit_enable) {
        p->phy = phy;
    }
    p->hrs = hrs;
    /* Explicit virtual QOM-reset policy; SRS ResetAll never resets this
     * independent host-configuration view or the actual card. */
    p->hrs06_fullword = p->config.hrs06_initial_fullword;
    assert(o62m_v4_data_init_v1(&p->data, &p->config.initial_data_bank,
                                 p->data.max_block_size, false, epoch) ==
                                   O62M_V4_DATA_OK_V1);
    p->command_pending = false;
    p->descriptor_active = false;
    p->descriptor_remaining = 0;
    p->descriptor_total = 0;
    p->session_generation++;
    p->observation.controller_resets++;
    sdhci_update_irq(&p->parent_obj);
}

static const VMStateDescription o62m_sdhci_unmigratable = {
    .name = TYPE_O62M_SDHCI,
    .unmigratable = true,
};

static void o62m_sdhci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    (void)data;
    dc->realize = o62m_sdhci_realize;
    dc->unrealize = o62m_sdhci_unrealize;
    dc->vmsd = &o62m_sdhci_unmigratable;
    device_class_set_legacy_reset(dc, o62m_sdhci_machine_reset);
}

#endif
