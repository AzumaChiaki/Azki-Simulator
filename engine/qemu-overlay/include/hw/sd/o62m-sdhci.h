#ifndef HW_SD_O62M_SDHCI_H
#define HW_SD_O62M_SDHCI_H

/* Opt-in functional research controller. These are explicit virtual inputs,
 * not recovered O62M silicon reset values or physical latency measurements. */
#include "hw/sd/sdhci.h"
#include "hw/sd/o62m-hrs-v1.h"
#include "hw/sd/o62m-hrs-independent-config-v1.h"
#include "hw/sd/o62m-v4-data-v1.h"
#include "hw/sd/o62m-functional-phy-v1.h"

#define TYPE_O62M_SDHCI "o62m-sdhci-research"
#define O62M_SDHCI_OUTER_MMIO_INDEX 1
#define O62M_SDHCI_OUTER_MMIO_SIZE UINT32_C(0x300)

typedef bool (*O62MSdhciClockFn)(void *opaque, bool *available);
typedef bool (*O62MSdhciRamRangeFn)(void *opaque, uint64_t address,
                                   uint32_t bytes, bool write_to_ram);
typedef void (*O62MSdhciFaultFn)(void *opaque, const char *reason,
                                uint64_t offset, unsigned size,
                                bool write, uint64_t value);

typedef enum O62MSdhciTerminalBoundaryPolicy {
    O62M_SDHCI_TERMINAL_BOUNDARY_STRICT = 0,
    O62M_SDHCI_TERMINAL_BOUNDARY_IRQ,
    O62M_SDHCI_TERMINAL_BOUNDARY_NO_IRQ,
} O62MSdhciTerminalBoundaryPolicy;

typedef struct O62MSdhciConfig {
    bool explicit_research_profile;
    uint64_t capabilities_fullword;
    /* Explicit virtual embedded-card VCC input, not measured PMIC/VCCQ state.
     * Only a real POWER_ON request with selector0 uses this optional policy. */
    bool embedded_fixed_supply_enable;
    uint32_t embedded_fixed_supply_mv; /* exactly1800 or3300 when enabled */
    uint64_t max_current_fullword;
    uint16_t standard_version_fullword;
    O62MHrsSpecV1 hrs;
    /* Independent idealized functional PHY; no initial Ready seed. */
    O62MPhySpecV1 phy;
    /* Original AP accesses HRS01/empty BOOT and independent SRS host config
     * words +0x28/+0x30/+0x3c while CARD_RESET_LINE is pending. They retain
     * their actual handlers; no command/data/reset submission is authorized.
     * Default stays strict and the actual card job/deadline remain unchanged. */
    bool independent_cardline_host_config_enable;
    /* Exact original AP dc0548 HRS06 low3 retained timing selector.
     * Unknown silicon reset/fullword is an explicit virtual-profile input.
     * Only 32-bit +0x18 accesses are supported; opaque high bits cannot change. */
    bool hrs06_timing_enable;
    uint32_t hrs06_initial_fullword;
    O62MV4DataBankV1 initial_data_bank;
    uint32_t card_reset_assert_level; /* explicit virtual wire policy,0 or1 */
    bool dma_secure;
    bool allow_v4_32_resume; /* semantic virtual policy,not64-bit write order */
    O62MSdhciTerminalBoundaryPolicy terminal_boundary_policy;
    uint64_t work_latency_ns;
    uint32_t bytes_per_tick;
    uint32_t descriptors_per_tick;
    uint32_t total_descriptor_limit;
    O62MSdhciClockFn clock;
    O62MSdhciRamRangeFn ram_range;
    O62MSdhciFaultFn fault;
    void *opaque;
} O62MSdhciConfig;

/* Finite metadata from the exact functional worker gates. No command argument,
 * data, descriptor address or secret-derived value is exposed by these bits. */
#define O62M_SDHCI_BLOCK_NO_CARD       (UINT32_C(1) << 0)
#define O62M_SDHCI_BLOCK_CARD_HELD     (UINT32_C(1) << 1)
#define O62M_SDHCI_BLOCK_HOST_RESET    (UINT32_C(1) << 2)
#define O62M_SDHCI_BLOCK_PHY_NOT_READY (UINT32_C(1) << 3)
#define O62M_SDHCI_BLOCK_CLOCK_OFF     (UINT32_C(1) << 4)
#define O62M_SDHCI_BLOCK_INTCLK_OFF    (UINT32_C(1) << 5)
#define O62M_SDHCI_BLOCK_INTCLK_UNSTABLE (UINT32_C(1) << 6)
#define O62M_SDHCI_BLOCK_SDCLK_OFF     (UINT32_C(1) << 7)
#define O62M_SDHCI_BLOCK_POWER_OFF     (UINT32_C(1) << 8)

typedef enum O62MSdhciObservedCountSource {
    O62M_SDHCI_OBS_COUNT_IGNORED = 0,
    O62M_SDHCI_OBS_COUNT_LEGACY16,
    O62M_SDHCI_OBS_COUNT_V4_SAAR32,
} O62MSdhciObservedCountSource;

typedef struct O62MSdhciObservation {
    uint64_t mmio_reads, mmio_writes, commands, command_failures;
    /* submissions count accepted CMDREG transactions; commands counts actual
     * sdbus invocations, so zero commands does not imply no submission. */
    uint64_t command_submissions;
    uint32_t blocked_conditions;
    /* Current accepted command/transfer contract only, never command argument
     * or DMA address. SAAR is exposed only when it is actually a V4 count. */
    uint8_t command_index; /* low6 index, separate from command type bits */
    bool data_present, transfer_read, transfer_multi, transfer_bce, transfer_dma;
    bool host_v4_enabled;
    uint16_t transfer_block_size, legacy_block_count;
    uint32_t v4_block_count, canonical_block_count;
    O62MSdhciObservedCountSource count_source;
    int32_t card_state;
    bool card_cmd21_tuning_enabled, card_in_transfer_state;
    uint8_t card_bus_width_config, card_hs_timing;
    uint8_t card_ext_csd_revision, card_type;
    O62MHrsPhaseV1 hrs_phase;
    bool command_pending, command_inhibit, functional;
    bool internal_clock_enabled, internal_clock_stable, sd_clock_enabled;
    bool power_on;
    uint8_t power_voltage_code;
    bool embedded_fixed_supply_enabled, embedded_fixed_supply_active;
    uint32_t embedded_fixed_supply_mv;
    uint64_t card_bytes_read, card_bytes_written;
    uint64_t ram_bytes_read, ram_bytes_written;
    uint64_t descriptor_fetches, descriptors_completed;
    uint64_t controller_resets, card_assertions, card_deassertions;
    uint64_t boundary_events, completed_data_jobs, actual_auto_cmd12;
    uint64_t strict_faults;
    uint64_t phy_jobs_started, phy_jobs_completed, phy_jobs_cancelled;
    uint64_t phy_parameter_writes, phy_reset_assertions;
    bool phy_enabled, phy_ready;
    O62MV4DataBankV1 data_bank;
    O62MV4DataPhaseV1 data_phase;
    uint16_t block_offset;
    bool card_bound, card_held, clock_available, terminal_fault;
} O62MSdhciObservation;

/* Call once before realization, with a serialized stable config object.
 * Existing generic-sdhci devices never call this and keep their old behavior.
 * The new type requires explicit inputs; all default/unconfigured use fails.
 */
bool o62m_sdhci_configure(DeviceState *dev, const O62MSdhciConfig *config,
                          Error **errp);
/* After actual emmc card realization on this controller's sd-bus, before CPU
 * execution. Validates its actual boot-config against the explicit emptyBOOT
 * profile and applies the declared initial virtual reset line to that card.
 * Only this real card object is ever cold-reset for EMMC_RST assertion.
 */
bool o62m_sdhci_bind_card(DeviceState *dev, DeviceState *card, Error **errp);
/* Board must call after each actual clock-controller commit. Unknown clock
 * decode is a strict fault. A known gated clock pauses work without progress.
 */
bool o62m_sdhci_clock_changed(DeviceState *dev, Error **errp);
bool o62m_sdhci_observe(DeviceState *dev, O62MSdhciObservation *out,
                        Error **errp);

#endif
