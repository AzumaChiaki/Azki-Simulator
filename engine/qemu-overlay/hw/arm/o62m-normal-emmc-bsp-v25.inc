/* Explicit normal-AP volatile eMMC integration. Include after Marconi state
 * and marconi_missing(), once only. Physical reset and timing are unknown.
 * SPDX-License-Identifier: GPL-2.0-only */

static bool marconi_normal_emmc_clock(void *opaque, bool *available)
{
    MarconiProbeState *s = opaque;
    return o62m_emmc_functional_clock_query_v1(&s->clock_controls, &s->pll_rate,
        O62M_EMMC_VIRTUAL_SOURCES_DECLARED_V1, available);
}

static bool marconi_normal_emmc_ram(void *opaque, uint64_t address,
                                    uint32_t bytes, bool write_to_ram)
{
    MarconiProbeState *s = opaque;
    const uint64_t end = UINT64_C(0x638f4000);
    const bool ap_span = address >= MARCONI_AP_BASE && address < end &&
                         bytes <= end - address;
    const bool ipc_nv_span = s->ap_uncache_ram_created &&
        address >= UINT64_C(0x64d70000) && address < UINT64_C(0x64de0000) &&
        bytes <= UINT64_C(0x64de0000) - address;
    const bool ims_nv_span = s->ap_uncache_ram_created &&
        address >= UINT64_C(0x64de0000) && address < UINT64_C(0x64df0000) &&
        bytes <= UINT64_C(0x64df0000) - address;
    const bool tee_ree_sm_span = address >= UINT64_C(0x63df8000) &&
        address < UINT64_C(0x63e00000) &&
        bytes <= UINT64_C(0x63e00000) - address;
    const bool cp_image_span = s->cp_image_ram_created &&
        address >= UINT64_C(0x64000000) && address < UINT64_C(0x64d00000) &&
        bytes <= UINT64_C(0x64d00000) - address;
    const bool cp_boot_span = s->cp_image_ram_created &&
        address >= UINT64_C(0x22000000) && address < UINT64_C(0x22000400) &&
        bytes <= UINT64_C(0x22000400) - address;
    const bool cp_crf_span = s->cp_image_ram_created &&
        address >= UINT64_C(0x64df0000) && address < UINT64_C(0x64e00000) &&
        bytes <= UINT64_C(0x64e00000) - address;
    if (!s->tee_ap_image || !bytes ||
        (!ap_span && !ipc_nv_span && !ims_nv_span && !tee_ree_sm_span &&
         !cp_image_span && !cp_boot_span && !cp_crf_span)) {
        return false;
    }
    MemoryRegionSection section = memory_region_find(get_system_memory(),
                                                      address, bytes);
    bool valid = section.mr && memory_region_is_ram(section.mr) &&
                 int128_ge(section.size, int128_make64(bytes)) &&
                 (!write_to_ram || !memory_region_is_rom(section.mr)) &&
                 (!ipc_nv_span || section.mr == &s->ipc_nv_ram) &&
                 (!ims_nv_span || section.mr == &s->ims_nv_ram) &&
                 (!tee_ree_sm_span || section.mr == &s->tee_ree_ram) &&
                 (!cp_image_span || section.mr == &s->cp_image_ram) &&
                 (!cp_boot_span || section.mr == &s->cp_boot_ram) &&
                 (!cp_crf_span || section.mr == &s->cp_crf_ram);
    if (valid && ipc_nv_span) {
        marconi_public_report("MARCONI_NORMAL_EMMC_IPC_NV_DMA "
            "exact-original-mmap-span-and-unique-RAM-owner=true "
            "full-transfer-covered=true direction-write-to-RAM=%d "
            "address-size-and-payload-redacted=true", write_to_ram);
    }
    if (valid && ims_nv_span) {
        marconi_public_report("MARCONI_NORMAL_EMMC_IMS_NV_DMA "
            "exact-original-mmap-span-and-unique-RAM-owner=true "
            "full-transfer-covered=true direction-write-to-RAM=%d "
            "address-size-and-payload-redacted=true", write_to_ram);
    }
    if (valid && tee_ree_sm_span) {
        marconi_public_report("MARCONI_NORMAL_EMMC_TEE_REE_SM_DMA "
            "exact-original-mmap-span-and-unique-RAM-owner=true "
            "full-transfer-covered=true direction-write-to-RAM=%d "
            "address-size-and-payload-redacted=true", write_to_ram);
    }
    if (section.mr) {
        memory_region_unref(section.mr);
    }
    return valid;
}

static void marconi_normal_emmc_fault(void *opaque, const char *reason,
                                       uint64_t offset, unsigned size,
                                       bool write, uint64_t value)
{
    MarconiProbeState *s = opaque;
    /* reason is an adapter-owned fixed diagnostic token, never guest bytes. */
    marconi_public_report("MARCONI_NORMAL_EMMC_BOUNDARY reason=%s "
        "address-value-and-data-redacted=true no-boot-acceptance=true", reason);
    if(s->normal_emmc_bound&&!s->reported) {
        O62MSdhciObservation state;
        Error *error = NULL;
        if(o62m_sdhci_observe(s->normal_emmc_device,&state,&error)) {
            marconi_public_report("MARCONI_NORMAL_EMMC_FAULT_GATE "
                "submissions=%" PRIu64 " pending=%u inhibit=%u functional=%u "
                "blocked-conditions=%u hrs-phase=%u card-bound=%u card-held=%u "
                "reference-clock=%u internal-clock-enable=%u internal-clock-stable=%u "
                "SD-clock-enable=%u power-on=%u voltage-code=%u "
                "pre-cleanup-snapshot=true finite-configuration-only=true",
                state.command_submissions,state.command_pending,state.command_inhibit,
                state.functional,state.blocked_conditions,state.hrs_phase,
                state.card_bound,state.card_held,state.clock_available,
                state.internal_clock_enabled,state.internal_clock_stable,
                state.sd_clock_enabled,state.power_on,state.power_voltage_code);
            marconi_public_report("MARCONI_NORMAL_EMMC_FAULT_DATA_CONTRACT "
                "command-index=%u data-present=%u read=%u multi=%u BCE=%u DMA=%u "
                "host-v4=%u block-size=%u count-source=%u legacy-count=%u "
                "v4-count=%u canonical-count=%u pre-cleanup-snapshot=true "
                "no-command-argument-DMA-address-descriptor-or-payload-output=true",
                state.command_index,state.data_present,state.transfer_read,
                state.transfer_multi,state.transfer_bce,state.transfer_dma,
                state.host_v4_enabled,state.transfer_block_size,state.count_source,
                state.legacy_block_count,state.v4_block_count,state.canonical_block_count);
            marconi_public_report("MARCONI_NORMAL_EMMC_FAULT_CARD_MODE "
                "profile-enabled=%u transfer-state=%u state=%d bus-width-config=%u "
                "hs-timing=%u ext-csd-revision=%u card-type=%u "
                "pre-cleanup-snapshot=true public-mode-fields-only=true",
                state.card_cmd21_tuning_enabled,state.card_in_transfer_state,
                (int)state.card_state,state.card_bus_width_config,
                state.card_hs_timing,state.card_ext_csd_revision,state.card_type);
        }
        error_free(error);
    }
    (void)marconi_missing(s, UINT64_C(0x40100000) + offset, size, write, value);
}

static void marconi_normal_emmc_irq(void *opaque, int number, int level)
{
    MarconiProbeState *s = opaque;
    bool asserted = level != 0;
    (void)number;
    if (asserted != s->normal_emmc_irq_level) {
        uint64_t *count = asserted ? &s->normal_emmc_irq_asserts :
                                     &s->normal_emmc_irq_deasserts;
        if (*count == UINT64_MAX) {
            marconi_normal_emmc_fault(s, "IRQ-edge-count-overflow", 0, 0,
                                       false, 0);
            return;
        }
        ++*count;
        s->normal_emmc_irq_level = asserted;
        if(s->normal_emmc_irq_asserts+s->normal_emmc_irq_deasserts<=64)marconi_public_report("MARCONI_NORMAL_EMMC_IRQ level=%u "
            "asserts=%" PRIu64 " deasserts=%" PRIu64 " external=21 "
            "actual-controller-status-and-enable-source=true no-forced-CPU-state=true",
            asserted, s->normal_emmc_irq_asserts, s->normal_emmc_irq_deasserts);
    }
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->armv7m), 21), asserted);
}

static bool marconi_normal_emmc_clock_notify(MarconiProbeState *s,
                                               uint64_t address)
{
    if (!s->normal_emmc_bound) {
        return true;
    }
    uint64_t canonical = o62m_clock_control_canonical(address);
    if (canonical != UINT64_C(0x4006001c) &&
        canonical != UINT64_C(0x400600b4) &&
        canonical != UINT64_C(0x400600b8) &&
        canonical != UINT64_C(0x400600bc)) {
        return true;
    }
    Error *error = NULL;
    bool ok = o62m_sdhci_clock_changed(s->normal_emmc_device, &error);
    error_free(error);
    return ok;
}

static void marconi_normal_emmc_exit(Notifier *notify, void *opaque)
{
    MarconiProbeState *s = container_of(notify, MarconiProbeState,
                                       normal_emmc_exit);
    O62MSdhciObservation controller;
    Error *error = NULL;
    (void)opaque;
    marconi_public_report("MARCONI_NORMAL_EMMC_IRQ_TOTALS asserts=%" PRIu64 " deasserts=%" PRIu64, s->normal_emmc_irq_asserts,s->normal_emmc_irq_deasserts);
    if (s->normal_emmc_bound &&
        o62m_sdhci_observe(s->normal_emmc_device, &controller, &error)) {
        marconi_public_report("MARCONI_NORMAL_EMMC_WORK_COUNTS reads=%" PRIu64
            " writes=%" PRIu64 " commands=%" PRIu64 " command-failures=%" PRIu64
            " card-read-bytes=%" PRIu64 " card-write-bytes=%" PRIu64
            " RAM-read-bytes=%" PRIu64 " RAM-write-bytes=%" PRIu64
            " descriptors=%" PRIu64 " completed-data-jobs=%" PRIu64
            " controller-resets=%" PRIu64 " card-assertions=%" PRIu64
            " card-deassertions=%" PRIu64 " actual-work-only=true no-payload-output=true",
            controller.mmio_reads, controller.mmio_writes, controller.commands,
            controller.command_failures, controller.card_bytes_read,
            controller.card_bytes_written, controller.ram_bytes_read,
            controller.ram_bytes_written, controller.descriptor_fetches,
            controller.completed_data_jobs, controller.controller_resets,
            controller.card_assertions, controller.card_deassertions);
        marconi_public_report("MARCONI_NORMAL_EMMC_PHY_COUNTS enabled=%u ready=%u "
            "jobs-started=%" PRIu64 " jobs-completed=%" PRIu64
            " jobs-cancelled=%" PRIu64 " parameter-writes=%" PRIu64
            " reset-assertion-edges=%" PRIu64
            " actual-captured-parameter-apply=true no-analog-or-physical-reset-claim=true",
            controller.phy_enabled, controller.phy_ready,
            controller.phy_jobs_started, controller.phy_jobs_completed,
            controller.phy_jobs_cancelled, controller.phy_parameter_writes,
            controller.phy_reset_assertions);
        marconi_public_report("MARCONI_NORMAL_EMMC_GATE_COUNTS "
            "submissions=%" PRIu64 " pending=%u inhibit=%u functional=%u "
            "blocked-conditions=%u hrs-phase=%u card-bound=%u card-held=%u "
            "reference-clock=%u internal-clock-enable=%u internal-clock-stable=%u "
            "SD-clock-enable=%u power-on=%u voltage-code=%u "
            "actual-canonical-state-only=true no-argument-DMA-or-payload-output=true",
            controller.command_submissions,controller.command_pending,
            controller.command_inhibit,controller.functional,
            controller.blocked_conditions,controller.hrs_phase,
            controller.card_bound,controller.card_held,controller.clock_available,
            controller.internal_clock_enabled,controller.internal_clock_stable,
            controller.sd_clock_enabled,controller.power_on,
            controller.power_voltage_code);
    }
    error_free(error);
    error = NULL;
    bdrv_graph_rdlock_main_loop();
    bool detached = blk_bs(s->normal_emmc_backend) == NULL;
    bdrv_graph_rdunlock_main_loop();
    bool wiped;
    if (detached) {
        /* qemu_cleanup stops producers and drains/closes every BlockBackend
         * before these atexit callbacks. This private backend's fixed close
         * destroys its hash table using volatile page wipes. Do not pretend
         * a removed BlockDriverState was observed as a live empty store. */
        wiped = true;
        marconi_public_report("MARCONI_NORMAL_EMMC_EXIT_WIPE "
            "private-RAM-medium-zeroized=true backend-detached=true "
            "provenance=fixed-private-driver-close-volatile-page-destroy "
            "live-empty-observation-claimed=false no-payload-or-hash-output=true");
    } else {
        /* Bound model-owned worker failures request normal shutdown so
         * qemu_cleanup drains/closes the private backend before atexit.
         * A live backend is only wiped if VM producers are already stopped;
         * do not claim coverage of a running direct-exit/abort path. */
        O62MMemoryBlockObservationV1 medium;
        wiped = !runstate_is_running() &&
            o62m_memory_block_wipe_v1(s->normal_emmc_backend, &error) &&
            o62m_memory_block_observe_v1(s->normal_emmc_backend, &medium,
                                         &error) &&
            medium.terminal_wiped && !medium.allocated_pages;
        marconi_public_report("MARCONI_NORMAL_EMMC_EXIT_WIPE "
            "private-RAM-medium-zeroized=%s backend-detached=false "
            "provenance=terminal-wipe-and-live-empty-observation "
            "no-payload-or-hash-output=true", wiped ? "true" : "false");
    }
    error_free(error);
    assert(wiped);
}

static void marconi_normal_emmc_create(MarconiProbeState *s)
{
    Error *error = NULL;
    BlockBackend *backend = o62m_memory_block_create_v1("o62m-normal-volatile",
                                                       UINT64_C(1) << 32,
                                                       &error);
    if (!backend) {
        error_report_err(error);
        exit(EXIT_FAILURE);
    }
    FILE *source = g_fopen(s->normal_emmc_media, "rb");
    uint8_t *buffer = g_malloc(1 << 20);
    uint64_t offset = 0;
    bool ok = source != NULL;
    while (ok && offset < (UINT64_C(1) << 32)) {
        const size_t length = 1 << 20;
        ok = fread(buffer, 1, length, source) == length;
        if (!ok) {
            break;
        }
        /* The private medium already has real known zero pages when absent.
         * Do not allocate hundreds of MiB merely to rewrite known zeros. */
        bool nonzero = false;
        for (size_t i = 0; i < length; ++i) {
            if (buffer[i]) {
                nonzero = true;
                break;
            }
        }
        if (nonzero) {
            ok = o62m_memory_block_seed_v1(backend, offset, buffer, length,
                                           &error);
        }
        offset += length;
    }
    if (ok) {
        ok = fgetc(source) == EOF && !ferror(source);
    }
    if (source) {
        ok = fclose(source) == 0 && ok;
    }
    o62m_sha256_zeroize_v1(buffer, 1 << 20);
    g_free(buffer);
    ok = ok && o62m_memory_block_seal_v1(backend, &error);
    if(ok && s->normal_emmc_writeback)
        ok=o62m_memory_block_set_writeback_v1(backend,s->normal_emmc_writeback,&s->reported,&error);
    if (!ok) {
        error_free(error);
        blk_unref(backend); /* Fixed private driver close wipes copied pages. */
        error_report("normal-emmc-media requires a readable 4GiB instance image "
                     "with valid owned writeback path; private medium not published");
        exit(EXIT_FAILURE);
    }
    s->normal_emmc_backend = backend;
    s->normal_emmc_device = qdev_new(TYPE_O62M_SDHCI);
    O62MSdhciConfig config = {
        .explicit_research_profile = true,
        .capabilities_fullword = UINT64_C(0x456cc881),
        .max_current_fullword = 0,
        .standard_version_fullword = 5,
        .hrs = {
            .explicit_enable = true,
            .hrs00_initial_fullword = 0,
            .hrs01_initial_fullword = 0,
            .hrs01_reset_fullword = 0,
            .hrs11_initial_fullword = 0,
            .hrs31_initial_fullword = UINT64_C(0x06020001),
            .hrs36_initial_fullword = 0,
            .card_initial_applied_valid = true,
            .card_initial_applied_level = 0,
            .virtual_card_auto_boot_off = true,
            .media_bootcfg = 0,
            .controller_latency_ns = 500,
            .card_latency_ns = 500,
            .instance_id = UINT64_C(0x4e4f524d),
            .now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
        },
        /* Explicit idealized PHY initial backing and apply latency. No initial
         * Ready bit, no analog DLL claim, and CARD500ns is unchanged. */
        .phy = {
            .explicit_enable = true,
            .idealized_apply_enable = true,
            .retained_parameter_reuse_enable = true,
            .hrs04_initial_fullword = 0,
            .hrs07_initial_fullword = 0,
            .hrs09_initial_fullword = 0,
            .hrs10_initial_fullword = 0,
            .hrs16_initial_fullword = 0,
            .phy_initial = {0, 0, 0, 0, 0, 0},
            .init_latency_ns = 1000,
            .instance_id = UINT64_C(0x5048594e),
            .now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
        },
        .independent_cardline_host_config_enable = true,
        .hrs06_timing_enable = true,
        .hrs06_initial_fullword = 0,
        /* Optional explicit external VCC board input; not inferred PMIC state,
         * and separate from SDHCI HostControl2 signal-voltage/VCCQ requests. */
        .embedded_fixed_supply_enable = s->normal_emmc_fixed_supply != NULL,
        .embedded_fixed_supply_mv = s->normal_emmc_fixed_supply ?
            (!strcmp(s->normal_emmc_fixed_supply,"external-3v3-v1") ? 3300 : 1800) : 0,
        .card_reset_assert_level = 1,
        .dma_secure = false,
        .allow_v4_32_resume = true,
        .terminal_boundary_policy = O62M_SDHCI_TERMINAL_BOUNDARY_NO_IRQ,
        .work_latency_ns = 500,
        .bytes_per_tick = 512,
        .descriptors_per_tick = 8,
        .total_descriptor_limit = 4096,
        .clock = marconi_normal_emmc_clock,
        .ram_range = marconi_normal_emmc_ram,
        .fault = marconi_normal_emmc_fault,
        .opaque = s,
    };
    if (!o62m_sdhci_configure(s->normal_emmc_device, &config, &error)) {
        error_report_err(error);
        exit(EXIT_FAILURE);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->normal_emmc_device), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->normal_emmc_device),
                    O62M_SDHCI_OUTER_MMIO_INDEX, UINT64_C(0x40100000));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->normal_emmc_device), 0,
                       qemu_allocate_irq(marconi_normal_emmc_irq, s, 0));
    DeviceState *card = qdev_new(TYPE_EMMC);
    qdev_prop_set_uint8(card,"boot-config",0);
    qdev_prop_set_bit(card,"o62m-rpmb",true);
    if(s->normal_emmc_tuning_profile) {
        /* This is the actual card's sole endpoint-enable owner, set before
         * realization. No card revision/mode/width is pre-populated. */
        qdev_prop_set_bit(card,"o62m-cmd21-tuning",true);
    }
    qdev_prop_set_drive_err(card, "drive", backend, &error_fatal);
    qdev_realize_and_unref(card,
                           qdev_get_child_bus(s->normal_emmc_device, "sd-bus"),
                           &error_fatal);
    if (!o62m_sdhci_bind_card(s->normal_emmc_device, card, &error)) {
        error_report_err(error);
        exit(EXIT_FAILURE);
    }
    s->normal_emmc_bound = true;
    s->normal_emmc_exit.notify = marconi_normal_emmc_exit;
    qemu_add_exit_notifier(&s->normal_emmc_exit);
    if(s->normal_emmc_tuning_profile)error_report("MARCONI_NORMAL_EMMC_TUNING_PROFILE "
        "standard-CMD21-endpoint-opt-in=true actual-card-selected-width-and-HS_TIMING-required=true "
        "standard-pattern-FIFO-DMA-and-original-comparison-required=true "
        "revision-advertisement-unchanged=true physical-HS200-analog-claim=false");
    if(s->normal_emmc_fixed_supply)error_report("MARCONI_NORMAL_EMMC_FIXED_SUPPLY "
        "explicit-virtual-external-VCC-mV=%u actual-host-power-request-still-required=true "
        "voltage-selector-zero-readback-preserved=true physical-PMIC-rail-mapping-not-claimed=true "
        "signal-voltage-VCCQ-unchanged=true",config.embedded_fixed_supply_mv);
    error_report("MARCONI_NORMAL_EMMC_PROFILE original-normal-AP-controller=true "
        "declared-virtual-v4-4GiB=true real-eMMC-and-private-volatile-RAM-backend=true "
        "nine-original-ROMFS-payloads-unchanged=true source-fd-closed-before-TEE=true "
        "same-four-canonical-clock-owners=true IRQexternal21=true "
        "synthetic-revision-capabilities-reset-timing-and-card-wire-policy=true "
        "physical-reset-clock-lock-full-SBL-boot-and-UI-not-claimed=true");
}
