/*
 * XRing Marconi virtual board for the original O62M firmware.
 * Device transactions, clocks, interrupts, scanout and touch share their
 * canonical state. Unsupported operations terminate without inventing results.
 * Board inputs and firmware are provided by machine properties.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "qemu/osdep.h"
#ifndef _WIN32
#include <sys/mman.h>
#endif
#include "o62m-display-te-input-v1.h"
#include "o62m-dpu-frame-v1.h"
#include "qemu/host-utils.h"
#include "exec/cpu-common.h"
#include "exec/translation-block.h"
#include "exec/target_page.h"
#include "tcg/insn-start-words.h"
#include "qapi/error.h"
#include <glib/gstdio.h>
#include "block/graph-lock.h"
#include "hw/i2c/i2c.h"
#include "ui/input.h"
#include "ui/console.h"
#include "o62m-touch-cst9220-v3.h"
#include "hw/sd/o62m-sdhci.h"
#include "hw/sd/o62m-memory-block-v1.h"
#include "o62m-emmc-functional-clock-v1.h"
#include "crypto/random.h"
#include "system/system.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "trace/control.h"
#include "qemu/timer.h"
#include "hw/boards.h"
#include "hw/arm/armv7m.h"
#include "hw/cpu/cluster.h"
#include "target/arm/arm-powerctl.h"
#include "o62m-cp-cold-v4.h"
#include "o62m-cp-boot-config-v1.h"
#include "o62m-bbpll-v1.h"
#include "o62m-xipc-config-v1.h"
#include "target/arm/cpu.h"
#include "target/arm/cpregs.h"
#include "hw/arm/boot.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-clock.h"
#include "system/address-spaces.h"
#include "system/runstate.h"
#include "system/reset.h"
#include "hw/core/cpu.h"
#include "hw/char/serial.h"
#include "hw/irq.h"
#include "hw/loader.h"
#include "system/system.h"
#include "qom/object.h"

#include "pmic-adc-model.h"
#include "o62m-pmic1000-v1.h"
#include "o62m-pmic-clock-set-v3.h"
#include "o62m-cp-rfspim-tx-v2.h"
#include "o62m-rfspim-progress-v1.h"
#include "o62m-pmic-regulators-v3.h"
#include "o62m-pmu-init-v1.h"
#include "o62m-rtc-seconds-v1.h"
#include "o62m-rtc32k-reference-v1.h"
#include "o62m-rtc-calibration-v2.h"
#include "o62m-pm32k-counter-v1.h"
#include "o62m-pm-functional-v3.h"
#include "o62m-lps-mmio-calls-v2.h"
#include "o62m-psram-init-calls-v5.h"
#include "o62m-psram-calibration-v1.h"
#include "o62m-psram-controller-v1.h"
#include "o62m-pubcp-dfs-repeat-v1.h"
#include "o62m-ap-runtime-config-v1.h"
#include "o62m-pm-domain4-v2.h"
#include "o62m-pm-domain0-on-v1.h"
#include "o62m-mempll-vote-v1.h"
#include "o62m-pubap-clock-defs-v1.h"
#include "o62m-pubcp-clock-defs-v2.h"
#include "o62m-usb-clock-defs-v1.h"
#include "o62m-mipi-clock-defs-v1.h"
#include "o62m-aon120-config-v2.h"
#include "o62m-virtual-dvs-pmic-peer-v1.h"
#include "o62m-static-c894-callers-v1.h"
#include "o62m-static-memset-callers-v1.h"
#include "o62m-static-spmi-write-callers-v1.h"
#include "o62m-static-spmi-wrapper-callers-v1.h"
#include "o62m-virtual-auxadc-reset-v1.h"
#include "o62m-pmic-mode-config-defs-v1.h"
#include "o62m-pmic-mode-config-defs-v4.h"
#include "o62m-pmic-low-events-v1.h"
#include "o62m-pmic-voltage-snapshot-v6.h"
#include "o62m-static-pmic-typed-ports-v1.h"
#include "o62m-static-pmic-public-ports-v2.h"
#include "o62m-auxadc-channel-adapter-v1.h"
#include "o62m-ap-efuse-read-view-v1.h"
#include "o62m-vpu-clock-defs-v1.h"
#include "o62m-pm-bit-domain9-v1.h"
#include "boot-clock-model.h"
#include "ap-clock-defaults-v1.h"
#include "uart-tx-model.h"
#include "o62m-uart1-boot-config-v6.h"
#include "hwspinlock-model.h"
#include "hwlock-retained-restore-v1.h"
#include "o62m-config-envelope-v4.h"
#include "o62m-ota-config-body-v1.h"
#include "o62m-dw32-timer-v1.h"
#include "timer64-model.h"
#include "o62m-cp-cplf-v3.h"
#include "o62m-pmic-boot-events-v6.h"
#include "dfs_config.h"
#include "o62m-pubap-quiesce-v1.h"
#include "o62m-sensys-config-v1.h"
#include "o62m-mipi-ulps-v2.h"
#include "o62m-display-short-command-v3.h"
#include "o62m-spi-nopeer-v2.h"
#include "clock48-projection.h"
#include "timer0-stop-projection.h"
#include "o62m-shot-timer-v1.h"
#include "aon144-read-projection.h"
#include "aon-prediv-query.h"
#include "clock-controls.h"
#include "clock-control-defs-v2.h"
#include "board-control-defs-v6.h"
#include "soc4005-config-defs-v1.h"
#include "otp-program-v2.h"
#include "context-writes-v1.h"
#include "thermal-config-v1.h"
#include "o62m-thermal-mode0-v2.h"
#include "o62m-i2c-empty-config-v3.h"
#include "o62m-i2c-nopeer-v3.h"
#include "o62m-pmic-sync107c-v1.h"
#include "o62m-gpu-boot-reset-v1.h"
#include "o62m-gpu-lvgl-batch-v24.h"
#include "o62m-wdt4012-functional-v1.h"
#include "o62m-cp-wdt-reset-v2.h"
#include "o62m-wdt4012-original-tops-v1.h"
#include "pll-rate-defs-v1.h"
#include "te-platform-fatal-v1.h"
#include "o62m-te-context-v1.h"
#include "o62m-trng-unkeyed-v1.h"
#include "o62m-rnp-pool-v1.h"
#include "o62m-virtual-ao-v1.h"
#include "o62m-te-common-view-v2.h"
#include "o62m-te-capabilities-v3.h"
#include "o62m-virtual-otp-v1.h"
#include "o62m-hash-work-queue-v7.h"
#include "o62m-symc-empty-v1.h"
#include "o62m-hash-stream-v1.h"
#include "o62m-aca-sram-v12.h"
#include "mbox-outbound-v2.h"
#include "ap-mbox-outbound-v2.h"
#include "ap-tee-mailbox-v2.h"
#include "ap-ipcmsg-slots-v2.h"
#include "resource-attribute-defs-v1.h"
#include "range-configuration-defs-v4.h"

#define MARCONI_OTA_BASE 0x60530000u
#define MARCONI_OTA_BYTES 0x00600000u
#define MARCONI_OTA_IMAGE_BYTES 1351680u
#define MARCONI_OTA_SHA256 "837b92c9200e87710be2d877fed449c64380e924f6912652a9ca33fc8ccc394a"

#define MARCONI_AP_BASE 0x605c0000u
#define MARCONI_AP_BYTES 0x03333c00u
#define MARCONI_AP_IMAGE_BYTES 16596992u
#define MARCONI_AP_SECURE_ALIAS 0x705c0000u
#define MARCONI_AP_SHA256 \
    "c60ec97e12de3959b03a5963f05ade29f7843af835e83a528d4ef1b55f3d6428"
#define MARCONI_SBL_BASE 0x60001000u
#define MARCONI_SBL_BYTES 0x0023f000u
#define MARCONI_SBL_VTOR 0x60002000u
/* Exact mmap tee/lps_tee extents and evidenced secure address aliases. */
#define MARCONI_TEE_BASE 0x63e00000u
#define MARCONI_TEE_BYTES 0x00200000u
#define MARCONI_TEE_ALIAS 0x73e00000u
#define MARCONI_LPS_TEE_BASE 0x22005000u
#define MARCONI_LPS_TEE_BYTES 0x00005000u
#define MARCONI_LPS_TEE_ALIAS 0x32005000u
#define MARCONI_TEE_SHA256 \
    "db5e11234bd0c35a05d460db7f4e8fb4929c761846deed2449dd9a1c0178513a"
#define MARCONI_PROBE_EXIT 86

/* Profile-local host diagnostic gate. It never changes guest instructions,
 * architecture or work results. Enabled BEFORE any derived private result
 * enters guest RAM, and remains enabled for this process/owned CPU. */
static bool marconi_sensitive_guest_data;
static CPUState *marconi_sensitive_cpu[3];
static void (*marconi_saved_cpu_dump[3])(CPUState *,FILE *,int);
static void marconi_public_report(const char *fmt,...) G_GNUC_PRINTF(1,2);
static void marconi_public_report(const char *fmt,...)
{ va_list ap;va_start(ap,fmt);error_vreport(fmt,ap);va_end(ap); }
static void marconi_value_report(const char *fmt,...) G_GNUC_PRINTF(1,2);
static void marconi_value_report(const char *fmt,...)
{
    if(marconi_sensitive_guest_data)return;
    va_list ap;va_start(ap,fmt);error_vreport(fmt,ap);va_end(ap);
}
static void marconi_private_cpu_dump(CPUState *cpu,FILE *stream,int flags)
{
 for(unsigned i=0;i<3;i++)if(cpu==marconi_sensitive_cpu[i]) {
  if(marconi_sensitive_guest_data){fputs("MARCONI_CPU_STATE_REDACTED private-result-visible-to-guest=true\n",stream);return;}
  if(marconi_saved_cpu_dump[i])marconi_saved_cpu_dump[i](cpu,stream,flags);return;
 }
 fputs("MARCONI_CPU_STATE_REDACTED unregistered-CPU=true\n",stream);
}
static bool marconi_private_cpu_register(CPUState *cpu)
{
 for(unsigned i=0;i<3;i++)if(marconi_sensitive_cpu[i]==cpu)return true;
 CPUClass *klass=CPU_GET_CLASS(cpu);
 if(!klass->dump_state||klass->dump_state==marconi_private_cpu_dump)return false;
 for(unsigned i=0;i<3;i++)if(!marconi_sensitive_cpu[i]) {
  marconi_saved_cpu_dump[i]=klass->dump_state;marconi_sensitive_cpu[i]=cpu;klass->dump_state=marconi_private_cpu_dump;return true;
 }
 return false;
}
#include "o62m-crf-init-config-v1.h"
#define TYPE_MARCONI_CRF_CPU "marconi-crf-cortex-m0-arm-cpu"
static void marconi_crf_cpu_init(Object *obj)
{
 CPU(obj)->cluster_index=2;
 object_property_set_uint(obj,"mp-affinity",0x200,&error_fatal);
 if(!marconi_private_cpu_register(CPU(obj)))abort();
}
static const TypeInfo marconi_crf_cpu_type={.name=TYPE_MARCONI_CRF_CPU,
 .parent=ARM_CPU_TYPE_NAME("cortex-m0"),.instance_init=marconi_crf_cpu_init};
#define error_report(...) marconi_value_report(__VA_ARGS__)

typedef struct MarconiI2CNopeerContextV1 { void *owner; unsigned index; } MarconiI2CNopeerContextV1;

typedef struct MarconiTouchPeer { I2CSlave parent; void *owner; } MarconiTouchPeer;

typedef struct MarconiProbeState {
    MachineState parent_obj;
    ARMv7MState armv7m;
    ARMv7MState crf_armv7m;
    Clock *crf_clock;
    MemoryRegion crf_container,crf_ram,crf_missing;
    AddressSpace crf_as;
    bool crf_created,crf_release_accepted,crf_entry_zero;
    uint64_t crf_stop_requests;
    uint32_t crf_config[ARRAY_SIZE(o62m_crf_config_registers_v1)];
    unsigned crf_download_next;
    uint64_t crf_writes,crf_control_requests,crf_optional_reads;
    uint16_t crf_control_d000;
    uint64_t crf_control_d000_requests;
    uint16_t crf_optional_config[10];
    Notifier crf_exit;
    bool cp_rfspim_read_pending,cp_rfspim_read_running,cp_rfspim_rx_valid;
    uint16_t cp_rfspim_read_address,cp_rfspim_rx;
    int64_t cp_rfspim_read_deadline;
    uint64_t cp_rfspim_read_count,cp_rfspim_read_completed,cp_rfspim_read_cancelled;

    CPUClusterState cp_cluster;
    ARMCPU cp_cpu;
    MemoryRegion cp_container,cp_system_alias,cp_boot_low,cp_atcm;
    uint32_t cp_atcm_control;
    uint64_t cp_atcm_writes,cp_atcm_changes;
    Notifier cp_atcm_exit;
    DeviceState *cp_l2;
    MemoryRegion *cp_l2_mmio;
    uint32_t cp_l2_lockdown[2];
    uint64_t cp_l2_requests,cp_l2_completions;
    unsigned cp_l2_way_completed;
    Notifier cp_l2_exit;
    struct {
        uint32_t cfg, status_enable, signal_enable, common_raw;
        uint16_t channels_enabled;
        uint64_t config_writes;
    } cp_axidma;
    Notifier cp_axidma_exit;
    struct {
        uint32_t word[12];
        uint64_t reads,writes,control_requests,rx_count_reads;
        uint16_t rx_count,events;
        uint64_t event_reads,event_clears;
        bool reset_held;
        uint64_t reset_assertions,reset_releases;
    } cp_sim_config;
    Notifier cp_sim_config_exit;
    struct {
        uint32_t control, local_enable[2], counter[2], capture[2];
        unsigned valid;
        uint64_t requests, reads, reset_requests;
    } cp_eac;
    Notifier cp_eac_exit;
    uint32_t cp_control398_requests[2][2];
    unsigned cp_control398_count[2];
    Notifier cp_control398_exit;
    uint32_t cp_domain_qos[2];
    unsigned cp_domain_qos_written;
    uint64_t cp_domain_qos_writes;
    Notifier cp_domain_qos_exit;
    uint32_t cp_dfs_request[DFS_CONFIG_DOMAIN_COUNT];
    unsigned cp_dfs_request_written;
    Notifier cp_dfs_request_exit;
    uint32_t cp_ssi_clock[2],cp_ssi_config[10];
    uint64_t cp_ssi_writes;
    O62MCPRFSPIMTxV2 cp_ssi_tx;
    QEMUTimer *cp_rfspim_event;
    uint64_t cp_rfspim_armed,cp_rfspim_clock_generation,cp_rfspim_frame_ns;
    O62MRFSPIMProgressV1 cp_rfspim_progress;
    O62MTimer64 cp_timer[2];
    O62MCPCplfV3 cp_cplf;
    QEMUTimer *cp_timer_event;
    bool cp_timer_created,cp_timer_irq_level;
    uint64_t cp_timer_reads,cp_timer_writes,cp_timer_irq_asserts;
    Notifier cp_timer_exit;
    uint32_t cp_intc_config[12];
    uint64_t cp_intc_config_writes,cp_intc_config_reads;
    Notifier cp_intc_config_exit;
    uint64_t cp_rfspim_retimes,cp_rfspim_pauses;
    bool cp_xtal26m_profile_enabled;
    uint32_t cp_rfspim_divider, *cp_rfspim_emitted;
    unsigned cp_rfspim_emitted_count;
    Notifier cp_ssi_exit;
    O62MBBPllV1 bbpll;
    QEMUTimer *bbpll_event;
    uint64_t bbpll_armed;
    Notifier bbpll_exit;
    uint32_t cp_clock_init[16];
    uint32_t cp_clock_setup_config[11];
    uint32_t cp_clock_gate_config[31];
    uint32_t cp_wifi_control;
    uint64_t cp_wifi_control_reads,cp_wifi_control_writes;
    uint32_t cp_dvfs_limit_word;
    uint64_t cp_dvfs_limit_writes;
    uint64_t cp_clock_setup_reads,cp_clock_setup_writes;
    unsigned cp_clock_init_count;
    Notifier cp_clock_init_exit;
    bool cp_created;
    O62MCpBootConfigV1 cp_ports[2];
    uint32_t cp_port820_control;
    QEMUTimer *cp_ports_event;
    Notifier cp_ports_exit;
    uint64_t cp_ports_armed[2],cp_ports_cold_generation[2];
    uint64_t cp_ports_controller_generation,cp_ports_cal_generation;
    O62MCpColdV1 cp_cold;
    QEMUTimer *cp_cold_event;
    uint64_t cp_cold_armed;
    Notifier cp_cold_exit;
    MemoryRegion ap_ram;
    MemoryRegion handoff_ap_ram;
    MemoryRegion handoff_ap_alias;
    MemoryRegion handoff_heap_alias;
    MemoryRegion mmap_ram;
    MemoryRegion dfx_ram,dfx_view;
    Notifier dfx_exit;
    MemoryRegion ap_boot_buffer_ram;
    MemoryRegion gpu_sys_low_ram, gpu_sys_high_ram;
    bool ap_boot_buffer_enabled;
    MemoryRegion ap_uncache_ram;
    MemoryRegion ipc_nv_ram;
    MemoryRegion ims_nv_ram;
    char *ap_uncache_ram_profile;
    bool ap_uncache_ram_created;
    MemoryRegion heap_tail;
    MemoryRegion aon_ram[4];
    MemoryRegion aon_alias[4];
    MemoryRegion dfx_alias;
    MemoryRegion sp_ap_shared;
    MemoryRegion sp_ap_shared_alias;
    MemoryRegion tee_ree_ram;
    MemoryRegion tee_ree_alias;
    MemoryRegion dsp_cp_ram;
    MemoryRegion dsp_cp_alias;
    MemoryRegion dsp_shared;
    MemoryRegion dsp_shared_alias;
    MemoryRegion cp_image_ram, cp_boot_ram, cp_crf_ram;
    MemoryRegion cp_image_secure, cp_boot_secure, cp_crf_secure;
    bool cp_image_ram_created;
    Notifier cp_image_ram_exit;
    MemoryRegion cp_log;
    MemoryRegion cp_log_alias;
    MemoryRegion tee_alias;
    MemoryRegion ap_cp_sm_ram;
    MemoryRegion ap_cp_sm_alias;
    MemoryRegion lps_tee_ram;
    MemoryRegion lps_tee_alias;
    MemoryRegion missing;
    MemoryRegion memsysctl_guard;
    MemoryRegion pmu_guard;
    char *pmu_init_fixture;
    O62MPMU pmu_init;
    char *rtc_seconds_fixture;
    O62MRTCSeconds rtc_seconds;
    char *rtc32k_reference_profile;
    bool rtc32k_reference_available;
    O62MRTC32kReferenceV1 rtc32k_reference;
    QEMUTimer *rtc32k_reference_event;
    uint64_t rtc32k_armed_generation, rtc32k_reads;
    uint16_t rtc_calibration_config[3];
    uint8_t rtc_calibration_config_written;
    uint64_t rtc_calibration_config_commits;
    O62MRTCCalibrationV1 rtc_calibration;
    QEMUTimer *rtc_calibration_event;
    uint64_t rtc_calibration_armed_generation;
    Notifier rtc32k_reference_exit;
    uint32_t pmic_poweron_seconds;
    char *pm_counter_fixture;
    O62MPM32KCounter pm_counter;
    char *pm_functional_profile;
    O62MPMFunctionalV1 pm_functional;
    char *ap_board_config;
    char *ap_aon_iosleep_config;
    uint32_t ap_aon_iosleep_words[11];
    bool ap_aon_iosleep_capture_written[3];
    uint64_t ap_aon_iosleep_reads,ap_aon_iosleep_writes;
    uint32_t ap_board_words[13];
    uint64_t cp_pm_bookkeeping_reads,cp_pm_bookkeeping_writes;
    uint32_t cp_pm_progress;
    uint64_t cp_pm_progress_reads,cp_pm_progress_writes;
    uint32_t cp_pm_runtime_request,cp_pm_runtime_config;
    uint64_t cp_pm_runtime_control_writes;
    uint32_t cp_pm_events,cp_pm_local_events,cp_pm_init_config[26];
    uint32_t cp_busmon_config[9][7];
    uint64_t cp_busmon_reads,cp_busmon_writes;
    uint16_t cp_busmon_configured;
    uint64_t cp_pm_event_reads,cp_pm_event_clears,cp_pm_init_writes;
    bool ap_board_boot_reason_present;
    bool ap_board_idle_config_present;
    uint8_t ap_board_idle_written;
    bool ap_board_usb_clock_present;
    bool ap_board_usb_config_present;
    bool ap_board_wdt_written;
    bool ap_board_power_policy_written;
    uint32_t ap_vpu_board_words[6];
    uint8_t ap_vpu_board_written;
    uint64_t ap_board_accesses;
    char *pm_domain7_profile;
    O62MPMDomain4V1 pm_domain7; /* independent reusable lifecycle, timing e4/e8/ec */
    QEMUTimer *pm_domain7_event;
    char *vpu_clock_inputs;
    O62MClockControls vpu_clocks;
    uint64_t vpu_clock_reads,vpu_clock_writes;
    char *aon120_config_inputs;
    O62MClockControls aon120_config;
    uint64_t aon120_config_reads,aon120_config_writes;
    uint32_t aon120_config_written;
    char *virtual_dvs_profile;
    O62MVirtualDvsJobV1 virtual_dvs;
    QEMUTimer *virtual_dvs_event;
    uint32_t virtual_dvs_requested;
    uint64_t virtual_dvs_status_reads;
    uint32_t dvfs_com_vote,dvfs_controls[5];
    bool dvfs_com_vote_written,dvfs_control_written[5];
    uint64_t dvfs_com_vote_writes,dvfs_control_writes,dvfs_request200_count[2];
    char *pm_bit8_profile;
    O62MPMBitDomain9V1 pm_bit8; /* independent instance; PMC22c/234 adapter */
    QEMUTimer *pm_bit8_event;
    char *mipi_clock_inputs;
    O62MClockControls mipi_clocks;
    uint64_t mipi_clock_reads,mipi_clock_writes;
    char *pm_bit9_profile;
    O62MPMBitDomain9V1 pm_bit9;
    QEMUTimer *pm_bit9_event;
    char *usb_clock_inputs;
    O62MClockControls usb_clocks;
    uint64_t usb_clock_reads,usb_clock_writes;
    char *pubcp_clock_inputs;
    O62MClockControls pubcp_clocks;
    O62MClockControlDef pubcp_owned_defs[O62M_PUBCP_CLOCK_COUNT_V1];
    uint64_t pubcp_clock_reads,pubcp_clock_writes;
    uint32_t lps_recovery_control,lps_pubcp_auto88;
    uint32_t psram_init_word[O62M_PSRAM_INIT_WORDS_V5];
    bool psram_init_written[O62M_PSRAM_INIT_WORDS_V5];
    struct {uint32_t value;unsigned word;} psram_init_history[256];
    unsigned psram_init_count;
    Notifier psram_init_exit;
    O62MPsramCalibrationV1 psram_cal;
    QEMUTimer *psram_cal_event;
    uint64_t psram_cal_armed,psram_cal_mempll,psram_cal_config,psram_cal_transitions;
    unsigned psram_cal_revision,psram_config_revision;
    uint64_t psram_cal_reads;
    O62MPsramControllerV1 psram_controller;
    QEMUTimer *psram_controller_event;
    uint64_t psram_controller_armed,psram_controller_cal_generation,psram_controller_reads;
    unsigned psram_controller_revision,psram_controller_captured_revision;
    struct {uint32_t parameter,command;} psram_pairs[4];
    uint32_t psram_pair_staging;
    unsigned psram_pair_count;
    bool psram_pair_staged;
    uint64_t psram_final_reads,psram_final_writes;
    O62MPubcpDfsRepeatV1 pubcp_dfs_repeat;
    uint32_t pubap_restore_history[8];
    unsigned pubap_restore_count;
    char *pubap_clock_inputs;
    O62MClockControls pubap_clocks;
    O62MClockControlDef pubap_owned_defs[O62M_PUBAP_CLOCK_COUNT_V1];
    uint64_t pubap_clock_reads,pubap_clock_writes;
    char *pm_domain5_profile;
    O62MPMDomain4V1 pm_domain5; /* independent instance of the same address-free lifecycle */
    QEMUTimer *pm_domain5_event;
    Notifier pm_domain5_exit;
    char *pm_domain4_profile;
    O62MPMDomain4V1 pm_domain4;
    QEMUTimer *pm_domain4_event;
    char *dpu_initial_b0;
    uint32_t dpu_initial_b0_word, dpu_control164;
    uint64_t dpu_control164_writes;
    uint32_t dpu_boot_config[10];
    uint32_t dpu_irq_pending, dpu_irq_mask;
    uint32_t dpu_channel_config[6],dpu_channel_written;
    O62MDpuFrameV1 dpu_frame;
    QEMUTimer *dpu_frame_event;
    Notifier dpu_frame_exit;
    bool dpu_frame_pending;
    int64_t dpu_frame_deadline;
    uint64_t dpu_frame_requests,dpu_frame_completions,dpu_frame_cancelled,dpu_frame_status_acks;
    uint64_t dpu_initial_b0_reads;
    char *ap_runtime_config;
    uint32_t ap_runtime_words[O62M_AP_RUNTIME_CONFIG_WORDS_V1];
    uint32_t ap_timing_commit_request;
    uint32_t pm_domain_request_word;
    O62MXipcConfigV1 xipc_config;
    QEMUTimer *xipc_config_event;
    uint64_t xipc_config_armed;
    uint32_t xipc_cleanup_selector;
    bool xipc_cleanup_selector_written,xipc_cleanup_selector_secure;
    struct {uint32_t selector;bool secure;} xipc_cleanup_history[64];
    Notifier xipc_cleanup_exit;
    uint64_t xipc_cleanup_submissions;
    O62MSensysConfigV1 sensys_config;
    QEMUTimer *sensys_config_event;
    Notifier sensys_config_exit;
    uint64_t sensys_config_armed_generation;
    O62MMipiUlpsV2 mipi_phy;
    O62MDisplayShortV3 display_short;
    QEMUTimer *display_short_event;
    uint64_t display_short_armed, display_short_status_reads;
    uint32_t mipi_cmd_mode, mipi_packet_config, mipi_mode_config, mipi_receive_vcid;
    uint32_t mipi_boot_config[13];
    uint32_t mipi_clock_request;
    uint64_t mipi_clock_request_writes;
    QEMUTimer *mipi_phy_pll_event, *mipi_phy_ulps_event;
    Notifier mipi_phy_exit;
    uint64_t mipi_phy_pll_armed, mipi_phy_ulps_armed;
    bool mipi_phy_reference_available, mipi_phy_test_available;
    uint64_t ap_runtime_accesses;
    Clock *cpuclk;
    bool reported;
    bool sbl_reset;
    bool tee_reset;
    bool ota_reset;
    char *ota_sbl_argument_fixture;
    uint32_t ota_sbl_boot_mode;
    char *ota_invalid_clock_marker_fixture;
    uint32_t ota_invalid_clock_marker;
    char *ota_retained_footer_image;
    char *ota_retained_hwlock_image;
    bool ota_config_write_observations;
    O62MConfigEnvelopeV1 ota_config_envelope;
    uint64_t ap_pinmux_reads, ap_pinmux_writes;
    char *ota_config_body_inputs;
    O62MOtaConfigBodyV1 ota_config_body;
    char *timer4014_inputs;
    bool timer4014_load_plus_one;
    O62MDw32V1 timer4014;
    QEMUTimer *timer4014_event;
    uint64_t timer4014_logged_expiries[2];
    uint64_t timer4014_accesses;
    MemoryRegion ota_retained_footer_ram;
    char *mmap_image;
    char *tee_ap_image;
    bool counter32k;
    unsigned counter_reads;
    O62MUart1BootConfigV1 uart1_boot,cp_uart2;
    Notifier cp_uart2_exit;
    bool uart1_empty_irq_level;
    uint64_t uart1_empty_asserts,uart1_empty_deasserts;
    Notifier uart1_empty_exit;
    QEMUTimer *uart1_tx_event;
    uint8_t uart1_outbound[65536];
    uint64_t uart1_accepted,uart1_emitted,uart1_discarded,uart1_cancelled;
    bool uart16550;
    bool uart_dw_config;
    SerialState uart;
    unsigned uart_accesses;
    uint8_t uart_ier_extra;
    uint8_t uart_stet;
    uint32_t uart_dlf;
    char *uart_tx_fixture;
    O62MUartTx tx;
    QEMUTimer *tx_timer;
    uint32_t uart_pclk;
    char *adc_fixture;
    O62MPmicAdc pmic_adc;
    char *auxadc_channel_profile;
    O62MPmicAdcAPAdapterV1 auxadc_ap;
    QEMUTimer *auxadc_event;
    bool auxadc_irq_level;
    uint64_t auxadc_irq_assertions,auxadc_irq_deassertions;
    uint64_t auxadc_reported_started,auxadc_reported_completed;
    uint64_t auxadc_cc_reads,auxadc_result_reads;
    char *pmic1000_initial;
    O62MPmic1000V1 pmic1000;
    char *pmic_clock_inputs;
    O62MPmicClockV2 pmic_clocks;
    uint16_t pmic_config1328;
    uint64_t pmic_config1328_commits;
    Notifier pmic_config1328_exit;
    char *pmic_regulator_inputs;
    O62MPmicRegulatorsV1 pmic_regulators;
    char *virtual_auxadc_reset_profile;
    O62MVirtualAuxadcResetV1 virtual_auxadc_reset;
    char *virtual_pmic_bc_config;
    uint16_t pmic_bc_word;
    uint64_t pmic_bc_reads,pmic_bc_writes;
    char *pmic_mode_config_inputs;
    O62MClockControls pmic_mode_config;
    bool pmic_mode_extended;
    uint16_t pmic_threshold_words[2];
    uint8_t pmic_threshold_written;
    uint16_t pmic_low_event_pending;
    char *pmic_voltage_source_profile;
    O62MPmicVoltageSnapshotV6 pmic_voltage;
    QEMUTimer *pmic_voltage_event;
    uint64_t pmic_voltage_armed_generation;
    Notifier pmic_voltage_exit;
    uint64_t pmic_mode_reads,pmic_mode_writes;
    char *gpu_boot_identity_profile;
    char *venc_device_profile;
    char *vdec_device_profile;
    char *ap_mailbox_config;
    char *adsp_publication_profile;
    char *pmic_vbus_input;
    char *pmic_boot_events_profile;
    O62MPmicBootEventsV6 pmic_boot_events;
    uint64_t pmic_vbus_reads;
    uint64_t adsp_publication_reads;
    uint32_t ap_mailbox_enable;
    struct { uint32_t enable; uint64_t reads,writes; } cp_mailbox_config;
    O62MAPIPCSlotsV1 ap_ipc_slots;
    Notifier ap_ipc_slots_exit;
    char *gpu_boot_reset_profile;
    O62MGpuBootResetV1 gpu_boot_reset;
    QEMUTimer *gpu_boot_event;
    uint64_t gpu_boot_armed_generation,gpu_boot_reads,gpu_boot_writes;
    uint64_t gpu_address_writes;
    bool gpu_address_staged;
    O62MGpuBatchV1 *gpu_fill;
    QEMUTimer *gpu_fill_event;
    uint32_t gpu_state_bank[512];bool gpu_state_known[512];
    uint64_t gpu_state_commits;
    uint64_t gpu_fill_generation,gpu_fill_armed,gpu_fill_reset_generation;
    uint64_t gpu_fill_submissions,gpu_fill_completions,gpu_fill_cancelled;
    uint64_t gpu_host_parse_us,gpu_host_render_us,gpu_host_capture_us,gpu_host_free_us,gpu_host_callback_us,dpu_host_capture_us;
    uint64_t gpu_fill_irq_asserts,gpu_fill_irq_deasserts;
    int64_t gpu_fill_deadline;
    bool gpu_fill_irq_level;
    Notifier gpu_boot_exit;
    char *pmic_sync107c_profile;
    O62MPmicSync107cV1 pmic_sync107c;
    QEMUTimer *pmic_sync107c_event;
    uint64_t pmic_sync107c_armed_generation,pmic_sync107c_pending_reads;
    Notifier pmic_sync107c_exit;
    char *pmic_button_config_initial;
    uint16_t pmic_button_config_word;
    uint64_t pmic_button_reads,pmic_button_writes;
    bool pmic_button_followup_present;
    uint16_t pmic_button_followup_word;
    uint64_t pmic_button_followup_reads,pmic_button_followup_writes;
    bool pmic_button_second_followup_present;
    uint16_t pmic_button_second_followup_word;
    uint64_t pmic_button_second_followup_reads,pmic_button_second_followup_writes;
    char *pmic_alias_read_policy;
    uint64_t pmic_alias_reads;
    O62MBootClockModel boot_clocks;
    O62MHwSpinlock hwlock;
    bool timer64;
    char *i2c_nopeer_profile;
    uint32_t i2c_nopeer_hz;
    O62MI2CNopeerV3 i2c_nopeer[5];
    I2CBus *i2c_nopeer_bus[5];
    QEMUTimer *i2c_nopeer_event[5];
    MarconiI2CNopeerContextV1 i2c_nopeer_context[5];
    Notifier i2c_nopeer_exit;
    O62MTouchCST9220V3 touch;
    QEMUTimer *touch_event;
    QEMUPutMouseEntry *touch_mouse_entry;
    Notifier touch_exit;
    bool touch_irq_level;
    char *ap_i2c_empty_config;
    O62MI2CEmptyConfigV3 ap_i2c_empty[5];
    char *ap_i2c_soc_commands;
    uint32_t ap_i2c_soc_last[2];
    bool ap_i2c_soc_written[2];
    uint64_t ap_i2c_soc_captures[2];
    char *wdt4012_profile;
    O62MWdt4012V1 wdt4012;
    O62MCPWdtResetV2 cp_wdt;
    QEMUTimer *cp_wdt_event;
    uint64_t cp_wdt_accesses,cp_wdt_starts,cp_wdt_feeds,cp_wdt_preloads,cp_wdt_expiries,cp_wdt_reset_cancellations;
    Notifier cp_wdt_exit;
    QEMUTimer *wdt4012_event;
    uint64_t wdt4012_reads,wdt4012_writes,wdt4012_gate_reports;
    char *ap_watchdog_init;
    uint32_t cp_aon_requests[2];
    int64_t cp_aon_request_ns[2];
    unsigned cp_aon_request_count;
    Notifier cp_aon_exit;
    uint32_t ap_watchdog_aon_word;
    bool ap_watchdog_aon_written;
    uint64_t ap_watchdog_aon_writes;
    bool ap_watchdog_soc_present;
    uint32_t ap_watchdog_soc_word;
    unsigned ap_watchdog_soc_phase;
    char *ap_spi_config;
    uint32_t ap_spi_config_words[7],ap_spi_error_latch;
    O62MSPINoPeerV1 ap_spi;
    QEMUTimer *ap_spi_event;
    uint64_t ap_spi_armed;
    Notifier ap_spi_exit;
    uint64_t ap_spi_config_reads,ap_spi_config_writes;
    char *ap_gpio_config;
    uint32_t ap_gpio_config_words[5];
    uint32_t ap_gpio_direction[5];
    uint32_t ap_gpio_output_latch[5];
    uint32_t ap_gpio_irq_config[5][6];
    uint32_t ap_gpio_route[12];
    O62MDisplayTEInputV1 display_te_input;
    QEMUTimer *display_te_event;
    Notifier display_te_exit;
    bool display_te_irq_level;
    bool ap_gpio_config_written[5];
    uint64_t ap_gpio_config_writes;
    char *ap_te_host1_config;
    O62MSymcEmptyV1 ap_te_host1_symc;
    uint32_t ap_te_host1_rnp_mask;
    uint32_t ap_te_host1_aca_config[3];
    bool ap_te_host1_rnp_mask_written;
    uint64_t ap_te_host1_config_accesses;
    char *ap_te_platform_view;
    uint64_t ap_te_platform_clock_reads,ap_te_platform_clock_writes,ap_te_platform_stat_reads,ap_te_platform_cap_reads;
    char *ap_timer64_view;
    uint64_t ap_timer64_view_reads,ap_timer64_view_writes,ap_timer64_view_pairs;
    bool qos_observed_write_experiment;
    uint32_t qos_priority[2];
    bool qos_priority_written[2];
    uint64_t qos_a8_zero_writes;
    char *lowpage_response_experiment;
    uint8_t lowpage_written_bytes[0x1000];
    bool lowpage_initialized[0x1000];
    uint64_t lowpage_writes, lowpage_reads;
    O62MTimer64 timer64_model;
    QEMUTimer *timer64_timer;
    O62MTimer64 dvfs_timer64_model;
    QEMUTimer *dvfs_timer64_timer;
    char *lcd_frame_file;
    uint8_t *lcd_shared_frame;
    uint32_t lcd_shared_sequence;
    char *timer64_terminal_profile;
    Notifier timer64_exit;
    bool timer64_irq_level;
    uint64_t timer64_irq_asserts,timer64_irq_deasserts,timer64_logged_expirations;
    bool pmic_write_pending;
    bool pmic_write_dvs_owned;
    uint64_t pmic_write_dvs_generation;
    uint16_t pmic_write_reg, pmic_write_value;
    int64_t pmic_write_deadline;
    unsigned pmic_accesses;
    char *explore_seed;
    char *explore_latency;
    uint32_t ctrl54, spmi_ctrl, uart_clock;
    uint32_t spmi_irq_pending;
    uint64_t spmi_irq_reads, spmi_irq_clears;
    Notifier spmi_irq_exit;
    uint32_t pubap_dfs_control, pubcp_dfs_control;
    char *dfs_config_fixture;
    struct dfs_config_state dfs_config;
    O62MPubapQuiesceV1 pubap_quiesce;
    QEMUTimer *pubap_quiesce_event;
    Notifier pubap_quiesce_exit;
    uint64_t pubap_quiesce_armed_generation;
    char *clock48_fixture;
    O62MClock48Projection clock48_projection;
    char *timer0_runtime_fixture;
    bool timer0_one_shot_experiment;
    O62MShotTimer timer0_runtime;
    QEMUTimer *timer0_runtime_timer;
    char *timer0_stop_fixture;
    O62MTimer0StopProjection timer0_stop_projection;
    char *aon144_read_fixture;
    O62MAon144ReadProjection aon144_read_projection;
    char *aon_prediv_query_fixture;
    O62MAonPredivQueryProjection aon_prediv_query;
    char *clock_controls_fixture;
    O62MClockControls clock_controls;
    uint64_t clock_controls_reads;
    uint64_t clock_controls_writes;
    char *ap_domain_status_fixture;
    O62MPMDomain0OnV1 ap_domain0;
    QEMUTimer *ap_domain0_event;
    uint64_t ap_domain0_armed_generation;
    Notifier ap_domain0_exit;
    uint64_t ap_domain_status_reads;
    uint32_t ap_domain0_guard;
    bool pm_domain1_available,pm_domain1_on;
    uint32_t pm_domain6_status;
    uint8_t pm_domain1_timing_written;
    bool dtgu_lut0_observed_write;
    bool dtgu_lut0_written;
    uint32_t dtgu_lut0;
    char *otp_read_fixture;
    O62MOtpRead otp_read;
    O62MOtpProgramV2 otp_program;
    Notifier otp_program_exit;
    char *ap_efuse_read_view;
    char *ap_thermal_config_view;
    char *thermal_mode0_profile;
    O62MThermalMode0V2 thermal_mode0;
    QEMUTimer *thermal_mode0_event;
    Notifier thermal_mode0_exit;
    bool thermal_mode0_clock;
    uint64_t thermal_mode0_status_reads,thermal_mode0_result_reads;
    char *dvfs_main_clock_initial;
    char *ap_pll_rate_view;
    char *dvfs_vote_config;
    uint32_t dvfs_vote_d0_word;
    bool dvfs_vote_d0_written;
    uint64_t dvfs_vote_d0_accesses;
    uint64_t ap_pll_rate_reads;
    uint32_t dvfs_main_clock_word;
    uint64_t dvfs_main_clock_reads;
    uint64_t ap_thermal_config_writes;
    O62MAPEfuseReadViewV1 ap_efuse_view;
    uint64_t ap_efuse_clock_accesses;
    char *tee_sbl_argument_fixture;
    uint32_t tee_sbl_boot_mode;
    char *rc_measurement_fixture;
    uint32_t rc_measurement[2];
    bool mbox_outbound_experiment;
    O62MMboxOutbound mbox_outbound;
    O62MMboxOutbound ap_mbox_outbound,cp_mbox_outbound;
    O62MApTeeMailboxV1 ap_tee_mbox;
    bool ap_tee_mbox_irq_level[2];
    uint64_t ap_tee_mbox_asserts[2],ap_tee_mbox_deasserts[2];
    Notifier ap_tee_mbox_exit;
    char *wdt_policy_fixture;
    uint32_t wdt_policy_word;
    char *te_platform_fatal_fixture;
    O62MTePlatformFatal te_platform_fatal;
    char *te_context_fixture;
    O62MTeContext te_context;
    O62MTeCommonViewV1 te_common_view;
    char *te_capability_inputs;
    O62MTeCapabilitiesV3 te_capabilities;
    char *hash_init_profile;
    O62MHmacSha256V1 hash_context;
    uint8_t hash_key_input[128],hash_inner_digest[32];
    unsigned hash_key_input_bytes;
    bool hash_is_hmac;
    bool hash_inner_done,hash_finished;
    QEMUTimer *hash_init_event;
    Notifier hash_exit;
    bool hash_job_queued,hash_exit_registered;
    uint64_t hash_work_ns;
    int64_t hash_deadline_ns;
    char *aca_config_profile;
    O62MAcaSramV12 aca_sram;
    Notifier aca_sram_exit;
    QEMUTimer *aca_work_event;
    uint64_t aca_work_armed;
    uint32_t aca_config_word[3];
    char *symc_empty_profile;
    O62MHashWorkQueueV7 symc_empty;
    char *rnp_mask_config;
    uint32_t rnp_mask_word;
    char *rnp_postkey_config;
    uint32_t rnp_live_config[6],rnp_controller_mask;
    bool rnp_live_config_active;
    char *virtual_otp_profile,*virtual_otp_public_template;
    O62MVirtualOtpV1 virtual_otp;
    QEMUTimer *virtual_otp_event;
    Notifier virtual_otp_exit;
    bool virtual_otp_exit_registered;
    char *trng_virtual_profile;
    O62MVirtualAoV1 trng_virtual;
    QEMUTimer *trng_virtual_event;
    Notifier trng_virtual_exit;
    bool trng_virtual_exit_registered;
    uint64_t trng_virtual_accesses;
    char *trng_unkeyed_fixture;
    O62MTrngUnkeyed trng_unkeyed;
    O62MRnpPoolV1 rnp_pool;
    QEMUTimer *rnp_pool_event;
    Notifier rnp_pool_exit;
    uint64_t rnp_pool_armed;
    uint32_t rnp_source_pending;
    char *pll_rate_fixture;
    O62MClockControls pll_rate;
    O62MMemPllVoteV1 mempll_vote;
    QEMUTimer *mempll_event;
    uint64_t mempll_armed;
    Notifier mempll_exit;
    bool thermal_observed_writes;
    O62MThermalConfig thermal_config;
    bool context_observed_writes;
    O62MContextWrites context_writes;
    char *normal_emmc_profile;
    char *normal_emmc_fixed_supply;
    char *normal_emmc_tuning_profile;
    char *normal_emmc_fault_test;
    bool normal_emmc_fault_test_injected;
    char *normal_emmc_media;
    char *normal_emmc_writeback;
    DeviceState *normal_emmc_device;
    BlockBackend *normal_emmc_backend;
    Notifier normal_emmc_exit;
    bool normal_emmc_bound, normal_emmc_irq_level;
    uint64_t normal_emmc_irq_asserts, normal_emmc_irq_deasserts;
    char *soc4005_config_inputs;
    char *ap_soc4005_2c0_initial;
    uint64_t ap_soc4005_2c0_accesses;
    O62MClockControls soc4005_config;
    char *board_controls_fixture;
    O62MClockControls board_controls;
    char *resource_attribute_fixture;
    O62MClockControls resource_attributes;
    char *range_configuration_fixture;
    O62MClockControls range_configuration;
    QEMUTimer *research_deadline;
    uint64_t transaction_ns;
    int64_t select_deadline;
    bool select_pending, slave_selected;
    unsigned explore_accesses;

} MarconiProbeState;

/*
 * AP 0x60c36826 extends the low word on wrap and computes
 * (extended_ticks * 10000) >> 15; 0x605cc5da uses a 32768-tick interval.
 * This provides the observed low word only. The counter's power/reset
 * controls, high word, interrupt routing and retention remain unknown.
 * The epoch is virtual machine time zero. Splitting seconds avoids an
 * overflowing nanosecond product; the final cast supplies modulo 2^32.
 */
static bool marconi_enable_private_guest_diagnostics(MarconiProbeState *s)
{
    CPUState *cpu=CPU(s->armv7m.cpu);
    if(marconi_sensitive_guest_data)return marconi_private_cpu_register(cpu)&&(!s->cp_created||marconi_private_cpu_register(CPU(&s->cp_cpu)))&&(!s->crf_created||marconi_private_cpu_register(CPU(s->crf_armv7m.cpu)));
    if(!s->hash_init_profile||!s->uart_tx_fixture)return false;
    CPUClass *klass=CPU_GET_CLASS(cpu);
    if(!klass->dump_state||klass->dump_state==marconi_private_cpu_dump)return false;
#if !defined(CONFIG_TRACE_LOG) || defined(CONFIG_TRACE_SIMPLE) || defined(CONFIG_TRACE_FTRACE) || defined(CONFIG_TRACE_UST) || defined(CONFIG_TRACE_DTRACE) || defined(CONFIG_TRACE_SYSLOG)
    return false; /* This reviewed profile covers this build's LOG backend. */
#else
    TraceEventIter iter;TraceEvent *event;
    trace_event_iter_init_all(&iter);
    while((event=trace_event_iter_next(&iter))) {
        if(trace_event_get_state_static(event)&&trace_event_get_state_dynamic(event))
            trace_event_set_state_dynamic(event,false);
    }
    trace_event_iter_init_all(&iter);
    while((event=trace_event_iter_next(&iter))) {
        if(trace_event_get_state_static(event)&&trace_event_get_state_dynamic(event))return false;
    }
    Error *log_error=NULL;bool ok=qemu_set_log(0,&log_error);error_free(log_error);log_error=NULL;
    if(!ok||(qemu_loglevel&~LOG_TRACE))return false;
    /* CONFIG_TRACE_LOG retains LOG_TRACE at zero requested flags. Disable
     * all dynamic events, and send any bare qemu_log to the standard OS
     * discard stream. Do not claim the old/file logging system is closed. */
    ok=qemu_set_log_filename("/dev/null",&log_error);error_free(log_error);
    if(!ok||(qemu_loglevel&~LOG_TRACE))return false;
#endif
    if(!marconi_private_cpu_register(cpu)||(s->cp_created&&!marconi_private_cpu_register(CPU(&s->cp_cpu)))||(s->crf_created&&!marconi_private_cpu_register(CPU(s->crf_armv7m.cpu))))return false;
    marconi_sensitive_guest_data=true;
    marconi_public_report("MARCONI_PRIVATE_GUEST_DIAGNOSTICS_ACTIVE dynamic-LOG-events-disabled=true LOG-output-OS-discard-sink=true "
                          "owned-CPU-register-dumps-redacted=true RAM-snapshots-disabled=true "
                          "BSP-value-and-UART-output-disabled=true no-key-digest-or-entropy-output=true");
    return true;
}

static bool marconi_pm_domain4_refresh(MarconiProbeState *s)
{
    if(!s->pm_domain4_profile)return true;
    uint64_t configs=s->pm_domain4.config_jobs,transitions=s->pm_domain4.transitions;
    if(!o62m_pm_domain4_advance_v1(&s->pm_domain4,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
        marconi_public_report("MARCONI_PM_DOMAIN4_BOUNDARY reason=virtual-time-invalid no-completion=true");
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC,MARCONI_PROBE_EXIT);return false;
    }
    if(configs!=s->pm_domain4.config_jobs)marconi_public_report("MARCONI_PM_DOMAIN4_CONFIG_COMMIT jobs=%" PRIu64
        " actual-active-timing-copy=true on-bit-preserved=true no-powerup-inferred=true",s->pm_domain4.config_jobs);
    if(transitions!=s->pm_domain4.transitions)marconi_public_report("MARCONI_PM_DOMAIN4_TRANSITION_COMPLETE jobs=%" PRIu64
        " model-domain-on=%u actual-prior-request-and-virtual-timer=true guard-word-unchanged=true physical-rail-effects-not-claimed=true",
        s->pm_domain4.transitions,s->pm_domain4.on);
    int64_t deadline=o62m_pm_domain4_deadline_v1(&s->pm_domain4);
    if(deadline>=0)timer_mod_ns(s->pm_domain4_event,deadline);else timer_del(s->pm_domain4_event);
    return true;
}
static void marconi_pm_domain4_callback(void *opaque)
{ (void)marconi_pm_domain4_refresh(opaque); }

static bool marconi_pm_domain7_refresh(MarconiProbeState *s)
{
    if(!s->pm_domain7_profile)return true;
    uint64_t configs=s->pm_domain7.config_jobs,transitions=s->pm_domain7.transitions;
    if(!o62m_pm_domain4_advance_v1(&s->pm_domain7,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
        marconi_public_report("MARCONI_PM_DOMAIN7_BOUNDARY reason=virtual-time-invalid no-completion=true");
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC,MARCONI_PROBE_EXIT);return false;
    }
    if(configs!=s->pm_domain7.config_jobs)marconi_public_report("MARCONI_PM_DOMAIN7_CONFIG_COMMIT jobs=%" PRIu64
        " actual-active-timing-copy=true on-bit-preserved=true no-powerup-inferred=true",s->pm_domain7.config_jobs);
    if(transitions!=s->pm_domain7.transitions)marconi_public_report("MARCONI_PM_DOMAIN7_TRANSITION_COMPLETE jobs=%" PRIu64
        " model-domain-on=%u actual-prior-request-and-virtual-timer=true guard-word-unchanged=true physical-rail-effects-not-claimed=true",
        s->pm_domain7.transitions,s->pm_domain7.on);
    int64_t deadline=o62m_pm_domain4_deadline_v1(&s->pm_domain7);
    if(deadline>=0)timer_mod_ns(s->pm_domain7_event,deadline);else timer_del(s->pm_domain7_event);
    return true;
}
static void marconi_pm_domain7_callback(void *opaque)
{ (void)marconi_pm_domain7_refresh(opaque); }

static void marconi_pm_domain5_cancel(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->pm_domain5_event)timer_del(s->pm_domain5_event);
    o62m_pm_domain_cancel_v2(&s->pm_domain5);
}
static void marconi_pm_domain5_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,pm_domain5_exit);(void)opaque;
    marconi_public_report("MARCONI_PM_DOMAIN5_EXIT configs=%" PRIu64 " transitions=%" PRIu64
        " pending=%u waits-for-config=%u owned-timer-cancelled=true",
        s->pm_domain5.config_jobs,s->pm_domain5.transitions,s->pm_domain5.transition_pending,
        s->pm_domain5.transition_waits_config);
    marconi_pm_domain5_cancel(s);
}
static bool marconi_pm_domain5_refresh(MarconiProbeState *s)
{
    if(!s->pm_domain5_profile)return true;
    if(s->reported){marconi_pm_domain5_cancel(s);return true;}
    bool waiting=s->pm_domain5.transition_waits_config;
    uint64_t configs=s->pm_domain5.config_jobs,transitions=s->pm_domain5.transitions;
    if(!o62m_pm_domain4_advance_v1(&s->pm_domain5,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
        marconi_public_report("MARCONI_PM_DOMAIN5_BOUNDARY reason=virtual-time-invalid no-completion=true");
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC,MARCONI_PROBE_EXIT);return false;
    }
    if(waiting&&!s->pm_domain5.transition_waits_config)
        marconi_public_report("MARCONI_PM_DOMAIN5_DEFERRED_ON_STARTED config-generation=%" PRIu64
            " actual-config-copy-completed=true deadline-ns=%" PRId64 " positive-transition-from-service-now=true",
            s->pm_domain5.queued_config_generation,s->pm_domain5.transition_due_ns);
    if(configs!=s->pm_domain5.config_jobs)marconi_public_report("MARCONI_PM_DOMAIN5_CONFIG_COMMIT jobs=%" PRIu64
        " actual-active-timing-copy=true on-bit-preserved=true no-powerup-inferred=true",s->pm_domain5.config_jobs);
    if(transitions!=s->pm_domain5.transitions)marconi_public_report("MARCONI_PM_DOMAIN5_TRANSITION_COMPLETE jobs=%" PRIu64
        " model-domain-on=%u actual-prior-request-and-virtual-timer=true guard-word-unchanged=true physical-rail-effects-not-claimed=true",
        s->pm_domain5.transitions,s->pm_domain5.on);
    int64_t deadline=o62m_pm_domain4_deadline_v1(&s->pm_domain5);
    if(deadline>=0)timer_mod_ns(s->pm_domain5_event,deadline);else timer_del(s->pm_domain5_event);
    return true;
}
static void marconi_pm_domain5_callback(void *opaque)
{ (void)marconi_pm_domain5_refresh(opaque); }

static bool marconi_pm_bit9_refresh(MarconiProbeState *s)
{
    if(!s->pm_bit9_profile)return true;
    uint64_t completed=s->pm_bit9.completed;
    if(!o62m_pm_bit_domain9_advance_v1(&s->pm_bit9,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
        marconi_public_report("MARCONI_PM_BIT9_BOUNDARY reason=virtual-time-invalid");
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC,MARCONI_PROBE_EXIT);return false;
    }
    if(completed!=s->pm_bit9.completed)marconi_public_report("MARCONI_PM_BIT9_TRANSITION_COMPLETE jobs=%" PRIu64
        " model-domain-on=%u actual-prior-request-and-virtual-timer=true no-physical-busy-encoding-or-USB-completion-claim=true",
        s->pm_bit9.completed,s->pm_bit9.on);
    if(s->pm_bit9.pending)timer_mod_ns(s->pm_bit9_event,s->pm_bit9.due_ns);else timer_del(s->pm_bit9_event);
    return true;
}
static void marconi_pm_bit9_callback(void *opaque)
{ (void)marconi_pm_bit9_refresh(opaque); }

static bool marconi_pm_bit8_refresh(MarconiProbeState *s)
{
    if(!s->pm_bit8_profile)return true;
    uint64_t completed=s->pm_bit8.completed;
    if(!o62m_pm_bit_domain9_advance_v1(&s->pm_bit8,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
        marconi_public_report("MARCONI_PM_BIT8_BOUNDARY reason=virtual-time-invalid");
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC,MARCONI_PROBE_EXIT);return false;
    }
    if(completed!=s->pm_bit8.completed)marconi_public_report("MARCONI_PM_BIT8_TRANSITION_COMPLETE jobs=%" PRIu64
        " model-domain-on=%u actual-prior-request-and-virtual-timer=true no-physical-busy-encoding-or-USB-completion-claim=true",
        s->pm_bit8.completed,s->pm_bit8.on);
    if(s->pm_bit8.pending)timer_mod_ns(s->pm_bit8_event,s->pm_bit8.due_ns);else timer_del(s->pm_bit8_event);
    return true;
}
static void marconi_pm_bit8_callback(void *opaque)
{ (void)marconi_pm_bit8_refresh(opaque); }

static uint64_t marconi_public_code_pc(uint64_t pc)
{
    if((pc>=MARCONI_TEE_ALIAS&&pc-MARCONI_TEE_ALIAS<UINT64_C(770048))||
       (pc>=MARCONI_AP_BASE&&pc-MARCONI_AP_BASE<MARCONI_AP_IMAGE_BYTES)||
       (pc>=UINT64_C(0x64000000)&&pc-UINT64_C(0x64000000)<UINT64_C(4890624))||pc<444u)return pc;
    return UINT64_MAX; /* fixed sentinel, never a private-derived invalid PC */
}

/* A single ABI-saved public control-flow token, never a stack/register dump.
 * Original c894 pushes6registers then leaves SP/R7 fixed across c8e4.
 * Original reset sets MSP61745d00; BSS ends61744d00. Restrict to this
 * conservative startup gap only, not a claim about arbitrary task stacks. */
static void marconi_public_c894_caller(MarconiProbeState *s,uint64_t pc)
{
    if(!s->virtual_dvs_profile||pc!=0x60c4c8e4||!s->tee_ap_image||current_cpu!=CPU(s->armv7m.cpu))return;
    CPUARMState *env=&s->armv7m.cpu->env;
    uint32_t sp=env->regs[13];
    if((sp&7)||sp>UINT32_MAX-20||env->regs[7]!=sp) {
        marconi_public_report("MARCONI_PUBLIC_C894_CALLER matched=false reason=original-callee-frame-precondition raw-stack-and-registers-not-output=true");return;
    }
    uint32_t saved=0;bool task_frame=false;
    if(sp>=UINT32_C(0x61744d00)&&sp<=UINT32_C(0x61745d00)-24) {
        /* Preserve the original conservative startup-gap observer. */
        uint64_t offset=(uint64_t)sp+20-MARCONI_AP_BASE;
        if(offset>memory_region_size(&s->handoff_ap_ram)-4)return;
        const uint8_t *ram=memory_region_get_ram_ptr(&s->handoff_ap_ram);
        saved=ldl_le_p(ram+offset);
    } else {
        /* One public-callee ABI slot, not a general stack reader. Exclude
         * image/BSS, APsecurealias, TEE63e/73e/private crypto RAM owners and
         * every MMIO/ROM/other MR. Never inspect a TCB or adjacent word. */
        uint64_t slot=(uint64_t)sp+20;
        uint64_t end=(uint64_t)MARCONI_AP_BASE+MARCONI_AP_BYTES;
        if(env->v7m.secure||(!arm_v7m_is_handler_mode(env)&&(env->v7m.control[0]&1u))||
           sp<UINT32_C(0x61744d00)||slot>=end||4>end-slot) {
            marconi_public_report("MARCONI_PUBLIC_C894_CALLER matched=false reason=outside-owned-AP-data-frame raw-stack-and-registers-not-output=true");return;
        }
        MemoryRegionSection section=memory_region_find(get_system_memory(),slot,4);
        bool valid=section.mr==&s->handoff_ap_ram&&memory_region_is_ram(section.mr)&&
                   !memory_region_is_rom(section.mr)&&int128_ge(section.size,int128_make64(4));
        if(section.mr)memory_region_unref(section.mr);
        if(!valid)return;
        uint8_t raw[4]={0};
        MemTxAttrs attrs={.secure=false,.user=false};
        MemTxResult result=address_space_read(cpu_get_address_space(CPU(s->armv7m.cpu),ARMASIdx_NS),
                                              slot,attrs,raw,sizeof(raw));
        if(result==MEMTX_OK)saved=ldl_le_p(raw);
        o62m_virtual_ao_wipe_v1(raw,sizeof(raw));
        if(result!=MEMTX_OK)return;
        task_frame=true;
    }
    int index=(saved&1)?o62m_static_c894_caller_index_v1(saved):-1;
    o62m_virtual_ao_wipe_v1(&saved,sizeof(saved));
    if(index>=0)marconi_public_report("MARCONI_PUBLIC_C894_CALLER matched=true static-caller-index=%d task-frame=%u single-ABI-word-only=true no-raw-LR-SP-stack-register-or-PMIC-data-output=true",index,task_frame);
    else marconi_public_report("MARCONI_PUBLIC_C894_CALLER matched=false reason=no-static-BL-candidate raw-stack-and-registers-not-output=true");
}

/* Exact d044 original push8-register frame at d0a6: one saved public
 * return token SP+28. This is not the c894 six-register ABI or a stack dump. */
static void marconi_public_spmi_write_caller(MarconiProbeState *s,uint64_t pc)
{
    if(pc!=UINT64_C(0x60c4d0a6)||!s->i2c_nopeer_profile||!s->virtual_dvs_profile||
       !s->tee_reset||!s->tee_ap_image||current_cpu!=CPU(s->armv7m.cpu))return;
    CPUARMState *env=&s->armv7m.cpu->env;
    if(env->v7m.secure||(!arm_v7m_is_handler_mode(env)&&(env->v7m.control[0]&1u)))return;
    uint32_t sp=env->regs[13];
    uint64_t slot=(uint64_t)sp+28;
    uint64_t end=(uint64_t)MARCONI_AP_BASE+MARCONI_AP_BYTES;
    if((sp&7)||sp>UINT32_MAX-28||env->regs[7]!=sp||sp<UINT32_C(0x61744d00)||
       slot>=end||4>end-slot) {
        marconi_public_report("MARCONI_PUBLIC_SPMI_WRITE_CALLER matched=false reason=outside-exact-owned-AP-frame "
            "single-ABI-slot-only=true no-raw-stack-register-or-address-output=true");return;
    }
    MemoryRegionSection section=memory_region_find(get_system_memory(),slot,4);
    bool valid=section.mr==&s->handoff_ap_ram&&memory_region_is_ram(section.mr)&&
               !memory_region_is_rom(section.mr)&&int128_ge(section.size,int128_make64(4));
    if(section.mr)memory_region_unref(section.mr);
    if(!valid)return;
    uint8_t raw[4]={0};uint32_t saved=0;
    MemTxAttrs attrs={.secure=false,.user=false};
    MemTxResult result=address_space_read(cpu_get_address_space(CPU(s->armv7m.cpu),ARMASIdx_NS),slot,attrs,raw,sizeof(raw));
    if(result==MEMTX_OK)saved=ldl_le_p(raw);
    o62m_virtual_ao_wipe_v1(raw,sizeof(raw));
    if(result!=MEMTX_OK)return;
    int index=(saved&1u)?o62m_static_spmi_write_caller_index_v1(saved):-1;
    o62m_virtual_ao_wipe_v1(&saved,sizeof(saved));
    if(index>=0)marconi_public_report("MARCONI_PUBLIC_SPMI_WRITE_CALLER matched=true static-caller-index=%d "
        "single-SPplus28-original-push8-ABI-token-only=true no-raw-stack-register-args-address-or-value-output=true",index);
    else marconi_public_report("MARCONI_PUBLIC_SPMI_WRITE_CALLER matched=false reason=no-finite-original-BL-candidate "
        "single-ABI-slot-only=true no-raw-stack-register-args-address-or-value-output=true");
    /* Only the exact original direct return d0c1 (index27) proves the
     * two-register wrapper frame. Its own caller LR is SP+32+4. Never
     * scan the stack or read the intervening saved-R7 word at SP+32. */
    if(index==49) {
        /* Direct e149 return proves the original e114 six-register frame. */
        uint64_t reboot_slot=(uint64_t)sp+52;
        if(sp>UINT32_MAX-52||reboot_slot>=end||4>end-reboot_slot)return;
        MemoryRegionSection reboot_section=memory_region_find(get_system_memory(),reboot_slot,4);
        bool reboot_valid=reboot_section.mr==&s->handoff_ap_ram&&memory_region_is_ram(reboot_section.mr)&&
            !memory_region_is_rom(reboot_section.mr)&&int128_ge(reboot_section.size,int128_make64(4));
        if(reboot_section.mr)memory_region_unref(reboot_section.mr);
        if(!reboot_valid)return;
        uint8_t reboot_raw[4]={0};uint32_t reboot_saved=0;
        result=address_space_read(cpu_get_address_space(CPU(s->armv7m.cpu),ARMASIdx_NS),reboot_slot,attrs,reboot_raw,4);
        if(result==MEMTX_OK)reboot_saved=ldl_le_p(reboot_raw);
        o62m_virtual_ao_wipe_v1(reboot_raw,sizeof(reboot_raw));
        if(result!=MEMTX_OK)return;
        int reboot_caller=reboot_saved==UINT32_C(0x60c521d9)?0:reboot_saved==UINT32_C(0x60e494c1)?1:-1;
        o62m_virtual_ao_wipe_v1(&reboot_saved,sizeof(reboot_saved));
        marconi_public_report("MARCONI_PUBLIC_REBOOT_CALLER static-caller-index=%d "
            "direct-e149-precondition=true exact-SPplus52-public-return-only=true no-args-stack-or-raw-register-output=true",reboot_caller);
        if(reboot_caller==0) {
            /* c52160 push8 and24-byte locals place its LR at nested SP+108. */
            uint64_t board_slot=(uint64_t)sp+108;
            if(sp>UINT32_MAX-108||board_slot>=end||4>end-board_slot)return;
            MemoryRegionSection board_section=memory_region_find(get_system_memory(),board_slot,4);
            bool board_valid=board_section.mr==&s->handoff_ap_ram&&memory_region_is_ram(board_section.mr)&&
                !memory_region_is_rom(board_section.mr)&&int128_ge(board_section.size,int128_make64(4));
            if(board_section.mr)memory_region_unref(board_section.mr);
            if(!board_valid)return;
            uint8_t board_raw[4]={0};uint32_t board_saved=0;
            result=address_space_read(cpu_get_address_space(CPU(s->armv7m.cpu),ARMASIdx_NS),board_slot,attrs,board_raw,4);
            if(result==MEMTX_OK)board_saved=ldl_le_p(board_raw);
            o62m_virtual_ao_wipe_v1(board_raw,sizeof(board_raw));
            if(result!=MEMTX_OK)return;
            int board_caller=board_saved==UINT32_C(0x605c3f95)?0:board_saved==UINT32_C(0x605d93fb)?1:
                board_saved==UINT32_C(0x60c829d3)?2:-1;
            o62m_virtual_ao_wipe_v1(&board_saved,sizeof(board_saved));
            marconi_public_report("MARCONI_PUBLIC_BOARD_RESET_CALLER static-caller-index=%d "
                "exact-prior-two-return-preconditions=true SPplus108-public-return-only=true no-args-stack-or-raw-register-output=true",board_caller);
        }
        return;
    }
    if(index!=27)return;
    uint64_t outer_slot=(uint64_t)sp+36;
    if(sp>UINT32_MAX-36||outer_slot>=end||4>end-outer_slot)return;
    MemoryRegionSection outer_section=memory_region_find(get_system_memory(),outer_slot,4);
    bool outer_valid=outer_section.mr==&s->handoff_ap_ram&&memory_region_is_ram(outer_section.mr)&&
                     !memory_region_is_rom(outer_section.mr)&&int128_ge(outer_section.size,int128_make64(4));
    if(outer_section.mr)memory_region_unref(outer_section.mr);
    if(!outer_valid)return;
    uint8_t outer_raw[4]={0};uint32_t outer_saved=0;
    result=address_space_read(cpu_get_address_space(CPU(s->armv7m.cpu),ARMASIdx_NS),outer_slot,attrs,outer_raw,sizeof(outer_raw));
    if(result==MEMTX_OK)outer_saved=ldl_le_p(outer_raw);
    o62m_virtual_ao_wipe_v1(outer_raw,sizeof(outer_raw));
    if(result!=MEMTX_OK)return;
    int outer_index=(outer_saved&1u)?o62m_static_spmi_wrapper_caller_index_v1(outer_saved):-1;
    o62m_virtual_ao_wipe_v1(&outer_saved,sizeof(outer_saved));
    if(outer_index>=0)marconi_public_report("MARCONI_PUBLIC_SPMI_WRAPPER_CALLER matched=true static-caller-index=%d "
        "prior-direct-wrapper-token-exact=true one-additional-SPplus36-original-push2-ABI-token=true "
        "no-general-unwind-TCB-args-neighbor-or-raw-register-output=true",outer_index);
    else marconi_public_report("MARCONI_PUBLIC_SPMI_WRAPPER_CALLER matched=false reason=no-finite-original-BL-candidate "
        "no-general-unwind-TCB-args-neighbor-or-raw-register-output=true");
}

/* Original MVE memset saves return in R3 before WLSTP/LETP repurposes LR.
 * Classify just that public control-flow word; never inspect loop args/data,
 * vector state, stack, or a TCB, and never output the input word. */
static void marconi_public_memset_caller(MarconiProbeState *s,uint64_t pc,hwaddr address,unsigned size,bool write)
{
    if(pc!=UINT64_C(0x60df1da4)||!s->tee_reset||!s->tee_ap_image||
       !s->virtual_dvs_profile||current_cpu!=CPU(s->armv7m.cpu))return;
    CPUARMState *env=&s->armv7m.cpu->env;
    if(env->v7m.secure||(!arm_v7m_is_handler_mode(env)&&(env->v7m.control[0]&1u)))return;
    bool in_original_uncache=write&&size&&address>=UINT64_C(0x638f4000)&&
        address<UINT64_C(0x63df4000)&&size<=UINT64_C(0x63df4000)-address;
    uint32_t saved_return=env->regs[3];
    int index=(saved_return&1u)?o62m_static_memset_caller_index_v1(saved_return):-1;
    o62m_virtual_ao_wipe_v1(&saved_return,sizeof(saved_return));
    if(index>=0) {
        static const struct { uint32_t base, bytes; } original_mmap[] = {
            {UINT32_C(0x63e00000),UINT32_C(0x200000)}, /* 0 tee */
            {UINT32_C(0x605c0000),UINT32_C(0x3333c00)}, /* 1 ap */
            {UINT32_C(0x64000000),UINT32_C(0xd00000)}, /* 2 CP */
            {UINT32_C(0x2200b800),UINT32_C(0x134800)}, /* 3 sensor */
            {UINT32_C(0x60280000),UINT32_C(0x299000)}, /* 4 dsp */
            {UINT32_C(0x64df0000),UINT32_C(0x10000)}, /* 5 CRF */
            {UINT32_C(0x22000000),UINT32_C(0x400)}, /* 6 CP_boot */
            {UINT32_C(0x60000000),UINT32_C(0x1000)}, /* 7 mmap */
            {UINT32_C(0x63e00000),UINT32_C(0x200000)}, /* 8 tee_f */
            {UINT32_C(0x605c0000),UINT32_C(0x2e34000)}, /* 9 ap_f */
            {UINT32_C(0x64000000),UINT32_C(0xd00000)}, /* 10 CP_f */
            {UINT32_C(0x2200b800),UINT32_C(0x134800)}, /* 11 sensor_f */
            {UINT32_C(0x60280000),UINT32_C(0x299000)}, /* 12 dsp_f */
            {UINT32_C(0x64df0000),UINT32_C(0x10000)}, /* 13 CRF_f */
            {UINT32_C(0x22000000),UINT32_C(0x400)}, /* 14 CPbootf */
            {UINT32_C(0x60001000),UINT32_C(0x23f000)}, /* 15 ap_cp_sm */
            {UINT32_C(0x60240000),UINT32_C(0x40000)}, /* 16 CP_log_buf */
            {UINT32_C(0x60519000),UINT32_C(0x6000)}, /* 17 dsp_log */
            {UINT32_C(0x6051f000),UINT32_C(0xa000)}, /* 18 dsp_dump */
            {UINT32_C(0x60529000),UINT32_C(0x800)}, /* 19 ap_dsp_sm_ucache */
            {UINT32_C(0x60529800),UINT32_C(0x16800)}, /* 20 ap_dsp_sm */
            {UINT32_C(0x60540000),UINT32_C(0x80000)}, /* 21 adsp_ext */
            {UINT32_C(0x638f4000),UINT32_C(0x500000)}, /* 22 ap_uncache */
            {UINT32_C(0x63df4000),UINT32_C(0x4000)}, /* 23 tee_ree_log */
            {UINT32_C(0x63df8000),UINT32_C(0x8000)}, /* 24 tee_ree_sm */
            {UINT32_C(0x64d70000),UINT32_C(0x70000)}, /* 25 ipc_nv_regions */
            {UINT32_C(0x64de0000),UINT32_C(0x10000)}, /* 26 ims_delta_nv */
            {UINT32_C(0x64e00000),UINT32_C(0x200000)}, /* 27 get_data */
            {UINT32_C(0x60000000),UINT32_C(0x4000000)}, /* 28 dump_ap */
            {UINT32_C(0x64000000),UINT32_C(0x1000000)}, /* 29 dump_cp */
            {UINT32_C(0x22000000),UINT32_C(0x5000)}, /* 30 dump_aon_ree */
            {UINT32_C(0x22005000),UINT32_C(0x5000)}, /* 31 dump_aon_tee */
            {UINT32_C(0x2200a000),UINT32_C(0x136000)}, /* 32 dump_aon_sp */
            {UINT32_C(0x22000400),UINT32_C(0x400)}, /* 33 adsp_resume */
            {UINT32_C(0x22003200),UINT32_C(0x600)}, /* 34 dfx_info */
            {UINT32_C(0x22003000),UINT32_C(0x200)}, /* 35 efuse_ram */
            {UINT32_C(0x22003800),UINT32_C(0x400)}, /* 36 mailbox */
            {UINT32_C(0x22003c00),UINT32_C(0x1000)}, /* 37 lps_ree */
            {UINT32_C(0x22004c00),UINT32_C(0x400)}, /* 38 clk_cnt */
            {UINT32_C(0x22000800),UINT32_C(0x2400)}, /* 39 sp_ap_sm */
            {UINT32_C(0x22002c00),UINT32_C(0x400)}, /* 40 cp_reserved */
            {UINT32_C(0x22005000),UINT32_C(0x5000)}, /* 41 lps_tee */
            {UINT32_C(0x2200a000),UINT32_C(0x6000)}, /* 42 sp_ext */
            {UINT32_C(0x21000000),UINT32_C(0x800)}, /* 43 dsp_cp_sm */
            {UINT32_C(0x21000800),UINT32_C(0xc000)}, /* 44 dsp_offload */
            {UINT32_C(0x2100c800),UINT32_C(0x3800)}, /* 45 reserved3 */
            {UINT32_C(0x20000000),UINT32_C(0x2a0000)}, /* 46 gpu_sys */
            {UINT32_C(0x60530000),UINT32_C(0x600000)}, /* 47 ota */
            {UINT32_C(0x0),UINT32_C(0x0)}, /* 48  */
        };
        int mmap_index=-1;uint32_t smallest=UINT32_MAX;unsigned matches=0;
        for(unsigned i=0;i<ARRAY_SIZE(original_mmap);i++) {
            uint64_t base=original_mmap[i].base,limit=base+original_mmap[i].bytes;
            if(write&&size&&address>=base&&address<limit&&size<=limit-address) {
                ++matches;
                if(original_mmap[i].bytes<smallest){smallest=original_mmap[i].bytes;mmap_index=i;}
            }
        }
        marconi_public_report("MARCONI_MEMSET_ORIGINAL_MMAP_CLASS tightest-record-index=%d matching-full-spans=%u still-rejected=true no-address-size-or-data-output=true",mmap_index,matches);
    marconi_public_report("MARCONI_PUBLIC_RAM_FRONTIER original-mmap-ap-uncache-complete-span-match=%u "
        "mapping-enabled=%u exact-memset-write-only=true no-raw-address-size-value-args-or-memory-output=true",
        in_original_uncache,s->ap_uncache_ram_created);
        marconi_public_report("MARCONI_PUBLIC_MEMSET_CALLER matched=true static-caller-index=%d "
            "original-R3-saved-return-only=true LR-loopcounter-not-read=true no-args-stack-memory-vector-or-raw-register-output=true",index);
    }
    else marconi_public_report("MARCONI_PUBLIC_MEMSET_CALLER matched=false reason=no-finite-original-BL-candidate "
        "no-args-stack-memory-vector-or-raw-register-output=true");
}

static uint32_t marconi_counter_ticks(uint64_t ns)
{
    return (uint32_t)((ns / NANOSECONDS_PER_SECOND) * 32768 +
                      ((ns % NANOSECONDS_PER_SECOND) * 32768) /
                      NANOSECONDS_PER_SECOND);
}

static uint64_t marconi_mmio_pc(void);
static MemTxResult marconi_missing(MarconiProbeState *s, hwaddr address,
                                  unsigned size, bool write, uint64_t value)
{
    if (!s->reported) {
        s->reported = true;
        if(marconi_sensitive_guest_data) {
            uint64_t pc=marconi_mmio_pc();
            if(current_cpu==CPU(s->armv7m.cpu)&&pc==UINT64_C(0x60e0ece2)) {
                static const uint32_t pm_banks[]={0x40050000,0x40060000,0x4a000000,0x4a090000,0x4a0c0000,0x4c010000,0x4c030000};
                uint32_t lr=s->armv7m.cpu->env.regs[14]&~1u;
                bool known_caller=lr>=MARCONI_AP_BASE&&lr-MARCONI_AP_BASE<MARCONI_AP_IMAGE_BYTES;
                for(unsigned i=0;i<ARRAY_SIZE(pm_banks);i++)if(address>=pm_banks[i]&&address-pm_banks[i]<0x1000) {
                    marconi_public_report("MARCONI_AP_PM_TARGET address=%08" HWADDR_PRIx " caller-valid=%u original-caller=%08x size=%u write=%u no-data=true",address,known_caller,known_caller?lr:0,size,write);break;
                }
            }
            if(s->cp_created&&current_cpu==CPU(&s->cp_cpu)&&
               ((pc>=UINT64_C(0x64000000)&&pc<UINT64_C(0x644aa000))||
                ((s->cp_atcm_control&1)&&pc<UINT64_C(0x2e4a8)))) {
                static const uint32_t device_banks[]={0x44050000,0x44110000,0x44120000,
                    0x44160000,0x44190000,0x44300000,0x44310000,0x443e0000,0x443f0000,
                    0x4a000000,0x4a090000,0x4a098000,0x4a0c0000,0x4a140000,0x4c010000,0x4c030000,0x4a400000,0x4a500000,0x44140000,0x4a120000,0x4a09c000,0x5a09c000,0x4c120000,0x5c120000,0x4c060000,0x4c070000,0x4a0a0000,0x4a0b0000,0x4a0f0000,0x44340000,0x4a050000,0x44330000,0x44130000,0x4a830000,0x4a840000,0x4a850000,0x4a860000,0x4a870000,0x4a930000,0x4a940000,0x4a950000,0x4a960000};
                for(unsigned i=0;i<ARRAY_SIZE(device_banks);i++)
                    if(address>=device_banks[i]&&address-device_banks[i]<0x1000) {
                        marconi_public_report("MARCONI_CP_DEVICE_BOUNDARY bank-index=%u device-address=%08" HWADDR_PRIx
                            " original-PC=%08" PRIx64 " access=%s size=%u metadata-only=true no-data=true",
                            i,address,pc,write?"write":"read",size);break;
                    }
            }
            if(pc==UINT64_C(0x73e817d0)&&current_cpu==CPU(s->armv7m.cpu)) {
                int host=address==UINT64_C(0x50333300)?0:
                         address==UINT64_C(0x50334300)?1:-1;
                marconi_public_report("MARCONI_TRNG_CONTROL_CLASS host-index=%d write=%u size32=%u "
                    "fill-request=%u clock-enable=%u low-two-only=%u secure=%u context-ready=%u "
                    "static-public-control-fields-only=true no-data-key-or-raw-value-output=true",
                    host,write,size==4,write&&host>=0&&!!(value&1),
                    write&&host>=0&&!!(value&2),write&&host>=0&&!(value&~UINT64_C(3)),
                    s->armv7m.cpu->env.v7m.secure,s->te_context.phase==O62M_TE_CONTEXT_READY_PHASE);
            }
            if(s->gpu_boot_reset_profile&&(pc==0x60e4768e||pc==0x60e476a6)&&current_cpu==CPU(s->armv7m.cpu)) {
                static const uint32_t ports[]={0,4,0x10,0x14,0x20,0x24,0x28,0x30,0x100,0x104,0x500,0x504,0x508};
                static const uint32_t callers[]={0x60e4788e,0x60e4789e,0x60e478e2,0x60e478f6,0x60e4793a,0x60e47980,0x60e47abc,0x60e47b28,0x60e47b94,0x60e47c00,0x60e47cc8,0x60e47cd6,0x60e47cf2,0x60e47d18,0x60e47d3e,0x60e47d64,0x60e47d8a,0x60e47db0,0x60e47dd6,0x60e47dfc,0x60e47e16,0x60e47e2e,0x60e47e42,0x60e47e56,0x60e47e6a,0x60e47e7e,0x60e47e92,0x60e47ea6,0x60e47eb8,0x60e47ecc,0x60e47ee2,0x60e47ef2,0x60e47f02,0x60e47f12,0x60e485cc,0x60e4866e,0x60e48df0,0x60e48f9c,0x60e49096,0x60e48608,0x60e48616,0x60e4864c,0x60e4865a};
                int port=-1,caller=-1;
                for(unsigned i=0;i<G_N_ELEMENTS(ports);i++)if(address==UINT64_C(0x40310000)+ports[i])port=i;
                for(unsigned i=0;i<G_N_ELEMENTS(callers);i++)if(s->armv7m.cpu->env.regs[14]==callers[i]+5)caller=i;
                marconi_public_report("MARCONI_GPU_BOOT_BOUNDARY static-port-index=%d static-caller-index=%d access=%s phase=%u reset-completions=%" PRIu64 " finite-original-public-catalog-only=true no-raw-register-or-memory-output=true",port,caller,write?"write":"read",s->gpu_boot_reset.phase,s->gpu_boot_reset.resets_completed);
            }
            if(pc==UINT64_C(0x60c49c3e)&&s->tee_reset&&s->tee_ap_image&&current_cpu==CPU(s->armv7m.cpu)) {
                static const uint32_t calls[]={0x60c3ab82,0x60c454b6,0x60c45502,0x60c49e8c,
                    0x60dd2084,0x60dd20b6,0x60dd20c2,0x60dd20e8,0x60dd20f4,0x60dd2100,
                    0x60dd210e,0x60dd2122,0x60dd2322};
                CPUARMState *env=&s->armv7m.cpu->env;
                if(!env->v7m.secure&&(arm_v7m_is_handler_mode(env)||!(env->v7m.control[0]&1u))) {
                    int caller=-1;
                    for(unsigned i=0;i<ARRAY_SIZE(calls);i++)if(env->regs[14]==calls[i]+5)caller=i;
                    marconi_public_report("MARCONI_PUBLIC_MASK_HELPER_CALLER static-caller-index=%d "
                        "finite-original-BL-catalog=true LR-public-return-token-only=true no-args-stack-or-raw-register-output=true",caller);
                }
            }
            if(pc==0x60c49bea&&write) {
                static const uint32_t calls[3]={0x60c28bb4,0x60c300f2,0x60c31398};
                int caller=-1;
                for(unsigned i=0;i<3;i++)if(s->armv7m.cpu->env.regs[14]==calls[i]+5)caller=i;
                int instance=address==0x4a030088?0:address==0x4a060088?1:address==0x4a070088?2:-1;
                marconi_public_report("MARCONI_PUBLIC_UART_INIT_CALLER static-caller-index=%d static-instance-index=%d "
                    "finite-original-BL-catalog=true no-raw-register-address-or-payload-output=true",caller,instance);
            }
            if(pc==0x60c393f6&&write&&current_cpu==CPU(s->armv7m.cpu)) {
                marconi_public_report("MARCONI_DPU_F0_WRITE_CLASS target-F0=%u size32=%u old-or-one=%u handler=%u finite-booleans-only=true",
                    address==0x403020f0,size==4,value==(uint64_t)(s->dpu_boot_config[3]|1u),
                    arm_v7m_is_handler_mode(&s->armv7m.cpu->env));
            }
            if(pc==UINT64_C(0x73e621e0)&&!write&&current_cpu==CPU(s->armv7m.cpu)) {
                static const uint32_t calls[]={0x73e62202,0x73e6221c,0x73e6222e,0x73e62240,0x73e6225c,
                    0x73e6236e,0x73e62398,0x73e623c2,0x73e623ec,0x73e6241e};
                int caller=-1,port=-1;
                for(unsigned i=0;i<ARRAY_SIZE(calls);i++)if(s->armv7m.cpu->env.regs[14]==calls[i]+5)caller=i;
                for(unsigned i=0;i<4;i++)if(address==UINT64_C(0x5a0c0000)+4*i)port=i;
                marconi_public_report("MARCONI_TEE_PM_GUARD_BOUNDARY static-caller-index=%d static-port-index=%d size32=%u "
                    "finite-original-BL-and-public-port-catalog=true no-raw-register-address-or-value-output=true",caller,port,size==4);
            }
            if(pc==0x73e60b26&&write&&current_cpu==CPU(s->armv7m.cpu)) {
                static const uint32_t calls[]={0x73e5b0e0,0x73e5f8e0,0x73e5f8ec,0x73e5f8f8,0x73e5f904,0x73e5f910,0x73e5f91c,0x73e5f938,0x73e5f944,0x73e5f9de,0x73e5f9ea,0x73e5f9f6,0x73e5fa02,0x73e5fa0e,0x73e5fa1a,0x73e5fa28,0x73e5fa88,0x73e5fa94,0x73e5faa0,0x73e5faac,0x73e5fab8,0x73e5fac4,0x73e5fad2,0x73e5fb2c,0x73e5fb38,0x73e5fb44,0x73e5fb50,0x73e5fb5c,0x73e5fb68,0x73e5ffd6,0x73e5fffc,0x73e60024,0x73e6003c,0x73e600b0,0x73e600e4,0x73e6011e,0x73e60150,0x73e6136a,0x73e6137a,0x73e6138c,0x73e613ba,0x73e61434,0x73e614fa,0x73e61502,0x73e61508,0x73e6151c,0x73e61538,0x73e61540,0x73e61548,0x73e61550,0x73e61558,0x73e61568};
                static const uint32_t extra_ports[]={0xa14,0x514,0xa68,0x568,0x298,0x2a8,0x780,0xc80,0x430,0x434};
                CPUARMState *env=&s->armv7m.cpu->env;
                uint32_t sp=env->regs[13],saved=0;int caller=-1,port=-1,extra=-1;
                bool frame=false;
                if(env->v7m.secure&&env->regs[14]==0x73e60c7f&&(sp&7)==4&&sp<=UINT32_MAX-28) {
                    uint64_t slot=(uint64_t)sp+24,offset=UINT64_MAX;
                    if(slot>=MARCONI_TEE_ALIAS+770048&&slot<=MARCONI_TEE_ALIAS+MARCONI_TEE_BYTES-4)offset=slot-MARCONI_TEE_ALIAS;
                    else if(slot>=MARCONI_TEE_BASE+770048&&slot<=MARCONI_TEE_BASE+MARCONI_TEE_BYTES-4)offset=slot-MARCONI_TEE_BASE;
                    if(offset!=UINT64_MAX) {
                        saved=ldl_le_p((const uint8_t *)memory_region_get_ram_ptr(&s->ap_ram)+offset);frame=true;
                        for(unsigned i=0;i<ARRAY_SIZE(calls);i++)if(saved==calls[i]+5)caller=i;
                    }
                }
                o62m_virtual_ao_wipe_v1(&saved,sizeof(saved));
                uint64_t reg=UINT64_MAX;
                if(address>=0x5a150000&&address<=0x5a15ffff)reg=address-0x5a150000;
                else if(address>=0x4a150000&&address<=0x4a15ffff)reg=address-0x4a150000;
                if(reg<=UINT32_MAX) {
                    port=o62m_static_pmic_public_port_index_v2((uint32_t)reg);
                    for(unsigned i=0;i<ARRAY_SIZE(extra_ports);i++)if(reg==extra_ports[i])extra=i;
                }
                marconi_public_report("MARCONI_TEE_SPMI_WRITE_CLASS caller-index=%d public-port-index=%d extra-static-port-index=%d "
                    "exact-ABI-frame=%u size32=%u standard-write-packet=%u selected=%u select-pending=%u write-pending=%u "
                    "single-return-token-only=true no-raw-stack-register-address-value-or-payload-output=true",
                    caller,port,extra,frame,size==4,(value&UINT64_C(0xffff0000))==UINT64_C(0x00180000),
                    s->slave_selected,s->select_pending,s->pmic_write_pending);
            }
            if(s->cp_created&&current_cpu==CPU(&s->cp_cpu)&&pc==0x6400873a&&write) {
                bool range=address>=0x4a150000&&address<=0x4a15ffff;
                int port=range?o62m_static_pmic_public_port_index_v2(address-0x4a150000):-1;
                uint32_t caller=s->cp_cpu.env.regs[14]&~UINT32_C(1);
                bool original=caller>=0x64000000&&caller<0x644aa000;
                bool tcm=(s->cp_atcm_control&1)&&caller<0x2e4a8;
                if(range)marconi_public_report("MARCONI_CP_SPMI_TARGET reg=%04x validated-caller=%u caller-PC=%08x static-copy-source-PC=%08x protocol-metadata-only=true no-payload=true",
                    (unsigned)(address-0x4a150000),original||tcm,(original||tcm)?caller:0,tcm?0x644147c0+caller:0);
                marconi_public_report("MARCONI_CP_SPMI_EARLY_BOUNDARY port-in-command-window=%u public-port-index=%d size32=%u controller-gates=%u selected=%u select-pending=%u write-pending=%u standard-slave8-packet=%u known-0504=%u known-0a04=%u no-address-or-payload-output=true",
                    range,port,size==4,(s->ctrl54&0x10)&&(s->spmi_ctrl&0x44)==0x44,
                    s->slave_selected,s->select_pending,s->pmic_write_pending,
                    (value&UINT64_C(0xffff0000))==UINT64_C(0x00180000),
                    address==0x4a150504,address==0x4a150a04);
            }
            marconi_public_c894_caller(s,pc);
            marconi_public_spmi_write_caller(s,pc);
            marconi_public_memset_caller(s,pc,address,size,write);
            if(s->pmic_alias_read_policy)marconi_public_report("MARCONI_PMIC_SAFE_COUNTERS total-canonical-writes=%" PRIu64 " alias-writes=%" PRIu64 " canonical68-writes=%" PRIu64 " ADC-started=%" PRIu64 " ADC-completed=%" PRIu64 " ADC-cancelled=%" PRIu64 " host-model-counts-only=true no-data-values=true",s->pmic_regulators.writes,s->pmic_regulators.alias_writes,s->pmic_regulators.word_writes[2],s->pmic_adc.started,s->pmic_adc.completed,s->pmic_adc.cancelled);
            /* Match only finite original typed PUBLIC PMIC ports; do not
             * print an arbitrary address, requested value or CPU register. */
            if(s->virtual_dvs_profile&&(pc==0x60c4c8e4||pc==0x60c4d0a6)&&address>=0x4a150000&&address<=0x4a15ffff) {
                int port=o62m_static_pmic_public_port_index_v2(address-0x4a150000);
                if(port>=0)marconi_public_report("MARCONI_PUBLIC_PMIC_PORT matched=true access=%s static-port-index=%d static-constant-catalog-only=true no-raw-address-register-stack-or-PMIC-data-output=true",write?"write":"read",port);
                else marconi_public_report("MARCONI_PUBLIC_PMIC_PORT matched=false access=%s no-raw-address-register-stack-or-PMIC-data-output=true",write?"write":"read");
            }
            if(s->cp_created&&current_cpu==CPU(&s->cp_cpu)&&(s->cp_atcm_control&1)&&pc<UINT64_C(0x2e4a8)) {
                marconi_public_report("MARCONI_PRIVATE_GUEST_BOUNDARY stage=original-CP-TCM pc=%08" PRIx64 " static-copy-source-PC=%08" PRIx64 " access=%s static-origin-association-only=true address-and-value-redacted=true",pc,UINT64_C(0x644147c0)+pc,write?"write":"read");
            } else if(s->cp_created&&current_cpu==CPU(&s->cp_cpu)&&((pc<444u)||(pc>=UINT64_C(0x64000000)&&pc-UINT64_C(0x64000000)<UINT64_C(4890624)))) {
                marconi_public_report("MARCONI_PRIVATE_GUEST_BOUNDARY stage=original-CP pc=%08" PRIx64 " access=%s address-and-value-redacted=true",pc,write?"write":"read");
            } else if((pc>=MARCONI_TEE_ALIAS&&pc-MARCONI_TEE_ALIAS<UINT64_C(770048))||
               (pc>=MARCONI_AP_BASE&&pc-MARCONI_AP_BASE<MARCONI_AP_IMAGE_BYTES)) {
                marconi_public_report("MARCONI_PRIVATE_GUEST_BOUNDARY stage=%s pc=%08" PRIx64
                                      " access=%s address-and-value-redacted=true",
                                      pc>=MARCONI_TEE_ALIAS?"original-TEE":"original-main-AP",pc,write?"write":"read");
            } else {
                if(current_cpu==CPU(s->armv7m.cpu)&&pc>=0x32006268&&pc<=0x320062a8) {
                    uint32_t lr=s->armv7m.cpu->env.regs[14];int caller=-1;
                    static const uint32_t psram_post_lrs[]={0x73e5993b,0x73e59949,0x73e598b3,0x73e59951,
                        0x73e598d7,0x73e59959,0x73e59805,0x73e5995d,0x73e59849,0x73e59961,
                        0x73e59ab1,0x73e5975d,0x73e59ab7};
                    static const uint32_t psram_post_ports[]={0x4a910780,0x5a9b0004,0x5a9b0008,
                        0x5a9b000c,0x5a9b0010,0x5a9b0014,0x5a9b0018,0x5a9b0000,0x5a9b002c,
                        0x5a000008,0x5a000100,0x5a000104};
                    int post_call=-1,post_port=-1;
                    for(unsigned i=0;i<ARRAY_SIZE(psram_post_lrs);++i)if(lr==psram_post_lrs[i])post_call=i;
                    for(unsigned i=0;i<ARRAY_SIZE(psram_post_ports);++i)if(address==psram_post_ports[i])post_port=i;
                    marconi_public_report("MARCONI_PSRAM_POST_BOUNDARY caller-class=%d port-class=%d "
                        "size32=%u write=%u dfs-phase16=%u spmi-select-pending=%u spmi-write-pending=%u "
                        "pubcp-780-old-or1000=%u acceptance-unchanged=true no-raw-data=true",
                        post_call,post_port,size==4,write,s->dfs_config.phase==16,s->select_pending,
                        s->pmic_write_pending,address==0x4a910780&&write&&value==(s->pubcp_dfs_control|UINT32_C(0x1000)));

                    for(unsigned i=0;i<ARRAY_SIZE(marconi_lps_mmio_calls_v2);++i)
                        if(lr==marconi_lps_mmio_calls_v2[i]+5)caller=i;
                    static const uint32_t known_ports[]={0x4c0101ac,0x4a000090,0x4a0c0030,0x4a0c0034,
                        0x4a0c00a8,0x4a0c00ac,0x4a0c00b0,0x4a0c000c,0x4a0c001c,0x5a000008,
                        0x4a09c414,0x4a820024,0x4a8003fc,0x4a92001c,0x4a9003fc};
                    int port=-1;for(unsigned i=0;i<ARRAY_SIZE(known_ports);++i)if(address==known_ports[i])port=i;
                    marconi_public_report("MARCONI_LPS_STATIC_PORT_CLASS index=%d raw-address-redacted=true",port);
                    marconi_public_report("MARCONI_LPS_MMIO_CALL_CLASS caller-index=%d "
                        "leaf-instruction-offset=%u write=%u size32=%u original-static-catalog=true "
                        "raw-PC-LR-address-and-payload-redacted=true",caller,(unsigned)(pc-0x32006268),write,size==4);
                }
                static const uint32_t ports[]={0x4a0c000c,0x5a0c000c,0x4a0c011c,0x5a0c011c,
                    0x4a0c021c,0x5a0c021c,0x4a0c0030,0x5a0c0030,0x4a0c0034,0x5a0c0034,
                    0x5a0c0014,0x4a0c0014};
                int port=-1;for(unsigned i=0;i<ARRAY_SIZE(ports);++i)if(address==ports[i])port=i;
                bool tee_physical=pc>=MARCONI_TEE_BASE&&pc-MARCONI_TEE_BASE<UINT64_C(770048);
                marconi_public_report("MARCONI_UNKNOWN_BOUNDARY_CLASS CPU-present=%u expected-CPU=%u "
                    "TEE-physical-code=%u zero-PC=%u static-PM-port-index=%d write=%u size32=%u "
                    "code-values-and-payload-redacted=true acceptance-unchanged=true",
                    current_cpu!=NULL,current_cpu==CPU(s->armv7m.cpu),tee_physical,pc==0,port,write,size==4);
                if(tee_physical)marconi_public_report("MARCONI_ORIGINAL_TEE_PHYSICAL_CODE canonical-ROM-PC=%08" PRIx64
                    " original-physical-code-view=true no-unknown-code-or-data-output=true",pc-MARCONI_TEE_BASE+MARCONI_TEE_ALIAS);
                marconi_public_report("MARCONI_PRIVATE_GUEST_BOUNDARY code-PC-address-and-value-redacted=true");
            }
        }
        /* Read-only observation of existing original-TEE-owned shared RAM.
         * No snapshot bytes are written back or returned as MMIO responses. */
        if(s->tee_reset&&!marconi_sensitive_guest_data) {
            const uint8_t *footer=(const uint8_t *)memory_region_get_ram_ptr(&s->aon_ram[3])+0x3f0;
            char hex[33];
            for(unsigned i=0;i<16;i++) {snprintf(hex+2*i,3,"%02x",footer[i]);}
            g_autofree gchar *digest=g_compute_checksum_for_data(G_CHECKSUM_SHA256,footer,16);
            error_report("MARCONI_CLKCNT_FOOTER_SNAPSHOT physical=22004ff0 bytes=16 hex=%s "
                         "sha256=%s read-only-RAM-observation=true no-host-footer-injection=true "
                         "ADC-started=%" PRIu64 " ADC-completed=%" PRIu64 " ADC-acknowledged=%" PRIu64
                         " ADC-ch3-reads=%" PRIu64 " ADC-ch4-reads=%" PRIu64,
                         hex,digest,s->pmic_adc.started,s->pmic_adc.completed,
                         s->pmic_adc.acknowledged,s->pmic_adc.sample_reads[0],s->pmic_adc.sample_reads[1]);
            error_report("MARCONI_HWLOCK_STATE_SNAPSHOT seed=%08x config=%08x owned=%08x external=%08x "
                         "asserted=%u complete=%u assert-ns=%" PRId64 " release-ns=%" PRId64 " "
                         "reset-count=%" PRIu64 " acquisitions=%" PRIu64 " conflicts=%" PRIu64 " releases=%" PRIu64 " "
                         "capture-ns=%" PRId64 " read-only-modelstate-observation=true",
                         s->hwlock.config_seed,s->hwlock.config,s->hwlock.owned,s->hwlock.external_owned,
                         s->hwlock.reset_asserted,s->hwlock.reset_complete,s->hwlock.asserted_ns,s->hwlock.released_ns,
                         s->hwlock.reset_count,s->hwlock.acquisitions,s->hwlock.conflicts,s->hwlock.releases,
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        }
        if(s->ota_reset&&!marconi_sensitive_guest_data&&current_cpu==CPU(s->armv7m.cpu)) {
            uint64_t sp=s->armv7m.cpu->env.regs[13];
            if(sp>=MARCONI_OTA_BASE&&sp+64<=((uint64_t)MARCONI_OTA_BASE+MARCONI_OTA_BYTES)) {
                const uint8_t *ram=(const uint8_t *)memory_region_get_ram_ptr(&s->ap_ram);
                for(unsigned i=0;i<16;i++) {
                    error_report("MARCONI_OTA_STACK_SNAPSHOT address=%08" PRIx64 " word=%08x "
                                 "read-only-owned-RAM-observation=true no-guest-state-change=true",
                                 sp+4*i,ldl_le_p(ram+(sp-MARCONI_OTA_BASE)+4*i));
                }
            }
        }
        error_report("MARCONI_PROBE_UNIMPLEMENTED access=%s address=0x%08"
                     HWADDR_PRIx " size=%u value=0x%016" PRIx64
                     " pc=0x%08" PRIx64
                     " transaction=MEMTX_ERROR stop=first-unimplemented-access",
                     write ? "write" : "read", address, size, value,
                     current_cpu ? CPU_GET_CLASS(current_cpu)->get_pc(current_cpu)
                                 : UINT64_MAX);
        error_report("MARCONI_PROBE_STATE hwlock-reset-count=%" PRIu64
                     " hwlock-reset-complete=%u hwlock-owned=0x%08x "
                     "adc-started=%" PRIu64 " adc-completed=%" PRIu64
                     " adc-acknowledged=%" PRIu64 " adc-ch3-reads=%" PRIu64
                     " adc-ch4-reads=%" PRIu64 " adc-result-channel=%u "
                     "adc-result=%u qos-conditional-zero-writes=%" PRIu64,
                     s->hwlock.reset_count, s->hwlock.reset_complete,
                     s->hwlock.owned, s->pmic_adc.started, s->pmic_adc.completed,
                     s->pmic_adc.acknowledged, s->pmic_adc.sample_reads[0],
                     s->pmic_adc.sample_reads[1], s->pmic_adc.result_channel,
                     s->pmic_adc.result, s->qos_a8_zero_writes);
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC,
                                              MARCONI_PROBE_EXIT);
        if (current_cpu) {
            cpu_dump_state(current_cpu, stderr, 0);
            cpu_exit(current_cpu);
        }
    }
    return MEMTX_ERROR;
}

/* Experimental environment: opaque reset seed is NOT a recovered silicon
 * reset value. Writable fields follow the unmodified SBL helpers; unknown
 * bits must survive. The selected latency is a virtual test parameter.
 * Only supported ADC register semantics supply PMIC data; unknown identity
 * and other registers still fail. This is an explicit hypothesis used to find the next access. */
/* One canonical regulator68 word also backs the existing ADC power mirror.
 * Apply to copies first: counter/field/ADC errors cannot publish partial state.
 * Only the previously modeled exactADC40 command may cancel its conversion.
 * Other enable fields retain data without inventing rail-ready or voltfeedback. */
static void marconi_cp_timer_stop(MarconiProbeState *s);
static void marconi_cp_timer_reset(void *opaque);
static bool marconi_cp_timer_refresh(MarconiProbeState *s);
static bool marconi_cp_wdt_refresh(MarconiProbeState *s);
static void marconi_bbpll_source_changed(MarconiProbeState *s);
static void marconi_rfspim_cancel(MarconiProbeState *s);
static void marconi_rfspim_source_changed(MarconiProbeState *s);
static void marconi_rfspim_service_at(MarconiProbeState *s,int64_t now);
static bool marconi_rfspim_clock_before_change(MarconiProbeState *s);
static O62MPmicRegulatorResultV1 marconi_pmic_regulator_apply(
    O62MPmicRegulatorsV1 *regulators, O62MPmicAdc *adc,
    uint32_t reg, uint32_t value, int64_t now)
{
    O62MPmicRegulatorsV1 next=*regulators;
    O62MPmicAdc next_adc=*adc;
    uint16_t old68,new68;
    O62MPmicRegulatorResultV1 result=o62m_pmic_regulator_read_v1(regulators,0x68,&old68);
    if(result) {return result;}
    assert(old68==adc->control_68);
    result=o62m_pmic_regulator_write_v1(&next,reg,value);
    if(result) {return result;}
    result=o62m_pmic_regulator_read_v1(&next,0x68,&new68);
    if(result) {return result;}
    if((reg==0x568||reg==0xa68)&&value==0x40) {
        if(!o62m_pmic_adc_write(&next_adc,reg,value,now)) {
            return O62M_PMIC_REGULATOR_V1_BAD_ARGUMENT;
        }
        assert(next_adc.control_68==new68);
    } else {
        next_adc.control_68=new68;
    }
    *regulators=next;
    *adc=next_adc;
    return O62M_PMIC_REGULATOR_V1_OK;
}

/* Opt-in conditioned OS entropy endpoint. No private material, digest or
 * entropy bytes are mapped, returned to firmware, snapshotted or logged. */
static bool marconi_virtual_ao_boundary(MarconiProbeState *s,const char *reason)
{
    if(s->trng_virtual_event) {timer_del(s->trng_virtual_event);}
    if(!s->reported) {
        s->reported=true;
        error_report("MARCONI_VIRTUAL_AO_BOUNDARY reason=%s no-invented-hardware-error-bit=true "
                     "normal-boot-not-accepted=true",reason);
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC,MARCONI_PROBE_EXIT);
        if(current_cpu) {cpu_dump_state(current_cpu,stderr,0);cpu_exit(current_cpu);}
    }
    return false;
}

static bool marconi_virtual_ao_refresh(MarconiProbeState *s)
{
    if(!s->trng_virtual_profile) {return true;}
    if(s->reported) {return false;}
    O62MVirtualAoMetadataV1 metadata;
    O62MVirtualAoResultV1 result=o62m_virtual_ao_query_v1(&s->trng_virtual,&metadata);
    if(result) {return marconi_virtual_ao_boundary(s,o62m_virtual_ao_reason_v1(result));}
    if(metadata.phase!=O62M_VAO_BUSY_V1) {return true;}
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    O62MVirtualAoJobV1 job;bool due;
    result=o62m_virtual_ao_job_due_v1(&s->trng_virtual,now,&job,&due);
    if(result) {return marconi_virtual_ao_boundary(s,o62m_virtual_ao_reason_v1(result));}
    if(!due) {timer_mod_ns(s->trng_virtual_event,job.deadline_ns);return true;}
    uint32_t gate;
    if(s->te_context.phase!=O62M_TE_CONTEXT_READY_PHASE||
       o62m_clock_controls_read(&s->board_controls,0x50050018,4,&gate)!=O62M_CLOCK_CONTROLS_OK||!(gate&2)) {
        (void)o62m_virtual_ao_fail_v1(&s->trng_virtual,now,&job);
        return marconi_virtual_ao_boundary(s,"work-clock-or-context-unavailable");
    }
    /* This is actual source work. Time becoming due alone cannot set VALID.
     * Every path after acquiring staging wipes the whole staging allocation. */
    uint8_t material[O62M_VAO_MAX_BYTES_V1]={0};
    Error *source_error=NULL;
    int source_result=qcrypto_random_bytes(material,job.bytes,&source_error);
    bool source_ok=source_result==0&&source_error==NULL;
    result=o62m_virtual_ao_complete_v1(&s->trng_virtual,now,&job,material,
                                    source_ok?job.bytes:0,source_ok);
    o62m_virtual_ao_wipe_v1(material,sizeof(material));
    error_free(source_error);
    timer_del(s->trng_virtual_event);
    if(result!=O62M_VAO_OK_V1) {
        return marconi_virtual_ao_boundary(s,o62m_virtual_ao_reason_v1(result));
    }
    result=o62m_virtual_ao_query_v1(&s->trng_virtual,&metadata);
    if(result||metadata.phase!=O62M_VAO_VALID_V1||!metadata.material_present||metadata.jobs_completed!=1) {
        return marconi_virtual_ao_boundary(s,"source-completion-without-valid-private-store");
    }
    error_report("MARCONI_VIRTUAL_AO_MATERIAL_COMMIT job=%" PRIu64 " key-bytes=%u source-bytes=%" PRIu64
                 " virtual-ns=%" PRId64 " actual-OS-CSPRNG-success=true private-unmapped-store-present=true "
                 "staging-wiped=true no-key-or-hash-output=true physical-conditioning-length-and-timing-not-claimed=true",
                 job.id,metadata.key_bytes,metadata.provider_reported_bytes,now);
    return true;
}

static void marconi_virtual_ao_callback(void *opaque)
{ (void)marconi_virtual_ao_refresh(opaque); }

static void marconi_virtual_ao_exit(Notifier *notify,void *opaque)
{
    MarconiProbeState *s=container_of(notify,MarconiProbeState,trng_virtual_exit);
    (void)opaque;
    if(s->trng_virtual_event) {timer_del(s->trng_virtual_event);}
    (void)o62m_virtual_ao_destroy_v1(&s->trng_virtual);
    bool wiped=o62m_virtual_ao_zero_v1((const uint8_t *)&s->trng_virtual,sizeof(s->trng_virtual));
    marconi_public_report("MARCONI_VIRTUAL_AO_EXIT_WIPE private-host-state-zeroized=%s no-material-output=true",wiped?"true":"false");
    assert(wiped);
    s->trng_virtual_exit_registered=false;
}

/* Explicit virtual autonomous AO power-on work; no poll-triggered source,
 * controller programming BUSY, guest key mapping or physical timing claim. */
static bool marconi_virtual_otp_boundary(MarconiProbeState *s,const char *reason)
{
    if(s->virtual_otp_event) {timer_del(s->virtual_otp_event);}
    if(!s->reported) {
        s->reported=true;
        error_report("MARCONI_VIRTUAL_OTP_BOUNDARY reason=%s private-material-not-output=true normal-boot-not-accepted=true",reason);
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC,MARCONI_PROBE_EXIT);
        if(current_cpu) {cpu_dump_state(current_cpu,stderr,0);cpu_exit(current_cpu);}
    }
    return false;
}
static bool marconi_virtual_otp_refresh(MarconiProbeState *s)
{
    if(!s->virtual_otp_profile) {return true;}
    if(s->reported) {return false;}
    O62MVirtualOtpJobV1 job;bool due;
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int result=o62m_virtual_otp_job_v1(&s->virtual_otp,now,&job,&due);
    if(result) {return marconi_virtual_otp_boundary(s,"invalid-power-on-job-state");}
    if(s->virtual_otp.phase!=O62M_VOTP_LOADING_V1) {return true;}
    if(!due) {timer_mod_ns(s->virtual_otp_event,job.due_ns);return true;}
    uint8_t material[64]={0};Error *first_error=NULL,*second_error=NULL;
    unsigned key_bytes=s->virtual_otp.key_bytes;
    int first=qcrypto_random_bytes(material,key_bytes,&first_error);
    bool source_ok=first==0&&first_error==NULL;
    if(source_ok) {
        int second=qcrypto_random_bytes(material+key_bytes,key_bytes,&second_error);
        source_ok=second==0&&second_error==NULL;
    }
    result=o62m_virtual_otp_complete_v1(&s->virtual_otp,now,&job,material,source_ok?2u*key_bytes:0,source_ok);
    o62m_virtual_otp_wipe_v1(material,sizeof material);
    error_free(first_error);error_free(second_error);timer_del(s->virtual_otp_event);
    if(result) {return marconi_virtual_otp_boundary(s,"actual-key-source-or-image-shadow-load-failed");}
    O62MVirtualOtpMetadataV1 meta;
    result=o62m_virtual_otp_query_v1(&s->virtual_otp,&meta);
    if(result||!meta.backing_present||meta.shadow_valid!=63||meta.phase!=O62M_VOTP_VALID_V1) {
        return marconi_virtual_otp_boundary(s,"source-complete-without-real-backing-and-six-copied-shadows");
    }
    error_report("MARCONI_VIRTUAL_OTP_COMMIT image-bytes=%u key-bytes=%u "
                 "virtual-ns=%" PRId64 " actual-OS-source-calls=2 actual-private-backing=true "
                 "actual-six-typed-shadow-copies=true staging-wiped=true no-key-or-hash-output=true "
                 "synthetic-public-CM-image=true physical-fuses-reset-clock-and-timing-not-claimed=true",
                 meta.image_bytes,meta.key_bytes,now);
    return true;
}
static void marconi_virtual_otp_callback(void *opaque)
{ (void)marconi_virtual_otp_refresh(opaque); }
static void marconi_virtual_otp_exit(Notifier *notify,void *opaque)
{
    MarconiProbeState *s=container_of(notify,MarconiProbeState,virtual_otp_exit);(void)opaque;
    if(s->virtual_otp_event) {timer_del(s->virtual_otp_event);}
    o62m_virtual_otp_destroy_v1(&s->virtual_otp);
    bool wiped=o62m_virtual_otp_zero_v1(&s->virtual_otp,sizeof(s->virtual_otp));
    marconi_public_report("MARCONI_VIRTUAL_OTP_EXIT_WIPE private-host-backing-and-shadows-zeroized=%s no-material-output=true",wiped?"true":"false");
    assert(wiped);s->virtual_otp_exit_registered=false;
}

static void marconi_owned_runtime_failure_stop(MarconiProbeState *s);

static bool marconi_aca_source_available(MarconiProbeState *s)
{
    uint32_t gate;
    return s->te_context.phase==O62M_TE_CONTEXT_READY_PHASE&&(s->aca_config_word[0]&0x400u)&&
        o62m_clock_controls_read(&s->board_controls,UINT64_C(0x50050018),4,&gate)==O62M_CLOCK_CONTROLS_OK&&(gate&2);
}
static void marconi_otp_program_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,otp_program_exit);
    uint64_t commits=s->otp_program.commits,clears=s->otp_program.clears;
    uint64_t control_reads=s->otp_program.control_reads,control_writes=s->otp_program.control_writes;
    volatile uint8_t *p=(volatile uint8_t *)&s->otp_read;
    for(size_t i=0;i<sizeof(s->otp_read);i++)p[i]=0;
    p=(volatile uint8_t *)&s->otp_program;
    for(size_t i=0;i<sizeof(s->otp_program);i++)p[i]=0;
    marconi_public_report("MARCONI_BOARD_OTP_EXIT commits=%" PRIu64 " clears=%" PRIu64 " control-reads=%" PRIu64 " control-writes=%" PRIu64 " private-owner-wiped=true no-data-output=true",commits,clears,control_reads,control_writes);
}
static void marconi_aca_cancel(MarconiProbeState *s)
{
    if(s->aca_work_event)timer_del(s->aca_work_event);
    o62m_aca_op_cancel_v12(&s->aca_sram);s->aca_work_armed=0;
}
static void marconi_aca_work_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->reported){marconi_aca_cancel(s);return;}
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(!marconi_aca_source_available(s)||!(s->aca_config_word[0]&1u)||now<0||
       !s->aca_sram.running||s->aca_sram.generation!=s->aca_work_armed) {
        marconi_aca_cancel(s);return;
    }
    if((uint64_t)now<s->aca_sram.deadline){timer_mod_ns(s->aca_work_event,s->aca_sram.deadline);return;}
    unsigned opcode=s->aca_sram.opcode;bool save_result=s->aca_sram.save_result;
    if(!o62m_aca_op_complete_v12(&s->aca_sram,s->aca_work_armed,(uint64_t)now)) {
        marconi_aca_cancel(s);marconi_owned_runtime_failure_stop(s);return;
    }
    marconi_public_report("MARCONI_ACA_OPERATION_COMPLETE opcode=%u completions=%" PRIu64
        " virtual-ns=%" PRId64 " actual-SRAM-calculation=true result-writeback=%u no-operands-or-result-output=true",
        opcode,s->aca_sram.completions,now,save_result);
}
static void marconi_aca_sram_reset(void *opaque)
{
    MarconiProbeState *s=opaque;
    marconi_aca_cancel(s);
    (void)o62m_aca_sram_init_v12(&s->aca_sram,O62M_ACA_SRAM_BYTES_V12);
}
static void marconi_aca_sram_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,aca_sram_exit);(void)opaque;
    uint64_t addresses=s->aca_sram.address_writes,writes=s->aca_sram.data_writes,reads=s->aca_sram.data_reads;
    uint64_t config_reads=s->aca_sram.config_reads,config_writes=s->aca_sram.config_writes,status_reads=s->aca_sram.status_reads;
    uint64_t requests=s->aca_sram.requests,completions=s->aca_sram.completions,writeback_words=s->aca_sram.writeback_words;
    marconi_aca_cancel(s);o62m_aca_sram_wipe_v12(&s->aca_sram);
    bool wiped=o62m_virtual_otp_zero_v1(&s->aca_sram,sizeof s->aca_sram);
    marconi_public_report("MARCONI_ACA_SRAM_EXIT address-writes=%" PRIu64 " data-writes=%" PRIu64
        " data-reads=%" PRIu64 " config-reads=%" PRIu64 " config-writes=%" PRIu64
        " status-reads=%" PRIu64 " requests=%" PRIu64 " completions=%" PRIu64
        " writeback-words=%" PRIu64 " private-backing-wiped=%u timer-cancelled=true no-address-or-operand-output=true",addresses,writes,reads,config_reads,config_writes,status_reads,requests,completions,writeback_words,wiped);
    assert(wiped);
}
static bool marconi_hash_boundary(MarconiProbeState *s,const char *reason)
{
    if(s->hash_init_event)timer_del(s->hash_init_event);
    o62m_hmac_sha256_destroy_v1(&s->hash_context);
    o62m_sha256_zeroize_v1(s->hash_key_input,sizeof s->hash_key_input);
    o62m_sha256_zeroize_v1(s->hash_inner_digest,sizeof s->hash_inner_digest);
    if(!s->reported) {
        s->reported=true;
        marconi_public_report("MARCONI_HASH_CRYPTO_BOUNDARY reason=%s no-private-key-or-digest-output=true no-success-completion=true",reason);
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC,MARCONI_PROBE_EXIT);
        if(current_cpu)cpu_exit(current_cpu);
    }
    return false;
}
/* Currently proven secure source role only: same existing TEE RAM owner and
 * its original alias. No MMIO/DMA-to-private-model mapping is permitted. */
static bool marconi_hash_read_tee(MarconiProbeState *s,uint64_t address,uint8_t *out,size_t bytes)
{
    if(!s->tee_reset||!out||bytes>MARCONI_TEE_BYTES)return false;
    uint64_t offset;
    if(address>=MARCONI_TEE_ALIAS&&address-MARCONI_TEE_ALIAS<=MARCONI_TEE_BYTES-bytes)offset=address-MARCONI_TEE_ALIAS;
    else if(address>=MARCONI_TEE_BASE&&address-MARCONI_TEE_BASE<=MARCONI_TEE_BYTES-bytes)offset=address-MARCONI_TEE_BASE;
    else return false;
    memcpy(out,(const uint8_t *)memory_region_get_ram_ptr(&s->ap_ram)+offset,bytes);return true;
}
static bool marconi_hash_stream_read(void *opaque,uint64_t address,uint8_t *out,size_t bytes,bool descriptor)
{
    MarconiProbeState *s=opaque;MemoryRegion *owner=NULL;uint64_t offset=0;
    const uint64_t base[]={MARCONI_TEE_BASE,MARCONI_TEE_ALIAS,0x64000000,0x74000000,0x64df0000,0x74df0000};
    const uint64_t size[]={MARCONI_TEE_BYTES,MARCONI_TEE_BYTES,0xd00000,0xd00000,0x10000,0x10000};
    MemoryRegion *regions[]={&s->ap_ram,&s->ap_ram,&s->cp_image_ram,&s->cp_image_ram,&s->cp_crf_ram,&s->cp_crf_ram};
    for(unsigned i=0;i<(descriptor?2u:6u);i++) {
        if((i<2?s->tee_reset:s->cp_image_ram_created)&&bytes<=size[i]&&
           address>=base[i]&&address-base[i]<=size[i]-bytes) {owner=regions[i];offset=address-base[i];break;}
    }
    if(!owner||!bytes)return false;
    if(out)memcpy(out,(const uint8_t *)memory_region_get_ram_ptr(owner)+offset,bytes);
    return true;
}
static bool marconi_hash_init_refresh(MarconiProbeState *s)
{
    if(!s->hash_init_profile||!s->hash_job_queued)return true;
    if(s->reported)return false;
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(now<s->hash_deadline_ns){timer_mod_ns(s->hash_init_event,s->hash_deadline_ns);return true;}
    uint32_t *frame=s->symc_empty.host[1].cq;
    if(frame[0]==UINT32_C(0xff000001)) {
        if(s->symc_empty.host[1].cq_words!=1)return marconi_hash_boundary(s,"CLEAR-invalid-frame");
        o62m_hmac_sha256_destroy_v1(&s->hash_context);o62m_sha256_zeroize_v1(s->hash_key_input,sizeof s->hash_key_input);
        o62m_sha256_zeroize_v1(s->hash_inner_digest,sizeof s->hash_inner_digest);
        s->hash_inner_done=false;s->hash_finished=false;
        if(!o62m_virtual_otp_zero_v1(&s->hash_context,sizeof s->hash_context)||
           !o62m_virtual_otp_zero_v1(s->hash_key_input,sizeof s->hash_key_input)||
           !o62m_virtual_otp_zero_v1(s->hash_inner_digest,sizeof s->hash_inner_digest)||
           !o62m_hash_work_complete_v7(&s->symc_empty,UINT32_C(0xff000001),7))
            return marconi_hash_boundary(s,"actual-CLEAR-zeroize-or-publication-failed");
        s->hash_job_queued=false;timer_del(s->hash_init_event);
        marconi_public_report("MARCONI_HASH_CLEAR_COMPLETE actual-private-context-zeroized=true CSQ-code=7 status=0 slot=0");return true;
    }
    if(frame[0]==UINT32_C(0x2000003f)) {
        if(s->symc_empty.host[1].cq_words!=(s->hash_is_hmac?7u:3u)||!s->hash_context.active||!s->hash_inner_done||s->hash_finished)
            return marconi_hash_boundary(s,"FINISH-without-real-padded-HMAC-inner-or-duplicate");
        uint64_t iv_addr=((uint64_t)frame[2]<<32)|frame[1],key_addr=((uint64_t)frame[4]<<32)|frame[3];
        uint64_t out_addr=s->hash_is_hmac?(((uint64_t)frame[6]<<32)|frame[5]):iv_addr;
        uint8_t iv[32]={0},key_input[128]={0},digest[32]={0};uint64_t offset=UINT64_MAX;
        bool ok=true;
        if(s->hash_is_hmac)ok=marconi_hash_read_tee(s,iv_addr,iv,sizeof iv)&&
             marconi_hash_read_tee(s,key_addr,key_input,s->hash_key_input_bytes)&&
             !memcmp(key_input,s->hash_key_input,s->hash_key_input_bytes==128?64:s->hash_key_input_bytes);
        static const uint8_t sha256_iv[32]={0x6a,0x09,0xe6,0x67,0xbb,0x67,0xae,0x85,0x3c,0x6e,0xf3,0x72,0xa5,0x4f,0xf5,0x3a,
                                          0x51,0x0e,0x52,0x7f,0x9b,0x05,0x68,0x8c,0x1f,0x83,0xd9,0xab,0x5b,0xe0,0xcd,0x19};
        if(s->hash_is_hmac)ok=ok&&!memcmp(iv,sha256_iv,sizeof iv)&&o62m_hmac_sha256_outer_final_v2(&s->hash_context,s->hash_inner_digest,digest);
        else memcpy(digest,s->hash_inner_digest,sizeof digest);
        if(out_addr>=MARCONI_TEE_ALIAS&&out_addr-MARCONI_TEE_ALIAS<=MARCONI_TEE_BYTES-sizeof digest)offset=out_addr-MARCONI_TEE_ALIAS;
        else if(out_addr>=MARCONI_TEE_BASE&&out_addr-MARCONI_TEE_BASE<=MARCONI_TEE_BYTES-sizeof digest)offset=out_addr-MARCONI_TEE_BASE;
        /* Proven output role is original TEE allocated RAM after image bytes.
         * No write to original image/code, model-private memory or MMIO. */
        ok=ok&&offset!=UINT64_MAX&&offset>=UINT64_C(770048);
        if(ok)ok=marconi_enable_private_guest_diagnostics(s);
        if(ok) {
            memcpy((uint8_t *)memory_region_get_ram_ptr(&s->ap_ram)+offset,digest,sizeof digest);
            memory_region_set_dirty(&s->ap_ram,offset,sizeof digest);s->hash_finished=true;
        }
        o62m_sha256_zeroize_v1(iv,sizeof iv);o62m_sha256_zeroize_v1(key_input,sizeof key_input);o62m_sha256_zeroize_v1(digest,sizeof digest);
        if(!ok)return marconi_hash_boundary(s,"actual-FINISH-source-outerhash-output-role-or-diagnostic-gate-failed");
        if(!o62m_hash_work_complete_v7(&s->symc_empty,UINT32_C(0x2000003f),3))
            return marconi_hash_boundary(s,"actual-FINISH-DMA-done-but-publication-failed");
        s->hash_job_queued=false;timer_del(s->hash_init_event);
        marconi_public_report("MARCONI_HASH_FINISH_COMPLETE actual-digest-assembly-or-HMAC-outerhash=true actual-bounded-guest-DMA-bytes=32 "
                              "CSQ-code=3 status=0 slot=0 private-diagnostics-active=true digest-and-key-not-output=true");return true;
    }
    if(frame[0]==UINT32_C(0x40000003)) {
        if(s->symc_empty.host[1].cq_words!=3||!s->hash_context.active||s->hash_is_hmac||s->hash_inner_done)
            return marconi_hash_boundary(s,"LL-PROC-not-active-unfinished-digest");
        uint64_t list=((uint64_t)frame[2]<<32)|frame[1];O62MHashStreamPlanV1 plan;
        int result=o62m_hash_stream_plan_v1(list,marconi_hash_stream_read,s,&plan);
        unsigned nodes=plan.count;uint64_t total=plan.total;
        if(!result)result=o62m_hash_stream_update_v1(&s->hash_context,&plan,marconi_hash_stream_read,s);
        o62m_sha256_zeroize_v1(&plan,sizeof plan);
        if(result) {
            marconi_public_report("MARCONI_HASH_LL_REJECT class=%d nodes=%u total-bytes=%" PRIu64
                " addresses-attributes-and-data-redacted=true",result,nodes,total);
            return marconi_hash_boundary(s,"LL-PROC-descriptor-source-or-update-failed");
        }
        if(!o62m_hash_work_complete_v7(&s->symc_empty,UINT32_C(0x40000003),2))
            return marconi_hash_boundary(s,"LL-PROC-real-update-publication-failed");
        s->hash_job_queued=false;timer_del(s->hash_init_event);
        marconi_public_report("MARCONI_HASH_LL_COMPLETE nodes=%u source-bytes=%" PRIu64
            " actual-incremental-SHA256=true padding=false inner-done=false CSQ-code=2 virtual-ns=%" PRId64
            " no-source-or-digest-output=true",nodes,total,now);return true;
    }
    if(frame[0]==UINT32_C(0x40000039)) {
        if(s->symc_empty.host[1].cq_words!=1||!s->hash_context.active||s->hash_is_hmac||s->hash_inner_done||
           !o62m_hmac_sha256_inner_final_v2(&s->hash_context,s->hash_inner_digest))
            return marconi_hash_boundary(s,"ZERO-final-not-active-digest-or-real-padding-failed");
        s->hash_inner_done=true;
        if(!o62m_hash_work_complete_v7(&s->symc_empty,UINT32_C(0x40000039),2))
            return marconi_hash_boundary(s,"ZERO-final-real-padding-publication-failed");
        s->hash_job_queued=false;timer_del(s->hash_init_event);
        marconi_public_report("MARCONI_HASH_ZERO_FINAL_COMPLETE actual-SHA256-padding=true CSQ-code=2 "
            " no-source-read-or-digest-output=true");return true;
    }
    if(frame[0]==UINT32_C(0x40000019)) {
        if(s->symc_empty.host[1].cq_words!=4||!s->hash_context.active||s->hash_inner_done)
            return marconi_hash_boundary(s,"PROC-without-active-HMAC-or-duplicate-padding");
        uint64_t count=(uint64_t)frame[1]+1,source=((uint64_t)frame[3]<<32)|frame[2];
        if(count>MARCONI_TEE_BYTES||
           !((source>=MARCONI_TEE_ALIAS&&source-MARCONI_TEE_ALIAS<=MARCONI_TEE_BYTES-count)||
             (source>=MARCONI_TEE_BASE&&source-MARCONI_TEE_BASE<=MARCONI_TEE_BYTES-count)))
            return marconi_hash_boundary(s,"PROC-source-range-or-size-not-in-proven-TEE-RAM");
        uint8_t scratch[4096]={0};bool ok=true;
        for(uint64_t done=0;done<count;) {
            size_t take=MIN(count-done,sizeof scratch);
            if(!marconi_hash_read_tee(s,source+done,scratch,take)||
               !o62m_hmac_sha256_update_v1(&s->hash_context,scratch,take)) {ok=false;break;}
            o62m_sha256_zeroize_v1(scratch,sizeof scratch);done+=take;
        }
        o62m_sha256_zeroize_v1(scratch,sizeof scratch);
        if(ok)ok=o62m_hmac_sha256_inner_final_v2(&s->hash_context,s->hash_inner_digest);
        if(!ok)return marconi_hash_boundary(s,"actual-PROC-DMA-HMAC-update-or-inner-padding-failed");
        s->hash_inner_done=true;
        if(!o62m_hash_work_complete_v7(&s->symc_empty,UINT32_C(0x40000019),2))
            return marconi_hash_boundary(s,"real-PROC-complete-but-CSQ-publication-invalid");
        s->hash_job_queued=false;timer_del(s->hash_init_event);
        marconi_public_report("MARCONI_HASH_PROC_COMPLETE source-bytes=%" PRIu64 " virtual-ns=%" PRId64
                     " actual-bounded-TEE-DMA=true actual-HMAC-inner-update-and-final-padding=true "
                     "length-minus-one-decoded=true CSQ-code=2 status=0 slot=0 no-inner-digest-or-key-output=true",count,now);
        return true;
    }

    uint32_t header=frame[0];bool hmac=(header&16)!=0;unsigned key_source=(header>>2)&3;
    if((header!=UINT32_C(0x80000101)&&header!=UINT32_C(0x80000111)&&header!=UINT32_C(0x80000115)&&header!=UINT32_C(0x80000119))||
       s->symc_empty.host[1].cq_words!=(hmac?5u:3u)||(s->hash_context.active&&!s->hash_finished))
        return marconi_hash_boundary(s,"unsupported-INIT-frame-or-active-unfinished-slot");
    uint64_t iv_addr=((uint64_t)frame[2]<<32)|frame[1],key_addr=((uint64_t)frame[4]<<32)|frame[3];
    static const uint8_t standard_iv[32]={0x6a,0x09,0xe6,0x67,0xbb,0x67,0xae,0x85,0x3c,0x6e,0xf3,0x72,0xa5,0x4f,0xf5,0x3a,
                                        0x51,0x0e,0x52,0x7f,0x9b,0x05,0x68,0x8c,0x1f,0x83,0xd9,0xab,0x5b,0xe0,0xcd,0x19};
    uint8_t iv[32]={0},key_input[128]={0},derived[16]={0};unsigned key_span=hmac?(key_source==2?128:48):0;
    bool ok=marconi_hash_read_tee(s,iv_addr,iv,sizeof iv)&&!memcmp(iv,standard_iv,sizeof iv);
    if(ok&&hmac)ok=marconi_hash_read_tee(s,key_addr,key_input,key_span);
    o62m_hmac_sha256_destroy_v1(&s->hash_context);o62m_sha256_zeroize_v1(s->hash_key_input,sizeof s->hash_key_input);
    o62m_sha256_zeroize_v1(s->hash_inner_digest,sizeof s->hash_inner_digest);
    s->hash_inner_done=false;s->hash_finished=false;s->hash_is_hmac=hmac;s->hash_key_input_bytes=key_span;
    if(ok&&hmac&&key_source!=2) {
        unsigned shadow_bit=key_source==1?8:2,root_offset=key_source==1?24:4;
        ok=o62m_virtual_otp_valid_v1(&s->virtual_otp)&&s->virtual_otp.phase==O62M_VOTP_VALID_V1&&
           s->virtual_otp.key_bytes==16&&(s->virtual_otp.shadow_valid&shadow_bit)&&
           o62m_ladder_aes128_dec3_v1(s->virtual_otp.private_shadow+root_offset,16,key_input,48,derived)&&
           o62m_hmac_sha256_init_v1(&s->hash_context,derived,sizeof derived);
    } else if(ok&&hmac) {
        /* Original ABI owns a128B hkey buffer. SHA256 consumes its already
         * normalized64B block; do not hash the128B buffer as a raw long key. */
        ok=o62m_hmac_sha256_init_v1(&s->hash_context,key_input,64);
    } else if(ok) {
        o62m_sha256_init_v1(&s->hash_context.inner);
        ok=!o62m_sha256_starts_v1(&s->hash_context.inner,0);s->hash_context.active=ok;
    }
    if(ok&&key_span)memcpy(s->hash_key_input,key_input,key_span);
    o62m_sha256_zeroize_v1(iv,sizeof iv);o62m_sha256_zeroize_v1(key_input,sizeof key_input);o62m_sha256_zeroize_v1(derived,sizeof derived);
    if(!ok)return marconi_hash_boundary(s,"actual-INIT-IV-key-source-or-hash-context-work-failed");
    if(!o62m_hash_work_complete_v7(&s->symc_empty,header,1))return marconi_hash_boundary(s,"actual-INIT-complete-but-publication-invalid");
    s->hash_job_queued=false;timer_del(s->hash_init_event);
    marconi_public_report("MARCONI_HASH_INIT_COMPLETE algorithm=SHA256 mode=%s key-source=%s "
                          "key-input-ABI-span=%u actual-IV-and-source-work=true actual-crypto-context-initialized=true "
                          "CSQ-code=1 status=0 slot=0 virtual-ladder-assumption-for-OTP-keys-only=true no-private-data-output=true",
                          hmac?"HMAC":"digest",hmac?(key_source==0?"MODEL16":key_source==1?"ROOT16":"EXT-normalized64"):"none",key_span);

    return true;
}
static void marconi_hash_init_callback(void *opaque)
{ (void)marconi_hash_init_refresh(opaque); }
static void marconi_hash_exit(Notifier *notify,void *opaque)
{
    MarconiProbeState *s=container_of(notify,MarconiProbeState,hash_exit);(void)opaque;
    if(s->hash_init_event)timer_del(s->hash_init_event);
    o62m_hmac_sha256_destroy_v1(&s->hash_context);o62m_sha256_zeroize_v1(s->hash_key_input,sizeof s->hash_key_input);
    o62m_sha256_zeroize_v1(s->hash_inner_digest,sizeof s->hash_inner_digest);
    bool wiped=o62m_virtual_otp_zero_v1(s->hash_inner_digest,sizeof s->hash_inner_digest)&&o62m_virtual_otp_zero_v1(&s->hash_context,sizeof s->hash_context)&&o62m_virtual_otp_zero_v1(s->hash_key_input,sizeof s->hash_key_input);
    marconi_public_report("MARCONI_HASH_EXIT_WIPE private-HMAC-state-and-key-input-zeroized=%s no-key-or-digest-output=true",wiped?"true":"false");
    assert(wiped);s->hash_exit_registered=false;
}

/* Explicit retained PMIC configuration view, not IRQ status. Original
 * dcc094 SET is baseline. Optin ADC adds original dc0b70 single-bit CLEAR;
 * both preserve other bits. No write creates a peripheral-ready source. */
/* Existing non-card probes preserve their original fail-fast behavior.
 * Once a volatile card is bound, owned worker failures must unwind via the
 * main loop so vCPUs stop and block graph cleanup actually wipes its pages. */
static void marconi_owned_runtime_failure_stop(MarconiProbeState *s)
{
    if(!s->normal_emmc_bound)exit(MARCONI_PROBE_EXIT);
    s->reported=true;
    marconi_public_report("MARCONI_BOUND_RUNTIME_FAILURE_CLEANUP normal-shutdown-requested=true stop-before-further-model-commit=true private-RAM-backend-needs-block-cleanup=true");
    qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC,MARCONI_PROBE_EXIT);
    if(current_cpu)cpu_exit(current_cpu);
}
static bool marconi_mempll_source(MarconiProbeState *s)
{
    uint32_t mode,post,ref;
    if(!s->pll_rate_fixture||
       o62m_clock_controls_read(&s->pll_rate,0x5a09c400,4,&mode)!=O62M_CLOCK_CONTROLS_OK||
       o62m_clock_controls_read(&s->pll_rate,0x5a09c404,4,&post)!=O62M_CLOCK_CONTROLS_OK||
       o62m_clock_controls_read(&s->pll_rate,0x5a09c408,4,&ref)!=O62M_CLOCK_CONTROLS_OK)return false;
    return (ref&0x3f)&&((post>>24)&7)&&((post>>27)&7)&&
           (((ref>>6)&0xfff)||((mode&8)&&(post&0xffffff)));
}
static void marconi_mempll_cancel(MarconiProbeState *s)
{
    if(s->mempll_event)timer_del(s->mempll_event);
    o62m_mempll_cancel_v1(&s->mempll_vote);
}
static void marconi_mempll_reset(void *opaque)
{
    MarconiProbeState *s=opaque;
    timer_del(s->mempll_event);o62m_mempll_reset_v1(&s->mempll_vote);
}
static void marconi_mempll_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->reported){marconi_mempll_cancel(s);return;}
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(!s->mempll_vote.pending||s->mempll_armed!=s->mempll_vote.generation||!marconi_mempll_source(s)) {
        marconi_mempll_cancel(s);marconi_owned_runtime_failure_stop(s);return;
    }
    if(now<s->mempll_vote.deadline){timer_mod_ns(s->mempll_event,s->mempll_vote.deadline);return;}
    if(!o62m_mempll_complete_v1(&s->mempll_vote,s->mempll_armed,now,true)) {
        marconi_mempll_cancel(s);marconi_owned_runtime_failure_stop(s);return;
    }
    marconi_public_report("MARCONI_MEMPLL_QUALIFIED generation=%" PRIu64 " completions=%" PRIu64
        " actual-votes-and-positive-reference-qualification=true no-CP-state-published=true",
        s->mempll_vote.generation,s->mempll_vote.completions);
}
static void marconi_mempll_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,mempll_exit);(void)opaque;
    marconi_mempll_cancel(s);
    marconi_public_report("MARCONI_MEMPLL_EXIT writes=%" PRIu64 " requests=%" PRIu64
        " completions=%" PRIu64 " cancelled=%" PRIu64 " pending=0 timer-cancelled=true",
        s->mempll_vote.writes,s->mempll_vote.requests,s->mempll_vote.completions,s->mempll_vote.cancelled);
}
static bool marconi_rnp_source_available(MarconiProbeState *s)
{
    uint32_t gate;
    return s->rnp_live_config_active&&s->te_context.phase==O62M_TE_CONTEXT_READY_PHASE&&
        (s->rnp_mask_word&1)&&
        o62m_clock_controls_read(&s->board_controls,UINT64_C(0x50050018),4,&gate)==O62M_CLOCK_CONTROLS_OK&&(gate&2);
}
static void marconi_rnp_pool_cancel(MarconiProbeState *s)
{
    if(s->rnp_pool_event)timer_del(s->rnp_pool_event);
    o62m_rnp_cancel_v1(&s->rnp_pool);
}
static void marconi_rnp_pool_reset(void *opaque)
{
    MarconiProbeState *s=opaque;
    marconi_rnp_pool_cancel(s);s->rnp_source_pending=0;
}
static void marconi_rnp_pool_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->reported){marconi_rnp_pool_cancel(s);return;}
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(!s->rnp_pool.pending||s->rnp_pool.generation!=s->rnp_pool_armed||!marconi_rnp_source_available(s)) {
        marconi_rnp_pool_cancel(s);marconi_owned_runtime_failure_stop(s);return;
    }
    if(now<s->rnp_pool.deadline){timer_mod_ns(s->rnp_pool_event,s->rnp_pool.deadline);return;}
    uint8_t bytes[32]={0};Error *error=NULL;
    int rc=qcrypto_random_bytes(bytes,sizeof(bytes),&error);
    bool ok=o62m_rnp_complete_v1(&s->rnp_pool,s->rnp_pool_armed,now,bytes,rc==0&&!error);
    o62m_rnp_wipe_v1(bytes,sizeof(bytes));error_free(error);timer_del(s->rnp_pool_event);
    if(!ok){marconi_rnp_pool_cancel(s);marconi_owned_runtime_failure_stop(s);return;}
    if(s->rnp_pool.completions<=32)marconi_public_report("MARCONI_RNP_POOL_COMPLETE generation=%" PRIu64
        " completions=%" PRIu64 " bytes=32 actual-OS-source-success=true source-data-and-hash-redacted=true",
        s->rnp_pool.generation,s->rnp_pool.completions);
}
static void marconi_rnp_pool_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,rnp_pool_exit);(void)opaque;
    marconi_rnp_pool_cancel(s);
    bool wiped=o62m_virtual_otp_zero_v1(s->rnp_pool.pool,sizeof(s->rnp_pool.pool));
    marconi_public_report("MARCONI_RNP_POOL_EXIT requests=%" PRIu64 " completions=%" PRIu64
        " cancelled=%" PRIu64 " word-reads=%" PRIu64 " pending=%u private-pool-wiped=%u AO-key-owner-independent=true",
        s->rnp_pool.requests,s->rnp_pool.completions,s->rnp_pool.cancelled,s->rnp_pool.reads,s->rnp_pool.pending,wiped);
    assert(wiped);
}
static void marconi_ap_domain0_reset(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->ap_domain0_event)timer_del(s->ap_domain0_event);
    o62m_pm_domain0_reset_v1(&s->ap_domain0);s->ap_domain0_guard=0;
}
static void marconi_ap_domain0_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->reported) {o62m_pm_domain0_cancel_v1(&s->ap_domain0);timer_del(s->ap_domain0_event);return;}
    if(!o62m_pm_domain0_complete_v1(&s->ap_domain0,s->ap_domain0_armed_generation,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
        o62m_pm_domain0_cancel_v1(&s->ap_domain0);timer_del(s->ap_domain0_event);
        marconi_owned_runtime_failure_stop(s);return;
    }
    marconi_public_report("MARCONI_DOMAIN0_ON_SERVICE_COMPLETE generation=%" PRIu64 " completions=%" PRIu64
        " actual-current-job=true already-on-preserved=true available-restored=true",s->ap_domain0.generation,s->ap_domain0.completions);
}
static void marconi_ap_domain0_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,ap_domain0_exit);(void)opaque;
    timer_del(s->ap_domain0_event);o62m_pm_domain0_cancel_v1(&s->ap_domain0);
    marconi_public_report("MARCONI_DOMAIN0_ON_SERVICE_EXIT requests=%" PRIu64 " completions=%" PRIu64
        " cancelled=%" PRIu64 " pending=0 timer-cancelled=true",s->ap_domain0.requests,s->ap_domain0.completions,s->ap_domain0.cancelled);
}
static void marconi_ap_tee_mbox_refresh(MarconiProbeState *s)
{
    if(!s->ap_mailbox_config||!s->mbox_outbound_experiment||!s->board_controls_fixture)return;
    uint32_t enable[2]={s->ap_mailbox_enable,0};
    bool known=o62m_clock_controls_read(&s->board_controls,UINT64_C(0x5c120000),4,&enable[1])==O62M_CLOCK_CONTROLS_OK;
    for(unsigned i=0;i<2;i++) {
        bool level=known&&!s->reported&&o62m_ap_tee_mbox_irq_v1(&s->ap_tee_mbox,i,enable[i]);
        if(level!=s->ap_tee_mbox_irq_level[i]) {
            s->ap_tee_mbox_irq_level[i]=level;
            if(level)++s->ap_tee_mbox_asserts[i];else ++s->ap_tee_mbox_deasserts[i];
            qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->armv7m),78+i),level);
            marconi_public_report("MARCONI_AP_TEE_DOORBELL_IRQ receiver=%u external=%u level=%u asserts=%" PRIu64
                " deasserts=%" PRIu64 " actual-pending-and-existing-enable=true",
                i,78+i,level,s->ap_tee_mbox_asserts[i],s->ap_tee_mbox_deasserts[i]);
        }
    }
}
static void marconi_ap_tee_mbox_reset(void *opaque)
{
    MarconiProbeState *s=opaque;o62m_ap_tee_mbox_cancel_v1(&s->ap_tee_mbox);
    marconi_ap_tee_mbox_refresh(s);
}
static void marconi_ap_tee_mbox_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,ap_tee_mbox_exit);(void)opaque;
    marconi_ap_tee_mbox_reset(s);
    for(unsigned i=0;i<2;i++)
        marconi_public_report("MARCONI_AP_TEE_DOORBELL_EXIT receiver=%u delivered=%" PRIu64 " coalesced=%" PRIu64
            " reads=%" PRIu64 " clear-writes=%" PRIu64 " doorbell-acknowledged=%" PRIu64 " cancelled=%" PRIu64
            " asserts=%" PRIu64 " deasserts=%" PRIu64 " pending=0 line-low=true ring-and-session-owned-by-firmware=true",
            i,s->ap_tee_mbox.delivered[i],s->ap_tee_mbox.coalesced[i],s->ap_tee_mbox.reads[i],s->ap_tee_mbox.clears[i],
            s->ap_tee_mbox.acknowledged[i],s->ap_tee_mbox.cancelled[i],s->ap_tee_mbox_asserts[i],s->ap_tee_mbox_deasserts[i]);
}
static void marconi_ap_ipc_slots_reset(void *opaque)
{
    MarconiProbeState *s=opaque;o62m_ap_ipc_slots_reset_v2(&s->ap_ipc_slots);
    o62m_virtual_otp_wipe_v1(&s->cp_mailbox_config,sizeof s->cp_mailbox_config);
    o62m_virtual_otp_wipe_v1(&s->cp_mbox_outbound,sizeof s->cp_mbox_outbound);
}
static void marconi_ap_ipc_slots_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,ap_ipc_slots_exit);(void)opaque;
    uint64_t cp_reads=s->cp_mailbox_config.reads,cp_writes=s->cp_mailbox_config.writes;
    unsigned cp_notifies=s->cp_mbox_outbound.count;
    marconi_ap_ipc_slots_reset(s);
    marconi_public_report("MARCONI_CP_MAILBOX_OUTBOUND_EXIT delivered-to-AP=%u history-wiped=true",cp_notifies);
    marconi_public_report("MARCONI_CP_MAILBOX_CONFIG_EXIT reads=%" PRIu64 " writes=%" PRIu64 " enable-wiped=true no-CP-event-or-IRQ-generated=true",cp_reads,cp_writes);
    marconi_public_report("MARCONI_AP_IPC_SLOTS_EXIT writes=%" PRIu64 " commits=%" PRIu64
        " reads=%" PRIu64 " cancelled=%" PRIu64 " payload-wiped=%u payload-reads=%" PRIu64 " peer-consumed=%" PRIu64,
        s->ap_ipc_slots.writes,s->ap_ipc_slots.commits,s->ap_ipc_slots.reads,s->ap_ipc_slots.cancelled,
        o62m_virtual_otp_zero_v1(s->ap_ipc_slots.payload,sizeof(s->ap_ipc_slots.payload)),
        s->ap_ipc_slots.payload_reads,s->ap_ipc_slots.releases);
}
static void marconi_cp_uart2_reset(void *opaque)
{
    MarconiProbeState *s=opaque;
    o62m_uart1_boot_init_v1(&s->cp_uart2);
}
static void marconi_cp_uart2_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,cp_uart2_exit);(void)opaque;
    uint64_t accesses=s->cp_uart2.accesses,resets=s->cp_uart2.resets;
    o62m_virtual_otp_wipe_v1(&s->cp_uart2,sizeof s->cp_uart2);
    marconi_public_report("MARCONI_CP_UART2_EXIT accesses=%" PRIu64 " resets=%" PRIu64
        " independent-state-wiped=true accepted-data-transfers=0",accesses,resets);
}
static void marconi_uart1_empty_refresh(MarconiProbeState *s)
{
    bool level=!s->reported&&o62m_uart1_empty_irq_v1(&s->uart1_boot);
    if(level!=s->uart1_empty_irq_level) {
        s->uart1_empty_irq_level=level;
        if(level)s->uart1_empty_asserts++;else s->uart1_empty_deasserts++;
        qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->armv7m),60),level);
        marconi_public_report("MARCONI_UART1_EMPTY_IRQ level=%u asserts=%" PRIu64 " deasserts=%" PRIu64
            " external=60 actual-IER-and-queue-condition=true PTIME=%u FIFO=%u no-peer-ACK=true",
            level,s->uart1_empty_asserts,s->uart1_empty_deasserts,(s->uart1_boot.ier&0x80)!=0,(s->uart1_boot.fcr&1)!=0);
    }
}
static void marconi_uart1_tx_emit(void *opaque,uint8_t byte,int64_t completed)
{
    MarconiProbeState *s=opaque;
    assert(s->uart1_emitted<sizeof(s->uart1_outbound));
    s->uart1_outbound[s->uart1_emitted++]=byte;
    if(s->uart1_emitted<=32||!(s->uart1_emitted%1024))
        marconi_public_report("MARCONI_UART1_TX_COMPLETE emitted=%" PRIu64 " completed-ns=%" PRId64
            " actual-wire-frame=true private-outbound=true no-RX-or-peer-ACK=true",s->uart1_emitted,completed);
}
static void marconi_uart1_tx_arm(MarconiProbeState *s)
{
    int64_t deadline=o62m_uart_tx_next_deadline(&s->uart1_boot.tx);
    if(s->reported||deadline==O62M_UART_TX_NO_DEADLINE)timer_del(s->uart1_tx_event);
    else timer_mod_ns(s->uart1_tx_event,deadline);
    marconi_uart1_empty_refresh(s);
}
static bool marconi_uart1_tx_advance(MarconiProbeState *s, int64_t now)
{
    if(s->reported)return false;
    if(!o62m_uart_tx_advance(&s->uart1_boot.tx,now,marconi_uart1_tx_emit,s))return false;
    marconi_uart1_tx_arm(s);return true;
}
static void marconi_uart1_tx_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->reported){timer_del(s->uart1_tx_event);return;}
    if(!marconi_uart1_tx_advance(s,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
        timer_del(s->uart1_tx_event);marconi_owned_runtime_failure_stop(s);return;
    }
}
static void marconi_uart1_empty_reset(void *opaque)
{
    MarconiProbeState *s=opaque;
    timer_del(s->uart1_tx_event);
    s->uart1_cancelled+=s->uart1_boot.tx.count+s->uart1_boot.tx.active;
    o62m_uart1_boot_init_v1(&s->uart1_boot);
    s->uart1_boot.tx.last_ns=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    marconi_uart1_empty_refresh(s);
}
static void marconi_uart1_empty_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,uart1_empty_exit);(void)opaque;
    marconi_uart1_empty_reset(s);
    o62m_virtual_otp_wipe_v1(s->uart1_outbound,sizeof(s->uart1_outbound));
    bool wiped=o62m_virtual_otp_zero_v1(s->uart1_outbound,sizeof(s->uart1_outbound));
    marconi_public_report("MARCONI_UART1_TX_EXIT accepted=%" PRIu64 " emitted=%" PRIu64
        " discarded=%" PRIu64 " cancelled=%" PRIu64 " asserts=%" PRIu64 " deasserts=%" PRIu64
        " line-low=true private-outbound-wiped=%u no-external-peer=true",
        s->uart1_accepted,s->uart1_emitted,s->uart1_discarded,s->uart1_cancelled,
        s->uart1_empty_asserts,s->uart1_empty_deasserts,wiped);
}
static void marconi_ap_spi_cancel(MarconiProbeState *s)
{
    if(s->ap_spi_event)timer_del(s->ap_spi_event);
    o62m_spi_cancel_v1(&s->ap_spi);
}
static bool marconi_ap_spi_enabled(MarconiProbeState *s)
{
    return s->ap_spi_config_words[1]==1&&s->ap_spi_config_words[2]==1&&
        !(s->ap_spi_config_words[5]&63)&&
        o62m_spi_mode_v1(s->ap_spi_config_words[0],s->ap_spi_config_words[6]);
}
static void marconi_ap_spi_arm(MarconiProbeState *s)
{
    if(s->ap_spi.pending){s->ap_spi_armed=s->ap_spi.generation;
        timer_mod_ns(s->ap_spi_event,s->ap_spi.deadline_ns);}
}
static void marconi_ap_spi_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->reported||!s->ap_spi.pending){marconi_ap_spi_cancel(s);return;}
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(now<s->ap_spi.deadline_ns){timer_mod_ns(s->ap_spi_event,s->ap_spi.deadline_ns);return;}
    if(!marconi_ap_spi_enabled(s)||!o62m_spi_complete_v1(&s->ap_spi,s->ap_spi_armed,now,
        s->ap_spi_config_words[0],s->ap_spi_config_words[6])) {
        marconi_public_report("MARCONI_AP_SPI_BOUNDARY completion-rejected=true no-new-RX=true");
        marconi_ap_spi_cancel(s);marconi_owned_runtime_failure_stop(s);return;
    }
    marconi_public_report("MARCONI_AP_SPI_FRAME completed=%" PRIu64 " TX-queued=%u RX-queued=%u "
        "virtual-ns=%" PRId64 " actual-clocked-8bit-frame=true explicit-no-peer-MISO-high=true raw-TX-redacted=true",
        s->ap_spi.completed,s->ap_spi.tx_count,s->ap_spi.rx_count,now);
    marconi_ap_spi_arm(s);
}
static void marconi_ap_spi_reset(void *opaque)
{
    MarconiProbeState *s=opaque;marconi_ap_spi_cancel(s);
    memset(s->ap_spi_config_words,0,sizeof(s->ap_spi_config_words));s->ap_spi_error_latch=0;
}
static void marconi_ap_spi_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,ap_spi_exit);(void)opaque;
    marconi_ap_spi_cancel(s);
    marconi_public_report("MARCONI_AP_SPI_EXIT accepted=%u completed=%" PRIu64 " cancelled=%" PRIu64
        " RX-consumed=%" PRIu64 " owned-timer-cancelled=true no-peer=true",
        s->ap_spi.history_count,s->ap_spi.completed,s->ap_spi.cancelled,s->ap_spi.received);
}
static bool marconi_pubap_quiesce_domain(MarconiProbeState *s)
{
    return marconi_pm_domain4_refresh(s)&&s->pm_domain4.available&&s->pm_domain4.on;
}
static void marconi_pubap_quiesce_reset(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->pubap_quiesce_event)timer_del(s->pubap_quiesce_event);
    o62m_pubap_quiesce_cancel_v1(&s->pubap_quiesce);
}
static void marconi_pubap_quiesce_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->reported||!s->pubap_quiesce.pending){marconi_pubap_quiesce_reset(s);return;}
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(now<s->pubap_quiesce.deadline_ns){timer_mod_ns(s->pubap_quiesce_event,s->pubap_quiesce.deadline_ns);return;}
    if(!o62m_pubap_quiesce_complete_v1(&s->pubap_quiesce,s->pubap_quiesce_armed_generation,
                                    now,marconi_pubap_quiesce_domain(s))) {
        marconi_public_report("MARCONI_PUBAP_QUIESCE_BOUNDARY current-service-or-domain-unavailable=true");
        marconi_pubap_quiesce_reset(s);marconi_owned_runtime_failure_stop(s);return;
    }
    marconi_public_report("MARCONI_PUBAP_QUIESCE_COMPLETE generation=%" PRIu64 " completions=%" PRIu64
        " virtual-ns=%" PRId64 " actual-control-service=true no-accepted-frequency-transition=true no-frequency-or-IRQ-synthesis=true",
        s->pubap_quiesce.generation,s->pubap_quiesce.completions,now);
}
static void marconi_pubap_quiesce_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,pubap_quiesce_exit);(void)opaque;
    marconi_public_report("MARCONI_PUBAP_QUIESCE_EXIT requests=%" PRIu64 " completions=%" PRIu64
        " pending=%u owned-timer-cancelled=true",s->pubap_quiesce.requests,s->pubap_quiesce.completions,s->pubap_quiesce.pending);
    marconi_pubap_quiesce_reset(s);
}
static void marconi_sensys_config_cancel(MarconiProbeState *s)
{
    if(s->sensys_config_event)timer_del(s->sensys_config_event);
    o62m_sensys_config_cancel_v1(&s->sensys_config);
}
static void marconi_sensys_config_reset(void *opaque)
{
    MarconiProbeState *s=opaque;marconi_sensys_config_cancel(s);
    s->sensys_config.desired=0;s->sensys_config.effective=0;
}
static void marconi_sensys_config_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->reported||!s->sensys_config.pending){marconi_sensys_config_cancel(s);return;}
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(now<s->sensys_config.deadline_ns){timer_mod_ns(s->sensys_config_event,s->sensys_config.deadline_ns);return;}
    if(!o62m_sensys_config_complete_v1(&s->sensys_config,s->sensys_config_armed_generation,now)) {
        marconi_public_report("MARCONI_SENSYS_CONFIG_BOUNDARY current-generation-commit-rejected=true");
        marconi_sensys_config_cancel(s);marconi_owned_runtime_failure_stop(s);return;
    }
    marconi_public_report("MARCONI_SENSYS_CONFIG_COMMIT generation=%" PRIu64 " completions=%" PRIu64
        " virtual-ns=%" PRId64 " actual-desired-to-effective-copy=true peer-absent=true no-peer-ready-or-IRQ=true",
        s->sensys_config.generation,s->sensys_config.completions,now);
}
static void marconi_sensys_config_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,sensys_config_exit);(void)opaque;
    marconi_public_report("MARCONI_SENSYS_CONFIG_EXIT requests=%" PRIu64 " completions=%" PRIu64
        " replaced=%" PRIu64 " pending=%u owned-timer-cancelled=true peer-unpublished=true",
        s->sensys_config.requests,s->sensys_config.completions,s->sensys_config.replaced,s->sensys_config.pending);
    marconi_sensys_config_cancel(s);
}
static void marconi_display_short_cancel(MarconiProbeState *s)
{
    if(s->display_short_event)timer_del(s->display_short_event);
    o62m_display_short_cancel_v3(&s->display_short);
}
static void marconi_mipi_phy_invalidate(MarconiProbeState *s)
{
    marconi_display_short_cancel(s);
    if(s->mipi_phy_pll_event)timer_del(s->mipi_phy_pll_event);
    if(s->mipi_phy_ulps_event)timer_del(s->mipi_phy_ulps_event);
    o62m_mipi_invalidate_v2(&s->mipi_phy);
}
static bool marconi_mipi_phy_sources(MarconiProbeState *s)
{
    return s->mipi_phy_reference_available&&s->mipi_phy_test_available&&
        marconi_pm_bit8_refresh(s)&&o62m_pm_bit_domain9_clock_available_v1(&s->pm_bit8);
}
static bool marconi_display_short_eligible(MarconiProbeState *s)
{
    return marconi_mipi_phy_sources(s)&&s->mipi_phy.pll_locked&&s->mipi_phy.lanes_active&&
        !s->mipi_phy.pll_pending&&!s->mipi_phy.ulps_pending&&(s->mipi_phy.rstz&7)==7&&
        s->mipi_phy.ulps_control==0&&(s->mipi_mode_config&1)&&(s->mipi_cmd_mode&1)&&
        !(s->mipi_cmd_mode&2)&&(s->mipi_packet_config&4);
}
static void marconi_display_short_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->reported||(!s->display_short.pending&&!s->display_short.read_pending)){marconi_display_short_cancel(s);return;}
    bool eligible=marconi_display_short_eligible(s);
    if(!eligible) {
        marconi_public_report("MARCONI_DISPLAY_SHORT_BOUNDARY sources-or-config-invalid=true");
        marconi_display_short_cancel(s);marconi_owned_runtime_failure_stop(s);return;
    }
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool host_pending=s->display_short.pending;
    int64_t deadline=host_pending?s->display_short.deadline_ns:s->display_short.peer_deadline_ns;
    if(now<deadline){timer_mod_ns(s->display_short_event,deadline);return;}
    bool accepted=host_pending?
        o62m_display_short_consume_v3(&s->display_short,s->display_short_armed,now,
            s->mipi_phy.pll_generation,s->mipi_phy.ulps_generation,eligible):
        o62m_display_short_nopeer_v3(&s->display_short,s->display_short_armed,now,
            s->mipi_phy.pll_generation,s->mipi_phy.ulps_generation,eligible);
    if(!accepted) {
        marconi_public_report("MARCONI_DISPLAY_SHORT_BOUNDARY current-generation-work-rejected=true");
        marconi_display_short_cancel(s);marconi_owned_runtime_failure_stop(s);return;
    }
    if(host_pending) {
        marconi_public_report("MARCONI_DISPLAY_SHORT_CONSUMED generation=%" PRIu64 " consumed=%" PRIu64
            " virtual-ns=%" PRId64 " actual-queued-header-consumed=true outbound-retained=true panel-ACK=false panel-RX=false",
            s->display_short.generation,s->display_short.consumed,now);
        if(s->display_short.read_pending)timer_mod_ns(s->display_short_event,s->display_short.peer_deadline_ns);
    } else marconi_public_report("MARCONI_DISPLAY_NOPEER_TIMEOUT generation=%" PRIu64 " timeouts=%" PRIu64
        " virtual-ns=%" PRId64 " RX-empty=true read-busy=false no-injected-response=true",
        s->display_short.generation,s->display_short.peer_timeouts,now);
}
static void marconi_mipi_phy_reset(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->mipi_phy_pll_event)timer_del(s->mipi_phy_pll_event);
    if(s->mipi_phy_ulps_event)timer_del(s->mipi_phy_ulps_event);
    if(s->display_short_event)timer_del(s->display_short_event);
    o62m_display_short_reset_v3(&s->display_short);
    o62m_mipi_reset_v2(&s->mipi_phy);
    s->mipi_cmd_mode=0;s->mipi_packet_config=0;s->mipi_mode_config=0;s->mipi_receive_vcid=0;
    memset(s->mipi_boot_config,0,sizeof(s->mipi_boot_config));
    s->mipi_clock_request=0;
}
static void marconi_mipi_phy_pll_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->reported||!s->mipi_phy.pll_pending){marconi_mipi_phy_invalidate(s);return;}
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(now<s->mipi_phy.pll_deadline_ns){timer_mod_ns(s->mipi_phy_pll_event,s->mipi_phy.pll_deadline_ns);return;}
    bool stopping=s->mipi_phy.pll_stopping;
    if(!o62m_mipi_pll_complete_v2(&s->mipi_phy,s->mipi_phy_pll_armed,now,marconi_mipi_phy_sources(s))) {
        marconi_public_report("MARCONI_MIPI_PHY_BOUNDARY stage=PLL source-or-current-request-invalid=true");
        marconi_mipi_phy_invalidate(s);marconi_owned_runtime_failure_stop(s);return;
    }
    if(stopping) {
        marconi_public_report("MARCONI_MIPI_PHY_PLL_STOP generation=%" PRIu64 " completions=%" PRIu64
            " virtual-ns=%" PRId64 " actual-entry-then-positive-delay=true",s->mipi_phy.pll_generation,s->mipi_phy.pll_stops,now);
        return;
    }
    marconi_public_report("MARCONI_MIPI_PHY_PLL_LOCK generation=%" PRIu64 " completions=%" PRIu64
        " virtual-ns=%" PRId64 " actual-test-config-force-and-26-reference-cycles=true ULPS-not-yet-exited=true",
        s->mipi_phy.pll_generation,s->mipi_phy.pll_completions,now);
}
static void marconi_mipi_phy_ulps_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->reported||!s->mipi_phy.ulps_pending){marconi_mipi_phy_invalidate(s);return;}
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(now<s->mipi_phy.ulps_deadline_ns){timer_mod_ns(s->mipi_phy_ulps_event,s->mipi_phy.ulps_deadline_ns);return;}
    bool entering=s->mipi_phy.ulps_entering;
    bool source=marconi_mipi_phy_sources(s)&&(!entering||(!s->display_short.pending&&!s->display_short.read_pending));
    if(!o62m_mipi_ulps_qualify_v2(&s->mipi_phy,s->mipi_phy_ulps_armed,now,source)) {
        marconi_public_report("MARCONI_MIPI_PHY_BOUNDARY stage=ULPS source-or-current-request-invalid=true");
        marconi_mipi_phy_invalidate(s);marconi_owned_runtime_failure_stop(s);return;
    }
    if(entering) {
        s->mipi_phy_pll_armed=s->mipi_phy.pll_generation;
        timer_mod_ns(s->mipi_phy_pll_event,s->mipi_phy.pll_deadline_ns);
        marconi_public_report("MARCONI_MIPI_PHY_ULPS_ENTERED generation=%" PRIu64 " entries=%" PRIu64
            " virtual-ns=%" PRId64 " actual-entry-request-held=true PLL-still-locked=true stop-deadline-ns=%" PRId64,
            s->mipi_phy.ulps_generation,s->mipi_phy.ulps_entries,now,s->mipi_phy.pll_deadline_ns);
        return;
    }
    marconi_public_report("MARCONI_MIPI_PHY_ULPS_QUALIFIED generation=%" PRIu64 " qualifications=%" PRIu64
        " virtual-ns=%" PRId64 " actual-exit-request-held-1ms=true active-not-awaits-actual-release=true",
        s->mipi_phy.ulps_generation,s->mipi_phy.ulps_qualifications,now);
}
static void marconi_mipi_phy_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,mipi_phy_exit);(void)opaque;
    marconi_public_report("MARCONI_MIPI_PHY_EXIT test-commits=%" PRIu64 " PLL-requests=%" PRIu64
        " PLL-completions=%" PRIu64 " ULPS-requests=%" PRIu64 " ULPS-qualifications=%" PRIu64
        " ULPS-exits=%" PRIu64 " ULPS-entries=%" PRIu64 " PLL-stops=%" PRIu64 " owned-timers-cancelled=true PHY-control-only=true",
        s->mipi_phy.test_commits,s->mipi_phy.pll_requests,s->mipi_phy.pll_completions,
        s->mipi_phy.ulps_requests,s->mipi_phy.ulps_qualifications,s->mipi_phy.ulps_exits,s->mipi_phy.ulps_entries,s->mipi_phy.pll_stops);
    marconi_mipi_phy_invalidate(s);
    marconi_public_report("MARCONI_DISPLAY_SHORT_EXIT requests=%u consumed=%" PRIu64 " cancelled=%" PRIu64
        " pending=%u read-pending=%u nopeer-timeouts=%" PRIu64 " status-reads=%" PRIu64 " bounded-history-retained=true owned-timer-cancelled=true panel-ACK=false panel-RX=false",
        s->display_short.history_count,s->display_short.consumed,s->display_short.cancelled,
        s->display_short.pending,s->display_short.read_pending,s->display_short.peer_timeouts,s->display_short_status_reads);
}
static int marconi_gpu_diagnostic_ram_owner(MarconiProbeState *s,uint64_t address,uint64_t bytes)
{
    if(!bytes||address>UINT32_MAX||bytes>UINT64_C(0x100000000)-address)return -1;
    MemoryRegionSection section=memory_region_find(get_system_memory(),address,bytes);
    MemoryRegion *owners[]={&s->handoff_ap_ram,&s->ap_uncache_ram,&s->gpu_sys_low_ram,
                            &s->ap_boot_buffer_ram,&s->gpu_sys_high_ram};
    int owner=-1;
    for(unsigned i=0;i<G_N_ELEMENTS(owners);i++)if(section.mr==owners[i])owner=i;
    if(owner<0||!memory_region_is_ram(section.mr)||memory_region_is_rom(section.mr)||
       !int128_ge(section.size,int128_make64(bytes)))owner=-1;
    if(section.mr)memory_region_unref(section.mr);
    return owner;
}
static bool marconi_gpu_ram_overlap(uint64_t a,uint64_t an,uint64_t b,uint64_t bn)
{
    MemoryRegionSection left=memory_region_find(get_system_memory(),a,an);
    MemoryRegionSection right=memory_region_find(get_system_memory(),b,bn);
    bool overlap=false;
    if(left.mr&&left.mr==right.mr) {
        uint64_t x=left.offset_within_region,y=right.offset_within_region;
        overlap=x>=y?x-y<bn:y-x<an;
    }
    if(left.mr)memory_region_unref(left.mr);
    if(right.mr)memory_region_unref(right.mr);
    return overlap;
}
static bool marconi_gpu_source_matches_completed_state_target(MarconiProbeState *s,const O62MGpuBatchOpV1 *op)
{
    if(!s->gpu_state_known[0x10]||!s->gpu_state_known[0x11]||!s->gpu_state_known[0x12]||!s->gpu_state_known[0x13]||
       s->gpu_state_bank[0x10]!=0x103||s->gpu_state_bank[0x12]!=op->source_stride||
       (s->gpu_state_bank[0x13]&0xffff)!=op->source_width||(s->gpu_state_bank[0x13]>>16)!=op->source_height)return false;
    uint64_t bytes=(uint64_t)op->source_stride*op->source_height;
    MemoryRegionSection source=memory_region_find(get_system_memory(),op->source,bytes);
    MemoryRegionSection previous=memory_region_find(get_system_memory(),s->gpu_state_bank[0x11],bytes);
    bool match=source.mr&&source.mr==previous.mr&&source.offset_within_region==previous.offset_within_region&&
        int128_ge(source.size,int128_make64(bytes))&&int128_ge(previous.size,int128_make64(bytes));
    if(source.mr)memory_region_unref(source.mr);if(previous.mr)memory_region_unref(previous.mr);return match;
}
static void marconi_gpu_fill_irq_refresh(MarconiProbeState *s)
{
    bool level=!s->reported&&(s->gpu_boot_reset.interrupt_pending&s->gpu_boot_reset.interrupt_enable)!=0;
    if(level==s->gpu_fill_irq_level)return;
    s->gpu_fill_irq_level=level;
    if(level)++s->gpu_fill_irq_asserts;else ++s->gpu_fill_irq_deasserts;
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->armv7m),43),level);
    marconi_public_report("MARCONI_GPU_FILL_IRQ level=%u external=43 asserts=%" PRIu64 " deasserts=%" PRIu64
        " actual-END-and-enable=true",level,s->gpu_fill_irq_asserts,s->gpu_fill_irq_deasserts);
}
static void marconi_gpu_fill_free(MarconiProbeState *s)
{
    if(s->gpu_fill){int64_t host_start=g_get_monotonic_time();o62m_virtual_otp_wipe_v1(s->gpu_fill,sizeof(*s->gpu_fill));g_free(s->gpu_fill);s->gpu_fill=NULL;s->gpu_host_free_us+=g_get_monotonic_time()-host_start;}
}
static void marconi_gpu_fill_cancel(MarconiProbeState *s)
{
    if(s->gpu_fill_event)timer_del(s->gpu_fill_event);
    if(s->gpu_fill){++s->gpu_fill_cancelled;marconi_gpu_fill_free(s);}
    ++s->gpu_fill_generation;s->gpu_fill_deadline=0;s->gpu_address_staged=false;
    s->gpu_boot_reset.command_address=0;s->gpu_boot_reset.command_size=0;s->gpu_boot_reset.fetch_position=0;
    s->gpu_boot_reset.interrupt_pending=0;marconi_gpu_fill_irq_refresh(s);
}
static void marconi_gpu_state_clear(MarconiProbeState *s)
{
    o62m_virtual_otp_wipe_v1(s->gpu_state_bank,sizeof(s->gpu_state_bank));
    memset(s->gpu_state_known,0,sizeof(s->gpu_state_known));
}
static void marconi_gpu_fill_reset(void *opaque)
{
    MarconiProbeState *s=opaque;marconi_gpu_fill_cancel(s);marconi_gpu_state_clear(s);
    timer_del(s->gpu_boot_event);s->gpu_boot_reset.phase=O62M_GPU_NEW;
    ++s->gpu_boot_reset.generation;s->gpu_boot_reset.deadline_ns=0;
    s->gpu_boot_reset.interrupt_enable=0;marconi_gpu_fill_irq_refresh(s);
}
static void marconi_dpu_frame_irq(MarconiProbeState *s)
{ qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->armv7m),41),!s->reported&&(s->dpu_irq_pending&s->dpu_irq_mask&1)); }
static bool marconi_dpu_frame_source(MarconiProbeState *s)
{ return !s->reported&&(s->dpu_boot_config[0]&1)&&marconi_display_short_eligible(s); }
#define MARCONI_FRAME_CAPACITY (32u + 1024u * 1024u * 3u)
static bool marconi_dpu_frame_capture(MarconiProbeState *s,const uint8_t *pixels)
{
    const O62MDpuFrameV1 *f=&s->dpu_frame;
    if(!s->lcd_frame_file)return true;
#ifndef _WIN32
    if(!s->lcd_shared_frame) {
        int fd=open(s->lcd_frame_file,O_RDWR|O_CREAT|O_TRUNC,0600);
        if(fd<0)return false;
        if(ftruncate(fd,MARCONI_FRAME_CAPACITY)<0){close(fd);return false;}
        void *mapped=mmap(NULL,MARCONI_FRAME_CAPACITY,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        close(fd);if(mapped==MAP_FAILED)return false;
        s->lcd_shared_frame=mapped;memset(mapped,0,MARCONI_FRAME_CAPACITY);
        memcpy(mapped,"Q66FRAME",8);stl_le_p(s->lcd_shared_frame+8,1);
        stl_le_p(s->lcd_shared_frame+28,2);
    }
    uint8_t *frame=s->lcd_shared_frame;
    uint32_t next=s->lcd_shared_sequence+2;if(!next)next=2;
    qatomic_set((uint32_t *)(frame+12),cpu_to_le32(next-1));smp_wmb();
    stl_le_p(frame+16,f->width);stl_le_p(frame+20,f->height);stl_le_p(frame+24,f->width*3);
    uint8_t *rgb=frame+32;
    for(uint64_t i=0;i<f->bytes;i+=4){*rgb++=pixels[i+2];*rgb++=pixels[i+1];*rgb++=pixels[i];}
    smp_wmb();qatomic_set((uint32_t *)(frame+12),cpu_to_le32(next));s->lcd_shared_sequence=next;
    return true;
#else
    return false;
#endif
}
static void marconi_dpu_frame_callback(void *opaque)
{
    MarconiProbeState *s=opaque;if(!s->dpu_frame_pending)return;
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(now<s->dpu_frame_deadline){timer_mod_ns(s->dpu_frame_event,s->dpu_frame_deadline);return;}
    const O62MDpuFrameV1 *f=&s->dpu_frame;
    if(!marconi_dpu_frame_source(s)||marconi_gpu_diagnostic_ram_owner(s,f->source,f->span)<0)goto fail;
    uint8_t *pixels=g_malloc(f->bytes);bool ok=true;MemTxAttrs attrs={.secure=false,.user=false};
    for(unsigned y=0;y<f->height&&ok;y++)ok=address_space_read(cpu_get_address_space(CPU(s->armv7m.cpu),ARMASIdx_NS),
        (uint64_t)f->source+(uint64_t)y*f->stride,attrs,pixels+(size_t)y*f->width*4,f->width*4)==MEMTX_OK;
    if(ok)ok=marconi_dpu_frame_capture(s,pixels);
    o62m_virtual_otp_wipe_v1(pixels,f->bytes);g_free(pixels);if(!ok)goto fail;
    s->dpu_frame_pending=false;++s->dpu_frame_completions;s->dpu_irq_pending|=1;marconi_dpu_frame_irq(s);
    if(s->dpu_frame_completions<=8)marconi_public_report("MARCONI_DPU_FRAME_COMPLETE frames=%" PRIu64
        " width=%u height=%u actual-DMA-and-private-scanout=true completion-bit-after-work=true",s->dpu_frame_completions,f->width,f->height);
    return;
fail:
    marconi_public_report("MARCONI_DPU_FRAME_BOUNDARY actual-frame-work-incomplete=true no-completion-published=true");
    marconi_owned_runtime_failure_stop(s);
}
static bool marconi_dpu_frame_submit(MarconiProbeState *s)
{
    O62MDpuFrameV1 f;bool known=o62m_dpu_frame_plan_v1(&f,s->dpu_boot_config[1],s->dpu_channel_config);
    int owner=known?marconi_gpu_diagnostic_ram_owner(s,f.source,f.span):-1;
    if(s->dpu_frame_pending||(s->dpu_irq_pending&1)||s->dpu_channel_written!=63||!marconi_dpu_frame_source(s)||!known||owner<0){
        marconi_public_report("MARCONI_DPU_FRAME_ADMISSION configured=%u source-ready=%u supported-geometry-format=%u RAM-owner=%d busy=%u completion-pending=%u",
            s->dpu_channel_written==63,marconi_dpu_frame_source(s),known,owner,s->dpu_frame_pending,(s->dpu_irq_pending&1)!=0);return false;
    }
    s->dpu_frame=f;s->dpu_frame_pending=true;++s->dpu_frame_requests;
    s->dpu_frame_deadline=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)+(f.bytes*UINT64_C(1000000000)+134217727)/134217728;
    timer_mod_ns(s->dpu_frame_event,s->dpu_frame_deadline);
    if(s->dpu_frame_requests<=8)marconi_public_report("MARCONI_DPU_FRAME_SUBMIT requests=%" PRIu64 " width=%u height=%u source-private=true actual-F4-strobe=true",s->dpu_frame_requests,f.width,f.height);
    return true;
}
static void marconi_dpu_frame_reset(void *opaque)
{
    MarconiProbeState *s=opaque;timer_del(s->dpu_frame_event);
    if(s->dpu_frame_pending)++s->dpu_frame_cancelled;
    s->dpu_frame_pending=false;s->dpu_irq_pending&=~1u;marconi_dpu_frame_irq(s);
    o62m_virtual_otp_wipe_v1(&s->dpu_frame,sizeof(s->dpu_frame));
    o62m_virtual_otp_wipe_v1(s->dpu_channel_config,sizeof(s->dpu_channel_config));s->dpu_channel_written=0;
}
static void marconi_dpu_frame_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,dpu_frame_exit);(void)opaque;
    marconi_dpu_frame_reset(s);
#ifndef _WIN32
    if(s->lcd_shared_frame){munmap(s->lcd_shared_frame,MARCONI_FRAME_CAPACITY);s->lcd_shared_frame=NULL;}
#endif
    marconi_public_report("MARCONI_DPU_FRAME_EXIT requests=%" PRIu64 " completed=%" PRIu64 " cancelled=%" PRIu64 " status-read-acks=%" PRIu64 " timer-cancelled-and-private-state-wiped=true",
        s->dpu_frame_requests,s->dpu_frame_completions,s->dpu_frame_cancelled,s->dpu_frame_status_acks);
}
static void marconi_gpu_fill_early_classify(MarconiProbeState *s,uint64_t slots)
{
    uint64_t address=s->gpu_boot_reset.command_address,bytes=slots&&slots<=131072?slots*8:0,left=bytes,at=address;
    unsigned parts=0,mask=0;bool all=bytes!=0&&bytes<=UINT64_C(0x100000000)-address;
    MemoryRegion *owners[]={&s->handoff_ap_ram,&s->ap_uncache_ram,&s->gpu_sys_low_ram,&s->ap_boot_buffer_ram,&s->gpu_sys_high_ram};
    while(all&&left&&parts<8) {
        MemoryRegionSection section=memory_region_find(get_system_memory(),at,left);int owner=-1;
        for(unsigned i=0;i<G_N_ELEMENTS(owners);i++)if(section.mr==owners[i])owner=i;
        uint64_t span=int128_get64(section.size);
        if(owner<0||!span||span>left||!memory_region_is_ram(section.mr)||memory_region_is_rom(section.mr))all=false;
        else {mask|=1u<<owner;at+=span;left-=span;++parts;}
        if(section.mr)memory_region_unref(section.mr);
    }
    all=all&&!left;
    unsigned size_class=!slots?0:slots<=4096?1:slots<=8192?2:slots<=32768?3:slots<=131072?4:5;
    marconi_public_report("MARCONI_GPU_FILL_EARLY_BOUNDARY nonzero-slots=%u within4096=%u size-class=%u staged=%u busy=%u phase-idle=%u open=%u "
        "first-byte-owner=%d full-single-owner=%d complete-known-RAM-span=%u sections=%u known-owner-mask=%u no-address-or-payload-output=true",
        slots!=0,slots<=4096,size_class,s->gpu_address_staged,s->gpu_fill!=NULL,s->gpu_boot_reset.phase==O62M_GPU_IDLE,
        o62m_gpu_boot_open_v1(s->gpu_boot_reset.control),marconi_gpu_diagnostic_ram_owner(s,address,1),
        bytes?marconi_gpu_diagnostic_ram_owner(s,address,bytes):-1,all,parts,mask);
}
static bool marconi_gpu_fill_begin(MarconiProbeState *s,uint64_t slots)
{
    if(s->gpu_fill||!s->gpu_address_staged||!slots||slots>8192||
       s->gpu_boot_reset.phase!=O62M_GPU_IDLE||!o62m_gpu_boot_open_v1(s->gpu_boot_reset.control)||
       s->gpu_fill_generation==UINT64_MAX||s->gpu_fill_submissions==UINT64_MAX){marconi_gpu_fill_early_classify(s,slots);return false;}
    size_t bytes=slots*8;uint32_t address=s->gpu_boot_reset.command_address;
    int cmd_owner=marconi_gpu_diagnostic_ram_owner(s,address,bytes);
    if(cmd_owner<0){marconi_gpu_fill_early_classify(s,slots);return false;}
    uint8_t *raw=g_malloc0(bytes);O62MGpuBatchV1 *plan=g_new0(O62MGpuBatchV1,1);
    MemTxAttrs attrs={.secure=false,.user=false};
    MemTxResult tx=address_space_read(cpu_get_address_space(CPU(s->armv7m.cpu),ARMASIdx_NS),address,attrs,raw,bytes);
    int64_t host_parse_start=g_get_monotonic_time();
    unsigned result=tx==MEMTX_OK?o62m_gpu_batch_parse_v1(plan,raw,bytes,s->gpu_state_bank,s->gpu_state_known):O62M_FILL_FRAMING;
    s->gpu_host_parse_us+=g_get_monotonic_time()-host_parse_start;
    o62m_virtual_otp_wipe_v1(raw,bytes);g_free(raw);
    int target_owner=-1,tess_owner=-1,count_owner=-1;
    if(result==O62M_FILL_OK&&plan->op_count) {
        target_owner=marconi_gpu_diagnostic_ram_owner(s,plan->target,(uint64_t)plan->stride*plan->height);
        tess_owner=marconi_gpu_diagnostic_ram_owner(s,plan->tess_address,plan->tess_bytes);
        count_owner=marconi_gpu_diagnostic_ram_owner(s,plan->count_address,plan->count_bytes);
    }
    bool sources_valid=result==O62M_FILL_OK;
    if(result==O62M_FILL_OK)for(unsigned i=0;i<plan->op_count;i++) {
        O62MGpuBatchOpV1 *op=&plan->ops[i];if(!op->image)continue;
        uint64_t span=(uint64_t)op->source_stride*op->source_height,target_span=(uint64_t)plan->stride*plan->height;
        bool overlap=marconi_gpu_ram_overlap(op->source,span,plan->target,target_span);
        sources_valid&=!overlap&&marconi_gpu_diagnostic_ram_owner(s,op->source,span)>=0;
        op->source_matches_completed_state_target=(op->source_config==0x01020107||op->source_config==0x01000107)&&marconi_gpu_source_matches_completed_state_target(s,op);
    }
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),latency=1000+plan->clip_pixels*16;
    if(result||(plan->op_count&&(target_owner<0||tess_owner<0||count_owner<0))||!sources_valid||latency>INT64_MAX-now) {
        marconi_public_report("MARCONI_GPU_COMMAND_REJECT opcode=%u selector=%u byte-offset=%u total-bytes=%u prior-ops=%u word-zero=%u waiting=%u last-barrier=%u private-data-redacted=true",plan->reject_command_class,plan->reject_command_selector,plan->reject_command_offset,plan->reject_command_bytes,plan->reject_command_ops,plan->reject_command_word_zero,plan->reject_command_waiting,plan->reject_command_barrier);
        marconi_public_report("MARCONI_GPU_FILL_REJECT finite-reason=%u target-owner=%d tess-owner=%d count-owner=%d sources-valid=%u register-index=%u control-metadata=0x%08x source-format-key=0x%08x source-config-metadata=0x%08x path-config-metadata=0x%08x path-bytes=%u path-moves=%u path-lines=%u path-cubics=%u path-closes=%u path-unknown-op=%u mode-reject-mask=%u path-framing=%u path-relative=%u path-quadratics=%u target-format-metadata=0x%08x target-width=%u target-height=%u target-stride=%u scale-metadata=0x%08x bias-metadata=0x%08x solid-gray=%u solid-opaque=%u source-width=%u source-height=%u source-stride=%u no-target-write=true no-END=true",
            result,target_owner,tess_owner,count_owner,sources_valid,plan->reject_register,plan->reject_control,plan->reject_source_key,plan->reject_source_config,plan->reject_path_config,plan->path_bytes,plan->path_ops[2],plan->path_ops[4],plan->path_ops[8],plan->path_ops[1],plan->path_unknown,plan->mode_reject_mask,plan->path_framing,plan->path_ops[3]+plan->path_ops[5]+plan->path_ops[7]+plan->path_ops[9],plan->path_ops[6]+plan->path_ops[7],plan->reject_target_format,plan->reject_layout&0xffff,plan->reject_layout>>16,plan->reject_stride,plan->reject_scale,plan->reject_bias,plan->reject_solid_gray,plan->reject_solid_opaque,plan->reject_source_layout&0xffff,plan->reject_source_layout>>16,plan->reject_source_stride);
        o62m_virtual_otp_wipe_v1(plan,sizeof(*plan));g_free(plan);
        return false;
    }
    s->gpu_fill=plan;s->gpu_fill_armed=++s->gpu_fill_generation;s->gpu_fill_reset_generation=s->gpu_boot_reset.generation;
    s->gpu_fill_deadline=now+latency;++s->gpu_fill_submissions;
    s->gpu_boot_reset.command_size=(uint32_t)slots;s->gpu_boot_reset.fetch_position=address;s->gpu_address_staged=false;
    timer_mod_ns(s->gpu_fill_event,s->gpu_fill_deadline);
    if(!plan->op_count){
        marconi_public_report("MARCONI_GPU_STATE_ONLY_SUBMIT submissions=%" PRIu64 " virtual-latency-ns=%" PRId64 " command-snapshot=true pending-END=false target-access=false",s->gpu_fill_submissions,latency);
        return true;
    }
    marconi_public_report("MARCONI_GPU_FILL_SUBMIT submissions=%" PRIu64 " tiles=%u edges=%u width=%u height=%u stride=%u format=%s target-owner=%d "
        "virtual-latency-ns=%" PRId64 " command-snapshot=true pending-END=false private-inputs-redacted=true",
        s->gpu_fill_submissions,plan->tile_count,plan->edge_count,plan->width,plan->height,plan->stride,plan->target_format==0x100?"A8":plan->target_format==0x106?"L8":plan->target_format==0x102?"BGRX8888":"BGRA8888",target_owner,latency);
    return true;
}
static void marconi_gpu_fill_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(!s->gpu_fill)return;
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(s->reported||s->gpu_fill_armed!=s->gpu_fill_generation||s->gpu_fill_reset_generation!=s->gpu_boot_reset.generation||
       s->gpu_boot_reset.phase!=O62M_GPU_IDLE||!o62m_gpu_boot_open_v1(s->gpu_boot_reset.control)) {
        marconi_gpu_fill_cancel(s);return;
    }
    if(now<s->gpu_fill_deadline){timer_mod_ns(s->gpu_fill_event,s->gpu_fill_deadline);return;}
    int64_t host_callback_start=g_get_monotonic_time();
    O62MGpuBatchV1 *plan=s->gpu_fill;
    if(!plan->op_count){
        memcpy(s->gpu_state_bank,plan->final_state,sizeof(s->gpu_state_bank));
        memcpy(s->gpu_state_known,plan->final_valid,sizeof(s->gpu_state_known));++s->gpu_state_commits;
        ++s->gpu_fill_completions;
        s->gpu_boot_reset.fetch_position=s->gpu_boot_reset.command_address+s->gpu_boot_reset.command_size*8;
        s->gpu_boot_reset.interrupt_pending|=1;
        marconi_public_report("MARCONI_GPU_STATE_ONLY_COMPLETE completions=%" PRIu64 " virtual-ns=%" PRId64 " actual-state-committed=true target-access=false END0-consumed=true",s->gpu_fill_completions,now);
        marconi_gpu_fill_free(s);s->gpu_fill_deadline=0;marconi_gpu_fill_irq_refresh(s);s->gpu_host_callback_us+=g_get_monotonic_time()-host_callback_start;return;
    }
    size_t bytes=(size_t)plan->stride*plan->height;
    bool valid=marconi_gpu_diagnostic_ram_owner(s,plan->target,bytes)>=0&&
        marconi_gpu_diagnostic_ram_owner(s,plan->tess_address,plan->tess_bytes)>=0&&
        marconi_gpu_diagnostic_ram_owner(s,plan->count_address,plan->count_bytes)>=0;
    uint8_t *pixels=g_malloc0(bytes);MemTxAttrs attrs={.secure=false,.user=false};
    AddressSpace *as=cpu_get_address_space(CPU(s->armv7m.cpu),ARMASIdx_NS);
    bool rendered=valid&&address_space_read(as,plan->target,attrs,pixels,bytes)==MEMTX_OK;
    unsigned rendered_ops=0;
    for(unsigned i=0;rendered&&i<plan->op_count;i++) {
        O62MGpuBatchOpV1 *op=&plan->ops[i];uint8_t *source=NULL;size_t source_bytes=0;
        if(op->image) {
            source_bytes=(size_t)op->source_stride*op->source_height;
            rendered=marconi_gpu_diagnostic_ram_owner(s,op->source,source_bytes)>=0&&
                !marconi_gpu_ram_overlap(op->source,source_bytes,plan->target,bytes);
            if(rendered){source=g_malloc(source_bytes);rendered=address_space_read(as,op->source,attrs,source,source_bytes)==MEMTX_OK;}
        }
        if(rendered){int64_t host_render_start=g_get_monotonic_time();rendered=o62m_gpu_batch_render_op_v1(plan,i,pixels,bytes,source,source_bytes);s->gpu_host_render_us+=g_get_monotonic_time()-host_render_start;}
        if(source){o62m_virtual_otp_wipe_v1(source,source_bytes);g_free(source);}
        if(rendered)++rendered_ops;
    }
    bool committed=rendered&&address_space_write(as,plan->target,attrs,pixels,bytes)==MEMTX_OK;
    o62m_virtual_otp_wipe_v1(pixels,bytes);g_free(pixels);
    if(!committed) {
        marconi_public_report("MARCONI_GPU_FILL_WORK_REJECT resources-valid=%u render-succeeded=%u rendered-ops=%u END-published=false",valid,rendered,rendered_ops);
        marconi_gpu_fill_cancel(s);marconi_owned_runtime_failure_stop(s);return;
    }
    marconi_public_report("MARCONI_GPU_BATCH_WORK ordered-ops=%u solid-ops=%u image-ops=%u path-moves=%u path-lines=%u path-quadratics=%u path-cubics=%u all-private-render-before-target-commit=true source-buffers-wiped=true",rendered_ops,plan->solid_ops,plan->image_ops,plan->path_ops[2],plan->path_ops[4],plan->path_ops[6],plan->path_ops[8]);
    memcpy(s->gpu_state_bank,plan->final_state,sizeof(s->gpu_state_bank));
    memcpy(s->gpu_state_known,plan->final_valid,sizeof(s->gpu_state_known));++s->gpu_state_commits;
    ++s->gpu_fill_completions;
    s->gpu_boot_reset.fetch_position=s->gpu_boot_reset.command_address+s->gpu_boot_reset.command_size*8;
    s->gpu_boot_reset.interrupt_pending|=1;
    marconi_public_report("MARCONI_GPU_FILL_COMPLETE completions=%" PRIu64 " covered-samples=%" PRIu64 " per-operation-changed-pixels=%" PRIu64
        " virtual-ns=%" PRId64 " width=%u height=%u stride=%u format=%s actual-guest-target-written=true original-LVGL-source=true END0-consumed=true silicon-AA-bitexact=false",
        s->gpu_fill_completions,plan->covered_samples,plan->changed_pixels,now,plan->width,plan->height,plan->stride,plan->target_format==0x100?"A8":plan->target_format==0x106?"L8":plan->target_format==0x102?"BGRX8888":"BGRA8888");
    marconi_gpu_fill_free(s);s->gpu_fill_deadline=0;marconi_gpu_fill_irq_refresh(s);s->gpu_host_callback_us+=g_get_monotonic_time()-host_callback_start;
}

static void marconi_gpu_boot_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->reported||s->gpu_boot_reset.phase!=O62M_GPU_RESET_WORK){timer_del(s->gpu_boot_event);return;}
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(now<s->gpu_boot_reset.deadline_ns){timer_mod_ns(s->gpu_boot_event,s->gpu_boot_reset.deadline_ns);return;}
    if(!o62m_gpu_boot_complete_v1(&s->gpu_boot_reset,s->gpu_boot_armed_generation,now)) {
        marconi_public_report("MARCONI_GPU_BOOT_RESET_BOUNDARY completion-rejected=true");marconi_owned_runtime_failure_stop(s);return;
    }
    marconi_public_report("MARCONI_GPU_BOOT_RESET_COMPLETE generation=%" PRIu64 " completions=%" PRIu64
        " real-reset-request-release-and-timer=true command-front-end-cleared=true supported-engine-empty=true",s->gpu_boot_reset.generation,s->gpu_boot_reset.resets_completed);
}
static void marconi_gpu_boot_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,gpu_boot_exit);(void)opaque;
    timer_del(s->gpu_boot_event);
    marconi_gpu_fill_cancel(s);marconi_gpu_state_clear(s);
    marconi_public_report("MARCONI_GPU_HOST_TIME parse-us=%" PRIu64 " render-us=%" PRIu64 " GPU-capture-us=%" PRIu64 " plan-free-us=%" PRIu64 " callback-us=%" PRIu64 " DPU-capture-us=%" PRIu64 " guest-clock-unchanged=true",s->gpu_host_parse_us,s->gpu_host_render_us,s->gpu_host_capture_us,s->gpu_host_free_us,s->gpu_host_callback_us,s->dpu_host_capture_us);
    marconi_public_report("MARCONI_GPU_STATE_EXIT completed-bank-commits=%" PRIu64 " private-bank-wiped=%u",s->gpu_state_commits,o62m_virtual_otp_zero_v1(s->gpu_state_bank,sizeof(s->gpu_state_bank)));
    marconi_public_report("MARCONI_GPU_FILL_EXIT submissions=%" PRIu64 " completions=%" PRIu64 " cancelled=%" PRIu64
        " private-plan-wiped=%u IRQ-low=%u timer-cancelled=true",s->gpu_fill_submissions,s->gpu_fill_completions,s->gpu_fill_cancelled,
        s->gpu_fill==NULL,!s->gpu_fill_irq_level);
    o62m_virtual_otp_wipe_v1(&s->gpu_boot_reset.command_address,sizeof(s->gpu_boot_reset.command_address));
    marconi_public_report("MARCONI_GPU_ADDRESS_EXIT writes=%" PRIu64 " staged=%u private-address-wiped=%u",
        s->gpu_address_writes,s->gpu_address_staged,
        o62m_virtual_otp_zero_v1(&s->gpu_boot_reset.command_address,sizeof(s->gpu_boot_reset.command_address)));
    s->gpu_address_staged=false;
    marconi_public_report("MARCONI_GPU_BOOT_EXIT reset-completions=%" PRIu64 " pending=%u reads=%" PRIu64 " writes=%" PRIu64 " timer-cancelled=true",
        s->gpu_boot_reset.resets_completed,s->gpu_boot_reset.phase==O62M_GPU_RESET_WORK,s->gpu_boot_reads,s->gpu_boot_writes);
}

static void marconi_rtc_calibration_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->reported) {timer_del(s->rtc_calibration_event);o62m_rtc_calibration_cancel_v1(&s->rtc_calibration);return;}
    if(!s->rtc_calibration.pending)return;
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(!o62m_rtc_calibration_complete_v1(&s->rtc_calibration,s->rtc_calibration_armed_generation,&s->rtc32k_reference,now)) {
        timer_del(s->rtc_calibration_event);o62m_rtc_calibration_cancel_v1(&s->rtc_calibration);
        marconi_public_report("MARCONI_RTC_CALIBRATION_CANCELLED source-or-generation-invalid=true no-result-published=true");
        marconi_owned_runtime_failure_stop(s);return;
    }
    marconi_public_report("MARCONI_RTC_CALIBRATION_LATCH generation=%" PRIu64 " completions=%" PRIu64
        " virtual-ns=%" PRId64 " computed-reference-count=true internal-latch-only=true no-B4-done-or-IRQ=true",
        s->rtc_calibration.generation,s->rtc_calibration.completions,now);
}
static void marconi_rtc32k_reference_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->reported) {timer_del(s->rtc32k_reference_event);o62m_rtc32k_stop_v1(&s->rtc32k_reference);return;}
    if(!s->rtc32k_reference.pending)return;
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(!o62m_rtc32k_complete_v1(&s->rtc32k_reference,s->rtc32k_armed_generation,now)) {
        error_report("MARCONI_RTC32K_REFERENCE_BOUNDARY invalid-qualification-callback=true");
        timer_del(s->rtc32k_reference_event);o62m_rtc32k_stop_v1(&s->rtc32k_reference);
        marconi_owned_runtime_failure_stop(s);return;
    }
    marconi_public_report("MARCONI_RTC32K_REFERENCE_QUALIFIED generation=%" PRIu64 " completions=%" PRIu64
        " virtual-ns=%" PRId64 " actual-32-reference-cycles=true later-calibration-not-complete=true",
        s->rtc32k_reference.generation,s->rtc32k_reference.completions,now);
}
static void marconi_rtc32k_reference_reset(void *opaque)
{
    MarconiProbeState *s=opaque;
    timer_del(s->rtc_calibration_event);o62m_rtc_calibration_cancel_v1(&s->rtc_calibration);
    timer_del(s->rtc32k_reference_event);
    if(s->reported) {o62m_rtc32k_stop_v1(&s->rtc32k_reference);return;}
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(!o62m_rtc32k_source_start_v1(&s->rtc32k_reference,s->rtc32k_reference_available,now)) {
        error_report("MARCONI_RTC32K_REFERENCE_BOUNDARY invalid-source-start=true");
        timer_del(s->rtc32k_reference_event);o62m_rtc32k_stop_v1(&s->rtc32k_reference);
        marconi_owned_runtime_failure_stop(s);return;
    }
    s->rtc32k_armed_generation=s->rtc32k_reference.generation;
    if(s->rtc32k_reference.pending)timer_mod_ns(s->rtc32k_reference_event,s->rtc32k_reference.deadline_ns);
    marconi_public_report("MARCONI_RTC32K_REFERENCE_SOURCE_START generation=%" PRIu64
        " available=%u pending=%u virtual-ns=%" PRId64 " independent-reference-input=true ready-cleared=true",
        s->rtc32k_reference.generation,s->rtc32k_reference_available,s->rtc32k_reference.pending,now);
}
static void marconi_rtc32k_reference_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,rtc32k_reference_exit);(void)opaque;
    timer_del(s->rtc32k_reference_event);
    marconi_public_report("MARCONI_RTC32K_REFERENCE_EXIT starts=%" PRIu64 " completions=%" PRIu64
        " pending=%u owned-timer-cancelled=true",s->rtc32k_reference.starts,s->rtc32k_reference.completions,s->rtc32k_reference.pending);
    timer_del(s->rtc_calibration_event);
    marconi_public_report("MARCONI_RTC_CALIBRATION_EXIT requests=%" PRIu64 " completions=%" PRIu64
        " pending=%u owned-timer-cancelled=true",s->rtc_calibration.requests,s->rtc_calibration.completions,s->rtc_calibration.pending);
    o62m_rtc_calibration_cancel_v1(&s->rtc_calibration);
    o62m_rtc32k_stop_v1(&s->rtc32k_reference);
}

static void marconi_pmic_voltage_schedule(MarconiProbeState *s)
{
    if(s->pmic_voltage.pending&&!s->reported) {
        s->pmic_voltage_armed_generation=s->pmic_voltage.generation;
        timer_mod_ns(s->pmic_voltage_event,s->pmic_voltage.deadline_ns);
    } else timer_del(s->pmic_voltage_event);
}
static bool marconi_pmic_voltage_refresh(MarconiProbeState *s)
{
    if(!s->pmic_voltage_source_profile)return true;
    if(s->reported){timer_del(s->pmic_voltage_event);return false;}
    if(!s->pmic_voltage.pending){timer_del(s->pmic_voltage_event);return true;}
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(now<s->pmic_voltage.deadline_ns)return true;
    if(!o62m_pmic_voltage_complete_v6(&s->pmic_voltage,s->pmic_voltage_armed_generation,now)) {
        marconi_public_report("MARCONI_PMIC_VOLTAGE_BOUNDARY stage=sample-completion no-sample-or-ready-created=true");
        marconi_owned_runtime_failure_stop(s);return false;
    }
    timer_del(s->pmic_voltage_event);
    marconi_public_report("MARCONI_PMIC_VOLTAGE_COMPLETE generation=%" PRIu64 " completions=%" PRIu64
        " actual-positive-virtual-delay-and-source-latch=true coherent-voltage-and-current24-latches=true no-physical-measurement-or-IRQ=true raw-sample-redacted=true",
        s->pmic_voltage.generation,s->pmic_voltage.completions);
    return true;
}
static void marconi_pmic_voltage_callback(void *opaque)
{ (void)marconi_pmic_voltage_refresh(opaque); }
static void marconi_pmic_voltage_reset(void *opaque)
{
    MarconiProbeState *s=opaque;uint32_t f0=0;
    timer_del(s->pmic_voltage_event);
    if(!o62m_pmic_voltage_reset_v6(&s->pmic_voltage,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))||
       o62m_clock_controls_read(&s->pmic_mode_config,0xf0,4,&f0)!=O62M_CLOCK_CONTROLS_OK||
       !o62m_pmic_voltage_gate_v6(&s->pmic_voltage,f0,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
        marconi_owned_runtime_failure_stop(s);return;
    }
}
static void marconi_pmic_voltage_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,pmic_voltage_exit);(void)opaque;
    timer_del(s->pmic_voltage_event);
    marconi_public_report("MARCONI_PMIC_VOLTAGE_EXIT requests=%" PRIu64 " completions=%" PRIu64
        " pending=%u cancellations=%" PRIu64 " timer-cancelled=true raw-sample-redacted=true",
        s->pmic_voltage.requests,s->pmic_voltage.completions,s->pmic_voltage.pending,s->pmic_voltage.cancellations);
    s->pmic_voltage.pending=false;s->pmic_voltage.valid=false;s->pmic_voltage.ready=false;s->pmic_voltage.latched_raw24=0;s->pmic_voltage.latched_current_raw24=0;
    s->pmic_voltage.latched_positive=0;s->pmic_voltage.latched_negative=0;
    s->pmic_voltage.charge_positive=0;s->pmic_voltage.charge_negative=0;s->pmic_voltage.charge_fraction_ns=0;
    memset(s->pmic_voltage.voltage_history,0,sizeof(s->pmic_voltage.voltage_history));
    memset(s->pmic_voltage.current_history,0,sizeof(s->pmic_voltage.current_history));
    memset(s->pmic_voltage.latched_voltage_history,0,sizeof(s->pmic_voltage.latched_voltage_history));
    memset(s->pmic_voltage.latched_current_history,0,sizeof(s->pmic_voltage.latched_current_history));
    s->pmic_voltage.history_count=s->pmic_voltage.latched_history_count=0;
    s->pmic_voltage.sample_ticks=s->pmic_voltage.latched_sample_ticks=0;
}

/* Only the prefix of already accepted shared-SPMI transactions is covered.
 * ACK says nothing about WAIT_BUS producers or downstream ADC/DVS settle. */
static void marconi_pmic_sync107c_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->reported) {timer_del(s->pmic_sync107c_event);return;}
    bool present=false;int64_t deadline=0;
    int result=o62m_pmic_sync107c_next_deadline_v1(&s->pmic_sync107c,&present,&deadline);
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(result==O62M_PMIC_SYNC107C_OK_V1&&!present) {timer_del(s->pmic_sync107c_event);return;}
    if(result==O62M_PMIC_SYNC107C_OK_V1&&now>=0&&now<deadline) {timer_mod_ns(s->pmic_sync107c_event,deadline);return;}
    if(result==O62M_PMIC_SYNC107C_OK_V1)result=o62m_pmic_sync107c_complete_v1(&s->pmic_sync107c,now,s->pmic_sync107c_armed_generation);
    if(result!=O62M_PMIC_SYNC107C_OK_V1) {
        marconi_public_report("MARCONI_PMIC_SYNC107C_BOUNDARY reason-code=%d actual-timer-or-strict-failure=true no-register-output=true",result);
        marconi_owned_runtime_failure_stop(s);return;
    }
    marconi_public_report("MARCONI_PMIC_SYNC107C_COMPLETE generation=%" PRIu64 " requests=%" PRIu64
        " completions=%" PRIu64 " reads-while-pending=%" PRIu64
        " actual-prior-successful-peercommit-and-positive-elapsed-QEMU-timer=true accepted-SPMI-prefix-only=true "
        "no-WAIT-BUS-ADC-DVS-settle-IRQ-or-physical-ACK-claim=true raw-register-redacted=true",
        s->pmic_sync107c.generation,s->pmic_sync107c.requests,s->pmic_sync107c.completions,s->pmic_sync107c_pending_reads);
}
static void marconi_pmic_sync107c_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,pmic_sync107c_exit);(void)opaque;
    timer_del(s->pmic_sync107c_event);
    marconi_public_report("MARCONI_PMIC_SYNC107C_EXIT requests=%" PRIu64 " completions=%" PRIu64
        " pending=%u reads-while-pending=%" PRIu64 " timer-cancelled=true no-register-values=true",
        s->pmic_sync107c.requests,s->pmic_sync107c.completions,s->pmic_sync107c.pending,s->pmic_sync107c_pending_reads);
}

/* Host contact input reaches the original driver through the attached peer. */
static bool marconi_touch_fail(MarconiProbeState *s,const char *stage,uint16_t reg)
{
    if(!s->reported){
        marconi_public_report("MARCONI_TOUCH_BOUNDARY stage=%s register-address=%04x no-payload-output=true",stage,reg);
        marconi_owned_runtime_failure_stop(s);
    }
    return false;
}
static void marconi_touch_refresh(MarconiProbeState *s)
{
    if(!s->touch_event)return;
    O62MTouchCST9220V3 *t=&s->touch;
    const uint32_t bit=1u<<9;
    bool powered=(s->ap_gpio_output_latch[0]&s->ap_gpio_direction[0]&(1u<<25))&&
                 (s->ap_gpio_output_latch[1]&s->ap_gpio_direction[1]&(1u<<10));
    if(t->powered&&!powered){o62m_touch_reset_v3(t);timer_del(s->touch_event);}
    t->powered=powered;
    o62m_touch_clock_v3(t,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    bool enabled=(s->ap_gpio_irq_config[1][0]&bit)!=0;
    bool masked=(s->ap_gpio_irq_config[1][1]&bit)!=0;
    bool routed=(s->ap_aon_iosleep_words[1]&bit)!=0;
    bool input=!(s->ap_gpio_direction[1]&bit);
    bool available=t->count&&o62m_touch_scanning_v3(t);
    bool low=available&&!t->rearm;
    bool edge=(s->ap_gpio_irq_config[1][2]&bit)!=0;
    bool high=(s->ap_gpio_irq_config[1][3]&bit)!=0;
    bool both=(s->ap_gpio_irq_config[1][5]&bit)!=0;
    if(input){
        if(edge&&low!=t->irq_low&&(both||high==!low))t->pending=true;
        if(!edge)t->pending=powered&&(high==!low);
    }
    t->irq_low=low;
    bool level=!s->reported&&input&&enabled&&!masked&&routed&&t->pending;
    if(level!=s->touch_irq_level){s->touch_irq_level=level;qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->armv7m),77),level);}
    if(!s->reported&&available&&t->rearm&&enabled&&!masked&&routed&&input){
        if(!timer_pending(s->touch_event))timer_mod_ns(s->touch_event,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)+100000);
    } else timer_del(s->touch_event);
}
static void marconi_touch_timer(void *opaque)
{
    MarconiProbeState *s=opaque;
    s->touch.rearm=false;marconi_touch_refresh(s);
}
static void marconi_touch_mouse(void *opaque,int x,int y,int z,int buttons)
{
    MarconiProbeState *s=opaque;
    if(z||s->reported)return;
    marconi_touch_refresh(s);
    unsigned px=(unsigned)CLAMP(x,0,32767)*465/32767;
    unsigned py=(unsigned)CLAMP(y,0,32767)*465/32767;
    bool incoming_down=(buttons&MOUSE_EVENT_LBUTTON)!=0;
    if(incoming_down!=s->touch.host_down) {
      int64_t virtual_ns=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),host_ns=get_clock();
      for(unsigned bank=0;bank<2;bank++) {
        SysTickState *st=&s->armv7m.systick[bank];
        marconi_public_report("MARCONI_TOUCH_CLOCK down=%u systick-bank=%u virtual-ns=%" PRId64 " host-ns=%" PRId64 " enabled=%u irq-enabled=%u cpu-source=%u reload=%u input-Hz=%" PRIu64 " AP-timer-source-Hz=%" PRIu64 " AP-timer-divisor=%u AP-timer-load=%" PRIu64 " AP-timer-expirations=%" PRIu64 " timing-fields-only=true",
          incoming_down,bank,virtual_ns,host_ns,!!(st->control&1),!!(st->control&2),!!(st->control&4),st->reload,(uint64_t)clock_get_hz((st->control&4)?st->cpuclk:st->refclk),s->timer64_model.source_hz,s->timer64_model.divisor,s->timer64_model.load,s->timer64_model.expirations);
      }
    }
    uint64_t before=s->touch.inputs;
    bool ok=o62m_touch_contact_v3(&s->touch,px,py,(buttons&MOUSE_EVENT_LBUTTON)!=0);
    if(!ok&&!(buttons&MOUSE_EVENT_LBUTTON)&&s->touch.host_down){marconi_touch_fail(s,"release-unavailable",0xd000);return;}
    if(!ok){marconi_public_report("MARCONI_TOUCH_INPUT_REJECT powered=%u mode=%u queued=%u",s->touch.powered,s->touch.mode,s->touch.count);return;}
    if(s->touch.inputs!=before)marconi_public_report("MARCONI_TOUCH_INPUT count=%" PRIu64 " down=%u x=%u y=%u queued=%u",s->touch.inputs,s->touch.host_down,px,py,s->touch.count);
    marconi_touch_refresh(s);
}
static bool marconi_touch_packet(MarconiProbeState *s)
{
    uint16_t reg=(s->touch.tx[0]<<8)|s->touch.tx[1];
    uint64_t before=s->touch.acks;
    if(!o62m_touch_packet_v3(&s->touch))return marconi_touch_fail(s,"write",reg);
    if(s->touch.acks!=before)marconi_public_report("MARCONI_TOUCH_ACK reports=%" PRIu64 " queued=%u",s->touch.acks,s->touch.count);
    marconi_touch_refresh(s);return true;
}
static int marconi_touch_i2c_event(I2CSlave *dev,enum i2c_event event)
{
    MarconiTouchPeer *peer=(MarconiTouchPeer *)dev;MarconiProbeState *s=peer->owner;
    marconi_touch_refresh(s);
    if(event==I2C_START_SEND||event==I2C_START_RECV){
        if(!s->touch.powered||s->reported)return 1;
        if(!marconi_touch_packet(s))return 1;
    } else if(event==I2C_FINISH){(void)marconi_touch_packet(s);}
    return 0;
}
static int marconi_touch_i2c_send(I2CSlave *dev,uint8_t value)
{
    MarconiTouchPeer *peer=(MarconiTouchPeer *)dev;MarconiProbeState *s=peer->owner;
    if(s->touch.tx_count==sizeof(s->touch.tx)){marconi_touch_fail(s,"write-length",s->touch.pointer);return 1;}
    s->touch.tx[s->touch.tx_count++]=value;return 0;
}
static uint8_t marconi_touch_i2c_recv(I2CSlave *dev)
{
    MarconiTouchPeer *peer=(MarconiTouchPeer *)dev;MarconiProbeState *s=peer->owner;uint8_t value=0;
    if(!o62m_touch_read_v3(&s->touch,s->touch.pointer,&value))marconi_touch_fail(s,"read",s->touch.pointer);
    else {s->touch.pointer++;s->touch.reads++;}
    return value;
}
static void marconi_touch_class_init(ObjectClass *oc,const void *data)
{
    I2CSlaveClass *k=I2C_SLAVE_CLASS(oc);(void)data;
    k->event=marconi_touch_i2c_event;k->send=marconi_touch_i2c_send;k->recv=marconi_touch_i2c_recv;
}
static const TypeInfo marconi_touch_type={.name="o62m-cst9220-touch",.parent=TYPE_I2C_SLAVE,
    .instance_size=sizeof(MarconiTouchPeer),.class_init=marconi_touch_class_init};
static void marconi_touch_reset(void *opaque)
{
    MarconiProbeState *s=opaque;timer_del(s->touch_event);o62m_touch_reset_v3(&s->touch);
    s->touch_irq_level=false;qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->armv7m),77),0);
}
static void marconi_touch_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,touch_exit);(void)opaque;
    marconi_public_report("MARCONI_TOUCH_EXIT inputs=%" PRIu64 " acknowledgements=%" PRIu64 " queued=%u reads=%" PRIu64 " writes=%" PRIu64,
        s->touch.inputs,s->touch.acks,s->touch.count,s->touch.reads,s->touch.writes);
    qemu_remove_mouse_event_handler(s->touch_mouse_entry);marconi_touch_reset(s);
    o62m_virtual_ao_wipe_v1(&s->touch,sizeof(s->touch));
}
static void marconi_touch_create(MarconiProbeState *s)
{
    s->touch_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_touch_timer,s);
    marconi_touch_reset(s);
    I2CSlave *dev=i2c_slave_new("o62m-cst9220-touch",0x5a);((MarconiTouchPeer *)dev)->owner=s;
    i2c_slave_realize_and_unref(dev,s->i2c_nopeer_bus[0],&error_fatal);
    s->touch_mouse_entry=qemu_add_mouse_event_handler(marconi_touch_mouse,s,1,"XRing CST9220 touch");
    qemu_register_reset(marconi_touch_reset,s);s->touch_exit.notify=marconi_touch_exit;qemu_add_exit_notifier(&s->touch_exit);
    marconi_public_report("MARCONI_TOUCH_PROFILE controller=0 address=5a width=466 height=466 GPIO-power=25 reset=42 IRQ=41 NVIC=77 virtual-preinstalled-original-vendor-firmware=true");
}

/* Defaultoff local virtual I2C no-peer backend. Actual bus address NACK
 * is required before committing abort; never synthesize an ACK or RX byte. */
static bool marconi_i2c_nopeer_failure(MarconiProbeState *s,unsigned index,int result,const char *stage)
{
    if(s->i2c_nopeer_event[index])timer_del(s->i2c_nopeer_event[index]);
    if(!s->reported) {
        marconi_public_report("MARCONI_I2C_NOPEER_BOUNDARY controller-index=%u reason-code=%d stage=%s "
            "actual-work-or-strict-failure-only=true no-address-payload-or-register-output=true",index,result,stage);
        marconi_owned_runtime_failure_stop(s);
    }
    return false;
}
static bool marconi_i2c_nopeer_refresh(MarconiProbeState *s,unsigned index)
{
    if(!s->i2c_nopeer_profile)return true;
    if(s->reported) {timer_del(s->i2c_nopeer_event[index]);return false;}
    int64_t signed_now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(signed_now<0)return marconi_i2c_nopeer_failure(s,index,O62M_I2C_NOPEER_TIME_V3,"negative-clock");
    uint64_t now=(uint64_t)signed_now;
    O62MI2CNopeerRequestV3 request={0};
    int result=o62m_i2c_nopeer_due_v3(&s->i2c_nopeer[index],&s->ap_i2c_empty[index],now,&request);
    if(result==O62M_I2C_NOPEER_REQUEST_V3) {
        O62MI2CNopeerV3 *master=&s->i2c_nopeer[index];
        I2CBus *bus=s->i2c_nopeer_bus[index];
        int bus_result=0;uint8_t received=0;
        if(!master->bus_open||(request.command&0x400)||master->bus_read!=request.read)
            bus_result=i2c_start_transfer(bus,request.target,request.read);
        if(bus_result){
            i2c_end_transfer(bus);
            result=o62m_i2c_nopeer_complete_nack_v3(master,&s->ap_i2c_empty[index],now,request.generation,bus_result);
            if(result!=O62M_I2C_NOPEER_OK_V3)return marconi_i2c_nopeer_failure(s,index,result,"bus-NACK");
            marconi_public_report("MARCONI_I2C_NOPEER_NACK controller-index=%u nacks=%" PRIu64 " actual-QEMU-start-result=true",index,master->nacks);
        } else {
            if(request.read){
                if(master->rx_count==16)return marconi_i2c_nopeer_failure(s,index,O62M_I2C_NOPEER_OVERFLOW_V3,"RX-full");
                received=i2c_recv(bus);
            } else bus_result=i2c_send(bus,request.command&0xff);
            if(s->reported)return false;
            if(bus_result)return marconi_i2c_nopeer_failure(s,index,bus_result,"data-NACK");
            if(request.command&0x200){if(request.read)i2c_nack(bus);i2c_end_transfer(bus);}
            if(s->reported)return false;
            result=o62m_i2c_nopeer_complete_ack_v3(master,&s->ap_i2c_empty[index],now,request.generation,received);
            if(result!=O62M_I2C_NOPEER_OK_V3)return marconi_i2c_nopeer_failure(s,index,result,"byte-completion");
        }
        o62m_virtual_ao_wipe_v1(&request,sizeof(request));
    } else {
        o62m_virtual_ao_wipe_v1(&request,sizeof(request));
        if(result!=O62M_I2C_NOPEER_OK_V3)return marconi_i2c_nopeer_failure(s,index,result,"due");
    }
    bool present=false;uint64_t deadline=0;
    result=o62m_i2c_nopeer_next_deadline_v3(&s->i2c_nopeer[index],&s->ap_i2c_empty[index],&present,&deadline);
    if(result!=O62M_I2C_NOPEER_OK_V3)return marconi_i2c_nopeer_failure(s,index,result,"deadline-query");
    if(present) {
        if(deadline<=now||deadline>INT64_MAX)return marconi_i2c_nopeer_failure(s,index,O62M_I2C_NOPEER_TIME_V3,"deadline-range");
        timer_mod_ns(s->i2c_nopeer_event[index],(int64_t)deadline);
    } else timer_del(s->i2c_nopeer_event[index]);
    return true;
}
static void marconi_i2c_nopeer_callback(void *opaque)
{
    MarconiI2CNopeerContextV1 *context=opaque;
    (void)marconi_i2c_nopeer_refresh(context->owner,context->index);
}
static void marconi_i2c_nopeer_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,i2c_nopeer_exit);(void)opaque;
    for(unsigned i=0;i<5;i++) {
        if(s->i2c_nopeer_event[i])timer_del(s->i2c_nopeer_event[i]);
        if(s->i2c_nopeer_bus[i])i2c_end_transfer(s->i2c_nopeer_bus[i]);
        marconi_public_report("MARCONI_I2C_NOPEER_TOTALS controller=%u reads=%" PRIu64 " writes=%" PRIu64 " enqueued=%" PRIu64 " nacks=%" PRIu64 " acked=%" PRIu64 " received=%" PRIu64 " abort-discarded=%" PRIu64,
                              i,s->i2c_nopeer[i].reads,s->i2c_nopeer[i].writes,s->i2c_nopeer[i].enqueued,s->i2c_nopeer[i].nacks,s->i2c_nopeer[i].acked,s->i2c_nopeer[i].received,s->i2c_nopeer[i].abort_discarded);
        o62m_i2c_nopeer_wipe_v3(&s->i2c_nopeer[i]);
    }
    bool wiped=o62m_virtual_otp_zero_v1(s->i2c_nopeer,sizeof(s->i2c_nopeer));
    marconi_public_report("MARCONI_I2C_NOPEER_EXIT_WIPE private-FIFOs-and-job-states-zeroized=%s timers-cancelled=true "
        "local-bus-transactions-ended=true no-payload-or-hash-output=true",wiped?"true":"false");
    assert(wiped);
}
static void marconi_i2c_nopeer_create(MarconiProbeState *s)
{
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(now<0) {error_report("invalid virtualI2C initialclock");exit(EXIT_FAILURE);}
    for(unsigned i=0;i<5;i++) {
        int result=o62m_i2c_nopeer_init_v3(&s->i2c_nopeer[i],true,s->i2c_nopeer_hz,(uint64_t)now);
        if(result!=O62M_I2C_NOPEER_OK_V3) {error_report("invalid virtualI2C initialstate");exit(EXIT_FAILURE);}
        char *name=g_strdup_printf("ap-i2c-no-peer-%u",i);
        s->i2c_nopeer_bus[i]=i2c_init_bus(DEVICE(&s->armv7m),name);g_free(name);
        qbus_realize(BUS(s->i2c_nopeer_bus[i]),&error_fatal);
        s->i2c_nopeer_context[i].owner=s;s->i2c_nopeer_context[i].index=i;
        s->i2c_nopeer_event[i]=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_i2c_nopeer_callback,&s->i2c_nopeer_context[i]);
    }
    marconi_touch_create(s);
    s->i2c_nopeer_exit.notify=marconi_i2c_nopeer_exit;qemu_add_exit_notifier(&s->i2c_nopeer_exit);
}

/* Actual virtual-clock watchdog work. The first expiry is an explicit
 * unsupported hardware boundary, never a fabricated IRQ or SoC reset. */
static void marconi_wdt4012_timing_report(MarconiProbeState *s,const char *stage)
{
 if(!s->wdt4012_profile)return;
 if(!strcmp(stage,"control-or-feed")&&s->wdt4012_gate_reports++>=8)return;
 uint32_t functional=0,bus=0;
 bool f=o62m_clock_controls_read(&s->clock_controls,0x40060064,4,&functional)==O62M_CLOCK_CONTROLS_OK;
 bool b=o62m_clock_controls_read(&s->clock_controls,0x400600e8,4,&bus)==O62M_CLOCK_CONTROLS_OK;
 marconi_public_report("MARCONI_WDT4012_TIMING stage=%s functional-known=%u functional-gate=%u bus-known=%u bus-gate=%u running=%u feeds=%" PRIu64 " preloads=%" PRIu64 " now-ns=%" PRId64 " last-load-ns=%" PRId64 " deadline-ns=%" PRId64 " PC=%08" PRIx64 " no-register-or-counter-payload=true",
  stage,f,!!(functional&0x10000),b,!!(bus&0x20000),s->wdt4012.running,s->wdt4012.feeds,s->wdt4012.preloads,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),s->wdt4012.started_ns,s->wdt4012.deadline_ns,current_cpu?marconi_public_code_pc(marconi_mmio_pc()):UINT64_MAX);
}
static bool marconi_wdt4012_boundary(MarconiProbeState *s,int result,const char *stage)
{
    if(s->wdt4012_event)timer_del(s->wdt4012_event);
    if(!s->reported) {
        uint64_t pc=marconi_public_code_pc(current_cpu?CPU_GET_CLASS(current_cpu)->get_pc(current_cpu):UINT64_MAX);
        marconi_public_report("MARCONI_WDT4012_BOUNDARY stage=%s reason-code=%d first-expiry=%u pc=%08" PRIx64
            " actual-virtual-clock-state=true no-IRQ-reset-or-ready-generated=true no-counter-or-register-value-output=true",
            stage,result,result==O62M_WDT4012_FIRST_EXPIRY_V1,pc);
        marconi_owned_runtime_failure_stop(s);
    }
    return false;
}
static bool marconi_wdt4012_refresh(MarconiProbeState *s)
{
    if(!s->wdt4012_profile)return true;
    if(s->reported) {if(s->wdt4012_event)timer_del(s->wdt4012_event);return false;}
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),deadline=0;
    int result=o62m_wdt4012_advance_v1(&s->wdt4012,now);
    if(result!=O62M_WDT4012_OK_V1){marconi_wdt4012_timing_report(s,"expiry");return marconi_wdt4012_boundary(s,result,"clock-advance");}
    bool present=false;
    result=o62m_wdt4012_next_deadline_v1(&s->wdt4012,&present,&deadline);
    if(result!=O62M_WDT4012_OK_V1)return marconi_wdt4012_boundary(s,result,"next-deadline");
    if(present) {
        if(deadline<=now)return marconi_wdt4012_boundary(s,O62M_WDT4012_TIME_V1,"deadline-not-future");
        timer_mod_ns(s->wdt4012_event,deadline);
    } else timer_del(s->wdt4012_event);
    return true;
}
static void marconi_wdt4012_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    /* This independently fires on QEMU_CLOCK_VIRTUAL, without requiring any
     * later MMIO access. Reported/VM-shutdown paths cannot commit more work. */
    (void)marconi_wdt4012_refresh(s);
}
static void marconi_explore_update(MarconiProbeState *s);
static bool marconi_adc_core_advance(MarconiProbeState *s,int64_t now)
{
    if(s->auxadc_channel_profile)return o62m_pmic_adc_ap_advance_v1(&s->pmic_adc,now);
    o62m_pmic_adc_update(&s->pmic_adc,now);return true;
}
static bool marconi_adc_core_write(MarconiProbeState *s,O62MPmicAdc *core,uint16_t reg,uint16_t value,int64_t now)
{
    if(s->pmic_voltage_source_profile&&reg==0xb8) {
        O62MPmicVoltageSnapshotV6 sample=s->pmic_voltage;
        O62MPmicAdc adc=*core;uint16_t pending=s->pmic_low_event_pending;
        bool ack=(value&0x800)!=0;
        if(ack&&!o62m_pmic_voltage_ack_v6(&sample,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)))return false;
        uint16_t lower_value=(value&~0x800)|(core->opaque_seed&0x800);
        if(!o62m_pmic_low_events_write_v1(&adc,&s->auxadc_ap,&pending,reg,lower_value,now))return false;
        *core=adc;
        if(core==&s->pmic_adc) {
            s->pmic_low_event_pending=pending;
            if(ack) {
                s->pmic_voltage=sample;marconi_pmic_voltage_schedule(s);
                marconi_public_report("MARCONI_PMIC_VOLTAGE_ACK generation=%" PRIu64 " requests=%" PRIu64
                    " pending=%u actual-successful-peer-commit=true prior-ready-cleared=true virtual-ACK-then-next-sample-policy=true no-silicon-trigger-claim=true",
                    sample.generation,sample.requests,sample.pending);
            }
        }
        return true;
    }
    if(s->pmic_mode_extended&&(reg==0xb8||reg==0x5b0)) {
        uint16_t pending=s->pmic_low_event_pending;
        if(!o62m_pmic_low_events_write_v1(core,&s->auxadc_ap,&pending,reg,value,now))return false;
        if(core==&s->pmic_adc) {
            s->pmic_low_event_pending=pending;
            marconi_public_report("MARCONI_PMIC_LOW_EVENTS_COMMIT operation=%s actual-shared-SPMI-deadline=true ADC-bit8-owner-preserved=true low-six-sources-quiescent=true no-new-event-or-IRQ=true",reg==0xb8?"W1C":"mask-SET");
        }
        return true;
    }
    return s->auxadc_channel_profile?o62m_pmic_adc_ap_write_v1(core,&s->auxadc_ap,reg,value,now):
        o62m_pmic_adc_write(core,reg,value,now);
}
static bool marconi_adc_core_read(MarconiProbeState *s,uint16_t reg,uint16_t *value,int64_t now)
{
    if(s->pmic_voltage_source_profile&&(reg==0xb4||reg==0x164||reg==0x168||reg==0x13c||reg==0x140||reg==0x144||reg==0x148||reg==0x16c||reg==0x170||reg==0x1fc||reg==0x1f8||reg==0x1f4||reg==0x208||reg==0x204||reg==0x200||(reg>=0x174&&reg<=0x1f0&&!(reg&3)))) {
        if(!marconi_pmic_voltage_refresh(s))return false;
        if(reg!=0xb4) {
            bool ok=o62m_pmic_voltage_read_v6(&s->pmic_voltage,reg,value);
            if(reg>=0x174&&reg<=0x1f0)marconi_public_report("MARCONI_PMIC_HISTORY_READ generation=%" PRIu64
                " latched-count=%u actual-sample-ticks=%" PRIu64 " accepted=%u snapshot-only=true no-sample-values=true",
                s->pmic_voltage.generation,s->pmic_voltage.latched_history_count,s->pmic_voltage.latched_sample_ticks,ok);
            if(!ok)marconi_public_report("MARCONI_PMIC_VOLTAGE_READ_BOUNDARY generation=%" PRIu64 " valid=%u pending=%u enabled=%u no-sample-output=true",
                s->pmic_voltage.generation,s->pmic_voltage.valid,s->pmic_voltage.pending,s->pmic_voltage.enabled);
            return ok;
        }
    }
    bool ok=s->auxadc_channel_profile?o62m_pmic_adc_ap_read_v1(&s->pmic_adc,&s->auxadc_ap,reg,value,now):
        o62m_pmic_adc_read(&s->pmic_adc,reg,value,now);
    if(ok&&s->pmic_mode_extended)*value=o62m_pmic_low_events_project_v1(reg,*value,s->pmic_low_event_pending);
    if(ok&&s->pmic_voltage_source_profile) {
        if(reg==0xb4)*value=o62m_pmic_voltage_status_v6(&s->pmic_voltage,*value);
        if(reg==0xb8)*value&=~0x800;
    }
    return ok;
}
static bool marconi_auxadc_refresh(MarconiProbeState *s,int64_t now)
{
    if(!s->auxadc_channel_profile)return true;
    if(!marconi_adc_core_advance(s,now))return false;
    if(s->auxadc_reported_started!=s->pmic_adc.started) {
        s->auxadc_reported_started=s->pmic_adc.started;
        marconi_public_report("MARCONI_AUXADC_CONVERSION_STARTED jobs=%" PRIu64 " configured-channel=%u actual-SPMI-trigger-edge=true actual-input-snapshot-in-canonical-core=true pending=true no-sample-values-or-ready-created=true",s->pmic_adc.started,s->pmic_adc.pending_channel);
    }
    if(s->auxadc_reported_completed!=s->pmic_adc.completed) {
        s->auxadc_reported_completed=s->pmic_adc.completed;
        marconi_public_report("MARCONI_AUXADC_CONVERSION_COMPLETED jobs=%" PRIu64 " completed-channel=%u actual-deadline-input-copy=true result-valid=true raw-sample-redacted=true no-guest-RAM-flag-write=true",s->pmic_adc.completed,s->pmic_adc.result_channel);
    }
    bool level=o62m_pmic_adc_ap_irq_ready_v1(&s->pmic_adc,s->pmic_bc_word);
    if(level!=s->auxadc_irq_level) {
        s->auxadc_irq_level=level;
        if(level)s->auxadc_irq_assertions++;else s->auxadc_irq_deassertions++;
        qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->armv7m),82),level);
        marconi_public_report("MARCONI_AUXADC_IRQ_LINE level=%u assertions=%" PRIu64 " deassertions=%" PRIu64 " external-IRQ82-exception98=true derived-from-actual-ready-valid-local100-globalBC2=true explicit-level-until-ADCACK-policy=true no-injected-frame-or-guestflag=true",level,s->auxadc_irq_assertions,s->auxadc_irq_deassertions);
    }
    if(s->pmic_adc.pending)timer_mod_ns(s->auxadc_event,s->pmic_adc.deadline_ns);else timer_del(s->auxadc_event);
    return true;
}
static void marconi_auxadc_callback(void *opaque)
{ marconi_explore_update(opaque); }
static bool marconi_pmic_mode_selected(MarconiProbeState *s,uint32_t reg,size_t *index)
{
    if(s->pmic_mode_extended&&(reg==0x05f0||reg==0x0af0))reg=0xf0;
    return s->pmic_mode_config_inputs&&o62m_clock_controls_index(&s->pmic_mode_config,reg,4,index)==O62M_CLOCK_CONTROLS_OK;
}
static bool marconi_pmic_button_port(MarconiProbeState *s,uint16_t reg)
{
    return s->pmic_button_config_initial&&(reg==UINT16_C(0x1044)||
           (s->pmic_button_followup_present&&reg==UINT16_C(0x107c))||
           (s->pmic_button_second_followup_present&&reg==UINT16_C(0x10ac)));
}
static bool marconi_pmic_button_validate(MarconiProbeState *s,uint16_t reg,uint32_t value)
{
    bool second=reg==UINT16_C(0x107c),third=reg==UINT16_C(0x10ac);
    uint16_t old=third?s->pmic_button_second_followup_word:second?s->pmic_button_followup_word:s->pmic_button_config_word;
    if(value>UINT16_MAX)return false;
    if(s->pmic_sync107c_profile&&second) {
        value=o62m_pmic_sync107c_normalize_v1((uint16_t)value);
        old=o62m_pmic_sync107c_normalize_v1(old);
        if(!(value&1u))return false;
    }
    uint32_t mask=third?3u:second?1u:UINT32_C(0x063f),changed=value^old;
    uint64_t writes=third?s->pmic_button_second_followup_writes:second?s->pmic_button_followup_writes:s->pmic_button_writes;
    return marconi_pmic_button_port(s,reg)&&value<=UINT16_MAX&&(value|old)==value&&
           !(changed&~mask)&&(!changed||!(changed&(changed-1)))&&writes!=UINT64_MAX;
}
static bool marconi_pmic_bc_validate(MarconiProbeState *s,uint32_t value)
{
    uint32_t changed=value^s->pmic_bc_word;
    return value<=UINT16_MAX&&((value|s->pmic_bc_word)==value||s->auxadc_channel_profile)&&
        (!changed||!(changed&(changed-1)))&&s->pmic_bc_writes!=UINT64_MAX;
}
static bool marconi_pmic_config1328_validate(uint32_t value)
{
    return value<=UINT16_MAX&&(value&255)==(value>>8);
}
static void marconi_pmic_config1328_reset(void *opaque)
{
    MarconiProbeState *s=opaque;
    o62m_virtual_otp_wipe_v1(&s->pmic_config1328,sizeof s->pmic_config1328);
    s->pmic_config1328_commits=0;
}
static void marconi_pmic_config1328_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,pmic_config1328_exit);(void)opaque;
    marconi_public_report("MARCONI_PMIC_CONFIG1328_EXIT commits=%" PRIu64 " private-config-wiped=true configuration-only=true",s->pmic_config1328_commits);
    marconi_pmic_config1328_reset(s);
}
static void marconi_explore_update(MarconiProbeState *s)
{
    if(s->normal_emmc_bound&&s->reported)return;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->pmic_write_pending && now >= s->pmic_write_deadline) {
        /* Explicit negative lifecycle test, never a normal boot profile.
         * The original CPU must first reach its actual post-key DVS worker.
         * Stop before the queued peer commit; no status/material is forged. */
        if(s->normal_emmc_fault_test&&!s->normal_emmc_fault_test_injected&&
           marconi_sensitive_guest_data&&s->pmic_write_dvs_owned) {
            s->normal_emmc_fault_test_injected=true;
            marconi_public_report("MARCONI_NORMAL_EMMC_FAULT_TEST injected=true site=owned-SPMI-worker private-gate-active=true queued-peer-not-committed=true negative-lifecycle-test-only=true boot-acceptance=false");
            marconi_owned_runtime_failure_stop(s);return;
        }
        /* Service ADC work only up to the earlier bus deadline before a
         * power/reset/control command may cancel it; then advance to now. */
        if(s->auxadc_channel_profile&&!marconi_adc_core_advance(s,s->pmic_write_deadline)) {
            marconi_public_report("MARCONI_AUXADC_BOUNDARY reason=pre-bus-deadline-advance-rejected no-new-ready=true");marconi_owned_runtime_failure_stop(s);return;
        }
        if(s->pmic_clock_inputs&&(s->pmic_write_reg==0x0504||s->pmic_write_reg==0x0a04||s->pmic_write_reg==0x0004)) {
            marconi_rfspim_service_at(s,now);
            if(s->reported)return;
        }
        s->pmic_write_pending = false;
        bool accepted;
        if(s->pmic_write_dvs_owned) {
            s->pmic_write_dvs_owned=false;
            bool gated=(s->ctrl54&0x10)&&(s->spmi_ctrl&0xcc)==0xcc&&s->slave_selected&&!s->select_pending;
            int result=gated?o62m_virtual_dvs_pmic_commit_v1(&s->virtual_dvs,&s->pmic_regulators,
                s->pmic_write_dvs_generation,8,s->pmic_write_reg,s->pmic_write_value,s->pmic_write_deadline):O62M_VDVS_V1_BAD_PHASE;
            accepted=result==O62M_VDVS_V1_OK;
            if(!accepted) {
                (void)o62m_virtual_dvs_fail_v1(&s->virtual_dvs,s->virtual_dvs.generation,now);
                marconi_public_report("MARCONI_VIRTUAL_DVS_BOUNDARY reason=shared-SPMI-peer-commit-rejected code=%d no-phase-or-index-completion=true",result);
                marconi_owned_runtime_failure_stop(s);return;
            }
            marconi_public_report("MARCONI_VIRTUAL_DVS_BUS_COMMIT generation=%" PRIu64 " actual-shared-SPMI-deadline=true actual-canonical-PMIC-word-updated=true feedback-fc0-preserved=true rail-settle-pending=true no-level-completion-yet=true",s->virtual_dvs.generation);
        } else if(s->rtc32k_reference_profile&&s->pmic_write_reg==0x80) {
            O62MRTCCalibrationV1 trial=s->rtc_calibration;
            uint32_t window=((uint32_t)s->rtc_calibration_config[1]<<16)|s->rtc_calibration_config[2];
            accepted=s->pmic_write_value==1&&o62m_rtc_calibration_start_v2(&trial,
                s->rtc_calibration_config[0],s->rtc_calibration_config_written,window,&s->rtc32k_reference,now);
            if(!accepted) {marconi_owned_runtime_failure_stop(s);return;}
            s->rtc_calibration=trial;s->rtc_calibration_armed_generation=trial.generation;
            timer_mod_ns(s->rtc_calibration_event,trial.deadline_ns);
            marconi_public_report("MARCONI_RTC_CALIBRATION_START generation=%" PRIu64 " source-generation=%" PRIu64
                " virtual-ns=%" PRId64 " deadline-ns=%" PRId64 " actual-SPMI-commit=true input-snapshot=true no-completion-yet=true",
                trial.generation,trial.source_generation,now,trial.deadline_ns);
        } else if(s->rtc32k_reference_profile&&(s->pmic_write_reg==0x84||s->pmic_write_reg==0x88||s->pmic_write_reg==0x8c)) {
            unsigned i=(s->pmic_write_reg-0x84)/4;
            accepted=s->pmic_write_value<=UINT16_MAX&&
                (i!=0||!((s->pmic_write_value^s->rtc_calibration_config[0])&~UINT32_C(0x1f)));
            if(!accepted) {marconi_owned_runtime_failure_stop(s);return;}
            s->rtc_calibration_config[i]=s->pmic_write_value;s->rtc_calibration_config_written|=1u<<i;
            s->rtc_calibration_config_commits++;
            marconi_public_report("MARCONI_RTC_CALIBRATION_CONFIG_COMMIT index=%u commits=%" PRIu64
                " actual-SPMI-peer-commit=true retained-config-only=true no-calibration-start-or-completion=true",
                i,s->rtc_calibration_config_commits);
        } else if(marconi_pmic_button_port(s,s->pmic_write_reg)) {
            accepted=marconi_pmic_button_validate(s,s->pmic_write_reg,s->pmic_write_value);
            if(!accepted) {marconi_public_report("MARCONI_PMIC_BUTTON_CONFIG_BOUNDARY reason=queued-config-commit-rejected state-unchanged=true");marconi_owned_runtime_failure_stop(s);return;}
            bool second=s->pmic_write_reg==UINT16_C(0x107c),third=s->pmic_write_reg==UINT16_C(0x10ac);
            unsigned index=third?2:second?1:0;
            uint16_t *word=third?&s->pmic_button_second_followup_word:second?&s->pmic_button_followup_word:&s->pmic_button_config_word;
            uint64_t *writes=third?&s->pmic_button_second_followup_writes:second?&s->pmic_button_followup_writes:&s->pmic_button_writes;
            O62MPmicSync107cV1 sync_trial=s->pmic_sync107c;
            bool sync=s->pmic_sync107c_profile&&second;
            uint16_t committed=sync?o62m_pmic_sync107c_normalize_v1(s->pmic_write_value):s->pmic_write_value;
            if(sync&&o62m_pmic_sync107c_request_v1(&sync_trial,now)!=O62M_PMIC_SYNC107C_OK_V1) {
                marconi_public_report("MARCONI_PMIC_SYNC107C_BOUNDARY reason=peer-commit-job-preflight state-unchanged=true");
                marconi_owned_runtime_failure_stop(s);return;
            }
            *word=committed;++*writes;
            if(sync) {
                s->pmic_sync107c=sync_trial;
                s->pmic_sync107c_armed_generation=sync_trial.generation;
                timer_mod_ns(s->pmic_sync107c_event,sync_trial.deadline_ns);
                marconi_public_report("MARCONI_PMIC_SYNC107C_REQUEST generation=%" PRIu64
                    " actual-successful-peer-commit=true positive-delay-ns=%" PRId64
                    " ACK-cleared=true accepted-SPMI-prefix-only=true",sync_trial.generation,sync_trial.latency_ns);
            }
            marconi_public_report("MARCONI_PMIC_BUTTON_CONFIG_COMMIT port-index=%u writes=%" PRIu64
                " actual-shared-SPMI-deadline=true independent-full16-backing=true original-single-bit-OR-only=true "
                "no-button-sample-IRQ-or-peripheral-completion-generated=true raw-word-redacted=true",index,*writes);
        } else if(s->pmic_boot_events_profile&&o62m_pmic_boot_event_kind_v6(s->pmic_write_reg,&(unsigned){0})) {
            accepted=o62m_pmic_boot_events_write_v6(&s->pmic_boot_events,s->pmic_write_reg,s->pmic_write_value);
            if(!accepted){marconi_public_report("MARCONI_PMIC_BOOT_EVENTS_BOUNDARY stage=peer-commit state-unchanged=true");marconi_owned_runtime_failure_stop(s);return;}
            unsigned index,kind=o62m_pmic_boot_event_kind_v6(s->pmic_write_reg,&index);
            marconi_public_report("MARCONI_PMIC_BOOT_EVENTS_COMMIT kind=%u index=%u writes=%" PRIu64
                                  " actual-shared-SPMI-deadline=true retained-history-and-config-or-W1C-quiescent-pending=true no-event-or-ready-created=true",
                                  kind,index,s->pmic_boot_events.writes);
        } else if(s->pmic_mode_extended&&(s->pmic_write_reg==0x118||s->pmic_write_reg==0x11c)) {
            unsigned i=s->pmic_write_reg==0x11c?0:1;
            accepted=s->pmic_write_value<=UINT16_MAX&&(i==0||s->pmic_write_value<=0xff);
            if(!accepted){marconi_public_report("MARCONI_PMIC_THRESHOLD_CONFIG_BOUNDARY stage=peer-commit state-unchanged=true");marconi_owned_runtime_failure_stop(s);return;}
            s->pmic_threshold_words[i]=s->pmic_write_value;s->pmic_threshold_written|=1u<<i;
            s->pmic_mode_writes++;
            marconi_public_report("MARCONI_PMIC_THRESHOLD_CONFIG_COMMIT index=%u actual-shared-SPMI-deadline=true original-written-parameter-only=true no-sample-counter-event-or-IRQ-created=true",i);
        } else if(s->pmic_mode_config_inputs&&marconi_pmic_mode_selected(s,s->pmic_write_reg,&(size_t){0})) {
            size_t index;marconi_pmic_mode_selected(s,s->pmic_write_reg,&index);
            int result=s->pmic_mode_extended?o62m_pmic_mode_write_v4(&s->pmic_mode_config,s->pmic_write_reg,s->pmic_write_value):
                o62m_clock_controls_write(&s->pmic_mode_config,s->pmic_write_reg,4,s->pmic_write_value);
            accepted=result==O62M_CLOCK_CONTROLS_OK;
            if(!accepted){marconi_public_report("MARCONI_PMIC_MODE_CONFIG_BOUNDARY reason=queued-config-write-rejected code=%d",result);marconi_owned_runtime_failure_stop(s);return;}
            s->pmic_mode_writes++;
            marconi_public_report("MARCONI_PMIC_MODE_CONFIG_COMMIT static-word-index=%zu writes=%" PRIu64 " actual-shared-SPMI-deadline=true canonical-retained-fields-only=true GO-active-modes-and-IRQ-not-completed=true",index,s->pmic_mode_writes);
        } else if(s->virtual_pmic_bc_config&&s->pmic_write_reg==0xbc) {
            accepted=marconi_pmic_bc_validate(s,s->pmic_write_value);
            if(!accepted) {marconi_public_report("MARCONI_PMIC_BC_BOUNDARY reason=queued-config-commit-rejected state-unchanged=true");marconi_owned_runtime_failure_stop(s);return;}
            uint16_t changed=s->pmic_bc_word^s->pmic_write_value;
            unsigned bit=16;for(unsigned i=0;i<16;i++)if(changed==(1u<<i)){bit=i;break;}
            s->pmic_bc_word=s->pmic_write_value;s->pmic_bc_writes++;
            marconi_public_report("MARCONI_PMIC_BC_CONFIG_COMMIT writes=%" PRIu64 " static-changed-bit-index=%u direction-set=%u actual-shared-SPMI-deadline=true canonical-retained-word=true no-new-peripheral-work-or-ready-created=true existing-source-IRQ-level-follows-real-mask=true",s->pmic_bc_writes,bit,changed&&(s->pmic_bc_word&changed));
        } else if(s->virtual_auxadc_reset_profile&&o62m_virtual_auxadc_reset_supports_write_v1(s->pmic_write_reg)) {
            /* Preserve virtual ordering if a prior conversion completed before
             * this reset packet reached the bus peer. */
            if(!marconi_adc_core_advance(s,s->pmic_write_deadline)){marconi_public_report("MARCONI_AUXADC_BOUNDARY reason=reset-time-advance-rejected");marconi_owned_runtime_failure_stop(s);return;}
            accepted=o62m_virtual_auxadc_reset_apply_v1(&s->virtual_auxadc_reset,&s->pmic_adc,s->pmic_write_reg,s->pmic_write_value);
            if(!accepted) {marconi_public_report("MARCONI_VIRTUAL_AUXADC_BOUNDARY reason=actual-bus-reset-command-rejected no-ready-generated=true");marconi_owned_runtime_failure_stop(s);return;}
            marconi_public_report("MARCONI_VIRTUAL_AUXADC_RESET_COMMIT asserted=%u asserts=%" PRIu64 " releases=%" PRIu64 " actual-shared-SPMI-transaction-completed=true measurement-cancel-only-if-still-pending-at-bus-deadline=true configuration-and-inputs-retained=true no-sample-ready-or-IRQ-generated=true",s->virtual_auxadc_reset.asserted,s->virtual_auxadc_reset.asserts,s->virtual_auxadc_reset.releases);
        } else if(s->pmic_regulator_inputs&&o62m_pmic_regulator_supports_v1(s->pmic_write_reg)) {
            int result=marconi_pmic_regulator_apply(&s->pmic_regulators,&s->pmic_adc,
                s->pmic_write_reg,s->pmic_write_value,s->pmic_write_deadline);
            accepted=result==0;
            if(!accepted) {error_report("MARCONI_PMIC_REGULATOR_COMMIT_BOUNDARY reason=%s",o62m_pmic_regulator_reason_v1(result));}
            else {
                if(s->pmic_alias_read_policy) {
                    int alias=o62m_pmic_regulator_alias_index_v1(s->pmic_write_reg);
                    int index=alias>=0?o62m_pmic_regulator_aliases_v1[alias].target_index:o62m_pmic_regulator_index_v1(s->pmic_write_reg);
                    marconi_public_report("MARCONI_PMIC_CANONICAL_COMMIT static-target-word-index=%d alias-command=%u total-writes=%" PRIu64 " alias-writes=%" PRIu64 " actual-shared-SPMI-deadline-and-existing-owner-update=true no-raw-words-or-addresses=true",index,alias>=0,s->pmic_regulators.writes,s->pmic_regulators.alias_writes);
                }
                error_report("MARCONI_PMIC_REGULATOR_COMMIT reg=%04x data=%04x writes=%" PRIu64
                             " canonical68=%04x virtual-ns=%" PRId64
                             " actual-queued-write-completion=true no-voltage-feedback-or-ready-generated=true",
                             s->pmic_write_reg,s->pmic_write_value,s->pmic_regulators.writes,
                             s->pmic_adc.control_68,s->pmic_write_deadline);
            }
        } else if(s->normal_emmc_bound&&s->pmic_write_reg==0x1328) {
            accepted=marconi_pmic_config1328_validate(s->pmic_write_value);
            if(accepted) {
                s->pmic_config1328=s->pmic_write_value;++s->pmic_config1328_commits;
                marconi_public_report("MARCONI_PMIC_CONFIG1328_COMMIT commits=%" PRIu64 " actual-shared-SPMI-deadline=true configuration-only=true",s->pmic_config1328_commits);
            }
        } else if(s->pmic_clock_inputs&&(s->pmic_write_reg==0x0504||s->pmic_write_reg==0x0a04)) {
            accepted=o62m_pmic_clock_set_write_v3(&s->pmic_clocks,s->pmic_write_reg,s->pmic_write_value)==O62M_PMIC_CLOCK_OK_V2;
            marconi_public_report("MARCONI_CP_PMIC_CLOCK_SET_COMMIT accepted=%u clear=%u canonical-index=0 actual-shared-SPMI-deadline=true same-clock-owner=true no-oscillator-ready-or-transfer-created=true",accepted,s->pmic_write_reg==0x0a04);
        } else if(s->pmic_clock_inputs&&o62m_pmic_clock_address_v2(s->pmic_write_reg)) {
            int result=o62m_pmic_clock_write_v2(&s->pmic_clocks,s->pmic_write_reg,s->pmic_write_value);
            accepted=result==0;
            if(!accepted) {error_report("MARCONI_PMIC_CLOCK_COMMIT_BOUNDARY reason=%s",o62m_pmic_clock_reason_v2(result));}
            else {
                if(s->pmic_write_reg==0x001c) {s->pmic_adc.config_1c=s->pmic_clocks.word[2];}
                error_report("MARCONI_PMIC_CLOCK_COMMIT reg=%04x word=%04x writes=%" PRIu64 " virtual-ns=%" PRId64
                             " actual-queued-write-completion=true same-word-shared-aliases=true "
                             "no-status-or-physical-clock-effects-generated=true",
                             s->pmic_write_reg,s->pmic_write_value,s->pmic_clocks.writes,s->pmic_write_deadline);
            }
        } else if(s->pmic1000_initial&&s->pmic_write_reg==0x1000) {
            int result=o62m_pmic1000_write_v1(&s->pmic1000,s->pmic_write_value);
            accepted=result==0;
            if(!accepted) {error_report("MARCONI_PMIC1000_COMMIT_BOUNDARY reason=%s",o62m_pmic1000_reason_v1(result));}
            else {error_report("MARCONI_PMIC1000_COMMIT word=%04x writes=%" PRIu64 " virtual-ns=%" PRId64
                              " actual-queued-write-completion=true no-status-or-clock-ready-generated=true",
                              s->pmic1000.word,s->pmic1000.writes,s->pmic_write_deadline);}
        } else {
            accepted=marconi_adc_core_write(s,&s->pmic_adc,s->pmic_write_reg,
                                        s->pmic_write_value,s->pmic_write_deadline);
        }
        if(accepted&&s->pmic_voltage_source_profile&&
           (s->pmic_write_reg==0xf0||s->pmic_write_reg==0x5f0||s->pmic_write_reg==0xaf0)) {
            uint32_t f0;
            if(o62m_clock_controls_read(&s->pmic_mode_config,0xf0,4,&f0)!=O62M_CLOCK_CONTROLS_OK||
               !o62m_pmic_voltage_gate_v6(&s->pmic_voltage,f0,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
                marconi_owned_runtime_failure_stop(s);return;
            }
            marconi_pmic_voltage_schedule(s);
            marconi_public_report("MARCONI_PMIC_VOLTAGE_GATE generation=%" PRIu64 " enabled=%u pending=%u valid=%u ready=%u actual-canonical-config-commit=true no-register-output=true",
                s->pmic_voltage.generation,s->pmic_voltage.enabled,s->pmic_voltage.pending,s->pmic_voltage.valid,s->pmic_voltage.ready);
        }
        if(!accepted&&s->normal_emmc_bound) {
            marconi_public_report("MARCONI_PMIC_PEER_BOUNDARY reason=actual-queued-peer-rejected no-success-commit=true");
            marconi_owned_runtime_failure_stop(s);return;
        }
        assert(accepted);
        marconi_bbpll_source_changed(s);
        marconi_rfspim_source_changed(s);
        error_report("MARCONI_EXPLORE_PMIC commit reg=0x%04x value=0x%04x virtual-ns=%" PRId64,
                     s->pmic_write_reg, s->pmic_write_value, s->pmic_write_deadline);
    }
    if(!marconi_adc_core_advance(s,now)||!marconi_auxadc_refresh(s,now)){marconi_public_report("MARCONI_AUXADC_BOUNDARY reason=clock-or-derived-IRQ-refresh-rejected no-guestflag=true");marconi_owned_runtime_failure_stop(s);return;}
    if (s->select_pending &&
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= s->select_deadline) {
        s->select_pending = false;
        s->slave_selected = true;
        error_report("MARCONI_EXPLORE_SPMI select-complete slave=8 virtual-ns=%" PRId64,
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

/* Explicit virtual DVS engine. The existing SPMI queue is the sole bus owner;
 * the PMIC hardware-agent extension cannot alter ordinary CPU write policy.
 * Timer callbacks and guest MMIO serialize on QEMU's main loop. */
static bool marconi_virtual_dvs_refresh(MarconiProbeState *s)
{
    if(!s->virtual_dvs_profile)return true;
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    marconi_explore_update(s);
    if(s->normal_emmc_bound&&s->reported)return false;
    if(s->virtual_dvs.phase==O62M_VDVS_WAIT_BUS_V1) {
        if(!(s->ctrl54&0x10)||(s->spmi_ctrl&0xcc)!=0xcc)return false;
        if(s->select_pending||s->pmic_write_pending) {
            int64_t due=s->select_pending?s->select_deadline:s->pmic_write_deadline;
            if(s->pmic_write_pending&&s->pmic_write_deadline<due)due=s->pmic_write_deadline;
            if(due<=now)return false;
            timer_mod_ns(s->virtual_dvs_event,due);return true;
        }
        if(!s->slave_selected||now<0||s->transaction_ns>(uint64_t)(INT64_MAX-now))return false;
        if(o62m_virtual_dvs_mark_queued_v1(&s->virtual_dvs,s->virtual_dvs.generation,now)!=O62M_VDVS_V1_OK)return false;
        s->pmic_write_pending=true;s->pmic_write_dvs_owned=true;
        s->pmic_write_dvs_generation=s->virtual_dvs.generation;
        s->pmic_write_reg=s->virtual_dvs.pending_reg;s->pmic_write_value=s->virtual_dvs.pending_payload;
        s->pmic_write_deadline=now+(int64_t)s->transaction_ns;
        marconi_public_report("MARCONI_VIRTUAL_DVS_BUS_QUEUED generation=%" PRIu64 " shared-SPMI-queue=true request-snapshot-locked=true no-immediate-completion=true",s->virtual_dvs.generation);
    }
    if(s->virtual_dvs.phase==O62M_VDVS_TRANSFER_V1) {
        if(!s->pmic_write_pending||!s->pmic_write_dvs_owned||s->pmic_write_dvs_generation!=s->virtual_dvs.generation)return false;
        timer_mod_ns(s->virtual_dvs_event,s->pmic_write_deadline);return true;
    }
    if(s->virtual_dvs.phase==O62M_VDVS_SETTLE_V1) {
        uint64_t completed=s->virtual_dvs.completed;
        if(o62m_virtual_dvs_advance_v1(&s->virtual_dvs,now)!=O62M_VDVS_V1_OK)return false;
        if(completed!=s->virtual_dvs.completed)marconi_public_report("MARCONI_VIRTUAL_DVS_SETTLE_COMPLETE generation=%" PRIu64 " completed=%" PRIu64 " actual-prior-shared-SPMI-commit=true actual-virtual-settle-deadline=true phase-and-index-committed-together=true physical-analog-or-feedback-mirror-not-claimed=true",s->virtual_dvs.generation,s->virtual_dvs.completed);
        if(s->virtual_dvs.phase==O62M_VDVS_SETTLE_V1) {
            timer_mod_ns(s->virtual_dvs_event,s->virtual_dvs.settle_due_ns);return true;
        }
    }
    timer_del(s->virtual_dvs_event);return true;
}
static void marconi_virtual_dvs_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->normal_emmc_bound&&s->reported)return;
    if(!marconi_virtual_dvs_refresh(s)) {
        if(s->normal_emmc_bound&&s->reported)return;
        if(o62m_virtual_dvs_active_v1(&s->virtual_dvs))(void)o62m_virtual_dvs_fail_v1(&s->virtual_dvs,s->virtual_dvs.generation,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        marconi_public_report("MARCONI_VIRTUAL_DVS_BOUNDARY reason=owned-worker-rejected no-level-completion=true");marconi_owned_runtime_failure_stop(s);return;
    }
}
static bool marconi_virtual_dvs_request(MarconiProbeState *s,uint32_t index)
{
    uint32_t ctrl,cmd_a,cmd_b;
    uint32_t needed=index<=7?(UINT32_C(0x1f)|(UINT32_C(1)<<(5+index))|(UINT32_C(1)<<(17+index))):0;
    if(index>7||(s->aon120_config_written&needed)!=needed||o62m_clock_controls_read(&s->aon120_config,0x4a120000,4,&ctrl)!=O62M_CLOCK_CONTROLS_OK||
       o62m_clock_controls_read(&s->aon120_config,0x4a120014+4*index,4,&cmd_a)!=O62M_CLOCK_CONTROLS_OK||
       o62m_clock_controls_read(&s->aon120_config,0x4a120060+4*index,4,&cmd_b)!=O62M_CLOCK_CONTROLS_OK)return false;
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    O62MVirtualDvsJobV1 trial=s->virtual_dvs;uint64_t prior=trial.generation;
    bool cancel=o62m_virtual_dvs_active_v1(&trial);
    if(cancel&&o62m_virtual_dvs_cancel_v1(&trial,prior,now)!=O62M_VDVS_V1_OK)return false;
    int result=o62m_virtual_dvs_begin_v1(&trial,index,ctrl,cmd_a,cmd_b,now);
    if(result!=O62M_VDVS_V1_OK)return false;
    /* Invalid new commands above do not cancel any old owned work. A valid
     * new generation cancels only this agent's old pending bus transaction. */
    if(cancel&&s->pmic_write_pending&&s->pmic_write_dvs_owned&&s->pmic_write_dvs_generation==prior) {
        s->pmic_write_pending=false;s->pmic_write_dvs_owned=false;
    }
    s->virtual_dvs=trial;s->virtual_dvs_requested=index;
    marconi_public_report("MARCONI_VIRTUAL_DVS_REQUEST generation=%" PRIu64 " actual-original-request=true snapshot-of-equal-configured-pair-and-SID=true old-owned-generation-cancelled=%u no-immediate-PMIC-rail-or-index-change=true",trial.generation,cancel);
    return marconi_virtual_dvs_refresh(s);
}

static bool marconi_timer64_clock(MarconiProbeState *s, uint32_t csr, uint32_t pclk,
                                    O62MTimer64 *m)
{
    return o62m_timer64_set_clock(m, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
             (csr & 1) ? 26000000 : 32000, ((csr >> 8) & 255) + 1,
             (csr & 0x10000) && (pclk & 0x20000));
}

/* 4a010000 measures DVFS operations independently of the RTOS one-shot.
 * Both instances consume the original shared clock configuration. */
static bool marconi_dvfs_timer64_refresh(MarconiProbeState *s)
{
    uint32_t csr, pclk;
    uint64_t next;
    if (!s->ap_timer64_view) return true;
    if (s->reported) {
        timer_del(s->dvfs_timer64_timer);
        return false;
    }
    assert(o62m_boot_clock_read(&s->boot_clocks, 0x4006005c, &csr));
    assert(o62m_boot_clock_read(&s->boot_clocks, 0x400600f8, &pclk));
    if (!marconi_timer64_clock(s, csr, pclk, &s->dvfs_timer64_model)) return false;
    if (s->dvfs_timer64_model.expirations) {
        s->reported = true;
        timer_del(s->dvfs_timer64_timer);
        marconi_public_report("MARCONI_DVFS_TIMER64_BOUNDARY reason=unmodeled-expiry independent-counter=true");
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC, MARCONI_PROBE_EXIT);
        if (current_cpu) cpu_exit(current_cpu);
        return false;
    }
    next = o62m_timer64_next_event_ns(&s->dvfs_timer64_model);
    if (next == UINT64_MAX || next > INT64_MAX) timer_del(s->dvfs_timer64_timer);
    else timer_mod_ns(s->dvfs_timer64_timer, next);
    return true;
}

static void marconi_dvfs_timer64_callback(void *opaque)
{
    marconi_dvfs_timer64_refresh(opaque);
}

static void marconi_timer64_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,timer64_exit);
    if(s->timer64_timer)timer_del(s->timer64_timer);
    if(s->dvfs_timer64_timer)timer_del(s->dvfs_timer64_timer);
    o62m_virtual_otp_wipe_v1(&s->dvfs_timer64_model, sizeof(s->dvfs_timer64_model));
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->armv7m),10),0);
    marconi_public_report("MARCONI_REPEATED_ACCESS_TOTALS timer64-reads=%" PRIu64 " timer64-writes=%" PRIu64 " snapshot-pairs=%" PRIu64
                          " board-accesses=%" PRIu64 " wdt-reads=%" PRIu64 " wdt-writes=%" PRIu64 " virtual-ns=%" PRId64 " detailed-records-per-family=64",
                          s->ap_timer64_view_reads,s->ap_timer64_view_writes,s->ap_timer64_view_pairs,s->ap_board_accesses,
                          s->wdt4012_reads,s->wdt4012_writes,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    marconi_public_report("MARCONI_TIMER64_EXIT expirations=%" PRIu64 " asserts=%" PRIu64 " deasserts=%" PRIu64
                          " ADSP-unpublished-reads=%" PRIu64 " timer-cancelled=true IRQ-low=true",
                          s->timer64_model.expirations,s->timer64_irq_asserts,s->timer64_irq_deasserts,s->adsp_publication_reads);
}

static bool marconi_timer64_refresh(MarconiProbeState *s)
{
    uint64_t next;
    uint32_t csr, pclk;
    if (!s->timer64) { return true; }
    if(s->reported) {
        timer_del(s->timer64_timer);
        if(s->timer64_terminal_profile) {qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->armv7m),10),0);s->timer64_irq_level=false;}
        return false;
    }
    o62m_timer64_advance(&s->timer64_model, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    if (s->timer64_model.expirations&&!s->timer64_terminal_profile) {
        if (!s->reported) {
            s->reported = true;
            if(marconi_sensitive_guest_data)marconi_public_report("MARCONI_PRIVATE_TIMER_BOUNDARY device=timer64 reason=unproven-terminal-behavior pc=%08" PRIx64 " raw-register-and-memory-values-redacted=true no-IRQ-invented=true",marconi_public_code_pc(current_cpu?CPU_GET_CLASS(current_cpu)->get_pc(current_cpu):UINT64_MAX));
            error_report("MARCONI_PROBE_UNIMPLEMENTED access=timer-expiry address=0x40040014 "
                         "size=4 value=0 pc=0x%08" PRIx64 " stop=unproven-terminal-behavior "
                         "post-expiry-reload-or-stop=unknown irq-not-delivered",
                         current_cpu ? CPU_GET_CLASS(current_cpu)->get_pc(current_cpu) : UINT64_MAX);
            qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC, MARCONI_PROBE_EXIT);
            if (current_cpu) { cpu_exit(current_cpu); }
        }
        timer_del(s->timer64_timer);
        return false;
    }
    o62m_boot_clock_read(&s->boot_clocks, 0x4006005c, &csr);
    o62m_boot_clock_read(&s->boot_clocks, 0x400600f8, &pclk);
    assert(marconi_timer64_clock(s, csr, pclk, &s->timer64_model));
    if(s->timer64_terminal_profile) {
        bool level=o62m_timer64_irq(&s->timer64_model);
        if(level!=s->timer64_irq_level) {
            s->timer64_irq_level=level;
            if(level)++s->timer64_irq_asserts;else ++s->timer64_irq_deasserts;
            qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->armv7m),10),level);
            if(s->timer64_irq_asserts+s->timer64_irq_deasserts<=64)marconi_public_report("MARCONI_TIMER64_IRQ level=%u external=10 asserts=%" PRIu64 " deasserts=%" PRIu64
                                  " expirations=%" PRIu64 " virtual-ns=%" PRId64 " actual-countdown-mask-and-delayed-ACK=true",
                                  level,s->timer64_irq_asserts,s->timer64_irq_deasserts,s->timer64_model.expirations,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        }
        if(s->timer64_logged_expirations!=s->timer64_model.expirations) {
            s->timer64_logged_expirations=s->timer64_model.expirations;
            if(s->timer64_logged_expirations<=64)marconi_public_report("MARCONI_TIMER64_EXPIRY count=%" PRIu64 " explicit-virtual-HOLD_FIRST_EXPIRY=true no-automatic-reload=true",
                                  s->timer64_logged_expirations);
        }
    }
    next = o62m_timer64_next_event_ns(&s->timer64_model);
    if (next == UINT64_MAX || next > INT64_MAX) { timer_del(s->timer64_timer); }
    else { timer_mod_ns(s->timer64_timer, next); }
    return marconi_dvfs_timer64_refresh(s);
}

static void marconi_timer64_callback(void *opaque)
{
    marconi_timer64_refresh(opaque);
}

/* Independent two32bit-channel device, never aliases the 64bit timers.
 * Original OTA source graph declares32000Hz; the high software object writes
 * 32768. Preserve that distinction. Fixed-source availability is an explicit
 * functional experiment; source1 additionally needs its retained real gate. */
static bool marconi_timer4014_failure(MarconiProbeState *s,int result)
{
    if(!s->reported) {
        s->reported=true;
        if(marconi_sensitive_guest_data)marconi_public_report("MARCONI_PRIVATE_TIMER_BOUNDARY device=timer4014 reason=%s pc=%08" PRIx64 " raw-register-and-memory-values-redacted=true no-IRQ-invented=true",o62m_dw32_reason_v1(result),marconi_public_code_pc(current_cpu?CPU_GET_CLASS(current_cpu)->get_pc(current_cpu):UINT64_MAX));
        error_report("MARCONI_TIMER4014_BOUNDARY reason=%s virtual-ns=%" PRId64
                     " no-constant-counter-or-ready=true full-boot=false",
                     o62m_dw32_reason_v1(result),qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC,MARCONI_PROBE_EXIT);
        if(current_cpu) {cpu_dump_state(current_cpu,stderr,0);cpu_exit(current_cpu);}
    }
    if(s->timer4014_event) {timer_del(s->timer4014_event);}
    return false;
}

static void marconi_timer4014_words(MarconiProbeState *s,uint32_t *csr0,uint32_t *csr1,uint32_t *pclk)
{
    assert(o62m_boot_clock_read(&s->boot_clocks,0x40060050,csr0));
    assert(o62m_boot_clock_read(&s->boot_clocks,0x40060054,csr1));
    assert(o62m_boot_clock_read(&s->boot_clocks,0x400600f0,pclk));
}

static bool marconi_timer4014_clock(MarconiProbeState *s,const O62MClockControls *controller,
                                   uint32_t csr0,uint32_t csr1,uint32_t pclk,O62MDw32ClockV1 *clock)
{
    uint32_t words[2]={csr0,csr1};
    if((csr0&0x100000u)||(csr1&0x100000u)||(pclk&0x200000u)) {
        error_report("MARCONI_TIMER4014_CLOCK_REJECT reason=automatic-gating-sideeffects-unimplemented");return false;
    }
    for(unsigned i=0;i<2;i++) {
        bool parent_available=true;
        clock->source_hz[i]=(words[i]&1u)?26000000:32000;
        if(words[i]&1u) {
            uint32_t gate;
            if(!s->clock_controls_fixture||
               o62m_clock_controls_read(controller,0x4a098074,4,&gate)!=O62M_CLOCK_CONTROLS_OK) {
                error_report("MARCONI_TIMER4014_CLOCK_REJECT reason=source1-retained-parent-gate-required");return false;
            }
            parent_available=(gate&1u)!=0;
        }
        clock->divisor[i]=((words[i]>>8)&255u)+1u;
        clock->functional_gate[i]=(words[i]&0x10000u)&&parent_available;
    }
    clock->pclk_gate=(pclk&0x20000u)!=0;
    return true;
}

/* Validate all observations/deadline before committing state or IRQ levels. */
static bool marconi_timer4014_publish(MarconiProbeState *s,const O62MDw32V1 *trial)
{
    bool level[2],present;int64_t deadline;
    for(unsigned i=0;i<2;i++) {
        int result=o62m_dw32_irq_level_v1(trial,i,&level[i]);
        if(result) {return marconi_timer4014_failure(s,result);}
    }
    int result=o62m_dw32_next_event_v1(trial,&deadline,&present);
    if(result) {return marconi_timer4014_failure(s,result);}
    s->timer4014=*trial;
    for(unsigned i=0;i<2;i++) {
        qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->armv7m),8+i),level[i]);
        if(s->timer4014.channel[i].expiries!=s->timer4014_logged_expiries[i]) {
            s->timer4014_logged_expiries[i]=s->timer4014.channel[i].expiries;
            error_report("MARCONI_TIMER4014_EXPIRY channel=%u count=%" PRIu64 " current=%08x control=%08x "
                         "raw-irq=%u line=%u nvic-external=%u virtual-ns=%" PRId64 " actual-virtual-time-counting=true",
                         i,s->timer4014.channel[i].expiries,s->timer4014.channel[i].current,
                         s->timer4014.channel[i].control,s->timer4014.channel[i].raw_irq,level[i],8+i,
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        }
    }
    if(present) {timer_mod_ns(s->timer4014_event,deadline);} else {timer_del(s->timer4014_event);}
    return true;
}

static bool marconi_timer4014_refresh(MarconiProbeState *s)
{
    if(!s->timer4014_inputs) {return true;}
    uint32_t csr0,csr1,pclk;O62MDw32ClockV1 clock={0};
    marconi_timer4014_words(s,&csr0,&csr1,&pclk);
    if(!marconi_timer4014_clock(s,&s->clock_controls,csr0,csr1,pclk,&clock)) {return false;}
    O62MDw32V1 trial=s->timer4014;
    int result=o62m_dw32_set_clock_v1(&trial,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),&clock);
    if(result) {return marconi_timer4014_failure(s,result);}
    return marconi_timer4014_publish(s,&trial);
}

static void marconi_timer4014_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(!marconi_timer4014_refresh(s)&&!s->reported) {
        s->reported=true;
        error_report("MARCONI_TIMER4014_BOUNDARY reason=unmodeled-controller-clock-state during-virtual-event=true");
        timer_del(s->timer4014_event);
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC,MARCONI_PROBE_EXIT);
        if(current_cpu) {cpu_exit(current_cpu);}
    }
}

static bool marconi_timer4014_clock_address(uint64_t address)
{
    return address==0x40060050||address==0x40060054||address==0x400600f0||address==0x4a098074;
}

static bool marconi_timer4014_clock_preflight(MarconiProbeState *s,const O62MClockControls *controller,
                                             uint64_t address,uint32_t value)
{
    if(!s->timer4014_inputs||!marconi_timer4014_clock_address(address)) {return true;}
    uint32_t csr0,csr1,pclk;O62MDw32ClockV1 clock={0};
    marconi_timer4014_words(s,&csr0,&csr1,&pclk);
    if(address==0x40060050) {csr0=value;}
    if(address==0x40060054) {csr1=value;}
    if(address==0x400600f0) {pclk=value;}
    if(!marconi_timer4014_clock(s,controller,csr0,csr1,pclk,&clock)) {return false;}
    O62MDw32V1 trial=s->timer4014;
    int result=o62m_dw32_set_clock_v1(&trial,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),&clock);
    if(result) {return marconi_timer4014_failure(s,result);}
    bool present,level;int64_t deadline;
    result=o62m_dw32_next_event_v1(&trial,&deadline,&present);
    if(result) {return marconi_timer4014_failure(s,result);}
    for(unsigned i=0;i<2;i++) {
        result=o62m_dw32_irq_level_v1(&trial,i,&level);
        if(result) {return marconi_timer4014_failure(s,result);}
    }
    return true;
}

static void marconi_tx_refresh(MarconiProbeState *s);

static bool marconi_timer0_runtime_failure(MarconiProbeState *s,O62MShotResult result)
{
    if(!s->reported) {
        s->reported=true;
        if(marconi_sensitive_guest_data)marconi_public_report("MARCONI_PRIVATE_TIMER_BOUNDARY device=timer0-runtime reason=%s pc=%08" PRIx64 " raw-register-and-memory-values-redacted=true no-terminal-behavior-invented=true",o62m_shot_timer_reason(result),marconi_public_code_pc(current_cpu?CPU_GET_CLASS(current_cpu)->get_pc(current_cpu):UINT64_MAX));
        error_report("MARCONI_TIMER0_RUNTIME_BOUNDARY reason=%s pc=0x%08" PRIx64
            " virtual-ns=%" PRId64 " no-invented-periodic-or-terminal-behavior=true",
            o62m_shot_timer_reason(result),current_cpu?CPU_GET_CLASS(current_cpu)->get_pc(current_cpu):UINT64_MAX,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC,MARCONI_PROBE_EXIT);
        if(current_cpu) {cpu_dump_state(current_cpu,stderr,0);cpu_exit(current_cpu);}
    }
    if(s->timer0_runtime_timer) timer_del(s->timer0_runtime_timer);
    return false;
}

static void marconi_timer0_runtime_clock_words(MarconiProbeState *s,uint32_t *csr,uint32_t *pclk)
{
    /* These words are not duplicated in the old boot_clock projection. */
    assert(o62m_clock_controls_read(&s->clock_controls,0x40060058,4,csr)==O62M_CLOCK_CONTROLS_OK);
    assert(o62m_clock_controls_read(&s->clock_controls,0x400600f4,4,pclk)==O62M_CLOCK_CONTROLS_OK);
}

static O62MShotResult marconi_timer0_runtime_clock(MarconiProbeState *s,O62MShotTimer *m,uint32_t csr,uint32_t pclk)
{
    return o62m_shot_timer_set_clock(m,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
        (csr&1u)?26000000:32000,((csr>>8)&255u)+1u,(csr&0x10000u)&&(pclk&0x20000u));
}

static bool marconi_timer0_runtime_refresh(MarconiProbeState *s)
{
    if(!s->timer0_runtime_fixture) return true;
    uint32_t csr,pclk;uint64_t next;bool present,level;
    O62MShotResult r=o62m_shot_timer_advance(&s->timer0_runtime,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    if(r!=O62M_SHOT_OK) return marconi_timer0_runtime_failure(s,r);
    marconi_timer0_runtime_clock_words(s,&csr,&pclk);
    r=marconi_timer0_runtime_clock(s,&s->timer0_runtime,csr,pclk);
    if(r!=O62M_SHOT_OK) return marconi_timer0_runtime_failure(s,r);
    r=o62m_shot_timer_irq_level(&s->timer0_runtime,&level);
    if(r!=O62M_SHOT_OK) return marconi_timer0_runtime_failure(s,r);
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->armv7m),4),level);
    r=o62m_shot_timer_next_event(&s->timer0_runtime,&next,&present);
    if(r!=O62M_SHOT_OK) return marconi_timer0_runtime_failure(s,r);
    if(present&&next>INT64_MAX) return marconi_timer0_runtime_failure(s,O62M_SHOT_TIME_OVERFLOW);
    if(present) timer_mod_ns(s->timer0_runtime_timer,next);else timer_del(s->timer0_runtime_timer);
    return true;
}

static void marconi_timer0_runtime_callback(void *opaque)
{ marconi_timer0_runtime_refresh(opaque); }

#include "o62m-thermal-mode0-bsp-v2.inc"

/* Adapter views only: the generic provider is the authoritative control word.
 * Existing timed peripherals read these mirrors. Never seed a second value. */
static void marconi_clock_control_mirror(MarconiProbeState *s, uint64_t address,
                                         uint32_t value)
{
    int index = address <= UINT32_MAX ? o62m_boot_clock_index((uint32_t)address) : -1;
    if (index >= 0) {
        s->boot_clocks.values[index] = value;
    }
    if (address == 0x4a09000c) {
        s->uart_clock = value;
    } else if (address == 0x4a0900e4) {
        s->uart_pclk = value;
    }
}

/* Transactional GLOBAL STAT composition. This read creates no key, OTP,
 * software-init write or second word. Validate the real owner before rebinding
 * the temporary pointer; publish context/value only after all checks. */
static O62MTeContextResult marconi_te_global_stat_read(MarconiProbeState *s,
                                                       int64_t now,uint64_t *out)
{
    O62MTeContext trial=s->te_context;
    O62MTeCommonViewV1 view=s->te_common_view;
    uint64_t data=0;
    uint32_t otp=0;
    if(s->virtual_otp_profile&&
       o62m_virtual_otp_global_v1(&s->virtual_otp,now,&otp)!=O62M_VOTP_OK_V1)
        return O62M_TE_CONTEXT_UNINITIALIZED;
    O62MTeContextResult result;
    if(s->trng_virtual_profile) {
        if(view.context_owner!=&s->te_context)return O62M_TE_CONTEXT_UNINITIALIZED;
        view.context_owner=&trial;
        result=o62m_te_common_view_access_v1(&view,&trial,&s->trng_virtual,
                         O62M_TE_CONTEXT_STAT,4,false,&data,now);
        if(result==O62M_TE_CONTEXT_OK&&view.sw_init_done!=s->te_common_view.sw_init_done)
            return O62M_TE_CONTEXT_UNSUPPORTED_TRANSITION;
    } else {
        result=o62m_te_context_access(&trial,O62M_TE_CONTEXT_STAT,4,false,&data,now);
    }
    if(result!=O62M_TE_CONTEXT_OK)return result;
    if(s->virtual_otp_profile&&(data&O62M_VOTP_STAT_MASK_V1))
        return O62M_TE_CONTEXT_BAD_INITIAL;
    s->te_context=trial;*out=data|otp;return O62M_TE_CONTEXT_OK;
}

static bool marconi_ap_te_caller(MarconiProbeState *s)
{
    CPUARMState *env=&s->armv7m.cpu->env;
    return current_cpu==CPU(s->armv7m.cpu)&&!env->v7m.secure&&
           (arm_v7m_is_handler_mode(env)||!(env->v7m.control[0]&1u));
}

/* Virtual display sideband on the board-profile TE pin, GPIO133. */
static void marconi_display_te_refresh(MarconiProbeState *s)
{
    const uint32_t bit=UINT32_C(1)<<5;
    bool enabled=(s->ap_gpio_irq_config[4][0]&bit)!=0;
    bool masked=(s->ap_gpio_irq_config[4][1]&bit)!=0;
    bool routed=(s->ap_gpio_route[2]&bit)!=0;
    bool input=!(s->ap_gpio_direction[4]&bit);
    bool rising=(s->ap_gpio_irq_config[4][2]&bit)&&
        (s->ap_gpio_irq_config[4][3]&bit)&&!(s->ap_gpio_irq_config[4][5]&bit);
    o62m_display_te_config_v1(&s->display_te_input,
        !s->reported&&enabled&&routed&&input&&rising,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    bool level=!s->reported&&o62m_display_te_irq_v1(&s->display_te_input,enabled,masked,routed);
    if(level!=s->display_te_irq_level){
        s->display_te_irq_level=level;
        qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->armv7m),114),level);
    }
    if(s->display_te_input.running)timer_mod_ns(s->display_te_event,s->display_te_input.deadline);
    else timer_del(s->display_te_event);
}
static void marconi_display_te_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    marconi_display_te_refresh(s);
    if(!s->display_te_input.running)return;
    uint64_t before=s->display_te_input.rises;
    o62m_display_te_advance_v1(&s->display_te_input,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    marconi_display_te_refresh(s);
    if(s->display_te_input.rises!=before&&s->display_te_input.rises<=8)
        marconi_public_report("MARCONI_DISPLAY_TE_EDGE pin=133 rising-count=%" PRIu64
            " actual-virtual-input-transition=true queue-and-DPU-untouched=true",s->display_te_input.rises);
}
static void marconi_display_te_reset(void *opaque)
{
    MarconiProbeState *s=opaque;
    timer_del(s->display_te_event);
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->armv7m),114),0);
    s->display_te_irq_level=false;o62m_display_te_reset_v1(&s->display_te_input);
}
static void marconi_display_te_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,display_te_exit);(void)opaque;
    marconi_public_report("MARCONI_DISPLAY_TE_EXIT rises=%" PRIu64 " falls=%" PRIu64
        " actual-W1C-acks=%" PRIu64 " pending=%u timer-cancelled=true no-frame-completion-generated=true",
        s->display_te_input.rises,s->display_te_input.falls,s->display_te_input.acknowledgements,s->display_te_input.pending);
    marconi_display_te_reset(s);
}
static void marconi_xipc_config_callback(void *opaque)
{
 MarconiProbeState *s=opaque;
 if(s->reported){o62m_xipc_config_cancel_v1(&s->xipc_config);return;}
 if(!s->xipc_config.pending||s->xipc_config_armed!=s->xipc_config.generation)return;
 int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
 if(now<s->xipc_config.deadline){timer_mod_ns(s->xipc_config_event,s->xipc_config.deadline);return;}
 if(!o62m_xipc_config_complete_v1(&s->xipc_config,s->xipc_config_armed,now)) {
  marconi_owned_runtime_failure_stop(s);return;
 }
 marconi_public_report("MARCONI_XIPC_CONFIG_COMMIT commits=%" PRIu64 " virtual-ns=%" PRId64 " actual-selector-installed=true no-peer-ACK-or-IRQ=true",s->xipc_config.commits,now);
}
static void marconi_xipc_cleanup_reset(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->xipc_config_event)timer_del(s->xipc_config_event);
    o62m_xipc_config_reset_v1(&s->xipc_config);s->xipc_config_armed=0;
    o62m_virtual_otp_wipe_v1(s->xipc_cleanup_history,sizeof(s->xipc_cleanup_history));
    o62m_virtual_otp_wipe_v1(&s->xipc_cleanup_selector,sizeof(s->xipc_cleanup_selector));
    s->xipc_cleanup_selector_written=false;s->xipc_cleanup_selector_secure=false;
    s->xipc_cleanup_submissions=0;
}
static void marconi_xipc_cleanup_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,xipc_cleanup_exit);(void)opaque;
    uint64_t count=s->xipc_cleanup_submissions,commits=s->xipc_config.commits;unsigned secure=0;
    for(unsigned i=0;i<count;++i)secure+=s->xipc_cleanup_history[i].secure;
    marconi_xipc_cleanup_reset(s);
    o62m_virtual_otp_wipe_v1(&s->xipc_config,sizeof s->xipc_config);
    marconi_public_report("MARCONI_XIPC_CLEANUP_EXIT requests=%" PRIu64 " secure-requests=%u "
        "configuration-commits=%" PRIu64 " no-peer-ACK=true private-history-wiped=%u",count,secure,commits,
        o62m_virtual_otp_zero_v1(s->xipc_cleanup_history,sizeof(s->xipc_cleanup_history))&&
        o62m_virtual_otp_zero_v1(&s->xipc_cleanup_selector,sizeof(s->xipc_cleanup_selector)));
}
static bool marconi_psram_cal_source(MarconiProbeState *s,bool match)
{
    static const unsigned required[]={54,64,63,52,62,51,56,59,55,57,58,61,60};
    if(s->reported||!marconi_pm_domain5_refresh(s)||!s->pm_domain5.on||
       !s->pm_domain5.available||s->pm_domain5.config_pending||s->pm_domain5.transition_pending||
       !s->mempll_vote.locked||!marconi_mempll_source(s))return false;
    for(unsigned i=0;i<ARRAY_SIZE(required);++i)if(!s->psram_init_written[required[i]])return false;
    return !match||(s->psram_cal_revision==s->psram_config_revision&&
        s->psram_cal_mempll==s->mempll_vote.generation&&
        s->psram_cal_config==s->pm_domain5.staging_generation&&
        s->psram_cal_transitions==s->pm_domain5.transitions);
}
static void marconi_psram_cal_cancel(MarconiProbeState *s)
{
    if(s->psram_cal_event)timer_del(s->psram_cal_event);
    o62m_psram_cal_cancel_v1(&s->psram_cal);
}
static bool marconi_psram_controller_source(MarconiProbeState *s,bool match)
{
    return marconi_psram_cal_source(s,true)&&s->psram_cal.valid&&s->psram_cal.applied&&
        (!match||(s->psram_controller_cal_generation==s->psram_cal.generation&&
                  s->psram_controller_captured_revision==s->psram_controller_revision));
}
static void marconi_psram_controller_cancel(MarconiProbeState *s)
{
    if(s->psram_controller_event)timer_del(s->psram_controller_event);
    o62m_psram_controller_cancel_v1(&s->psram_controller);
}
static void marconi_psram_controller_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->reported){marconi_psram_controller_cancel(s);return;}
    if(!s->psram_controller.phase||s->psram_controller_armed!=s->psram_controller.generation||
       !marconi_psram_controller_source(s,true)) {
        marconi_psram_controller_cancel(s);marconi_owned_runtime_failure_stop(s);return;
    }
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(now<s->psram_controller.deadline){timer_mod_ns(s->psram_controller_event,s->psram_controller.deadline);return;}
    unsigned phase=s->psram_controller.phase;
    if(!o62m_psram_controller_complete_v1(&s->psram_controller,s->psram_controller_armed,now,true)) {
        marconi_psram_controller_cancel(s);marconi_owned_runtime_failure_stop(s);return;
    }
    if(s->psram_controller.phase)timer_mod_ns(s->psram_controller_event,s->psram_controller.deadline);
    marconi_public_report("MARCONI_PSRAM_CONTROLLER_COMPLETE phase=%u copies=%" PRIu64
        " initializations=%" PRIu64 " actual-config-or-calibrated-link-copy=true no-memory-or-peer-transaction=true",
        phase,s->psram_controller.copies,s->psram_controller.initializations);
}
static void marconi_psram_cal_reset(void *opaque)
{
    MarconiProbeState *s=opaque;
    timer_del(s->psram_cal_event);o62m_psram_cal_reset_v1(&s->psram_cal);
    timer_del(s->psram_controller_event);o62m_psram_controller_reset_v1(&s->psram_controller);
    ++s->psram_controller_revision;
    o62m_virtual_otp_wipe_v1(s->psram_pairs,sizeof(s->psram_pairs));
    o62m_virtual_otp_wipe_v1(&s->psram_pair_staging,sizeof(s->psram_pair_staging));
    s->psram_pair_count=0;s->psram_pair_staged=false;
    o62m_virtual_otp_wipe_v1(&s->pubcp_dfs_repeat,sizeof(s->pubcp_dfs_repeat));
    o62m_virtual_otp_wipe_v1(s->pubap_restore_history,sizeof(s->pubap_restore_history));s->pubap_restore_count=0;
    o62m_virtual_otp_wipe_v1(s->psram_init_word,sizeof(s->psram_init_word));
    memset(s->psram_init_written,0,sizeof(s->psram_init_written));
    o62m_virtual_otp_wipe_v1(s->psram_init_history,sizeof(s->psram_init_history));
    s->psram_init_count=0;++s->psram_config_revision;
}
static void marconi_psram_cal_callback(void *opaque)
{
    MarconiProbeState *s=opaque;
    if(s->reported){marconi_psram_cal_cancel(s);return;}
    if(!s->psram_cal.pending||s->psram_cal_armed!=s->psram_cal.generation||
       !marconi_psram_cal_source(s,true)) {
        marconi_psram_cal_cancel(s);marconi_owned_runtime_failure_stop(s);return;
    }
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if(now<s->psram_cal.deadline){timer_mod_ns(s->psram_cal_event,s->psram_cal.deadline);return;}
    if(!o62m_psram_cal_complete_v1(&s->psram_cal,s->psram_cal_armed,now,true)) {
        marconi_psram_cal_cancel(s);marconi_owned_runtime_failure_stop(s);return;
    }
    marconi_public_report("MARCONI_PSRAM_CAL_COMPLETE generation=%" PRIu64 " completions=%" PRIu64
        " virtual-reference-delayline-calculated=true positive-delay=true no-controller-or-peer-completion=true",
        s->psram_cal.generation,s->psram_cal.completions);
}

static void marconi_psram_init_exit(Notifier *notifier,void *opaque)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,psram_init_exit);(void)opaque;
    unsigned count=s->psram_init_count;
    unsigned restore=s->pubap_restore_count;
    o62m_virtual_otp_wipe_v1(s->pubap_restore_history,sizeof(s->pubap_restore_history));s->pubap_restore_count=0;
    marconi_public_report("MARCONI_PUBAP_RESTORE_EXIT writes=%u requests-unconsumed=true private-history-wiped=%u",
        restore,o62m_virtual_otp_zero_v1(s->pubap_restore_history,sizeof(s->pubap_restore_history)));
    unsigned dfs_writes=s->pubcp_dfs_repeat.step;
    o62m_virtual_otp_wipe_v1(&s->pubcp_dfs_repeat,sizeof(s->pubcp_dfs_repeat));
    marconi_public_report("MARCONI_PUBCP_DFS_RECONFIG_EXIT writes=%u first-boot-phase=%u "
        "requests-unconsumed=true private-history-wiped=%u",dfs_writes,s->dfs_config.phase,
        o62m_virtual_otp_zero_v1(&s->pubcp_dfs_repeat,sizeof(s->pubcp_dfs_repeat)));
    unsigned pairs=s->psram_pair_count;bool staged=s->psram_pair_staged;
    o62m_virtual_otp_wipe_v1(s->psram_pairs,sizeof(s->psram_pairs));
    o62m_virtual_otp_wipe_v1(&s->psram_pair_staging,sizeof(s->psram_pair_staging));
    s->psram_pair_count=0;s->psram_pair_staged=false;
    marconi_public_report("MARCONI_PSRAM_PAIR_EXIT accepted=%u unconsumed=%u staged=%u private-wiped=%u",
        pairs,pairs,staged,o62m_virtual_otp_zero_v1(s->psram_pairs,sizeof(s->psram_pairs))&&
        o62m_virtual_otp_zero_v1(&s->psram_pair_staging,sizeof(s->psram_pair_staging)));
    unsigned ports=s->psram_controller.ports;
    marconi_psram_controller_cancel(s);
    o62m_virtual_otp_wipe_v1(s->psram_controller.pending_words,sizeof(s->psram_controller.pending_words));
    o62m_virtual_otp_wipe_v1(s->psram_controller.active_words,sizeof(s->psram_controller.active_words));
    o62m_virtual_otp_wipe_v1(s->psram_controller.pending_phy,sizeof(s->psram_controller.pending_phy));
    o62m_virtual_otp_wipe_v1(s->psram_controller.active_phy,sizeof(s->psram_controller.active_phy));
    marconi_public_report("MARCONI_PSRAM_CONTROLLER_EXIT requests=%" PRIu64 " copies=%" PRIu64
        " initializations=%" PRIu64 " cancelled=%" PRIu64 " activated-port-mask=%u reads=%" PRIu64
        " pending=0 private-active-and-pending-wiped=%u",
        s->psram_controller.requests,s->psram_controller.copies,s->psram_controller.initializations,
        s->psram_controller.cancelled,ports,s->psram_controller_reads,
        o62m_virtual_otp_zero_v1(s->psram_controller.pending_words,sizeof(s->psram_controller.pending_words))&&
        o62m_virtual_otp_zero_v1(s->psram_controller.active_words,sizeof(s->psram_controller.active_words))&&
        o62m_virtual_otp_zero_v1(s->psram_controller.active_phy,sizeof(s->psram_controller.active_phy)));
    marconi_psram_cal_cancel(s);
    marconi_public_report("MARCONI_PSRAM_CAL_EXIT requests=%" PRIu64 " completions=%" PRIu64
        " applies=%" PRIu64 " cancelled=%" PRIu64 " reads=%" PRIu64 " pending=0 result-invalid=true",
        s->psram_cal.requests,s->psram_cal.completions,s->psram_cal.applies,s->psram_cal.cancelled,s->psram_cal_reads);
    o62m_virtual_otp_wipe_v1(s->psram_init_word,sizeof(s->psram_init_word));
    o62m_virtual_otp_wipe_v1(s->psram_init_written,sizeof(s->psram_init_written));
    o62m_virtual_otp_wipe_v1(s->psram_init_history,sizeof(s->psram_init_history));
    s->psram_init_count=0;
    marconi_public_report("MARCONI_PSRAM_INIT_EXIT writes=%u original-write-history=true "
        "state-wiped=%u",count,
        o62m_virtual_otp_zero_v1(s->psram_init_word,sizeof(s->psram_init_word))&&
        o62m_virtual_otp_zero_v1(s->psram_init_history,sizeof(s->psram_init_history)));
}

static bool marconi_lps_pm_caller(MarconiProbeState *s, uint32_t pc, uint32_t lr)
{
    CPUARMState *env=&s->armv7m.cpu->env;
    return current_cpu==CPU(s->armv7m.cpu)&&env->v7m.secure&&
        (arm_v7m_is_handler_mode(env)||!(env->v7m.control[1]&1u))&&
        CPU_GET_CLASS(current_cpu)->get_pc(current_cpu)==pc&&env->regs[14]==lr;
}

static void marconi_cp_cold_inputs(MarconiProbeState *s,uint32_t timing[3])
{
 memset(timing,0,3*sizeof(uint32_t));
 for(unsigned j=0;j<3;j++)for(unsigned i=0;i<O62M_AP_RUNTIME_CONFIG_WORDS_V1;i++)
  if(o62m_ap_runtime_config_addresses_v1[i]==UINT64_C(0x4a0c0090)+4*j)timing[j]=s->ap_runtime_words[i];
}
static bool marconi_cp_cold_source(MarconiProbeState *s)
{
 uint32_t timing[3];marconi_cp_cold_inputs(s,timing);
 return o62m_cp_cold_source_v3(&s->cp_cold,s->pm_functional.config[1],timing,
      s->pm_domain1_timing_written==7&&s->ap_timing_commit_request==1&&s->pm_functional.source_commit_request[1]==1);
}
static const uint32_t marconi_bbpll_addresses[46]={0x44190004,0x44190008,0x4419000c,0x4419001c,0x44190020,0x4419003c,0x44190080,0x44190084,0x4412002c,0x441200e4,0x4c0101d4,0x441600c8,0x441600cc,0x441600d0,0x441600d4,0x441600ec,0x441600f8,0x441600f4,0x441600f0,0x44160050,0x44160054,0x44160058,0x44160064,0x44160068,0x4416006c,0x44160070,0x44160074,0x44160078,0x4416007c,0x44160080,0x44160084,0x44160088,0x4416008c,0x44160090,0x44160094,0x44160098,0x4416009c,0x441600a0,0x441600a4,0x441600a8,0x441600ac,0x4416001c,0x441100e0,0x44120118,0x4412011c,0x44120120};
static bool marconi_bbpll_source(MarconiProbeState *s)
{
 uint16_t a,b;
 return s->cp_created&&s->cp_cold.release_accepted&&s->pm_domain1_on&&s->pm_domain1_available&&
  marconi_cp_cold_source(s)&&o62m_pmic_regulator_read_v1(&s->pmic_regulators,0x60,&a)==0&&
  o62m_pmic_regulator_read_v1(&s->pmic_regulators,0x64,&b)==0&&(a&0x40)&&(b&0x2100)==0x2100;
}
static void marconi_bbpll_cancel(MarconiProbeState *s)
{if(s->bbpll_event)timer_del(s->bbpll_event);o62m_bbpll_cancel_v1(&s->bbpll);marconi_rfspim_source_changed(s);}
static void marconi_bbpll_source_changed(MarconiProbeState *s)
{if((s->bbpll.pending||s->bbpll.locked)&&!marconi_bbpll_source(s))marconi_bbpll_cancel(s);}
static void marconi_bbpll_reset(void *opaque)
{MarconiProbeState *s=opaque;if(s->bbpll_event)timer_del(s->bbpll_event);o62m_bbpll_reset_v1(&s->bbpll);s->bbpll_armed=0;}
static void marconi_bbpll_callback(void *opaque)
{
 MarconiProbeState *s=opaque;int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
 if(s->reported||!marconi_bbpll_source(s)){marconi_bbpll_cancel(s);return;}
 if(!s->bbpll.pending||s->bbpll_armed!=s->bbpll.generation)return;
 if(now<s->bbpll.deadline){timer_mod_ns(s->bbpll_event,s->bbpll.deadline);return;}
 if(!o62m_bbpll_complete_v1(&s->bbpll,s->bbpll_armed,now,true)){
  marconi_bbpll_cancel(s);marconi_owned_runtime_failure_stop(s);return;
 }
 marconi_public_report("MARCONI_CP_BBPLL_ACTIVE requests=%" PRIu64 " completions=%" PRIu64 " rate-Hz=%" PRIu64 " original-table-match=true actual-config-installed=true positive-reference-qualification=true",s->bbpll.requests,s->bbpll.completions,s->bbpll.rate_hz);
}
static void marconi_bbpll_exit(Notifier *notifier,void *opaque)
{
 MarconiProbeState *s=container_of(notifier,MarconiProbeState,bbpll_exit);(void)opaque;
 uint64_t requests=s->bbpll.requests,completed=s->bbpll.completions;
 marconi_rfspim_cancel(s);marconi_bbpll_cancel(s);o62m_virtual_ao_wipe_v1(&s->bbpll,sizeof s->bbpll);
 marconi_public_report("MARCONI_CP_BBPLL_EXIT requests=%" PRIu64 " completions=%" PRIu64 " timer-cancelled=true private-config-wiped=true",requests,completed);
}
static bool marconi_bbpll_port(hwaddr address)
{
 if(address==0x44190040)return true;
 for(unsigned i=0;i<46;i++)if(address==marconi_bbpll_addresses[i])return true;
 return false;
}
static bool marconi_bbpll_access(MarconiProbeState *s,hwaddr address,uint64_t *value,unsigned size,bool write)
{
 if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||size!=4||s->reported||
    !s->cp_cold.release_accepted||!marconi_cp_cold_source(s)||(write&&*value>UINT32_MAX))return false;
 uint64_t pc=marconi_mmio_pc();
 marconi_bbpll_source_changed(s);
 if(s->bbpll.pending&&s->bbpll.deadline<=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)){
  timer_del(s->bbpll_event);marconi_bbpll_callback(s);if(s->reported)return false;
 }
 if(address==0x44190040){if(write||(pc!=0x641995c0&&pc!=0x641995d4&&pc!=0x6418b74c))return false;*value=s->bbpll.locked?1:0;return true;}
 if(!write)for(unsigned i=0;i<46;i++)
  if(address==marconi_bbpll_addresses[i]&&(s->bbpll.written&(UINT64_C(1)<<i))) {
   *value=s->bbpll.word[i];return true;
  }
 static const struct {uint32_t pc;uint8_t index;bool write;} access[]={{0x64050c30,43,false},{0x6418b78c,44,true},{0x6418b78e,45,true},{0x6418b790,43,true},{0x6418b792,45,true},{0x6418b736,8,false},{0x6418b73c,8,true},{0x6418b744,0,false},{0x6418b74a,0,true},{0x6418b75a,41,true},{0x6418b75c,8,false},{0x6418b762,8,true},{0x6418b76c,10,false},{0x6418b772,10,true},{0x6419947e,1,true},{0x64199492,8,true},{0x641994a8,9,true},{0x641994be,10,true},{0x641994ca,11,true},{0x641994d2,12,true},{0x641994d6,13,true},{0x641994de,14,true},{0x641994e4,15,true},{0x641994ea,16,true},{0x641994f2,17,true},{0x641994f6,18,true},{0x641994fa,19,true},{0x64199500,20,true},{0x64199508,21,true},{0x6419950a,22,true},{0x6419950c,23,true},{0x6419950e,24,true},{0x64199510,25,true},{0x64199512,26,true},{0x64199514,27,true},{0x6419951e,28,true},{0x64199520,29,true},{0x64199526,30,true},{0x64199528,31,true},{0x6419952a,32,true},{0x6419952c,33,true},{0x64199534,34,true},{0x64199536,35,true},{0x64199538,36,true},{0x64199544,37,true},{0x64199548,38,true},{0x64199554,39,true},{0x6419955c,40,true},{0x64199560,41,true},{0x64199564,2,true},{0x6419956c,0,true},{0x64199576,3,true},{0x6419957e,4,true},{0x64199586,0,true},{0x6419958e,0,true},{0x64199592,6,true},{0x64199598,7,true},{0x6419959c,6,true},{0x641995a0,42,true},{0x641995aa,8,true},{0x641995b4,9,true},{0x641995be,0,true},{0x6419960e,43,true},{0x64199614,5,true},{0x64199618,44,true},{0x6419961c,45,true},{0x64199484,8,false},{0x64199494,9,false},{0x641994b8,10,false},{0x64199566,0,false},{0x6419956e,3,false},{0x64199578,4,false},{0x64199580,0,false},{0x64199588,0,false},{0x641995a4,8,false},{0x641995ac,9,false},{0x641995b8,0,false}};
 unsigned i=46;
 for(unsigned j=0;j<ARRAY_SIZE(access);j++)if(access[j].pc==pc&&access[j].write==write&&
     address==marconi_bbpll_addresses[access[j].index]){i=access[j].index;break;}
 if(write&&pc==UINT64_C(0x641217aa)) {
  if(address==UINT64_C(0x44120120)&&((*value^s->bbpll.word[45])&~UINT64_C(1))==0)i=45;
  if(address==UINT64_C(0x4412002c)&&((*value^s->bbpll.word[8])&~UINT64_C(0x20000))==0)i=8;
  if(address==UINT64_C(0x441200e4)&&((*value^s->bbpll.word[9])&~UINT64_C(0x20000))==0)i=9;
 }
 if(i==46)return false;
 if(!write){*value=s->bbpll.word[i];return true;}
 if(!marconi_rfspim_clock_before_change(s))return false;
 bool pending=s->bbpll.pending;
 if(!o62m_bbpll_write_v1(&s->bbpll,i,(uint32_t)*value,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),marconi_bbpll_source(s)))return false;
 if(pending&&!s->bbpll.pending)timer_del(s->bbpll_event);
 if(s->bbpll.pending){s->bbpll_armed=s->bbpll.generation;timer_mod_ns(s->bbpll_event,s->bbpll.deadline);}
 marconi_rfspim_source_changed(s);
 return true;
}

static void marconi_cp_cold_cancel(MarconiProbeState *s)
{
 marconi_cp_timer_stop(s);
 if(s->cp_wdt_event)timer_del(s->cp_wdt_event);
 if(s->cp_cold_event)timer_del(s->cp_cold_event);
 marconi_bbpll_cancel(s);
 o62m_cp_cold_cancel_v1(&s->cp_cold);s->cp_cold_armed=0;
 s->pm_domain1_on=s->pm_domain1_available=false;
}
static void marconi_cp_cold_source_changed(MarconiProbeState *s)
{
 if(s->cp_created&&s->cp_cold.stage>=6&&!marconi_cp_cold_source(s)) {
  marconi_cp_cold_cancel(s);marconi_owned_runtime_failure_stop(s);
 }
}
static void marconi_cp_cold_callback(void *opaque)
{
 MarconiProbeState *s=opaque;int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
 if(s->reported){marconi_cp_cold_cancel(s);return;}
 if(!s->cp_cold.phase||s->cp_cold.generation!=s->cp_cold_armed||!marconi_cp_cold_source(s)) {
  marconi_cp_cold_cancel(s);marconi_owned_runtime_failure_stop(s);return;
 }
 if(now<s->cp_cold.deadline){timer_mod_ns(s->cp_cold_event,s->cp_cold.deadline);return;}
 if(!o62m_cp_cold_complete_v1(&s->cp_cold,s->cp_cold_armed,now)) {
  marconi_cp_cold_cancel(s);marconi_owned_runtime_failure_stop(s);return;
 }
 s->pm_domain1_on=s->cp_cold.on;s->pm_domain1_available=s->cp_cold.available;
 marconi_public_report("MARCONI_CP_COLD_WORK phase=%u config-commits=%" PRIu64 " domain-transitions=%" PRIu64 " virtual-ns=%" PRId64 " actual-config-copy=true CPU-release=false",s->cp_cold.phase,s->cp_cold.config_commits,s->cp_cold.transitions,now);
 if(s->cp_cold.phase)timer_mod_ns(s->cp_cold_event,s->cp_cold.deadline);
}
static uint64_t marconi_cp_atcm_read(CPUARMState *env,const ARMCPRegInfo *ri)
{
 MarconiProbeState *s=ri->opaque;(void)env;return s->cp_atcm_control;
}
static void marconi_cp_atcm_write(CPUARMState *env,const ARMCPRegInfo *ri,uint64_t value)
{
 MarconiProbeState *s=ri->opaque;(void)env;
 /* This board places its sole populated TCM bank at zero. Size is read-only. */
 if(value&UINT64_C(0xfffc0000)) {
  marconi_public_report("MARCONI_CP_ATCM_BOUNDARY unsupported-base=true state-unchanged=true");
  marconi_owned_runtime_failure_stop(s);return;
 }
 uint32_t control=0x24u|(value&1u);++s->cp_atcm_writes;
 if(control!=s->cp_atcm_control) {
  s->cp_atcm_control=control;++s->cp_atcm_changes;
  memory_region_set_enabled(&s->cp_atcm,(control&1u)!=0);
 }
 marconi_public_report("MARCONI_CP_ATCM_CONFIG writes=%" PRIu64 " mapping-changes=%" PRIu64 " enabled=%u virtual-capacity=262144 guest-copy-required=true",s->cp_atcm_writes,s->cp_atcm_changes,control&1u);
}
static void marconi_cp_atcm_reset(CPUARMState *env,const ARMCPRegInfo *ri)
{
 MarconiProbeState *s=ri->opaque;(void)env;
 s->cp_atcm_control=0x24;memory_region_set_enabled(&s->cp_atcm,false);
}
static const ARMCPRegInfo marconi_cp_atcm_regs[]={
 { .name="ATCM",.cp=15,.opc1=0,.crn=9,.crm=1,.opc2=1,.access=PL1_RW,
   .type=ARM_CP_OVERRIDE|ARM_CP_IO,.readfn=marconi_cp_atcm_read,
   .writefn=marconi_cp_atcm_write,.resetfn=marconi_cp_atcm_reset },
};
static void marconi_cp_atcm_exit(Notifier *notifier,void *opaque)
{
 MarconiProbeState *s=container_of(notifier,MarconiProbeState,cp_atcm_exit);(void)opaque;
 memory_region_set_enabled(&s->cp_atcm,false);s->cp_atcm_control=0x24;
 volatile uint8_t *data=memory_region_get_ram_ptr(&s->cp_atcm);
 for(size_t i=0;i<0x40000;i++)data[i]=0;
 marconi_public_report("MARCONI_CP_ATCM_EXIT writes=%" PRIu64 " mapping-changes=%" PRIu64 " private-bytes-wiped=262144 overlay-disabled=true",s->cp_atcm_writes,s->cp_atcm_changes);
}
static void marconi_cp_cold_reset(void *opaque)
{
 MarconiProbeState *s=opaque;marconi_cp_timer_reset(s);if(s->cp_cold_event)timer_del(s->cp_cold_event);
 o62m_cp_cold_reset_v1(&s->cp_cold);s->cp_cold_armed=0;s->pm_domain1_on=s->pm_domain1_available=false;s->pm_domain6_status=0;
 if(s->cp_created)cpu_reset(CPU(&s->cp_cpu));
}
static void marconi_cp_cold_exit(Notifier *notifier,void *opaque)
{
 MarconiProbeState *s=container_of(notifier,MarconiProbeState,cp_cold_exit);(void)opaque;
 uint64_t requests=s->cp_cold.requests,configs=s->cp_cold.config_commits,transitions=s->cp_cold.transitions;bool released=s->cp_cold.release_accepted;
 s->pm_domain6_status=0;
 marconi_rfspim_cancel(s);marconi_cp_cold_cancel(s);volatile uint8_t *p=(volatile uint8_t *)&s->cp_cold;for(size_t i=0;i<sizeof s->cp_cold;i++)p[i]=0;
 marconi_public_report("MARCONI_CP_COLD_EXIT requests=%" PRIu64 " config-commits=%" PRIu64 " domain-transitions=%" PRIu64 " release-accepted=%u timer-cancelled=true private-config-wiped=true execution-success-not-inferred=true",requests,configs,transitions,released);
}
static bool marconi_cp_cold_port(hwaddr address)
{
 static const uint64_t ports[]={0x5c010110,0x4c010110,0x5c01010c,0x5a0c0004,0x5a0c0028,0x5c0300c4,0x5c030118,0x5c030100,0x5c0300e8,0x54120000,0x44120000,0x5c0101e8};
 for(unsigned i=0;i<ARRAY_SIZE(ports);i++)if(address==ports[i])return true;return false;
}
static uint64_t marconi_mmio_pc(void)
{
 if(!current_cpu)return UINT64_MAX;
 uint64_t pc=CPU_GET_CLASS(current_cpu)->get_pc(current_cpu),data[INSN_START_WORDS];
 if(!cpu_unwind_state_data(current_cpu,current_cpu->mem_io_pc,data))return pc;
 return tcg_cflags_has(current_cpu,CF_PCREL)?(pc&TARGET_PAGE_MASK)|data[0]:data[0];
}
static bool marconi_cp_ports_source(MarconiProbeState *s,unsigned port,bool match)
{
 if(s->reported||!s->cp_cold.release_accepted||!s->pm_domain1_on||!s->pm_domain1_available||
    (s->xipc_config.active&0x18)!=0x18)return false;
 if(!port&&(!marconi_psram_controller_source(s,true)||!s->psram_controller.operating||!s->psram_init_written[83]))return false;
 return !match||(s->cp_ports_cold_generation[port]==s->cp_cold.generation&&
    (port||(s->cp_ports_controller_generation==s->psram_controller.generation&&
            s->cp_ports_cal_generation==s->psram_cal.generation)));
}
static void marconi_cp_ports_cancel(MarconiProbeState *s)
{
 if(s->cp_ports_event)timer_del(s->cp_ports_event);
 for(unsigned i=0;i<2;i++){o62m_cp_boot_config_cancel_v1(&s->cp_ports[i]);s->cp_ports_armed[i]=0;}
}
static void marconi_cp_ports_schedule(MarconiProbeState *s)
{
 int64_t deadline=INT64_MAX;for(unsigned i=0;i<2;i++)if(s->cp_ports[i].pending&&s->cp_ports[i].deadline<deadline)deadline=s->cp_ports[i].deadline;
 if(deadline!=INT64_MAX)timer_mod_ns(s->cp_ports_event,deadline);
}
static void marconi_cp_ports_callback(void *opaque)
{
 MarconiProbeState *s=opaque;int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
 if(s->reported){marconi_cp_ports_cancel(s);return;}
 for(unsigned i=0;i<2;i++)if(s->cp_ports[i].pending) {
  uint32_t control=i?s->cp_port820_control:s->psram_init_word[83];
  if(s->cp_ports_armed[i]!=s->cp_ports[i].generation||!marconi_cp_ports_source(s,i,true)||control!=s->cp_ports[i].requested) {
   marconi_cp_ports_cancel(s);marconi_owned_runtime_failure_stop(s);return;
  }
  if(now<s->cp_ports[i].deadline)continue;
  if(!o62m_cp_boot_config_complete_v1(&s->cp_ports[i],s->cp_ports_armed[i],now)) {
   marconi_cp_ports_cancel(s);marconi_owned_runtime_failure_stop(s);return;
  }
  marconi_public_report("MARCONI_CP_BOOT_CONFIG_COMMIT port=%u commits=%" PRIu64 " virtual-ns=%" PRId64 " actual-config-copy=true own-config-work-only=true no-peer-or-DMA-completion=true",i,s->cp_ports[i].commits,now);
 }
 marconi_cp_ports_schedule(s);
}
static void marconi_cp_ports_reset(void *opaque)
{
 MarconiProbeState *s=opaque;marconi_cp_ports_cancel(s);
 for(unsigned i=0;i<2;i++)o62m_cp_boot_config_reset_v1(&s->cp_ports[i]);
 s->cp_port820_control=0;s->cp_ports_controller_generation=s->cp_ports_cal_generation=0;
 memset(s->cp_ports_cold_generation,0,sizeof s->cp_ports_cold_generation);
}
static void marconi_cp_ports_exit(Notifier *notifier,void *opaque)
{
 MarconiProbeState *s=container_of(notifier,MarconiProbeState,cp_ports_exit);(void)opaque;
 uint64_t requests=s->cp_ports[0].requests+s->cp_ports[1].requests,commits=s->cp_ports[0].commits+s->cp_ports[1].commits;
 marconi_cp_ports_reset(s);o62m_virtual_otp_wipe_v1(s->cp_ports,sizeof s->cp_ports);
 marconi_public_report("MARCONI_CP_BOOT_CONFIG_EXIT requests=%" PRIu64 " commits=%" PRIu64 " timer-cancelled=true private-config-wiped=true",requests,commits);
}
static void marconi_cp_aon_reset(void *opaque)
{
 MarconiProbeState *s=opaque;memset(s->cp_aon_requests,0,sizeof s->cp_aon_requests);
 memset(s->cp_aon_request_ns,0,sizeof s->cp_aon_request_ns);s->cp_aon_request_count=0;
}
static void marconi_cp_aon_exit(Notifier *notifier,void *opaque)
{
 MarconiProbeState *s=container_of(notifier,MarconiProbeState,cp_aon_exit);(void)opaque;
 unsigned count=s->cp_aon_request_count;marconi_cp_aon_reset(s);
 s->ap_watchdog_aon_word=0;s->ap_watchdog_aon_written=false;
 marconi_public_report("MARCONI_CP_AON_PAIR_EXIT requests=%u original-AP-history-preserved-until-exit=true private-request-history-wiped=true reset-completion-unpublished=true",count);
}

static bool marconi_rfspim_source(MarconiProbeState *s,uint64_t *frame_ns);
static bool marconi_crf_source(MarconiProbeState *s)
{
 return marconi_cp_cold_source(s)&&(s->pmic_clocks.word[0]&0x400)&&s->cp_aon_request_count==2;
}
static int marconi_crf_config_index(uint32_t address)
{
 for(unsigned i=0;i<ARRAY_SIZE(o62m_crf_config_registers_v1);i++)
  if(address==o62m_crf_config_registers_v1[i])return i;
 return -1;
}
static void marconi_crf_reset_devices(MarconiProbeState *s)
{
 if(!s->crf_created)return;
 device_cold_reset(DEVICE(&s->crf_armv7m.systick[M_REG_NS]));
 device_cold_reset(DEVICE(&s->crf_armv7m.nvic));
 cpu_reset(CPU(s->crf_armv7m.cpu));
}
static void marconi_crf_reset(void *opaque)
{
 MarconiProbeState *s=opaque;marconi_crf_reset_devices(s);
 if(s->crf_created)o62m_virtual_otp_wipe_v1(memory_region_get_ram_ptr(&s->crf_ram),0xc000);
 memset(s->crf_config,0,sizeof s->crf_config);s->crf_download_next=0;
 s->crf_release_accepted=false;s->crf_entry_zero=false;s->crf_writes=0;s->crf_stop_requests=0;
 s->crf_control_requests=s->crf_optional_reads=0;memset(s->crf_optional_config,0,sizeof s->crf_optional_config);
 o62m_virtual_otp_wipe_v1(&s->crf_control_d000,sizeof s->crf_control_d000);s->crf_control_d000_requests=0;
}
static void marconi_crf_exit(Notifier *notifier,void *opaque)
{
 MarconiProbeState *s=container_of(notifier,MarconiProbeState,crf_exit);(void)opaque;
 bool released=s->crf_release_accepted;uint64_t writes=s->crf_writes;
 uint64_t stops=s->crf_stop_requests;bool stopped=s->crf_created&&s->crf_armv7m.cpu->power_state==PSCI_OFF;
 marconi_public_report("MARCONI_CRF_D000_EXIT received-control-requests=%" PRIu64 " effect-unimplemented=true",s->crf_control_d000_requests);
 marconi_public_report("MARCONI_CRF_CONFIG_EXIT control-requests=%" PRIu64 " optional-input-reads=%" PRIu64 " control-work-completion-unimplemented=true",s->crf_control_requests,s->crf_optional_reads);
 marconi_public_report("MARCONI_CRF_STOP_EXIT accepted-requests=%" PRIu64 " architectural-powered-off=%u",stops,stopped);
 marconi_crf_reset(s);
 marconi_public_report("MARCONI_CRF_EXIT release-accepted=%u target-writes=%" PRIu64 " CPU-powered-off=true private-RAM-and-config-wiped=true",released,writes);
}
static MemTxResult marconi_crf_mmio(void *opaque,hwaddr address,uint64_t *value,unsigned size,bool write)
{
 MarconiProbeState *s=opaque;
 if(s->reported)return MEMTX_ERROR;
 int i=address>=0x4000c000&&address<=0x4000ffff?marconi_crf_config_index(address-0x40000000):-1;
 uint64_t pc=marconi_mmio_pc();bool access=false;
 if(s->crf_release_accepted&&marconi_crf_source(s)&&!write&&size==2&&pc==0x1910&&
    address>=0x4000ea00&&address<=0x4000ea24&&(address&3)==0) {
  *value=s->crf_optional_config[(address-0x4000ea00)/4];++s->crf_optional_reads;
  if(s->crf_optional_reads==1)marconi_public_report("MARCONI_CRF_OPTIONAL_INPUT empty-ten-word-virtual-configuration=true original-software-marker-unmodified=true calibration-success-unclaimed=true");
  return MEMTX_OK;
 }
 for(unsigned j=0;j<ARRAY_SIZE(o62m_crf_init_access_v1);j++)
  if(pc==o62m_crf_init_access_v1[j].pc&&address==UINT64_C(0x40000000)+o62m_crf_init_access_v1[j].address&&
     size==o62m_crf_init_access_v1[j].size&&write==o62m_crf_init_access_v1[j].write)access=true;
 if(s->crf_release_accepted&&marconi_crf_source(s)&&i>=0&&access) {
  if(write){
   s->crf_config[i]=size==2?(uint16_t)*value:(uint32_t)*value;++s->crf_writes;
   if(address==0x4000c50c||address==0x4000c70c||address==0x4000c520||address==0x4000c528)++s->crf_control_requests;
  }else *value=s->crf_config[i];
  return MEMTX_OK;
 }
 marconi_public_report("MARCONI_CRF_DEVICE_BOUNDARY pc=%08" PRIx64 " address=%08" HWADDR_PRIx " size=%u access=%s data-redacted=true",marconi_mmio_pc(),address,size,write?"write":"read");
 marconi_owned_runtime_failure_stop(s);return MEMTX_ERROR;
}
static MemTxResult marconi_crf_read(void *opaque,hwaddr address,uint64_t *value,unsigned size,MemTxAttrs attrs)
{(void)attrs;return marconi_crf_mmio(opaque,address,value,size,false);}
static MemTxResult marconi_crf_write(void *opaque,hwaddr address,uint64_t value,unsigned size,MemTxAttrs attrs)
{(void)attrs;return marconi_crf_mmio(opaque,address,&value,size,true);}
static const MemoryRegionOps marconi_crf_ops={.read_with_attrs=marconi_crf_read,.write_with_attrs=marconi_crf_write,
 .endianness=DEVICE_LITTLE_ENDIAN,.valid.min_access_size=1,.valid.max_access_size=4,.impl.min_access_size=1,.impl.max_access_size=4};
static bool marconi_crf_receive_write(MarconiProbeState *s,uint32_t frame)
{
 uint32_t address=(frame>>15)&0xfffe;uint16_t value=(uint16_t)frame;
 if(!(frame&0x80000000)||!s->crf_created)return false;
 if(address<0xc000) {
  uint8_t data[2];stw_le_p(data,value);
  if(address_space_write(&s->crf_as,address,MEMTXATTRS_UNSPECIFIED,data,2)!=MEMTX_OK)return false;
  if(address==s->crf_download_next&&address<0x7530)s->crf_download_next+=2;
  ++s->crf_writes;return true;
 }
 if(address==0xd000) {
  if(value!=0x20)return false;
  s->crf_control_d000=value;++s->crf_control_d000_requests;++s->crf_control_requests;++s->crf_writes;
  marconi_public_report("MARCONI_CRF_D000_REQUEST received=true actual-frame-completion=true effect-unimplemented=true");
  return true;
 }
 int i=marconi_crf_config_index(address);if(i<0)return false;
 if(address==0xd078) {
  if(value==0xff) {
   if(!s->crf_release_accepted&&!s->crf_stop_requests)return false;
   int result=arm_set_cpu_off(UINT64_C(0x200));
   if(result!=QEMU_ARM_POWERCTL_RET_SUCCESS&&result!=QEMU_ARM_POWERCTL_IS_OFF)return false;
   device_cold_reset(DEVICE(&s->crf_armv7m.systick[M_REG_NS]));
   device_cold_reset(DEVICE(&s->crf_armv7m.nvic));
   s->crf_release_accepted=false;s->crf_entry_zero=false;++s->crf_stop_requests;
   marconi_public_report("MARCONI_CRF_STOP accepted=true actual-frame-completion=true architectural-power-off-requested=true downloaded-RAM-retained=true PC-SP-patch=false");
  } else {
   if(value||s->crf_release_accepted)return false;
   s->crf_entry_zero=true;
  }
 }
 if(address==0xd07c) {
  uint64_t ns;
  if(value!=1||s->crf_release_accepted||!s->crf_entry_zero||s->crf_download_next!=0x7530||
     !marconi_crf_source(s)||!marconi_rfspim_source(s,&ns)||!marconi_enable_private_guest_diagnostics(s))return false;
  device_cold_reset(DEVICE(&s->crf_armv7m.systick[M_REG_NS]));
  device_cold_reset(DEVICE(&s->crf_armv7m.nvic));
  if(arm_set_cpu_on_and_reset(UINT64_C(0x200))!=QEMU_ARM_POWERCTL_RET_SUCCESS)return false;
  s->crf_release_accepted=true;
  marconi_public_report("MARCONI_CRF_RELEASE accepted=true actual-downloaded-bytes=%u architectural-reset-vector=true PC-SP-patch=false virtual-cpu=cortex-m0 virtual-clock-hz=26000000",s->crf_download_next);
 }
 s->crf_config[i]=value;++s->crf_writes;return true;
}

#define MARCONI_RFSPIM_RECORD_WORDS 32768u
static bool marconi_rfspim_clock_rate(MarconiProbeState *s,uint64_t *rate)
{
 if(!marconi_cp_cold_source(s)||
    (s->bbpll.written&(UINT64_C(1)<<43))==0||(s->bbpll.written&(UINT64_C(1)<<45))==0||
    s->cp_ssi_config[9]!=1||
    s->cp_ssi_config[4]!=0x011f0040||s->cp_ssi_config[0]!=6||
    s->cp_ssi_config[2]!=0||s->cp_ssi_config[5]!=9)return false;
 uint64_t parent;
 if(s->bbpll.word[43]&1) {
  if(!s->bbpll.locked||!marconi_bbpll_source(s))return false;
  const uint32_t *w=s->bbpll.active;
  uint64_t scaled=((((uint64_t)(w[3]&4095)<<24)|(w[4]&0xffffff))*26000000)>>24;
  unsigned denom=(w[0]&63)*((w[0]>>9)&7)*((w[0]>>6)&7);
  if(!denom)return false;
  parent=((scaled+denom-1)/denom+1)>>1;
 } else {
  if(!s->cp_xtal26m_profile_enabled)return false;
  parent=26000000;
 }
 unsigned divider=((s->cp_rfspim_divider>>8)&3)+1;
 *rate=((s->bbpll.word[45]&1)&&(s->cp_rfspim_divider&0x10000)&&
        (s->cp_ssi_clock[0]&0x20000)&&(s->cp_ssi_clock[1]&0x1000000))?(parent+divider-1)/divider:0;
 return true;
}
static bool marconi_rfspim_source(MarconiProbeState *s,uint64_t *frame_ns)
{
 uint64_t rate;
 if(!marconi_rfspim_clock_rate(s,&rate)||!rate)return false;
 *frame_ns=(UINT64_C(32)*s->cp_ssi_config[0]*1000000000+rate-1)/rate;return true;
}
static void marconi_rfspim_cancel(MarconiProbeState *s)
{
 if(s->cp_rfspim_event)timer_del(s->cp_rfspim_event);
 if(s->cp_ssi_tx.count||s->cp_ssi_tx.running)o62m_cp_rfspim_cancel_v2(&s->cp_ssi_tx);
 s->cp_rfspim_armed=0;o62m_rfspim_progress_reset_v1(&s->cp_rfspim_progress);
 if(s->cp_rfspim_read_pending)++s->cp_rfspim_read_cancelled;
 s->cp_rfspim_read_pending=s->cp_rfspim_read_running=s->cp_rfspim_rx_valid=false;
 s->cp_rfspim_rx=0;s->cp_rfspim_read_address=0;
}
static void marconi_rfspim_source_changed_at(MarconiProbeState *s,int64_t now)
{
 uint64_t rate;
 if(s->crf_release_accepted&&!s->reported&&!marconi_crf_source(s)) {
  marconi_public_report("MARCONI_CRF_SOURCE_LOST CPU-stop=true no-completion-created=true");
  marconi_crf_reset_devices(s);marconi_rfspim_cancel(s);marconi_owned_runtime_failure_stop(s);return;
 }
 if(!(s->cp_ssi_tx.count||s->cp_rfspim_read_pending))return;
 if(!marconi_rfspim_clock_rate(s,&rate)) {
  bool failure=!s->reported;
  if(failure)marconi_public_report("MARCONI_RFSPIM_SOURCE_LOST pending=%u selected-source-invalid=true incomplete-frames-cancelled=true",s->cp_ssi_tx.count);
  marconi_rfspim_cancel(s);if(failure)marconi_owned_runtime_failure_stop(s);return;
 }
 if(!s->cp_rfspim_progress.active)return;
 int64_t deadline;
 bool changed=s->cp_rfspim_progress.rate_hz!=rate;
 if(!o62m_rfspim_progress_rate_v1(&s->cp_rfspim_progress,now,rate)){
  marconi_rfspim_cancel(s);marconi_owned_runtime_failure_stop(s);return;
 }
 if(changed) {
  ++s->cp_rfspim_retimes;s->cp_rfspim_pauses+=rate==0;
  if(s->cp_ssi_tx.running){++s->cp_ssi_tx.generation;s->cp_rfspim_armed=s->cp_ssi_tx.generation;}
  marconi_public_report("MARCONI_RFSPIM_CLOCK_CHANGE paused=%u retimes=%" PRIu64 " queued-work-preserved=true",rate==0,s->cp_rfspim_retimes);
 }
 if(o62m_rfspim_progress_deadline_v1(&s->cp_rfspim_progress,&deadline)) {
  if(s->cp_ssi_tx.running)s->cp_ssi_tx.deadline=deadline;
  if(s->cp_rfspim_read_running)s->cp_rfspim_read_deadline=deadline;
  timer_mod_ns(s->cp_rfspim_event,deadline);
 } else if(!rate&&s->cp_rfspim_progress.remaining) {
  timer_del(s->cp_rfspim_event);
  if(s->cp_ssi_tx.running)s->cp_ssi_tx.deadline=INT64_MAX;
  if(s->cp_rfspim_read_running)s->cp_rfspim_read_deadline=INT64_MAX;
 } else {marconi_rfspim_cancel(s);marconi_owned_runtime_failure_stop(s);}
}
static void marconi_rfspim_source_changed(MarconiProbeState *s)
{
 marconi_rfspim_source_changed_at(s,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}
static bool marconi_rfspim_start(MarconiProbeState *s,int64_t now)
{
 uint64_t rate;
 if(s->cp_ssi_tx.running||s->cp_rfspim_read_running)return true;
 if(!s->cp_ssi_tx.count&&!s->cp_rfspim_read_pending)return true;
 if(!marconi_rfspim_clock_rate(s,&rate)||!rate)return false;
 uint64_t ns=(UINT64_C(32)*s->cp_ssi_config[0]*1000000000+rate-1)/rate;
 if(now<0||ns>INT64_MAX-(uint64_t)now)return false;
 if(!o62m_rfspim_progress_start_v1(&s->cp_rfspim_progress,now,32*s->cp_ssi_config[0],rate))return false;
 s->cp_rfspim_frame_ns=ns;
 if(!s->cp_ssi_tx.count) {
  s->cp_rfspim_read_running=true;s->cp_rfspim_read_deadline=now+ns;
  timer_mod_ns(s->cp_rfspim_event,s->cp_rfspim_read_deadline);return true;
 }
 if(!o62m_cp_rfspim_start_v2(&s->cp_ssi_tx,now,ns)){o62m_rfspim_progress_reset_v1(&s->cp_rfspim_progress);return false;}
 s->cp_rfspim_armed=s->cp_ssi_tx.generation;timer_mod_ns(s->cp_rfspim_event,s->cp_ssi_tx.deadline);return true;
}
static void marconi_rfspim_complete_at(MarconiProbeState *s,int64_t now)
{
 if(s->cp_rfspim_read_running) {
  if(s->reported){marconi_rfspim_cancel(s);return;}
  marconi_rfspim_source_changed_at(s,now);if(!s->cp_rfspim_read_running)return;
  if(s->cp_rfspim_progress.remaining)return;
  o62m_rfspim_progress_reset_v1(&s->cp_rfspim_progress);
  uint8_t data[2];
  if(address_space_read(&s->crf_as,s->cp_rfspim_read_address,MEMTXATTRS_UNSPECIFIED,data,2)!=MEMTX_OK){
   marconi_rfspim_cancel(s);marconi_owned_runtime_failure_stop(s);return;
  }
  s->cp_rfspim_rx=lduw_le_p(data);memset(data,0,sizeof data);
  s->cp_rfspim_rx_valid=true;s->cp_rfspim_read_pending=s->cp_rfspim_read_running=false;++s->cp_rfspim_read_completed;
  marconi_public_report("MARCONI_RFSPIM_READ_COMPLETE count=%" PRIu64 " current-target-RAM-sampled=true expected-value-not-tested=true data-redacted=true",s->cp_rfspim_read_completed);return;
 }
 if(!s->cp_ssi_tx.running||s->cp_rfspim_armed!=s->cp_ssi_tx.generation)return;
 if(s->reported){marconi_rfspim_cancel(s);return;}
 marconi_rfspim_source_changed_at(s,now);
 if(!s->cp_ssi_tx.running)return;
 if(s->cp_rfspim_progress.remaining)return;
 if(!s->cp_rfspim_emitted||s->cp_rfspim_emitted_count>=MARCONI_RFSPIM_RECORD_WORDS){
  marconi_public_report("MARCONI_RFSPIM_CAPTURE_LIMIT actual-transmission-record-full=true hardware-busy-unmodified=true");
  marconi_rfspim_cancel(s);marconi_owned_runtime_failure_stop(s);return;
 }
 uint32_t frame;
 if(!o62m_cp_rfspim_complete_v2(&s->cp_ssi_tx,now,s->cp_rfspim_armed,&frame)){
  marconi_rfspim_cancel(s);marconi_owned_runtime_failure_stop(s);return;
 }
 o62m_rfspim_progress_reset_v1(&s->cp_rfspim_progress);
 s->cp_rfspim_emitted[s->cp_rfspim_emitted_count++]=frame;
 if(!marconi_crf_receive_write(s,frame)) {
  marconi_public_report("MARCONI_CRF_TARGET_WRITE_BOUNDARY register-address=%04x data-redacted=true",(frame>>15)&0xfffe);
  frame=0;marconi_rfspim_cancel(s);marconi_owned_runtime_failure_stop(s);return;
 }
 frame=0;
 if(s->cp_ssi_tx.completed<=4||s->cp_ssi_tx.completed%4096==0)
  marconi_public_report("MARCONI_RFSPIM_TX_EMITTED frames=%" PRIu64 " pending=%u frame-ns=%" PRIu64 " actual-private-frame-recorded=true RX-created=0 peer-ACK=false",s->cp_ssi_tx.completed,s->cp_ssi_tx.count,s->cp_rfspim_frame_ns);
 if(!marconi_rfspim_start(s,now)){marconi_rfspim_cancel(s);marconi_owned_runtime_failure_stop(s);}
}
/* Advance only work accepted before this observation. Continuous queued
 * frames start at the previous wire deadline, not callback dispatch time. */
static void marconi_rfspim_service_at(MarconiProbeState *s,int64_t now)
{
 unsigned budget=O62M_CP_RFSPIM_TX_CAPACITY_V2+1;
 while(!s->reported&&s->cp_rfspim_progress.active&&budget--) {
  int64_t due;
  if(!o62m_rfspim_progress_deadline_v1(&s->cp_rfspim_progress,&due)||due>now) {
   marconi_rfspim_source_changed_at(s,now);return;
  }
  timer_del(s->cp_rfspim_event);
  marconi_rfspim_complete_at(s,due);
 }
}
static void marconi_rfspim_callback(void *opaque)
{
 MarconiProbeState *s=opaque;
 marconi_rfspim_service_at(s,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}
static bool marconi_rfspim_clock_before_change(MarconiProbeState *s)
{
 if(s->reported)return false;
 marconi_rfspim_service_at(s,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
 return !s->reported;
}
static void marconi_cp_ssi_reset(void *opaque)
{
 MarconiProbeState *s=opaque;marconi_rfspim_cancel(s);
 memset(s->cp_ssi_clock,0,sizeof s->cp_ssi_clock);memset(s->cp_ssi_config,0,sizeof s->cp_ssi_config);s->cp_ssi_writes=0;
 o62m_cp_rfspim_reset_v2(&s->cp_ssi_tx);s->cp_rfspim_divider=0x10100;
 s->cp_xtal26m_profile_enabled=true;s->cp_rfspim_retimes=s->cp_rfspim_pauses=0;
 if(s->cp_rfspim_emitted)o62m_virtual_otp_wipe_v1(s->cp_rfspim_emitted,MARCONI_RFSPIM_RECORD_WORDS*sizeof(uint32_t));
 s->cp_rfspim_emitted_count=0;s->cp_rfspim_clock_generation=s->cp_rfspim_frame_ns=0;
 s->cp_rfspim_read_count=s->cp_rfspim_read_completed=s->cp_rfspim_read_cancelled=0;
}
static void marconi_cp_ssi_exit(Notifier *notifier,void *opaque)
{
 MarconiProbeState *s=container_of(notifier,MarconiProbeState,cp_ssi_exit);(void)opaque;
 uint64_t writes=s->cp_ssi_writes,queued=s->cp_ssi_tx.enqueued,reads=s->cp_ssi_tx.status_reads,emitted=s->cp_ssi_tx.completed,cancelled=s->cp_ssi_tx.cancelled;
 unsigned count=s->cp_ssi_tx.count;
 marconi_public_report("MARCONI_RFSPIM_READ_EXIT requested=%" PRIu64 " completed=%" PRIu64 " cancelled=%" PRIu64 " pending=%u unread=%u private-RX-wiped=true",s->cp_rfspim_read_count,s->cp_rfspim_read_completed,s->cp_rfspim_read_cancelled,s->cp_rfspim_read_pending,s->cp_rfspim_rx_valid);
 marconi_public_report("MARCONI_RFSPIM_CLOCK_EXIT retimes=%" PRIu64 " pauses=%" PRIu64 " remaining-work-preserved-until-complete-or-cancel=true",s->cp_rfspim_retimes,s->cp_rfspim_pauses);
 marconi_cp_ssi_reset(s);s->cp_rfspim_divider=0;s->cp_xtal26m_profile_enabled=false;
 g_free(s->cp_rfspim_emitted);s->cp_rfspim_emitted=NULL;
 marconi_public_report("MARCONI_RFSPIM_EXIT writes=%" PRIu64 " tx-enqueued=%" PRIu64 " tx-pending=%u status-reads=%" PRIu64 " emitted=%" PRIu64 " cancelled=%" PRIu64 " read-ledger-separate=true private-config-FIFO-and-emitted-frames-wiped=true",writes,queued,count,reads,emitted,cancelled);
}
static bool marconi_cp_ssi_access(MarconiProbeState *s,hwaddr address,uint64_t *value,unsigned size,bool write)
{
 if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||(size!=4&&size!=8)||s->reported||
    !s->cp_cold.release_accepted||!marconi_cp_cold_source(s)||(write&&size==4&&*value>UINT32_MAX))return false;
 int64_t access_now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
 marconi_rfspim_service_at(s,access_now);
 if(s->reported)return false;
 uint64_t pc=marconi_mmio_pc();
 if(size==4&&(pc==0x1afb0||pc==0x1afb8)) {
  uint64_t ns;if(write||!marconi_rfspim_source(s,&ns))return false;
  if(pc==0x1afb0&&address==0x443e0028){*value=o62m_cp_rfspim_status_v2(&s->cp_ssi_tx)|(s->cp_rfspim_rx_valid?8:0);return true;}
  if(pc==0x1afb8&&address==0x443e0060&&s->cp_rfspim_rx_valid){*value=s->cp_rfspim_rx;s->cp_rfspim_rx=0;s->cp_rfspim_rx_valid=false;return true;}
  return false;
 }
 if(size==4&&(pc==0x1afac||pc==0x1afae)) {
  uint64_t ns;
  if(!(s->cp_atcm_control&1)||!marconi_rfspim_source(s,&ns)||!write)return false;
  if(pc==0x1afac) {
   if(address!=0x443e0008||*value!=1)return false;
   s->cp_ssi_config[9]=1;++s->cp_ssi_writes;return true;
  }
  if(address!=0x443e0060)return false;
  uint32_t command=(uint32_t)*value;
  if(command==0x58000000&&!s->cp_rfspim_read_pending&&!s->cp_rfspim_rx_valid) {
   s->cp_rfspim_read_address=0xb000;s->cp_rfspim_read_pending=true;++s->cp_rfspim_read_count;
   if(!marconi_rfspim_start(s,access_now)){marconi_rfspim_cancel(s);return false;}
   return true;
  }
  unsigned emitted_match=0,queued_match=0,first_sequence=0,last_sequence=0;
  if(!(command&0x80000000)) {
   for(unsigned i=0;i<s->cp_rfspim_emitted_count;i++) {
    if(((s->cp_rfspim_emitted[i]>>16)&0x7fff)==((command>>16)&0x7fff)) {
     ++emitted_match;if(!first_sequence)first_sequence=i+1;last_sequence=i+1;
    }
   }
   for(unsigned i=0;i<s->cp_ssi_tx.count;i++)
    queued_match+=((s->cp_ssi_tx.word[(s->cp_ssi_tx.head+i)%O62M_CP_RFSPIM_TX_CAPACITY_V2]>>16)&0x7fff)==((command>>16)&0x7fff);
  }
  marconi_public_report("MARCONI_RFSPIM_READ_BOUNDARY write-direction=%u known-d250-command=%u known-d250-caller=%u prior-emitted-same-address=%u queued-same-address=%u request-unaccepted=true RX-created=0 address-and-payload-redacted=true",
   !!(command&0x80000000),command==0x69280000,s->cp_cpu.env.regs[14]==0x641094d3,emitted_match,queued_match);
  uint32_t caller=s->cp_cpu.env.regs[14]&~1u;
  bool rom_caller=caller>=0x64000000u&&caller<0x64200000u;
  bool tcm_caller=caller<0x2e4a8u;
  marconi_public_report("MARCONI_RFSPIM_READ_METADATA register-address=%04x caller-code-matched=%u caller-PC=%08x static-copy-source-PC=%08x prior-emission-first-sequence=%u prior-emission-last-sequence=%u prior-emission-count=%u peer-state-unproven=true payload-redacted=true",
   (command>>15)&0xfffe,rom_caller||tcm_caller,(rom_caller||tcm_caller)?caller:0,
   tcm_caller?0x644147c0u+caller:0,first_sequence,last_sequence,emitted_match);
  command=0;return false;
 }
 if(size==4&&(pc==0x1b004||pc==0x1b006||pc==0x1b01a||pc==0x1b076||pc==0x1b07a||pc==0x1b096)) {
  if(!(s->cp_atcm_control&1)||!(s->cp_ssi_clock[0]&0x20000)||!(s->cp_ssi_clock[1]&0x1000000)||
     s->cp_ssi_config[4]!=0x011f0040||s->cp_ssi_config[0]!=6||s->cp_ssi_config[2]!=0)return false;
  if(pc==0x1b004||pc==0x1b076) {
   if(!write||address!=0x443e0008||*value!=1)return false;
   s->cp_ssi_config[9]=1;++s->cp_ssi_writes;return true;
  }
  if(s->cp_ssi_config[9]!=1)return false;
  marconi_rfspim_source_changed_at(s,access_now);
  if(pc==0x1b006||pc==0x1b07a) {
   if(write||address!=0x443e0028)return false;
   *value=o62m_cp_rfspim_status_v2(&s->cp_ssi_tx);++s->cp_ssi_tx.status_reads;return true;
  }
  if(!write||address!=0x443e0060||s->cp_rfspim_read_pending||s->cp_rfspim_rx_valid)return false;
  if(s->cp_rfspim_emitted_count+s->cp_ssi_tx.count>=MARCONI_RFSPIM_RECORD_WORDS){
   marconi_public_report("MARCONI_RFSPIM_CAPTURE_LIMIT new-frame-unaccepted=true hardware-busy-unmodified=true");return false;
  }
  uint64_t ns;
  if(!marconi_rfspim_source(s,&ns)||!o62m_cp_rfspim_push_v2(&s->cp_ssi_tx,(uint32_t)*value))return false;
  if(!marconi_rfspim_start(s,access_now)){marconi_rfspim_cancel(s);return false;}
  if(s->cp_ssi_tx.enqueued<=4)marconi_public_report("MARCONI_RFSPIM_TX_ENQUEUE count=%u capacity=32 command-direction=write RX-created=0 private-frame-retained=true",s->cp_ssi_tx.count);
  return true;
 }
 if(size==8) {
  if(!write||pc!=0x6400597e||address!=0x443e0018||
     !(s->cp_ssi_clock[0]&0x20000)||!(s->cp_ssi_clock[1]&0x1000000))return false;
  s->cp_ssi_config[7]=(uint32_t)*value;s->cp_ssi_config[8]=(uint32_t)(*value>>32);
  s->cp_ssi_writes+=2;
  marconi_public_report("MARCONI_CP_SSI_CONFIG_PAIR_WRITE writes=%" PRIu64 " original-aligned-STRD=true two-config-words-retained=true no-transfer-or-IRQ-created=true",s->cp_ssi_writes);return true;
 }
 if(address==0x44120074||address==0x441200b8) {
  unsigned i=address==0x44120074?0:1;
  uint32_t bit=i?0x1000000:0x20000;
  if(!write){*value=s->cp_ssi_clock[i];return true;}
  bool gate=pc==UINT64_C(0x641217aa)&&((*value^s->cp_ssi_clock[i])&~(uint64_t)bit)==0;
  bool close=pc==(i?UINT64_C(0x641af500):UINT64_C(0x641af508))&&
             *value==(s->cp_ssi_clock[i]&~bit);
  if(!gate&&!close&&(pc!=(i?0x64005956:0x6400594e)||*value!=(s->cp_ssi_clock[i]|bit)))return false;
  if(!marconi_rfspim_clock_before_change(s))return false;
  s->cp_ssi_clock[i]=(uint32_t)*value;++s->cp_ssi_writes;marconi_rfspim_source_changed(s);return true;
 }
 if(write&&(pc==0x641a68f4||pc==0x641a68fa||pc==0x641a68fc)) {
  unsigned i=pc==0x641a68f4?5:pc==0x641a68fa?9:2;
  uint32_t offset=i==5?0xf8:i==9?8:0x10;
  if(address!=UINT64_C(0x443e0000)+offset||!(s->cp_ssi_clock[0]&0x20000)||
     !(s->cp_ssi_clock[1]&0x1000000)||(i!=5&&*value>1))return false;
  if(s->cp_ssi_tx.count&&*value!=s->cp_ssi_config[i])return false;
  s->cp_ssi_config[i]=(uint32_t)*value;++s->cp_ssi_writes;marconi_rfspim_source_changed(s);
  marconi_public_report("MARCONI_CP_SSI_CONFIG_UPDATE index=%u writes=%" PRIu64 " same-register-owner=true no-transfer-or-IRQ-created=true",i,s->cp_ssi_writes);return true;
 }
 static const uint16_t offsets[10]={0x14,0xc,0x10,4,0,0xf8,0x2c,0x18,0x1c,8};
 static const uint32_t pcs[10]={0x64005960,0x64005964,0x64005964,0x64005968,0x64005970,
  0x64005974,0x6400597a,0x6400597e,0x6400597e,0x64005984};
 if(!write||!(s->cp_ssi_clock[0]&0x20000)||!(s->cp_ssi_clock[1]&0x1000000))return false;
 for(unsigned i=0;i<10;i++)if(address==UINT64_C(0x443e0000)+offsets[i]&&pc==pcs[i]) {
  if((i==0&&((*value&1)||*value<2||*value>65534))||(i==9&&*value>1))return false;
  s->cp_ssi_config[i]=(uint32_t)*value;++s->cp_ssi_writes;marconi_rfspim_source_changed(s);
  marconi_public_report("MARCONI_CP_SSI_CONFIG_WRITE index=%u writes=%" PRIu64 " actual-config-retained=true TX-empty=true no-transfer-or-IRQ-created=true",i,s->cp_ssi_writes);return true;
 }
 return false;
}

static void marconi_dfx_exit(Notifier *notifier,void *opaque)
{
 MarconiProbeState *s=container_of(notifier,MarconiProbeState,dfx_exit);(void)opaque;
 o62m_virtual_otp_wipe_v1(memory_region_get_ram_ptr(&s->dfx_ram),0x800);
 marconi_public_report("MARCONI_DFX_EXIT private-backing-bytes-wiped=2048 guest-aperture-bytes=1536");
}
static void marconi_cp_timer_line(MarconiProbeState *s,bool level)
{
 if(level==s->cp_timer_irq_level)return;
 s->cp_timer_irq_level=level;if(level)++s->cp_timer_irq_asserts;
 qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->cp_cpu),ARM_CPU_IRQ),level);
 if(s->cp_timer_irq_asserts<=8)
  marconi_public_report("MARCONI_CP_TIMER_IRQ level=%u assertions=%" PRIu64 " actual-timer-or-watchdog-source=true",level,s->cp_timer_irq_asserts);
}
static void marconi_cp_timer_stop(MarconiProbeState *s)
{
 if(!s->cp_timer_created)return;
 timer_del(s->cp_timer_event);marconi_cp_timer_line(s,false);
}
static void marconi_cp_timer_reset(void *opaque)
{
 MarconiProbeState *s=opaque;if(!s->cp_timer_created)return;
 marconi_cp_timer_stop(s);
 uint64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
 for(unsigned i=0;i<2;i++) {
  o62m_virtual_otp_wipe_v1(&s->cp_timer[i],sizeof s->cp_timer[i]);
  bool ok=o62m_timer64_create(&s->cp_timer[i],now,0,0,0,0,i?30518:500,
   i?32768:2000000,1,false,O62M_TIMER64_HOLD_FIRST_EXPIRY);assert(ok);
 }
 o62m_cp_cplf_reset_v3(&s->cp_cplf);
 s->cp_timer_reads=s->cp_timer_writes=s->cp_timer_irq_asserts=0;
}
static bool marconi_cp_timer_refresh(MarconiProbeState *s)
{
 if(!s->cp_timer_created)return true;
 if(s->reported){marconi_cp_timer_stop(s);return true;}
 uint64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),next=UINT64_MAX;
 bool source=s->cp_cold.release_accepted&&s->pm_domain1_on&&s->pm_domain1_available&&marconi_cp_cold_source(s);
 for(unsigned i=0;i<2;i++) {
  if(!o62m_timer64_set_clock(&s->cp_timer[i],now,i?32768:2000000,1,
      source&&(i||(s->cp_clock_setup_config[2]&0x20000))))return false;
  s->cp_cplf.input[i]=o62m_timer64_irq(&s->cp_timer[i]);
  uint64_t due=o62m_timer64_next_event_ns(&s->cp_timer[i]);if(due<next)next=due;
 }
 bool enabled=source&&(s->cp_intc_config[11]&1);
 s->cp_cplf.input[2]=!s->cp_wdt.held&&s->cp_wdt.core.irq_pending;
 unsigned pending=o62m_cp_cplf_pending_v3(&s->cp_cplf,enabled,s->cp_intc_config);
 marconi_cp_timer_line(s,pending!=UINT32_MAX);
 if(next<=INT64_MAX)timer_mod_ns(s->cp_timer_event,next);else timer_del(s->cp_timer_event);
 return true;
}
static void marconi_cp_timer_callback(void *opaque)
{
 MarconiProbeState *s=opaque;
 if(!marconi_cp_timer_refresh(s)){marconi_cp_timer_stop(s);marconi_owned_runtime_failure_stop(s);}
}
static void marconi_cp_timer_exit(Notifier *notifier,void *opaque)
{
 MarconiProbeState *s=container_of(notifier,MarconiProbeState,cp_timer_exit);(void)opaque;
 marconi_cp_timer_stop(s);
 marconi_public_report("MARCONI_CP_TIMER_EXIT reads=%" PRIu64 " writes=%" PRIu64 " expirations0=%" PRIu64 " expirations1=%" PRIu64 " irq-assertions=%" PRIu64 " claims=%" PRIu64 " eois=%" PRIu64 " active=%u active-valid=%u timers-cancelled=true private-state-wiped=true",
  s->cp_timer_reads,s->cp_timer_writes,s->cp_timer[0].expirations,s->cp_timer[1].expirations,
  s->cp_timer_irq_asserts,s->cp_cplf.claims,s->cp_cplf.eois,s->cp_cplf.active,s->cp_cplf.active_valid);
 o62m_virtual_otp_wipe_v1(s->cp_timer,sizeof s->cp_timer);o62m_virtual_otp_wipe_v1(&s->cp_cplf,sizeof s->cp_cplf);
 s->cp_timer_created=false;
}
static bool marconi_cp_timer_access(MarconiProbeState *s,hwaddr address,uint64_t *value,unsigned size,bool write)
{
 if(!s->cp_timer_created||current_cpu!=CPU(&s->cp_cpu)||size!=4||s->reported||
    !(s->cp_clock_setup_config[3]&0x20000)||!s->cp_cold.release_accepted||!marconi_cp_cold_source(s)||(write&&*value>UINT32_MAX))return false;
 unsigned instance=address>=0x44310000;uint32_t offset=address-(instance?0x44310000:0x44300000);
 static const struct {uint32_t pc;uint8_t offset;bool write;} access[]={
  {0x29f34,0x1c,false},{0x29f3a,0x1c,true},{0x29f3e,0x10,false},{0x29f44,0x10,true},
  {0x29f48,0x10,false},{0x29f4e,0x10,true},{0x29f52,0x10,false},{0x29f58,0x10,true},
  {0x29f5c,0x10,false},{0x29f62,0x10,true},{0x29f66,8,false},{0x29f6a,12,false},
  {0x29f6e,0,false},{0x29f72,4,false},{0x29f76,0x20,false},{0x29f7e,0x18,false},
  {0x29f84,0x18,true},{0x29f88,0,false},{0x29f8a,0,true},{0x29f8e,4,false},{0x29f90,4,true},
  {0x29f94,0x10,false},{0x29f9a,0x10,true},{0x29f9e,0x10,false},{0x29fa4,0x10,true}};
 bool match=false;uint64_t pc=marconi_mmio_pc();
 for(unsigned i=0;i<ARRAY_SIZE(access);i++)if(pc==access[i].pc&&offset==access[i].offset&&write==access[i].write)match=true;
 if(!match||!marconi_cp_timer_refresh(s))return false;
 O62MTimer64 trial=s->cp_timer[instance];uint32_t result=0;uint64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
 bool ok=write?o62m_timer64_write(&trial,now,offset,*value):o62m_timer64_read(&trial,now,offset,&result);
 if(!ok)return false;
 s->cp_timer[instance]=trial;
 if(write)++s->cp_timer_writes;else{++s->cp_timer_reads;*value=result;}
 if(s->cp_timer_reads+s->cp_timer_writes<=12)
  marconi_public_report("MARCONI_CP_TIMER_ACCESS instance=%u offset=%02x write=%u actual-independent-timer=true",instance,offset,write);
 return marconi_cp_timer_refresh(s);
}
static void marconi_cp_intc_config_reset(void *opaque)
{
 MarconiProbeState *s=opaque;
 o62m_virtual_otp_wipe_v1(s->cp_intc_config,sizeof s->cp_intc_config);s->cp_intc_config_writes=s->cp_intc_config_reads=0;
}
static void marconi_cp_intc_config_exit(Notifier *notifier,void *opaque)
{
 MarconiProbeState *s=container_of(notifier,MarconiProbeState,cp_intc_config_exit);(void)opaque;
 uint64_t writes=s->cp_intc_config_writes,reads=s->cp_intc_config_reads;marconi_cp_intc_config_reset(s);
 marconi_public_report("MARCONI_CP_INTC_CONFIG_EXIT writes=%" PRIu64 " reads=%" PRIu64 " private-config-wiped=true routing-scope=0,44,45",writes,reads);
}
static bool marconi_cp_intc_config_access(MarconiProbeState *s,hwaddr address,uint64_t *value,unsigned size,bool write)
{
 if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||size!=4||s->reported||
    !s->cp_cold.release_accepted||!marconi_cp_cold_source(s)||(write&&*value>UINT32_MAX))return false;
 uint64_t pc=marconi_mmio_pc();unsigned i;
 bool route=address>=0x443f00a0&&address<=0x443f00b4&&(address&3)==0;
 if(route||address==0x443f0180||address==0x443f0080||address==0x443f0084) {
  if(!marconi_cp_timer_refresh(s))return false;
  if(route) {
   unsigned index=(address-0x443f00a0)/4;
   if(write) {if((pc!=0x641a83ce&&pc!=0x641aa502)||!o62m_cp_cplf_route_v3(&s->cp_cplf,index,*value))return false;}
   else {if(pc!=0x641a83ba&&pc!=0x641aa4fc)return false;*value=s->cp_cplf.route[index];}
  } else if(address==0x443f0180) {
   if(write||pc!=0x27edc)return false;
   *value=o62m_cp_cplf_claim_v3(&s->cp_cplf,s->cp_intc_config[11]&1,s->cp_intc_config);
   if(s->cp_cplf.claims<=8||*value==0)marconi_public_report("MARCONI_CP_INTC_CLAIM irq=%" PRIu64 " claims=%" PRIu64,*value,s->cp_cplf.claims);
  } else {
   if(!write||pc!=0x27f04||!o62m_cp_cplf_eoi_v3(&s->cp_cplf,(address-0x443f0080)/4,*value))return false;
   if(address==0x443f0080)marconi_public_report("MARCONI_CP_WDT_INTC_EOI actual-active-IRQ0-ended=true peripheral-IRQ-not-cleared-by-EOI=true");
  }
  return marconi_cp_timer_refresh(s);
 }
 bool priority=address>=0x443f00e0&&address<=0x443f0108&&(address&3)==0;
 if(priority) {
  i=(address-0x443f00e0)/4;
  if(!write) {*value=s->cp_intc_config[i];++s->cp_intc_config_reads;return true;}
  if(!(pc==0x640013c8&&*value==0x00924924)) {
   if(pc!=0x641f65f4)return false;
   unsigned fields=i==10?4:8;uint32_t mask=i==10?0xfffu:0xffffffu;
   if(((uint32_t)*value^s->cp_intc_config[i])&~mask)return false;
   for(unsigned n=0;n<fields;n++)if(!(0xb3u&(1u<<((*value>>(3*n))&7))))return false;
  }
 } else if(write&&address==0x443f0188&&pc==0x640013c2&&*value==1)i=11;
 else return false;
 s->cp_intc_config[i]=(uint32_t)*value;++s->cp_intc_config_writes;
 if(s->cp_intc_config_writes==1||s->cp_intc_config_writes==12)
  marconi_public_report("MARCONI_CP_INTC_CONFIG writes=%" PRIu64 " actual-config-retained=true routing-scope=0,44,45",s->cp_intc_config_writes);
 return marconi_cp_timer_refresh(s);
}
static const uint16_t marconi_cp_clock_gate_offsets[31]={
 0x04,0x88,0x8c,0x08,0x1c,0x20,0x24,0x58,0x64,0x68,0x6c,0x70,0x80,0x84,0x94,0x98,
 0x9c,0xac,0xb0,0xb4,0xc0,0xc4,0xc8,0xcc,0xe0,0xd8,0xe8,0xec,0xf4,0xf8,0x100
};
static const uint16_t marconi_cp_clock_init_offsets[16]={
 0x14,0x30,0x34,0x0c,0x10,0x38,0x3c,0x40,0x44,0x48,0x4c,0x50,0x54,0xdc,0xf0,0x28
};
static void marconi_cp_clock_init_reset(void *opaque)
{
 MarconiProbeState *s=opaque;memset(s->cp_clock_init,0,sizeof s->cp_clock_init);s->cp_clock_init_count=0;
 o62m_virtual_otp_wipe_v1(s->cp_clock_setup_config,sizeof s->cp_clock_setup_config);
 o62m_virtual_otp_wipe_v1(s->cp_clock_gate_config,sizeof s->cp_clock_gate_config);s->cp_clock_setup_reads=s->cp_clock_setup_writes=0;
 s->cp_wifi_control=0;s->cp_wifi_control_reads=s->cp_wifi_control_writes=0;
 s->ap_board_words[11]=s->ap_board_words[12]=0;
 s->cp_pm_bookkeeping_reads=s->cp_pm_bookkeeping_writes=0;
 s->cp_pm_progress=0;s->cp_pm_progress_reads=s->cp_pm_progress_writes=0;
 s->cp_pm_runtime_request=s->cp_pm_runtime_config=0;s->cp_pm_runtime_control_writes=0;
 s->cp_pm_events=s->cp_pm_local_events=0;memset(s->cp_pm_init_config,0,sizeof s->cp_pm_init_config);
 s->cp_pm_event_reads=s->cp_pm_event_clears=s->cp_pm_init_writes=0;
 o62m_virtual_otp_wipe_v1(s->cp_busmon_config,sizeof s->cp_busmon_config);
 s->cp_busmon_reads=s->cp_busmon_writes=0;s->cp_busmon_configured=0;
 o62m_virtual_otp_wipe_v1(&s->cp_dvfs_limit_word,sizeof s->cp_dvfs_limit_word);s->cp_dvfs_limit_writes=0;
}
static void marconi_cp_clock_init_exit(Notifier *notifier,void *opaque)
{
 MarconiProbeState *s=container_of(notifier,MarconiProbeState,cp_clock_init_exit);(void)opaque;
 uint64_t limit_writes=s->cp_dvfs_limit_writes;
 marconi_public_report("MARCONI_CP_PM_PROGRESS_EXIT reads=%" PRIu64 " writes=%" PRIu64 " actual-software-word=true no-power-transition-generated=true",s->cp_pm_progress_reads,s->cp_pm_progress_writes);
 marconi_public_report("MARCONI_CP_BUSMON_EXIT reads=%" PRIu64 " writes=%" PRIu64 " configured-mask=%u control-bit0-clear=true no-statistics-fault-or-IRQ-generated=true",s->cp_busmon_reads,s->cp_busmon_writes,s->cp_busmon_configured);
 marconi_public_report("MARCONI_CP_PM_EVENT_EXIT reads=%" PRIu64 " clears=%" PRIu64 " config-writes=%" PRIu64 " reset-epoch-latches=true no-event-generated=true",s->cp_pm_event_reads,s->cp_pm_event_clears,s->cp_pm_init_writes);
 marconi_public_report("MARCONI_CP_PM_RUNTIME_CONTROL_EXIT writes=%" PRIu64 " cold-job-not-replayed=true event-effects-unimplemented=true no-status-or-IRQ-generated=true",s->cp_pm_runtime_control_writes);
 marconi_public_report("MARCONI_CP_PM_BOOKKEEPING_EXIT reads=%" PRIu64 " writes=%" PRIu64 " software-calculated-retained-words=true existing-counter-unchanged=true",s->cp_pm_bookkeeping_reads,s->cp_pm_bookkeeping_writes);
 marconi_public_report("MARCONI_CP_WIFI_CONTROL_EXIT reads=%" PRIu64 " writes=%" PRIu64 " retained-config-only=true no-scan-or-peer-result=true",s->cp_wifi_control_reads,s->cp_wifi_control_writes);
 marconi_public_report("MARCONI_CP_CLOCK_SETUP_EXIT reads=%" PRIu64 " writes=%" PRIu64 " actual-config-only=true private-config-wiped=true",s->cp_clock_setup_reads,s->cp_clock_setup_writes);
 unsigned count=s->cp_clock_init_count;marconi_cp_clock_init_reset(s);
 marconi_public_report("MARCONI_CP_DVFS_LIMIT_EXIT actual-config-writes=%" PRIu64 " config-wiped=true",limit_writes);
 marconi_public_report("MARCONI_CP_CLOCK_INIT_EXIT actual-config-writes=%u private-config-wiped=true no-rate-lock-or-ready-inferred=true",count);
}
static bool marconi_cp_pm_progress_access(MarconiProbeState *s,uint64_t *value,unsigned size,bool write)
{
 if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||size!=4||s->reported||
    !s->cp_cold.release_accepted||!marconi_cp_cold_source(s)||(write&&*value>UINT32_MAX))return false;
 if(write) {s->cp_pm_progress=(uint32_t)*value;++s->cp_pm_progress_writes;}
 else {*value=s->cp_pm_progress;++s->cp_pm_progress_reads;}
 if(s->cp_pm_progress_reads+s->cp_pm_progress_writes<=8)
  marconi_public_report("MARCONI_CP_PM_PROGRESS write=%u reads=%" PRIu64 " writes=%" PRIu64
      " actual-software-word=true no-power-transition-generated=true",write,s->cp_pm_progress_reads,s->cp_pm_progress_writes);
 return true;
}
static int marconi_cp_busmon_index(hwaddr address)
{
 static const uint32_t bases[9]={0x4a830000,0x4a840000,0x4a850000,0x4a860000,0x4a870000,0x4a930000,0x4a940000,0x4a950000,0x4a960000};
 for(unsigned i=0;i<9;i++)if(address>=bases[i]&&address-bases[i]<0x1000)return i;
 return -1;
}
static bool marconi_cp_busmon_access(MarconiProbeState*s,hwaddr address,uint64_t*value,unsigned size,bool write)
{
 int instance=marconi_cp_busmon_index(address);
 if(instance<0||!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||size!=4||s->reported||
    !s->cp_cold.release_accepted||!marconi_cp_cold_source(s)||(write&&*value>UINT32_MAX))return false;
 static const uint16_t offsets[7]={0x100,0x110,0x124,0x128,0x11c,0x120,0xc};
 unsigned index=7;for(unsigned i=0;i<7;i++)if((address&0xffff)==offsets[i])index=i;
 if(index==7)return false;
 uint64_t pc=marconi_mmio_pc();uint32_t old=s->cp_busmon_config[instance][index];
 if(!write) {
  bool valid=index==0?(pc==0x641a8032||pc==0x641a8024):index==1?pc==0x6412f24a:index==6&&pc==0x641a803e;
  if(!valid)return false;
  *value=old;++s->cp_busmon_reads;return true;
 }
 bool valid=false;
 if(index==0)valid=(pc==0x641a8038&&*value==old)||(pc==0x641a802c&&*value==(old&~1u));
 else if(index==1)valid=pc==0x6412f250&&(*value==(old|4u)||*value==(old|0x40u));
 else if(index==6)valid=pc==0x641a8042&&*value==(old|8u);
 else {
  static const uint32_t pcs[4]={0x64128480,0x64128484,0x640243b8,0x640243bc};
  valid=pc==pcs[index-2];
 }
 if(!valid)return false;
 s->cp_busmon_config[instance][index]=(uint32_t)*value;++s->cp_busmon_writes;
 if(index==6) {
  s->cp_busmon_configured|=1u<<instance;
  marconi_public_report("MARCONI_CP_BUSMON_CONFIG instance=%d actual-filter-config-written=true control-bit0-clear=true statistics-and-fault-delivery-unimplemented=true",instance);
 }
 return true;
}

static const uint32_t marconi_cp_pm_init_addresses[26]={0x44110048,0x44110054,0x44110058,0x4411004c,0x44110074,0x441100bc,0x441100c0,0x441100c4,0x441100c8,0x44110068,0x44110064,0x4411006c,0x44110070,0x4411005c,0x44110060,0x44110050,0x441100b0,0x441100b4,0x441100b8,0x4413016c,0x44120124,0x441100a4,0x441100ac,0x441100f4,0x4411013c,0x4c03008c};
static bool marconi_cp_pm_init_port(hwaddr address)
{
 if(address==0x4c03038c||address==0x4c03068c||address==0x4c0302bc||address==0x4c0302b8||address==0x4c0300e8||address==0x4c0300ec)return true;
 for(unsigned i=0;i<26;i++)if(address==marconi_cp_pm_init_addresses[i])return true;
 return false;
}
static bool marconi_cp_pm_init_access(MarconiProbeState*s,hwaddr address,uint64_t*value,unsigned size,bool write)
{
 if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||size!=4||s->reported||
    !s->cp_cold.release_accepted||!marconi_cp_cold_source(s)||(write&&*value>UINT32_MAX))return false;
 uint64_t pc=marconi_mmio_pc();
 if(address==0x4c0302bc||address==0x4c0300e8) {
  if(write||(address==0x4c0302bc?(pc!=0x641a4112&&pc!=0x641a40de):pc!=0x641a4764))return false;
  *value=address==0x4c0302bc?s->cp_pm_events:s->cp_pm_local_events|(s->cp_cold.phase==1?1u:0u);
  ++s->cp_pm_event_reads;return true;
 }
 if(address==0x4c0302b8||address==0x4c0300ec) {
  if(!write)return false;
  if(address==0x4c0302b8) {
   if(pc!=0x641a40dc||!*value||(*value&~UINT64_C(0x1f9))||(*value&(*value-1)))return false;
   s->cp_pm_events&=~(uint32_t)*value;
  } else {
   if(pc!=0x641a476e||*value!=4)return false;
   s->cp_pm_local_events&=~UINT32_C(4);
  }
  ++s->cp_pm_event_clears;return true;
 }
 if(address==0x4c03038c||address==0x4c03068c) {
  if(!write||pc!=0x641219e6||(*value!=1&&*value!=2))return false;
  if(address==0x4c03038c)s->cp_pm_init_config[25]|=(uint32_t)*value;
  else s->cp_pm_init_config[25]&=~(uint32_t)*value;
  ++s->cp_pm_init_writes;return true;
 }
 unsigned index=26;for(unsigned i=0;i<26;i++)if(address==marconi_cp_pm_init_addresses[i])index=i;
 if(index==26)return false;
 if(!write){*value=s->cp_pm_init_config[index];return true;}
 static const struct {unsigned index;uint32_t pc,clear,set;} writes[]={
{0,0x641a462a,0x0,0x100},
{1,0x641a4632,0x0,0x100},
{2,0x641a463a,0x0,0x100},
{3,0x641a4642,0x0,0x100},
{4,0x641a464a,0x0,0x100},
{5,0x641a4652,0x0,0x100},
{6,0x641a465a,0x0,0x100},
{7,0x641a4662,0x0,0x100},
{8,0x641a466c,0x0,0x100},
{9,0x641a4676,0x0,0x100},
{10,0x641a467e,0x0,0x100},
{11,0x641a4686,0x0,0x100},
{12,0x641a468e,0x0,0x100},
{13,0x641a4696,0x0,0x100},
{14,0x641a469e,0x0,0x100},
{15,0x641a46a6,0x0,0x100},
{16,0x641a46ae,0x0,0x100},
{17,0x641a46b6,0x0,0x100},
{18,0x641a46be,0x0,0x100},
{19,0x641a46ce,0x0,0x200},
{20,0x641a46e0,0x20000,0x0},
{20,0x641a46ec,0x10,0x0},
{20,0x641a46f8,0x400,0x0},
{20,0x641a4704,0x8000,0x0},
{21,0x641a4732,0xffffffff,0x1e019e},
{22,0x641a474a,0x0,0x1},
{23,0x640ff34c,0xffffffff,0x1},
{24,0x640ff34a,0xffffffff,0x0}};
 for(unsigned i=0;i<ARRAY_SIZE(writes);i++)if(index==writes[i].index&&pc==writes[i].pc&&
    *value==((s->cp_pm_init_config[index]&~writes[i].clear)|writes[i].set)) {
  s->cp_pm_init_config[index]=(uint32_t)*value;++s->cp_pm_init_writes;return true;
 }
 return false;
}

static bool marconi_cp_clock_init_access(MarconiProbeState *s,hwaddr address,uint64_t *value,unsigned size,bool write)
{
 if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||size!=4||s->reported||
    !s->cp_cold.release_accepted||!marconi_cp_cold_source(s))return false;
 if(address==UINT64_C(0x44110098)) {
  uint64_t pc=marconi_mmio_pc();
  if(!write) {
   if(pc!=UINT64_C(0x6419565e)&&pc!=UINT64_C(0x64195666))return false;
   *value=s->cp_wifi_control;++s->cp_wifi_control_reads;return true;
  }
  bool set=pc==UINT64_C(0x64195664)&&*value==(s->cp_wifi_control|UINT32_C(2));
  bool clear=pc==UINT64_C(0x6419566c)&&*value==(s->cp_wifi_control&~UINT32_C(1));
  if(!set&&!clear)return false;
  s->cp_wifi_control=(uint32_t)*value;++s->cp_wifi_control_writes;return true;
 }
 for(unsigned i=0;i<ARRAY_SIZE(marconi_cp_clock_gate_offsets);i++)
  if(address==UINT64_C(0x44120000)+marconi_cp_clock_gate_offsets[i]) {
   if(!write){*value=s->cp_clock_gate_config[i];++s->cp_clock_setup_reads;return true;}
   unsigned offset=marconi_cp_clock_gate_offsets[i];
   uint32_t mask=UINT32_C(1)<<((offset==0x9c||offset==0xb0||offset==0xd8)?24:17);
   bool eac_stop=offset==0xb0&&marconi_mmio_pc()==UINT64_C(0x1abc4)&&
       *value==(s->cp_clock_gate_config[i]&~UINT32_C(0x5000000));
   bool gtimer_stop=((offset==0xac&&marconi_mmio_pc()==UINT64_C(0x6410954e))||
                     (offset==0x8c&&marconi_mmio_pc()==UINT64_C(0x64109556)))&&
       *value==(s->cp_clock_gate_config[i]&~UINT32_C(0x20000));
   bool wifi_stop=((offset==0x9c&&marconi_mmio_pc()==UINT64_C(0x64195646)&&
                    *value==(s->cp_clock_gate_config[i]&~UINT32_C(0x5000000)))||
                   (offset==0xc0&&marconi_mmio_pc()==UINT64_C(0x6419564e)&&
                    *value==(s->cp_clock_gate_config[i]&~UINT32_C(0x220000))));
   bool pm_config=((offset==0xf4&&marconi_mmio_pc()==UINT64_C(0x641a4710))||
                   (offset==0x20&&marconi_mmio_pc()==UINT64_C(0x641a471a))||
                   (offset==0x24&&marconi_mmio_pc()==UINT64_C(0x641a4722)))&&
       *value==(s->cp_clock_gate_config[i]&~UINT32_C(0x200000));
   if(!pm_config&&!eac_stop&&!gtimer_stop&&!wifi_stop&&(marconi_mmio_pc()!=UINT64_C(0x641217aa)||*value>UINT32_MAX||
      ((*value^s->cp_clock_gate_config[i])&~(uint64_t)mask)))return false;
   if(offset==0x88&&!marconi_cp_wdt_refresh(s))return false;
   s->cp_clock_gate_config[i]=(uint32_t)*value;++s->cp_clock_setup_writes;
   return offset!=0x88||marconi_cp_wdt_refresh(s);
  }
 static const uint16_t divider_offsets[6]={0x18,0x7c,0xa0,0xa8,0xbc,0xfc};
 for(unsigned i=0;i<ARRAY_SIZE(divider_offsets);i++)if(address==UINT64_C(0x44120000)+divider_offsets[i]) {
  uint32_t *word=i==3?&s->cp_rfspim_divider:&s->cp_clock_setup_config[6+i-(i>3)];
  if(!write){*value=*word;++s->cp_clock_setup_reads;return true;}
  bool wifi_stop=address==UINT64_C(0x441200a0)&&marconi_mmio_pc()==UINT64_C(0x64195656)&&
      *value==(*word&~UINT32_C(0x110000));
  if(!wifi_stop&&(marconi_mmio_pc()!=UINT64_C(0x641217aa)||*value>UINT32_MAX||
     ((*value^*word)&~UINT64_C(0x10000))))return false;
  if(i==3&&!marconi_rfspim_clock_before_change(s))return false;
  *word=(uint32_t)*value;++s->cp_clock_setup_writes;
  if(i==3)marconi_rfspim_source_changed(s);
  marconi_public_report("MARCONI_CP_DIVIDER_GATE_CONFIG index=%u actual-canonical-write=true",i);return true;
 }
 if(address==0x441200d0||address==0x441200d4) {
  unsigned i=address==0x441200d0?4:5;
  if(!write){*value=s->cp_clock_setup_config[i];++s->cp_clock_setup_reads;return true;}
  if(marconi_mmio_pc()!=UINT64_C(0x641217aa)||*value>UINT32_MAX||
     ((*value^s->cp_clock_setup_config[i])&~UINT64_C(0x20000)))return false;
  s->cp_clock_setup_config[i]=(uint32_t)*value;++s->cp_clock_setup_writes;
  marconi_public_report("MARCONI_CP_MUX_GATE_CONFIG index=%u actual-config-retained=true functional-work-unmodeled=true",i-4);return true;
 }
 for(unsigned i=0;i<s->cp_clock_init_count;i++)
  if(address==UINT64_C(0x44120000)+marconi_cp_clock_init_offsets[i]) {
   if(!write){*value=s->cp_clock_init[i];return true;}
   if(marconi_mmio_pc()!=UINT64_C(0x641217aa)||*value>UINT32_MAX||
      ((*value^s->cp_clock_init[i])&~UINT64_C(0x20000)))return false;
   s->cp_clock_init[i]=(uint32_t)*value;++s->cp_clock_setup_writes;return true;
  }
 if(address==0x44120090||address==0x4412005c||address==0x44120078||address==0x44120060) {
  if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||size!=4||s->reported||
     !s->cp_cold.release_accepted||!marconi_cp_cold_source(s))return false;
  unsigned i=address==0x44120090?0:address==0x4412005c?1:address==0x44120078?2:3;
  static const uint32_t write_pc[4]={0x6415af1a,0x6415af1a,0x6415ad9c,0x6415adaa};
  uint64_t pc=marconi_mmio_pc();
  if(!write){*value=s->cp_clock_setup_config[i];++s->cp_clock_setup_reads;return true;}
  uint32_t expected=i<2?s->cp_clock_setup_config[i]&~UINT32_C(0x20000):s->cp_clock_setup_config[i]|UINT32_C(0x20000);
  bool gate=pc==UINT64_C(0x641217aa)&&*value<=UINT32_MAX&&
      ((*value^s->cp_clock_setup_config[i])&~UINT64_C(0x20000))==0;
  if(!gate&&(pc!=write_pc[i]||*value!=expected))return false;
  s->cp_clock_setup_config[i]=(uint32_t)*value;++s->cp_clock_setup_writes;
  if(!marconi_cp_timer_refresh(s))return false;
  marconi_public_report("MARCONI_CP_CLOCK_SETUP_WRITE index=%u actual-config-only=true",i);return true;
 }

 if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||!write||size!=4||*value>UINT32_MAX||
    !s->cp_cold.release_accepted||!marconi_cp_cold_source(s)||s->reported||
    s->cp_clock_init_count>=ARRAY_SIZE(marconi_cp_clock_init_offsets)||
    marconi_mmio_pc()!=UINT64_C(0x641f5e5e))return false;
 unsigned index=s->cp_clock_init_count;
 if(address!=UINT64_C(0x44120000)+marconi_cp_clock_init_offsets[index])return false;
 s->cp_clock_init[index]=(uint32_t)*value;++s->cp_clock_init_count;
 marconi_public_report("MARCONI_CP_CLOCK_INIT_WRITE index=%u actual-word-retained=true no-rate-lock-or-ready-inferred=true",index);return true;
}
static void marconi_cp_dfs_request_reset(void *opaque)
{
 MarconiProbeState *s=opaque;memset(s->cp_dfs_request,0,sizeof s->cp_dfs_request);s->cp_dfs_request_written=0;
}
static void marconi_cp_dfs_request_exit(Notifier *notifier,void *opaque)
{
 MarconiProbeState *s=container_of(notifier,MarconiProbeState,cp_dfs_request_exit);(void)opaque;
 unsigned written=s->cp_dfs_request_written;marconi_cp_dfs_request_reset(s);
 marconi_public_report("MARCONI_CP_DFS_REQUEST_EXIT written-domain-mask=%u requests-unconsumed=true private-records-wiped=true",written);
}
static bool marconi_cp_dfs_request_access(MarconiProbeState *s,hwaddr address,uint64_t *value,unsigned size,bool write)
{
 if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||!write||size!=4||*value>UINT32_MAX||
    !s->cp_cold.release_accepted||!marconi_cp_cold_source(s)||s->reported||
    !s->dfs_config.enabled||s->dfs_config.phase!=16)return false;
 unsigned domain=address==0x4a8d0028?DFS_CONFIG_PUBAP:DFS_CONFIG_PUBCP;
 if((s->cp_dfs_request_written&(1u<<domain))||
    marconi_mmio_pc()!=(domain==DFS_CONFIG_PUBAP?UINT64_C(0x641fbe62):UINT64_C(0x641fbe6a)))return false;
 s->cp_dfs_request[domain]=(uint32_t)*value;s->cp_dfs_request_written|=1u<<domain;
 marconi_public_report("MARCONI_CP_DFS_REQUEST domain=%u actual-word-preserved=true request-unconsumed=true prior-config-and-quiesce-unchanged=true",domain);return true;
}
static void marconi_spmi_irq_reset(void *opaque)
{
 MarconiProbeState *s=opaque;s->spmi_irq_pending=0;
 s->spmi_irq_reads=s->spmi_irq_clears=0;
}
static void marconi_spmi_irq_exit(Notifier *notifier,void *opaque)
{
 MarconiProbeState *s=container_of(notifier,MarconiProbeState,spmi_irq_exit);(void)opaque;
 uint64_t reads=s->spmi_irq_reads,clears=s->spmi_irq_clears;
 marconi_spmi_irq_reset(s);
 marconi_public_report("MARCONI_SPMI_INTERRUPT_EXIT reads=%" PRIu64 " clears=%" PRIu64 " shared-status-wiped=true bus-transaction-state-independent=true",reads,clears);
}
static void marconi_cp_eac_reset(void *opaque)
{
 MarconiProbeState *s=opaque;
 o62m_virtual_otp_wipe_v1(&s->cp_eac,sizeof s->cp_eac);
}
static void marconi_cp_eac_exit(Notifier *notifier,void *opaque)
{
 MarconiProbeState *s=container_of(notifier,MarconiProbeState,cp_eac_exit);(void)opaque;
 uint64_t requests=s->cp_eac.requests,reads=s->cp_eac.reads,resets=s->cp_eac.reset_requests;
 marconi_cp_eac_reset(s);
 marconi_public_report("MARCONI_CP_EAC_EXIT captures=%" PRIu64 " reads=%" PRIu64 " reset-requests=%" PRIu64
    " reset-disabled-profile=true running-counter-supported=false state-wiped=true",requests,reads,resets);
}
static bool marconi_cp_eac_access(MarconiProbeState *s,hwaddr address,uint64_t *value,unsigned size,bool write)
{
 if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||s->reported||
    !s->cp_cold.release_accepted||!marconi_cp_cold_source(s))return false;
 uint64_t pc=marconi_mmio_pc();
 if((s->cp_clock_gate_config[18]&UINT32_C(0x1000000))||
    (s->cp_eac.local_enable[0]&2)||(s->cp_eac.local_enable[1]&2)) {
  marconi_public_report("MARCONI_CP_EAC_ENABLED_BOUNDARY RFC-gate-on=%u local-enable-on=%u running-counter-unimplemented=true",
    !!(s->cp_clock_gate_config[18]&UINT32_C(0x1000000)),!!((s->cp_eac.local_enable[0]|s->cp_eac.local_enable[1])&2));
  return false;
 }
 if(address==UINT64_C(0x44110039)) {
  if(write||size!=1||pc!=UINT64_C(0x1ab40))return false;
  *value=(s->cp_wdt.control>>8)&255;return true;
 }
 if(address==UINT64_C(0x44110438)||address==UINT64_C(0x44110238)) {
  bool assert_reset=address==UINT64_C(0x44110438);
  if(!write||size!=4||*value!=UINT32_C(0x200)||pc!=(assert_reset?UINT64_C(0x1ab3e):UINT64_C(0x1ab46)))return false;
  if(assert_reset) {
   s->cp_wdt.control&=~UINT32_C(0x200);
   memset(s->cp_eac.counter,0,sizeof s->cp_eac.counter);
   memset(s->cp_eac.capture,0,sizeof s->cp_eac.capture);
   s->cp_eac.control=0;s->cp_eac.valid=0;
   memset(s->cp_eac.local_enable,0,sizeof s->cp_eac.local_enable);
  } else s->cp_wdt.control|=UINT32_C(0x200);
  ++s->cp_eac.reset_requests;return true;
 }
 if(size!=4||(write&&*value>UINT32_MAX))return false;
 if(address==UINT64_C(0x44130154)) {
  if(!write) {if(pc!=UINT64_C(0x1aaee))return false;*value=s->cp_eac.control;return true;}
  if(pc!=UINT64_C(0x1aafe)||(*value!=(s->cp_eac.control|1)&&*value!=(s->cp_eac.control|2)))return false;
  s->cp_eac.control=(uint32_t)*value;
  for(unsigned i=0;i<2;i++)if(*value&(1u<<i)) {
   s->cp_eac.capture[i]=s->cp_eac.counter[i];s->cp_eac.valid|=1u<<i;
  }
  ++s->cp_eac.requests;
  marconi_public_report("MARCONI_CP_EAC_CAPTURE requests=%" PRIu64 " actual-request=true RFC-gate-off=true local-enables-off=true reset-counter-copied=true",s->cp_eac.requests);
  return true;
 }
 if(address==UINT64_C(0x44130160)||address==UINT64_C(0x44130164)) {
  unsigned i=address==UINT64_C(0x44130160)?0:1;
  bool caller=i?(pc==UINT64_C(0x1bcfe)||pc==UINT64_C(0x1bd74)):
                 (pc==UINT64_C(0x1bccc)||pc==UINT64_C(0x1bdc4));
  if(write||!caller||!(s->cp_eac.valid&(1u<<i)))return false;
  *value=s->cp_eac.capture[i];++s->cp_eac.reads;return true;
 }
 if(address==UINT64_C(0x4413000c)||address==UINT64_C(0x44130070)) {
  unsigned i=address==UINT64_C(0x4413000c)?0:1;
  if(pc!=(write?(i?UINT64_C(0x1abd4):UINT64_C(0x1abce)):
                (i?UINT64_C(0x1abc8):UINT64_C(0x1abc6))))return false;
  if(write) {if(*value!=(s->cp_eac.local_enable[i]&~UINT32_C(2)))return false;s->cp_eac.local_enable[i]=(uint32_t)*value;}
  else *value=s->cp_eac.local_enable[i];
  return true;
 }
 return false;
}
static void marconi_cp_sim_config_reset(void *opaque)
{
 MarconiProbeState *s=opaque;
 o62m_virtual_otp_wipe_v1(&s->cp_sim_config,sizeof s->cp_sim_config);
}
static void marconi_cp_sim_config_exit(Notifier *notifier,void *opaque)
{
 MarconiProbeState *s=container_of(notifier,MarconiProbeState,cp_sim_config_exit);(void)opaque;
 uint64_t reads=s->cp_sim_config.reads,writes=s->cp_sim_config.writes,requests=s->cp_sim_config.control_requests;
 uint64_t rx_reads=s->cp_sim_config.rx_count_reads;
 uint64_t assertions=s->cp_sim_config.reset_assertions,releases=s->cp_sim_config.reset_releases;
 uint64_t event_reads=s->cp_sim_config.event_reads,event_clears=s->cp_sim_config.event_clears;
 marconi_cp_sim_config_reset(s);
 marconi_public_report("MARCONI_CP_SIM_CONFIG_EXIT reads=%" PRIu64 " writes=%" PRIu64 " control-requests=%" PRIu64
     " rx-count-reads=%" PRIu64 " reset-assertions=%" PRIu64 " reset-releases=%" PRIu64
     " event-reads=%" PRIu64 " event-clears=%" PRIu64
     " transfers-admitted=0 card-responses=0 private-state-wiped=true",reads,writes,requests,rx_reads,assertions,releases,event_reads,event_clears);
}
static bool marconi_cp_sim_reset_access(MarconiProbeState *s,uint64_t *value,unsigned size,bool write)
{
 if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||size!=4||s->reported||
    !s->cp_cold.release_accepted||!marconi_cp_cold_source(s))return false;
 uint64_t pc=marconi_mmio_pc();uint32_t old=s->cp_wdt.control;
 if(!write) {
  if(pc!=UINT64_C(0x640f881c))return false;
  *value=old;return true;
 }
 if(pc==UINT64_C(0x640f8836)) {
  if(*value!=(old&~UINT32_C(0x10)))return false;
  o62m_virtual_otp_wipe_v1(s->cp_sim_config.word,sizeof s->cp_sim_config.word);
  s->cp_sim_config.rx_count=0;s->cp_sim_config.events=0;
  s->cp_sim_config.reset_held=true;++s->cp_sim_config.reset_assertions;
 } else if(pc==UINT64_C(0x640f883c)) {
  if(!s->cp_sim_config.reset_held||*value!=(old|UINT32_C(0x10)))return false;
  s->cp_sim_config.reset_held=false;++s->cp_sim_config.reset_releases;
 } else return false;
 s->cp_wdt.control=(uint32_t)*value;
 marconi_public_report("MARCONI_CP_SIM_RESET held=%u assertions=%" PRIu64 " releases=%" PRIu64
     " config-and-RX-reset=true no-card-response-or-IRQ=true",s->cp_sim_config.reset_held,
     s->cp_sim_config.reset_assertions,s->cp_sim_config.reset_releases);
 return true;
}
static bool marconi_cp_sim_config_access(MarconiProbeState *s,hwaddr address,uint64_t *value,unsigned size,bool write)
{
 if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||size!=4||s->reported||s->cp_sim_config.reset_held||
    !s->cp_cold.release_accepted||!marconi_cp_cold_source(s)||(write&&*value>UINT32_MAX))return false;
 static const struct {uint32_t read_pc,write_pc,offset,mask;} fields[]={
  {0x64132d10,0x64132d1a,0x0,UINT32_C(0x20)},
  {0x64132d22,0x64132d2c,0x0,UINT32_C(0x40)},
  {0x6419da66,0x6419da70,0x0,UINT32_C(0x80)},
  {0x641a0fb0,0x641a0fba,0x0,UINT32_C(0x100000)},
  {0x641a1266,0x641a126e,0x0,UINT32_C(0x40000)},
  {0x641a1278,0x641a1282,0x0,UINT32_C(0x80000)},
  {0x641a124a,0x641a1250,0x0,UINT32_C(0x20000)},
  {0x641a1088,0x641a1092,0x0,UINT32_C(0x200000)},
  {0x640f8894,0x640f889e,0x0,UINT32_C(0x400000)},
  {0x641a0fe8,0x641a0ff2,0x0,UINT32_C(0x10000)},
  {0x641a0fcc,0x641a0fd6,0x0,UINT32_C(0x1000000)},
  {0x641a1004,0x641a100e,0x0,UINT32_C(0x800000)},
  {0x641a10fa,0x641a1100,0x0,UINT32_C(0x2000)},
  {0x640f887c,0x640f8882,0x0,UINT32_C(0x8000)},
  {0x641a0f62,0x641a0f68,0x0,UINT32_C(0x1000)},
  {0x641a1112,0x641a111c,0x0,UINT32_C(0x100)},
  {0x641a1070,0x641a1076,0x0,UINT32_C(0x800)},
  {0x641a1058,0x641a105e,0x0,UINT32_C(0x400)},
  {0x64141f9e,0x64141fa8,0x28,UINT32_C(0x3fc)},
  {0x64141f86,0x64141f8c,0x28,UINT32_C(0x3)},
  {0x641a103c,0x641a1046,0x0,UINT32_C(0x10)},
  {0x640f8852,0x640f8858,0x24,UINT32_C(0xffff)},
  {0x641a1020,0x641a102a,0x0,UINT32_C(0x8)},
  {0x641a0f94,0x641a0f9e,0x0,UINT32_C(0x4)},
  {0x641a1142,0x641a1148,0x2c,UINT32_C(0xfff)},
  {0x64141b0a,0x64141b10,0x18,UINT32_C(0xffff)},
  {0x64141b26,0x64141b2c,0x20,UINT32_C(0xffff)},
  {0x641a0f7a,0x641a0f82,0x0,UINT32_C(0x1)},
  {0x641a112e,0x641a1130,0x1c,UINT32_C(0xffffffff)},
  {0x641a115a,0x641a1164,0x0,UINT32_C(0x2)},
  {0x6412bb3e,0x6412bb44,0xc,UINT32_C(0x3ff)},
  {0x64140f44,0x64140f4a,0x10,UINT32_C(0x7fff)},
  {0x64140f2c,0x64140f32,0x14,UINT32_C(0xffff0000)},
  {0x64140d24,0x64140d2a,0x14,UINT32_C(0xffff)},
 };
 uint64_t pc=marconi_mmio_pc();
 if(!write&&address==UINT64_C(0x4433003c)&&pc==UINT64_C(0x640f826a)) {
  *value=s->cp_sim_config.events;++s->cp_sim_config.event_reads;return true;
 }
 if(address==UINT64_C(0x44330044)) {
  if(!write) {
   if(pc!=UINT64_C(0x64138072)&&pc!=UINT64_C(0x640f822a))return false;
   *value=0;return true;
  }
  if(pc==UINT64_C(0x64138076)) {if(*value!=UINT32_C(0xfff))return false;}
  else if(pc!=UINT64_C(0x640f8230)||(*value&~UINT64_C(0xfff)))return false;
  s->cp_sim_config.events&=~(uint16_t)*value;++s->cp_sim_config.event_clears;
  marconi_public_report("MARCONI_CP_SIM_EVENT_CLEAR clears=%" PRIu64 " actual-W1C=true no-event-produced=true",
      s->cp_sim_config.event_clears);return true;
 }
 if(!write&&address==UINT64_C(0x44330008)&&pc==UINT64_C(0x641955bc)) {
  *value=s->cp_sim_config.rx_count;
  ++s->cp_sim_config.rx_count_reads;
  if(s->cp_sim_config.rx_count_reads<=4)
   marconi_public_report("MARCONI_CP_SIM_RX_COUNT explicit-no-card-input=true queued-input=%u reads=%" PRIu64,
      s->cp_sim_config.rx_count,s->cp_sim_config.rx_count_reads);
  return true;
 }
 for(unsigned i=0;i<ARRAY_SIZE(fields);++i) {
  if(address!=UINT64_C(0x44330000)+fields[i].offset||(write&&pc!=fields[i].write_pc))continue;
  uint32_t *word=&s->cp_sim_config.word[fields[i].offset/4];
  if(write) {
   if(((uint32_t)*value^*word)&~fields[i].mask)return false;
   *word=(uint32_t)*value;++s->cp_sim_config.writes;
   if(pc==UINT64_C(0x641a1076)||pc==UINT64_C(0x641a105e))++s->cp_sim_config.control_requests;
  } else {*value=*word;++s->cp_sim_config.reads;}
  if(s->cp_sim_config.reads+s->cp_sim_config.writes<=64)
   marconi_public_report("MARCONI_CP_SIM_CONFIG access=%s offset=%02x reads=%" PRIu64 " writes=%" PRIu64
       " retained-config=true control-effects-unimplemented=true no-card-response-or-IRQ=true",
       write?"write":"read",fields[i].offset,s->cp_sim_config.reads,s->cp_sim_config.writes);
  return true;
 }
 return false;
}
static void marconi_cp_axidma_reset(void *opaque)
{
 MarconiProbeState *s=opaque;
 o62m_virtual_otp_wipe_v1(&s->cp_axidma,sizeof s->cp_axidma);
}
static void marconi_cp_axidma_exit(Notifier *notifier,void *opaque)
{
 MarconiProbeState *s=container_of(notifier,MarconiProbeState,cp_axidma_exit);(void)opaque;
 uint64_t writes=s->cp_axidma.config_writes;
 unsigned channels=s->cp_axidma.channels_enabled;
 marconi_cp_axidma_reset(s);
 marconi_public_report("MARCONI_CP_AXIDMA_EXIT config-writes=%" PRIu64 " enabled-channel-mask=%u admitted-transfers=0 private-state-wiped=true",writes,channels);
}
static bool marconi_cp_axidma_access(MarconiProbeState *s,hwaddr address,uint64_t *value,unsigned size,bool write)
{
 if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||size!=4||s->reported||
    !s->cp_cold.release_accepted||!marconi_cp_cold_source(s)||(write&&*value>UINT32_MAX))return false;
 uint32_t *word;uint64_t pc=marconi_mmio_pc(),expected;
 switch(address) {
  case 0x44140040:word=&s->cp_axidma.status_enable;expected=0x64003cc2;break;
  case 0x44140048:word=&s->cp_axidma.signal_enable;expected=0x64003cc6;break;
  case 0x44140010:word=&s->cp_axidma.cfg;expected=0x64003ccc;break;
  default:return false;
 }
 if(!write){*value=*word;return true;}
 if(pc!=expected)return false;
 *word=(uint32_t)*value;++s->cp_axidma.config_writes;
 bool common_irq=(s->cp_axidma.cfg&2)&&
    (s->cp_axidma.common_raw&s->cp_axidma.status_enable&s->cp_axidma.signal_enable);
 if(s->cp_axidma.config_writes<=8)marconi_public_report("MARCONI_CP_AXIDMA_CONFIG offset=%02x writes=%" PRIu64 " DMA-enabled=%u global-IRQ-enabled=%u common-IRQ=%u admitted-transfers=0 cfg-high-bits-retained-uninterpreted=true",
  (unsigned)(address-0x44140000),s->cp_axidma.config_writes,!!(s->cp_axidma.cfg&1),!!(s->cp_axidma.cfg&2),common_irq);
 return true;
}
static void marconi_cp_control398_reset(void *opaque)
{
 MarconiProbeState *s=opaque;
 o62m_virtual_otp_wipe_v1(s->cp_control398_requests,sizeof s->cp_control398_requests);
 memset(s->cp_control398_count,0,sizeof s->cp_control398_count);
}
static void marconi_cp_control398_exit(Notifier *notifier,void *opaque)
{
 MarconiProbeState *s=container_of(notifier,MarconiProbeState,cp_control398_exit);(void)opaque;
 unsigned a=s->cp_control398_count[0],b=s->cp_control398_count[1];
 marconi_cp_control398_reset(s);
 marconi_public_report("MARCONI_CP_CONTROL398_EXIT instance0-requests=%u instance1-requests=%u request-records-wiped=true effects-unimplemented=true",a,b);
}
static bool marconi_cp_control398_access(MarconiProbeState *s,hwaddr address,uint64_t *value,unsigned size,bool write)
{
 if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||!write||size!=4||
    !s->cp_cold.release_accepted||!marconi_cp_cold_source(s)||s->reported)return false;
 unsigned i=address==0x4a400398?0:1,n=s->cp_control398_count[i],phase=n&1;
 if(marconi_mmio_pc()!=(phase?UINT64_C(0x64025ed0):UINT64_C(0x64025ec8))||
    *value!=(phase?10:5))return false;
 s->cp_control398_requests[i][phase]=(uint32_t)*value;++s->cp_control398_count[i];
 if(n<8)marconi_public_report("MARCONI_CP_CONTROL398_REQUEST instance=%u sequence=%u actual-request-retained=true effects-unimplemented=true no-ready-IRQ-or-transfer=true",i,n+1);
 return true;
}
static bool marconi_cp_wdt_core_init(MarconiProbeState *s)
{
 return o62m_wdt4012_init_v2(&s->cp_wdt.core,true,32000,0,0,
     o62m_wdt4012_original_top_cycles_v1,O62M_WDT4012_TOP_COUNT_V2,
     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))==O62M_WDT4012_OK_V2;
}
static bool marconi_cp_wdt_failure(MarconiProbeState *s,int result)
{
 if(s->cp_wdt_event)timer_del(s->cp_wdt_event);
 if(!s->reported) {
  marconi_public_report("MARCONI_CP_WDT_BOUNDARY reason=%d reset-expiry=%u actual-countdown-state=true IRQ0-from-real-first-expiry=true no-system-reset-generated=true",
      result,result==O62M_WDT4012_RESET_EXPIRY_V2);
  marconi_owned_runtime_failure_stop(s);
 }
 return false;
}
static bool marconi_cp_wdt_refresh(MarconiProbeState *s)
{
 if(!s->cp_wdt_event)return true;
 if(s->reported){timer_del(s->cp_wdt_event);return false;}
 if(s->cp_wdt.held||!s->cp_wdt.core.profile_enabled){timer_del(s->cp_wdt_event);return true;}
 bool source=s->cp_cold.release_accepted&&marconi_cp_cold_source(s)&&
             (s->cp_clock_gate_config[1]&UINT32_C(0x20000));
 if(s->cp_wdt.core.running&&!source)return marconi_cp_wdt_failure(s,O62M_WDT4012_DISABLED_V2);
 uint64_t expirations=s->cp_wdt.core.expirations;
 int result=o62m_wdt4012_advance_v2(&s->cp_wdt.core,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
 if(s->cp_wdt.core.expirations!=expirations) {
  s->cp_wdt_expiries+=s->cp_wdt.core.expirations-expirations;
  marconi_public_report("MARCONI_CP_WDT_EXPIRY expiries=%" PRIu64 " irq-pending=%u second-stage=%u reset-expiry=%u actual-deadline=true",
      s->cp_wdt_expiries,s->cp_wdt.core.irq_pending,s->cp_wdt.core.second_stage,s->cp_wdt.core.expired);
 }
 if(result)return marconi_cp_wdt_failure(s,result);
 bool present=false;int64_t deadline=0;
 result=o62m_wdt4012_next_deadline_v2(&s->cp_wdt.core,&present,&deadline);
 if(result)return marconi_cp_wdt_failure(s,result);
 if(present)timer_mod_ns(s->cp_wdt_event,deadline);else timer_del(s->cp_wdt_event);
 return marconi_cp_timer_refresh(s);
}
static void marconi_cp_wdt_callback(void *opaque)
{
 (void)marconi_cp_wdt_refresh(opaque);
}
static void marconi_cp_wdt_reset(void *opaque)
{
 MarconiProbeState *s=opaque;if(s->cp_wdt_event)timer_del(s->cp_wdt_event);
 o62m_cp_wdt_reset_init_v2(&s->cp_wdt);
 if(!marconi_cp_wdt_core_init(s)){error_report("invalid CP watchdog virtual profile");exit(EXIT_FAILURE);}
 s->cp_wdt_accesses=s->cp_wdt_starts=s->cp_wdt_feeds=s->cp_wdt_preloads=s->cp_wdt_expiries=s->cp_wdt_reset_cancellations=0;
}
static void marconi_cp_wdt_exit(Notifier *notifier,void *opaque)
{
 MarconiProbeState *s=container_of(notifier,MarconiProbeState,cp_wdt_exit);(void)opaque;
 uint64_t assertions=s->cp_wdt.assertions,releases=s->cp_wdt.releases;bool running=s->cp_wdt.core.running;
 timer_del(s->cp_wdt_event);o62m_virtual_otp_wipe_v1(&s->cp_wdt,sizeof s->cp_wdt);
 marconi_public_report("MARCONI_CP_WDT_EXIT assertions=%" PRIu64 " releases=%" PRIu64 " accesses=%" PRIu64
     " starts=%" PRIu64 " feeds=%" PRIu64 " preloads=%" PRIu64 " expiries=%" PRIu64 " reset-cancellations=%" PRIu64
     " exit-cancelled-running=%u independent-state-wiped=true timer-cancelled=true actual-IRQ0-source=true",
     assertions,releases,s->cp_wdt_accesses,s->cp_wdt_starts,s->cp_wdt_feeds,s->cp_wdt_preloads,s->cp_wdt_expiries,s->cp_wdt_reset_cancellations,running);
}
static bool marconi_cp_wdt_reset_access(MarconiProbeState *s,uint64_t *value,unsigned size,bool write)
{
 uint64_t pc=marconi_mmio_pc();
 if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||size!=4||s->reported||
    !s->cp_cold.release_accepted||!marconi_cp_cold_source(s)||
    (write?(*value>UINT32_MAX||(pc!=UINT64_C(0x641ac796)&&pc!=UINT64_C(0x641ac79c))):
           (pc!=UINT64_C(0x641ac792)&&pc!=UINT64_C(0x641ac798))))return false;
 if(!marconi_cp_wdt_refresh(s))return false;
 if(write) {
  bool held=s->cp_wdt.held,running=s->cp_wdt.core.running;
  if(!o62m_cp_wdt_reset_write_v2(&s->cp_wdt,(uint32_t)*value))return false;
  if(s->cp_wdt.held){timer_del(s->cp_wdt_event);if(running)++s->cp_wdt_reset_cancellations;}
  else if(held&&!marconi_cp_wdt_core_init(s))return false;
 } else *value=s->cp_wdt.control;
 marconi_public_report("MARCONI_CP_WDT_RESET_ACCESS write=%u held=%u assertions=%" PRIu64 " releases=%" PRIu64
     " actual-independent-reset-state=true running=%u AP-watchdog-unchanged=true",write,s->cp_wdt.held,s->cp_wdt.assertions,s->cp_wdt.releases,s->cp_wdt.core.running);
 return marconi_cp_timer_refresh(s);
}
static bool marconi_cp_wdt_access(MarconiProbeState *s,hwaddr address,uint64_t *value,unsigned size,bool write)
{
 if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||size!=4||s->reported||s->cp_wdt.held||
    !s->cp_cold.release_accepted||!marconi_cp_cold_source(s)||
    !(s->cp_clock_gate_config[8]&UINT32_C(0x20000)))return false;
 if(write&&address==UINT64_C(0x44340000)&&(*value&1)&&
    !(s->cp_clock_gate_config[1]&UINT32_C(0x20000)))return false;
 if(address==UINT64_C(0x44340014)&&(write||marconi_mmio_pc()!=0x6405ca86))return false;
 if(address==UINT64_C(0x44340010))return false;
 if(!marconi_cp_wdt_refresh(s))return false;
 uint64_t starts=s->cp_wdt.core.starts,feeds=s->cp_wdt.core.feeds,preloads=s->cp_wdt.core.preloads;
 int result=o62m_wdt4012_access_v2(&s->cp_wdt.core,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
     address-UINT64_C(0x44340000),size,write,value);
 if(result)return marconi_cp_wdt_failure(s,result);
 ++s->cp_wdt_accesses;s->cp_wdt_starts+=s->cp_wdt.core.starts-starts;
 s->cp_wdt_feeds+=s->cp_wdt.core.feeds-feeds;s->cp_wdt_preloads+=s->cp_wdt.core.preloads-preloads;
 if(!marconi_cp_wdt_refresh(s))return false;
 if(address==UINT64_C(0x44340014))marconi_public_report("MARCONI_CP_WDT_ACK clears=%" PRIu64 " actual-original-EOI-read=true countdown-not-restarted=true",s->cp_wdt.core.irq_clears);
 if(s->cp_wdt_accesses<=32)marconi_public_report("MARCONI_CP_WDT_ACCESS write=%u accesses=%" PRIu64
     " starts=%" PRIu64 " feeds=%" PRIu64 " preloads=%" PRIu64 " running=%u timer-pending=%u virtual-source-hz=32000 same-original-TOP-table=true",
     write,s->cp_wdt_accesses,s->cp_wdt_starts,s->cp_wdt_feeds,s->cp_wdt_preloads,s->cp_wdt.core.running,timer_pending(s->cp_wdt_event));
 return true;
}
static void marconi_cp_domain_qos_reset(void *opaque)
{
 MarconiProbeState *s=opaque;memset(s->cp_domain_qos,0,sizeof s->cp_domain_qos);
 s->cp_domain_qos_written=0;s->cp_domain_qos_writes=0;
}
static void marconi_cp_domain_qos_exit(Notifier *notifier,void *opaque)
{
 MarconiProbeState *s=container_of(notifier,MarconiProbeState,cp_domain_qos_exit);(void)opaque;
 uint64_t writes=s->cp_domain_qos_writes;unsigned written=s->cp_domain_qos_written;
 marconi_cp_domain_qos_reset(s);
 marconi_public_report("MARCONI_CP_DOMAIN_CONFIG_EXIT writes=%" PRIu64 " written-mask=%u configuration-wiped=true no-completion-or-IRQ=true",writes,written);
}
static bool marconi_cp_domain_qos_access(MarconiProbeState *s,hwaddr address,uint64_t *value,unsigned size,bool write)
{
 if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||!write||size!=4||*value>UINT32_MAX||
    !s->cp_cold.release_accepted||!marconi_cp_cold_source(s)||s->reported)return false;
 unsigned index=address==0x44110114?0:1;
 if(marconi_mmio_pc()!=(index?UINT64_C(0x641fbe54):UINT64_C(0x641fbe4a)))return false;
 s->cp_domain_qos[index]=(uint32_t)*value;s->cp_domain_qos_written|=1u<<index;++s->cp_domain_qos_writes;
 marconi_public_report("MARCONI_CP_DOMAIN_CONFIG_WRITE index=%u writes=%" PRIu64 " actual-word-retained=true no-command-completion-or-IRQ=true",index,s->cp_domain_qos_writes);return true;
}
static void marconi_cp_l2_reset(void *opaque)
{
 MarconiProbeState *s=opaque;s->cp_l2_lockdown[0]=s->cp_l2_lockdown[1]=0;
 s->cp_l2_requests=s->cp_l2_completions=0;s->cp_l2_way_completed=0;
}
static void marconi_cp_l2_exit(Notifier *notifier,void *opaque)
{
 MarconiProbeState *s=container_of(notifier,MarconiProbeState,cp_l2_exit);(void)opaque;
 uint64_t requests=s->cp_l2_requests,completions=s->cp_l2_completions;
 marconi_cp_l2_reset(s);device_cold_reset(s->cp_l2);
 marconi_public_report("MARCONI_CP_L2_EXIT maintenance-requests=%" PRIu64 " completions=%" PRIu64 " board-config-cleared=true child-config-reset=true cache-lines=0",requests,completions);
}
static bool marconi_cp_l2_access(MarconiProbeState *s,hwaddr address,uint64_t *value,unsigned size,bool write)
{
 if(!s->cp_l2_mmio||!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||size!=4||
    !s->cp_cold.release_accepted||!s->pm_domain1_available||s->reported||
    (write&&*value>UINT32_MAX))return false;
 uint64_t pc=marconi_mmio_pc();unsigned offset=address-0x44050000;
 static const struct {uint32_t pc;uint16_t offset;bool write;} calls[]={
  {0x64160928,0x104,false},{0x6416092e,0x104,false},{0x6416093a,0x100,false},
  {0x6416094c,0x108,true},{0x6416094e,0x10c,true},
  {0x64160954,0x900,true},{0x6416095a,0x904,true},
  {0x6416095e,0x104,false},{0x6416096c,0x104,true},
  {0x64160978,0x77c,true},{0x6416097a,0x77c,false},
  {0x64160984,0x730,true},{0x6416098e,0x100,true},
  {0x64008ba4,0x7bc,true},{0x64008ba8,0x7bc,false},{0x64008bb4,0x730,true},
  {0x64008bdc,0x7b0,true},{0x64008bf2,0x730,true},
  {0x64008c12,0x7fc,true},{0x64008c16,0x7fc,false},{0x64008c22,0x730,true},
  {0x64008c4a,0x7f0,true},{0x64008c60,0x730,true},
  {0x64008c7e,0x7f0,true},{0x64008c8e,0x7f0,true},
  {0x64008cb2,0x770,true},{0x64008cc6,0x730,true},
 };
 bool matched=false;
 for(unsigned i=0;i<ARRAY_SIZE(calls);i++)if(pc==calls[i].pc&&offset==calls[i].offset&&write==calls[i].write){matched=true;break;}
 if(!matched)return false;
 if(offset==0x900||offset==0x904) {
  if(!write||(*value&~UINT64_C(0xff)))return false;
  s->cp_l2_lockdown[(offset-0x900)/4]=(uint32_t)*value;return true;
 }
 unsigned way=offset==0x77c?1u:offset==0x7bc?2u:offset==0x7fc?4u:0u;
 bool line=offset==0x770||offset==0x7b0||offset==0x7f0;
 bool maintenance=way||line||offset==0x730;
 if(maintenance) {
  if(write) {
   if(way?(*value&~UINT64_C(0xff))!=0:line?(*value&31u)!=0:*value!=0)return false;
  } else if(!way||!(s->cp_l2_way_completed&way))return false;
 }
 MemTxResult result=write?memory_region_dispatch_write(s->cp_l2_mmio,offset,*value,MO_LEUL,MEMTXATTRS_UNSPECIFIED):
  memory_region_dispatch_read(s->cp_l2_mmio,offset,value,MO_LEUL,MEMTXATTRS_UNSPECIFIED);
 if(result!=MEMTX_OK)return false;
 if(maintenance&&write) {
  ++s->cp_l2_requests;++s->cp_l2_completions;
  s->cp_l2_way_completed|=way;
  if(s->cp_l2_requests<=8)marconi_public_report("MARCONI_CP_L2_MAINTENANCE kind=%s requests=%" PRIu64 " completions=%" PRIu64 " cache-lines=0 asynchronous-cache-writes=0 synchronous-empty-set-work=true addresses-redacted=true",way?"way":line?"line":"sync",s->cp_l2_requests,s->cp_l2_completions);
 }
 if(offset==0x100&&write)marconi_public_report("MARCONI_CP_L2_CONTROL enabled=%u actual-original-write=true canonical-QEMU-l2x0=true",(unsigned)*value&1u);
 return true;
}
static bool marconi_cp_ports_access(MarconiProbeState *s,hwaddr address,uint64_t *value,unsigned size,bool write)
{
 if(size!=4||!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||(write&&*value>UINT32_MAX))return false;
 unsigned port=address>=UINT64_C(0x4a900000)?0:1;
 bool status=address==(port?UINT64_C(0x4a820028):UINT64_C(0x4a920020));
 uint64_t pc=marconi_mmio_pc();
 if(!marconi_cp_ports_source(s,port,s->cp_ports[port].requests!=0)){marconi_cp_ports_cancel(s);return false;}
 if(status) {
  uint32_t busy;
  if(write||pc!=(port?0x140u:0xfcu)||!o62m_cp_boot_config_status_v1(&s->cp_ports[port],&busy))return false;
  *value=busy<<(port?24:16);return true;
 }
 uint32_t old=port?s->cp_port820_control:s->psram_init_word[83];
 if(!write) {
  if(port?pc!=0x124u:(pc!=0xe0u&&pc!=0x110u))return false;
  *value=old;return true;
 }
 bool restore=!port&&pc==0x11cu;
 if(port?pc!=0x130u:(!restore&&pc!=0xecu))return false;
 uint32_t expected=restore?old|UINT32_C(0x03000000):old&~UINT32_C(0x03000000);
 if(*value!=expected||(!port&&s->psram_init_count==ARRAY_SIZE(s->psram_init_history))||
    (restore&&(!s->cp_ports[0].valid||s->cp_ports[0].pending)))return false;
 O62MCpBootConfigV1 trial=s->cp_ports[port];
 if(!o62m_cp_boot_config_request_v1(&trial,(uint32_t)*value,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)))return false;
 if(port)s->cp_port820_control=(uint32_t)*value;
 else {
  s->psram_init_word[83]=(uint32_t)*value;s->psram_init_written[83]=true;
  s->psram_init_history[s->psram_init_count].word=83;s->psram_init_history[s->psram_init_count++].value=(uint32_t)*value;
 }
 s->cp_ports[port]=trial;s->cp_ports_armed[port]=trial.generation;s->cp_ports_cold_generation[port]=s->cp_cold.generation;
 if(!port){s->cp_ports_controller_generation=s->psram_controller.generation;s->cp_ports_cal_generation=s->psram_cal.generation;}
 marconi_cp_ports_schedule(s);
 marconi_public_report("MARCONI_CP_BOOT_CONFIG_REQUEST port=%u restore=%u requests=%" PRIu64 " positive-service-ns=500 whole-config-captured=true no-immediate-completion=true",port,restore,trial.requests);
 return true;
}

static bool marconi_cp_cold_access(MarconiProbeState *s,hwaddr address,uint64_t *value,unsigned size,bool write)
{
 if(!s->cp_created||!current_cpu||size!=4||(write&&*value>UINT32_MAX))return false;
 uint64_t pc=marconi_mmio_pc();
 if(address==UINT64_C(0x44120000)) {
  if(current_cpu!=CPU(&s->cp_cpu)||!s->cp_cold.release_accepted||!marconi_cp_cold_source(s))return false;
  if(!write){*value=s->cp_cold.clock_control;return true;}
  if(pc!=UINT64_C(0x6405699e)||*value!=(s->cp_cold.clock_control|7u))return false;
  s->cp_cold.clock_control=(uint32_t)*value;
  marconi_public_report("MARCONI_CP_CLOCK_CONTROL shared-TEE-word=true actual-original-OR7=true no-new-reset-or-ready=true");return true;
 }
 if(address==UINT64_C(0x4c010110)) {
  if(write||current_cpu!=CPU(&s->cp_cpu)||!s->cp_cold.release_accepted)return false;
  *value=s->cp_cold.entry;return true;
 }
 CPUARMState *env=&s->armv7m.cpu->env;
 bool privileged=arm_v7m_is_handler_mode(env)||!(env->v7m.control[env->v7m.secure]&1);
 if(current_cpu!=CPU(s->armv7m.cpu)||!env->v7m.secure||!privileged)return false;
 if(address==UINT64_C(0x5c010110))return write&&pc==UINT64_C(0x73e60990)&&o62m_cp_cold_entry_v1(&s->cp_cold,(uint32_t)*value);
 if(address==UINT64_C(0x5c01010c)) {
  if(!write||pc!=UINT64_C(0x73e6099e)||!o62m_cp_cold_reason_v1(&s->cp_cold,(uint32_t)*value))return false;
  s->ap_board_words[9]=(uint32_t)*value;return true;
 }
 if(address==UINT64_C(0x5a0c0004)) {
  uint32_t timing[3];marconi_cp_cold_inputs(s,timing);
  return write&&pc==UINT64_C(0x73e609b6)&&o62m_cp_cold_on_v1(&s->cp_cold,(uint32_t)*value,timing,s->pm_domain1_timing_written==7&&s->ap_timing_commit_request==1);
 }
 if(address==UINT64_C(0x5c0300c4)||address==UINT64_C(0x5c030118)) {
  unsigned step=address==UINT64_C(0x5c0300c4)?4:5;
  if(!write||pc!=(step==4?UINT64_C(0x73e609b8):UINT64_C(0x73e609bc)))return false;
  O62MCpColdV1 cold=s->cp_cold;O62MPMFunctionalV1 config=s->pm_functional;
  if(!o62m_cp_cold_clock_note_v1(&cold,step,(uint32_t)*value)||!o62m_pm_functional_access_v1(&config,address-UINT64_C(0x10000000),size,true,value))return false;
  s->cp_cold=cold;s->pm_functional=config;return true;
 }
 if(address==UINT64_C(0x5c030100)) {
  if(!write||pc!=UINT64_C(0x73e609c0)||!o62m_cp_cold_submit_v1(&s->cp_cold,(uint32_t)*value,s->pm_functional.config[1],qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)))return false;
  s->cp_cold_armed=s->cp_cold.generation;timer_mod_ns(s->cp_cold_event,s->cp_cold.deadline);
  marconi_public_report("MARCONI_CP_COLD_SUBMIT actual-original-request=true positive-config-service-ns=1000 domain-service-ns=500 CPU-release=false");return true;
 }
 if(address==UINT64_C(0x5c0300e8)) {
  if(write||pc!=UINT64_C(0x73e609c4)||s->cp_cold.stage<6)return false;
  *value=(s->cp_cold.phase==1?1u:0u)|s->cp_pm_local_events;return true;
 }
 if(address==UINT64_C(0x5a0c0028)) {
  if(write||pc!=UINT64_C(0x73e609cc)||s->cp_cold.stage<6)return false;
  *value=(s->pm_domain1_on?1u:0)|(s->pm_domain1_available?2u:0);return true;
 }
 if(address==UINT64_C(0x54120000)) {
  if(!marconi_cp_cold_source(s))return false;
  if(!write){if(pc!=UINT64_C(0x73e609d4))return false;*value=s->cp_cold.clock_control;return true;}
  return pc==UINT64_C(0x73e609da)&&o62m_cp_cold_clock_v1(&s->cp_cold,(uint32_t)*value);
 }
 if(address==UINT64_C(0x5c0101e8)) {
  if(!write){if(pc!=UINT64_C(0x73e609de))return false;*value=s->ap_board_words[3];return true;}
  O62MCpColdV1 trial=s->cp_cold;
  if(pc!=UINT64_C(0x73e609e6)||!marconi_cp_cold_source(s)||!marconi_enable_private_guest_diagnostics(s)||
     !o62m_cp_cold_release_v1(&trial,s->ap_board_words[3],(uint32_t)*value))return false;
  if(arm_get_cpu_by_id(UINT64_C(0x100))!=CPU(&s->cp_cpu))return false;
  if(arm_set_cpu_on_and_reset(UINT64_C(0x100))!=QEMU_ARM_POWERCTL_RET_SUCCESS)return false;
  s->cp_cold=trial;s->ap_board_words[3]=(uint32_t)*value;
  marconi_public_report("MARCONI_CP_COLD_RESET_RELEASE_ACCEPTED architectural-reset0=true original-loaded-CPboot-low-alias=true no-PC-SP-register-patch=true execution-success-not-inferred=true");return true;
 }
 return false;
}

static bool marconi_explore_access(MarconiProbeState *s, hwaddr address,
                                    uint64_t *value, unsigned size, bool write)
{
    uint32_t *reg = NULL, mask = 0;
    if(!marconi_virtual_otp_refresh(s)||!marconi_hash_init_refresh(s)) {return false;}
    if(s->trng_virtual_profile) {
        if(!marconi_virtual_ao_refresh(s)) {return false;}
        O62MVirtualAoMetadataV1 metadata;
        if(o62m_virtual_ao_query_v1(&s->trng_virtual,&metadata)!=O62M_VAO_OK_V1) {return false;}
        if(write&&metadata.phase==O62M_VAO_BUSY_V1) {
            error_report("MARCONI_VIRTUAL_AO_BOUNDARY reason=concurrent-MMIO-write-during-owned-work "
                         "address=%08" HWADDR_PRIx " state-not-committed=true",address);
            return false;
        }
    }
    /* This evaluator does not implement concurrent clock-domain changes.
     * Conservatively stop every unrelated MMIO write while work is pending,
     * before any clock/board/control path can commit a gate/source change. */
    if(s->te_context_fixture&&s->te_context.phase==O62M_TE_CONTEXT_PENDING&&
       write&&address!=O62M_TE_CONTEXT_TOP) {
        error_report("MARCONI_TE_CONTEXT_BOUNDARY reason=concurrent-MMIO-write-during-work-unimplemented "
                     "address=%08" HWADDR_PRIx " no-clock-change-committed=true",address);
        return false;
    }
    /* The OTA provider proves only three32bit leaves. Reject an unproved
     * OTA64bit crossword request before the legacy STRD decomposition can
     * mutate A0 and then fail the new canonical A4 original-PC check. */
    if(s->soc4005_config_inputs&&write&&size==8&&address==0x400500a0) {return false;}
    /* QEMU presents the original TEE STRD at73e59b8c as one 64-bit
     * transaction. Decompose ONLY that register pair into its two evidenced
     * priority words; this makes no claim about interconnect atomicity. */
    if (s->explore_seed && s->qos_observed_write_experiment && write &&
        size == 8 && address == 0x400500a0) {
        uint64_t low = *value & UINT32_MAX, high = *value >> 32;
        return marconi_explore_access(s, address, &low, 4, true) &&
               marconi_explore_access(s, address + 4, &high, 4, true);
    }
    if(s->gpu_boot_reset_profile&&address>=UINT64_C(0x40310000)&&address<UINT64_C(0x40311000)) {
        if(s->reported||size!=4||(address&3)||(write&&*value>UINT32_MAX))return false;
        uint32_t offset=address-UINT64_C(0x40310000);
        if(s->gpu_boot_identity_profile&&(offset==0x20||offset==0x24||offset==0x30||offset==0xe8)) {
            if(write||s->gpu_boot_reset.phase!=O62M_GPU_IDLE||s->gpu_boot_reads==UINT64_MAX)return false;
            /* Read-only virtual ABI labels selected by original AP init.
             * These fields neither complete work nor advertise rendering. */
            *value=offset==0x20?0x265:offset==0x24?0x2002:offset==0x30?0x41e:0;
            ++s->gpu_boot_reads;
            marconi_public_report("MARCONI_GPU_BOOT_IDENTITY_READ offset=%u target-compatible-partial-virtual-device=true",offset);
            return true;
        }
        if(offset==0x10) {
            uint64_t pc=marconi_mmio_pc();
            if(write||!marconi_ap_te_caller(s)||(pc!=0x60e476ea&&pc!=0x60e476d8&&pc!=0x60e4768e))return false;
            *value=s->gpu_boot_reset.interrupt_pending;s->gpu_boot_reset.interrupt_pending=0;++s->gpu_boot_reads;
            marconi_gpu_fill_irq_refresh(s);
            marconi_public_report("MARCONI_GPU_FILL_INTR_READ actual-pending-returned=true virtual-read-to-clear=true");
            return true;
        }
        if(offset==0x14) {
            if(s->gpu_boot_reset.phase!=O62M_GPU_IDLE)return false;
            if(write) {
                if(s->gpu_boot_writes==UINT64_MAX)return false;
                s->gpu_boot_reset.interrupt_enable=(uint32_t)*value;++s->gpu_boot_writes;
            } else {
                if(s->gpu_boot_reads==UINT64_MAX)return false;
                *value=s->gpu_boot_reset.interrupt_enable;++s->gpu_boot_reads;
            }
            marconi_gpu_fill_irq_refresh(s);
            marconi_public_report("MARCONI_GPU_BOOT_INTR_ENABLE access=%s pending-unchanged=true",write?"write":"read");
            return true;
        }
        if(offset==0x504&&write&&s->gpu_address_staged&&marconi_ap_te_caller(s)&&
           marconi_mmio_pc()==UINT64_C(0x60e476a6)&&
           s->armv7m.cpu->env.regs[14]==UINT32_C(0x60e4861b)) {
            if(!marconi_gpu_fill_begin(s,*value))return false;
            ++s->gpu_boot_writes;return true;
        }
        if(offset==0x500) {
            uint64_t pc=marconi_mmio_pc();
            uint32_t lr=s->armv7m.cpu->env.regs[14];
            int caller=lr==UINT32_C(0x60e4860d)?0:lr==UINT32_C(0x60e48651)?1:-1;
            if(!write||!marconi_ap_te_caller(s)||pc!=UINT64_C(0x60e476a6)||caller<0||
               s->gpu_boot_reset.phase!=O62M_GPU_IDLE||!o62m_gpu_boot_open_v1(s->gpu_boot_reset.control)||
               s->gpu_fill||s->gpu_address_staged||s->gpu_address_writes==UINT64_MAX||s->gpu_boot_writes==UINT64_MAX)return false;
            s->gpu_boot_reset.command_address=(uint32_t)*value;s->gpu_address_staged=true;
            ++s->gpu_address_writes;++s->gpu_boot_writes;
            marconi_public_report("MARCONI_GPU_ADDRESS_STAGE static-caller-index=%d writes=%" PRIu64
                " actual-address-retained=true execution-submitted=false private-address-redacted=true",caller,s->gpu_address_writes);
            return true;
        }
        if(offset!=0&&offset!=4)return false;
        if(write) {
            if(offset!=0||s->gpu_boot_writes==UINT64_MAX)return false;
            O62MGpuBootResetV1 trial=s->gpu_boot_reset;
            if(!o62m_gpu_boot_control_v1(&trial,(uint32_t)*value,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)))return false;
            if(!(s->gpu_boot_reset.control&0x1000)&&(trial.control&0x1000))marconi_gpu_state_clear(s);
            s->gpu_boot_reset=trial;++s->gpu_boot_writes;
            if(trial.phase!=O62M_GPU_IDLE) {
                marconi_gpu_fill_cancel(s);
                o62m_virtual_otp_wipe_v1(&s->gpu_boot_reset.command_address,sizeof(s->gpu_boot_reset.command_address));
                s->gpu_address_staged=false;
            }
            if(trial.phase==O62M_GPU_RESET_WORK) {
                s->gpu_boot_armed_generation=trial.generation;timer_mod_ns(s->gpu_boot_event,trial.deadline_ns);
            } else timer_del(s->gpu_boot_event);
        } else {
            if(s->gpu_boot_reads==UINT64_MAX)return false;
            *value=offset?(s->gpu_fill?s->gpu_boot_reset.opaque_status:o62m_gpu_boot_status_v1(&s->gpu_boot_reset)):s->gpu_boot_reset.control;++s->gpu_boot_reads;
        }
        if(write||s->gpu_boot_reads<=32)marconi_public_report("MARCONI_GPU_BOOT_ACCESS offset=%u access=%s phase=%u generation=%" PRIu64 " raw-data-redacted=true",offset,write?"write":"read",s->gpu_boot_reset.phase,s->gpu_boot_reset.generation);
        return true;
    }
    if(s->ap_timer64_view&&address>=UINT64_C(0x4a010000)&&address<=UINT64_C(0x4a010020)) {
        if(s->normal_emmc_bound&&s->reported)return false;
        uint32_t bus_pclk;
        if(o62m_clock_controls_read(&s->clock_controls,UINT64_C(0x400600f8),4,&bus_pclk)!=O62M_CLOCK_CONTROLS_OK||
           !(bus_pclk&UINT32_C(0x20000)))return false;
        uint32_t offset=(uint32_t)(address-UINT64_C(0x4a010000));
        bool pair=!write&&size==8&&offset==8;
        if(!s->explore_seed||!s->timer64||
           (!pair&&(size!=4||(offset&3)||(write&&*value>UINT32_MAX)))||
           (offset!=0&&offset!=4&&offset!=8&&offset!=12&&offset!=16&&offset!=24&&offset!=28&&offset!=32))
            return false;
        uint64_t *counter=write?&s->ap_timer64_view_writes:&s->ap_timer64_view_reads;
        if(*counter==UINT64_MAX||(pair&&s->ap_timer64_view_pairs==UINT64_MAX))return false;
        if(!marconi_dvfs_timer64_refresh(s))return false;
        O62MTimer64 trial=s->dvfs_timer64_model;
        uint64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),out=0;
        uint32_t low=0,high=0;
        bool ok;
        if(pair) {
            ok=o62m_timer64_read(&trial,now,8,&low)&&o62m_timer64_read(&trial,now,12,&high);
            out=low|((uint64_t)high<<32);
        } else if(write) {
            ok=o62m_timer64_write(&trial,now,offset,(uint32_t)*value);
        } else {
            ok=o62m_timer64_read(&trial,now,offset,&low);out=low;
        }
        if(!ok)return false;
        s->dvfs_timer64_model=trial;
        if(!write)*value=out;
        ++*counter;if(pair)++s->ap_timer64_view_pairs;
        if(s->ap_timer64_view_reads+s->ap_timer64_view_writes<=64)marconi_public_report("MARCONI_AP_TIMER64_SOFTWARE_VIEW access=%s offset=%u "
            "reads=%" PRIu64 " writes=%" PRIu64 " snapshot-pairs=%" PRIu64
            " independent-DVFS-counter=true actual-time-and-sync-only=true "
            "counter-and-data-values-redacted=true RTOS-counter-unchanged=true",
            write?"write":"read",offset,s->ap_timer64_view_reads,
            s->ap_timer64_view_writes,s->ap_timer64_view_pairs);
        return marconi_dvfs_timer64_refresh(s);
    }
    if(s->venc_device_profile&&address==UINT64_C(0x41060000)) {
        if(!s->explore_seed||s->reported||write||size!=4)return false;
        *value=0;
        marconi_public_report("MARCONI_VENC_ABSENT_ID_READ virtual-device-absent=true unsupported-ID-zero=true no-codec-work-or-success-generated=true");
        return true;
    }
    if(s->vdec_device_profile&&address==UINT64_C(0x41070000)) {
        if(!s->explore_seed||s->reported||write||size!=4)return false;
        *value=0;
        marconi_public_report("MARCONI_VDEC_ABSENT_ID_READ virtual-device-absent=true unsupported-ID-zero=true no-codec-work-or-success-generated=true");
        return true;
    }
    if(s->ap_board_config&&address>=0x41010060&&address<0x41010078) {
        if(!s->explore_seed||s->reported||(size!=4&&size!=8)||(address&(size-1))||address+size>0x41010078||
           (write&&size==4&&*value>UINT32_MAX))return false;
        unsigned index=(address-0x41010060)/4,count=size/4;
        uint8_t mask=((1U<<count)-1)<<index;
        if(!write&&(s->ap_vpu_board_written&mask)!=mask)return false;
        if(write) {
            for(unsigned i=0;i<count;i++)s->ap_vpu_board_words[index+i]=(uint32_t)(*value>>(32*i));
            s->ap_vpu_board_written|=mask;
        } else {
            *value=0;
            for(unsigned i=0;i<count;i++)*value|=(uint64_t)s->ap_vpu_board_words[index+i]<<(32*i);
        }
        marconi_public_report("MARCONI_AP_VPU_BOARD_CONFIG access=%s first-word=%u words=%u "
                              "actual-written-backing-only=true no-ready-work-or-IRQ-generated=true raw-values-redacted=true",
                              write?"write":"read",index,count);
        return true;
    }
    if(s->explore_seed&&size==8&&write&&address==0x443e0018) {
        marconi_explore_update(s);return marconi_cp_ssi_access(s,address,value,size,write);
    }
    if (!s->explore_seed || size != 4 || (address & 3) ||
        (write && *value > UINT32_MAX)) {
        return false;
    }
    marconi_explore_update(s);
    if(s->normal_emmc_bound&&s->reported)return false;
    if(s->rc_measurement_fixture&&(address==0x5c090000||address==0x5c09000c)) {
        uint64_t pc=marconi_mmio_pc();
        unsigned index=address==0x5c090000?0:1;
        if(write||pc!=(index?0x73e624b2:0x73e624a0)) {
            error_report("MARCONI_RC_MEASUREMENT_REJECT access=%s address=0x%08" HWADDR_PRIx
                " pc=0x%08" PRIx64 " only-original-frequency-getter-read=true",write?"write":"read",address,pc);return false;
        }
        *value=s->rc_measurement[index];
        error_report("MARCONI_RC_MEASUREMENT_READ address=0x%08" HWADDR_PRIx
            " value=%08x pc=0x%08" PRIx64 " explicit-fullword-input=true "
            "physical-sample-generation-and-initial-values-unknown=true",address,s->rc_measurement[index],pc);
        return true;
    }
    if(s->adsp_publication_profile&&address==UINT64_C(0x4c010104)) {
        if(write||s->adsp_publication_reads==UINT64_MAX)return false;
        *value=0;++s->adsp_publication_reads;
        if(s->adsp_publication_reads<=8||(s->adsp_publication_reads%1024)==0)
            marconi_public_report("MARCONI_ADSP_UNPUBLISHED_READ reads=%" PRIu64 " virtual-ns=%" PRId64
                                  " no-peer-publication=true metadata-zero=true no-audio-execution-or-success-pointer=true",
                                  s->adsp_publication_reads,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        return true;
    }
    if(marconi_cp_busmon_index(address)>=0)return marconi_cp_busmon_access(s,address,value,size,write);
    if(marconi_cp_pm_init_port(address))return marconi_cp_pm_init_access(s,address,value,size,write);
    if((address>=UINT64_C(0x44130000)&&address<UINT64_C(0x44131000))||
       address==UINT64_C(0x44110039)||address==UINT64_C(0x44110238)||address==UINT64_C(0x44110438))
        return marconi_cp_eac_access(s,address,value,size,write);
    if(address==UINT64_C(0x4c010144))return marconi_cp_pm_progress_access(s,value,size,write);
    if(address==UINT64_C(0x44110038)) {
      uint64_t pc=marconi_mmio_pc();
      if(pc==UINT64_C(0x640f881c)||pc==UINT64_C(0x640f8836)||pc==UINT64_C(0x640f883c))
        return marconi_cp_sim_reset_access(s,value,size,write);
      return marconi_cp_wdt_reset_access(s,value,size,write);
    }
    if(address>=UINT64_C(0x44340000)&&address<=UINT64_C(0x44340014))return marconi_cp_wdt_access(s,address,value,size,write);
    if(s->ap_mailbox_config&&s->mbox_outbound_experiment&&s->board_controls_fixture&&
       (address==UINT64_C(0x4c120048)||address==UINT64_C(0x4c120040)||
        address==UINT64_C(0x5c120020)||address==UINT64_C(0x5c120018))) {
        uint64_t pc=marconi_mmio_pc();
        unsigned receiver=address>=UINT64_C(0x50000000);
        bool status=address==(receiver?UINT64_C(0x5c120020):UINT64_C(0x4c120048));
        if((!receiver&&!marconi_ap_te_caller(s))||write==status)return false;
        if(status) {
            if(pc!=(receiver?UINT64_C(0x73e57828):UINT64_C(0x60c2765e)))return false;
            *value=o62m_ap_tee_mbox_read_v1(&s->ap_tee_mbox,receiver);
        } else {
            if(receiver?(pc!=UINT64_C(0x73e5784a)&&pc!=UINT64_C(0x73e57830)):
               (pc!=UINT64_C(0x60c27682)&&pc!=UINT64_C(0x60c277be)))return false;
            if(!o62m_ap_tee_mbox_clear_v1(&s->ap_tee_mbox,receiver,(uint32_t)*value))return false;
        }
        marconi_ap_tee_mbox_refresh(s);
        marconi_public_report("MARCONI_AP_TEE_DOORBELL_ACCESS receiver=%u access=%s pending=%u reads=%" PRIu64
            " clears=%" PRIu64 " local-doorbell-only=true",
            receiver,write?"clear":"read",s->ap_tee_mbox.pending[receiver]!=0,
            s->ap_tee_mbox.reads[receiver],s->ap_tee_mbox.clears[receiver]);
        return true;
    }
    if(s->ap_mailbox_config&&address>=UINT64_C(0x4c1200c8)&&address<=UINT64_C(0x4c120158)) {
        uint64_t pc=marconi_mmio_pc();
        bool cp=s->cp_created&&current_cpu==CPU(&s->cp_cpu)&&s->cp_cold.release_accepted&&!s->reported&&marconi_cp_cold_source(s);
        if((!marconi_ap_te_caller(s)&&!cp)||!o62m_ap_ipc_slots_access_v2(&s->ap_ipc_slots,address,size,write,value,pc))return false;
        if(s->ap_ipc_slots.reads<=32||write)
            marconi_public_report("MARCONI_AP_IPC_SLOTS access=%s status=%u writes=%" PRIu64 " commits=%" PRIu64
                " reads=%" PRIu64 " busy-slots=%u staged-slots=%u actual-D8-only-release=true no-payload-output=true",
                write?"write":"read",address==UINT64_C(0x4c1200c8),s->ap_ipc_slots.writes,s->ap_ipc_slots.commits,
                s->ap_ipc_slots.reads,ctpop32(s->ap_ipc_slots.busy),ctpop32(s->ap_ipc_slots.staged));
        return true;
    }
    if(s->ap_mailbox_config&&address==UINT64_C(0x4c120028)) {
        if(write)s->ap_mailbox_enable=(uint32_t)*value;else *value=s->ap_mailbox_enable;
        marconi_ap_tee_mbox_refresh(s);
        marconi_public_report("MARCONI_AP_MAILBOX_ENABLE access=%s retained-enable=true derived-doorbell-IRQ=true raw-values-redacted=true",write?"write":"read");
        return true;
    }
    if(s->ap_mailbox_config&&address==UINT64_C(0x4c120050)) {
        if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||size!=4||s->reported||
           !s->cp_cold.release_accepted||!marconi_cp_cold_source(s)||
           (write&&*value>UINT32_MAX))return false;
        if(write){s->cp_mailbox_config.enable=(uint32_t)*value;++s->cp_mailbox_config.writes;}
        else {*value=s->cp_mailbox_config.enable;++s->cp_mailbox_config.reads;}
        marconi_public_report("MARCONI_CP_MAILBOX_ENABLE access=%s reads=%" PRIu64 " writes=%" PRIu64
            " canonical-receiver-config=true no-event-delivery-IRQ-or-ACK=true",write?"write":"read",s->cp_mailbox_config.reads,s->cp_mailbox_config.writes);
        return true;
    }
    if(s->ap_mailbox_config&&address==UINT64_C(0x4c120038)&&s->cp_created&&current_cpu==CPU(&s->cp_cpu)) {
        uint64_t pc=marconi_mmio_pc();uint64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        O62MMboxOutbound *history=&s->cp_mbox_outbound;
        if(!write||size!=4||pc!=UINT64_C(0x6417e760)||!s->cp_cold.release_accepted||
           !marconi_cp_cold_source(s)||!s->mbox_outbound_experiment||!s->board_controls_fixture||
           !*value||*value>UINT32_MAX||(*value&(*value-1))||history->count>=O62M_MBOX_OUTBOUND_CAPACITY||
           (history->count&&now<history->request[history->count-1].now_ns))return false;
        unsigned event=ctz32((uint32_t)*value);
        if(!o62m_ap_tee_mbox_deliver_mask_v2(&s->ap_tee_mbox,0,(uint32_t)*value))return false;
        history->request[history->count++]=(O62MMboxRequest){now,1,event};
        marconi_ap_tee_mbox_refresh(s);
        marconi_public_report("MARCONI_CP_MAILBOX_OUTBOUND count=%u low-event=%u actual-AP-pending=true slot-busy-unchanged=true",history->count,event);
        return true;
    }
    if(s->ap_mailbox_config&&address>=0x4c120010&&address<=0x4c1200b4) {
        uint64_t pc=marconi_mmio_pc();
        if(write&&pc==0x60c2ff9a&&s->ap_mbox_outbound.count>=O62M_MBOX_OUTBOUND_CAPACITY)
            marconi_public_report("MARCONI_AP_MAILBOX_HISTORY_BOUNDARY count=%u capacity=%u recording-budget-exhausted=true hardware-busy-claimed=false no-overwrite=true",s->ap_mbox_outbound.count,O62M_MBOX_OUTBOUND_CAPACITY);
        if(!write||!marconi_ap_te_caller(s)||!o62m_ap_mbox_outbound_notify_v1(&s->ap_mbox_outbound,
            address,size,*value,pc,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
            bool address_known=false;
            for(unsigned d=0;d<5;d++)for(unsigned half=0;half<2;half++)
                address_known|=address==UINT64_C(0x4c120010)+d*0x28u+half*4u;
            marconi_public_report("MARCONI_AP_MAILBOX_REJECT write=%u caller=%u pc-match=%u size32=%u nonzero=%u u32=%u onehot=%u address-known=%u history-space=%u monotonic=%u no-payload-output=true",
                write,marconi_ap_te_caller(s),pc==UINT64_C(0x60c2ff9a),size==4,*value!=0,*value<=UINT32_MAX,
                *value&& !(*value&(*value-1)),address_known,s->ap_mbox_outbound.count<O62M_MBOX_OUTBOUND_CAPACITY,
                !s->ap_mbox_outbound.count||(uint64_t)qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)>=s->ap_mbox_outbound.request[s->ap_mbox_outbound.count-1].now_ns);
            return false;
        }
        O62MMboxRequest *r=&s->ap_mbox_outbound.request[s->ap_mbox_outbound.count-1];
        bool routed=s->mbox_outbound_experiment&&s->board_controls_fixture&&r->destination==0&&r->event==0;
        if(routed){o62m_ap_tee_mbox_deliver_v1(&s->ap_tee_mbox,1);marconi_ap_tee_mbox_refresh(s);}
        marconi_public_report("MARCONI_AP_MAILBOX_OUTBOUND_REQUEST count=%u routed-to-TEE=%u "
            "every-original-write-preserved=true separate-from-TEE-history=true payload-redacted=true",
            s->ap_mbox_outbound.count,routed);
        return true;
    }
    if(s->mbox_outbound_experiment&&address>0x5c120000&&address<=0x5c1200c8) {
        uint64_t pc=marconi_mmio_pc();
        if(!write||!o62m_mbox_outbound_notify(&s->mbox_outbound,address,size,*value,pc,
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
            error_report("MARCONI_MBOX_OUTBOUND_REJECT access=%s address=0x%08" HWADDR_PRIx
                " pc=0x%08" PRIx64 " requests=%u no-peer-status-ACK-or-delivery=true",
                write?"write":"read",address,pc,s->mbox_outbound.count);return false;
        }
        O62MMboxRequest *r=&s->mbox_outbound.request[s->mbox_outbound.count-1];
        bool routed=s->ap_mailbox_config&&s->board_controls_fixture&&r->destination==1&&r->event==19;
        if(routed){o62m_ap_tee_mbox_deliver_v1(&s->ap_tee_mbox,0);marconi_ap_tee_mbox_refresh(s);}
        error_report("MARCONI_MBOX_OUTBOUND_REQUEST destination=%u event=%u count=%u "
            "virtual-ns=%" PRIu64 " routed-to-AP=%u every-original-write-preserved=true",
            r->destination,r->event,s->mbox_outbound.count,r->now_ns,routed);
        return true;
    }
    if(s->timer4014_inputs&&address>=0x40140000&&address<0x40140028) {
        uint64_t data=*value;
        uint32_t csr0,csr1,pclk;O62MDw32ClockV1 clock={0};
        marconi_timer4014_words(s,&csr0,&csr1,&pclk);
        if(!marconi_timer4014_clock(s,&s->clock_controls,csr0,csr1,pclk,&clock)) {return false;}
        O62MDw32V1 trial=s->timer4014;
        int result=o62m_dw32_set_clock_v1(&trial,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),&clock);
        if(result) {return marconi_timer4014_failure(s,result);}
        result=o62m_dw32_access_v1(&trial,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                 address-0x40140000,size,write,&data);
        if(result) {
            error_report("MARCONI_TIMER4014_MMIO_BOUNDARY reason=%s address=%08" HWADDR_PRIx,
                         o62m_dw32_reason_v1(result),address);return false;
        }
        if(!marconi_timer4014_publish(s,&trial)) {return false;}
        if(!write) {*value=data;}
        if(s->timer4014_accesses++<2048) {
            error_report("MARCONI_TIMER4014_ACCESS access=%s address=%08" HWADDR_PRIx
                         " value=%08" PRIx64 " pc=%08" PRIx64 " virtual-ns=%" PRId64
                         " counter-from-virtual-time=true independent-two32bit-channels=true",
                         write?"write":"read",address,data,
                         marconi_mmio_pc(),
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        }
        return true;
    }
    if(address>=UINT64_C(0x4a050000)&&address<=UINT64_C(0x4a0500ff)) {
        if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||s->reported||
           !s->cp_cold.release_accepted||!marconi_cp_cold_source(s))return false;
        O62MUart1BootConfigV1 trial=s->cp_uart2;uint64_t data=*value;
        if(!o62m_uart1_boot_access_v1(&trial,address-0x4a050000,size,write,&data)||
           o62m_uart1_empty_irq_v1(&trial))return false;
        s->cp_uart2=trial;if(!write)*value=data;
        if(s->cp_uart2.accesses<=128)
            marconi_public_report("MARCONI_CP_UART2_CONFIG access=%s offset=%02x accesses=%" PRIu64
                " resets=%" PRIu64 " actual-configuration=true empty-queue-projection=true no-data-or-peer-response=true",
                write?"write":"read",(unsigned)(address-0x4a050000),s->cp_uart2.accesses,s->cp_uart2.resets);
        return true;
    }
    if(s->ap_gpio_config&&address>=0x4a060000&&address<=0x4a0600ff) {
        int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if(!marconi_ap_te_caller(s)||!marconi_uart1_tx_advance(s,now))return false;
        unsigned offset=address-0x4a060000;
        uint64_t discarded=s->uart1_boot.tx.discarded;
        if(write&&offset==0&&!(s->uart1_boot.lcr&0x80)) {
            if(size!=4||s->uart1_accepted>=sizeof(s->uart1_outbound)||
               !o62m_uart1_send_v1(&s->uart1_boot,(uint8_t)*value,
                    now,83500000,true))return false;
            s->uart1_accepted++;
            if(s->uart1_accepted<=32)
                marconi_public_report("MARCONI_UART1_TX_ACCEPT accepted=%" PRIu64
                    " explicit-source-Hz=83500000 explicit-CTS=true AFCE=%u holding=%u active=%u no-payload-output=true",
                    s->uart1_accepted,(s->uart1_boot.mcr&0x20)!=0,s->uart1_boot.tx.count,s->uart1_boot.tx.active);
        }else if(!o62m_uart1_boot_access_v1(&s->uart1_boot,offset,size,write,value))return false;
        s->uart1_discarded+=s->uart1_boot.tx.discarded-discarded;
        marconi_uart1_tx_arm(s);
        if(s->uart1_boot.accesses<=128)
            marconi_public_report("MARCONI_AP_UART1_BOOT_CONFIG access=%s offset=%02x"
                " accesses=%" PRIu64 " resets=%" PRIu64 " separate-UART1=true no-external-RX-source=true no-payload-output=true",
                write?"write":"read",offset,s->uart1_boot.accesses,s->uart1_boot.resets);
        return true;
    }
    if(s->ap_gpio_config&&!s->ota_config_body_inputs&&o62m_ota_config_body_address_v1(address)) {
        uint64_t pc=marconi_mmio_pc();
        bool cp=current_cpu==CPU(&s->cp_cpu);
        CPUARMState *env=&s->armv7m.cpu->env;
        uint32_t field_mask=0;
        if(pc==(write?0x60d73da4:0x60d73d9c)&&
           (address==0x4c05821c||address==0x4c058220))field_mask=UINT32_MAX;
        else if(pc==(write?0x60c45568:0x60c4555c))field_mask=7;
        else if(pc==(write?0x60c45d82:0x60c45d64))field_mask=0xe000;
        else if(pc==(write?0x60c45de6:0x60c45dcc))field_mask=0xc0000;
        else if(cp&&pc==(write?0x640c0db8:0x640c0da4))field_mask=0xc0000;
        else if(cp&&pc==(write?0x640c0e6c:0x640c0e58))field_mask=0x30000;
        bool caller=cp ? s->cp_cold.release_accepted&&marconi_cp_cold_source(s)&&
            (s->hwlock.owned&(UINT32_C(1)<<24))&&(cpsr_read(&s->cp_cpu.env)&CPSR_I) :
            marconi_ap_te_caller(s)&&env->v7m.basepri[0]==0x80;
        if(size!=4||!field_mask||!caller||s->ota_config_envelope.next_step!=5)return false;
        for(unsigned i=0;i<O62M_OTA_CONFIG_BODY_WORDS_V1;i++)if(address==o62m_ota_config_words_v1[i].address) {
            uint32_t old=s->ota_config_body.word[i];
            if(write) {
                if(*value>UINT32_MAX||(((uint32_t)*value^old)&~field_mask)||
                   (field_mask==0xe000&&!(*value&0x8000)))return false;
                s->ota_config_body.word[i]=(uint32_t)*value;s->ap_pinmux_writes++;
            } else {*value=old;s->ap_pinmux_reads++;}
            marconi_public_report("MARCONI_AP_PINMUX_CONFIG cpu=%u access=%s index=%u reads=%" PRIu64 " writes=%" PRIu64
                " sole-shared-word-backing=true explicit-virtual-initial-zero=true retained-caller-specific-fields-only=true no-pad-event-or-IRQ=true",
                cp?1:0,write?"write":"read",i,s->ap_pinmux_reads,s->ap_pinmux_writes);
            return true;
        }
        return false;
    }
    if(s->ota_config_body_inputs&&o62m_ota_config_body_address_v1(address)) {
        uint64_t pc=marconi_mmio_pc();
        uint64_t data=*value;
        unsigned cursor=s->ota_config_body.cursor;
        CPUARMState *env=&s->armv7m.cpu->env;
        bool privileged=arm_v7m_is_handler_mode(env)||!(env->v7m.control[env->v7m.secure]&UINT32_C(1));
        if(!env->v7m.secure||!privileged||cursor>=O62M_OTA_CONFIG_BODY_WORDS_V1||
           !(s->hwlock.owned&(UINT32_C(1)<<24))||s->ota_config_envelope.next_step!=5||
           s->ota_config_envelope.completed_cycles!=o62m_ota_config_words_v1[cursor].bank) {
            error_report("MARCONI_OTA_CONFIG_BODY_BOUNDARY reason=original-Secure-privileged-owned24-open-matching-bank-required cursor=%u",cursor);return false;
        }
        O62MOtaConfigBodyResultV1 result=o62m_ota_config_body_access_v1(
            &s->ota_config_body,pc,address,size,write,&data);
        if(result!=O62M_OTA_CONFIG_BODY_OK) {
            error_report("MARCONI_OTA_CONFIG_BODY_BOUNDARY reason=%s pc=%08" PRIx64,
                         o62m_ota_config_body_reason_v1(result),pc);return false;
        }
        if(!write) {*value=data;}
        error_report("MARCONI_OTA_CONFIG_BODY_ACCESS access=%s address=%08" HWADDR_PRIx
                     " value=%08" PRIx64 " pc=%08" PRIx64 " index=%u next=%u pending-write=%u reads=%u writes=%u "
                     "explicit-independent-initial-fullwords=true retained-configuration-only=true "
                     "silicon-reset-and-physical-pad-effects-unknown=true",
                     write?"write":"read",address,data,pc,cursor,s->ota_config_body.cursor,
                     s->ota_config_body.pending_write,s->ota_config_body.reads,s->ota_config_body.writes);
        return true;
    }
    if(s->ap_gpio_config&&!s->ota_config_write_observations&&o62m_config_envelope_address_v1(address)) {
        uint64_t pc=marconi_mmio_pc();
        bool cp=current_cpu==CPU(&s->cp_cpu);
        CPUARMState *env=&s->armv7m.cpu->env;
        bool caller=cp ? s->cp_cold.release_accepted&&marconi_cp_cold_source(s)&&
            (s->hwlock.owned&(UINT32_C(1)<<24))&&(cpsr_read(&s->cp_cpu.env)&CPSR_I) :
            marconi_ap_te_caller(s)&&env->v7m.basepri[0]==0x80;
        if(!caller)return false;
        int result=cp ? o62m_cp_config_envelope_access_v4(&s->ota_config_envelope,pc,address,size,write,value) :
            o62m_ap_config_envelope_access_v2(&s->ota_config_envelope,pc,address,size,write,value);
        if(result!=O62M_CONFIG_ENVELOPE_OK)return false;
        marconi_public_report("MARCONI_AP_CONFIG_ENVELOPE_WRITE cpu=%u step=%u cycles=%" PRIu64 " writes=%" PRIu64
            " sole-shared-word-backing=true original-caller-critical-section=true configuration-capture-only=true no-permission-ready-IRQ=true",
            cp?1:0,s->ota_config_envelope.next_step,s->ota_config_envelope.completed_cycles,s->ota_config_envelope.accepted_writes);
        return true;
    }
    if(s->ota_config_write_observations&&o62m_config_envelope_address_v1(address)) {
        uint64_t pc=marconi_mmio_pc();
        /* Independent input value, never aliases observation metadata. The
         * already-acquired original slot24 is mandatory for every step. */
        uint64_t input=*value;
        CPUARMState *env=&s->armv7m.cpu->env;
        bool privileged=arm_v7m_is_handler_mode(env)||!(env->v7m.control[env->v7m.secure]&UINT32_C(1));
        if(!env->v7m.secure||!privileged||!(s->hwlock.owned & (UINT32_C(1)<<24))) {
            error_report("MARCONI_OTA_CONFIG_ENVELOPE_BOUNDARY reason=original-Secure-privileged-owned24-required");return false;
        }
        if(s->ota_config_body_inputs) {
            unsigned cycle=s->ota_config_envelope.completed_cycles;
            unsigned expected=s->ota_config_envelope.next_step<5?(cycle?63:0):(cycle?150:63);
            if(cycle>=2||s->ota_config_body.pending_write||s->ota_config_body.cursor!=expected) {
                error_report("MARCONI_OTA_CONFIG_ENVELOPE_BOUNDARY reason=original-body-boundary-required cursor=%u cycle=%u step=%u",
                             s->ota_config_body.cursor,cycle,s->ota_config_envelope.next_step);return false;
            }
        }
        O62MConfigEnvelopeResultV1 result=o62m_config_envelope_access_v1(
            &s->ota_config_envelope,pc,address,size,write,&input);
        if(result!=O62M_CONFIG_ENVELOPE_OK) {
            error_report("MARCONI_OTA_CONFIG_ENVELOPE_BOUNDARY reason=%s pc=%08" PRIx64,
                         o62m_config_envelope_reason_v1(result),pc);return false;
        }
        error_report("MARCONI_OTA_CONFIG_ENVELOPE_WRITE address=%08" HWADDR_PRIx
                     " value=%08" PRIx64 " pc=%08" PRIx64 " accepted=%" PRIu64 " cycles=%" PRIu64
                     " original-slot24-owned=true write-observation-only=true "
                     "physical-permission-gate-sideeffects-and-readback-unimplemented=true",
                     address,input,pc,s->ota_config_envelope.accepted_writes,s->ota_config_envelope.completed_cycles);
        return true;
    }
    if(s->ota_invalid_clock_marker_fixture&&address==0x22004ff4) {
        uint64_t pc=marconi_mmio_pc();
        if(write||size!=4||pc!=0x605ebc58) {
            error_report("MARCONI_OTA_CLOCK_MARKER_BOUNDARY access=%s size=%u pc=%08" PRIx64
                         " only-original-guard-fullword-read=true",write?"write":"read",size,pc);return false;
        }
        *value=s->ota_invalid_clock_marker;
        error_report("MARCONI_OTA_CLOCK_MARKER_READ address=22004ff4 value=%08x pc=%08" PRIx64 " "
                     "explicit-invalid-cache-input=true original-fallback-required=true "
                     "physical-reset-unknown=true valid-magic-and-cachepayload-never-synthesized=true",
                     s->ota_invalid_clock_marker,pc);
        return true;
    }
    if(s->pm_counter_fixture&&address==O62M_PM32K_ADDRESS) {
        if(write||size!=4) {
            error_report("MARCONI_PM_COUNTER_BOUNDARY reason=proved-aligned-read-only size=%u write=%u",size,write);return false;
        }
        uint32_t sample;
        int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        O62MPM32KResult result=o62m_pm32k_read(&s->pm_counter,&sample,now);
        if(result!=O62M_PM32K_OK) {
            error_report("MARCONI_PM_COUNTER_BOUNDARY reason=%s virtual-ns=%" PRId64,
                         o62m_pm32k_reason(result),now);return false;
        }
        *value=sample;
        error_report("MARCONI_PM_COUNTER_READ address=4c0302c4 ticks=%u fraction=%u "
                     "virtual-ns=%" PRId64 " hz=32768 always-running-virtual-policy=true "
                     "physical-gate-reset-wrap-unknown=true independent-counter=true",
                     sample,s->pm_counter.fraction_numerator,now);
        return true;
    }
    /* Independent retained control/vote paths only. No frequency transition,
     * DFSstate projection, ready bit or interrupt is generated by these writes. */
    if(s->virtual_dvs_profile&&address==0x4a12009c) {
        if(write) {
            if(*value>15)return false;
            s->dvfs_com_vote=*value;s->dvfs_com_vote_written=true;s->dvfs_com_vote_writes++;
        } else {
            if(!s->dvfs_com_vote_written)return false;*value=s->dvfs_com_vote;
        }
        marconi_public_report("MARCONI_DVFS_COM_VOTE access=%s writes=%" PRIu64 " actual-retained-vote=true DFSstate-and-frequency-unchanged=true no-completion-or-IRQ-created=true",write?"write":"read",s->dvfs_com_vote_writes);return true;
    }
    if(s->virtual_dvs_profile&&address==0x4a120200) {
        if(!write||(*value!=1&&*value!=4))return false;
        unsigned index=*value==1?0:1;s->dvfs_request200_count[index]++;
        marconi_public_report("MARCONI_DVFS_CONTROL_REQUEST static-onehot-index=%u count=%" PRIu64 " actual-request-latched=true no-W1S-readback-or-run-completion-inferred=true",index,s->dvfs_request200_count[index]);return true;
    }
    if(s->virtual_dvs_profile) {
        static const uint32_t ports[5]={0x4a1200a0,0x4a1200a4,0x4a1200ac,0x4a1200b8,0x4a1200bc};
        for(unsigned i=0;i<5;i++)if(address==ports[i]) {
            if(write) {
                if(*value!=1)return false;
                s->dvfs_controls[i]=*value;s->dvfs_control_written[i]=true;s->dvfs_control_writes++;
            } else {
                if(!s->dvfs_control_written[i])return false;*value=s->dvfs_controls[i];
            }
            marconi_public_report("MARCONI_DVFS_ENABLE_CONFIG access=%s static-word-index=%u writes=%" PRIu64 " actual-retained-config=true no-clock-frequency-or-IRQ-created=true",write?"write":"read",i,s->dvfs_control_writes);return true;
        }
    }
    if(s->dvfs_vote_config&&address==UINT64_C(0x4a1200d0)) {
        uint64_t pc=marconi_mmio_pc();
        if(size!=4||s->dvfs_vote_d0_accesses==UINT64_MAX)return false;
        if(write) {
            if(pc!=UINT64_C(0x60e06aa2)||*value>15)return false;
            s->dvfs_vote_d0_word=(uint32_t)*value;s->dvfs_vote_d0_written=true;
        } else {
            if(pc!=UINT64_C(0x60e06abe)||!s->dvfs_vote_d0_written)return false;
            *value=s->dvfs_vote_d0_word;
        }
        ++s->dvfs_vote_d0_accesses;
        marconi_public_report("MARCONI_DVFS_VOTE_D0_CONFIG access=%s accesses=%" PRIu64 " actual-original-config-retained=true independent-written-only-word=true no-initial-seed-DFS-state-clock-transition-completion-or-IRQ=true raw-word-redacted=true",write?"write":"read",s->dvfs_vote_d0_accesses);
        return true;
    }
    if(s->dvfs_main_clock_initial&&address==UINT64_C(0x4a1200dc)) {
        uint64_t pc=marconi_mmio_pc();
        bool cp_view=s->cp_created&&current_cpu==CPU(&s->cp_cpu)&&
            s->cp_cold.release_accepted&&!s->reported&&marconi_cp_cold_source(s);
        if(write||size!=4||(!cp_view&&pc!=UINT64_C(0x60e07190))||s->dvfs_main_clock_reads==UINT64_MAX)return false;
        *value=s->dvfs_main_clock_word;++s->dvfs_main_clock_reads;
        marconi_public_report("MARCONI_DVFS_MAIN_CLOCK_INITIAL_READ reads=%" PRIu64 " explicit-independent-fullword-input=true original-guest-mux-div-parenttable-decode=true no-DFS-state-completion-ready-or-frequency-update=true raw-word-redacted=true",s->dvfs_main_clock_reads);
        return true;
    }
    if(address==UINT64_C(0x4a1200cc)) {
        if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||!write||size!=4||*value>15||
           marconi_mmio_pc()!=UINT64_C(0x6405bb8e)||!s->cp_cold.release_accepted||
           s->reported||!marconi_cp_cold_source(s))return false;
        s->cp_dvfs_limit_word=(uint32_t)*value;++s->cp_dvfs_limit_writes;
        marconi_public_report("MARCONI_CP_DVFS_LIMIT_CONFIG writes=%" PRIu64 " actual-config-retained=true no-voltage-clock-transition-or-ready=true",s->cp_dvfs_limit_writes);
        return true;
    }
    if(s->virtual_dvs_profile&&(address==0x4a120088||address==0x4a120038)) {
        if(!marconi_virtual_dvs_refresh(s))return false;
        if(address==0x4a120088) {
            if(write)return marconi_virtual_dvs_request(s,*value);
            *value=s->virtual_dvs_requested;return true;
        }
        if(write)return false;
        uint64_t consumer=marconi_mmio_pc();
        /* The same fullword also contains independent DFS state bits6:3.
         * Only the proven DVS getter/poll may consume this narrow view. */
        if(consumer!=0x60e068ea&&consumer!=0x60e0684e)return false;
        uint32_t status;
        if(o62m_virtual_dvs_status_v1(&s->virtual_dvs,&status)!=O62M_VDVS_V1_OK)return false;
        *value=status;s->virtual_dvs_status_reads++;
        if(s->virtual_dvs_status_reads<=4||s->virtual_dvs.phase==O62M_VDVS_IDLE_V1)
            marconi_public_report("MARCONI_VIRTUAL_DVS_STATUS phase=%u completed=%" PRIu64 " actual-job-state=true raw-values-redacted=true",s->virtual_dvs.phase,s->virtual_dvs.completed);
        return true;
    }
    if(s->aon120_config_inputs) {
        size_t index;int selected=o62m_clock_controls_index(&s->aon120_config,address,size,&index);
        if(selected!=O62M_CLOCK_CONTROLS_UNKNOWN_ADDRESS) {
            if(selected!=O62M_CLOCK_CONTROLS_OK)return false;
            uint32_t data=0;int result=write?o62m_clock_controls_write(&s->aon120_config,address,size,*value):
                o62m_clock_controls_read(&s->aon120_config,address,size,&data);
            if(result!=O62M_CLOCK_CONTROLS_OK)return false;
            if(write){s->aon120_config_writes++;s->aon120_config_written|=UINT32_C(1)<<index;}else {*value=data;s->aon120_config_reads++;}
            marconi_public_report("MARCONI_AON120_CONFIG_ACCESS access=%s static-word-index=%zu reads=%" PRIu64 " writes=%" PRIu64
                                  " actual-retained-config-only=true configuration-does-not-complete-work=true raw-values-redacted=true",
                                  write?"write":"read",index,s->aon120_config_reads,s->aon120_config_writes);return true;
        }
    }
    if(s->pm_domain7_profile&&(address==0x4a0c0040||address==0x4a0c003c)) {
        if(size!=4||!marconi_pm_domain7_refresh(s))return false;
        if(address==0x4a0c0040) {
            if(write)return false;*value=o62m_pm_domain4_status_v1(&s->pm_domain7);
            marconi_public_report("MARCONI_PM_DOMAIN7_QUERY model-domain-on=%u available=%u explicit-virtual-state=true raw-values-redacted=true",s->pm_domain7.on,s->pm_domain7.available);return true;
        }
        if(!write||*value>1||!o62m_pm_domain4_request_v1(&s->pm_domain7,*value==1,1,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)))return false;
        marconi_public_report("MARCONI_PM_DOMAIN7_REQUEST target-on=%u actual-request=true transition-pending=true no-immediate-completion=true",s->pm_domain7.target_on);
        return marconi_pm_domain7_refresh(s);
    }
    if(s->pm_bit9_profile&&(address==0x4c030228||address==0x4c030230)) {
        if(size!=4||!marconi_pm_bit9_refresh(s))return false;
        if(address==0x4c030228) {
            if(write)return false;*value=o62m_pm_bit_domain9_status_v1(&s->pm_bit9);
            marconi_public_report("MARCONI_PM_BIT9_QUERY model-domain-on=%u pending=%u explicit-initial-and-functional-pending-policy=true raw-values-redacted=true",s->pm_bit9.on,s->pm_bit9.pending);
            return true;
        }
        if(!write||!o62m_pm_bit_domain9_request_v1(&s->pm_bit9,*value,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)))return false;
        marconi_public_report("MARCONI_PM_BIT9_REQUEST target-on=%u actual-command-received=true transition-pending=true down-has-no-guest-postpoll=true",s->pm_bit9.target_on);
        return marconi_pm_bit9_refresh(s);
    }
    if(s->pm_bit8_profile&&(address==0x4c03022c||address==0x4c030234)) {
        if(size!=4||!marconi_pm_bit8_refresh(s))return false;
        if(address==0x4c03022c) {
            if(write)return false;*value=o62m_pm_bit_domain9_status_v1(&s->pm_bit8);
            marconi_public_report("MARCONI_PM_BIT8_QUERY model-domain-on=%u pending=%u explicit-initial-and-functional-pending-policy=true raw-values-redacted=true",s->pm_bit8.on,s->pm_bit8.pending);
            return true;
        }
        if(!write||!o62m_pm_bit_domain9_request_v1(&s->pm_bit8,*value,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)))return false;
        if(s->mipi_phy_pll_event)marconi_mipi_phy_invalidate(s);
        marconi_public_report("MARCONI_PM_BIT8_REQUEST target-on=%u actual-command-received=true transition-pending=true down-has-no-guest-postpoll=true",s->pm_bit8.target_on);
        return marconi_pm_bit8_refresh(s);
    }
    if(s->usb_clock_inputs) {
        size_t index;int selected=o62m_clock_controls_index(&s->usb_clocks,address,size,&index);
        if(selected!=O62M_CLOCK_CONTROLS_UNKNOWN_ADDRESS) {
            if(selected!=O62M_CLOCK_CONTROLS_OK||!marconi_pm_bit9_refresh(s)||!o62m_pm_bit_domain9_clock_available_v1(&s->pm_bit9))return false;
            uint32_t data=0;int result=write?o62m_clock_controls_write(&s->usb_clocks,address,size,*value):
                o62m_clock_controls_read(&s->usb_clocks,address,size,&data);
            if(result!=O62M_CLOCK_CONTROLS_OK)return false;
            if(write)s->usb_clock_writes++;else {*value=data;s->usb_clock_reads++;}
            marconi_public_report("MARCONI_USB_CLOCK_ACCESS access=%s static-word-index=%zu reads=%" PRIu64 " writes=%" PRIu64
                                  " original-ROM-fields-only=true real-domain9-state-gated=true no-host-USB-packet-or-PLLready-generated=true raw-values-redacted=true",
                                  write?"write":"read",index,s->usb_clock_reads,s->usb_clock_writes);return true;
        }
    }
    if(s->mipi_clock_inputs) {
        size_t index;int selected=o62m_clock_controls_index(&s->mipi_clocks,address,size,&index);
        if(selected!=O62M_CLOCK_CONTROLS_UNKNOWN_ADDRESS) {
            if(selected!=O62M_CLOCK_CONTROLS_OK||!marconi_pm_bit8_refresh(s)||!o62m_pm_bit_domain9_clock_available_v1(&s->pm_bit8))return false;
            uint32_t data=0;int result=write?o62m_clock_controls_write(&s->mipi_clocks,address,size,*value):
                o62m_clock_controls_read(&s->mipi_clocks,address,size,&data);
            if(result!=O62M_CLOCK_CONTROLS_OK)return false;
            if(write)s->mipi_clock_writes++;else {*value=data;s->mipi_clock_reads++;}
            marconi_public_report("MARCONI_MIPI_CLOCK_ACCESS access=%s static-word-index=%zu reads=%" PRIu64 " writes=%" PRIu64
                                  " original-ROM-fields-only=true real-domain8-state-gated=true no-host-MIPI-packet-or-PLLready-generated=true raw-values-redacted=true",
                                  write?"write":"read",index,s->mipi_clock_reads,s->mipi_clock_writes);return true;
        }
    }
    if(s->pubap_clock_inputs) {
        bool listed=false;
        for(unsigned i=0;i<O62M_PUBAP_CLOCK_COUNT_V1;i++)if(address==o62m_pubap_clock_defs_v1[i].address)listed=true;
        if(listed&&(!marconi_pm_domain4_refresh(s)||!s->pm_domain4.available||!s->pm_domain4.on))return false;
        size_t index;
        int selected=o62m_clock_controls_index(&s->pubap_clocks,address,size,&index);
        if(selected!=O62M_CLOCK_CONTROLS_UNKNOWN_ADDRESS) {
            if(selected!=O62M_CLOCK_CONTROLS_OK||!marconi_pm_domain4_refresh(s)||
               !s->pm_domain4.available||!s->pm_domain4.on)return false;
            uint32_t data=0;
            int result=write?o62m_clock_controls_write(&s->pubap_clocks,address,size,*value):
                o62m_clock_controls_read(&s->pubap_clocks,address,size,&data);
            if(result!=O62M_CLOCK_CONTROLS_OK)return false;
            if(write)s->pubap_clock_writes++;else {*value=data;s->pubap_clock_reads++;}
            marconi_public_report("MARCONI_PUBAP_CLOCK_ACCESS access=%s new-owner-word-index=%zu reads=%" PRIu64 " writes=%" PRIu64
                                  " original-ROM-fields-only=true real-domain4-state-gated=true no-PLL-lock-ready-or-frame-generated=true raw-values-redacted=true",
                                  write?"write":"read",index,s->pubap_clock_reads,s->pubap_clock_writes);
            return true;
        }
    }
    if(address==0x44120074||address==0x441200b8||(address>=0x443e0000&&address<0x443e0100))
        return marconi_cp_ssi_access(s,address,value,size,write);
    if(marconi_bbpll_port(address))return marconi_bbpll_access(s,address,value,size,write);
    if(address==UINT64_C(0x44110098)||(address>0x44120000&&address<=0x44120100))
        return marconi_cp_clock_init_access(s,address,value,size,write);
    if(address==0x4a8d0028||address==0x4a9b0028)return marconi_cp_dfs_request_access(s,address,value,size,write);
    if(address>=0x44330000&&address<0x44331000)return marconi_cp_sim_config_access(s,address,value,size,write);
    if(address>=0x44140000&&address<0x44141000)return marconi_cp_axidma_access(s,address,value,size,write);
    if(address==0x4a400398||address==0x4a500398)return marconi_cp_control398_access(s,address,value,size,write);
    if(address==0x44110114||address==0x44110118)return marconi_cp_domain_qos_access(s,address,value,size,write);
    if(address>=0x44050000&&address<0x44051000)return marconi_cp_l2_access(s,address,value,size,write);
    if((address>=0x44300000&&address<=0x44300020)||(address>=0x44310000&&address<=0x44310020))
        return marconi_cp_timer_access(s,address,value,size,write);
    if(address>=0x443f0000&&address<0x443f1000)return marconi_cp_intc_config_access(s,address,value,size,write);
    if(s->cp_created&&current_cpu==CPU(&s->cp_cpu)&&
       (address==0x4a92001c||address==0x4a920020||address==0x4a820024||address==0x4a820028))
        return marconi_cp_ports_access(s,address,value,size,write);
    if(s->pubcp_clock_inputs&&address==0x4a92001c&&current_cpu==CPU(s->armv7m.cpu)) {
        uint64_t pc=marconi_mmio_pc();
        unsigned step;
        if(marconi_lps_pm_caller(s,(uint32_t)pc,0x73e59a97))step=0;
        else if(marconi_lps_pm_caller(s,(uint32_t)pc,0x73e59aa1))step=1;
        else if(marconi_lps_pm_caller(s,(uint32_t)pc,0x73e59aad))step=2;
        else return false;
        bool read_pc=step==0?pc==0x320062a4:pc==0x3200629c;
        bool write_pc=step==0?(pc>=0x320062a4&&pc<=0x320062a8):(pc>=0x3200629c&&pc<=0x320062a0);
        if(size!=4||(!write&&!read_pc)||(write&&!write_pc)||
           !marconi_psram_controller_source(s,true)||!s->psram_controller.operating)return false;
        uint32_t old=s->psram_init_word[83];
        if(write) {
            uint32_t next=step==0?old&UINT32_C(0xffff0000):old|(step==1?UINT32_C(0x03000a28):UINT32_C(0x10000));
            if(*value!=next||s->psram_init_count==ARRAY_SIZE(s->psram_init_history))return false;
            s->psram_init_word[83]=next;s->psram_init_written[83]=true;
            s->psram_init_history[s->psram_init_count].word=83;
            s->psram_init_history[s->psram_init_count++].value=next;++s->psram_final_writes;
        } else {*value=old;++s->psram_final_reads;}
        marconi_public_report("MARCONI_PSRAM_FINAL_CONFIG step=%u write=%u reads=%" PRIu64 " writes=%" PRIu64
            " actual-retained-RMW=true no-status-or-IRQ=true raw-data-redacted=true",
            step,write,s->psram_final_reads,s->psram_final_writes);return true;
    }
    if(s->pubcp_clock_inputs&&(address==0x4a900324||address==0x4a900004)) {
        uint32_t lr=address==0x4a900324?0x320058ed:0x320058f9;
        if(write||size!=4||!marconi_lps_pm_caller(s,0x32006268,lr)||
           !s->psram_controller.requests)return false;
        if(!marconi_psram_controller_source(s,true)){marconi_psram_controller_cancel(s);return false;}
        *value=address==0x4a900324?s->psram_controller.config_ready:s->psram_controller.operating;
        ++s->psram_controller_reads;
        if(s->psram_controller_reads<=2||*value)
            marconi_public_report("MARCONI_PSRAM_CONTROLLER_READ status-class=%u ready=%u reads=%" PRIu64
                " read-does-not-advance=true",address==0x4a900324?0:1,(unsigned)*value,s->psram_controller_reads);
        return true;
    }
    if(s->pubcp_clock_inputs&&address==0x4a910204) {
        if(write||size!=4||!marconi_lps_pm_caller(s,0x32006268,0x3200573f)||
           !s->psram_cal.requests)return false;
        if(!marconi_psram_cal_source(s,true)){marconi_psram_cal_cancel(s);return false;}
        *value=o62m_psram_cal_status_v1(&s->psram_cal);++s->psram_cal_reads;
        if(s->psram_cal_reads<=2||s->psram_cal.valid)
            marconi_public_report("MARCONI_PSRAM_CAL_READ valid=%u pending=%u reads=%" PRIu64
                " read-does-not-advance=true",s->psram_cal.valid,s->psram_cal.pending,s->psram_cal_reads);
        return true;
    }
    if(s->pubcp_clock_inputs&&write&&current_cpu==CPU(s->armv7m.cpu)&&
       marconi_mmio_pc()==0x32006276) {
        for(unsigned i=0;i<ARRAY_SIZE(marconi_psram_init_calls_v5);++i) {
            if(address!=marconi_psram_init_calls_v5[i].address||
               !marconi_lps_pm_caller(s,0x32006276,marconi_psram_init_calls_v5[i].call+5))continue;
            if(size!=4||*value>UINT32_MAX||s->psram_init_count==ARRAY_SIZE(s->psram_init_history)||
               !marconi_pm_domain5_refresh(s)||!s->pm_domain5.available||!s->pm_domain5.on)return false;
            unsigned word=marconi_psram_init_calls_v5[i].word;
            uint32_t call=marconi_psram_init_calls_v5[i].call;
            bool start=call==0x3200572a;
            bool copy=call==0x32005752||call==0x3200575e||call==0x3200576a;
            bool apply=call==0x32005776,tail=call==0x32005786;
            bool ctl_open=call==0x320058cc,ctl_submit=call==0x320058d8;
            bool port=word>=77&&word<=80;
            bool parameter=word==81,command=word==82;
            bool phy_input=word>=51&&word<=64&&word!=53;
            O62MPsramCalibrationV1 trial=s->psram_cal;
            O62MPsramControllerV1 ctl=s->psram_controller;
            if(start) {
                if(!marconi_psram_cal_source(s,false)||
                   !o62m_psram_cal_request_v1(&trial,(uint32_t)*value,26000000,1000,
                       qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),true))return false;
            } else if(copy||apply||tail) {
                if(!marconi_psram_cal_source(s,true)){marconi_psram_cal_cancel(s);return false;}
                if((copy&&!o62m_psram_cal_copy_v1(&trial,(unsigned)((address-0x4a910208)/4),(uint32_t)*value))||
                   (apply&&!o62m_psram_cal_apply_v1(&trial,(uint32_t)*value))||
                   (tail&&(!trial.applied||*value!=3)))return false;
            } else if(ctl_open||ctl_submit||port) {
                if(!marconi_psram_controller_source(s,port)) {
                    marconi_psram_controller_cancel(s);return false;
                }
                if(ctl_open) {
                    if(*value!=0)return false;
                    o62m_psram_controller_open_v1(&ctl);
                } else if(ctl_submit) {
                    uint64_t valid=0;
                    for(unsigned k=0;k<O62M_PSRAM_CONTROLLER_WORDS_V1;++k)
                        if(s->psram_init_written[k])valid|=UINT64_C(1)<<k;
                    if(*value!=1||!o62m_psram_controller_submit_v1(&ctl,s->psram_init_word,valid,
                        &s->psram_init_word[72],s->psram_cal.code,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),true))return false;
                } else if(!o62m_psram_controller_port_v1(&ctl,word-77,(uint32_t)*value))return false;
            } else if(parameter||command) {
                static const uint32_t parameter_calls[]={0x73e59a1a,0x73e59af2,0x73e59a5c,0x73e59a78};
                static const uint32_t command_calls[]={0x73e59a24,0x73e59afc,0x73e59a66,0x73e59a82};
                if(!marconi_psram_controller_source(s,true)||!s->psram_controller.operating||
                   s->psram_controller.ports!=15||s->psram_pair_count==ARRAY_SIZE(s->psram_pairs))return false;
                if(parameter) {
                    if(s->psram_pair_staged||call!=parameter_calls[s->psram_pair_count])return false;
                } else if(!s->psram_pair_staged||call!=command_calls[s->psram_pair_count]||
                          *value!=UINT32_C(0x80000010))return false;
            } else if(s->psram_cal.pending||s->psram_controller.phase)return false;
            if(!start&&!copy&&!apply&&!tail&&!ctl_open&&!ctl_submit&&!port&&!parameter&&!command) {
                if(phy_input) {
                    if(s->psram_cal.valid)marconi_psram_cal_cancel(s);
                    ++s->psram_config_revision;
                }
                if(word<51)++s->psram_controller_revision;
                if(s->psram_controller.config_ready||s->psram_controller.operating)
                    marconi_psram_controller_cancel(s);
            }
            if(start||copy||apply||tail)s->psram_cal=trial;
            if(ctl_open||ctl_submit||port)s->psram_controller=ctl;
            s->psram_init_word[word]=(uint32_t)*value;s->psram_init_written[word]=true;
            s->psram_init_history[s->psram_init_count].word=word;
            s->psram_init_history[s->psram_init_count++].value=(uint32_t)*value;
            if(start) {
                s->psram_cal_revision=s->psram_config_revision;
                s->psram_cal_mempll=s->mempll_vote.generation;
                s->psram_cal_config=s->pm_domain5.staging_generation;
                s->psram_cal_transitions=s->pm_domain5.transitions;
                s->psram_cal_armed=s->psram_cal.generation;
                timer_mod_ns(s->psram_cal_event,s->psram_cal.deadline);
                marconi_public_report("MARCONI_PSRAM_CAL_REQUEST generation=%" PRIu64 " requests=%" PRIu64
                    " actual-command-committed=true prior-configs=13 virtual-reference-hz=26000000 cell-ps=1000",
                    s->psram_cal.generation,s->psram_cal.requests);
            }
            if(parameter){s->psram_pair_staging=(uint32_t)*value;s->psram_pair_staged=true;}
            if(command) {
                s->psram_pairs[s->psram_pair_count].parameter=s->psram_pair_staging;
                s->psram_pairs[s->psram_pair_count++].command=(uint32_t)*value;
                o62m_virtual_otp_wipe_v1(&s->psram_pair_staging,sizeof(s->psram_pair_staging));
                s->psram_pair_staged=false;
                marconi_public_report("MARCONI_PSRAM_PAIR_ACCEPTED pairs=%u unconsumed=%u "
                    "actual-parameter-command-pair=true no-completion-or-IRQ=true raw-data-redacted=true",
                    s->psram_pair_count,s->psram_pair_count);
            }
            if(ctl_open)timer_del(s->psram_controller_event);
            if(ctl_submit) {
                s->psram_controller_cal_generation=s->psram_cal.generation;
                s->psram_controller_captured_revision=s->psram_controller_revision;
                s->psram_controller_armed=s->psram_controller.generation;
                timer_mod_ns(s->psram_controller_event,s->psram_controller.deadline);
                marconi_public_report("MARCONI_PSRAM_CONTROLLER_REQUEST requests=%" PRIu64
                    " actual-zero-one-command=true prior-controller-config-words=45 snapshot-captured=true",
                    s->psram_controller.requests);
            }
            marconi_public_report("MARCONI_PSRAM_INIT_WRITE static-call-index=%u word-index=%u writes=%u "
                "actual-write-captured=true calibration-command=%u calibration-apply=%u raw-data-redacted=true",
                i,word,s->psram_init_count,start,apply);
            return true;
        }
    }
    if(s->pubcp_clock_inputs&&address==0x5a9c0000&&current_cpu==CPU(s->armv7m.cpu)) {
        uint64_t pc=marconi_mmio_pc();
        bool clear=marconi_lps_pm_caller(s,(uint32_t)pc,0x73e599d3);
        bool set=marconi_lps_pm_caller(s,(uint32_t)pc,0x73e599dd);
        if(clear||set) {
            bool read_pc=clear?pc==0x320062a4:pc==0x3200629c;
            bool write_pc=clear?(pc>=0x320062a4&&pc<=0x320062a8):(pc>=0x3200629c&&pc<=0x320062a0);
            uint32_t old;
            if(size!=4||(!write&&!read_pc)||(write&&!write_pc)||!marconi_pm_domain5_refresh(s)||
               !s->pm_domain5.available||!s->pm_domain5.on||
               o62m_clock_controls_read(&s->pubcp_clocks,0x4a9c0000,4,&old)!=O62M_CLOCK_CONTROLS_OK)return false;
            if(write) {
                uint32_t next=clear?old&~UINT32_C(0x707):old|UINT32_C(7);
                O62MClockControls trial=s->pubcp_clocks;
                if(*value!=next||
                   (clear&&o62m_clock_controls_write(&trial,0x4a9c0000,4,old&~UINT32_C(7))!=O62M_CLOCK_CONTROLS_OK)||
                   o62m_clock_controls_write(&trial,0x4a9c0000,4,next)!=O62M_CLOCK_CONTROLS_OK)return false;
                s->pubcp_clocks=trial;
            } else *value=old;
            marconi_public_report("MARCONI_LPS_PUBCP_MUX_CONFIG access=%s clear-fields=%u "
                "same-canonical-word=true actual-RMW=true no-clock-ready-or-peer-state=true",write?"write":"read",clear);
            return true;
        }
    }
    if(s->pubcp_clock_inputs&&address==0x4c0101ac) {
        uint64_t pc=marconi_mmio_pc();
        uint32_t bit=0;
        if(marconi_lps_pm_caller(s,(uint32_t)pc,0x73e5999b)||
           marconi_lps_pm_caller(s,(uint32_t)pc,0x32005ddf))bit=4;
        else if(marconi_lps_pm_caller(s,(uint32_t)pc,0x32005be9))bit=2;
        if(size!=4||!bit||(!write&&pc!=0x3200629c)||
           (write&&pc!=0x3200629c&&pc!=0x3200629e&&pc!=0x320062a0))return false;
        if(write) {
            if(*value!=(s->lps_recovery_control|bit))return false;
            s->lps_recovery_control=(uint32_t)*value;
        } else *value=s->lps_recovery_control;
        marconi_public_report("MARCONI_LPS_RECOVERY_CONFIG access=%s original-OR-field=%u "
            "retained-word-only=true no-reset-completion-or-peer-state=true",write?"write":"read",bit);
        return true;
    }
    if(s->pubcp_clock_inputs&&address>=0x5a9c0000&&address<=0x5a9c008c&&
       current_cpu==CPU(s->armv7m.cpu)&&
       marconi_mmio_pc()>=0x3200629c&&
       marconi_mmio_pc()<=0x320062a8&&
       ((s->armv7m.cpu->env.regs[14]==0x320059d9&&
         marconi_mmio_pc()>=0x320062a4)||
        (s->armv7m.cpu->env.regs[14]==0x32005a01&&
         marconi_mmio_pc()<=0x320062a0))) {
        uint64_t pc=marconi_mmio_pc();
        bool clear=marconi_lps_pm_caller(s,(uint32_t)pc,0x320059d9);
        bool set=marconi_lps_pm_caller(s,(uint32_t)pc,0x32005a01);
        bool read_pc=clear?pc==0x320062a4:pc==0x3200629c;
        bool write_pc=clear?(pc==0x320062a4||pc==0x320062a6||pc==0x320062a8):
                            (pc==0x3200629c||pc==0x3200629e||pc==0x320062a0);
        if(size!=4||(address&3)||(!clear&&!set)||(!write&&!read_pc)||(write&&!write_pc)||
           !marconi_pm_domain5_refresh(s)||!s->pm_domain5.available||!s->pm_domain5.on)return false;
        unsigned word=(address-0x5a9c0000)/4;
        uint32_t old,mask=(word==0||word==3)?UINT32_C(0x100000):UINT32_C(0x200000);
        uint32_t canonical=(uint32_t)address-UINT32_C(0x10000000);
        int legacy=o62m_boot_clock_index(canonical);
        unsigned owner=legacy>=0?1:word==34?2:word==35?3:0;
        if(owner==1) {if(!o62m_boot_clock_read(&s->boot_clocks,canonical,&old))return false;}
        else if(owner==2)old=s->lps_pubcp_auto88;
        else if(o62m_clock_controls_read(owner==3?&s->board_controls:&s->pubcp_clocks,
                                         owner==3?address:canonical,4,&old)!=O62M_CLOCK_CONTROLS_OK)return false;
        if(write) {
            uint32_t next=clear?old&~mask:old|mask;
            if(*value!=next)return false;
            if(owner==1) {if(!o62m_boot_clock_write(&s->boot_clocks,canonical,next))return false;}
            else if(owner==2)s->lps_pubcp_auto88=next;
            else if(o62m_clock_controls_write(owner==3?&s->board_controls:&s->pubcp_clocks,
                                              owner==3?address:canonical,4,next)!=O62M_CLOCK_CONTROLS_OK)return false;
        } else *value=old;
        marconi_public_report("MARCONI_LPS_PUBCP_AUTO_CONFIG word-index=%u owner-index=%u access=%s "
            "set=%u same-canonical-backing=true real-domain5-on=true no-clock-completion-or-peer-state=true",
            word,owner,write?"write":"read",set);
        return true;
    }
    if(s->pubcp_clock_inputs) {
        bool listed=false;
        for(unsigned i=0;i<O62M_PUBCP_CLOCK_COUNT_V1;i++)if(address==o62m_pubcp_clock_defs_v1[i].address)listed=true;
        if(listed&&(!marconi_pm_domain5_refresh(s)||!s->pm_domain5.available||!s->pm_domain5.on))return false;
        size_t index;
        int selected=o62m_clock_controls_index(&s->pubcp_clocks,address,size,&index);
        if(selected!=O62M_CLOCK_CONTROLS_UNKNOWN_ADDRESS) {
            if(selected!=O62M_CLOCK_CONTROLS_OK||!marconi_pm_domain5_refresh(s)||
               !s->pm_domain5.available||!s->pm_domain5.on)return false;
            uint32_t data=0;
            int result=write?o62m_clock_controls_write(&s->pubcp_clocks,address,size,*value):
                o62m_clock_controls_read(&s->pubcp_clocks,address,size,&data);
            if(result!=O62M_CLOCK_CONTROLS_OK)return false;
            if(write)s->pubcp_clock_writes++;else {*value=data;s->pubcp_clock_reads++;}
            marconi_public_report("MARCONI_PUBCP_CLOCK_ACCESS access=%s new-owner-word-index=%zu reads=%" PRIu64 " writes=%" PRIu64
                                  " original-ROM-fields-only=true real-domain5-state-gated=true no-PLL-lock-ready-or-frame-generated=true raw-values-redacted=true",
                                  write?"write":"read",index,s->pubcp_clock_reads,s->pubcp_clock_writes);
            return true;
        }
    }
    if(s->vpu_clock_inputs) {
        bool listed=false;
        for(unsigned i=0;i<O62M_VPU_CLOCK_COUNT_V1;i++)if(address==o62m_vpu_clock_defs_v1[i].address)listed=true;
        if(listed&&(!marconi_pm_domain7_refresh(s)||!s->pm_domain7.available||!s->pm_domain7.on))return false;
        size_t index;
        int selected=o62m_clock_controls_index(&s->vpu_clocks,address,size,&index);
        if(selected!=O62M_CLOCK_CONTROLS_UNKNOWN_ADDRESS) {
            if(selected!=O62M_CLOCK_CONTROLS_OK||!marconi_pm_domain7_refresh(s)||
               !s->pm_domain7.available||!s->pm_domain7.on)return false;
            uint32_t data=0;
            int result=write?o62m_clock_controls_write(&s->vpu_clocks,address,size,*value):
                o62m_clock_controls_read(&s->vpu_clocks,address,size,&data);
            if(result!=O62M_CLOCK_CONTROLS_OK)return false;
            if(write)s->vpu_clock_writes++;else {*value=data;s->vpu_clock_reads++;}
            marconi_public_report("MARCONI_VPU_CLOCK_ACCESS access=%s new-owner-word-index=%zu reads=%" PRIu64 " writes=%" PRIu64
                                  " original-ROM-fields-only=true real-domain7-state-gated=true no-PLL-lock-ready-or-frame-generated=true raw-values-redacted=true",
                                  write?"write":"read",index,s->vpu_clock_reads,s->vpu_clock_writes);
            return true;
        }
    }
    if((s->pm_domain4_profile||s->pm_domain5_profile)&&(address==0x4a0c011c||address==0x4a0c021c)) {
        if(size!=4||!write||!*value||*value>0x80||(*value&(*value-1)))return false;
        bool fifth=(*value&0xf0)!=0;
        O62MPMDomain4V1 *domain=fifth?&s->pm_domain5:&s->pm_domain4;
        if((fifth&&!s->pm_domain5_profile)||(!fifth&&!s->pm_domain4_profile)||
           !(fifth?marconi_pm_domain5_refresh(s):marconi_pm_domain4_refresh(s)))return false;
        uint32_t variant=(uint32_t)*value>>(fifth?4:0);
        if(!o62m_pm_domain4_request_v1(domain,address==0x4a0c011c,variant,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)))return false;
        if(address==0x4a0c011c)s->pm_domain_request_word|=(uint32_t)*value;
        else s->pm_domain_request_word&=~(uint32_t)*value;
        marconi_public_report("MARCONI_PM_DOMAIN_REQUEST logical-domain=%u target-on=%u real-transition-pending=true no-immediate-completion=true",fifth?5:4,domain->target_on);
        return fifth?marconi_pm_domain5_refresh(s):marconi_pm_domain4_refresh(s);
    }
    if(s->pm_domain5_profile&&address==0x4a0c001c) {
        uint64_t pc=marconi_mmio_pc();
        bool ap=marconi_ap_te_caller(s);
        bool lps=marconi_lps_pm_caller(s,write?0x32006276:0x32006268,
                                      write?0x32005bab:0x32005b9f);
        if(size!=4||(!ap&&!lps)||!marconi_pm_domain5_refresh(s))return false;
        if(!write) {
            if(!lps&&pc!=0x60c55572)return false;
            *value=s->pm_domain_request_word;return true;
        }
        O62MPMDomain4V1 trial=s->pm_domain5;
        if((!lps&&pc!=0x60c55578)||*value>UINT32_MAX||(*value&0x80)||
           ((*value^s->pm_domain_request_word)&~UINT64_C(0x80))||
           !o62m_pm_domain4_request_v1(&trial,false,8,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)))return false;
        s->pm_domain5=trial;s->pm_domain_request_word=(uint32_t)*value;
        marconi_public_report("MARCONI_PM_DOMAIN_DIRECT_REQUEST logical-domain=5 target-on=0 "
            "same-alias-request-word=true preserved-other-fields=true actual-original-BIC80=true "
            "real-transition-pending=true no-immediate-completion=true");
        return marconi_pm_domain5_refresh(s);
    }
    if((s->pm_domain4_profile&&(address==0x4a0c0030||address==0x4a0c000c))||
       (s->pm_domain5_profile&&address==0x4a0c0034)) {
        if(write) {
            uint64_t pc=marconi_mmio_pc();
            bool set=marconi_lps_pm_caller(s,0x32006276,0x32005b57);
            bool clear=marconi_lps_pm_caller(s,0x32006276,0x32005b8d)||
                (pc==0x60c5556a&&marconi_ap_te_caller(s));
            uint32_t next=set?s->pm_domain4.guard_word|UINT32_C(2):
                              s->pm_domain4.guard_word&~UINT32_C(2);
            if(size!=4||address!=0x4a0c000c||(!set&&!clear)||*value!=next)return false;
            s->pm_domain4.guard_word=next;
            marconi_public_report("MARCONI_PM_DOMAIN_GUARD_WRITE set-bit1=%u actual-original-RMW=true "
                "same-canonical-guard=true no-domain-state-change=true",set);
            return true;
        }
        if(size!=4)return false;
        bool fifth=address==0x4a0c0034;
        if(!(fifth?marconi_pm_domain5_refresh(s):marconi_pm_domain4_refresh(s)))return false;
        O62MPMDomain4V1 *domain=fifth?&s->pm_domain5:&s->pm_domain4;
        /* Guard000c is one canonical opaque word, not a pair of ON mirrors. */
        *value=address==0x4a0c000c?s->pm_domain4.guard_word:o62m_pm_domain4_status_v1(domain);
        marconi_public_report("MARCONI_PM_DOMAIN_QUERY logical-domain=%u kind=%s model-domain-on=%u available=%u "
                              "status-and-single-guard-independent=true explicit-virtual-policy=true raw-values-redacted=true",
                              fifth?5:4,address==0x4a0c000c?"guard":"status",domain->on,domain->available);
        return true;
    }
    if(s->dpu_initial_b0&&(address==0x40302138||address==0x4030213c)) {
        uint64_t pc=marconi_mmio_pc();
        if(size!=4||!marconi_ap_te_caller(s))return false;
        if(address==0x40302138) {
            if(write||(pc!=0x60c4558c&&pc!=0x60c3e10c))return false;
            *value=s->dpu_irq_pending;
            if(s->dpu_irq_pending&1){s->dpu_irq_pending&=~1u;++s->dpu_frame_status_acks;marconi_dpu_frame_irq(s);}
            marconi_public_report("MARCONI_DPU_IRQ_STATUS empty=%u actual-completion-latch=true virtual-read-clear-bit0=true",*value==0);
            return true;
        }
        static const uint32_t set_reads[5]={0x60da7bae,0x60da7bba,0x60da7bc6,0x60da7bd2,0x60da7bde};
        static const uint32_t set_writes[5]={0x60da7bb6,0x60da7bc2,0x60da7bce,0x60da7bda,0x60da7be6};
        static const uint32_t clear_reads[5]={0x60da7bf6,0x60da7c02,0x60da7c0e,0x60da7c1a,0x60da7c26};
        static const uint32_t clear_writes[5]={0x60da7bfe,0x60da7c0a,0x60da7c16,0x60da7c22,0x60da7c2e};
        static const uint32_t masks[5]={1,0x1000,0x20000000,0x40000000,0x80000000};
        for(unsigned i=0;i<5;i++) {
            if(!write&&(pc==set_reads[i]||pc==clear_reads[i])){*value=s->dpu_irq_mask;return true;}
            if(write&&(pc==set_writes[i]||pc==clear_writes[i])) {
                uint32_t next=pc==set_writes[i]?s->dpu_irq_mask|masks[i]:s->dpu_irq_mask&~masks[i];
                if(*value!=next)return false;
                s->dpu_irq_mask=next;marconi_dpu_frame_irq(s);
                marconi_public_report("MARCONI_DPU_IRQ_MASK field-index=%u enable=%u actual-RMW=true no-pending-or-IRQ-created=true",i,pc==set_writes[i]);
                return true;
            }
        }
        return false;
    }
    if(s->dpu_initial_b0&&address>=0x40302000&&address<0x40302100) {
        uint64_t pc=marconi_mmio_pc();
        if(address==0x403020f4) {
            if(!marconi_ap_te_caller(s)||size!=4||!write||*value!=1||pc!=0x60c37796)return false;
            return marconi_dpu_frame_submit(s);
        }
        static const uint16_t offsets[]={0x24,0x28,0x2c,0x6c,0x34,0x38,0x3c};
        static const uint32_t masks[]={0xe0007,0xffffffff,0x1ffff,0x0fff0fff,7,0xffffffff,0xffff};
        for(unsigned i=0;i<ARRAY_SIZE(offsets);i++)if(address==0x40302000+offsets[i]&&pc>=0x60c3e3ba&&pc<=0x60c3e44e) {
            if(!marconi_ap_te_caller(s)||size!=4)return false;
            uint32_t *word=i?&s->dpu_channel_config[i-1]:&s->dpu_boot_config[1];
            if(write){if(*value>UINT32_MAX||((*value^*word)&~masks[i])||s->dpu_frame_pending)return false;
                *word=(uint32_t)*value;if(i)s->dpu_channel_written|=1u<<(i-1);
            }else *value=*word;
            return true;
        }
    }
    if(s->dpu_initial_b0&&address>=0x40302000&&address<0x40302200) {
        static const struct {uint32_t pc;uint16_t offset;uint8_t index;uint32_t mask;unsigned operation;} calls[]={
            {0x60c49cd6,0xec,0,0x0,3},
            {0x60c49cde,0xec,0,0x1,1},
            {0x60c49cee,0xec,0,0x0,3},
            {0x60c49cf6,0xec,0,0x1,0},
            {0x60c39284,0x24,1,0x0,3},
            {0x60c3928e,0x24,1,0x8,1},
            {0x60c392a6,0xe8,2,0x0,3},
            {0x60c392b2,0xe8,2,0x0,3},
            {0x60c39360,0xe8,2,0x0,3},
            {0x60c3936c,0xe8,2,0x0,3},
            {0x60c39378,0xe8,2,0x0,3},
            {0x60c393d6,0xe8,2,0x0,3},
            {0x60c393e4,0xe8,2,0x0,3},
            {0x60c392ae,0xe8,2,0x100,1},
            {0x60c392ba,0xe8,2,0x1,1},
            {0x60c39368,0xe8,2,0x2,0},
            {0x60c39374,0xe8,2,0x20,0},
            {0x60c39380,0xe8,2,0x200,0},
            {0x60c393e0,0xe8,2,0x1,1},
            {0x60c393ee,0xe8,2,0x100,1},
            {0x60c392be,0xf0,3,0x0,3},
            {0x60c393f2,0xf0,3,0x0,3},
            {0x60c392c6,0xf0,3,0x1,1},
            {0x60c393f6,0xf0,3,0x1,1},
            {0x60c393fa,0xf0,3,0x1,1},
            {0x60c39324,0x140,4,0x0,3},
            {0x60c39332,0x140,4,0x0,3},
            {0x60c3932e,0x140,4,0x4000,0},
            {0x60c3933a,0x140,4,0x40,0},
            {0x60c39342,0x154,5,0x0,3},
            {0x60c39356,0x154,5,0x0,3},
            {0x60c393fe,0x154,5,0x0,3},
            {0x60c3940a,0x154,5,0x0,3},
            {0x60c39416,0x154,5,0x0,3},
            {0x60c39352,0x154,5,0x7,0},
            {0x60c3935c,0x154,5,0x7,2},
            {0x60c39406,0x154,5,0x10,1},
            {0x60c39412,0x154,5,0x600,0},
            {0x60c3941e,0x154,5,0x600,1},
            {0x60dc094a,0x100,6,0x0,3},
            {0x60dc0956,0x100,6,0x0,3},
            {0x60dc0962,0x100,6,0x0,3},
            {0x60dc096e,0x100,6,0x0,3},
            {0x60dc0952,0x100,6,0x1fff,0},
            {0x60dc095e,0x100,6,0x1fff,2},
            {0x60dc096a,0x100,6,0x1fff0000,0},
            {0x60dc0976,0x100,6,0x1fff0000,2},
            {0x60dc097a,0x104,7,0x0,3},
            {0x60dc0986,0x104,7,0x0,3},
            {0x60dc0990,0x104,7,0x0,3},
            {0x60dc099c,0x104,7,0x0,3},
            {0x60dc09a8,0x104,7,0x0,3},
            {0x60dc09ba,0x104,7,0x0,3},
            {0x60dc0a4c,0x104,7,0x0,3},
            {0x60dc0982,0x104,7,0x1fff,0},
            {0x60dc098c,0x104,7,0x1fff,2},
            {0x60dc0998,0x104,7,0x1fff0000,0},
            {0x60dc09a4,0x104,7,0x1fff0000,2},
            {0x60dc09b0,0x104,7,0x40000000,1},
            {0x60dc09c2,0x104,7,0x80000000,2},
            {0x60dc09c6,0x110,8,0x0,3},
            {0x60dc09d4,0x110,8,0x0,3},
            {0x60dc09e0,0x110,8,0x0,3},
            {0x60dc09ec,0x110,8,0x0,3},
            {0x60dc09d0,0x110,8,0xfff,0},
            {0x60dc09dc,0x110,8,0xfff,2},
            {0x60dc09e8,0x110,8,0xfff0000,0},
            {0x60dc09f4,0x110,8,0xfff0000,2},
            {0x60dc09f8,0x114,9,0x0,3},
            {0x60dc0a04,0x114,9,0x0,3},
            {0x60dc0a0e,0x114,9,0x0,3},
            {0x60dc0a1a,0x114,9,0x0,3},
            {0x60dc0a26,0x114,9,0x0,3},
            {0x60dc0a36,0x114,9,0x0,3},
            {0x60dc0a56,0x114,9,0x0,3},
            {0x60dc0a00,0x114,9,0xfff,0},
            {0x60dc0a0a,0x114,9,0xfff,2},
            {0x60dc0a16,0x114,9,0xfff0000,0},
            {0x60dc0a22,0x114,9,0xfff0000,2},
            {0x60dc0a2e,0x114,9,0x40000000,1},
            {0x60dc0a3e,0x114,9,0x80000000,2},
        };
        uint64_t pc=marconi_mmio_pc();
        for(unsigned i=0;i<sizeof(calls)/sizeof(calls[0]);i++) {
            if(calls[i].pc!=pc||calls[i].offset!=address-0x40302000||
               (calls[i].operation!=3)!=write)continue;
            if(size!=4||!marconi_ap_te_caller(s))return false;
            uint32_t *word=&s->dpu_boot_config[calls[i].index],mask=calls[i].mask;
            if(!write){*value=*word;return true;}
            if(*value>UINT32_MAX||((*value^*word)&~mask)||
               (calls[i].operation==0&&(*value&mask))||
               (calls[i].operation==1&&(*value&mask)!=mask))return false;
            *word=(uint32_t)*value;
            marconi_public_report("MARCONI_DPU_BOOT_CONFIG public-pc=%08" PRIx64
                " word-index=%u actual-retained-config-or-enable-write=true no-submit-DMA-frame-or-IRQ=true",
                marconi_public_code_pc(pc),calls[i].index);
            return true;
        }
    }
    if(s->dpu_initial_b0&&address==0x40302164) {
        uint64_t pc=marconi_mmio_pc();
        if(size!=4||!marconi_ap_te_caller(s))return false;
        if(!write) {
            if(pc!=0x60dcbf86)return false;
            *value=s->dpu_control164;return true;
        }
        if(pc!=0x60dcbf90||*value!=(s->dpu_control164|1u))return false;
        s->dpu_control164=(uint32_t)*value;++s->dpu_control164_writes;
        marconi_public_report("MARCONI_DPU_CONTROL164_REQUEST writes=%" PRIu64
            " actual-retained-request=true unconsumed=true no-reset-done-frame-or-IRQ=true",s->dpu_control164_writes);
        return true;
    }
    if(s->dpu_initial_b0&&address==0x403020b0) {
        if(write||size!=4)return false;
        *value=s->dpu_initial_b0_word;s->dpu_initial_b0_reads++;
        marconi_public_report("MARCONI_DPU_INITIAL_B0_READ reads=%" PRIu64
                              " explicit-zero-initial-word-only=true queried-mode1-not-asserted=true "
                              "no-render-completion-frame-or-IRQ=true raw-values-redacted=true",s->dpu_initial_b0_reads);
        return true;
    }
    if(s->ap_runtime_config) {
        for(unsigned i=0;i<O62M_AP_RUNTIME_CONFIG_WORDS_V1;i++)if(address==o62m_ap_runtime_config_addresses_v1[i]) {
            if(size!=4||(write&&*value>UINT32_MAX))return false;
            if(write&&s->pm_domain4_profile&&(address==0x4a0c00a8||address==0x4a0c00ac||address==0x4a0c00b0)) {
                if(!marconi_pm_domain4_refresh(s)||!marconi_pm_domain5_refresh(s))return false;
                O62MPMDomain4V1 d4=s->pm_domain4,d5=s->pm_domain5;int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                if(!o62m_pm_domain4_note_timing_write_v1(&d4,(address-0x4a0c00a8)/4,now)||
                   (s->pm_domain5_profile&&!o62m_pm_domain4_note_timing_write_v1(&d5,(address-0x4a0c00a8)/4,now)))return false;
                s->pm_domain4=d4;if(s->pm_domain5_profile)s->pm_domain5=d5;
            }
            if(write&&s->pm_domain7_profile&&(address==0x4a0c00e4||address==0x4a0c00e8||address==0x4a0c00ec)) {
                if(!marconi_pm_domain7_refresh(s)||!o62m_pm_domain4_note_timing_write_v1(&s->pm_domain7,(address-0x4a0c00e4)/4,
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)))return false;
            }
            if(write)s->ap_runtime_words[i]=(uint32_t)*value;else *value=s->ap_runtime_words[i];
            if(write&&(address==0x4a0c0090||address==0x4a0c0094||address==0x4a0c0098)) {
                unsigned field=(address-0x4a0c0090)/4;s->pm_domain1_timing_written|=1u<<field;
                marconi_public_report("MARCONI_PM_DOMAIN1_TIMING_CAPTURE field-index=%u written-mask=%u "
                    "canonical-retained-config=true no-config-execution-or-availability=true raw-word-redacted=true",field,s->pm_domain1_timing_written);
            }
            if(write&&(address==0x4a0c0090||address==0x4a0c0094||address==0x4a0c0098))marconi_cp_cold_source_changed(s);
            s->ap_runtime_accesses++;
            marconi_public_report("MARCONI_AP_RUNTIME_CONFIG_ACCESS access=%s accesses=%" PRIu64
                                  " sole-new-word-backing=true no-status-ready-clock-output-generated=true raw-values-redacted=true",
                                  write?"write":"read",s->ap_runtime_accesses);
            return true;
        }
        if(address==0x4a0c00f0) {
            if(size!=4||!write||*value!=1)return false;
            if(s->pm_domain4_profile) {
                uint32_t timing[3]={0};
                for(unsigned j=0;j<3;j++)for(unsigned i=0;i<O62M_AP_RUNTIME_CONFIG_WORDS_V1;i++)
                    if(o62m_ap_runtime_config_addresses_v1[i]==0x4a0c00a8+4*j)timing[j]=s->ap_runtime_words[i];
                if(!marconi_pm_domain4_refresh(s)||!marconi_pm_domain5_refresh(s))return false;
                O62MPMDomain4V1 d4=s->pm_domain4,d5=s->pm_domain5;int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                if(!o62m_pm_domain4_commit_v1(&d4,timing,now)||
                   (s->pm_domain5_profile&&!o62m_pm_domain4_commit_v1(&d5,timing,now)))return false;
                s->pm_domain4=d4;if(s->pm_domain5_profile)s->pm_domain5=d5;
                if(!marconi_pm_domain4_refresh(s)||!marconi_pm_domain5_refresh(s))return false;
            }
            if(s->pm_domain7_profile) {
                uint32_t timing[3]={0};
                for(unsigned j=0;j<3;j++)for(unsigned i=0;i<O62M_AP_RUNTIME_CONFIG_WORDS_V1;i++)
                    if(o62m_ap_runtime_config_addresses_v1[i]==0x4a0c00e4+4*j)timing[j]=s->ap_runtime_words[i];
                if(!marconi_pm_domain7_refresh(s)||!o62m_pm_domain4_commit_v1(&s->pm_domain7,timing,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)))return false;
                if(!marconi_pm_domain7_refresh(s))return false;
            }
            s->ap_timing_commit_request=1;
            marconi_cp_cold_source_changed(s);
            if(s->pm_domain1_timing_written)marconi_public_report("MARCONI_PM_DOMAIN1_CONFIG_REQUEST written-mask=%u "
                "actual-shared-F0-request=true timing-recorded=true domain-transition-requires-cold-submit=true",s->pm_domain1_timing_written);
            marconi_public_report("MARCONI_AP_TIMING_CONFIG_REQUEST actual-original-request-latched=true no-completion-status-or-IRQ-generated=true");
            return true;
        }
    }
    if(s->ap_gpio_config&&address>=0x4a000058&&address<=0x4a00015c) {
        static const struct {uint32_t address,read_pc,write_pc;} routes[]={
            {0x4a000058,0x60dd21a6,0x60dd21aa},
            {0x4a000068,0x60dd21b2,0x60dd21b6},
            {0x4a000150,0x60dd2188,0x60dd218e},
            {0x4a00005c,0x60dd2212,0x60dd2216},
            {0x4a00006c,0x60dd21dc,0x60dd21e0},
            {0x4a000154,0x60dd221e,0x60dd2224},
            {0x4a000060,0x60dd2304,0x60dd2308},
            {0x4a000070,0x60dd22b4,0x60dd22b8},
            {0x4a000158,0x60dd2310,0x60dd2316},
            {0x4a000064,0x60dd227e,0x60dd2282},
            {0x4a000074,0x60dd2246,0x60dd224a},
            {0x4a00015c,0x60dd228a,0x60dd2290},
        };
        uint64_t pc=marconi_mmio_pc();
        bool cp=s->cp_created&&current_cpu==CPU(&s->cp_cpu)&&s->cp_cold.release_accepted&&
            !s->reported&&marconi_cp_cold_source(s)&&pc==(write?UINT64_C(0x6405c82e):UINT64_C(0x6405c824));
        for(unsigned i=0;i<ARRAY_SIZE(routes);i++)if(address==routes[i].address) {
            if(size!=4||(!cp&&(!marconi_ap_te_caller(s)||
                (pc!=(write?routes[i].write_pc:routes[i].read_pc)&&
                 !(!write&&i==2&&pc==UINT64_C(0x60c3799a))))))return false;
            if(write) {
                uint32_t old=s->ap_gpio_route[i],next=(uint32_t)*value,changed=old^next;
                if(*value>UINT32_MAX||(old&next)!=old||(changed&&(changed&(changed-1))))return false;
                s->ap_gpio_route[i]=next;
                if(i==2&&s->display_te_event)marconi_display_te_refresh(s);
            } else *value=s->ap_gpio_route[i];
            marconi_public_report("MARCONI_AP_GPIO_ROUTE cpu=%u access=%s word-index=%u actual-single-bit-OR-config=true canonical-routing=true no-immediate-input-edge=true",cp?1:0,write?"write":"read",i);
            return true;
        }
    }
    if(s->ap_aon_iosleep_config) {
        static const uint64_t addresses[11]={0x4c01014c,0x4c010150,0x4c010188,0x4c01018c,
            0x4c010190,0x4c010194,0x4c010198,0x4c01019c,0x4c0100a8,0x4c0100ac,0x4c0105e8};
        for(unsigned i=0;i<11;i++)if(address==addresses[i]) {
            uint64_t pc=marconi_mmio_pc();
            uint64_t *counter=write?&s->ap_aon_iosleep_writes:&s->ap_aon_iosleep_reads;
            if(*counter==UINT64_MAX)return false;
            if(i<8) {
                static const uint32_t gpio_read_pc[8]={0x60dd2196,0x60dd2188,0x60dd2202,0x60dd21f4,
                    0x60dd22dc,0x60dd22ce,0x60dd226e,0x60dd2260};
                static const uint32_t gpio_write_pc[8]={0x60dd219c,0x60dd218e,0x60dd2208,0x60dd21fa,
                    0x60dd22e2,0x60dd22d4,0x60dd2274,0x60dd2266};
                const bool gpio_caller=pc==(write?gpio_write_pc[i]:gpio_read_pc[i])||
                    (!write&&i==1&&marconi_mmio_pc()==UINT64_C(0x60c3799a));
                if(!gpio_caller&&pc!=(write?UINT64_C(0x60c3863a):UINT64_C(0x60c38634)))return false;
                if(gpio_caller&&size!=4)return false;
                if(write) {
                    uint32_t old=s->ap_aon_iosleep_words[i],next=(uint32_t)*value;
                    uint32_t changed=next^old;
                    if((next&old)!=old||(changed&&(changed&(changed-1))))return false;
                    s->ap_aon_iosleep_words[i]=next;
                    if(i==1)marconi_touch_refresh(s);
                } else *value=s->ap_aon_iosleep_words[i];
            } else {
                static const uint64_t capture_pcs[3]={0x60c3dc0a,0x60c3dc0e,0x60c3dc12};
                if(!write||pc!=capture_pcs[i-8])return false;
                if(i==10&&*value!=32)return false; /* Actual bounded-loop counter. */
                s->ap_aon_iosleep_words[i]=(uint32_t)*value;
                s->ap_aon_iosleep_capture_written[i-8]=true;
            }
            ++*counter;
            marconi_public_report("MARCONI_AP_AON_IOSLEEP_CONFIG access=%s static-word-index=%u "
                "reads=%" PRIu64 " writes=%" PRIu64
                " original-RMW-or-write-capture-only=true no-status-powerstate-ready-or-IRQ-created=true "
                "raw-values-redacted=true",write?"write":"read",i,
                s->ap_aon_iosleep_reads,s->ap_aon_iosleep_writes);
            return true;
        }
    }
    if(s->mempll_event&&(address==0x5a09d440||address==0x5a09d444||
       address==0x5a09e440||address==0x5a09e444||address==0x5a09c414||
       address==0x4a09e440||address==0x4a09e444)) {
        uint64_t pc=marconi_mmio_pc();
        CPUARMState *env=&s->armv7m.cpu->env;
        bool secure=address>=UINT64_C(0x50000000);
        bool privileged=arm_v7m_is_handler_mode(env)||!(env->v7m.control[env->v7m.secure]&1);
        if(size!=4||(write&&*value>UINT32_MAX)||
           (secure?(!env->v7m.secure||!privileged):!marconi_ap_te_caller(s)))return false;
        bool source=marconi_mempll_source(s);
        if(!source)marconi_mempll_cancel(s);
        if(address==0x5a09c414) {
            if(write||pc!=0x73e5bc3a)return false;
            *value=s->mempll_vote.locked;
            marconi_public_report("MARCONI_MEMPLL_LOCK_READ qualified=%u pending=%u generation=%" PRIu64
                " derived-current-state=true no-peer-state=true",s->mempll_vote.locked,
                s->mempll_vote.pending,s->mempll_vote.generation);
            return true;
        }
        uint64_t expected=address==0x5a09d440?0x73e5bbb0:address==0x5a09d444?0x73e5bbb4:
            address==0x5a09e444?0x73e5bbf4:address==0x5a09e440?0x73e5bbf8:
            address==0x4a09e444?0x60c55598:0x60c5559c;
        if(!write||pc!=expected||(!secure&&*value!=0x1ffe))return false;
        unsigned index=(address&4)?1:0;
        bool set=address==0x5a09d440||address==0x5a09d444;
        O62MMemPllVoteV1 trial=s->mempll_vote;
        if(!o62m_mempll_write_v1(&trial,index,set,(uint32_t)*value,
                                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),source))return false;
        s->mempll_vote=trial;
        if(trial.pending){s->mempll_armed=trial.generation;timer_mod_ns(s->mempll_event,trial.deadline);}
        else timer_del(s->mempll_event);
        marconi_public_report("MARCONI_MEMPLL_VOTE_WRITE word-index=%u set=%u writes=%" PRIu64
            " requests=%" PRIu64 " pending=%u actual-SET-CLEAR-single-owner=true raw-mask-redacted=true",
            index,set,trial.writes,trial.requests,trial.pending);
        return true;
    }
    if(s->cp_created&&(address==0x4c030100||address==0x4c0300d8||address==0x4c030108)) {
        uint64_t pc=marconi_mmio_pc();
        if(current_cpu!=CPU(&s->cp_cpu)||size!=4||!write||s->reported||
           !s->cp_cold.release_accepted||s->cp_cold.phase||!marconi_cp_cold_source(s))return false;
        if(address==0x4c030100) {
            if(pc!=0x640d3a5c||*value!=0)return false;
            s->cp_cold.request_control=0;
        } else if(address==0x4c0300d8) {
            if(pc!=0x641a47b4||*value!=1)return false;
            s->cp_pm_runtime_request=(uint32_t)*value;
        } else {
            if(pc!=0x640d3a78||*value!=5)return false;
            s->cp_pm_runtime_config=(uint32_t)*value;
        }
        ++s->cp_pm_runtime_control_writes;
        marconi_public_report("MARCONI_CP_PM_RUNTIME_CONTROL writes=%" PRIu64 " actual-control-retained=true no-cold-job-replay=true no-event-completion-or-IRQ=true",s->cp_pm_runtime_control_writes);
        return true;
    }
    if(s->cp_created&&marconi_cp_cold_port(address))return marconi_cp_cold_access(s,address,value,size,write);
    if(s->normal_emmc_profile&&address==0x5c0101bc) {
        CPUARMState *env=&s->armv7m.cpu->env;
        bool privileged=arm_v7m_is_handler_mode(env)||!(env->v7m.control[env->v7m.secure]&1);
        if(write||size!=4||current_cpu!=CPU(s->armv7m.cpu)||
           !env->v7m.secure||!privileged||
           marconi_mmio_pc()!=0x73e61e5e)return false;
        *value=4;
        marconi_public_report("MARCONI_BOOT_VERIFY_POLICY source=explicit-virtual-board "
            "verify-gate-enabled=true readonly=true validation-result-not-synthesized=true");
        return true;
    }
    if(s->normal_emmc_profile&&address==0x4c0101b0) {
        if(write||size!=4||current_cpu!=CPU(s->armv7m.cpu)||
           marconi_mmio_pc()!=0x60c5ebe6)return false;
        *value=0;
        marconi_public_report("MARCONI_BOOT_STORAGE_SELECTION source=explicit-normal-emmc-profile "
            "selector=0 readonly=true physical-strap-dump=false no-work-completion=true");
        return true;
    }
    if(s->ap_board_config&&(address==0x4c01009c||address==0x4c010120||address==0x4c010114||address==0x4c0101e8||address==0x4a09c600||address==0x40050208||address==0x4c01013c||address==0x4c010148||address==0x4c010130||address==0x4c01010c||address==0x4c010124||address==0x4c01012c||address==0x4c010134)) {
        unsigned index=address==0x4c01009c?0:address==0x4c010120?1:address==0x4c010114?2:address==0x4c0101e8?3:address==0x4a09c600?4:address==0x40050208?5:address==0x4c01013c?6:address==0x4c010148?7:address==0x4c010130?8:address==0x4c01010c?9:address==0x4c010124?10:address==0x4c01012c?11:12;
        if(index>=11&&(!s->counter32k||!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||
            !s->cp_cold.release_accepted||!marconi_cp_cold_source(s)))return false;
        if(index==10) {
            uint64_t policy_pc=marconi_mmio_pc();
            if(write) {
                if(policy_pc!=0x60c835f8)return false;
            } else {
                bool cp_policy=s->cp_created&&current_cpu==CPU(&s->cp_cpu)&&marconi_mmio_pc()==0x641a4400;
                if((policy_pc!=0x60e0ecbc&&!cp_policy)||!s->ap_board_power_policy_written)return false;
            }
        }
        if(index>=5&&index<=8&&!s->ap_board_idle_config_present)return false;
        if(index>=6&&index<=8&&!write&&!(s->ap_board_idle_written&(1u<<(index-6))))return false;
        if(index==9&&!s->ap_board_boot_reason_present)return false;
        if(index==4&&!s->ap_board_usb_clock_present)return false;
        if(index==3&&!s->ap_board_usb_config_present)return false;
        if(index==2&&!write&&!s->ap_board_wdt_written)return false;
        if(size!=4||(write&&*value>UINT32_MAX))return false;
        if(write) {
            s->ap_board_words[index]=(uint32_t)*value;
            if(index==2)s->ap_board_wdt_written=true;
            if(index==10)s->ap_board_power_policy_written=true;
            if(index>=6&&index<=8)s->ap_board_idle_written|=1u<<(index-6);
        } else *value=s->ap_board_words[index];
        s->ap_board_accesses++;
        if(index>=11) {if(write)++s->cp_pm_bookkeeping_writes;else ++s->cp_pm_bookkeeping_reads;}
        uint64_t pc=marconi_mmio_pc();
        if(s->ap_board_accesses<=64)marconi_public_report("MARCONI_AP_BOARD_CONFIG_ACCESS access=%s public-pc=%08" PRIx64 " accesses=%" PRIu64
                              " exact-selected-board-word-backing=true no-alias-status-or-ready-inferred=true raw-values-redacted=true",
                              write?"write":"read",marconi_public_code_pc(pc),s->ap_board_accesses);
        return true;
    }
    if(s->mipi_phy_pll_event&&address==0x40e00094) {
        uint64_t pc=marconi_mmio_pc();
        if(size!=4||!marconi_ap_te_caller(s))return false;
        if(!write) {
            if(pc!=0x60c396e4)return false;
            *value=s->mipi_clock_request;return true;
        }
        if(pc!=0x60c396ec||*value!=(uint64_t)(s->mipi_clock_request|1u)||
           s->mipi_phy.pll_locked||s->mipi_phy.lanes_active||
           s->mipi_phy.pll_pending||s->mipi_phy.ulps_pending||!s->mipi_phy.ulps_entries)return false;
        s->mipi_clock_request=(uint32_t)*value;++s->mipi_clock_request_writes;
        marconi_public_report("MARCONI_MIPI_HS_CLOCK_REQUEST writes=%" PRIu64
            " retained-request=true PHY-in-ULPS=true PLL-off=true HS-active=false no-source-or-lane-state-change=true",
            s->mipi_clock_request_writes);
        return true;
    }
    if(s->mipi_phy_pll_event&&address>=0x40e00000&&address<0x40e00200) {
        static const struct {uint32_t pc,offset,index,mask;unsigned operation;} calls[]={
            {0x60c394f0,0xc,0,0x0,3},
            {0x60c394fa,0xc,0,0x0,3},
            {0x60c394f8,0xc,0,0x3,0},
            {0x60c39500,0xc,0,0x3,2},
            {0x60c39502,0x10,1,0x0,3},
            {0x60c3950c,0x10,1,0x0,3},
            {0x60c3950a,0x10,1,0xf,0},
            {0x60c39510,0x10,1,0xf,2},
            {0x60c395a6,0x14,2,0x0,3},
            {0x60c395ba,0x14,2,0x0,3},
            {0x60c395d0,0x14,2,0x0,3},
            {0x60c395e4,0x14,2,0x0,3},
            {0x60c395fa,0x14,2,0x0,3},
            {0x60c39522,0x48,3,0x0,3},
            {0x60c3952a,0x48,3,0x0,3},
            {0x60c39528,0x48,3,0xfff,0},
            {0x60c3952e,0x48,3,0xfff,2},
            {0x60c39530,0x4c,4,0x0,3},
            {0x60c39538,0x4c,4,0x0,3},
            {0x60c39536,0x4c,4,0xfff,0},
            {0x60c39540,0x4c,4,0xfff,2},
            {0x60c39550,0x50,5,0x0,3},
            {0x60c3955c,0x50,5,0x0,3},
            {0x60c3955a,0x50,5,0x7fff,0},
            {0x60c39560,0x50,5,0x7fff,2},
            {0x60c39562,0x54,6,0x0,3},
            {0x60c3956a,0x54,6,0x0,3},
            {0x60c39568,0x54,6,0x3ff,0},
            {0x60c3956e,0x54,6,0x3ff,2},
            {0x60c39570,0x58,7,0x0,3},
            {0x60c39578,0x58,7,0x0,3},
            {0x60c39576,0x58,7,0x3ff,0},
            {0x60c3957c,0x58,7,0x3ff,2},
            {0x60c3957e,0x5c,8,0x0,3},
            {0x60c39586,0x5c,8,0x0,3},
            {0x60c39584,0x5c,8,0x3ff,0},
            {0x60c3958a,0x5c,8,0x3ff,2},
            {0x60c3958c,0x60,9,0x0,3},
            {0x60c3959a,0x60,9,0x0,3},
            {0x60c39598,0x60,9,0x3fff,0},
            {0x60c3959e,0x60,9,0x3fff,2},
            {0x60c39512,0x64,10,0x0,3},
            {0x60c3951c,0x64,10,0x0,3},
            {0x60c3951a,0x64,10,0xffff,0},
            {0x60c39520,0x64,10,0xffff,2},
            {0x60c3960e,0x11c,11,0x0,3},
            {0x60c39712,0x11c,11,0x0,3},
            {0x60c39744,0xe0,12,0x0,3},
            {0x60c395b2,0x14,2,0x1,2},
            {0x60c395c6,0x14,2,0x2,2},
            {0x60c395dc,0x14,2,0x4,2},
            {0x60c395f0,0x14,2,0x8,2},
            {0x60c39606,0x14,2,0x10,2},
            {0x60c39708,0x11c,11,0x1,0},
            {0x60c39630,0x11c,11,0x10,0},
            {0x60c3974c,0xe0,12,0x1,0},
        };
        uint64_t pc=marconi_mmio_pc();
        for(unsigned i=0;i<ARRAY_SIZE(calls);i++) {
            if(calls[i].pc!=pc||calls[i].offset!=address-0x40e00000||
               (calls[i].operation!=3)!=write)continue;
            if(size!=4||!marconi_ap_te_caller(s))return false;
            if(!marconi_mipi_phy_sources(s)){marconi_mipi_phy_invalidate(s);return false;}
            uint32_t *word=&s->mipi_boot_config[calls[i].index],mask=calls[i].mask;
            if(!write){*value=*word;return true;}
            if(*value>UINT32_MAX||((*value^*word)&~mask)||
               (calls[i].operation==0&&(*value&mask)))return false;
            *word=(uint32_t)*value;
            marconi_public_report("MARCONI_MIPI_BOOT_CONFIG public-pc=%08" PRIx64
                " word-index=%u actual-retained-write=true no-packet-frame-clock-request-or-IRQ=true",
                marconi_public_code_pc(pc),calls[i].index);
            return true;
        }
    }
    if(s->display_short_event&&(address==0x40e0006c||address==0x40e00074)) {
        uint64_t pc=marconi_mmio_pc();
        if(size!=4||!marconi_ap_te_caller(s))return false;
        if(!marconi_display_short_eligible(s)){marconi_display_short_cancel(s);return false;}
        if(!write) {
            if(address!=0x40e00074||(pc!=0x60c4ab30&&pc!=0x60c4ab56&&pc!=0x60c4ab5c&&pc!=0x60c4ac3e&&pc!=0x60c4ac66&&pc!=0x60c4ac98&&pc!=0x60c4acb8&&pc!=0x60c4ace8))return false;
            uint32_t status;if(!o62m_display_short_status_v3(&s->display_short,&status))return false;
            *value=status;++s->display_short_status_reads;
            if(s->display_short_status_reads<=8)marconi_public_report("MARCONI_DISPLAY_SHORT_STATUS pending=%u read-pending=%u reads=%" PRIu64
                " queue-projection=true no-read-driven-consumption=true",s->display_short.pending,s->display_short.read_pending,s->display_short_status_reads);
            return true;
        }
        if(address!=0x40e0006c)return false;
        unsigned type=*value&0x3f;
        if((pc!=0x60c4ab52||(type!=0x37&&!o62m_display_short_write_type_v3(type)))&&
           (pc!=0x60c4ac60||type!=0x14)) {
            marconi_public_report("MARCONI_DISPLAY_PACKET_BOUNDARY data-type=%02x known-caller=%u payload-redacted=true",type,pc==0x60c4ab52||pc==0x60c4ac60);return false;
        }
        if(type==0x14&&((*value>>6)&3)!=(s->mipi_receive_vcid&3))return false;
        O62MDisplayShortV3 trial=s->display_short;
        int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if(!o62m_display_short_submit_v3(&trial,*value,now,s->mipi_phy.pll_generation,
            s->mipi_phy.ulps_generation,true,
            (s->mipi_cmd_mode&(1u<<o62m_display_short_lp_bit_v3(type)))!=0,(s->mipi_cmd_mode&2)!=0))return false;
        s->display_short=trial;s->display_short_armed=trial.generation;
        timer_mod_ns(s->display_short_event,trial.deadline_ns);
        marconi_public_report("MARCONI_DISPLAY_SHORT_REQUEST kind=%s generation=%" PRIu64
            " request=%u virtual-ns=%" PRId64 " deadline-ns=%" PRId64 " complete-header-retained=true panel-ACK=false",
            type==0x14?"generic-read-one-parameter":type==0x37?"set-maximum-return-packet-size":"short-write",
            trial.generation,trial.history_count,now,trial.deadline_ns);
        return true;
    }
    if(s->mipi_phy_pll_event&&(address==0x40e00068||address==0x40e0002c||address==0x40e00034||address==0x40e00030)) {
        uint64_t pc=marconi_mmio_pc();
        if(size!=4||!marconi_ap_te_caller(s))return false;
        if(!marconi_mipi_phy_sources(s)){marconi_mipi_phy_invalidate(s);return false;}
        uint32_t *word=address==0x40e00068?&s->mipi_cmd_mode:
            address==0x40e0002c?&s->mipi_packet_config:
            address==0x40e00034?&s->mipi_mode_config:&s->mipi_receive_vcid;
        if(!write) {
            bool matched=(address==0x40e0002c&&pc==0x60c49d5e)||
                (address==0x40e00034&&(pc==0x60c4aafc||pc==0x60c4ac22||pc==0x60c394e2))||
                (address==0x40e00030&&(pc==0x60c4ac2e||pc==0x60c4ac38));
            if(address==0x40e00068) {
                static const uint32_t reads[]={0x60c4ab0a,0x60c36c6e,0x60c36cde,0x60c36ce8,0x60c36cf2,0x60c36db0,0x60c36dba,0x60c36dc4,0x60c36dce,0x60c36eae,0x60c36eb4,0x60c36ee8,0x60c49d72,0x60c49d86,0x60c49d8e,0x60c49d96,0x60c49d9e,0x60c49db2,0x60c49dba,0x60c49dc2,0x60c49dd6,0x60c49dde,0x60c49de6,0x60dc05a4,0x60dc05ac,0x60dc05b4,0x60dc05d4,0x60dc05dc,0x60dc05e4,0x60dc05ec};
                for(unsigned i=0;i<sizeof(reads)/sizeof(reads[0]);i++)if(pc==reads[i])matched=true;
            }
            if(!matched)return false;
            *value=*word;return true;
        }
        uint32_t mask=0;unsigned operation=2;
        if(address==0x40e0002c) {if(pc!=0x60c49d64)return false;mask=4;operation=1;}
        else if(address==0x40e00034) {if(pc!=0x60c4ab06&&pc!=0x60c4ac28&&pc!=0x60c394ea)return false;mask=1;operation=1;}
        else if(address==0x40e00030) {
            if(pc==0x60c4ac36){mask=3;operation=0;}
            else if(pc==0x60c4ac3c){mask=3;operation=2;}
            else return false;
        }
        else {
            static const struct {uint32_t pc,mask;unsigned operation;} writes[]={
                {0x60c4ab12,0x1,1},
                {0x60c36c76,0x2,1},
                {0x60c36cba,0x2,0},
                {0x60c36ec4,0x1000000,2},
                {0x60c36ef0,0x1000000,2},
                {0x60c49d78,0x40000,1},
                {0x60c49d8c,0x100,1},
                {0x60c49d94,0x200,1},
                {0x60c49d9c,0x400,1},
                {0x60c49da4,0x4000,1},
                {0x60c49db8,0x800,1},
                {0x60c49dc0,0x1000,1},
                {0x60c49dc8,0x2000,1},
                {0x60c49ddc,0x10000,1},
                {0x60c49de4,0x20000,1},
                {0x60c49dec,0x80000,1},
                {0x60dc05aa,0x10000,0},
                {0x60dc05b2,0x20000,0},
                {0x60dc05ba,0x80000,0},
                {0x60dc05da,0x100,0},
                {0x60dc05e2,0x200,0},
                {0x60dc05ea,0x400,0},
                {0x60dc05f2,0x4000,0},
            };
            for(unsigned i=0;i<sizeof(writes)/sizeof(writes[0]);i++)if(pc==writes[i].pc) {
                mask=writes[i].mask;operation=writes[i].operation;break;
            }
        }
        if(!mask||((*value^*word)&~mask)||(operation==1&&(*value&mask)!=mask)||
           (operation==0&&(*value&mask)))return false;
        *word=*value;
        if((s->display_short.pending||s->display_short.read_pending)&&!marconi_display_short_eligible(s))marconi_display_short_cancel(s);
        marconi_public_report("MARCONI_MIPI_MODE_CONFIG public-pc=%08" PRIx64
            " actual-retained-write=true no-packet-or-FIFO-work=true",marconi_public_code_pc(pc));
        return true;
    }
    if(s->mipi_phy_pll_event&&address>=0x40e00000&&address<0x40e00100) {
        static const struct {uint32_t pc;uint8_t offset;bool write;} calls[]={
            {0x60c38e4e,0xb0,false},
            {0x60c4a924,0xa0,false},
            {0x60c4a92e,0xa0,true},
            {0x60c4a938,0xa8,false},
            {0x60c4a944,0xa8,false},
            {0x60c4a940,0xa8,true},
            {0x60c4a94c,0xa8,true},
            {0x60c4a950,0xa4,false},
            {0x60c4a972,0xb0,false},
            {0x60c4a9b6,0xb0,false},
            {0x60c4a9fc,0xa0,false},
            {0x60c4aada,0xa0,false},
            {0x60c4aa04,0xa0,true},
            {0x60c4aae4,0xa0,true},
            {0x60c4aa8e,0xa4,false},
            {0x60c4aa2a,0xa8,false},
            {0x60c4aa36,0xa8,false},
            {0x60c4aa42,0xa8,false},
            {0x60c4aa4e,0xa8,false},
            {0x60c4aa5e,0xa8,false},
            {0x60c4aa6a,0xa8,false},
            {0x60c4aa76,0xa8,false},
            {0x60c4aa82,0xa8,false},
            {0x60c4aa32,0xa8,true},
            {0x60c4aa3e,0xa8,true},
            {0x60c4aa4a,0xa8,true},
            {0x60c4aa56,0xa8,true},
            {0x60c4aa66,0xa8,true},
            {0x60c4aa72,0xa8,true},
            {0x60c4aa7e,0xa8,true},
            {0x60c4aa8a,0xa8,true},
            {0x60dc1012,0xb0,false},
            {0x60c4a9e6,0xb0,false},
            {0x60c4aa08,0xb0,false},
            {0x60c4aaac,0xb0,false},
            {0x60db4e04,0xb4,false},
            {0x60db4e28,0xb4,false},
            {0x60db4e4c,0xb4,false},
            {0x60db4e5e,0xb4,false},
            {0x60db4e0e,0xb4,true},
            {0x60db4e32,0xb4,true},
            {0x60db4e56,0xb4,true},
            {0x60db4e66,0xb4,true},
            {0x60c49d3a,0xb8,false},
            {0x60c49d46,0xb8,false},
            {0x60db4e16,0xb8,false},
            {0x60db4e3a,0xb8,false},
            {0x60c49d42,0xb8,true},
            {0x60c49d4c,0xb8,true},
            {0x60db4e20,0xb8,true},
            {0x60db4e42,0xb8,true},
        };
        uint64_t pc=marconi_mmio_pc();
        unsigned off=address-0x40e00000;bool matched=false;
        for(unsigned i=0;i<sizeof(calls)/sizeof(calls[0]);i++)
            if(calls[i].pc==pc&&calls[i].offset==off&&calls[i].write==write)matched=true;
        if(size!=4||!matched||!marconi_ap_te_caller(s))return false;
        if(!marconi_mipi_phy_sources(s)){marconi_mipi_phy_invalidate(s);return false;}
        int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if(s->mipi_phy.pll_pending&&now>=s->mipi_phy.pll_deadline_ns){
            timer_del(s->mipi_phy_pll_event);marconi_mipi_phy_pll_callback(s);
        }
        if(s->mipi_phy.ulps_pending&&now>=s->mipi_phy.ulps_deadline_ns){
            timer_del(s->mipi_phy_ulps_event);marconi_mipi_phy_ulps_callback(s);
        }
        if(s->reported)return false;
        if(!write) {
            uint32_t data;if(!o62m_mipi_read_v2(&s->mipi_phy,off,&data))return false;
            *value=data;
            if(off==0xb0)marconi_public_report("MARCONI_MIPI_PHY_STATUS PLL-locked=%u lanes-active=%u inert-projection=true no-read-trigger=true",
                s->mipi_phy.pll_locked,s->mipi_phy.lanes_active);
            return true;
        }
        if(off==0xa8&&s->mipi_phy.lanes_active&&
           (s->display_short.pending||s->display_short.read_pending))return false;
        O62MMipiUlpsV2 trial=s->mipi_phy;
        if(!o62m_mipi_write_v2(&trial,off,*value,now,true)){
            marconi_public_report("MARCONI_MIPI_ACCESS_REJECT offset=%02x pll-pending=%u ulps-pending=%u ulps-qualified=%u remaining-ns=%" PRId64,
                off,s->mipi_phy.pll_pending,s->mipi_phy.ulps_pending,s->mipi_phy.ulps_qualified,
                s->mipi_phy.ulps_pending?s->mipi_phy.ulps_deadline_ns-now:0);
            return false;
        }
        bool pll_start=trial.pll_pending&&trial.pll_generation!=s->mipi_phy.pll_generation;
        bool ulps_start=trial.ulps_pending&&trial.ulps_generation!=s->mipi_phy.ulps_generation;
        bool test_commit=trial.test_commits!=s->mipi_phy.test_commits;
        bool exited=trial.ulps_exits!=s->mipi_phy.ulps_exits;
        s->mipi_phy=trial;
        if(test_commit)marconi_public_report("MARCONI_MIPI_PHY_TEST_COMMIT commits=%" PRIu64
            " actual-address-and-data-clock-pulse=true no-lock-produced=true",trial.test_commits);
        if(pll_start) {
            s->mipi_phy_pll_armed=trial.pll_generation;timer_mod_ns(s->mipi_phy_pll_event,trial.pll_deadline_ns);
            marconi_public_report("MARCONI_MIPI_PHY_PLL_REQUEST generation=%" PRIu64 " virtual-ns=%" PRId64
                " deadline-ns=%" PRId64 " actual-FORCEPLL=true no-immediate-lock=true",trial.pll_generation,now,trial.pll_deadline_ns);
        }
        if(ulps_start) {
            s->mipi_phy_ulps_armed=trial.ulps_generation;timer_mod_ns(s->mipi_phy_ulps_event,trial.ulps_deadline_ns);
            if(trial.ulps_entering)marconi_public_report("MARCONI_MIPI_PHY_ULPS_ENTRY_REQUEST generation=%" PRIu64
                " virtual-ns=%" PRId64 " deadline-ns=%" PRId64 " actual-clock-and-data-entry-requests=true packet-work-empty=true",
                trial.ulps_generation,now,trial.ulps_deadline_ns);
            else marconi_public_report("MARCONI_MIPI_PHY_ULPS_REQUEST generation=%" PRIu64 " virtual-ns=%" PRId64
                " deadline-ns=%" PRId64 " actual-clock-and-data-exit-requests=true no-immediate-active-not=true",
                trial.ulps_generation,now,trial.ulps_deadline_ns);
        }
        if(exited)marconi_public_report("MARCONI_MIPI_PHY_ULPS_EXIT exits=%" PRIu64
            " actual-qualified-request-release=true one-lane-virtual-profile=true no-pixels-or-packets=true",trial.ulps_exits);
        return true;
    }
    if(s->sensys_config_event&&(address==0x4c010030||address==0x4c010048)) {
        uint64_t pc=marconi_mmio_pc();
        if(size!=4||!marconi_ap_te_caller(s))return false;
        if(address==0x4c010030) {
            if(!write) {
                if(pc!=0x60c30de0&&pc!=0x60c30e36)return false;
                *value=s->sensys_config.desired;
            } else {
                if(pc!=0x60c30dea&&pc!=0x60c30e3e)return false;
                O62MSensysConfigV1 trial=s->sensys_config;
                int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                if(!o62m_sensys_config_request_v1(&trial,*value,pc==0x60c30e3e,now))return false;
                s->sensys_config=trial;s->sensys_config_armed_generation=trial.generation;
                timer_mod_ns(s->sensys_config_event,trial.deadline_ns);
                marconi_public_report("MARCONI_SENSYS_CONFIG_REQUEST generation=%" PRIu64
                    " requests=%" PRIu64 " replaced=%" PRIu64 " virtual-ns=%" PRId64
                    " deadline-ns=%" PRId64 " actual-retained-write=true peer-absent=true no-completion-yet=true",
                    trial.generation,trial.requests,trial.replaced,now,trial.deadline_ns);
            }
        } else {
            uint32_t status;
            if(write||pc!=0x60c30df2||!o62m_sensys_config_status_v1(&s->sensys_config,&status))return false;
            *value=status;
            marconi_public_report("MARCONI_SENSYS_CONFIG_STATUS generation=%" PRIu64
                " pending=%u valid=%u peer-absent=true inert-projection=true no-peer-boot-ACK=true",
                s->sensys_config.generation,s->sensys_config.pending,s->sensys_config.valid);
        }
        return true;
    }
    if(s->ap_runtime_config&&(address==0x4a000008||address==0x4a000100||address==0x4a000104||
       address==0x5a000100||address==0x5a000104)) {
        if(s->xipc_config.pending&&qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)>=s->xipc_config.deadline){
            timer_del(s->xipc_config_event);marconi_xipc_config_callback(s);
        }
        if(s->reported)return false;
        uint64_t pc=marconi_mmio_pc();
        bool secure_view=address==0x5a000100||address==0x5a000104;
        hwaddr word=secure_view?address-UINT64_C(0x10000000):address;
        uint32_t secure_lr=word==0x4a000008?0x73e59ac1:word==0x4a000100?0x73e59ad7:0x73e59ae5;
        bool secure=marconi_lps_pm_caller(s,write?0x32006276:0x32006268,secure_lr);
        bool ap=!secure_view&&marconi_ap_te_caller(s);
        bool cp=s->cp_created&&current_cpu==CPU(&s->cp_cpu)&&s->cp_cold.release_accepted&&address==0x4a000008&&!write&&pc==0xb4;
        if(size!=4||(!secure&&!ap&&!cp)||(secure&&word!=0x4a000008&&!secure_view))return false;
        if(word==0x4a000008) {
            if(write||(!secure&&!cp&&pc!=0x60c30c96))return false;
            *value=(uint32_t)s->xipc_config.active<<9;
        } else if(word==0x4a000100) {
            uint32_t required=secure?0xau:0x10u;
            if(!write||(!secure&&pc!=0x60c30ca4)||(*value&~UINT64_C(31))||(*value&required)!=required||
               s->xipc_cleanup_selector_written||s->xipc_cleanup_submissions==ARRAY_SIZE(s->xipc_cleanup_history))return false;
            s->xipc_cleanup_selector=(uint32_t)*value;s->xipc_cleanup_selector_written=true;
            s->xipc_cleanup_selector_secure=secure;
        } else {
            if(!write||(!secure&&pc!=0x60c30cb0)||*value!=1||!s->xipc_cleanup_selector_written||
               secure!=s->xipc_cleanup_selector_secure||s->xipc_cleanup_submissions==ARRAY_SIZE(s->xipc_cleanup_history))return false;
            O62MXipcConfigV1 trial=s->xipc_config;
            if(!o62m_xipc_config_submit_v1(&trial,s->xipc_cleanup_selector,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)))return false;
            s->xipc_cleanup_history[s->xipc_cleanup_submissions].selector=s->xipc_cleanup_selector;
            s->xipc_cleanup_history[s->xipc_cleanup_submissions++].secure=secure;
            s->xipc_config=trial;s->xipc_config_armed=trial.generation;
            timer_mod_ns(s->xipc_config_event,trial.deadline);
            s->xipc_cleanup_selector_written=false;
            o62m_virtual_otp_wipe_v1(&s->xipc_cleanup_selector,sizeof(s->xipc_cleanup_selector));
            marconi_public_report("MARCONI_XIPC_CLEANUP_SUBMISSION requests=%" PRIu64 " secure=%u "
                "actual-selector-and-submit-writes=true config-service-ns=500 completion-pending=true no-peer-ACK-or-IRQ=true",
                s->xipc_cleanup_submissions,secure);
        }
        return true;
    }
    if(s->pm_functional_profile&&address>=0x4c030000&&address<0x4c030800) {
        if(address==0x4c03036c||address==0x4c03066c) {
            bool set=address==0x4c03036c;
            bool lps=marconi_lps_pm_caller(s,0x32006276,set?0x32005b61:0x32005b97);
            bool ap=!set&&marconi_ap_te_caller(s)&&
                marconi_mmio_pc()==0x60c5556e;
            if((!lps&&!ap)||!write||!s->pm_domain5_profile||
               !marconi_pm_domain5_refresh(s))return false;
            O62MPMFunctionalV1 config=s->pm_functional;
            O62MPMDomain4V1 domain=s->pm_domain5;
            if(!o62m_pm_functional_access_v1(&config,address,size,true,value)||
               (set&&!o62m_pm_domain5_on_after_config_v2(&domain,
                                                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))))return false;
            s->pm_functional=config;
            if(set)s->pm_domain5=domain;
            marconi_public_report("MARCONI_LPS_PM_CONFIG_REQUEST set=%u real-config-commit=true "
                "domain5-on-request=%u waits-for-config=%u existing-timing-lifecycle=true raw-request-word-unchanged=true "
                "no-immediate-completion-or-peer-state=true",set,set,set&&domain.transition_waits_config);
            return marconi_pm_domain5_refresh(s);
        }
        if(!o62m_pm_functional_access_v1(&s->pm_functional,address,size,write,value))return false;
        if(write)marconi_cp_cold_source_changed(s);
        uint64_t pc=marconi_mmio_pc();
        /* Public instruction position/count only; no generated guest data. */
        marconi_public_report("MARCONI_PM_FUNCTIONAL_ACCESS access=%s public-pc=%08" PRIx64 " accesses=%" PRIu64
                              " explicit-virtual-SET-CLEAR-config-policy=true real-backing-updated=true "
                              "requests-not-completion=true no-wake-IRQ-or-status-generated=true raw-values-redacted=true",
                              write?"write":"read",marconi_public_code_pc(pc),s->pm_functional.accesses);
        return true;
    }
    if (s->wdt_policy_fixture && address==0x5c010114) {
        uint64_t pc=marconi_mmio_pc();
        if(write||pc!=0x73e59494) {
            error_report("MARCONI_WDT_POLICY_REJECT access=%s pc=0x%08" PRIx64
                " original-selector-read-only=true",write?"write":"read",pc);
            return false;
        }
        *value=s->wdt_policy_word;
        error_report("MARCONI_WDT_POLICY_READ value=%08x pc=0x%08" PRIx64
            " explicit-initial-selector=true no-watchdog-status-or-reset-proof=true",s->wdt_policy_word,pc);
        return true;
    }
    if(s->aca_config_profile&&((address>=UINT64_C(0x50333000)&&address<=UINT64_C(0x5033307c))||
       (address>=UINT64_C(0x50333100)&&address<=UINT64_C(0x50333140))||
       address==UINT64_C(0x50333148)||address==UINT64_C(0x5033314c)||
       (address>=UINT64_C(0x50333150)&&address<=UINT64_C(0x50333160)))) {
        CPUARMState *env=&s->armv7m.cpu->env;uint32_t gate;
        bool privileged=arm_v7m_is_handler_mode(env)||!(env->v7m.control[env->v7m.secure]&1);
        if(current_cpu!=CPU(s->armv7m.cpu)||!env->v7m.secure||!privileged||
           s->te_context.phase!=O62M_TE_CONTEXT_READY_PHASE||size!=4||(address&3)||
           (write&&*value>UINT32_MAX)||!(s->aca_config_word[0]&UINT32_C(0x400))||
           o62m_clock_controls_read(&s->board_controls,UINT64_C(0x50050018),4,&gate)!=O62M_CLOCK_CONTROLS_OK||!(gate&2))return false;
        if(address==UINT64_C(0x50333148)) {
            if(write) {
                uint32_t command=(uint32_t)*value;
                if(o62m_aca_op_entry_v12(&s->aca_sram,command)) {
                    marconi_public_report("MARCONI_ACA_OPERATION_ENTRY opcode=%u requests=%" PRIu64 " actual-entry-queued=true writeback=%u input-blocks-zero=%u completion=false",command>>27,s->aca_sram.requests,s->aca_sram.save_result,((s->aca_sram.gr[(command>>18)&31u]>>16)&127u)==0);
                    return true;
                }
                unsigned opcode=command>>27,len_id=(command>>23)&15u;
                uint32_t bits=s->aca_sram.length[len_id];
                unsigned length_class=bits==0?0:bits<=128?1:bits<=256?2:bits<=512?3:bits<=1024?4:bits<=2048?5:bits<=4096?6:7;
                marconi_public_report("MARCONI_ACA_ENTRY_BOUNDARY opcode=%u length-class=%u B-kind=%u raw-bit11=%u need-intr=%u fifo-occupancy=%u command-not-consumed=true no-address-or-operand-output=true",
                    opcode,length_class,(opcode==8||opcode==10||opcode==11)?2u:(command>>17)&1u,(command>>11)&1u,command&1u,s->aca_sram.fifo_occupancy);
            }
            return false;
        }
        uint32_t word=write?(uint32_t)*value:0;
        if(!o62m_aca_sram_access_v12(&s->aca_sram,(unsigned)(address-UINT64_C(0x50333000)),write,&word)) {
            marconi_public_report("MARCONI_ACA_SRAM_REJECT port-offset=%03x write=%u config-field-or-cursor-or-unwritten-word=true no-address-or-operand-output=true",
                (unsigned)(address-UINT64_C(0x50333000)),write);return false;
        }
        if(!write)*value=word;
        uint64_t accesses=s->aca_sram.address_writes+s->aca_sram.data_writes+s->aca_sram.data_reads+
            s->aca_sram.config_reads+s->aca_sram.config_writes+s->aca_sram.status_reads;
        if(accesses<=16||address<UINT64_C(0x50333150))marconi_public_report("MARCONI_ACA_SRAM_ACCESS port-offset=%03x write=%u accesses=%" PRIu64
            " actual-private-word-backing=true reads-do-not-complete=true no-address-or-operand-output=true",
            (unsigned)(address-UINT64_C(0x50333000)),write,accesses);
        return true;
    }
    if(s->aca_config_profile&&(address==0x50333144||address==0x50333164||address==0x50333168)) {
        uint32_t gate;CPUARMState *env=&s->armv7m.cpu->env;
        bool privileged=arm_v7m_is_handler_mode(env)||!(env->v7m.control[env->v7m.secure]&UINT32_C(1));
        if(!env->v7m.secure||!privileged||s->te_context.phase!=O62M_TE_CONTEXT_READY_PHASE||
           o62m_clock_controls_read(&s->board_controls,0x50050018,4,&gate)!=O62M_CLOCK_CONTROLS_OK||!(gate&2)||
           current_cpu!=CPU(s->armv7m.cpu)||size!=4||(write&&*value>UINT32_MAX)) {return false;}
        unsigned index=address==0x50333144?0:address==0x50333164?1:2;
        if(write) {
            uint32_t word=(uint32_t)*value;
            if(index==0) {
                if(word&~UINT32_C(0x4f1))return false;
                if((word&0x401u)==0x401u) {
                    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                    if(now<0||s->aca_sram.fifo_occupancy!=1)return false;
                    if(!s->aca_sram.running) {
                        if(!o62m_aca_op_run_v12(&s->aca_sram,(uint64_t)now))return false;
                        s->aca_work_armed=s->aca_sram.generation;
                        timer_mod_ns(s->aca_work_event,s->aca_sram.deadline);
                    }
                } else marconi_aca_cancel(s);
            }
            s->aca_config_word[index]=word;
        }else {*value=s->aca_config_word[index];}
        uint64_t pc=marconi_mmio_pc();
        error_report("MARCONI_ACA_CONFIG_ACCESS access=%s address=%08" HWADDR_PRIx
                     " value=%08" PRIx64 " pc=%08" PRIx64
                     " actual-observed-integer-operations-only=true no-signature-result-generated=true",
                     write?"write":"read",address,*value,pc);return true;
    }
    if(s->ap_i2c_empty_config&&address>=UINT64_C(0x40150000)&&address<=UINT64_C(0x40190080)) {
        uint64_t relative=address-UINT64_C(0x40150000);
        unsigned index=(unsigned)(relative>>16);
        uint32_t offset=(uint32_t)(relative&UINT64_C(0xffff));
        if(index>=5||offset>0x80||!marconi_ap_te_caller(s))return false;
        uint64_t data=write?*value:0;
        if(s->i2c_nopeer_profile) {
            if(!marconi_i2c_nopeer_refresh(s,index))return false;
            int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            if(now<0)return marconi_i2c_nopeer_failure(s,index,O62M_I2C_NOPEER_TIME_V3,"negative-MMIO-clock");
            int result=o62m_i2c_nopeer_access_v3(&s->i2c_nopeer[index],&s->ap_i2c_empty[index],
                                               (uint64_t)now,offset,size,write,&data);
            if(result!=O62M_I2C_NOPEER_OK_V3){
                marconi_public_report("MARCONI_I2C_ACCESS_REJECT controller=%u offset=%02x result=%d write=%u abort=%u queued=%u",index,offset,result,write,s->i2c_nopeer[index].abort_latched,s->i2c_nopeer[index].count);
                return false;
            }
            if(write&&offset==0x6c&&!data)i2c_end_transfer(s->i2c_nopeer_bus[index]);
            if(!marconi_i2c_nopeer_refresh(s,index))return false;
            if(!write)*value=data;
            if(s->i2c_nopeer[index].reads+s->i2c_nopeer[index].writes<=64)marconi_public_report("MARCONI_I2C_NOPEER_ACCESS controller-index=%u access=%s reads=%" PRIu64
                " writes=%" PRIu64 " enqueued=%" PRIu64 " nacks=%" PRIu64 " pending=%u queued=%u "
                "canonical-config-single-owner=true no-payload-register-address-or-ACK-success-output=true",
                index,write?"write":"read",s->i2c_nopeer[index].reads,s->i2c_nopeer[index].writes,
                s->i2c_nopeer[index].enqueued,s->i2c_nopeer[index].nacks,s->i2c_nopeer[index].pending,s->i2c_nopeer[index].count);
            return true;
        }
        if(!o62m_i2c_empty_config_access_v3(&s->ap_i2c_empty[index],offset,size,write,&data))return false;
        if(!write)*value=data;
        marconi_public_report("MARCONI_AP_I2C_EMPTY_CONFIG controller-index=%u access=%s reads=%" PRIu64
            " writes=%" PRIu64 " config-enable=%u actual-empty-queue-and-no-job-state=true "
            "DATA-INTR-nonzeroMASK-peer-IRQ-and-transfer-strict=true RAW-emptyFIFOlevel-comparator-explicit-policy=true no-ready-completion-or-raw-register-output=true",
            index,write?"write":"read",s->ap_i2c_empty[index].reads,s->ap_i2c_empty[index].writes,
            s->ap_i2c_empty[index].config[11]);
        return true;
    }
    /* Two original I2C software command ports, not an inferred physical
     * SET/CLEAR alias. Capture only actual original callers; no response,
     * controller-ready state, transfer, IRQ, or reset is synthesized. */
    if(s->ap_i2c_soc_commands&&(address==UINT64_C(0x40058000)||address==UINT64_C(0x40054000))) {
        unsigned index=address==UINT64_C(0x40058000)?0:1;
        uint32_t expected_lr=index?UINT32_C(0x60c273a3):UINT32_C(0x60c27393);
        uint64_t pc=marconi_mmio_pc();
        uint32_t command=write?(uint32_t)*value:0;
        if(!write||!marconi_ap_te_caller(s)||pc!=UINT64_C(0x60c49cca)||
           s->armv7m.cpu->env.regs[14]!=expected_lr||!command||
           (command&(command-1))||(command&UINT32_C(0x1ff))||
           s->ap_i2c_soc_captures[index]==UINT64_MAX)return false;
        s->ap_i2c_soc_last[index]=command;s->ap_i2c_soc_written[index]=true;
        s->ap_i2c_soc_captures[index]++;
        marconi_public_report("MARCONI_AP_I2C_SOC_COMMAND port-index=%u captures=%" PRIu64
            " original-PC-caller-address-and-onehot-constraints=true initially-unwritten-request-capture=true "
            "no-readback-transfer-reset-ready-or-IRQ-generated=true raw-command-and-registers-redacted=true",
            index,s->ap_i2c_soc_captures[index]);
        return true;
    }
    if(s->wdt4012_profile&&address>=UINT64_C(0x40120000)&&address<=UINT64_C(0x40120014)) {
        if(!marconi_ap_te_caller(s))return false;
        uint64_t *count=write?&s->wdt4012_writes:&s->wdt4012_reads;
        if(*count==UINT64_MAX)return marconi_wdt4012_boundary(s,O62M_WDT4012_OVERFLOW_V1,"access-counter");
        if(!marconi_wdt4012_refresh(s))return false;
        uint64_t data=write?*value:0;
        int result=o62m_wdt4012_access_v1(&s->wdt4012,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
            (uint32_t)(address-UINT64_C(0x40120000)),size,write,&data);
        if(result!=O62M_WDT4012_OK_V1)return marconi_wdt4012_boundary(s,result,write?"MMIO-write":"MMIO-read");
        if(!marconi_wdt4012_refresh(s))return false;
        if(!write)*value=data;
        ++*count;
        if(write&&(address==UINT64_C(0x40120000)||address==UINT64_C(0x4012000c)))marconi_wdt4012_timing_report(s,"control-or-feed");
        if(s->wdt4012_reads+s->wdt4012_writes<=64)marconi_public_report("MARCONI_WDT4012_ACCESS access=%s reads=%" PRIu64 " writes=%" PRIu64
            " starts=%" PRIu64 " feeds=%" PRIu64 " preloads=%" PRIu64 " running=%u timer-armed=%u "
            "real-elapsed-and-original-custom15TOP=true disabled-preload-and-CCVR-are-explicit-virtual-policy=true "
            "first-expiry-strict=true no-counter-register-key-or-IRQ-output=true",
            write?"write":"read",s->wdt4012_reads,s->wdt4012_writes,s->wdt4012.starts,
            s->wdt4012.feeds,s->wdt4012.preloads,s->wdt4012.running,timer_pending(s->wdt4012_event));
        return true;
    }
    if(s->ap_watchdog_soc_present&&address==UINT64_C(0x40050000)) {
        static const uint64_t pcs[]={UINT64_C(0x60c49c3e),UINT64_C(0x60c49c48),
                                    UINT64_C(0x60c49e90),UINT64_C(0x60c49e94)};
        unsigned step=s->ap_watchdog_soc_phase;
        uint64_t pc=marconi_mmio_pc();
        if(step>=ARRAY_SIZE(pcs)||!marconi_ap_te_caller(s)||pc!=pcs[step]||
           write!=((step&1u)!=0)||s->armv7m.cpu->env.regs[14]!=UINT32_C(0x60c49e91))return false;
        if(write) {
            uint32_t expected=step==1?(s->ap_watchdog_soc_word&~UINT32_C(0x10000)):
                                     (s->ap_watchdog_soc_word|UINT32_C(0x10000));
            if(*value!=expected)return false;
            s->ap_watchdog_soc_word=(uint32_t)*value;
        } else *value=s->ap_watchdog_soc_word;
        ++s->ap_watchdog_soc_phase;
        marconi_public_report("MARCONI_AP_WATCHDOG_SOC_CONFIG access=%s step=%u "
            " exact-original-fourstep-RMW-and-caller=true independent-explicit-fullword=true "
            "opaque-bits-preserved=true no-reset-counter-IRQ-ready-or-alias-effect-generated=true raw-word-redacted=true",
            write?"write":"read",step);
        return true;
    }
    if(s->ap_watchdog_init&&(address==UINT64_C(0x4c01044c)||address==UINT64_C(0x4c01024c))) {
        if(s->cp_created&&current_cpu==CPU(&s->cp_cpu)) {
            unsigned index=address==UINT64_C(0x4c01044c)?0:1;
            uint64_t pc=marconi_mmio_pc();int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            if(!write||size!=4||*value!=0x40000||s->cp_aon_request_count!=index||now<0||
               pc!=(index?UINT64_C(0x641091c2):UINT64_C(0x641091b6))||
               !s->cp_cold.release_accepted||!marconi_cp_cold_source(s)||
               (index&&now<s->cp_aon_request_ns[0]))return false;
            s->cp_aon_requests[index]=(uint32_t)*value;s->cp_aon_request_ns[index]=now;
            ++s->cp_aon_request_count;
            marconi_public_report("MARCONI_CP_AON_PAIR_REQUEST index=%u requests=%u virtual-ns=%" PRId64 " shared44c-request-owner=true prior-AP-request-preserved=true no-reset-ready-or-CRF-release=true",index,s->cp_aon_request_count,now);
            return true;
        }
        if(address!=UINT64_C(0x4c01044c))return false;
        uint64_t pc=marconi_mmio_pc();
        if(!write||*value!=UINT32_C(0x84)||pc!=UINT64_C(0x60c44740)||
           !marconi_ap_te_caller(s)||s->ap_watchdog_aon_writes==UINT64_MAX)return false;
        s->ap_watchdog_aon_word=(uint32_t)*value;s->ap_watchdog_aon_written=true;
        ++s->ap_watchdog_aon_writes;
        marconi_public_report("MARCONI_AP_WATCHDOG_AON_CONFIG writes=%" PRIu64
            " exact-original-WO-config-capture=true initially-unwritten=true no-existing-owner-or-alias=true "
            "no-reset-counter-IRQ-ready-or-hardware-sideeffect-generated=true raw-word-redacted=true",
            s->ap_watchdog_aon_writes);
        return true;
    }
    if(s->ap_spi_config&&address>=UINT64_C(0x401b0000)&&address<=UINT64_C(0x401b0060)) {
        static const uint32_t offsets[]={0,8,16,24,28,44,20};
        uint32_t offset=(uint32_t)(address-UINT64_C(0x401b0000));
        uint64_t *count=write?&s->ap_spi_config_writes:&s->ap_spi_config_reads;
        if(!marconi_ap_te_caller(s)||*count==UINT64_MAX||size!=4||(write&&*value>UINT32_MAX))return false;
        if(offset==0x20||offset==0x24||offset==0x34||offset==0x60) {
            if(offset!=0x60&&write)return false;
            if(offset==0x20)*value=s->ap_spi.tx_count;
            else if(offset==0x24)*value=s->ap_spi.rx_count;
            else if(offset==0x34)*value=o62m_spi_risr_v1(&s->ap_spi,s->ap_spi_config_words[3],s->ap_spi_config_words[4]);
            else if(write) {
                if(!marconi_ap_spi_enabled(s)||!o62m_spi_write_v1(&s->ap_spi,(uint32_t)*value,
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),s->ap_spi_config_words[0],s->ap_spi_config_words[6]))return false;
                marconi_ap_spi_arm(s);
            } else {
                uint32_t frame;if(!o62m_spi_read_v1(&s->ap_spi,&frame))return false;*value=frame;
            }
        } else if(offset==72) {
            if(write||s->ap_spi_error_latch)return false;
            *value=s->ap_spi_error_latch;
        } else {
            unsigned i;for(i=0;i<ARRAY_SIZE(offsets)&&offsets[i]!=offset;i++) {}
            if(i==ARRAY_SIZE(offsets))return false;
            if(offset==20) {
                uint64_t pc=marconi_mmio_pc();
                if(pc!=(write?0x60c2756a:0x60c27562))return false;
                if(write&&((s->ap_spi_config_words[1]&1)||
                    (((uint32_t)*value^s->ap_spi_config_words[i])&UINT32_C(0xffff0000))||
                    (*value&1)||(*value&0xffff)<2))return false;
            }
            if(write) {
                if((offset==8&&*value>1)||(offset==44&&(*value&63u))||
                   ((offset==24||offset==28)&&*value>=64))return false;
                if(offset!=24&&offset!=28&&*value!=s->ap_spi_config_words[i]&&
                   (s->ap_spi.pending||s->ap_spi.tx_count))return false;
                if(offset==16&&(*value||s->ap_spi_config_words[i])) {
                    uint64_t pc=marconi_mmio_pc();
                    uint32_t old=s->ap_spi_config_words[i];
                    if((old&~1u)||
                       !((pc==0x60c287d8&&*value==(uint64_t)(old|1u))||
                         (pc==0x60c2885c&&*value==(uint64_t)(old&~1u))))return false;
                }
                if(offset==0&&(s->ap_spi_config_words[1]&1u)&&
                   *value!=s->ap_spi_config_words[0])return false;
                s->ap_spi_config_words[i]=(uint32_t)*value;
                if(offset==8&&!*value)marconi_ap_spi_cancel(s);
            } else *value=s->ap_spi_config_words[i];
        }
        ++*count;
        if(offset!=0x20&&offset!=0x24&&offset!=0x34&&offset!=0x60)
            marconi_public_report("MARCONI_AP_SPI_CONFIG access=%s offset-index=%u "
                "single-owner=true raw-word-redacted=true no-peer=true",write?"write":"read",offset/4);
        return true;
    }
    if(s->ap_gpio_config) {
        bool cp_gpio=s->cp_created&&current_cpu==CPU(&s->cp_cpu)&&
            s->cp_cold.release_accepted&&!s->reported&&marconi_cp_cold_source(s);
        bool config_caller=marconi_ap_te_caller(s)||cp_gpio;
        static const uint64_t ports[]={UINT64_C(0x4c060060),UINT64_C(0x4c070060),
            UINT64_C(0x4a0a0060),UINT64_C(0x4a0b0060),UINT64_C(0x4a0f0060)};
        for(unsigned i=0;i<ARRAY_SIZE(ports);i++) {
            if(address==ports[i]-0x10&&!write&&size==4&&marconi_ap_te_caller(s)&&
               marconi_mmio_pc()==UINT64_C(0x60c37202)){
                unsigned pin=s->armv7m.cpu->env.regs[2];
                if(pin>=151||pin/32!=i)return false;
                if(pin==4||pin==5||pin==17||pin==40||pin==58||pin==1){
                    uint32_t bit=UINT32_C(1)<<(pin&31);
                    if(s->ap_gpio_direction[i]&bit)return false;
                    *value=bit;return true;
                }
                if(pin==41){marconi_touch_refresh(s);*value=s->touch.irq_low?0:UINT32_C(1)<<9;return true;}
                if(pin==25||pin==42){
                    uint32_t bit=UINT32_C(1)<<(pin&31);
                    if(!(s->ap_gpio_direction[i]&bit))return false;
                    *value=s->ap_gpio_output_latch[i]&bit;return true;
                }
                uint32_t caller=s->armv7m.cpu->env.regs[14]&~1u;
                if(caller<UINT32_C(0x605c0000)||caller>=UINT32_C(0x61594000))caller=0;
                marconi_public_report("MARCONI_GPIO_INPUT_BOUNDARY public-pin=%u bank=%u original-caller=%08x original-getter=true no-private-data=true",pin,i,caller);
                return false;
            }
            if(i==1&&(address==ports[i]-0x20||address==ports[i]-0x14)){
                if(!marconi_ap_te_caller(s)||size!=4)return false;
                const uint32_t bit=1u<<9;
                if(address==ports[i]-0x20){
                    if(write)return false;marconi_touch_refresh(s);
                    *value=s->touch.pending&&(s->ap_gpio_irq_config[1][0]&bit)&&!(s->ap_gpio_irq_config[1][1]&bit)?bit:0;
                } else {
                    if(!write||*value!=bit)return false;
                    s->touch.pending=false;marconi_touch_refresh(s);
                }
                return true;
            }
            if(i==4&&(address==ports[i]-0x20||address==ports[i]-0x14)) {
                if(!marconi_ap_te_caller(s)||size!=4)return false;
                const uint32_t bit=UINT32_C(1)<<5;
                if(address==ports[i]-0x20) {
                    if(write)return false;
                    *value=s->display_te_input.pending&&
                        (s->ap_gpio_irq_config[4][0]&bit)&&!(s->ap_gpio_irq_config[4][1]&bit)?bit:0;
                } else {
                    if(!write||!*value||(*value&~(uint64_t)bit))return false;
                    o62m_display_te_ack_v1(&s->display_te_input);
                    marconi_display_te_refresh(s);
                }
                return true;
            }
            static const uint32_t config_offsets[]={0x30,0x34,0x38,0x3c,0x48,0x68};
            for(unsigned j=0;j<ARRAY_SIZE(config_offsets);j++) {
                if(address!=ports[i]-0x60+config_offsets[j])continue;
                if(!config_caller||size!=4||(write&&*value>UINT32_MAX))return false;
                if(write){s->ap_gpio_irq_config[i][j]=(uint32_t)*value;
                    if(i==4)marconi_display_te_refresh(s);
                    if(i<2)marconi_touch_refresh(s);}
                else *value=s->ap_gpio_irq_config[i][j];
                marconi_public_report("MARCONI_AP_GPIO_IRQ_CONFIG cpu=%u access=%s bank-index=%u field-index=%u "
                    "independent-zero-config-and-real-RMW=true configuration-retained=true TE-input-owns-only-pin133=true raw-word-redacted=true",
                    cp_gpio?1:0,write?"write":"read",i,j);
                return true;
            }
            if(address==ports[i]-0x60||address==ports[i]-0x58) {
                if(!write||!marconi_ap_te_caller(s)||!*value||(*value&(*value-1)))return false;
                bool set=address==ports[i]-0x60;
                if(set)s->ap_gpio_output_latch[i]|=(uint32_t)*value;
                else s->ap_gpio_output_latch[i]&=~(uint32_t)*value;
                if(i<2)marconi_touch_refresh(s);
                marconi_public_report("MARCONI_AP_GPIO_OUTPUT_COMMAND bank-index=%u set=%u "
                    "actual-single-bit-latch-update=true input-projection-peer-IRQ-unmodeled=true raw-word-redacted=true",i,set);
                return true;
            }
            if(address==ports[i]-0x5c) {
                if(!config_caller||size!=4||(write&&*value>UINT32_MAX))return false;
                if(write){s->ap_gpio_direction[i]=(uint32_t)*value;
                    if(i==4)marconi_display_te_refresh(s);
                    if(i<2)marconi_touch_refresh(s);}
                else *value=s->ap_gpio_direction[i];
                marconi_public_report("MARCONI_AP_GPIO_DIRECTION cpu=%u access=%s bank-index=%u "
                    "explicit-input-direction-reset=true actual-RMW-retained=true canonical-pin-direction=true raw-word-redacted=true",
                    cp_gpio?1:0,write?"write":"read",i);
                return true;
            }
            if(address!=ports[i])continue;
            uint64_t pc=marconi_mmio_pc();
            bool cp_config=s->cp_created&&current_cpu==CPU(&s->cp_cpu)&&
                s->cp_cold.release_accepted&&!s->reported&&marconi_cp_cold_source(s)&&pc==UINT64_C(0x6405c820);
            bool ap_config=marconi_ap_te_caller(s)&&pc==UINT64_C(0x60c44580);
            if(!write||size!=4||*value!=1||(!ap_config&&!cp_config)||
               s->ap_gpio_config_writes==UINT64_MAX)return false;
            s->ap_gpio_config_words[i]=(uint32_t)*value;s->ap_gpio_config_written[i]=true;
            ++s->ap_gpio_config_writes;
            marconi_public_report("MARCONI_AP_GPIO_CONFIG_CAPTURE bank-index=%u writes=%" PRIu64
                " exact-original-five-ports-and-W1-PC=true initially-unwritten=true retained-config-only=true "
                "no-GPIO-input-output-IRQ-ready-or-POR-read-response-generated=true raw-word-redacted=true",
                i,s->ap_gpio_config_writes);
            return true;
        }
    }
    if(s->ap_te_host1_config&&
       (address==UINT64_C(0x4033430c)||address==UINT64_C(0x40334144)||
        address==UINT64_C(0x40334164)||address==UINT64_C(0x40334168)||
        (address>=UINT64_C(0x40334200)&&address<UINT64_C(0x40334270))||
        (address>=UINT64_C(0x40334280)&&address<UINT64_C(0x403342f0)))) {
        uint32_t gate;
        if(!marconi_ap_te_caller(s)||s->te_context.phase!=O62M_TE_CONTEXT_READY_PHASE||
           s->ap_te_host1_config_accesses==UINT64_MAX||
           o62m_clock_controls_read(&s->board_controls,UINT64_C(0x50050018),4,&gate)!=O62M_CLOCK_CONTROLS_OK||!(gate&2))return false;
        const char *port;
        if(address==UINT64_C(0x4033430c)) {
            uint64_t pc=marconi_mmio_pc();
            if(write) {
                if(pc!=UINT64_C(0x60e530b2))return false;
                s->ap_te_host1_rnp_mask=(uint32_t)*value;
                s->ap_te_host1_rnp_mask_written=true;
            } else {
                if(pc!=UINT64_C(0x60e53116)||!s->ap_te_host1_rnp_mask_written)return false;
                *value=s->ap_te_host1_rnp_mask;
            }
            port="RNP-mask";
        } else if(address==UINT64_C(0x40334144)||address==UINT64_C(0x40334164)||address==UINT64_C(0x40334168)) {
            unsigned index=address==UINT64_C(0x40334144)?0:address==UINT64_C(0x40334164)?1:2;
            /* Configuration only: OP_RUN, command ENTRY, SRAM and all
             * arithmetic/status/completion ports remain unsupported. */
            if(write&&index==0&&(*value&1u))return false;
            if(write)s->ap_te_host1_aca_config[index]=(uint32_t)*value;
            else *value=s->ap_te_host1_aca_config[index];
            port="ACA-config";
        } else {
            /* Translate parser coordinates only. This instance owns separate
             * host1 config/CQ/CSQ storage; no host0 state or key is shared. */
            uint64_t logical=address-UINT64_C(0x40334200)+UINT64_C(0x50333200);
            if(!o62m_symc_empty_access_v1(&s->ap_te_host1_symc,logical,size,write,value))return false;
            port=address<UINT64_C(0x40334280)?"SCA-empty-config":"HASH-empty-config";
        }
        ++s->ap_te_host1_config_accesses;
        marconi_public_report("MARCONI_AP_TE_HOST1_CONFIG access=%s port=%s accesses=%" PRIu64
            " separate-host1-backing=true shared-const-capability-geometry=true no-host0-queue-key-or-mask-alias=true "
            "no-CQ-work-entropy-IRQ-or-completion-generated=true raw-word-redacted=true",
            write?"write":"read",port,s->ap_te_host1_config_accesses);
        return true;
    }
    if(s->symc_empty_profile&&((address>=0x50333200&&address<0x50333270)||
       (address>=0x50333280&&address<0x503332f0))) {
        uint32_t gate;CPUARMState *env=&s->armv7m.cpu->env;
        bool privileged=arm_v7m_is_handler_mode(env)||!(env->v7m.control[env->v7m.secure]&UINT32_C(1));
        if(!env->v7m.secure||!privileged||s->te_context.phase!=O62M_TE_CONTEXT_READY_PHASE||
           o62m_clock_controls_read(&s->board_controls,0x50050018,4,&gate)!=O62M_CLOCK_CONTROLS_OK||!(gate&2))return false;
        uint64_t pc=marconi_mmio_pc();
        if(marconi_sensitive_guest_data&&write&&address==0x50333280) {
            unsigned op=(*value>>24)&255;
            const char *name=op==0x80?"INIT":op==0x40?"PROC":op==0x20?"FINISH":op==0xff?"CLEAR":"unsupported-family";
            if(op==0x80)marconi_public_report("MARCONI_HASH_PRIVATE_COMMAND family=%s slot=%u mode=%s key-source=%s algorithm-id=%u key-size-selector=%u parameters-and-raw-values-redacted=true",
                                  name,(unsigned)((*value>>19)&31),(*value&16)?"HMAC":"digest",
                                  (((*value>>2)&3)==0)?"MODEL":(((*value>>2)&3)==1)?"ROOT":(((*value>>2)&3)==2)?"EXT":"unsupported",
                                  (unsigned)((*value>>7)&15),(unsigned)((*value>>1)&1));
            else marconi_public_report("MARCONI_HASH_PRIVATE_COMMAND family=%s slot=%u parameters-and-raw-values-redacted=true",name,(unsigned)((*value>>19)&31));
            if(op==0x40) {
                unsigned flags=(unsigned)*value&255u;
                marconi_public_report("MARCONI_HASH_PROC_CLASS slot=%u flags-low8=%u reserved-mid-nonzero=%u "
                    "exact-legacy-final=%u CQwords=%u CSQwords=%u control-admitted=%u "
                    "active=%u hmac=%u inner-done=%u finished=%u job-queued=%u "
                    "public-command-control-only=true parameter-address-length-and-payload-redacted=true",
                    (unsigned)((*value>>19)&31),flags,!!(*value&UINT32_C(0x0007ff00)),
                    *value==UINT32_C(0x40000019),s->symc_empty.host[1].cq_words,s->symc_empty.host[1].csq_words,
                    (s->symc_empty.host[1].control&UINT32_C(0xc1))==UINT32_C(0xc1),
                    s->hash_context.active,s->hash_is_hmac,s->hash_inner_done,s->hash_finished,s->hash_job_queued);
            }
        }
        if(!o62m_hash_work_queue_access_v7(&s->symc_empty,address,size,write,value)) {
            error_report("MARCONI_SYMC_EMPTY_BOUNDARY address=%08" HWADDR_PRIx " pc=%08" PRIx64
                         " CQ-CSQ-key-or-unsupported-register-strict=true no-operation-completed=true",address,pc);return false;
        }
        error_report("MARCONI_SYMC_QUEUE_ACCESS access=%s address=%08" HWADDR_PRIx
                     " value=%08" PRIx64 " pc=%08" PRIx64
                     " canonical-CQdepth=true CQwords=%u CSQwords=%u completion-only-after-actual-work=true",
                     write?"write":"read",address,*value,pc,s->symc_empty.host[1].cq_words,s->symc_empty.host[1].csq_words);
        uint32_t command=s->symc_empty.host[1].cq[0];
        unsigned command_words=o62m_hash_frame_words_v7(&s->symc_empty.host[1],command);
        if(write&&(address==UINT64_C(0x50333280)||address==UINT64_C(0x50333284))&&
           command_words&&s->symc_empty.host[1].cq_words==command_words) {
            const uint32_t *frame=s->symc_empty.host[1].cq;
            uint64_t iv=((uint64_t)frame[2]<<32)|frame[1],eks=((uint64_t)frame[4]<<32)|frame[3];
            if(frame[0]==UINT32_C(0x80000115))error_report("MARCONI_HASH_INIT_JOB_PENDING frame-words=5 IV-address=%016" PRIx64
                         " EKS-address=%016" PRIx64 " accepted-original-CQ=true "
                         "operation=HMAC-SHA256-ROOT-secure-key-ladder real-crypto-backend-required=true "
                         "no-CSQ-IRQ-or-key-output=true no-work-completion-claimed=true",iv,eks);
            if(s->hash_init_profile) {
                int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                if(s->hash_job_queued||((frame[0]>>24)==0x80&&s->hash_context.active&&!s->hash_finished)||
                   (((frame[0]>>24)==0x40)&&(!s->hash_context.active||s->hash_inner_done||
                    (frame[0]!=UINT32_C(0x40000019)&&s->hash_is_hmac)))||
                   (frame[0]==UINT32_C(0x2000003f)&&(!s->hash_context.active||!s->hash_inner_done||s->hash_finished))||
                   s->hash_work_ns>(uint64_t)(INT64_MAX-now))return marconi_hash_boundary(s,"unsupported-command-state-or-work-clock-overflow");
                s->hash_deadline_ns=now+(int64_t)s->hash_work_ns;s->hash_job_queued=true;
                timer_mod_ns(s->hash_init_event,s->hash_deadline_ns);
            } else {
                s->reported=true;
                qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC,MARCONI_PROBE_EXIT);
                if(current_cpu) {cpu_exit(current_cpu);}
            }
        }
        return true;
    }
    if(s->rnp_postkey_config&&s->rnp_pool_event&&s->rnp_live_config_active&&
       ((address>=UINT64_C(0x50333300)&&address<=UINT64_C(0x5033332c)&&address!=UINT64_C(0x5033330c))||
        address==UINT64_C(0x5033339c))) {
        CPUARMState *env=&s->armv7m.cpu->env;
        bool privileged=arm_v7m_is_handler_mode(env)||!(env->v7m.control[env->v7m.secure]&1);
        if(!env->v7m.secure||!privileged||size!=4||(address&3)||(write&&*value>UINT32_MAX))return false;
        if(!marconi_rnp_source_available(s)){marconi_rnp_pool_cancel(s);return false;}
        if(address==UINT64_C(0x5033339c)) {
            if(write){if(*value&~UINT64_C(15))return false;s->rnp_source_pending&=~(uint32_t)*value;}
            else *value=s->rnp_source_pending;
            return true;
        }
        uint32_t v=write?(uint32_t)*value:0;
        if(!o62m_rnp_access_v1(&s->rnp_pool,(uint32_t)(address-UINT64_C(0x50333300)),write,&v,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)))return false;
        if(!write)*value=v;
        if(write&&address==UINT64_C(0x50333300)) {
            if(s->rnp_pool.pending) {
                s->rnp_pool_armed=s->rnp_pool.generation;timer_mod_ns(s->rnp_pool_event,s->rnp_pool.deadline);
                if(s->rnp_pool.requests<=32)marconi_public_report("MARCONI_RNP_POOL_REQUEST generation=%" PRIu64
                    " requests=%" PRIu64 " work-ns=3000 actual-control-write=true no-data-or-key-output=true",
                    s->rnp_pool.generation,s->rnp_pool.requests);
            } else timer_del(s->rnp_pool_event);
        }
        return true;
    }
    if(s->rnp_mask_config&&address==0x5033330c) {
        uint32_t gate;CPUARMState *env=&s->armv7m.cpu->env;
        bool privileged=arm_v7m_is_handler_mode(env)||!(env->v7m.control[env->v7m.secure]&UINT32_C(1));
        if(!env->v7m.secure||!privileged||s->te_context.phase!=O62M_TE_CONTEXT_READY_PHASE||
           o62m_clock_controls_read(&s->board_controls,0x50050018,4,&gate)!=O62M_CLOCK_CONTROLS_OK||!(gate&2)||
           size!=4||(write&&*value>UINT32_MAX)) {return false;}
        if(write) {
            if(s->rnp_pool.pending&&*value!=s->rnp_mask_word)return false;
            if(s->rnp_pool.valid&&!(*value&1))return false;
            s->rnp_mask_word=(uint32_t)*value;
        }else {*value=s->rnp_mask_word;}
        uint64_t pc=marconi_mmio_pc();
        error_report("MARCONI_RNP_MASK_CONFIG access=%s value=%08" PRIx64 " pc=%08" PRIx64
                     " bit0-mask-only=true reserved-bits-opaque=true no-event-entropy-bank-fill-or-IRQ-created=true",
                     write?"write":"read",*value,pc);return true;
    }
    if(s->rnp_postkey_config&&s->trng_virtual.phase==O62M_VAO_VALID_V1&&
       ((address>=0x50333384&&address<=0x50333394)||address==0x503333a0||address==0x503333a4)) {
        uint32_t gate;CPUARMState *env=&s->armv7m.cpu->env;
        bool privileged=arm_v7m_is_handler_mode(env)||!(env->v7m.control[env->v7m.secure]&UINT32_C(1));
        if(!env->v7m.secure||!privileged||s->te_context.phase!=O62M_TE_CONTEXT_READY_PHASE||
           o62m_clock_controls_read(&s->board_controls,0x50050018,4,&gate)!=O62M_CLOCK_CONTROLS_OK||!(gate&2)||
           size!=4||(address&3)||(write&&*value>UINT32_MAX)||!s->trng_virtual.material_present) {return false;}
        if(!s->rnp_live_config_active) {
            /* Frozen AO config becomes only the historical key-job input.
             * This bank is now the sole live configuration provider; the
             * old core retains its real private key and metadata unchanged. */
            memcpy(s->rnp_live_config,s->trng_virtual.config,sizeof(s->trng_virtual.config));
            s->rnp_live_config_active=true;
            error_report("MARCONI_RNP_POSTKEY_CONFIG_HANDOVER actual-AO-key-present=true "
                         "frozen-config-now-job-history-only=true new-bank-sole-live-owner=true no-key-change=true");
        }
        uint32_t *word=address==0x503333a0?&s->rnp_controller_mask:
                       &s->rnp_live_config[address==0x503333a4?5:(address-0x50333384)/4];
        if(write) {if(s->rnp_pool.pending&&*value!=*word)return false;*word=(uint32_t)*value;}else {*value=*word;}
        uint64_t pc=marconi_mmio_pc();
        error_report("MARCONI_RNP_POSTKEY_CONFIG access=%s address=%08" HWADDR_PRIx
                     " value=%08" PRIx64 " pc=%08" PRIx64
                     " raw-configuration-only=true RNP-fill-and-data-strict=true no-work-or-completion-generated=true",
                     write?"write":"read",address,*value,pc);return true;
    }
    if(s->trng_virtual_profile&&address>=0x50333300&&address<0x50333400) {
        uint32_t gate;
        CPUARMState *env=&s->armv7m.cpu->env;
        bool privileged=arm_v7m_is_handler_mode(env)||
                        !(env->v7m.control[env->v7m.secure]&UINT32_C(1));
        if(!env->v7m.secure||!privileged||s->te_context.phase!=O62M_TE_CONTEXT_READY_PHASE||
           o62m_clock_controls_read(&s->board_controls,0x50050018,4,&gate)!=O62M_CLOCK_CONTROLS_OK||!(gate&2)) {
            error_report("MARCONI_VIRTUAL_AO_BOUNDARY reason=secure-privileged-context-ready-clock-enabled-required");return false;
        }
        int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        O62MVirtualAoResultV1 result=o62m_virtual_ao_access_v1(&s->trng_virtual,now,address,size,write,value);
        if(result) {
            error_report("MARCONI_VIRTUAL_AO_BOUNDARY access=%s address=%08" HWADDR_PRIx
                         " reason=%s no-invented-completion=true",write?"write":"read",address,
                         o62m_virtual_ao_reason_v1(result));return false;
        }
        O62MVirtualAoMetadataV1 metadata;
        result=o62m_virtual_ao_query_v1(&s->trng_virtual,&metadata);
        assert(!result);
        if(write&&address==O62M_TRNG_UNKEYED_CTL) {
            O62MVirtualAoJobV1 job;bool due;
            result=o62m_virtual_ao_job_due_v1(&s->trng_virtual,now,&job,&due);
            assert(!result&&!due);
            timer_mod_ns(s->trng_virtual_event,job.deadline_ns);
            error_report("MARCONI_VIRTUAL_AO_JOB_BEGIN job=%" PRIu64 " key-bytes=%u deadline-ns=%" PRId64
                         " private-store-empty=true valid=false explicit-virtual-profile=true",job.id,job.bytes,job.deadline_ns);
        }
        if(s->trng_virtual_accesses++<256) {
            error_report("MARCONI_VIRTUAL_AO_ACCESS access=%s address=%08" HWADDR_PRIx
                         " value=%08" PRIx64 " phase=%u material-present=%u config-written-mask=%02x "
                         "virtual-ns=%" PRId64 " no-key-or-entropy-output=true",
                         write?"write":"read",address,*value,metadata.phase,metadata.material_present,
                         metadata.config_written_mask,now);
        }
        return true;
    }
    if(s->trng_unkeyed_fixture&&address>=0x50333300&&address<0x50333400) {
        uint32_t gate;
        CPUARMState *env=&s->armv7m.cpu->env;
        bool privileged=arm_v7m_is_handler_mode(env)||
                        !(env->v7m.control[env->v7m.secure]&UINT32_C(1) /* CONTROL.nPRIV: architectural bit0 */);
        if(!env->v7m.secure||!privileged||s->te_context.phase!=O62M_TE_CONTEXT_READY_PHASE||
           o62m_clock_controls_read(&s->board_controls,0x50050018,4,&gate)!=O62M_CLOCK_CONTROLS_OK||!(gate&2)) {
            error_report("MARCONI_TRNG_UNKEYED_BOUNDARY reason=secure-privileged-context-ready-clock-enabled-required");return false;
        }
        uint64_t pc=marconi_mmio_pc();
        O62MTrngUnkeyedResult result=o62m_trng_unkeyed_access(&s->trng_unkeyed,address,size,write,value);
        if(result!=O62M_TRNG_UNKEYED_OK) {
            error_report("MARCONI_TRNG_UNKEYED_BOUNDARY access=%s address=%08" HWADDR_PRIx
                         " value=%08" PRIx64 " pc=%08" PRIx64 " reason=%s "
                         "no-key-created-state-not-committed=true",
                         write?"write":"read",address,*value,pc,o62m_trng_unkeyed_reason(result));return false;
        }
        error_report("MARCONI_TRNG_UNKEYED_ACCESS access=%s address=%08" HWADDR_PRIx
                     " value=%08" PRIx64 " pc=%08" PRIx64 " config-written-mask=%02x "
                     "explicit-empty-AO-key=true no-key-generated=true",
                     write?"write":"read",address,*value,pc,s->trng_unkeyed.written_mask);
        return true;
    }
    if(s->virtual_otp_profile&&(address==0x50333600||address==0x50333610||
       (address>=0x50331000&&address<0x50333000))) {
        uint32_t gate;CPUARMState *env=&s->armv7m.cpu->env;
        bool privileged=arm_v7m_is_handler_mode(env)||!(env->v7m.control[env->v7m.secure]&UINT32_C(1));
        if(!env->v7m.secure||!privileged||
           o62m_clock_controls_read(&s->board_controls,0x50050018,4,&gate)!=O62M_CLOCK_CONTROLS_OK||!(gate&2)) {
            error_report("MARCONI_VIRTUAL_OTP_MMIO_BOUNDARY reason=secure-privileged-work-clock-required");return false;
        }
        int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        int result=o62m_virtual_otp_access_v1(&s->virtual_otp,now,address,size,write,value);
        uint64_t pc=marconi_mmio_pc();
        if(result) {
            error_report("MARCONI_VIRTUAL_OTP_MMIO_BOUNDARY address=%08" HWADDR_PRIx " pc=%08" PRIx64
                         " reason-code=%d rejected-before-value-output=true private-key-MMIO-unimplemented=true",address,pc,result);
            return false;
        }
        error_report("MARCONI_VIRTUAL_OTP_MMIO access=%s address=%08" HWADDR_PRIx
                     " value=%08" PRIx64 " pc=%08" PRIx64
                     " public-data-or-derived-status-only=true actual-backing-shadow-owner=true",
                     write?"write":"read",address,*value,pc);return true;
    }
    if(s->te_capability_inputs&&(address==0x50330004||address==0x50330008||address==0x5033000c||
       address==0x50330010||address==0x50330018||address==0x503300d0||
       address==0x503300d4||address==0x503300d8||address==0x503300dc||
       (address>=0x50330024&&address<=0x5033009c&&!((address-0x50330024)&7)))) {
        uint32_t gate;
        CPUARMState *env=&s->armv7m.cpu->env;
        bool privileged=arm_v7m_is_handler_mode(env)||
                        !(env->v7m.control[env->v7m.secure]&UINT32_C(1));
        if(!env->v7m.secure||!privileged||
           o62m_clock_controls_read(&s->board_controls,0x50050018,4,&gate)!=O62M_CLOCK_CONTROLS_OK||!(gate&2)) {
            error_report("MARCONI_TE_CAPABILITY_BOUNDARY reason=secure-privileged-work-clock-required");return false;
        }
        O62MTeContextResult result=o62m_te_caps_access_v3(&s->te_capabilities,&s->te_context,address,size,write,value);
        uint64_t pc=marconi_mmio_pc();
        if(result) {
            error_report("MARCONI_TE_CAPABILITY_BOUNDARY address=%08" HWADDR_PRIx
                         " pc=%08" PRIx64 " reason=%s state-not-committed=true",
                         address,pc,o62m_te_context_reason(result));return false;
        }
        error_report("MARCONI_TE_CAPABILITY_READ address=%08" HWADDR_PRIx
                     " value=%08" PRIx64 " pc=%08" PRIx64
                     " explicit-virtual-raw-capability=true common-geometry-single-owner=true no-completion-generated=true",
                     address,*value,pc);return true;
    }
    if(s->ap_te_platform_view) {
        static const struct {uint64_t address,pc;} ap_cap_reads[]={
            {UINT64_C(0x40330008),UINT64_C(0x60e52c5c)},
            {UINT64_C(0x40330010),UINT64_C(0x60e52c68)},
            {UINT64_C(0x403300d8),UINT64_C(0x60e52c7a)},
            {UINT64_C(0x403300dc),UINT64_C(0x60e52c94)},
            {UINT64_C(0x4033000c),UINT64_C(0x60e52ca2)},
            {UINT64_C(0x40330018),UINT64_C(0x60e52cbe)},
            {UINT64_C(0x403300d0),UINT64_C(0x60e52cc8)},
            {UINT64_C(0x403300d4),UINT64_C(0x60e52cf0)},
            {UINT64_C(0x40330008),UINT64_C(0x60e52d5c)},
            {UINT64_C(0x40330004),UINT64_C(0x60e52ea8)},
            {UINT64_C(0x40330024),UINT64_C(0x60e52e1c)},
            {UINT64_C(0x4033002c),UINT64_C(0x60e52e1c)},
            {UINT64_C(0x40330034),UINT64_C(0x60e52e1c)},
            {UINT64_C(0x4033003c),UINT64_C(0x60e52e1c)},
            {UINT64_C(0x40330044),UINT64_C(0x60e52e1c)},
            {UINT64_C(0x4033004c),UINT64_C(0x60e52e1c)},
            {UINT64_C(0x40330054),UINT64_C(0x60e52e1c)},
            {UINT64_C(0x4033005c),UINT64_C(0x60e52e1c)},
            {UINT64_C(0x40330064),UINT64_C(0x60e52e1c)},
            {UINT64_C(0x4033006c),UINT64_C(0x60e52e1c)},
            {UINT64_C(0x40330074),UINT64_C(0x60e52e1c)},
            {UINT64_C(0x4033007c),UINT64_C(0x60e52e1c)},
            {UINT64_C(0x40330084),UINT64_C(0x60e52e1c)},
            {UINT64_C(0x4033008c),UINT64_C(0x60e52e1c)},
            {UINT64_C(0x40330094),UINT64_C(0x60e52e1c)},
            {UINT64_C(0x4033009c),UINT64_C(0x60e52e1c)}
        };
        uint64_t pc=marconi_mmio_pc();
        for(unsigned i=0;i<ARRAY_SIZE(ap_cap_reads);i++) {
            if(address!=ap_cap_reads[i].address||pc!=ap_cap_reads[i].pc)continue;
            uint32_t gate;uint64_t data=0;
            if(write||!marconi_ap_te_caller(s)||s->ap_te_platform_cap_reads==UINT64_MAX||
               o62m_clock_controls_read(&s->board_controls,UINT64_C(0x50050018),4,&gate)!=O62M_CLOCK_CONTROLS_OK||!(gate&2)||
               o62m_te_caps_access_v3(&s->te_capabilities,&s->te_context,address+UINT64_C(0x10000000),size,false,&data)!=O62M_TE_CONTEXT_OK)
                return false;
            *value=data;++s->ap_te_platform_cap_reads;
            marconi_public_report("MARCONI_AP_TE_PUBLIC_CAP reads=%" PRIu64 " exact-port-index=%u "
                "NS-privileged-RO=true same-existing-canonical-capabilities-and-COMMON-geometry=true "
                "guest-bitfield-parser-unchanged=true no-work-or-completion-generated=true raw-word-redacted=true physical-alias-not-proven=true",
                s->ap_te_platform_cap_reads,i);
            return true;
        }
    }
    if(s->ap_te_platform_view&&address==UINT64_C(0x40330014)) {
        uint64_t pc=marconi_mmio_pc();
        uint32_t gate;uint64_t data=0;
        if(write||(pc!=UINT64_C(0x60c51df8)&&pc!=UINT64_C(0x60e52e64))||!marconi_ap_te_caller(s)||
           s->ap_te_platform_stat_reads==UINT64_MAX||
           o62m_clock_controls_read(&s->board_controls,UINT64_C(0x50050018),4,&gate)!=O62M_CLOCK_CONTROLS_OK||!(gate&2)||
           marconi_te_global_stat_read(s,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),&data)!=O62M_TE_CONTEXT_OK)
            return false;
        *value=data;++s->ap_te_platform_stat_reads;
        marconi_public_report("MARCONI_AP_TE_PUBLIC_STAT reads=%" PRIu64
            " exact-original-AP-read=true NS-privileged-only=true same-canonical-GLOBAL-owner=true "
            "transactional-derived-status=true no-key-OTP-or-sw-init-write=true raw-word-redacted=true physical-alias-not-proven=true",
            s->ap_te_platform_stat_reads);
        return true;
    }
    if(s->te_context_fixture&&(address==O62M_TE_CONTEXT_TOP||address==O62M_TE_CONTEXT_STAT||
                              address==O62M_TE_CONTEXT_HOST0_IRQ)) {
        uint32_t gate;
        CPUARMState *env=&s->armv7m.cpu->env;
        bool privileged=arm_v7m_is_handler_mode(env)||
                        !(env->v7m.control[env->v7m.secure]&UINT32_C(1) /* CONTROL.nPRIV: architectural bit0 */);
        if(!env->v7m.secure||!privileged||
           o62m_clock_controls_read(&s->board_controls,0x50050018,4,&gate)!=O62M_CLOCK_CONTROLS_OK||!(gate&2)) {
            error_report("MARCONI_TE_CONTEXT_BOUNDARY reason=secure-privileged-original-TE-wrapper-clock-required");
            return false;
        }
        O62MTeContextPhase before=s->te_context.phase;
        int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        bool sw_before=s->trng_virtual_profile&&s->te_common_view.sw_init_done;
        O62MTeContextResult result;
        if(!write&&address==O62M_TE_CONTEXT_STAT) {
            result=marconi_te_global_stat_read(s,now,value);
        } else {
            result=s->trng_virtual_profile?
                o62m_te_common_view_access_v1(&s->te_common_view,&s->te_context,&s->trng_virtual,
                                             address,size,write,value,now):
                o62m_te_context_access(&s->te_context,address,size,write,value,now);
        }
        uint64_t pc=marconi_mmio_pc();
        if(result!=O62M_TE_CONTEXT_OK) {
            error_report("MARCONI_TE_CONTEXT_BOUNDARY access=%s address=%08" HWADDR_PRIx
                         " pc=%08" PRIx64 " reason=%s state-not-committed=true",
                         write?"write":"read",address,pc,o62m_te_context_reason(result));return false;
        }
        if(s->hash_init_profile&&!write&&address==O62M_TE_CONTEXT_HOST0_IRQ) {
            if(*value&3u)return marconi_hash_boundary(s,"COMMON-initial-HASH-SCA-pending-bit-conflict");
            *value|=o62m_hash_work_irq_aggregate_v7(&s->symc_empty);
        }
        if(s->te_context.phase!=O62M_TE_CONTEXT_READY_PHASE&&s->rnp_pool_event)marconi_rnp_pool_cancel(s);
        if(s->te_context.phase!=O62M_TE_CONTEXT_READY_PHASE&&s->aca_work_event)marconi_aca_cancel(s);
        if(before!=s->te_context.phase) {
            error_report("MARCONI_TE_CONTEXT_PHASE before=%u after=%u required-bytes=%u "
                         "capacity-bytes=%u started-ns=%" PRId64 " deadline-ns=%" PRId64 " "
                         "virtual-ns=%" PRId64 " actual-capacity-evaluation=true "
                         "geometry-workclock-timing-explicit-conditional=true full-TE=false",
                         before,s->te_context.phase,s->te_context.required_bytes,
                         s->te_context.capacity_bytes,s->te_context.started_ns,
                         s->te_context.deadline_ns,now);
        }
        if(s->trng_virtual_profile&&!sw_before&&s->te_common_view.sw_init_done) {
            error_report("MARCONI_TE_SW_INIT_DONE_WRITE address=%08" HWADDR_PRIx " value=%08" PRIx64
                         " pc=%08" PRIx64 " virtual-ns=%" PRId64
                         " original-host0-write=true real-context-ready-and-private-key-present=true "
                         "globalSTAT6-from-actual-write-STAT23-from-private-store=true other-opaque-bits-preserved=true",
                         address,*value,pc,now);
        }
        error_report("MARCONI_TE_CONTEXT_ACCESS access=%s address=%08" HWADDR_PRIx
                     " value=%08" PRIx64 " pc=%08" PRIx64 " phase=%u virtual-ns=%" PRId64,
                     write?"write":"read",address,*value,pc,s->te_context.phase,now);
        return true;
    }
    if (s->te_platform_fatal_fixture && (address==0x50333708||address==0x50330020)) {
        uint64_t pc=marconi_mmio_pc();
        if (!o62m_te_platform_fatal_access(&s->te_platform_fatal,address,size,write,pc,value)) {
            error_report("MARCONI_TE_FATAL_REJECT access=%s address=0x%08" HWADDR_PRIx
                " pc=0x%08" PRIx64 " phase=%u bounded-original-fault-path-only=true",
                write?"write":"read",address,pc,s->te_platform_fatal.phase);
            return false;
        }
        error_report("MARCONI_TE_FATAL_ACCESS access=%s address=0x%08" HWADDR_PRIx
            " value=%08" PRIx64 " pc=0x%08" PRIx64 " phase=%u "
            "explicit-initial-device-fault=true normal-boot=false no-ready-response=true",
            write?"write":"read",address,*value,pc,s->te_platform_fatal.phase);
        return true;
    }
    if (s->pll_rate_fixture) {
        size_t index;
        uint64_t backing=address;
        bool ap_pll_view=false;
        bool cp_pll_view=s->cp_created&&current_cpu==CPU(&s->cp_cpu)&&
            s->cp_cold.release_accepted&&!s->reported&&marconi_cp_cold_source(s);
        /* Only12 exact original descriptor words gain software counterpart
         * views. One existing word per pair; no PLL-bank-wide alias. */
        if(s->ota_reset||s->ap_pll_rate_view||cp_pll_view) {
            for(unsigned i=0;i<O62M_PLL_RATE_COUNT;i++) {
                if(address==o62m_pll_rate_defs[i].address-UINT64_C(0x10000000)) {
                    backing=o62m_pll_rate_defs[i].address;
                    ap_pll_view=!s->ota_reset&&s->ap_pll_rate_view&&!cp_pll_view;break;
                }
            }
        }
        if (o62m_clock_controls_index(&s->pll_rate,backing,size,&index)!=O62M_CLOCK_CONTROLS_UNKNOWN_ADDRESS) {
            uint64_t pc=marconi_mmio_pc();
            /* current_cpu's reported PC can identify the fraction block
             * prefix6ff8 (ROM descriptor LDRH), while its sole MMIO read is
             *6ffa. Permit only this evidenced prefix and fraction wordrole;
             * no CPU PC/state mutation or general caller-PC relaxation. */
            bool original_read_pc=s->ota_reset?
                (pc==0x605e930e||pc==0x605e9312||pc==0x605e9318||pc==0x605e9326||pc==0x605e9358):
                (ap_pll_view?
                    ((index%3==0&&pc==0x60c46fa6)||
                     (index%3==1&&(pc==0x60c46fc4||pc==0x60c46ffa||pc==0x60c46ff8))||
                     (index%3==2&&(pc==0x60c46fac||pc==0x60c46fb4))):
                    o62m_pll_rate_original_read_pc(pc));
            uint32_t data;
            if (write||(!original_read_pc&&!cp_pll_view)||
                o62m_clock_controls_read(&s->pll_rate,backing,size,&data)!=O62M_CLOCK_CONTROLS_OK) {
                error_report("MARCONI_PLL_RATE_REJECT access=%s address=0x%08" HWADDR_PRIx
                    " pc=0x%08" PRIx64 " only-original-rate-read-supported=true",write?"write":"read",address,pc);
                return false;
            }
            if(ap_pll_view&&s->ap_pll_rate_reads==UINT64_MAX)return false;
            *value=data;
            if(cp_pll_view)marconi_public_report("MARCONI_CP_PLL_RATE_VIEW static-word-index=%zu same-canonical-12word-owner=true original-guest-frequency-calculation=true no-new-initial-value-lock-or-ready=true",index);
            if(ap_pll_view) {
                ++s->ap_pll_rate_reads;
                marconi_public_report("MARCONI_AP_PLL_RATE_VIEW static-word-index=%zu reads=%" PRIu64 " same-TEE-canonical-explicit-12word-owner=true exact-original-role-and-bounded-reported-PC-read=true original-guest-computes-frequency=true no-new-seed-lock-ready-reset-write-or-frequency-completion=true raw-word-redacted=true",index,s->ap_pll_rate_reads);
            }
            error_report("MARCONI_PLL_RATE_READ address=0x%08" HWADDR_PRIx
                " value=%08x pc=0x%08" PRIx64
                " explicit-unknown-initial-input=true no-request-or-lock-status-model=true",address,data,pc);
            return true;
        }
    }
    if (s->thermal_observed_writes && address>=0x5a110000 && address<=0x5a110050) {
        uint64_t pc = marconi_mmio_pc();
        if (write && o62m_thermal_config_write(&s->thermal_config, address, size, *value, pc)) {
            error_report("MARCONI_THERMAL_OBSERVED_WRITE address=0x%08" HWADDR_PRIx
                " value=%08" PRIx64 " pc=0x%08" PRIx64
                " original-computed-config-retained=true measurement-IRQ-unsupported=true",address,*value,pc);
            return true;
        }
    }
    if(s->thermal_mode0_profile&&(address==UINT64_C(0x4a110018)||
       address==UINT64_C(0x4a110030)||address==UINT64_C(0x4a110034)||
       address==UINT64_C(0x5a110018)||address==UINT64_C(0x5a110030)||
       address==UINT64_C(0x5a110034)))
        return marconi_thermal_mode0_access(s,address&0xff,size,write,value);
    if(s->ap_thermal_config_view&&address>=UINT64_C(0x4a110000)&&address<=UINT64_C(0x4a110050)) {
        static const uint8_t offsets[8]={0x4c,0x2c,0x3c,0x44,0x40,0x04,0x48,0x08};
        static const uint32_t pcs[8][2]={{0x60dadec4,0x60dadf80},{0x60daded2,0},
            {0x60daded4,0x60dadfbe},{0x60daded6,0},{0x60dadf6c,0x60dadfb8},
            {0x60dadfc6,0},{0x60dae01e,0},{0x60dadf72,0x60dae022}};
        uint64_t pc=marconi_mmio_pc();
        if(write&&size==4&&*value<=UINT32_MAX&&s->ap_thermal_config_writes<UINT64_MAX) {
            for(unsigned i=0;i<G_N_ELEMENTS(offsets);++i) {
                if(address==UINT64_C(0x4a110000)+offsets[i]&&(pc==pcs[i][0]||(pcs[i][1]&&pc==pcs[i][1]))) {
                    s->thermal_config.word[i]=(uint32_t)*value;
                    s->thermal_config.written[i]=true;
                    ++s->ap_thermal_config_writes;
                    marconi_public_report("MARCONI_AP_THERMAL_CONFIG_WRITE static-word-index=%u writes=%" PRIu64 " same-TEE-canonical-config-owner=true original-guest-computed-word-retained=true no-read-measurement-ready-or-IRQ=true raw-word-redacted=true",i,s->ap_thermal_config_writes);
                    return true;
                }
            }
        }
        marconi_public_report("MARCONI_AP_THERMAL_BOUNDARY exact-config-view-only=true no-read-measurement-ready-or-IRQ=true");
        return false;
    }
    if(s->ap_efuse_read_view&&(address==0x4a300030||(address>=0x4a301000&&address<=0x4a3011fc))) {
        uint64_t pc=marconi_mmio_pc();
        if(!o62m_ap_efuse_read_view_access_v1(&s->ap_efuse_view,&s->otp_read,address,size,write,pc,value)) {
            marconi_public_report("MARCONI_AP_EFUSE_BOUNDARY reason=unproved-or-locked-read-view-access no-data-programming-ready-or-key-output=true");return false;
        }
        if(address==0x4a300030)marconi_public_report("MARCONI_AP_EFUSE_CONTROL access=%s reads=%" PRIu64 " writes=%" PRIu64 " unlocked=%u actual-original-control-command=true independent-AP30-gate=true TEE64-unchanged=true raw-word-redacted=true",write?"write":"read",s->ap_efuse_view.key_reads,s->ap_efuse_view.key_writes,s->ap_efuse_view.unlocked);
        else marconi_public_report("MARCONI_AP_EFUSE_DATA_READ static-word-index=%u reads=%" PRIu64 " actual-shared-explicit-128word-board-owner=true original-data-unknown=true no-data-copy-second-owner-or-physical-alias-claim=true raw-word-redacted=true",(unsigned)((address-0x4a301000)/4),s->ap_efuse_view.data_reads);
        return true;
    }
    if (s->otp_read_fixture && (address==0x5a300060 || address==0x5a300064 || address==0x5a300068 || address==0x5a30007c || (address>=0x5a301000 && address<=0x5a3011fc))) {
        uint64_t pc=marconi_mmio_pc();
        uint64_t commits=s->otp_program.commits;
        if(!o62m_otp_program_access_v2(&s->otp_program,&s->otp_read,address,size,write,pc,value)) {
            marconi_public_report("MARCONI_BOARD_OTP_BOUNDARY exact-original-transaction-required=true no-address-or-data-output=true");return false;
        }
        if(s->otp_program.commits!=commits)
            marconi_public_report("MARCONI_BOARD_OTP_PROGRAM commits=%" PRIu64 " actual-shared-owner-bitset=true synchronous-store=true no-address-or-data-output=true",s->otp_program.commits);
        return true;
    }
    if (s->context_observed_writes && address>=0x5c010000 && address<=0x5c0101ff) {
        uint64_t pc = marconi_mmio_pc();
        if (write && o62m_context_write(&s->context_writes, address, size, *value, pc)) {
            error_report("MARCONI_CONTEXT_OBSERVED_WRITE address=0x%08" HWADDR_PRIx
                " value=%08" PRIx64 " pc=0x%08" PRIx64
                " retained-written-only=true hardware-context-engine-unsupported=true",
                address, *value, pc);
            return true;
        }
    }
    if (s->range_configuration_fixture) {
        size_t index;
        O62MClockControlsResult selected = o62m_clock_controls_index(
            &s->range_configuration, address, size, &index);
        if (selected != O62M_CLOCK_CONTROLS_UNKNOWN_ADDRESS) {
            if (selected != O62M_CLOCK_CONTROLS_OK) { return false; }
            uint64_t pc = marconi_mmio_pc();
            if (!write && !o62m_range_configuration_read_allowed(index, pc)) {
                error_report("MARCONI_RANGE_CONFIGURATION_REJECT access=read address=0x%08"
                    HWADDR_PRIx " pc=0x%08" PRIx64 " reason=opaque-input-only-for-original-capture-PC", address, pc);
                return false;
            }
            uint32_t data = 0;
            O62MClockControlsResult result = write ?
                o62m_clock_controls_write(&s->range_configuration, address, size, *value) :
                o62m_clock_controls_read(&s->range_configuration, address, size, &data);
            if (result != O62M_CLOCK_CONTROLS_OK) {
                error_report("MARCONI_RANGE_CONFIGURATION_REJECT access=%s address=0x%08"
                    HWADDR_PRIx " reason=%s", write ? "write" : "read", address,
                    o62m_clock_controls_reason(result));
                return false;
            }
            if (!write) { *value = data; }
            error_report("MARCONI_RANGE_CONFIGURATION_ACCESS access=%s address=0x%08"
                HWADDR_PRIx " value=%08" PRIx64 " pc=0x%08" PRIx64 " opaque-capture=%u "
                "access-permission-and-IRQ-effects-unmodeled=true",
                write ? "write" : "read", address, *value, pc,
                o62m_range_configuration_snapshot_only[index]);
            return true;
        }
    }
    if (s->resource_attribute_fixture) {
        size_t index;
        if (o62m_clock_controls_index(&s->resource_attributes, address, size, &index) !=
            O62M_CLOCK_CONTROLS_UNKNOWN_ADDRESS) {
            uint32_t data = 0;
            O62MClockControlsResult result = write ?
                o62m_clock_controls_write(&s->resource_attributes, address, size, *value) :
                o62m_clock_controls_read(&s->resource_attributes, address, size, &data);
            if (result != O62M_CLOCK_CONTROLS_OK) {
                error_report("MARCONI_RESOURCE_ATTRIBUTE_REJECT access=%s address=0x%08"
                    HWADDR_PRIx " reason=%s", write ? "write" : "read", address,
                    o62m_clock_controls_reason(result));
                return false;
            }
            if (!write) { *value = data; }
            error_report("MARCONI_RESOURCE_ATTRIBUTE_ACCESS access=%s address=0x%08"
                HWADDR_PRIx " value=%08" PRIx64 " known-field-state-only=true "
                "security-polarity-and-permission-effects-unverified=true",
                write ? "write" : "read", address, *value);
            return true;
        }
    }
    if (s->board_controls_fixture) {
        size_t index;
        /* Exact original OTA/AP+98 view of the TEE28byte RMW leaf.
         * No bank-wide alias, additional writable field, or second seed. */
        uint64_t backing=address==UINT64_C(0x4c010098)?UINT64_C(0x5c010098):address;
        /* Exact AP efuse named clock-helper view of existing board owner5.
         * Preserve the same fullword/masks1,2; no bank-wide security alias. */
        bool ap_efuse_clock=s->ap_efuse_read_view&&address==UINT64_C(0x4a000404);
        if(ap_efuse_clock)backing=UINT64_C(0x5a000404);
        bool ap_te_clock=s->ap_te_platform_view&&address==UINT64_C(0x40050018);
        if(ap_te_clock) {
            uint64_t pc=marconi_mmio_pc();
            uint32_t oldword;
            uint64_t *count=write?&s->ap_te_platform_clock_writes:&s->ap_te_platform_clock_reads;
            if(!marconi_ap_te_caller(s)||*count==UINT64_MAX||
               o62m_clock_controls_read(&s->board_controls,UINT64_C(0x50050018),4,&oldword)!=O62M_CLOCK_CONTROLS_OK||
               (write? !((pc==UINT64_C(0x60c51df0)&&*value==(oldword|UINT32_C(2)))||
                         (pc==UINT64_C(0x60c51ed6)&&*value==(oldword&~UINT32_C(2)))):
                       !(pc==UINT64_C(0x60c51dea)||pc==UINT64_C(0x60c51ed0))))return false;
            backing=UINT64_C(0x50050018);
        }
        if (o62m_clock_controls_index(&s->board_controls, backing, size, &index) !=
            O62M_CLOCK_CONTROLS_UNKNOWN_ADDRESS) {
            uint32_t data = 0;
            O62MClockControlsResult result = write ?
                o62m_clock_controls_write(&s->board_controls, backing, size, *value) :
                o62m_clock_controls_read(&s->board_controls, backing, size, &data);
            if (result != O62M_CLOCK_CONTROLS_OK) {
                error_report("MARCONI_BOARD_CONTROL_REJECT access=%s address=0x%08"
                    HWADDR_PRIx " reason=%s", write ? "write" : "read", address,
                    o62m_clock_controls_reason(result));
                return false;
            }
            if (!write) { *value = data; }
            if(write&&backing==UINT64_C(0x50050018)&&!(*value&2)&&s->rnp_pool_event)marconi_rnp_pool_cancel(s);
            if(write&&backing==UINT64_C(0x50050018)&&!(*value&2)&&s->aca_work_event)marconi_aca_cancel(s);
            if(write&&backing==UINT64_C(0x5c120000))marconi_ap_tee_mbox_refresh(s);
            if(ap_te_clock) {
                if(write)++s->ap_te_platform_clock_writes;else ++s->ap_te_platform_clock_reads;
                marconi_public_report("MARCONI_AP_TE_PUBLIC_CLOCK reads=%" PRIu64 " writes=%" PRIu64
                    " canonical-board-owner-index=%zu exact-original-RMW-only=true mask2-unchanged=true "
                    "NS-privileged-only=true no-new-seed-or-ready-effect=true raw-word-redacted=true",
                    s->ap_te_platform_clock_reads,s->ap_te_platform_clock_writes,index);
            }
            if(ap_efuse_clock) {
                s->ap_efuse_clock_accesses++;
                marconi_public_report("MARCONI_AP_EFUSE_CLOCK_VIEW access=%s accesses=%" PRIu64 " canonical-board-owner-index=%zu original-masks1-and2-unchanged=true exact-AP-view-only=true no-new-seed-bankalias-or-ready-effect=true raw-word-redacted=true",write?"write":"read",s->ap_efuse_clock_accesses,index);
            }
            error_report("MARCONI_BOARD_CONTROL_ACCESS access=%s address=0x%08"
                HWADDR_PRIx " value=%08" PRIx64 " retained-control-only=true "
                "no-inferred-ready-or-hardware-side-effects=true",
                write ? "write" : "read", address, *value);
            return true;
        }
    }
    if(s->ap_domain_status_fixture&&(address==UINT64_C(0x5a0c0010)||address==UINT64_C(0x4a0c0010))) {
        uint64_t pc=marconi_mmio_pc();
        bool tee=address==UINT64_C(0x5a0c0010)&&pc==0x73e621e4&&s->armv7m.cpu->env.regs[14]==0x73e62389;
        bool ap=address==UINT64_C(0x4a0c0010)&&pc==0x60e0ecf4&&s->armv7m.cpu->env.regs[14]==0x60e0ee2d;
        if(!write||size!=4||*value!=1||(!tee&&!ap)||current_cpu!=CPU(s->armv7m.cpu)||s->reported||
           !o62m_pm_domain0_request_v1(&s->ap_domain0,1,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)))return false;
        s->ap_domain0_armed_generation=s->ap_domain0.generation;
        timer_mod_ns(s->ap_domain0_event,s->ap_domain0.deadline_ns);
        marconi_public_report("MARCONI_DOMAIN0_ON_SERVICE_REQUEST generation=%" PRIu64 " requests=%" PRIu64
            " positive-virtual-service-ns=1000 already-on-preserved=true available-cleared=true",s->ap_domain0.generation,s->ap_domain0.requests);
        return true;
    }
    if(s->ap_domain_status_fixture&&(address==UINT64_C(0x5a0c0000)||address==UINT64_C(0x4a0c0000))) {
        uint64_t pc=marconi_mmio_pc();
        bool tee=address==UINT64_C(0x5a0c0000)&&pc==0x73e621e0&&s->armv7m.cpu->env.regs[14]==0x73e62373;
        bool ap=address==UINT64_C(0x4a0c0000)&&pc==0x60e0ece2&&s->armv7m.cpu->env.regs[14]==0x60e0ee17;
        if(write||size!=4||(!tee&&!ap)||current_cpu!=CPU(s->armv7m.cpu))return false;
        *value=s->ap_domain0_guard;
        marconi_public_report("MARCONI_DOMAIN0_GUARD explicit-cold-policy=true inhibit-request=false "
            "independent-from-domain-status=true shared-AP-TEE-owner=true no-request-or-completion-effect=true");
        return true;
    }
    if(s->ap_domain_status_fixture&&(address==UINT64_C(0x4a0c0038)||address==UINT64_C(0x5a0c0038))) {
        if(write||size!=4||s->reported||!s->cp_created)return false;
        *value=s->pm_domain6_status;
        marconi_public_report("MARCONI_LTE_DOMAIN_STATUS on=%u available=%u declared-virtual-off-initial-state=true accepted-ON-transactions=0 read-does-not-advance=true",
            !!(s->pm_domain6_status&1),!!(s->pm_domain6_status&2));return true;
    }
    if(s->ap_domain_status_fixture&&address==UINT64_C(0x4a0c0028)) {
        bool cp_status=s->cp_created&&current_cpu==CPU(&s->cp_cpu)&&
            marconi_mmio_pc()==UINT64_C(0x641ac018);
        if(write||size!=4||(!marconi_ap_te_caller(s)&&!cp_status))return false;
        *value=(s->pm_domain1_available?2u:0u)|(s->pm_domain1_on?1u:0u);
        marconi_public_report("MARCONI_PM_DOMAIN1_STATE available=%u on=%u timing-written-mask=%u "
            "shared-cold-transaction-state=true read-does-not-advance=true",
            s->pm_domain1_available,s->pm_domain1_on,s->pm_domain1_timing_written);
        return true;
    }
    if (s->ap_domain_status_fixture &&
        (address == 0x5a0c0024 || address == 0x4a0c0024)) {
        if(write||size!=4)return false;
        *value=s->ap_domain0.status;++s->ap_domain_status_reads;
        if(s->ap_domain_status_reads<=8||s->ap_domain0.pending)
            marconi_public_report("MARCONI_AP_DOMAIN0_STATUS_READ pending=%u on=%u available=%u reads=%" PRIu64
                " single-owner=true read-does-not-progress-job=true",s->ap_domain0.pending,
                !!(s->ap_domain0.status&1),!!(s->ap_domain0.status&2),s->ap_domain_status_reads);
        return true;
    }
    if (s->clock_controls_fixture) {
        size_t index;
        uint64_t backing = o62m_clock_control_canonical(address);
        O62MClockControlsResult selected = o62m_clock_controls_index(
            &s->clock_controls, backing, size, &index);
        if (selected != O62M_CLOCK_CONTROLS_UNKNOWN_ADDRESS) {
            uint32_t data = 0;
            O62MClockControls trial = s->clock_controls;
            bool ap_default_write=false;
            O62MClockControlsResult result = write ?
                o62m_clock_controls_write(&trial, backing, size, *value) :
                o62m_clock_controls_read(&s->clock_controls, backing, size, &data);
            if(write&&result!=O62M_CLOCK_CONTROLS_OK) {
                uint64_t pc=marconi_mmio_pc();
                if(s->ap_runtime_config&&size==4&&
                   o62m_ap_runtime_clear_v1(backing,pc,s->clock_controls.word[index],*value)) {
                    trial.word[index]=(uint32_t)*value;
                    result=O62M_CLOCK_CONTROLS_OK;
                }
                if(s->ap_runtime_config&&size==4&&backing==UINT64_C(0x4006018c)&&
                   pc==UINT64_C(0x60c83680)&&*value==UINT64_C(0x200000)) {
                    trial.word[index]=(uint32_t)*value;
                    result=O62M_CLOCK_CONTROLS_OK;
                    ap_default_write=true;
                }
                if(result!=O62M_CLOCK_CONTROLS_OK&&o62m_ap_clock_defaults_write(&trial,backing,size,*value,pc)) {
                    result=O62M_CLOCK_CONTROLS_OK;
                    ap_default_write=true;
                }
            }
            if (result != O62M_CLOCK_CONTROLS_OK) {
                error_report("MARCONI_CLOCK_CONTROLS_REJECT access=%s address=0x%08"
                    HWADDR_PRIx " reason=%s", write ? "write" : "read", address,
                    o62m_clock_controls_reason(result));
                return false;
            }
            if (write) {
                bool tx_clock = s->uart_tx_fixture &&
                    (backing == 0x4a09000c || backing == 0x4a0900e4);
                if (tx_clock) {
                    marconi_tx_refresh(s);
                    if (backing == 0x4a09000c &&
                        ((s->uart_clock ^ *value) & 0x703) &&
                        (s->tx.active || s->tx.count)) {
                        return false;
                    }
                }
                if (s->timer64 && (backing == 0x4006005c || backing == 0x400600f8)) {
                    O62MTimer64 timer_trial;
                    uint32_t csr, pclk;
                    if (!marconi_timer64_refresh(s)) { return false; }
                    timer_trial = s->timer64_model;
                    o62m_boot_clock_read(&s->boot_clocks, 0x4006005c, &csr);
                    o62m_boot_clock_read(&s->boot_clocks, 0x400600f8, &pclk);
                    if (backing == 0x4006005c) { csr = *value; }
                    else { pclk = *value; }
                    if (!marconi_timer64_clock(s, csr, pclk, &timer_trial)) { return false; }
                    if (s->ap_timer64_view) {
                        timer_trial = s->dvfs_timer64_model;
                        if (!marconi_timer64_clock(s, csr, pclk, &timer_trial)) return false;
                    }
                }
                if(s->timer0_runtime_fixture&&(backing==0x40060058||backing==0x400600f4)) {
                    if(!marconi_timer0_runtime_refresh(s)) return false;
                    O62MShotTimer timer_trial=s->timer0_runtime;uint32_t csr,pclk;
                    marconi_timer0_runtime_clock_words(s,&csr,&pclk);
                    if(backing==0x40060058)csr=(uint32_t)*value;else pclk=(uint32_t)*value;
                    O62MShotResult r=marconi_timer0_runtime_clock(s,&timer_trial,csr,pclk);
                    if(r!=O62M_SHOT_OK) {
                        error_report("MARCONI_TIMER0_CLOCK_REJECT reason=%s",o62m_shot_timer_reason(r));return false;
                    }
                }
                if(!marconi_timer4014_clock_preflight(s,&trial,backing,(uint32_t)*value)) {return false;}
                s->clock_controls = trial;
                if(backing==UINT64_C(0x40060064)||backing==UINT64_C(0x400600e8))marconi_wdt4012_timing_report(s,"gate-write");
                marconi_clock_control_mirror(s, backing, (uint32_t)*value);
                if(!marconi_timer4014_refresh(s)) {return false;}
                if(ap_default_write) {
                    error_report("MARCONI_AP_CLOCK_DEFAULT_WRITE address=0x%08" PRIx64
                        " value=%08" PRIx64 " pc=0x%08" PRIx64
                        " exact-original-ROM-pair=true scoped-compound-control-write=true",backing,*value,
                        marconi_mmio_pc());
                }
                if(!marconi_timer0_runtime_refresh(s)) return false;
                if (tx_clock) { marconi_tx_refresh(s); }
                if (!marconi_timer64_refresh(s)) { return false; }
            }
            if (!write) {
                *value = data;
                ++s->clock_controls_reads;
            } else {
                ++s->clock_controls_writes;
            }
            error_report("MARCONI_CLOCK_CONTROLS_ACCESS access=%s address=0x%08"
                HWADDR_PRIx " backing=0x%08" PRIx64 " value=%08" PRIx64
                " reads=%" PRIu64 " writes=%" PRIu64
                " retained-control-state-only=true clock-ready-unmodeled=true",
                write ? "write" : "read", address, backing, *value,
                s->clock_controls_reads, s->clock_controls_writes);
            return true;
        }
    }
    if (s->aon_prediv_query_fixture) {
        bool selected = false;
        for (unsigned i = 0; i < 26; ++i) {
            if (address == o62m_aon_prediv_query_address(i)) { selected = true; break; }
        }
        if (selected) {
            if (write) {
                error_report("MARCONI_AON_PREDIV_REJECT access=write address=0x%08"
                    HWADDR_PRIx " reason=read-only-projection", address);
                return false;
            }
            uint32_t data = 0;
            O62MAonPredivQueryResult result = o62m_aon_prediv_query_read(
                &s->aon_prediv_query, address, size, &data);
            if (result != O62M_AON_PREDIV_QUERY_OK) {
                error_report("MARCONI_AON_PREDIV_REJECT access=read address=0x%08"
                    HWADDR_PRIx " phase=%u reason=%s", address, s->aon_prediv_query.phase,
                    o62m_aon_prediv_query_reason(result));
                return false;
            }
            *value = data;
            error_report("MARCONI_AON_PREDIV_ACCESS address=0x%08" HWADDR_PRIx
                " value=%08x phase=%u explicit-word-query-only=true",
                address, data, s->aon_prediv_query.phase);
            if (s->aon_prediv_query.phase == 26) {
                error_report("MARCONI_AON_PREDIV_SEQUENCE_OBSERVED reads=26 "
                    "clock-gate-effects-PLL-ready-and-availability-unmodeled=true");
            }
            return true;
        }
    }
    if (s->aon144_read_fixture && address == 0x5a098144) {
        if (write) {
            error_report("MARCONI_AON144_READ_REJECT access=write reason=read-only-projection");
            return false;
        }
        uint32_t data = 0;
        O62MAon144ReadResult result = o62m_aon144_read(
            &s->aon144_read_projection, address, size, &data);
        if (result != O62M_AON144_READ_OK) {
            error_report("MARCONI_AON144_READ_REJECT access=read reason=%s",
                o62m_aon144_read_reason(result));
            return false;
        }
        *value = data;
        error_report("MARCONI_AON144_READ_OBSERVED value=%08x "
            "independent-explicit-word=true gate-query-only=true "
            "PLL-lock-clock-availability-and-ready-unmodeled=true", data);
        return true;
    }
    if(s->timer0_runtime_fixture&&address>=0x50030000&&address<=0x50030020) {
        uint32_t csr,pclk,data=0;
        marconi_timer0_runtime_clock_words(s,&csr,&pclk);
        if(!(pclk&0x20000u)) return false; /* unproved bus response while PCLK disabled */
        if(!marconi_timer0_runtime_refresh(s)) return false;
        O62MShotResult r=write?o62m_shot_timer_write(&s->timer0_runtime,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),address-0x50030000,size,*value):
            o62m_shot_timer_read(&s->timer0_runtime,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                address-0x50030000,size,&data);
        if(r!=O62M_SHOT_OK) {
            error_report("MARCONI_TIMER0_RUNTIME_REJECT access=%s offset=0x%02" HWADDR_PRIx
                " reason=%s",write?"write":"read",address-0x50030000,o62m_shot_timer_reason(r));
            return false;
        }
        if(!write) *value=data;
        error_report("MARCONI_TIMER0_RUNTIME_ACCESS access=%s offset=0x%02" HWADDR_PRIx
            " value=%08" PRIx64 " virtual-ns=%" PRId64 " expiry-count=%" PRIu64,
            write?"write":"read",address-0x50030000,*value,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),s->timer0_runtime.expirations);
        return marconi_timer0_runtime_refresh(s);
    }
    if (s->timer0_stop_fixture && address == 0x50030010) {
        uint32_t data = 0;
        O62MTimer0StopResult result = write ?
            o62m_timer0_stop_write(&s->timer0_stop_projection, address, size, *value) :
            o62m_timer0_stop_read(&s->timer0_stop_projection, address, size, &data);
        if (result != O62M_TIMER0_STOP_OK) {
            error_report("MARCONI_TIMER0_STOP_REJECT access=%s address=0x%08"
                HWADDR_PRIx " phase=%u reason=%s",
                write ? "write" : "read", address, s->timer0_stop_projection.phase,
                o62m_timer0_stop_reason(result));
            return false;
        }
        if (!write) { *value = data; }
        error_report("MARCONI_TIMER0_STOP_ACCESS access=%s value=0x%08" PRIx64
            " phase=%u retained-control-only=true",
            write ? "write" : "read", *value, s->timer0_stop_projection.phase);
        if (s->timer0_stop_projection.phase == 2) {
            error_report("MARCONI_TIMER0_STOP_SEQUENCE_OBSERVED initial=%08x word=%08x "
                "counter-busy-clock-IRQ-or-reset-implemented=false",
                s->timer0_stop_projection.initial, s->timer0_stop_projection.word);
        }
        return true;
    }
    if (s->clock48_fixture) {
        bool selected = false;
        for (unsigned i = 0; i < 5; ++i) {
            if (address == o62m_clock48_address(i)) { selected = true; break; }
        }
        if (selected) {
            uint32_t data = 0;
            unsigned before = s->clock48_projection.phase;
            O62MClock48Result result = write ?
                o62m_clock48_projection_write(&s->clock48_projection, address,
                                               size, (uint32_t)*value) :
                o62m_clock48_projection_read(&s->clock48_projection, address, size, &data);
            if (result != O62M_CLOCK48_OK) {
                error_report("MARCONI_CLOCK48_REJECT access=%s address=0x%08"
                    HWADDR_PRIx " phase=%u reason=%s",
                    write ? "write" : "read", address, s->clock48_projection.phase,
                    o62m_clock48_reason(result));
                return false;
            }
            if (!write) { *value = data; }
            error_report("MARCONI_CLOCK48_ACCESS access=%s address=0x%08"
                HWADDR_PRIx " value=0x%08" PRIx64 " phase=%u "
                "logical-control-projection-only=true clocks-or-ready-unmodeled=true",
                write ? "write" : "read", address, *value, s->clock48_projection.phase);
            if (before != 5 && s->clock48_projection.phase == 5) {
                error_report("MARCONI_CLOCK48_SEQUENCE_OBSERVED words=%08x:%08x:%08x:%08x:%08x "
                    "functional-clock-frequency-IRQ-or-ready-implemented=false",
                    s->clock48_projection.word[0], s->clock48_projection.word[1],
                    s->clock48_projection.word[2], s->clock48_projection.word[3],
                    s->clock48_projection.word[4]);
            }
            return true;
        }
    }
    if(s->pubap_quiesce_event&&(address==0x4a8d002c||address==0x4a8d001c)) {
        uint64_t pc=marconi_mmio_pc();
        if(size!=4||!marconi_ap_te_caller(s)||!s->dfs_config.enabled||s->dfs_config.phase!=16||
           !marconi_pubap_quiesce_domain(s)) {
            marconi_pubap_quiesce_reset(s);return false;
        }
        uint32_t *word=&s->dfs_config.retained[DFS_CONFIG_PUBAP][DFS_CONFIG_SEED_2C];
        if(address==0x4a8d002c) {
            if(!write) {
                if(pc!=0x60c5550a)return false;
                *value=*word;
            } else {
                O62MPubapQuiesceV1 trial=s->pubap_quiesce;
                int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                if(pc!=0x60c55518||*value>UINT32_MAX||(*value&15)!=3||
                   ((*value^*word)&~UINT64_C(15))||
                   !(s->dfs_config.written_mask[DFS_CONFIG_PUBAP]&(1u<<DFS_CONFIG_WRITE_00))||
                   s->dfs_config.observed[DFS_CONFIG_PUBAP][DFS_CONFIG_WRITE_00]!=1||
                   !o62m_pubap_quiesce_start_v1(&trial,now))return false;
                *word=(uint32_t)*value;s->pubap_quiesce=trial;
                s->pubap_quiesce_armed_generation=trial.generation;
                timer_mod_ns(s->pubap_quiesce_event,trial.deadline_ns);
                marconi_public_report("MARCONI_PUBAP_QUIESCE_REQUEST generation=%" PRIu64
                    " virtual-ns=%" PRId64 " deadline-ns=%" PRId64
                    " actual-AP-write=true same-TEE-word=true high28-preserved=true TEE-history-preserved=true",
                    trial.generation,now,trial.deadline_ns);
            }
        } else {
            uint32_t status;
            if(write||pc!=0x60c55526||!o62m_pubap_quiesce_status_v1(&s->pubap_quiesce,&status))return false;
            *value=status;
            marconi_public_report("MARCONI_PUBAP_QUIESCE_STATUS generation=%" PRIu64
                " pending=%u valid=%u inert-projection=true no-poll-driven-work=true",
                s->pubap_quiesce.generation,s->pubap_quiesce.pending,s->pubap_quiesce.valid);
        }
        return true;
    }
    if(s->dfs_config_fixture&&s->dfs_config.phase==16&&write&&address==0x5a8d002c&&
       marconi_lps_pm_caller(s,0x32006276,0x73e604bb)) {
        uint32_t *word=&s->dfs_config.retained[DFS_CONFIG_PUBAP][DFS_CONFIG_SEED_2C];
        uint32_t next=(*word&UINT32_C(0xfffffff0))|1;
        if(size!=4||!s->dfs_config.enabled||*value!=next||
           s->pubap_restore_count==ARRAY_SIZE(s->pubap_restore_history)||
           !marconi_psram_controller_source(s,true)||!s->psram_controller.operating)return false;
        *word=(uint32_t)*value;s->pubap_restore_history[s->pubap_restore_count++]=(uint32_t)*value;
        marconi_pubap_quiesce_reset(s);
        marconi_public_report("MARCONI_PUBAP_RESTORE_WRITE writes=%u actual-retained-vote=true "
            "prior-quiesce-invalidated=true requests-unconsumed=true no-new-transition-status-or-IRQ=true",
            s->pubap_restore_count);return true;
    }
    if(s->dfs_config_fixture&&s->dfs_config.phase==16&&current_cpu==CPU(s->armv7m.cpu)) {
        static const struct {uint32_t address,read_lr,write_lr;} repeat[]={
            {0x5a9b0004,0x73e598b3,0x73e59951},{0x5a9b0008,0x73e598d7,0x73e59959},
            {0x5a9b000c,0,0x73e59805},{0x5a9b0010,0,0x73e5995d},
            {0x5a9b0014,0,0x73e59849},{0x5a9b0018,0,0x73e59961},
            {0x5a9b0000,0,0x73e59ab1},{0x5a9b002c,0x73e5975d,0x73e59ab7}};
        for(unsigned i=0;i<ARRAY_SIZE(repeat);++i) {
            if(address!=repeat[i].address||
               !marconi_lps_pm_caller(s,write?0x32006276:0x32006268,write?repeat[i].write_lr:repeat[i].read_lr))continue;
            if(size!=4||!marconi_psram_controller_source(s,true)||!s->psram_controller.operating)return false;
            uint32_t data;
            if(!o62m_pubcp_dfs_repeat_v1(&s->dfs_config,&s->pubcp_dfs_repeat,write,
                (uint32_t)(address-0x5a9b0000),*value,&data))return false;
            if(!write)*value=data;
            marconi_public_report("MARCONI_PUBCP_DFS_RECONFIG access=%s static-word-index=%u writes=%u "
                "first-boot-phase=%u sole-retained-owner=true requests-unconsumed=true raw-data-redacted=true",
                write?"write":"read",i,s->pubcp_dfs_repeat.step,s->dfs_config.phase);return true;
        }
    }
    if (s->dfs_config_fixture) {
        /* Current scope is the two secure TEE addresses actually reached.
         * No physical/nonsecure alias, status or transition is invented. */
        unsigned domain;
        hwaddr base;
        if (address >= 0x5a8d0000 && address < 0x5a8d1000) {
            domain = DFS_CONFIG_PUBAP;
            base = 0x5a8d0000;
        } else if (address >= 0x5a9b0000 && address < 0x5a9b1000) {
            domain = DFS_CONFIG_PUBCP;
            base = 0x5a9b0000;
        } else {
            domain = DFS_CONFIG_DOMAIN_COUNT;
            base = 0;
        }
        if (domain != DFS_CONFIG_DOMAIN_COUNT) {
            uint32_t data = 0;
            uint32_t before = s->dfs_config.phase;
            enum dfs_config_result result = write ?
                dfs_config_write(&s->dfs_config, domain, address - base, size, *value) :
                dfs_config_read(&s->dfs_config, domain, address - base, size, &data);
            if (result != DFS_CONFIG_OK) {
                error_report("MARCONI_DFS_CONFIG_REJECT access=%s address=0x%08"
                    HWADDR_PRIx " phase=%u reason=%s ready-never-generated=true",
                    write ? "write" : "read", address, s->dfs_config.phase,
                    dfs_config_result_name(result));
                return false;
            }
            if (!write) { *value = data; }
            error_report("MARCONI_DFS_CONFIG_ACCESS access=%s address=0x%08"
                HWADDR_PRIx " value=0x%08" PRIx64 " phase=%u "
                "retained-input-or-write-observation=true physical-effects-unmodeled=true",
                write ? "write" : "read", address, *value, s->dfs_config.phase);
            if (before != 16 && s->dfs_config.phase == 16) {
                error_report("MARCONI_DFS_CONFIG_SEQUENCE_OBSERVED writes=16 "
                    "PUBAP04=0x%08x PUBAP08=0x%08x PUBAP2c=0x%08x "
                    "PUBCP04=0x%08x PUBCP08=0x%08x PUBCP2c=0x%08x "
                    "DFS-transition-or-ready-implemented=false",
                    s->dfs_config.retained[0][0], s->dfs_config.retained[0][1],
                    s->dfs_config.retained[0][2], s->dfs_config.retained[1][0],
                    s->dfs_config.retained[1][1], s->dfs_config.retained[1][2]);
            }
            return true;
        }
    }
    if(s->soc4005_config_inputs||s->ap_soc4005_2c0_initial) {
        size_t index;
        if(o62m_clock_controls_index(&s->soc4005_config,address,size,&index)!=O62M_CLOCK_CONTROLS_UNKNOWN_ADDRESS) {
            uint64_t pc=marconi_mmio_pc();
            bool original_pc=s->ap_soc4005_2c0_initial?
                ((address==UINT64_C(0x400502c0)&&pc==(write?UINT64_C(0x60d30cc2):UINT64_C(0x60d30cb6)))||
                 (address==UINT64_C(0x400500b4)&&pc==(write?UINT64_C(0x60c3942a):UINT64_C(0x60c39422)))):
                o62m_soc4005_config_original_pc_v1(address,pc,write);
            if(!original_pc||(s->ap_soc4005_2c0_initial&&s->ap_soc4005_2c0_accesses==UINT64_MAX)) {
                error_report("MARCONI_SOC4005_CONFIG_REJECT address=%08" HWADDR_PRIx
                             " reason=unproved-original-access-pc pc=%08" PRIx64,address,pc);return false;
            }
            uint32_t data=0;
            int result=write?o62m_clock_controls_write(&s->soc4005_config,address,size,*value):
                             o62m_clock_controls_read(&s->soc4005_config,address,size,&data);
            if(result) {error_report("MARCONI_SOC4005_CONFIG_REJECT address=%08" HWADDR_PRIx
                " reason=%s",address,o62m_clock_controls_reason(result));return false;}
            if(!write) {*value=data;}
            if(s->ap_soc4005_2c0_initial&&address==0x400500b4) {
                marconi_public_report("MARCONI_AP_SOC4005_B4_CONFIG access=%s same-canonical-owner=true low2-retained=true no-completion=true",write?"write":"read");
                return true;
            }
            if(s->ap_soc4005_2c0_initial) {
                ++s->ap_soc4005_2c0_accesses;
                marconi_public_report("MARCONI_AP_SOC4005_2C0_CONFIG access=%s accesses=%" PRIu64 " same-old-definition-subset-owner=true independent-fullword-initial=true only-original-ff00-field-RMW=true A4-unchanged=true B4-independent-canonical-config=true no-ready-or-device-completion=true raw-word-redacted=true",write?"write":"read",s->ap_soc4005_2c0_accesses);
            }
            /* A4 has only this active backing in OTA when enabled. The
             * legacy A4 projection below is never read or written here. */
            error_report("MARCONI_SOC4005_CONFIG_ACCESS access=%s address=%08" HWADDR_PRIx
                         " value=%08" PRIx64 " pc=%08" PRIx64
                         " retained-config-only=true hardware-effects-and-ready-unmodeled=true",
                         write?"write":"read",address,*value,pc);
            return true;
        }
    }
    if (s->qos_observed_write_experiment &&
        address >= 0x400500a0 && address <= 0x400500a8) {
        if (address == 0x400500a8) {
            /* No reset, mask, status or ready value is inferred. This opt-in
             * hypothesis accepts ONLY the transaction shared by AP/TEE/CP.
             * No readback and no silicon side effect is claimed. */
            if (!write || *value != 0) { return false; }
            ++s->qos_a8_zero_writes;
            error_report("MARCONI_CONDITIONAL_QOS_A8 observed-zero-write=%" PRIu64
                         " semantics=unknown side-effects=unmodeled "
                         "conditional-reachability-only=true",
                         s->qos_a8_zero_writes);
        } else {
            unsigned index = (address - 0x400500a0) / 4;
            /* AP psramqosprint identifies all eight 4-bit master-priority
             * fields in each word. Retain actual writes; an unwritten
             * register has an unknown reset value and cannot be read. */
            if (write) {
                s->qos_priority[index] = *value;
                s->qos_priority_written[index] = true;
            } else {
                if (!s->qos_priority_written[index]) { return false; }
                *value = s->qos_priority[index];
            }
            error_report("MARCONI_CONDITIONAL_QOS_PRIORITY access=%s "
                         "address=0x%08" HWADDR_PRIx " value=0x%08" PRIx64
                         " arbitration=unmodeled", write ? "write" : "read",
                         address, *value);
        }
        return true;
    }
    if (s->adc_fixture && address >= 0x4a150000 && address <= 0x4a15ffff &&
        !(write && address == 0x4a150000 && *value == 0x00880000)) {
        uint16_t regno = address - 0x4a150000, sample;
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if (!(s->ctrl54 & 0x10) || (s->spmi_ctrl & 0x44) != 0x44) { return false; }
        if (s->select_pending || s->pmic_write_pending) {
            if (write) { return false; }
            *value = 0x200000; /* busy prior command, not valid PMIC data */
        } else if (!s->slave_selected) {
            return false;
        } else if (write) {
            if(now<0||s->transaction_ns>(uint64_t)(INT64_MAX-now)) {
                error_report("MARCONI_SPMI_WRITE_BOUNDARY reason=virtual-deadline-overflow queue-unchanged=true");return false;
            }
            int64_t deadline=now+(int64_t)s->transaction_ns;
            O62MPmicAdc trial = s->pmic_adc;
            if((*value & 0xffff0000)!=0x00180000) {return false;}
            if(s->rtc32k_reference_profile&&regno==0x80) {
                O62MRTCCalibrationV1 cal_trial=s->rtc_calibration;
                uint32_t window=((uint32_t)s->rtc_calibration_config[1]<<16)|s->rtc_calibration_config[2];
                if((*value&UINT16_MAX)!=1||!o62m_rtc_calibration_start_v2(&cal_trial,
                    s->rtc_calibration_config[0],s->rtc_calibration_config_written,window,&s->rtc32k_reference,now))return false;
            } else if(s->rtc32k_reference_profile&&(regno==0x84||regno==0x88||regno==0x8c)) {
                if(regno==0x84&&(((*value&UINT16_MAX)^s->rtc_calibration_config[0])&~UINT32_C(0x1f)))return false;
            } else if(marconi_pmic_button_port(s,regno)) {
                if(!marconi_pmic_button_validate(s,regno,*value&UINT16_MAX))return false;
            } else if(s->pmic_boot_events_profile&&o62m_pmic_boot_event_kind_v6(regno,&(unsigned){0})) {
                if(!o62m_pmic_boot_events_validate_v6(&s->pmic_boot_events,regno,*value&UINT16_MAX))return false;
            } else if(s->pmic_mode_extended&&(regno==0x118||regno==0x11c)) {
                if(regno==0x118&&(*value&UINT16_MAX)>0xff)return false;
            } else if(s->pmic_mode_config_inputs&&marconi_pmic_mode_selected(s,regno,&(size_t){0})) {
                O62MClockControls modes_trial=s->pmic_mode_config;
                int result=s->pmic_mode_extended?o62m_pmic_mode_write_v4(&modes_trial,regno,*value&0xffff):
                    o62m_clock_controls_write(&modes_trial,regno,4,*value&0xffff);
                if(result!=O62M_CLOCK_CONTROLS_OK)return false;
            } else if(s->virtual_pmic_bc_config&&regno==0xbc) {
                if(!marconi_pmic_bc_validate(s,*value&0xffff))return false;
            } else if(s->virtual_auxadc_reset_profile&&o62m_virtual_auxadc_reset_supports_write_v1(regno)) {
                O62MVirtualAuxadcResetV1 reset_trial=s->virtual_auxadc_reset;
                if(s->auxadc_channel_profile&&!o62m_pmic_adc_ap_advance_v1(&trial,now))return false;
                if(!s->auxadc_channel_profile)o62m_pmic_adc_update(&trial,now);
                if(!o62m_virtual_auxadc_reset_apply_v1(&reset_trial,&trial,regno,*value&0xffff))return false;
            } else if(s->pmic_regulator_inputs&&o62m_pmic_regulator_supports_v1(regno)) {
                O62MPmicRegulatorsV1 regulator_trial=s->pmic_regulators;
                int result=marconi_pmic_regulator_apply(&regulator_trial,&trial,regno,*value&0xffff,now);
                if(result) {
                    if(s->pmic_alias_read_policy&&o62m_pmic_regulator_alias_index_v1(regno)>=0)
                        marconi_public_report("MARCONI_PMIC_ALIAS_WRITE_REJECT static-alias-index=%d validation-code=%d zero-command=%u raw-value-redacted=true canonical-state-unchanged=true",o62m_pmic_regulator_alias_index_v1(regno),result,(*value&0xffff)==0);
                    error_report("MARCONI_PMIC_REGULATOR_WRITE_BOUNDARY reason=%s reg=%04x word=%04x",
                    o62m_pmic_regulator_reason_v1(result),regno,(unsigned)(*value&0xffff));return false;}
            } else if(s->normal_emmc_bound&&regno==0x1328) {
                if(!marconi_pmic_config1328_validate(*value&UINT16_MAX))return false;
            } else if(s->pmic_clock_inputs&&(regno==0x0504||regno==0x0a04)) {
                int result=o62m_pmic_clock_set_validate_v3(&s->pmic_clocks,regno,*value&UINT16_MAX);
                marconi_public_report("MARCONI_CP_PMIC_CLOCK_SET_ADMISSION accepted=%u clear=%u static-field-match=%u queued-work-only=true raw-value-redacted=true",result==0,regno==0x0a04,(*value&UINT16_MAX)==0x0400);
                if(result)return false;
            } else if(s->pmic_clock_inputs&&o62m_pmic_clock_address_v2(regno)) {
                uint32_t data=*value&0xffff;
                int result=o62m_pmic_clock_validate_write_v2(&s->pmic_clocks,regno,data);
                if(result) {error_report("MARCONI_PMIC_CLOCK_WRITE_BOUNDARY reason=%s reg=%04x word=%04x",
                                        o62m_pmic_clock_reason_v2(result),regno,data);return false;}
                if(regno==0x001c&&s->pmic_adc.pending&&data!=s->pmic_clocks.word[2]) {
                    error_report("MARCONI_PMIC_CLOCK_WRITE_BOUNDARY reason=active-ADC-gate-divider-change-unimplemented");return false;
                }
            } else if(s->pmic1000_initial&&regno==0x1000) {
                int result=o62m_pmic1000_validate_write_v1(&s->pmic1000,*value&0xffff);
                if(result) {
                    error_report("MARCONI_PMIC1000_WRITE_BOUNDARY reason=%s word=%04" PRIx64,
                                 o62m_pmic1000_reason_v1(result),*value&0xffff);return false;
                }
            } else if((s->virtual_auxadc_reset_profile&&s->virtual_auxadc_reset.asserted)||
                      !marconi_adc_core_write(s,&trial,regno,*value&0xffff,now)) {
                marconi_public_report("MARCONI_SPMI_WRITE_CLASS_BOUNDARY known-0504=%u format-slave8=true no-parameter-output=true",regno==0x0504);
                return false;
            }
            s->pmic_write_pending = true;
            s->pmic_write_dvs_owned = false;
            s->pmic_write_reg = regno;
            s->pmic_write_value = s->pmic_sync107c_profile&&regno==UINT16_C(0x107c)?
                o62m_pmic_sync107c_normalize_v1((uint16_t)*value):(uint16_t)*value;
            s->pmic_write_deadline = deadline;
        } else {
            /* A functional synchronous read transaction. Completion means the
             * modeled selected slave actually supplied a supported register.
             * No claim of physical SPMI read latency is made. */
            if(marconi_pmic_button_port(s,regno)) {
                bool second=regno==UINT16_C(0x107c),third=regno==UINT16_C(0x10ac);
                unsigned index=third?2:second?1:0;
                uint64_t *reads=third?&s->pmic_button_second_followup_reads:second?&s->pmic_button_followup_reads:&s->pmic_button_reads;
                if(*reads==UINT64_MAX)return false;
                sample=third?s->pmic_button_second_followup_word:second?s->pmic_button_followup_word:s->pmic_button_config_word;++*reads;
                if(second&&s->pmic_sync107c_profile) {
                    uint16_t projected;
                    if(o62m_pmic_sync107c_project_v1(&s->pmic_sync107c,(uint16_t)sample,&projected)!=O62M_PMIC_SYNC107C_OK_V1)return false;
                    sample=projected;
                    if(s->pmic_sync107c.pending)++s->pmic_sync107c_pending_reads;
                }
                if(*reads<=32)marconi_public_report("MARCONI_PMIC_BUTTON_CONFIG_READ port-index=%u reads=%" PRIu64
                    " same-independent-retained-full16-backing=true explicit-initial-or-actual-prior-bus-commit=true "
                    "no-button-sample-status-or-IRQ-inferred=true raw-word-redacted=true",index,*reads);
            } else if(s->pmic_vbus_input&&regno==0x1080) {
                sample=0;++s->pmic_vbus_reads;
                if(s->pmic_vbus_reads<=16)marconi_public_report("MARCONI_PMIC_VBUS_INPUT_READ reads=%" PRIu64
                    " explicit-virtual-VBUS-offline=true fullword-zero-input-policy=true selected-peer-input-only=true no-charge-ready-or-IRQ=true",s->pmic_vbus_reads);
            } else if(s->auxadc_channel_profile&&regno==0xcc) {
                sample=o62m_pmic_adc_ap_irq_ready_v1(&s->pmic_adc,s->pmic_bc_word)?2:0;
                s->auxadc_cc_reads++;
                if(s->auxadc_cc_reads<=32)marconi_public_report("MARCONI_AUXADC_CC_SOURCE_READ reads=%" PRIu64 " actual-derived-ADC-slot1-pending=%u other-unmodeled-sources-quiescent-profile=true no-read-clear-or-physical-registerclaim=true",s->auxadc_cc_reads,sample!=0);
            } else if(s->pmic_alias_read_policy&&o62m_pmic_regulator_alias_index_v1(regno)>=0) {
                if(s->pmic_alias_reads==UINT64_MAX)return false;
                sample=0;s->pmic_alias_reads++;
                marconi_public_report("MARCONI_PMIC_ALIAS_COMMAND_READ static-alias-index=%d reads=%" PRIu64 " explicit-read-as-zero-functional-policy=true no-alias-backing-or-base-mirror=true canonical-enable-state-unchanged-by-read=true physical-read-semantics-unproved=true no-workcompletion-or-IRQ-generated=true",o62m_pmic_regulator_alias_index_v1(regno),s->pmic_alias_reads);
            } else if(s->pmic_boot_events_profile&&o62m_pmic_boot_event_kind_v6(regno,&(unsigned){0})) {
                if(!o62m_pmic_boot_events_read_v6(&s->pmic_boot_events,regno,&sample))return false;
                unsigned index,kind=o62m_pmic_boot_event_kind_v6(regno,&index);
                marconi_public_report("MARCONI_PMIC_BOOT_EVENTS_READ kind=%u index=%u explicit-virtual-or-actual-written-peer-data=true no-event-or-completion-inferred=true",kind,index);
            } else if(s->pmic_mode_extended&&(regno==0x118||regno==0x11c)) {
                unsigned i=regno==0x11c?0:1;
                if(!(s->pmic_threshold_written&(1u<<i)))return false;
                sample=s->pmic_threshold_words[i];
            } else if(s->pmic_mode_config_inputs&&marconi_pmic_mode_selected(s,regno,&(size_t){0})) {
                size_t index;uint32_t data;marconi_pmic_mode_selected(s,regno,&index);
                if(o62m_clock_controls_read(&s->pmic_mode_config,regno,4,&data)!=O62M_CLOCK_CONTROLS_OK||data>UINT16_MAX)return false;
                sample=data;s->pmic_mode_reads++;
                marconi_public_report("MARCONI_PMIC_MODE_CONFIG_READ static-word-index=%zu reads=%" PRIu64 " actual-independent-retained-fields=true raw-word-redacted=true no-GO-ready-or-active-mode-inferred=true",index,s->pmic_mode_reads);
            } else if(s->virtual_pmic_bc_config&&regno==0xbc) {
                if(s->pmic_bc_reads==UINT64_MAX)return false;
                sample=s->pmic_bc_word;s->pmic_bc_reads++;
                marconi_public_report("MARCONI_PMIC_BC_CONFIG_READ reads=%" PRIu64 " actual-independent-retained-word=true no-CC-status-or-ADC-ready-mirror=true raw-word-redacted=true",s->pmic_bc_reads);
            } else if(s->virtual_auxadc_reset_profile&&regno==0x294) {
                if(!o62m_virtual_auxadc_reset_read_v1(&s->virtual_auxadc_reset,&sample))return false;
                marconi_public_report("MARCONI_VIRTUAL_AUXADC_294_READ reads=%" PRIu64 " actual-independent-explicit-word=true only-first-reset-consumer-discard-proven=true later-consumer-sideeffects-unproved=true no-ADC-sample-or-ready-inferred=true raw-word-redacted=true",s->virtual_auxadc_reset.reads);
            } else if(s->pmic_regulator_inputs&&o62m_pmic_regulator_supports_v1(regno)) {
                int result=o62m_pmic_regulator_read_v1(&s->pmic_regulators,regno,&sample);
                if(result) {error_report("MARCONI_PMIC_REGULATOR_READ_BOUNDARY reason=%s reg=%04x",
                    o62m_pmic_regulator_reason_v1(result),regno);return false;}
                if(regno==0x68) {assert(s->pmic_adc.control_68==sample);}
                error_report("MARCONI_PMIC_REGULATOR_READ reg=%04x word=%04x writes=%" PRIu64
                             " virtual-ns=%" PRId64 " actual-selected-slave8-data=true "
                             "independent-full16word-or-retained-config=true no-voltage-feedback-or-ready-generated=true",
                             regno,sample,s->pmic_regulators.writes,now);
            } else if(s->pmic_clock_inputs&&o62m_pmic_clock_address_v2(regno)) {
                int result=o62m_pmic_clock_read_v2(&s->pmic_clocks,regno,&sample);
                if(result) {error_report("MARCONI_PMIC_CLOCK_READ_BOUNDARY reason=%s",o62m_pmic_clock_reason_v2(result));return false;}
                if(regno==0x001c) {assert(s->pmic_adc.config_1c==sample);}
                error_report("MARCONI_PMIC_CLOCK_READ reg=%04x word=%04x writes=%" PRIu64 " virtual-ns=%" PRId64
                             " real-selected-slave8-data=true six-independent-initial-full16words=true "
                             "same-word-shared-aliases=true physical-clock-effects-unmodeled=true",
                             regno,sample,s->pmic_clocks.writes,now);
            } else if(s->pmic1000_initial&&regno==0x1000) {
                int result=o62m_pmic1000_read_v1(&s->pmic1000,&sample);
                if(result) {error_report("MARCONI_PMIC1000_READ_BOUNDARY reason=%s",o62m_pmic1000_reason_v1(result));return false;}
                error_report("MARCONI_PMIC1000_READ word=%04x writes=%" PRIu64 " virtual-ns=%" PRId64
                             " real-selected-slave8-data=true fullword-independent-input-or-retained-write=true "
                             "physical-initial-state-and-clock-output-effects-unmodeled=true",sample,s->pmic1000.writes,now);
            } else if(s->rtc32k_reference_profile&&(regno==0x84||regno==0x88||regno==0x8c)) {
                unsigned i=(regno-0x84)/4;
                if(!(s->rtc_calibration_config_written&(1u<<i)))return false;
                sample=s->rtc_calibration_config[i];
            } else if(s->rtc32k_reference_profile&&regno==0x1010) {
                sample=o62m_rtc32k_status_v1(&s->rtc32k_reference);
                if(s->rtc32k_reads++<8)marconi_public_report("MARCONI_RTC32K_REFERENCE_READ generation=%" PRIu64
                    " qualified=%u pure-status-projection=true no-read-trigger-or-calibration-ACK=true",
                    s->rtc32k_reference.generation,s->rtc32k_reference.ready);
            } else if(s->rtc_seconds_fixture&&s->pmic_boot_events_profile&&
                      (regno==0x1124||regno==0x1128||regno==0x112c||regno==0x1130)) {
                bool poweron=regno==0x1124||regno==0x1128;
                uint32_t seconds=poweron?s->pmic_poweron_seconds:0;
                unsigned shift=(regno==0x1128||regno==0x1130)?16:0;
                sample=(seconds>>shift)&UINT16_MAX;
                marconi_public_report("MARCONI_PMIC_RTC_HISTORY_READ kind=%s half=%u stable-initial-RTC-latch-or-explicit-no-history=true no-live-counter-change-or-IRQ=true timestamp-redacted=true",
                    poweron?"poweron":"shutdown",shift/16);
            } else if(s->rtc_seconds_fixture&&(regno==0x111c||regno==0x1120)) {
                O62MRTCResult result=o62m_rtc_read(&s->rtc_seconds,regno,&sample,now);
                if(result!=O62M_RTC_OK) {
                    error_report("MARCONI_RTC_SECONDS_BOUNDARY reg=%04x reason=%s "
                                 "virtual-ns=%" PRId64,regno,o62m_rtc_reason(result),now);
                    return false;
                }
                error_report("MARCONI_RTC_SECONDS_READ reg=%04x data=%04x "
                             "seconds=%u phase-ns=%u virtual-ns=%" PRId64 " "
                             "always-running-virtual-clock-policy=true "
                             "physical-reset-oscillator-latch-calibration-unknown=true",
                             regno,sample,s->rtc_seconds.seconds,s->rtc_seconds.subsecond_ns,now);
            } else if((s->virtual_auxadc_reset_profile&&s->virtual_auxadc_reset.asserted)||
                      !marconi_adc_core_read(s,regno,&sample,now)) {return false;}
            if(s->auxadc_channel_profile&&(regno==0x290||regno==0x28c||regno==0x2a0||regno==0x2a4)) {
                s->auxadc_result_reads++;
                marconi_public_report("MARCONI_AUXADC_RESULT_READ reads=%" PRIu64 " completed-channel=%u actual-retained-result-source=true ACK-does-not-erase-data=true raw-sample-redacted=true no-host-write-of-callback-flag=true",s->auxadc_result_reads,s->pmic_adc.result_channel);
            }
            /* valid is asserted only after the selected modeled slave
             * supplied actual ADC or advancing RTC register data. */
            *value = 0x400000 | sample;
        }
        if (s->pmic_accesses++ < 512) {
            error_report("MARCONI_EXPLORE_PMIC access=%s reg=0x%04x value=0x%08" PRIx64
                         " virtual-ns=%" PRId64 " adc-started=%" PRIu64 " adc-completed=%" PRIu64,
                         write ? "write" : "read", regno, *value, now,
                         s->pmic_adc.started, s->pmic_adc.completed);
        }
        return true;
    }
    if (s->timer64 && address >= 0x40040000 && address <= 0x40040020) {
        uint32_t bus_clock;
        o62m_boot_clock_read(&s->boot_clocks, 0x400600f8, &bus_clock);
        /* Physical bus response with pclk disabled is not established. */
        if (!(bus_clock & 0x20000)) { return false; }
        uint32_t v = write ? *value : 0;
        if (!marconi_timer64_refresh(s)) { return false; }
        bool ok = write ? o62m_timer64_write(&s->timer64_model,
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), address - 0x40040000, v) :
                 o62m_timer64_read(&s->timer64_model,
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), address - 0x40040000, &v);
        if (!ok) { return false; }
        *value = v;
        error_report("MARCONI_EXPLORE_TIMER64 access=%s offset=0x%02" HWADDR_PRIx
                     " value=0x%08x virtual-ns=%" PRId64, write ? "write" : "read",
                     address - 0x40040000, v, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        return marconi_timer64_refresh(s);
    }
    if (address == 0x4a00802c || address == 0x4a00402c ||
        (address >= 0x4a020000 && address <= 0x4a022004)) {
        uint32_t v = write ? *value : 0;
        bool ok;
        if (write && (address == 0x4a00802c || address == 0x4a00402c)) {
            ok = o62m_hwspinlock_reset_write(&s->hwlock, address, v,
                                            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        } else {
            ok = write ? o62m_hwspinlock_write(&s->hwlock, address, v) :
                         o62m_hwspinlock_read(&s->hwlock, address, &v);
        }
        if (!ok) { return false; }
        *value = v;
        error_report("MARCONI_EXPLORE_HWLOCK access=%s address=0x%08" HWADDR_PRIx
                     " value=0x%08x owned=0x%08x", write ? "write" : "read", address, v,
                     s->hwlock.owned);
        return true;
    }
    if (o62m_boot_clock_index(address) >= 0) {
        uint32_t v = write ? *value : 0;
        if (s->timer64 && write && (address == 0x4006005c || address == 0x400600f8)) {
            O62MTimer64 trial;
            uint32_t csr, pclk;
            if (!marconi_timer64_refresh(s)) { return false; }
            trial = s->timer64_model;
            o62m_boot_clock_read(&s->boot_clocks, 0x4006005c, &csr);
            o62m_boot_clock_read(&s->boot_clocks, 0x400600f8, &pclk);
            if (address == 0x4006005c) { csr = v; } else { pclk = v; }
            if (!marconi_timer64_clock(s, csr, pclk, &trial)) { return false; }
            if (s->ap_timer64_view) {
                trial = s->dvfs_timer64_model;
                if (!marconi_timer64_clock(s, csr, pclk, &trial)) return false;
            }
        }
        if(write&&!marconi_timer4014_clock_preflight(s,&s->clock_controls,address,v)) {return false;}
        bool ok = write ? o62m_boot_clock_write(&s->boot_clocks, address, v) :
                          o62m_boot_clock_read(&s->boot_clocks, address, &v);
        if (!ok) { return false; }
        *value = v;
        if(!marconi_timer4014_refresh(s)) {return false;}
        if (!marconi_timer64_refresh(s)) { return false; }
        error_report("MARCONI_EXPLORE_CLOCK access=%s address=0x%08" HWADDR_PRIx
                     " value=0x%08x", write ? "write" : "read", address, v);
        return true;
    }
    switch (address) {
    /* Original TEE73e598ec/73e5992c RMW sets bit12 before PUBAP/PUBCP
     * DFS setup. Only this retained control bit is evidenced here. Its
     * hardware name/effects and the DFS activation/handshake are unknown;
     * do not synthesize status at5a8d001c/5a9b001c. */
    case 0x4a810780: reg = &s->pubap_dfs_control; mask = 0x1000; break;
    case 0x4a910780: reg = &s->pubcp_dfs_control; mask = 0x1000; break;
    case 0x4a000054: reg = &s->ctrl54; mask = 0x10; break;
    case 0x4a140028:
    case 0x4a14002c:
        if(!s->cp_created||current_cpu!=CPU(&s->cp_cpu)||size!=4||
           !(s->ctrl54&0x10)||(s->spmi_ctrl&0x44)!=0x44)return false;
        if(address==0x4a140028) {
            if(write||marconi_mmio_pc()!=UINT64_C(0x6412c09c))return false;
            *value=s->spmi_irq_pending;++s->spmi_irq_reads;
        } else {
            if(!write||*value>UINT32_MAX||marconi_mmio_pc()!=UINT64_C(0x6412c34e))return false;
            s->spmi_irq_pending&=~(uint32_t)*value;++s->spmi_irq_clears;
        }
        marconi_public_report("MARCONI_SPMI_INTERRUPT_ACCESS write=%u actual-shared-status=true no-unknown-event-generated=true no-bus-ready-substitution=true",write);
        return true;
    case 0x4a14003c: reg = &s->spmi_ctrl; mask = s->aon120_config_inputs?0xcc:0x44; break;
    case 0x4a09000c: reg = &s->uart_clock; mask = 0x10703; break;
    case 0x4a0900e4:
        if (!s->uart_tx_fixture) { return false; }
        reg = &s->uart_pclk; mask = 0x20000; break;
    case 0x4a140040:
        if (write || !(s->ctrl54 & 0x10) || (s->spmi_ctrl & 0x44) != 0x44) {
            return false;
        }
        *value = (s->select_pending || s->pmic_write_pending) ? 0x400000 : 0;
        break;
    case 0x4a150000:
        if (!write || *value != 0x00880000 || s->select_pending || s->pmic_write_pending ||
            !(s->ctrl54 & 0x10) || (s->spmi_ctrl & 0x44) != 0x44) {
            return false;
        }
        int64_t select_now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if(select_now<0||s->transaction_ns>(uint64_t)(INT64_MAX-select_now)) {
            error_report("MARCONI_SPMI_SELECT_BOUNDARY reason=virtual-deadline-overflow queue-unchanged=true");return false;
        }
        s->slave_selected = false;
        s->select_pending = true;
        s->select_deadline = select_now+(int64_t)s->transaction_ns;
        error_report("MARCONI_EXPLORE_SPMI select-submit slave=8 deadline=%" PRId64,
                     s->select_deadline);
        break;
    default: return false;
    }
    if (reg) {
        if (write) {
            bool tx_clock = s->uart_tx_fixture &&
                (address == 0x4a09000c || address == 0x4a0900e4);
            if (tx_clock) {
                marconi_tx_refresh(s);
                if (address == 0x4a09000c && ((*reg ^ *value) & 0x703) &&
                    (s->tx.active || s->tx.count)) { return false; }
            }
            if (((*reg ^ *value) & ~mask) || s->select_pending ||
                (s->pmic_write_pending && address != 0x4a09000c)) {
                return false;
            }
            *reg = *value;
            if (tx_clock) { marconi_tx_refresh(s); }
        } else {
            *value = *reg;
        }
    }
    if (s->explore_accesses++ < 128) {
        error_report("MARCONI_EXPLORE_MMIO access=%s address=0x%08" HWADDR_PRIx
                     " value=0x%08" PRIx64 " virtual-ns=%" PRId64,
                     write ? "write" : "read", address, *value,
                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    return true;
}

static void marconi_tx_emit(void *opaque, uint8_t byte, int64_t completed)
{
    MarconiProbeState *s = opaque;
    if(!marconi_sensitive_guest_data)qemu_chr_fe_write_all(&s->uart.chr, &byte, 1);
    if (s->tx.emitted <= 256) {
        error_report("MARCONI_EXPLORE_UART_TX byte=0x%02x completed-ns=%" PRId64,
                     byte, completed);
    }
}

static void marconi_tx_refresh(MarconiProbeState *s)
{
    int64_t now, deadline;
    bool ok;
    if (!s->uart_tx_fixture) { return; }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    ok = o62m_uart_tx_advance(&s->tx, now, marconi_tx_emit, s);
    assert(ok);
    s->tx.fifo = !!(s->uart.fcr & 1);
    o62m_uart_tx_irq_config(&s->tx, s->uart.ier & 2,
                           s->uart_ier_extra & 0x80, s->uart_stet);
    ok = o62m_uart_tx_set_running(&s->tx, (s->uart_clock & 0x10000) &&
                                 (s->uart_pclk & 0x20000), now);
    assert(ok);
    deadline = o62m_uart_tx_next_deadline(&s->tx);
    if (deadline == O62M_UART_TX_NO_DEADLINE) { timer_del(s->tx_timer); }
    else { timer_mod_ns(s->tx_timer, deadline); }
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->armv7m), 57), o62m_uart_tx_irq(&s->tx));
}

static void marconi_tx_timer(void *opaque)
{
    marconi_tx_refresh(opaque);
}

static bool marconi_uart_ptime(MarconiProbeState *s)
{
    return s->uart_dw_config && (s->uart_ier_extra & 0x80) &&
           (s->uart.fcr & 1);
}

static void marconi_uart_irq(void *opaque, int n, int level)
{
    MarconiProbeState *s = opaque;

    if (s->uart_tx_fixture) {
        level = o62m_uart_tx_irq(&s->tx);
    } else if (marconi_uart_ptime(s) && (s->uart.ier & 2) &&
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
    marconi_tx_refresh(s);
    if (offset < 0x20 && offset != 0x18) {
        /* The modem input wiring is unknown, so MSR is deliberately absent. */
        *value = serial_io_ops.read(uart, offset >> 2, 1);
        if (s->uart_dw_config && offset == 4 && !(uart->lcr & 0x80)) {
            *value |= s->uart_ier_extra;
        }
        if (s->uart_tx_fixture) {
            if (offset == 0x14) { *value = (*value & ~0x60u) | o62m_uart_tx_lsr(&s->tx); }
            if (offset == 8) { *value = o62m_uart_tx_iir(&s->tx); marconi_uart_refresh_irq(s); }
        } else if (marconi_uart_ptime(s)) {
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
    } else if (s->explore_seed && offset == 0xc0) {
        *value = s->uart_dlf;
    } else if (offset == 0x7c) {
        /* Synopsys USR: BUSY, TFNF, TFE, RFNE, RFF. Data transfer is
         * stopped below until clock/FIFO configuration is implemented. */
        *value = ((uart->lsr & 0x40) ? 0 : 1) |
                 (fifo8_is_full(&uart->xmit_fifo) ? 0 : 2) |
                 (fifo8_is_empty(&uart->xmit_fifo) ? 4 : 0) |
                 ((uart->lsr & 1) ? 8 : 0) |
                 (fifo8_is_full(&uart->recv_fifo) ? 16 : 0);
        if (s->uart_tx_fixture) { *value = (*value & ~7u) | o62m_uart_tx_usr(&s->tx); }
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
    marconi_tx_refresh(s);
    if (s->uart_tx_fixture && (s->tx.active || s->tx.count) &&
        (offset == 0x98 || offset == 8) && ((value ^ uart->fcr) & 1)) { return false; }
    if (s->uart_tx_fixture && (s->tx.active || s->tx.count) &&
        (offset == 0xc0 || offset == 0x0c ||
         (offset <= 4 && (uart->lcr & 0x80)))) { return false; }
    if (s->uart_tx_fixture && offset == 0 && !(uart->lcr & 0x80)) {
        static const uint32_t sources[] = {32000, 26000000, 167000000, 169000000};
        uint32_t hz = sources[s->uart_clock & 3] / (((s->uart_clock >> 8) & 7) + 1);
        if (!o62m_uart_tx_configure(&s->tx, hz, uart->divider, s->uart_dlf & 15,
                                    uart->lcr, uart->fcr & 1) ||
            !o62m_uart_tx_write(&s->tx, value, s->tx.last_ns)) {
            return false;
        }
        marconi_uart_log(s, address, value, true);
        marconi_tx_refresh(s);
        return true;
    }
    if (s->explore_seed && offset == 0xc0) {
        if ((s->uart_dlf ^ value) & ~15u) { return false; }
        s->uart_dlf = value;
    } else if (offset == 0x88 && !(value & ~6)) {
        /* SRR.RFR/XFR reset the corresponding real FIFO and its status.
         * Keep FCR configuration; SRR=6 must not reset LCR/divisor/IER.
         * Synopsys dw_uart_hal.h defines RFR=2 and XFR=4. */
        serial_io_ops.write(uart, 2, uart->fcr | value, 1);
        if (s->uart_tx_fixture && (value & 4)) { o62m_uart_tx_fifo_reset(&s->tx); }
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
                if (s->uart_tx_fixture && (value & 4)) { o62m_uart_tx_fifo_reset(&s->tx); }
            }
            serial_io_ops.write(uart, offset >> 2, value, 1);
        }
    } else {
        return false;
    }
    marconi_uart_log(s, address, value, true);
    marconi_uart_refresh_irq(s);
    marconi_tx_refresh(s);
    return true;
}

static hwaddr marconi_peripheral_alias(hwaddr address)
{
    /* Original TEE drivers use the secure 5a aliases of these same blocks.
     * The terminal missing-access report retains the original address. */
    if (address == 0x5a00802c || address == 0x5a00402c ||
        (address >= 0x5a020000 && address <= 0x5a022004) ||
        (address >= 0x5a000000 && address < 0x5a000100) ||
        (address >= 0x5a030000 && address < 0x5a030100) ||
        (address >= 0x5a090000 && address < 0x5a090200) ||
        (address >= 0x5a140000 && address < 0x5a160000)) {
        return address - 0x10000000;
    }
    return address;
}

static bool marconi_conditional_lowpage(MarconiProbeState *s, hwaddr address,
                                        uint64_t *value, unsigned size, bool write)
{
    if (!s->lowpage_response_experiment || !size || size > 8 ||
        address > 0x1000 - size) { return false; }
    bool retain = !strcmp(s->lowpage_response_experiment, "retain-written");
    if (!write) {
        for (unsigned i = 0; i < size; ++i) {
            if (!retain || !s->lowpage_initialized[address + i]) {
                error_report("MARCONI_CONDITIONAL_LOWPAGE_REJECT address=0x%08"
                             HWADDR_PRIx " size=%u reason=unknown-initial-bytes "
                             "mode=%s", address, size, s->lowpage_response_experiment);
                return false;
            }
        }
        *value = 0;
        for (unsigned i = 0; i < size; ++i) {
            *value |= (uint64_t)s->lowpage_written_bytes[address + i] << (8 * i);
        }
        ++s->lowpage_reads;
    } else {
        if (retain) {
            for (unsigned i = 0; i < size; ++i) {
                s->lowpage_written_bytes[address + i] = *value >> (8 * i);
                s->lowpage_initialized[address + i] = true;
            }
        }
        ++s->lowpage_writes;
    }
    error_report("MARCONI_CONDITIONAL_LOWPAGE access=%s address=0x%08" HWADDR_PRIx
                 " size=%u value=0x%016" PRIx64 " mode=%s writes=%" PRIu64
                 " reads=%" PRIu64 " pc=0x%08" PRIx64 " hardware-backing=unknown",
                 write ? "write" : "read", address, size, *value,
                 s->lowpage_response_experiment, s->lowpage_writes, s->lowpage_reads,
                 current_cpu ? CPU_GET_CLASS(current_cpu)->get_pc(current_cpu) : UINT64_MAX);
    return true;
}

static void marconi_conditional_lowpage_reset(void *opaque)
{
    MarconiProbeState *s = opaque;
    memset(s->lowpage_written_bytes, 0, sizeof(s->lowpage_written_bytes));
    memset(s->lowpage_initialized, 0, sizeof(s->lowpage_initialized));
    s->lowpage_writes = s->lowpage_reads = 0;
    error_report("MARCONI_CONDITIONAL_LOWPAGE_RESET initialized-bytes=0 "
                 "mode=%s reset-scope=experimental-lowpage-only",
                 s->lowpage_response_experiment);
}

static void marconi_cp_image_ram_exit(Notifier *notifier,void *data)
{
    MarconiProbeState *s=container_of(notifier,MarconiProbeState,cp_image_ram_exit);
    (void)data;
    if(!s->cp_image_ram_created)return;
    MemoryRegion *regions[]={&s->cp_image_ram,&s->cp_boot_ram,&s->cp_crf_ram};
    bool wiped=true;
    for(unsigned i=0;i<ARRAY_SIZE(regions);i++) {
        void *bytes=memory_region_get_ram_ptr(regions[i]);
        size_t length=memory_region_size(regions[i]);
        o62m_virtual_otp_wipe_v1(bytes,length);
        wiped=wiped&&o62m_virtual_otp_zero_v1(bytes,length);
    }
    marconi_public_report("MARCONI_CP_IMAGE_RAM_EXIT regions=3 wiped=%u no-payload-output=true",wiped);
}

#include "o62m-normal-emmc-bsp-v25.inc"

static MemTxResult marconi_missing_read(void *opaque, hwaddr address,
                                       uint64_t *value, unsigned size,
                                       MemTxAttrs attrs)
{
    MarconiProbeState *s = opaque;
    hwaddr canonical = marconi_peripheral_alias(address);

    if(s->normal_emmc_bound&&s->reported)return MEMTX_ERROR;
    if (marconi_conditional_lowpage(s, address, value, size, false)) {
        return MEMTX_OK;
    }
    if (marconi_explore_access(s, canonical, value, size, false)) {
        return MEMTX_OK;
    }
    if(s->normal_emmc_bound&&s->reported)return MEMTX_ERROR;
    if (marconi_uart_read(s, canonical, value, size)) {
        return MEMTX_OK;
    }
    if (s->counter32k && canonical == 0x4a000090 && size == 4) {
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
    hwaddr canonical = marconi_peripheral_alias(address);

    if(s->normal_emmc_bound&&s->reported)return MEMTX_ERROR;
    if (marconi_conditional_lowpage(s, address, &value, size, true)) {
        return MEMTX_OK;
    }
    if (marconi_explore_access(s, canonical, &value, size, true)) {
        if(!marconi_normal_emmc_clock_notify(s,canonical))return MEMTX_ERROR;
        return MEMTX_OK;
    }
    if(s->normal_emmc_bound&&s->reported)return MEMTX_ERROR;
    if (marconi_uart_write(s, canonical, value, size)) {
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

/* Architectural PMU register state, unbanked between Secure/Non-secure.
 * This opt-in intentionally stops at the first live-counting transaction.
 * No counter clock/event, DWT alias, DEMCR or IRQ is invented. */
static MemTxResult marconi_pmu_access(void *opaque, hwaddr offset,
                                     uint64_t *value, unsigned size,
                                     MemTxAttrs attrs, bool write)
{
    MarconiProbeState *s=opaque;
    O62MPMUResult result=o62m_pmu_access(&s->pmu_init,offset,size,write,
                                        attrs.secure,!attrs.user,value);
    uint32_t pc=s->armv7m.cpu->env.regs[15];
    if(result!=O62M_PMU_OK) {
        error_report("MARCONI_PMU_INIT_BOUNDARY access=%s offset=%03" HWADDR_PRIx
                     " pc=%08x reason=%s state-not-committed=true",
                     write?"write":"read",offset,pc,o62m_pmu_reason(result));
        return marconi_missing(s,0xe0003000+offset,size,write,*value);
    }
    error_report("MARCONI_PMU_INIT_ACCESS access=%s offset=%03" HWADDR_PRIx
                 " value=%08" PRIx64 " pc=%08x secure=%u privileged=%u "
                 "control=%08x enabled=%08x overflow=%08x "
                 "live-counting-DWT-DEMCR-IRQ-unimplemented=true",
                 write?"write":"read",offset,*value,pc,attrs.secure,!attrs.user,
                 s->pmu_init.control,s->pmu_init.enabled,s->pmu_init.overflow);
    return MEMTX_OK;
}
static MemTxResult marconi_pmu_read(void *opaque,hwaddr offset,uint64_t *value,
                                   unsigned size,MemTxAttrs attrs)
{
    *value=0;
    return marconi_pmu_access(opaque,offset,value,size,attrs,false);
}
static MemTxResult marconi_pmu_write(void *opaque,hwaddr offset,uint64_t value,
                                    unsigned size,MemTxAttrs attrs)
{ return marconi_pmu_access(opaque,offset,&value,size,attrs,true); }
static const MemoryRegionOps marconi_pmu_ops={
    .read_with_attrs=marconi_pmu_read,.write_with_attrs=marconi_pmu_write,
    .endianness=DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size=1,.valid.max_access_size=8,
    .impl.min_access_size=1,.impl.max_access_size=8,
};

/* M55 MEMSYSCTL is E001E000. The opt-in records only the original
 * Secure-privileged DTGU_LUT0 configuration write; this is not complete
 * TGU permission enforcement and does not create/alias TCM memory.
 * Other offsets/states retain strict faults instead of generic PPB RAZ/WI. */
static MemTxResult marconi_memsysctl_read(void *opaque, hwaddr offset,
                                        uint64_t *value, unsigned size,
                                        MemTxAttrs attrs)
{
    MarconiProbeState *s = opaque;
    if (s->dtgu_lut0_observed_write && attrs.secure && !attrs.user &&
        offset == 0x610 && size == 4 && s->dtgu_lut0_written) {
        *value = s->dtgu_lut0;
        error_report("MARCONI_DTGU_LUT0_READ value=%08x actual-write-readback-only=true "
                     "TCM-mapping-or-permission-enforcement-implemented=false", s->dtgu_lut0);
        return MEMTX_OK;
    }
    return marconi_missing(opaque, 0xe001e000 + offset, size, false, 0);
}
static MemTxResult marconi_memsysctl_write(void *opaque, hwaddr offset,
                                         uint64_t value, unsigned size,
                                         MemTxAttrs attrs)
{
    MarconiProbeState *s = opaque;
    if (s->dtgu_lut0_observed_write && attrs.secure && !attrs.user &&
        offset == 0x610 && size == 4 && value == 0x001fffff) {
        s->dtgu_lut0 = value;
        s->dtgu_lut0_written = true;
        error_report("MARCONI_DTGU_LUT0_WRITE value=%08x secure=1 privileged=1 "
                     "documented-register-observed-config-only=true "
                     "TCM-mapping-or-permission-enforcement-implemented=false", s->dtgu_lut0);
        return MEMTX_OK;
    }
    return marconi_missing(opaque, 0xe001e000 + offset, size, true, value);
}
static const MemoryRegionOps marconi_memsysctl_ops = {
    .read_with_attrs = marconi_memsysctl_read,
    .write_with_attrs = marconi_memsysctl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static void marconi_research_deadline(void *opaque)
{
    MarconiProbeState *s = opaque;
    if (!s->reported) {
        s->reported = true;
        marconi_public_report("MARCONI_RESEARCH_DEADLINE wall-seconds=600 dfs-observed-phase=%u "
                     "not-firmware-timeout-or-boot-success=true", s->dfs_config.phase);
        if(s->i2c_nopeer_profile) {
            uint64_t pc=marconi_public_code_pc(CPU_GET_CLASS(CPU(s->armv7m.cpu))->get_pc(CPU(s->armv7m.cpu)));
            marconi_public_report("MARCONI_RESEARCH_DEADLINE_PUBLIC_PC pc=%08" PRIx64
                " PMIC-config-reads0=%" PRIu64 " reads1=%" PRIu64 " reads2=%" PRIu64
                " writes0=%" PRIu64 " writes1=%" PRIu64 " writes2=%" PRIu64
                " counts-only-not-register-values=true no-stack-or-CPUdump=true",
                pc,s->pmic_button_reads,s->pmic_button_followup_reads,s->pmic_button_second_followup_reads,
                s->pmic_button_writes,s->pmic_button_followup_writes,s->pmic_button_second_followup_writes);
        }
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_HOST_ERROR, 87);
    }
}

/* Original SBL60062e4c/4e supplies R0=700c2800 on BLX toTEE.
 * This is an initial boot ABI argument only. The original TEE11206 stores it;
 * no firmware/global/PC/NS-state or later handoff register is modified. */
static void marconi_tee_sbl_argument_reset(void *opaque)
{
    MarconiProbeState *s=opaque;
    CPUARMState *env=&s->armv7m.cpu->env;
    if(env->regs[15]!=0x73e11200||!env->v7m.secure) {
        error_report("tee-sbl-argument-fixture reset ordering requires untouched original Secure TEE reset PC");
        exit(EXIT_FAILURE);
    }
    env->regs[0]=0x700c2800;
    error_report("MARCONI_TEE_SBL_INITIAL_ARGUMENT r0=700c2800 mode=%u "
        "original-SBL-ABI=true only-initial-r0=true pc=%08x secure=%u "
        "TEE-reset-store-and-all-handoff-instructions-unmodified=true",
        s->tee_sbl_boot_mode,env->regs[15],env->v7m.secure);
}

/* Separate original SBL boot('/ota/vela_ota.bin') ABI. Original SBL supplies
 * R0=600c2800 and BASEPRI=80 before BLX. Initial registers only: no original
 * OTA PC/SP/VTOR/security/branch is patched or skipped. */
static void marconi_ota_sbl_argument_reset(void *opaque)
{
    MarconiProbeState *s=opaque;
    CPUARMState *env=&s->armv7m.cpu->env;
    if(env->regs[15]!=0x60540820||!env->v7m.secure) {
        error_report("ota-sbl-argument-fixture requires original standaloneSecureOTAreset ordering");exit(EXIT_FAILURE);
    }
    env->regs[0]=0x600c2800;
    env->v7m.basepri[env->v7m.secure]=0x80;
    error_report("MARCONI_OTA_SBL_INITIAL_ARGUMENT r0=600c2800 mode=%u basepri=80 "
                 "only-original-SBL-initial-R0-and-BASEPRI=true original-OTA-reset-entry-untouched=true "
                 "standalone-Secure-reset-not-SBL-chain=true",s->ota_sbl_boot_mode);
}

static void marconi_probe_init(MachineState *machine)
{
    MarconiProbeState *s = (MarconiProbeState *)machine;
    DeviceState *cpu;
    g_autofree gchar *ap = NULL;
    g_autofree gchar *handoff_ap = NULL;
    gsize bytes = 0, handoff_ap_bytes = 0;
    hwaddr load_base = s->tee_reset ? MARCONI_TEE_BASE :
                       s->sbl_reset ? MARCONI_SBL_BASE : s->ota_reset ? MARCONI_OTA_BASE : MARCONI_AP_BASE;
    hwaddr ram_bytes = s->tee_reset ? MARCONI_TEE_BYTES :
                       s->sbl_reset ? MARCONI_SBL_BYTES : s->ota_reset ? MARCONI_OTA_BYTES : MARCONI_AP_BYTES;
    uint32_t vtor = s->tee_reset ? MARCONI_TEE_ALIAS :
                    s->sbl_reset ? MARCONI_SBL_VTOR : s->ota_reset ? MARCONI_OTA_BASE : MARCONI_AP_BASE;
    const char *stage = s->tee_reset ? "TEE" : s->sbl_reset ? "SBL" : s->ota_reset ? "OTA" : "AP";
    bool layout_matches;

    if ((unsigned)s->tee_reset+(unsigned)s->sbl_reset+(unsigned)s->ota_reset>1) {
        error_report("tee-reset, sbl-reset and ota-reset are mutually exclusive");
        exit(EXIT_FAILURE);
    }

    if(s->normal_emmc_profile||s->normal_emmc_media) {
        if(!s->normal_emmc_profile||!s->normal_emmc_media||strcmp(s->normal_emmc_profile,"declared-v7-boot2s-4g-v1")||
           !s->tee_reset||!s->tee_ap_image||!s->explore_seed||!s->clock_controls_fixture||!s->pll_rate_fixture) {
            error_report("normal-emmc requires declared-v7-boot2s-4g-v1 plus exactmedia originalTEE/AP exploreseed existing335clock andPLL12 owners");exit(EXIT_FAILURE);
        }
    }
    if(s->normal_emmc_tuning_profile&&(!s->normal_emmc_profile||
       strcmp(s->normal_emmc_tuning_profile,"standard-cmd21-v1"))) {
        error_report("normal-emmc-tuning-profile requires explicitnormalcard andstandard-cmd21-v1; actualcardstate-width-HS_TIMING stillrequired");exit(EXIT_FAILURE);
    }
    if(s->normal_emmc_fixed_supply&&(!s->normal_emmc_profile||
       (strcmp(s->normal_emmc_fixed_supply,"external-3v3-v1")&&
        strcmp(s->normal_emmc_fixed_supply,"external-1v8-v1")))) {
        error_report("normal-emmc-fixed-supply requires explicitnormalcard andexternal-3v3-v1 orexternal-1v8-v1 virtualboardinput; physicalPMICmappingunknown");exit(EXIT_FAILURE);
    }
    if(s->normal_emmc_fault_test&&(!s->normal_emmc_profile||
       strcmp(s->normal_emmc_fault_test,"owned-worker-v1"))) {
        error_report("normal-emmc-fault-test requires owned-worker-v1 andexplicitnormalvolatilecardprofile; negative-lifecycle-test-only");exit(EXIT_FAILURE);
    }
    if (s->tee_ap_image && !s->tee_reset) {
        error_report("tee-ap-image requires tee-reset=on; it never selects AP reset");
        exit(EXIT_FAILURE);
    }

    if(s->ota_sbl_argument_fixture) {
        char *end;
        if(!s->ota_reset||!g_ascii_isdigit(s->ota_sbl_argument_fixture[0])) {
            error_report("ota-sbl-argument-fixture requires originalOTA andunsignedsoftwaremode");exit(EXIT_FAILURE);
        }
        uint64_t word=g_ascii_strtoull(s->ota_sbl_argument_fixture,&end,0);
        if(*end||word>4) {error_report("ota-sbl-argument-fixture originalsoftwaremodeindex0..4 required");exit(EXIT_FAILURE);}
        s->ota_sbl_boot_mode=word;
    }
    if(s->ota_config_body_inputs) {
        if(!s->ota_reset||!s->explore_seed||!s->ota_retained_footer_image||
           !s->ota_retained_hwlock_image||!s->ota_config_write_observations) {
            error_report("ota-config-body-inputs requires originalOTA/explore/coherentactualretainedstate/envelopeobservations");exit(EXIT_FAILURE);
        }
        g_autofree gchar *bytes=NULL;
        gsize length=0;
        uint32_t initial[O62M_OTA_CONFIG_BODY_WORDS_V1];
        if(!g_file_get_contents(s->ota_config_body_inputs,&bytes,&length,NULL)||length!=sizeof(initial)) {
            error_report("ota-config-body-inputs requires exact600BLE150independentfullu32words, notsiliconreset");exit(EXIT_FAILURE);
        }
        for(unsigned i=0;i<O62M_OTA_CONFIG_BODY_WORDS_V1;i++) {initial[i]=ldl_le_p(bytes+4*i);}
        O62MOtaConfigBodyResultV1 result=o62m_ota_config_body_init_v1(&s->ota_config_body,initial,O62M_OTA_CONFIG_BODY_WORDS_V1,0x81);
        if(result!=O62M_OTA_CONFIG_BODY_OK) {error_report("ota-config-body-inputs invalid: %s",o62m_ota_config_body_reason_v1(result));exit(EXIT_FAILURE);}
        g_autofree gchar *digest=g_compute_checksum_for_data(G_CHECKSUM_SHA256,(const guchar *)bytes,length);
        error_report("MARCONI_OTA_CONFIG_BODY_INPUTS bytes=600 words=150 board=81 sha256=%s "
                     "independent-opaque-fullword-inputs=true only-proven-original-two-bank-stream=true "
                     "no-silicon-reset-or-physical-pad-sideeffect-claim=true",digest);
    }
    if(s->auxadc_channel_profile&&(!s->tee_reset||!s->tee_ap_image||!s->explore_seed||!s->adc_fixture||!s->virtual_pmic_bc_config||!s->pmic_regulator_inputs||!s->pmic_clock_inputs)) {
        error_report("auxadc-channel-profile requires originalTEE/AP/explore/ADC/canonicalBC/PMIC/clock owners before initialization");exit(EXIT_FAILURE);
    }
    if(s->pmic_regulator_inputs&&((!s->ota_reset&&!s->tee_reset)||!s->explore_seed||!s->adc_fixture)) {
        error_report("pmic-regulator-inputs requires originalOTAorTEE/explore/selectedSPMIslavemodel");exit(EXIT_FAILURE);
    }
    if(s->pmic_clock_inputs&&((!s->ota_reset&&!s->tee_reset)||!s->explore_seed||!s->adc_fixture||s->pmic1000_initial)) {
        error_report("pmic-clock-inputs requires originalOTAorTEE/explore/SPMIslavemodel andexcludespmic1000-initial");exit(EXIT_FAILURE);
    }
    if(s->pmic1000_initial) {
        char *end;
        if((!s->ota_reset&&!s->tee_reset)||!s->explore_seed||!s->adc_fixture||!g_ascii_isdigit(s->pmic1000_initial[0])) {
            error_report("pmic1000-initial requires originalOTAorTEE/explore/explicitSPMIselectedslavemodel andunsignedfull16bitword");exit(EXIT_FAILURE);
        }
        uint64_t word=g_ascii_strtoull(s->pmic1000_initial,&end,0);
        if(*end||word>UINT16_MAX) {error_report("pmic1000-initial requires independentfullu16input, notsiliconreset");exit(EXIT_FAILURE);}
        int result=o62m_pmic1000_init_v1(&s->pmic1000,word);
        if(result) {error_report("pmic1000-initial invalid: %s",o62m_pmic1000_reason_v1(result));exit(EXIT_FAILURE);}
        error_report("MARCONI_PMIC1000_INITIAL word=%04x explicit-independent-full16word=true "
                     "only-proven-muxfield-mask=0002 other-bit-changes-strict=true "
                     "no-status-clock-ready-or-silicon-reset-claim=true",s->pmic1000.word);
    }
    if(s->timer4014_load_plus_one&&!s->timer4014_inputs) {
        error_report("timer4014-load-plus-one requires explicit timer4014-inputs");exit(EXIT_FAILURE);
    }
    if(s->timer4014_inputs&&(!s->ota_reset||!s->explore_seed||!s->ota_config_body_inputs)) {
        error_report("timer4014-inputs requires originalOTA/explore/explicitconfigurationinputs andcoherentretainedstate");exit(EXIT_FAILURE);
    }
    if(s->ap_gpio_config&&!s->ota_config_write_observations) {
        o62m_config_envelope_init_v1(&s->ota_config_envelope,true);
    }
    if(s->ap_gpio_config)o62m_uart1_boot_init_v1(&s->uart1_boot);
    if(s->ap_gpio_config&&!s->ota_config_body_inputs) {
        memset(&s->ota_config_body,0,sizeof(s->ota_config_body));
        s->ap_pinmux_reads=s->ap_pinmux_writes=0;
    }
    if(s->ota_config_write_observations) {
        if(!s->ota_reset||!s->explore_seed||!s->ota_retained_footer_image||!s->ota_retained_hwlock_image) {
            error_report("ota-config-write-observations requires originalOTA/explore/coherentactualretainedfooterandhwlock");exit(EXIT_FAILURE);
        }
        o62m_config_envelope_init_v1(&s->ota_config_envelope,true);
        error_report("MARCONI_OTA_CONFIG_ENVELOPE_POLICY exact-original-ten-writes=true max-four-cycles=true "
                     "actual-slot24-ownership-required=true physical-effects-and-readback-unimplemented=true");
    }
    if(s->ota_retained_hwlock_image&&(!s->ota_reset||!s->explore_seed||!s->ota_retained_footer_image)) {
        error_report("ota-retained-hwlock-image requires originalOTA/explore andsamepredecessorretainedfooter");exit(EXIT_FAILURE);
    }
    if(s->ota_retained_footer_image&&(!s->ota_reset||!s->explore_seed||s->ota_invalid_clock_marker_fixture)) {
        error_report("ota-retained-footer-image requires originalOTA/explore andexcludesunbackedmarkermodel");exit(EXIT_FAILURE);
    }
    if(s->ota_invalid_clock_marker_fixture) {
        if(!s->ota_reset||!s->explore_seed||!g_ascii_isdigit(s->ota_invalid_clock_marker_fixture[0])) {
            error_report("ota-invalid-clock-marker requires originalOTA/explicitexplore/unsignedu32input");exit(EXIT_FAILURE);
        }
        char *end;
        uint64_t word=g_ascii_strtoull(s->ota_invalid_clock_marker_fixture,&end,0);
        if(*end||word>UINT32_MAX||word==0x5a5a55aa) {
            error_report("ota-invalid-clock-marker requires fullu32 unequalto validmagic5a5a55aa; cachepayload/producerunimplemented");exit(EXIT_FAILURE);
        }
        s->ota_invalid_clock_marker=word;
        error_report("MARCONI_OTA_CLOCK_MARKER_FIXTURE word=%08x explicit-invalid-cache=true "
                     "SBL-only-clears-prefix-not-this-footer=true reset-value-unknown=true",
                     s->ota_invalid_clock_marker);
    }
    if(s->ap_spi_config) {
        s->ap_spi_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_ap_spi_callback,s);
        qemu_register_reset(marconi_ap_spi_reset,s);
        s->ap_spi_exit.notify=marconi_ap_spi_exit;qemu_add_exit_notifier(&s->ap_spi_exit);
    }
    if(s->ap_gpio_config) {
        s->dpu_frame_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_dpu_frame_callback,s);
        qemu_register_reset(marconi_dpu_frame_reset,s);
        s->dpu_frame_exit.notify=marconi_dpu_frame_exit;qemu_add_exit_notifier(&s->dpu_frame_exit);
        s->display_te_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_display_te_callback,s);
        qemu_register_reset(marconi_display_te_reset,s);
        s->display_te_exit.notify=marconi_display_te_exit;qemu_add_exit_notifier(&s->display_te_exit);
        s->uart1_tx_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_uart1_tx_callback,s);
        qemu_register_reset(marconi_uart1_empty_reset,s);
        s->uart1_empty_exit.notify=marconi_uart1_empty_exit;qemu_add_exit_notifier(&s->uart1_empty_exit);
    }
    s->pm_domain_request_word=0;
    o62m_xipc_config_reset_v1(&s->xipc_config);s->xipc_cleanup_selector=0;
    s->xipc_config_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_xipc_config_callback,s);
    s->xipc_cleanup_selector_written=false;s->xipc_cleanup_submissions=0;
    qemu_register_reset(marconi_xipc_cleanup_reset,s);
    s->xipc_cleanup_exit.notify=marconi_xipc_cleanup_exit;qemu_add_exit_notifier(&s->xipc_cleanup_exit);
    if(s->ap_runtime_config&&s->mipi_clock_inputs&&s->pm_bit8_profile) {
        s->mipi_phy_reference_available=true;s->mipi_phy_test_available=true;
        memset(&s->display_short,0,sizeof(s->display_short));
        s->display_short_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_display_short_callback,s);
        s->mipi_phy_pll_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_mipi_phy_pll_callback,s);
        s->mipi_phy_ulps_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_mipi_phy_ulps_callback,s);
        qemu_register_reset(marconi_mipi_phy_reset,s);
        s->mipi_phy_exit.notify=marconi_mipi_phy_exit;qemu_add_exit_notifier(&s->mipi_phy_exit);
    }
    if(s->ap_runtime_config) {
        s->sensys_config_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_sensys_config_callback,s);
        qemu_register_reset(marconi_sensys_config_reset,s);
        s->sensys_config_exit.notify=marconi_sensys_config_exit;qemu_add_exit_notifier(&s->sensys_config_exit);
    }
    dfs_config_reset(&s->dfs_config);
    (void)o62m_clock_controls_init(&s->range_configuration, false, NULL, 0, NULL, 0);
    if (s->range_configuration_fixture) {
        if (!s->tee_reset || !s->explore_seed) {
            error_report("range-configuration-fixture requires original TEE reset and explicit explore-seed");
            exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words = g_strsplit(s->range_configuration_fixture, ":", -1);
        uint32_t seeds[O62M_RANGE_CONFIGURATION_COUNT];
        if (g_strv_length(words) != O62M_RANGE_CONFIGURATION_COUNT) {
            error_report("range-configuration-fixture requires exactly %u independent full u32 words",
                         O62M_RANGE_CONFIGURATION_COUNT);
            exit(EXIT_FAILURE);
        }
        for (unsigned i = 0; i < O62M_RANGE_CONFIGURATION_COUNT; ++i) {
            char *end;
            if (!g_ascii_isdigit(words[i][0])) {
                error_report("range-configuration-fixture words require explicit unsigned values");
                exit(EXIT_FAILURE);
            }
            uint64_t word = g_ascii_strtoull(words[i], &end, 0);
            if (*end || word > UINT32_MAX) {
                error_report("range-configuration-fixture words must fit full u32");
                exit(EXIT_FAILURE);
            }
            seeds[i] = word;
        }
        O62MClockControlsResult result = o62m_clock_controls_init(
            &s->range_configuration, true, o62m_range_configuration_defs,
            O62M_RANGE_CONFIGURATION_COUNT, seeds, O62M_RANGE_CONFIGURATION_COUNT);
        assert(result == O62M_CLOCK_CONTROLS_OK);
        error_report("MARCONI_RANGE_CONFIGURATION_FIXTURE count=%u "
            "368-config-words-and490-opaque-capture-inputs=true "
            "opaque-inputs-read-only-at-original62cac=true "
            "no-ready-faultstatus-IRQ-or-reset-synthesis=true", O62M_RANGE_CONFIGURATION_COUNT);
    }
    (void)o62m_clock_controls_init(&s->resource_attributes, false, NULL, 0, NULL, 0);
    if (s->resource_attribute_fixture) {
        if (!s->tee_reset || !s->explore_seed) {
            error_report("resource-attribute-fixture requires original TEE reset and explicit explore-seed");
            exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words = g_strsplit(s->resource_attribute_fixture, ":", -1);
        uint32_t seeds[O62M_RESOURCE_ATTRIBUTE_COUNT];
        if (g_strv_length(words) != O62M_RESOURCE_ATTRIBUTE_COUNT) {
            error_report("resource-attribute-fixture requires exactly two independent full u32 words");
            exit(EXIT_FAILURE);
        }
        for (unsigned i = 0; i < O62M_RESOURCE_ATTRIBUTE_COUNT; ++i) {
            char *end;
            if (!g_ascii_isdigit(words[i][0])) {
                error_report("resource-attribute-fixture words require explicit unsigned values");
                exit(EXIT_FAILURE);
            }
            uint64_t word = g_ascii_strtoull(words[i], &end, 0);
            if (*end || word > UINT32_MAX) {
                error_report("resource-attribute-fixture words must fit full u32");
                exit(EXIT_FAILURE);
            }
            seeds[i] = word;
        }
        O62MClockControlsResult result = o62m_clock_controls_init(
            &s->resource_attributes, true, o62m_resource_attribute_defs,
            O62M_RESOURCE_ATTRIBUTE_COUNT, seeds, O62M_RESOURCE_ATTRIBUTE_COUNT);
        assert(result == O62M_CLOCK_CONTROLS_OK);
        error_report("MARCONI_RESOURCE_ATTRIBUTE_FIXTURE words=%s explicit-independent-words=true "
            "no-security-polarity-permission-or-reset-proof=true", s->resource_attribute_fixture);
    }
    if(s->tee_sbl_argument_fixture) {
        if(!s->tee_reset||!s->tee_ap_image||!s->explore_seed) {
            error_report("tee-sbl-argument-fixture requires original TEE andAP plus explicit explore-seed");exit(EXIT_FAILURE);
        }
        char *end;
        if(!g_ascii_isdigit(s->tee_sbl_argument_fixture[0])) {
            error_report("tee-sbl-argument-fixture requires explicit original SBL boot-mode index0..4");exit(EXIT_FAILURE);
        }
        uint64_t mode=g_ascii_strtoull(s->tee_sbl_argument_fixture,&end,0);
        if(*end||mode>4) {error_report("tee-sbl-argument-fixture mode must be originalSBL index0..4");exit(EXIT_FAILURE);}
        s->tee_sbl_boot_mode=(uint32_t)mode;
    }
    if(s->rc_measurement_fixture) {
        if(!s->tee_reset||!s->explore_seed) {error_report("rc-measurement-fixture requires original TEE reset and explicit explore-seed");exit(EXIT_FAILURE);}
        g_auto(GStrv) words=g_strsplit(s->rc_measurement_fixture,":",-1);
        if(g_strv_length(words)!=2) {error_report("rc-measurement-fixture requires2 explicit fullu32 sample words");exit(EXIT_FAILURE);}
        for(unsigned i=0;i<2;i++) {
            char *end;
            if(!g_ascii_isdigit(words[i][0])) {error_report("rc-measurement-fixture requires unsigned words");exit(EXIT_FAILURE);}
            uint64_t word=g_ascii_strtoull(words[i],&end,0);
            if(*end||word>UINT32_MAX) {error_report("rc-measurement-fixture word exceeds fullu32");exit(EXIT_FAILURE);}
            s->rc_measurement[i]=(uint32_t)word;
        }
        error_report("MARCONI_RC_MEASUREMENT_FIXTURE words=%s explicit-external-fullwords=true no-generation-ready-or-reset-model=true",s->rc_measurement_fixture);
    }
    if(s->mbox_outbound_experiment&&(!s->tee_reset||!s->explore_seed)) {
        error_report("mbox-outbound-experiment requires original TEE reset and explicit explore-seed");exit(EXIT_FAILURE);
    }
    if(s->wdt_policy_fixture) {
        if(!s->tee_reset||!s->explore_seed) {
            error_report("wdt-policy-fixture requires original TEE reset and explicit explore-seed");exit(EXIT_FAILURE);
        }
        char *end;
        if(!g_ascii_isdigit(s->wdt_policy_fixture[0])) {
            error_report("wdt-policy-fixture requires one explicit unsigned fullu32");exit(EXIT_FAILURE);
        }
        uint64_t word=g_ascii_strtoull(s->wdt_policy_fixture,&end,0);
        if(*end||word>UINT32_MAX) {
            error_report("wdt-policy-fixture word must fit fullu32");exit(EXIT_FAILURE);
        }
        s->wdt_policy_word=(uint32_t)word;
        error_report("MARCONI_WDT_POLICY_FIXTURE word=%08x fullword-input=true selector-only-no-silicon-reset-proof=true",s->wdt_policy_word);
    }
    if(s->trng_virtual_profile) {
        if(!s->tee_reset||!s->explore_seed||!s->te_context_fixture||s->te_platform_fatal_fixture||s->trng_unkeyed_fixture) {
            error_report("virtual-ao-csprng requires originalTEE/explore/context andexcludesinitialfatal/legacyunkeyedprovider");exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words=g_strsplit(s->trng_virtual_profile,":",-1);
        uint64_t values[3];
        if(g_strv_length(words)!=3) {error_report("virtual-ao-csprng requires explicitkeyBytes:workNs:statusOpaque");exit(EXIT_FAILURE);}
        for(unsigned i=0;i<3;i++) {
            char *end;
            if(!g_ascii_isdigit(words[i][0])) {error_report("virtual-ao-csprng unsignedinputs required");exit(EXIT_FAILURE);}
            values[i]=g_ascii_strtoull(words[i],&end,0);
            if(*end) {error_report("virtual-ao-csprng invalidnumericinput");exit(EXIT_FAILURE);}
        }
        if(values[0]>O62M_VAO_MAX_BYTES_V1) {error_report("virtual-ao-csprng keyBytes exceedsresearchresourcecap256");exit(EXIT_FAILURE);}
        O62MVirtualAoSpecV1 spec={.explicit_enable=true,.status_opaque=values[2],.key_bytes=(uint32_t)values[0],
            .latency_ns=values[1],.instance_id=1,.now_ns=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)};
        int result=o62m_virtual_ao_init_v1(&s->trng_virtual,&spec);
        if(result) {error_report("virtual-ao-csprng invalid: %s",o62m_virtual_ao_reason_v1(result));exit(EXIT_FAILURE);}
        s->trng_virtual_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_virtual_ao_callback,s);
        s->trng_virtual_exit.notify=marconi_virtual_ao_exit;
        qemu_add_exit_notifier(&s->trng_virtual_exit);
        s->trng_virtual_exit_registered=true;
        error_report("MARCONI_VIRTUAL_AO_PROFILE key-bytes=%u work-ns=%" PRIu64 " status-opaque=%08" PRIx64
                     " initial-private-store-empty=true valid=false source=actual-host-OS-CSPRNG "
                     "conditioned-endpoint-research-policy=true physical-conditioning-keylength-timing-not-claimed=true",
                     spec.key_bytes,spec.latency_ns,spec.status_opaque);
    }
    if(s->trng_unkeyed_fixture) {
        if(!s->tee_reset||!s->explore_seed||!s->te_context_fixture||s->te_platform_fatal_fixture) {
            error_report("trng-unkeyed-config-fixture requires originalTEE/explore/context-evaluation andnoinitialfatalfixture");exit(EXIT_FAILURE);
        }
        char *end;
        if(!g_ascii_isdigit(s->trng_unkeyed_fixture[0])) {error_report("trng-unkeyed-config-fixture unsignedu32statusopaque required");exit(EXIT_FAILURE);}
        uint64_t opaque=g_ascii_strtoull(s->trng_unkeyed_fixture,&end,0);
        if(*end||opaque>UINT32_MAX) {error_report("trng-unkeyed-config-fixture fullu32opaque required");exit(EXIT_FAILURE);}
        O62MTrngUnkeyedResult result=o62m_trng_unkeyed_init(&s->trng_unkeyed,true,opaque);
        if(result!=O62M_TRNG_UNKEYED_OK) {error_report("trng-unkeyed-config-fixture invalid: %s",o62m_trng_unkeyed_reason(result));exit(EXIT_FAILURE);}
        error_report("MARCONI_TRNG_UNKEYED_FIXTURE status-opaque=%08" PRIx64 " "
                     "explicit-empty-AO-key-policy=true key-valid=false "
                     "hardware-initial-status-unknown=true key-generation-entropy-health-AO-write-unimplemented=true",opaque);
    }
    if(s->te_context_fixture) {
        if(!s->tee_reset||!s->explore_seed||!s->board_controls_fixture||s->te_platform_fatal_fixture) {
            error_report("te-context-fixture requires originalTEE/explore/boardclock andexcludesinitial-fatalfixture");exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words=g_strsplit(s->te_context_fixture,":",-1);
        uint64_t inputs[O62M_TE_CONTEXT_INITIAL_WORDS];
        if(g_strv_length(words)!=O62M_TE_CONTEXT_INITIAL_WORDS) {
            error_report("te-context-fixture requires24explicitgeometry/status/clock/hostwords");exit(EXIT_FAILURE);
        }
        for(unsigned i=0;i<O62M_TE_CONTEXT_INITIAL_WORDS;i++) {
            char *end;
            if(!g_ascii_isdigit(words[i][0])) {error_report("te-context-fixture unsignedinputs required");exit(EXIT_FAILURE);}
            inputs[i]=g_ascii_strtoull(words[i],&end,0);
            if(*end||inputs[i]>UINT32_MAX) {error_report("te-context-fixture fullu32inputrange exceeded");exit(EXIT_FAILURE);}
        }
        O62MTeContextResult result=o62m_te_context_init(&s->te_context,inputs,O62M_TE_CONTEXT_INITIAL_WORDS);
        if(result!=O62M_TE_CONTEXT_OK) {error_report("te-context-fixture invalid: %s",o62m_te_context_reason(result));exit(EXIT_FAILURE);}
        if(s->trng_virtual_profile) {
            result=o62m_te_common_view_init_v1(&s->te_common_view,&s->te_context,&s->trng_virtual);
            if(result) {error_report("virtualAO COMMONview invalid/inconsistentinitialSWorKEYbits: %s",o62m_te_context_reason(result));exit(EXIT_FAILURE);}
            error_report("MARCONI_TE_COMMON_VIEW_PROFILE sw-init-initial=false "
                         "TOP1-STAT6-from-actual-hostwrite=true STAT23-from-real-private-store=true "
                         "capacity-model-unchanged=true no-other-OTP-or-ready-generated=true");
        }
        error_report("MARCONI_TE_CONTEXT_FIXTURE required-bytes=%u capacity-bytes=%u "
                     "module-hz=%u work-cycles=%u initial-ready-error-clear=true "
                     "geometry-hostinitial-clocktiming-explicit-conditional=true "
                     "physical-slotlayout-OTP-TRNG-crypto-unimplemented=true",
                     s->te_context.required_bytes,s->te_context.capacity_bytes,
                     s->te_context.module_hz,s->te_context.work_cycles);
    }
    if(s->te_capability_inputs) {
        if(!s->te_context_fixture||!s->trng_virtual_profile||!s->tee_reset||!s->explore_seed) {
            error_report("te-capability-inputs requires originalTEE/explore/COMMON/virtualAO profiles");exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words=g_strsplit(s->te_capability_inputs,":",-1);
        uint64_t raw[O62M_TE_CAPS_WORDS_V3];
        if(g_strv_length(words)!=O62M_TE_CAPS_WORDS_V3) {error_report("te-capability-inputs requires8rawfullwords");exit(EXIT_FAILURE);}
        for(unsigned i=0;i<O62M_TE_CAPS_WORDS_V3;i++) {
            char *end;
            if(!g_ascii_isdigit(words[i][0])) {error_report("te-capability-inputs unsignedfullwordrequired");exit(EXIT_FAILURE);}
            raw[i]=g_ascii_strtoull(words[i],&end,0);
            if(*end||raw[i]>UINT32_MAX) {error_report("te-capability-inputs u32rangeexceeded");exit(EXIT_FAILURE);}
        }
        O62MTeContextResult result=o62m_te_caps_init_v3(&s->te_capabilities,&s->te_context,raw,O62M_TE_CAPS_WORDS_V3);
        if(result) {error_report("te-capability-inputs invalid/COMMONgeometryconflict: %s",o62m_te_context_reason(result));exit(EXIT_FAILURE);}
        error_report("MARCONI_TE_CAPABILITY_PROFILE raw-words=8 COMMONgeometry-canonical=true "
                     "physical-reset-configuration-unknown=true crypto-DMA-OTP-completion-unimplemented=true");
    }
    if(s->aca_config_profile) {
        if(!s->te_capability_inputs||!s->te_context_fixture||!s->tee_reset||!s->explore_seed) {error_report("aca-config requires originalnormal COMMON/caps profiles");exit(EXIT_FAILURE);}
        g_auto(GStrv) words=g_strsplit(s->aca_config_profile,":",-1);
        if(g_strv_length(words)!=3) {error_report("aca-config requires explicitCTRL:MASK:SUSPD initialfullwords");exit(EXIT_FAILURE);}
        for(unsigned i=0;i<3;i++) {
            char *end;if(!g_ascii_isdigit(words[i][0])) {error_report("aca-config unsignedfullwordsrequired");exit(EXIT_FAILURE);}
            uint64_t v=g_ascii_strtoull(words[i],&end,0);if(*end||v>UINT32_MAX||(i==0&&(v&1))) {error_report("aca-config u32range/initialOP_RUNunsupported");exit(EXIT_FAILURE);}s->aca_config_word[i]=(uint32_t)v;
        }
        uint64_t geometry=0;
        if(o62m_te_caps_access_v3(&s->te_capabilities,&s->te_context,UINT64_C(0x5033000c),4,false,&geometry)||
           !o62m_aca_sram_init_v12(&s->aca_sram,((geometry>>27)&31u)*1024u)) {
            error_report("aca-sram requires existing canonical 8KiB geometry");exit(EXIT_FAILURE);
        }
        s->aca_work_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_aca_work_callback,s);
        qemu_register_reset(marconi_aca_sram_reset,s);
        s->aca_sram_exit.notify=marconi_aca_sram_exit;qemu_add_exit_notifier(&s->aca_sram_exit);
        error_report("MARCONI_ACA_CONFIG_PROFILE explicit3initialfullwords=true canonical-SRAM-bytes=8192 actual-observed-integer-operations-only=true positive-virtual-service-ns=1000");
    }
    if(s->symc_empty_profile) {
        if(!s->te_capability_inputs||!s->te_context_fixture||!s->tee_reset||!s->explore_seed) {
            error_report("symc-empty-hosts requires originalnormal COMMON/capability profiles");exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words=g_strsplit(s->symc_empty_profile,":",-1);uint32_t values[10];
        if(g_strv_length(words)!=10) {error_report("symc-empty-hosts needs2initialCTRL+SCA/HASH3mask+suspend each");exit(EXIT_FAILURE);}
        for(unsigned i=0;i<10;i++) {
            char *end;if(!g_ascii_isdigit(words[i][0])) {error_report("symc-empty-hosts unsignedfullwordsrequired");exit(EXIT_FAILURE);}
            uint64_t v=g_ascii_strtoull(words[i],&end,0);if(*end||v>UINT32_MAX) {error_report("symc-empty-hosts initialu32overflow");exit(EXIT_FAILURE);}values[i]=(uint32_t)v;
        }
        if(!o62m_hash_work_queue_init_v7(&s->symc_empty,&s->te_capabilities,values,10)) {
            error_report("symc-empty-hosts invalidgeometry/initial-run-input");exit(EXIT_FAILURE);
        }
        error_report("MARCONI_SYMC_EMPTY_PROFILE two-real-empty-queues=true CQdepth-from-same-caps=true "
                     "host-run-empty-policy=INACTIVE CQ-CSQ-key-ports-strict=true no-completion-generated=true");
    }
    if(s->rnp_mask_config) {
        if(!s->trng_virtual_profile||!s->te_capability_inputs||!s->tee_reset||!s->explore_seed) {
            error_report("rnp-mask-config requires originalnormal COMMON/caps/virtualAO profiles");exit(EXIT_FAILURE);
        }
        char *end;if(!g_ascii_isdigit(s->rnp_mask_config[0])) {error_report("rnp-mask-config unsignedu32initialwordrequired");exit(EXIT_FAILURE);}
        uint64_t value=g_ascii_strtoull(s->rnp_mask_config,&end,0);
        if(*end||value>UINT32_MAX) {error_report("rnp-mask-config initialwordoverflowsu32");exit(EXIT_FAILURE);}
        s->rnp_mask_word=(uint32_t)value;
        error_report("MARCONI_RNP_MASK_PROFILE explicit-initial=%08x fill-and-data-strict=true no-entropy-exposure=true",s->rnp_mask_word);
    }
    if(s->rnp_postkey_config) {
        if(!s->rnp_mask_config||!s->trng_virtual_profile) {error_report("rnp-postkey-config requires explicitmask/virtualAO profiles");exit(EXIT_FAILURE);}
        g_auto(GStrv) words=g_strsplit(s->rnp_postkey_config,":",-1);uint32_t values[2];
        if(g_strv_length(words)!=2) {error_report("rnp-postkey-config needs expliciterrThreshold:controllerMask initialwords");exit(EXIT_FAILURE);}
        for(unsigned i=0;i<2;i++) {
            char *end;if(!g_ascii_isdigit(words[i][0])) {error_report("rnp-postkey-config unsignedu32required");exit(EXIT_FAILURE);}
            uint64_t v=g_ascii_strtoull(words[i],&end,0);if(*end||v>UINT32_MAX) {error_report("rnp-postkey-config u32overflow");exit(EXIT_FAILURE);}values[i]=(uint32_t)v;
        }
        s->rnp_live_config[5]=values[0];s->rnp_controller_mask=values[1];
        s->rnp_pool_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_rnp_pool_callback,s);
        s->rnp_pool_exit.notify=marconi_rnp_pool_exit;qemu_add_exit_notifier(&s->rnp_pool_exit);
        qemu_register_reset(marconi_rnp_pool_reset,s);

        error_report("MARCONI_RNP_POSTKEY_PROFILE threshold-initial=%08x controller-mask-initial=%08x "
                     "live-bank-inactive-until-actual-private-AO-key=true unsupported-config-effects-not-synthesized=true",values[0],values[1]);
    }
    if(s->virtual_otp_profile||s->virtual_otp_public_template) {
        if(!s->virtual_otp_profile||!s->virtual_otp_public_template||!s->te_capability_inputs||
           !s->trng_virtual_profile||!s->tee_reset||!s->explore_seed||
           (s->te_context.initial_stat&O62M_VOTP_STAT_MASK_V1)) {
            error_report("virtualOTP requires paired explicitpublictemplate/profile andnormal COMMON/caps/virtualAO withinitialownedSTATbitsclear");exit(EXIT_FAILURE);
        }
        uint32_t cfg=s->te_capabilities.raw[2],otp0=s->te_capabilities.raw[5],otp1=s->te_capabilities.raw[6];
        if((cfg&UINT32_C(0xf))!=7u) {error_report("virtualOTP requires declared internalTRNG/OTPexists/AOshadow andnormalizedzero-initial-value profile");exit(EXIT_FAILURE);}
        unsigned key_bytes=(cfg&16)?32:16;
        unsigned image_bytes=o62m_virtual_otp_prefix_v1(key_bytes)+
                             4u*((otp0&0x7ff)+((otp0>>12)&0x7ff)+(otp1&0x7ff));
        g_auto(GStrv) words=g_strsplit(s->virtual_otp_profile,":",-1);uint64_t values[3];
        if(g_strv_length(words)!=3) {error_report("virtualOTP requires workNs:settingOpaque:updateOpaque");exit(EXIT_FAILURE);}
        for(unsigned i=0;i<3;i++) {
            char *end;if(!g_ascii_isdigit(words[i][0])) {error_report("virtualOTP unsignedexplicitparametersrequired");exit(EXIT_FAILURE);}
            values[i]=g_ascii_strtoull(words[i],&end,0);
            if(*end||(i==0?values[i]>INT64_MAX:values[i]>UINT32_MAX)) {error_report("virtualOTP parameter-range-invalid");exit(EXIT_FAILURE);}
        }
        g_autofree char *public_image=NULL;gsize length=0;GError *file_error=NULL;
        if(!g_file_get_contents(s->virtual_otp_public_template,&public_image,&length,&file_error)) {
            error_report("virtualOTP publictemplate unreadable");g_clear_error(&file_error);exit(EXIT_FAILURE);
        }
        if(length!=image_bytes||length>O62M_VOTP_MAX_V1) {error_report("virtualOTP publictemplate size conflicts withcanonicalcapabilitygeometry");exit(EXIT_FAILURE);}
        int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        O62MVirtualOtpSpecV1 spec={2,now,(int64_t)values[0],key_bytes,image_bytes,
                                  (uint32_t)values[1],(uint32_t)values[2],(const uint8_t *)public_image,length};
        int result=o62m_virtual_otp_init_v1(&s->virtual_otp,&spec);
        if(result) {error_report("virtualOTP invalid explicittemplate/lifecycle input code=%d",result);exit(EXIT_FAILURE);}
        s->virtual_otp_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_virtual_otp_callback,s);
        s->virtual_otp_exit.notify=marconi_virtual_otp_exit;qemu_add_exit_notifier(&s->virtual_otp_exit);
        s->virtual_otp_exit_registered=true;timer_mod_ns(s->virtual_otp_event,s->virtual_otp.due_ns);
        error_report("MARCONI_VIRTUAL_OTP_PROFILE image-bytes=%u key-bytes=%u work-ns=%" PRIu64
                     " source-trigger=explicit-virtual-power-on private-backing-empty=true shadows-invalid=true "
                     "autonomous-AO-clock-assumption=true silicon-not-claimed=true programming-PUF-and-secret-MMIO-unimplemented=true",
                     image_bytes,key_bytes,values[0]);
    }
    if(s->hash_init_profile) {
        if(!s->symc_empty_profile||!s->virtual_otp_profile||!s->tee_reset||!s->explore_seed||!s->uart_tx_fixture||
           s->virtual_otp.key_bytes!=16||(s->te_capabilities.raw[2]&16)||
           (s->te_context.initial_host0_irq&3)) {
            error_report("hash-init-ladder128 requires explicitOTP16/COMMON/symc profile andunownedinitialHASH/SCAIRQbits");exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words=g_strsplit(s->hash_init_profile,":",-1);
        if(g_strv_length(words)!=2||strcmp(words[0],"aes128-dec3-hmac-sha256")) {error_report("hash-init-ladder128 explicitformula:workNs required");exit(EXIT_FAILURE);}
        char *end;if(!g_ascii_isdigit(words[1][0])) {error_report("hash-init-ladder128 unsignedlatencyrequired");exit(EXIT_FAILURE);}
        s->hash_work_ns=g_ascii_strtoull(words[1],&end,0);
        if(*end||!s->hash_work_ns||s->hash_work_ns>INT64_MAX) {error_report("hash-init-ladder128 positiveworkNsrangeinvalid");exit(EXIT_FAILURE);}
        s->hash_init_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_hash_init_callback,s);
        s->hash_exit.notify=marconi_hash_exit;qemu_add_exit_notifier(&s->hash_exit);s->hash_exit_registered=true;
        error_report("MARCONI_HASH_INIT_PROFILE formula=explicit-AES128-ECB-DEC3-HMAC-SHA256 OTP16-only=true "
                     "no32B-root-truncation=true actual-source-required=true work-ns=%" PRIu64
                     " physical-keyladder-formula-or-NVIC-route-not-claimed=true",s->hash_work_ns);
    }
    if (s->te_platform_fatal_fixture) {
        if (!s->tee_reset||!s->explore_seed) {
            error_report("te-platform-fatal-fixture requires original TEE reset and explicit explore-seed");exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words=g_strsplit(s->te_platform_fatal_fixture,":",-1);
        uint32_t seeds[2];
        if(g_strv_length(words)!=2) {
            error_report("te-platform-fatal-fixture requires explicit initialTOP_CONF andfatal HOST_STATE words");exit(EXIT_FAILURE);
        }
        for(unsigned i=0;i<2;i++) {
            char *end;
            if(!g_ascii_isdigit(words[i][0])) {error_report("te-platform-fatal-fixture requires unsigned values");exit(EXIT_FAILURE);}
            uint64_t word=g_ascii_strtoull(words[i],&end,0);
            if(*end||word>UINT32_MAX) {error_report("te-platform-fatal-fixture words must fit fullu32");exit(EXIT_FAILURE);}
            seeds[i]=(uint32_t)word;
        }
        if(!o62m_te_platform_fatal_init(&s->te_platform_fatal,seeds[0],seeds[1])) {
            error_report("te-platform-fatal-fixture requires initialTOP_CONF.bit0=0 andHOST_STATE.bit5=1; no successful-status fixture permitted");exit(EXIT_FAILURE);
        }
        error_report("MARCONI_TE_FATAL_FIXTURE words=%s single-original-fault-attempt=true normal-boot=false "
            "retry-ready-globalSTAT-entropy-crypto-unsupported=true",s->te_platform_fatal_fixture);
    }
    if(s->ap_soc4005_2c0_initial) {
        char *end;
        if(s->soc4005_config_inputs||!s->tee_reset||!s->tee_ap_image||!s->explore_seed||!g_ascii_isdigit(s->ap_soc4005_2c0_initial[0])) {
            error_report("ap-soc4005-2c0-initial requires originalTEE/AP exploreseed unsignedfullword andexclusivewithOTAthreewordprofile");exit(EXIT_FAILURE);
        }
        uint64_t initial=g_ascii_strtoull(s->ap_soc4005_2c0_initial,&end,0);
        if(*end||initial>UINT32_MAX) {error_report("ap-soc4005-2c0-initial mustfitfullu32");exit(EXIT_FAILURE);}
        uint32_t words[2]={0,(uint32_t)initial};
        if(o62m_clock_controls_init(&s->soc4005_config,true,&o62m_soc4005_config_defs_v1[1],2,words,2)!=O62M_CLOCK_CONTROLS_OK) {
            error_report("ap-soc4005-2c0-initial canonicaltwowordsubsetinitfailed");exit(EXIT_FAILURE);
        }
        error_report("MARCONI_AP_SOC4005_2C0_PROFILE exact-existing-definitions1-2-subset=true A4-not-enabled=true B4-explicit-virtual-cold0=true independent-fullword-initial-physical-reset-unknown=true exact-original-AP-RMW=true no-device-ready-or-completion=true");
    }
    if(s->soc4005_config_inputs) {
        if(!s->ota_reset||!s->explore_seed) {
            error_report("soc4005-config-inputs requires originalOTA andexplicitexplore-seed");exit(EXIT_FAILURE);
        }
        g_autofree gchar *bytes=NULL;gsize length=0;
        if(!g_file_get_contents(s->soc4005_config_inputs,&bytes,&length,NULL)||length!=12) {
            error_report("soc4005-config-inputs requires exact12BLEthreeindependentfullu32words A4,B4,2C0");exit(EXIT_FAILURE);
        }
        uint32_t initial[O62M_SOC4005_CONFIG_COUNT_V1];
        for(unsigned i=0;i<O62M_SOC4005_CONFIG_COUNT_V1;i++) {initial[i]=ldl_le_p(bytes+4*i);}
        int result=o62m_clock_controls_init(&s->soc4005_config,true,o62m_soc4005_config_defs_v1,
            O62M_SOC4005_CONFIG_COUNT_V1,initial,O62M_SOC4005_CONFIG_COUNT_V1);
        if(result) {error_report("soc4005-config-inputs invalid: %s",o62m_clock_controls_reason(result));exit(EXIT_FAILURE);}
        g_autofree gchar *digest=g_compute_checksum_for_data(G_CHECKSUM_SHA256,(const guchar *)bytes,length);
        error_report("MARCONI_SOC4005_CONFIG_INPUTS bytes=12 words=3 sha256=%s "
                     "A4-single-active-backing=true legacy-A4-projection-unused=true "
                     "no-actual-write-inferred-from-input=true physical-reset-effects-and-ready-unknown=true",digest);
    }
    (void)o62m_clock_controls_init(&s->pll_rate,false,NULL,0,NULL,0);
    if (s->pll_rate_fixture) {
        if ((!s->tee_reset&&!s->ota_reset)||!s->explore_seed) {
            error_report("pll-rate-fixture requires original TEE orOTA reset and explicit explore-seed");exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words=g_strsplit(s->pll_rate_fixture,":",-1);
        uint32_t seeds[O62M_PLL_RATE_COUNT];
        if (g_strv_length(words)!=O62M_PLL_RATE_COUNT) {
            error_report("pll-rate-fixture requires exactly12 explicit fullu32 words");exit(EXIT_FAILURE);
        }
        for(unsigned i=0;i<O62M_PLL_RATE_COUNT;i++) {
            char *end;
            if(!g_ascii_isdigit(words[i][0])) {error_report("pll-rate-fixture words require unsigned values");exit(EXIT_FAILURE);}
            uint64_t word=g_ascii_strtoull(words[i],&end,0);
            if(*end||word>UINT32_MAX) {error_report("pll-rate-fixture words must fit fullu32");exit(EXIT_FAILURE);}
            seeds[i]=(uint32_t)word;
        }
        assert(o62m_clock_controls_init(&s->pll_rate,true,o62m_pll_rate_defs,O62M_PLL_RATE_COUNT,seeds,O62M_PLL_RATE_COUNT)==O62M_CLOCK_CONTROLS_OK);
        error_report("MARCONI_PLL_RATE_FIXTURE words=%s explicit-unknown-initial-state=true "
            "only-original-rate-read-no-lock-status=true",s->pll_rate_fixture);
    }
    if(s->pll_rate_fixture&&s->ap_board_config&&s->tee_reset&&s->tee_ap_image) {
        s->mempll_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_mempll_callback,s);
        o62m_mempll_reset_v1(&s->mempll_vote);
        qemu_register_reset(marconi_mempll_reset,s);
        s->mempll_exit.notify=marconi_mempll_exit;qemu_add_exit_notifier(&s->mempll_exit);
        marconi_public_report("MARCONI_MEMPLL_PROFILE reference-Hz=26000000 qualification-ns=1000 "
            " cold-votes-zero=true cold-lock-false=true rate-owner-unchanged=true virtual-qualification=true");
    }
    (void)o62m_clock_controls_init(&s->aon120_config,false,NULL,0,NULL,0);
    if(s->aon120_config_inputs) {
        if(!s->tee_reset||!s->tee_ap_image||!s->explore_seed) {error_report("aon120-config-inputs originalTEE/AP/explore required");exit(EXIT_FAILURE);}
        g_autofree gchar *bytes=NULL;gsize length=0;
        if(!g_file_get_contents(s->aon120_config_inputs,&bytes,&length,NULL)||length!=4*O62M_AON120_CONFIG_COUNT_V2) {error_report("aon120-config-inputs exact25fullwordinitials required");exit(EXIT_FAILURE);}
        uint32_t initial[O62M_AON120_CONFIG_COUNT_V2];for(unsigned i=0;i<O62M_AON120_CONFIG_COUNT_V2;i++)initial[i]=ldl_le_p(bytes+4*i);
        if(o62m_clock_controls_init(&s->aon120_config,true,o62m_aon120_config_defs_v2,O62M_AON120_CONFIG_COUNT_V2,initial,O62M_AON120_CONFIG_COUNT_V2)!=O62M_CLOCK_CONTROLS_OK) {error_report("aon120configdefinition/input invalid");exit(EXIT_FAILURE);}
        error_report("MARCONI_AON120_CONFIG_INPUTS exact25originalports=true independent-fullword-initials=true config-alone-does-not-create-workcompletion-or-IRQ=true no-physicalreset-alias-or-hardwareeffect-claim=true");
    }
    if(s->pmic_button_config_initial) {
        char *end;
        if(!s->tee_reset||!s->tee_ap_image||!s->explore_seed||!s->adc_fixture||
           !g_ascii_isdigit(s->pmic_button_config_initial[0])) {
            error_report("pmic-button-config-initial requires originalTEE/AP sharedSPMI andunsignedexplicitfull16word");exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words=g_strsplit(s->pmic_button_config_initial,":",-1);
        unsigned count=g_strv_length(words);
        if(count<1||count>3) {error_report("pmic-button-config-initial requires initial1044[:initial107c[:initial10ac]]");exit(EXIT_FAILURE);}
        for(unsigned i=0;i<count;i++) {
            if(!g_ascii_isdigit(words[i][0])) {error_report("pmic-button-config-initial unsignedwordrequired");exit(EXIT_FAILURE);}
            uint64_t word=g_ascii_strtoull(words[i],&end,0);
            if(*end||word>UINT16_MAX) {error_report("pmic-button-config-initial mustfitfull16word");exit(EXIT_FAILURE);}
            if(i==0)s->pmic_button_config_word=(uint16_t)word;
            else if(i==1) {s->pmic_button_followup_word=(uint16_t)word;s->pmic_button_followup_present=true;}
            else {s->pmic_button_second_followup_word=(uint16_t)word;s->pmic_button_second_followup_present=true;}
        }
        error_report("MARCONI_PMIC_BUTTON_CONFIG_PROFILE independent-explicit-full16-initials=true exact1044-and-optional107c-10ac-only=true "
                     "original-single-bit-OR-fields063f-and1-and3-only=true actual-shared-SPMI-commit-required=true "
                     "physical-POR-bit-meaning-button-input-and-IRQ-unmodeled=true");
    }

    if(s->timer64_terminal_profile&&(!s->timer64||!s->tee_ap_image||strcmp(s->timer64_terminal_profile,"hold-irq-v1"))) {
        error_report("timer64-terminal requires timer64 originalAP and explicit hold-irq-v1 virtual terminal policy");exit(EXIT_FAILURE);
    }
    if(s->pmic_boot_events_profile) {
        if(!s->tee_reset||!s->tee_ap_image||!s->adc_fixture||!s->explore_seed||strcmp(s->pmic_boot_events_profile,"quiescent-v6")) {
            error_report("pmic-boot-events requires originalTEE/AP selected-PMIC fixture and explicit quiescent-v6 virtual profile");exit(EXIT_FAILURE);
        }
        o62m_pmic_boot_events_init_v6(&s->pmic_boot_events);
        error_report("MARCONI_PMIC_BOOT_EVENTS_PROFILE config1030-zero=true virtual-version-zero=true independent-pending-empty=true recorded-words-read-before-write-rejected=true");
    }
    if(s->pmic_vbus_input&&(!s->tee_reset||!s->tee_ap_image||!s->adc_fixture||!s->explore_seed||strcmp(s->pmic_vbus_input,"offline-v1"))) {
        error_report("pmic-vbus-input requires originalTEE/AP selected-PMIC fixture and explicit offline-v1 virtual input");exit(EXIT_FAILURE);
    }
    if(s->adsp_publication_profile&&(!s->tee_reset||!s->tee_ap_image||!s->explore_seed||strcmp(s->adsp_publication_profile,"unpublished-v1"))) {
        error_report("adsp-publication requires originalTEE/AP explore and explicit unpublished-v1 virtual platform");exit(EXIT_FAILURE);
    }
    if(s->ap_mailbox_config) {
        if(!s->tee_reset||!s->tee_ap_image||!s->explore_seed||strcmp(s->ap_mailbox_config,"zero-outbound-v2")) {
            error_report("ap-mailbox-config requires originalTEE/AP explore and explicit zero-outbound-v2 virtual profile");exit(EXIT_FAILURE);
        }
        memset(&s->ap_ipc_slots,0,sizeof(s->ap_ipc_slots));
        qemu_register_reset(marconi_ap_ipc_slots_reset,s);
        s->ap_ipc_slots_exit.notify=marconi_ap_ipc_slots_exit;qemu_add_exit_notifier(&s->ap_ipc_slots_exit);
        s->ap_mailbox_enable=0;
        memset(&s->ap_mbox_outbound,0,sizeof(s->ap_mbox_outbound));
        if(s->mbox_outbound_experiment&&s->board_controls_fixture) {
            memset(&s->ap_tee_mbox,0,sizeof(s->ap_tee_mbox));
            qemu_register_reset(marconi_ap_tee_mbox_reset,s);
            s->ap_tee_mbox_exit.notify=marconi_ap_tee_mbox_exit;qemu_add_exit_notifier(&s->ap_tee_mbox_exit);
        }
        error_report("MARCONI_AP_MAILBOX_CONFIG_PROFILE initial-enable-mask-zero=true AP-configuration-only=true no-peer-state-alias-or-completion-inferred=true");
    }
    if(s->venc_device_profile&&(!s->tee_reset||!s->tee_ap_image||!s->explore_seed||strcmp(s->venc_device_profile,"absent-v1"))) {
        error_report("venc-device requires originalTEE/AP explore and explicit absent-v1 virtual platform");exit(EXIT_FAILURE);
    }
    if(s->gpu_boot_identity_profile&&(!s->gpu_boot_reset_profile||strcmp(s->gpu_boot_identity_profile,"ap-target-partial-v1"))) {
        error_report("gpu-boot-identity requires ap-target-partial-v1 withcausalresetdevice");exit(EXIT_FAILURE);
    }
    if(s->gpu_boot_reset_profile) {
        g_auto(GStrv) parts=g_strsplit(s->gpu_boot_reset_profile,":",-1);
        if(g_strv_length(parts)!=4||strcmp(parts[0],"virtual-reset-v1")||!s->tee_reset||!s->tee_ap_image||!s->explore_seed) {
            error_report("gpu-boot-reset-profile requires virtual-reset-v1:control:opaqueStatus:latencyNs and originalTEE/AP");exit(EXIT_FAILURE);
        }
        uint64_t inputs[3];
        for(unsigned i=0;i<3;i++) {
            char *end;if(!g_ascii_isdigit(parts[i+1][0])){error_report("gpu boot inputs unsigned required");exit(EXIT_FAILURE);}
            inputs[i]=g_ascii_strtoull(parts[i+1],&end,0);
            if(*end||inputs[i]>(i<2?UINT32_MAX:INT64_MAX)){error_report("gpu boot input out of range");exit(EXIT_FAILURE);}
        }
        if(!o62m_gpu_boot_init_v1(&s->gpu_boot_reset,inputs[0],inputs[1],inputs[2],qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
            error_report("gpu boot initial reset/status/positive latency invalid");exit(EXIT_FAILURE);
        }
        s->gpu_boot_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_gpu_boot_callback,s);
        s->gpu_fill_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_gpu_fill_callback,s);
        qemu_register_reset(marconi_gpu_fill_reset,s);
        s->gpu_boot_exit.notify=marconi_gpu_boot_exit;qemu_add_exit_notifier(&s->gpu_boot_exit);
        error_report("MARCONI_GPU_BOOT_RESET_PROFILE external-power-and-reference-clock-available=true initial-control-and-opaque-status-explicit=true initial-idle-false=true latency-ns=%" PRIu64 " virtual-functional-profile=true",inputs[2]);
    }
    if(s->pmic_sync107c_profile) {
        g_auto(GStrv) parts=g_strsplit(s->pmic_sync107c_profile,":",-1);
        char *end;
        if(g_strv_length(parts)!=2||strcmp(parts[0],"accepted-prefix-v1")||
           !g_ascii_isdigit(parts[1][0])||!s->pmic_button_followup_present||
           (s->pmic_button_followup_word&2u)) {
            error_report("pmic-sync107c-profile requires accepted-prefix-v1:positiveNs and canonical107c backing with bit1zero");exit(EXIT_FAILURE);
        }
        uint64_t latency=g_ascii_strtoull(parts[1],&end,0);
        if(*end||!latency||latency>INT64_MAX||
           o62m_pmic_sync107c_init_v1(&s->pmic_sync107c,true,(int64_t)latency,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))!=O62M_PMIC_SYNC107C_OK_V1) {
            error_report("pmic-sync107c-profile invalid positive latency or virtual clock");exit(EXIT_FAILURE);
        }
        s->pmic_sync107c_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_pmic_sync107c_callback,s);
        s->pmic_sync107c_exit.notify=marconi_pmic_sync107c_exit;
        qemu_add_exit_notifier(&s->pmic_sync107c_exit);
        error_report("MARCONI_PMIC_SYNC107C_PROFILE latency-ns=%" PRIu64
            " retained-request-bit0=true independent-RO-ACK-bit1-initialzero=true repeated-W1-new-generation=true accepted-SPMI-prefix-only=true virtual-functional-policy=true",latency);
    }

    if(s->pmic_alias_read_policy) {
        if(strcmp(s->pmic_alias_read_policy,"read-zero-v1")||!s->tee_reset||!s->tee_ap_image||!s->adc_fixture||!s->pmic_regulator_inputs) {
            error_report("pmic-alias-read-policy requires read-zero-v1 originalTEE/AP/sharedSPMI/canonicalregulatorowner");exit(EXIT_FAILURE);
        }
        error_report("MARCONI_PMIC_ALIAS_READ_PROFILE exact-eight-enable-command-ports-only=true optin-read-as-zero-functional-policy=true ordinary-v1-read-still-strict=true no-alias-backing-or-base-mirror=true existing-shared-SPMI-canonical-SET-CLEAR-write-required=true no-physical-read-ready-or-IRQ-claim=true");
    }
    (void)o62m_clock_controls_init(&s->pmic_mode_config,false,NULL,0,NULL,0);
    if(s->pmic_mode_config_inputs) {
        if(!s->tee_reset||!s->tee_ap_image||!s->adc_fixture){error_report("pmic-mode-config-inputs originalTEE/AP/sharedSPMI required");exit(EXIT_FAILURE);}
        g_autofree gchar *bytes=NULL;gsize length=0;uint32_t initial[7];
        if(!g_file_get_contents(s->pmic_mode_config_inputs,&bytes,&length,NULL)||(length!=16&&length!=28)){error_report("pmic-mode-config-inputs four or seven u32containers required");exit(EXIT_FAILURE);}
        unsigned count=length/4;s->pmic_mode_extended=count==7;
        if(s->pmic_mode_extended&&!s->auxadc_channel_profile){error_report("extended PMIC mode configuration requires canonical AP ADC owner");exit(EXIT_FAILURE);}
        s->pmic_low_event_pending=0;
        for(unsigned i=0;i<count;i++){initial[i]=ldl_le_p(bytes+4*i);if(initial[i]>UINT16_MAX){error_report("PMICmode initials mustfitfull16");exit(EXIT_FAILURE);}}
        const O62MClockControlDef *defs=s->pmic_mode_extended?o62m_pmic_mode_config_defs_v4:o62m_pmic_mode_config_defs_v1;
        if(o62m_clock_controls_init(&s->pmic_mode_config,true,defs,count,initial,count)!=O62M_CLOCK_CONTROLS_OK){error_report("PMICmode configdefinition/input invalid");exit(EXIT_FAILURE);}
        error_report("MARCONI_PMIC_MODE_CONFIG_PROFILE independent-full16-initials=true ROM-individual-fields-only=true F0-single-canonical-owner-and-explicit-SET-CLEAR=true GO50-and-active-mode-application-strict=true no-physical-POR-or-completion-claim=true");
    }
    if(s->pmic_voltage_source_profile) {
        if(!s->pmic_mode_extended||!s->tee_reset||!s->tee_ap_image) {
            error_report("pmic-voltage-source requires originalTEE/AP andextendedPMIC configuration");exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words=g_strsplit(s->pmic_voltage_source_profile,":",-1);uint64_t input[6];
        if(g_strv_length(words)!=6){error_report("pmic-voltage-source raw24:latencyNS:sourceAvailable:clockAvailable:currentRaw24:currentBias required");exit(EXIT_FAILURE);}
        for(unsigned i=0;i<6;i++) {
            char *end;if(!g_ascii_isdigit(words[i][0])){error_report("pmic-voltage-source unsigned input required");exit(EXIT_FAILURE);}
            input[i]=g_ascii_strtoull(words[i],&end,0);if(*end){error_report("pmic-voltage-source invalid input");exit(EXIT_FAILURE);}
        }
        if(input[0]>0xffffff||!input[1]||input[1]>INT64_MAX||input[2]>1||input[3]>1||input[4]>0x7fffff||input[5]>0x7fffff||
           !o62m_pmic_voltage_init_v6(&s->pmic_voltage,input[0],input[1],input[2],input[3],input[4],input[5],qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
            error_report("pmic-voltage-source input range invalid");exit(EXIT_FAILURE);
        }
        s->pmic_voltage_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_pmic_voltage_callback,s);
        s->pmic_voltage_exit.notify=marconi_pmic_voltage_exit;qemu_add_exit_notifier(&s->pmic_voltage_exit);
        qemu_register_reset(marconi_pmic_voltage_reset,s);
        error_report("MARCONI_PMIC_VOLTAGE_PROFILE explicit-voltage-and-current24-inputs=true positive-virtual-delay=true virtual-F0-enable-mask0803=true ACK-then-next-sample=true no-physical-source-calibration-or-IRQ-claim=true");
    }
    if(s->virtual_pmic_bc_config) {
        char *end;if(!s->tee_reset||!s->tee_ap_image||!s->adc_fixture||!g_ascii_isdigit(s->virtual_pmic_bc_config[0])) {
            error_report("virtual-pmic-bc-config requires originalTEE/AP/sharedSPMI andunsignedinitialBCword");exit(EXIT_FAILURE);
        }
        uint64_t word=g_ascii_strtoull(s->virtual_pmic_bc_config,&end,0);
        if(*end||word>UINT16_MAX) {error_report("virtual-pmic-bc-config requires independentu16word");exit(EXIT_FAILURE);}
        s->pmic_bc_word=word;
        error_report("MARCONI_PMIC_BC_CONFIG_PROFILE independent-full16-initialword=true single-bit-SET-baseline-ADCprofile-adds-original-single-bit-CLEAR=true 16-bit-functional-config-policy-not-silicon-claim=true actual-shared-SPMI-commit-required=true no-CC-status-ADCready-or-IRQ-generated=true");
    }
    if(s->virtual_auxadc_reset_profile) {
        char *end;if(!s->tee_reset||!s->tee_ap_image||!s->adc_fixture||!g_ascii_isdigit(s->virtual_auxadc_reset_profile[0])) {
            error_report("virtual-auxadc-reset-profile requires originalTEE/AP/ADC andexplicitunsigned294word");exit(EXIT_FAILURE);
        }
        uint64_t word=g_ascii_strtoull(s->virtual_auxadc_reset_profile,&end,0);
        if(*end||word>UINT16_MAX||!o62m_virtual_auxadc_reset_init_v1(&s->virtual_auxadc_reset,word)) {
            error_report("virtual-auxadc-reset-profile requires independentu16initial294word");exit(EXIT_FAILURE);
        }
        error_report("MARCONI_VIRTUAL_AUXADC_PROFILE independent294word-explicit=true initial-reset-released-virtual-policy=true exact-a14-514-value2-only=true effect-only-after-actual-shared-SPMI-deadline=true measurement-reset-preserves-config-inputs-other-PMIC=true physical-294-sideeffects-resetdomain-and-POR-unclaimed=true");
    }
    if(s->virtual_dvs_profile) {
        if(!s->aon120_config_inputs||!s->pmic_regulator_inputs||!s->adc_fixture||!s->tee_reset||!s->tee_ap_image) {
            error_report("virtual-dvs-profile originalTEE/AP canonicalDVFS-config andsharedSPMI/PMIC peers required");exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words=g_strsplit(s->virtual_dvs_profile,":",-1);uint64_t inputs[3];
        if(g_strv_length(words)!=3) {error_report("virtual-dvs-profile requires initialIndex:initialMV:settleNs");exit(EXIT_FAILURE);}
        for(unsigned i=0;i<3;i++) {
            char *end;if(!g_ascii_isdigit(words[i][0])) {error_report("virtual-dvs unsignedinputs required");exit(EXIT_FAILURE);}
            inputs[i]=g_ascii_strtoull(words[i],&end,0);if(*end||inputs[i]>INT64_MAX) {error_report("virtual-dvs inputrange invalid");exit(EXIT_FAILURE);}
        }
        if(inputs[0]>7||inputs[1]>UINT32_MAX||o62m_virtual_dvs_init_v1(&s->virtual_dvs,inputs[0],inputs[1],inputs[2],qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))!=O62M_VDVS_V1_OK) {
            error_report("virtual-dvs initialindex0..7 validMV500..1130step10 positivesettle required");exit(EXIT_FAILURE);
        }
        s->virtual_dvs_requested=inputs[0];s->virtual_dvs_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_virtual_dvs_callback,s);
        error_report("MARCONI_VIRTUAL_DVS_PROFILE explicit-initial-index=%u explicit-initial-virtual-mV=%u settle-ns=%" PRIu64 " SID8-reg0240-mode1-packed-equalpair-is-virtual-policy=true shared-SPMI-canonical-PMIC-commit-required=true no-fc0-feedback-projection=true physical-reset-and-analog-semantics-not-claimed=true",(unsigned)inputs[0],(unsigned)inputs[1],inputs[2]);
    }
    if(s->pm_bit9_profile) {
        if(!s->tee_reset||!s->tee_ap_image) {error_report("pm-bit9-profile originalTEE/AP required");exit(EXIT_FAILURE);}
        g_auto(GStrv) words=g_strsplit(s->pm_bit9_profile,":",-1);uint64_t inputs[2];
        if(g_strv_length(words)!=2) {error_report("pm-bit9-profile requires initial-status-word0or1:transitionNs");exit(EXIT_FAILURE);}
        for(unsigned i=0;i<2;i++) {
            char *end;if(!g_ascii_isdigit(words[i][0])) {error_report("pm-bit9 unsignedinputs required");exit(EXIT_FAILURE);}
            inputs[i]=g_ascii_strtoull(words[i],&end,0);if(*end||inputs[i]>INT64_MAX) {error_report("pm-bit9 inputrange invalid");exit(EXIT_FAILURE);}
        }
        if(inputs[0]>1||!o62m_pm_bit_domain9_init_v1(&s->pm_bit9,inputs[0],inputs[1],qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
            error_report("pm-bit9 initialword0or1 andpositive latency required");exit(EXIT_FAILURE);
        }
        s->pm_bit9_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_pm_bit9_callback,s);
        error_report("MARCONI_PM_BIT9_PROFILE explicit-initial-status-word=%u pending-bit-is-virtual-not-settled-on-policy=true "
                     "power-state-only-changes-at-actual-request-deadline=true down-has-no-guest-postpoll=true physical-reset-and-busy-encoding-unknown=true",(unsigned)inputs[0]);
    }
    if(s->pm_bit8_profile) {
        if(!s->tee_reset||!s->tee_ap_image) {error_report("pm-bit8-profile originalTEE/AP required");exit(EXIT_FAILURE);}
        g_auto(GStrv) words=g_strsplit(s->pm_bit8_profile,":",-1);uint64_t inputs[2];
        if(g_strv_length(words)!=2) {error_report("pm-bit8-profile requires initial-status-word0or1:transitionNs");exit(EXIT_FAILURE);}
        for(unsigned i=0;i<2;i++) {
            char *end;if(!g_ascii_isdigit(words[i][0])) {error_report("pm-bit8 unsignedinputs required");exit(EXIT_FAILURE);}
            inputs[i]=g_ascii_strtoull(words[i],&end,0);if(*end||inputs[i]>INT64_MAX) {error_report("pm-bit8 inputrange invalid");exit(EXIT_FAILURE);}
        }
        if(inputs[0]>1||!o62m_pm_bit_domain9_init_v1(&s->pm_bit8,inputs[0],inputs[1],qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
            error_report("pm-bit8 initialword0or1 andpositive latency required");exit(EXIT_FAILURE);
        }
        s->pm_bit8_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_pm_bit8_callback,s);
        error_report("MARCONI_PM_BIT8_PROFILE explicit-initial-status-word=%u pending-bit-is-virtual-not-settled-on-policy=true "
                     "power-state-only-changes-at-actual-request-deadline=true down-has-no-guest-postpoll=true physical-reset-and-busy-encoding-unknown=true",(unsigned)inputs[0]);
    }
    (void)o62m_clock_controls_init(&s->vpu_clocks,false,NULL,0,NULL,0);
    if(s->vpu_clock_inputs) {
        if(!s->pm_domain7_profile||!s->tee_reset||!s->tee_ap_image) {error_report("vpu-clock-inputs requires explicitdomain7profile andoriginalTEE/AP");exit(EXIT_FAILURE);}
        g_autofree gchar *bytes=NULL;gsize length=0;
        if(!g_file_get_contents(s->vpu_clock_inputs,&bytes,&length,NULL)||length!=4*O62M_VPU_CLOCK_COUNT_V1) {error_report("vpu-clock-inputs exact15fullwordinitialinputs required");exit(EXIT_FAILURE);}
        uint32_t initial[O62M_VPU_CLOCK_COUNT_V1];for(unsigned i=0;i<O62M_VPU_CLOCK_COUNT_V1;i++)initial[i]=ldl_le_p(bytes+4*i);
        if(o62m_clock_controls_init(&s->vpu_clocks,true,o62m_vpu_clock_defs_v1,O62M_VPU_CLOCK_COUNT_V1,initial,O62M_VPU_CLOCK_COUNT_V1)!=O62M_CLOCK_CONTROLS_OK) {error_report("VPU clockcatalog/input invalid");exit(EXIT_FAILURE);}
        error_report("MARCONI_VPU_CLOCK_INPUTS exact15-ROMwords-21fields=true independent-fullword-inputs=true no-PLL-lock-ready-or-render-claim=true");
    }
    (void)o62m_clock_controls_init(&s->usb_clocks,false,NULL,0,NULL,0);
    if(s->usb_clock_inputs) {
        if(!s->pm_bit9_profile||!s->tee_reset||!s->tee_ap_image) {error_report("usb-clock-inputs requires explicitdomain9profile andoriginalTEE/AP");exit(EXIT_FAILURE);}
        g_autofree gchar *bytes=NULL;gsize length=0;
        if(!g_file_get_contents(s->usb_clock_inputs,&bytes,&length,NULL)||length!=4*O62M_USB_CLOCK_COUNT_V1) {error_report("usb-clock-inputs exact9fullwordinitialinputs required");exit(EXIT_FAILURE);}
        uint32_t initial[O62M_USB_CLOCK_COUNT_V1];for(unsigned i=0;i<O62M_USB_CLOCK_COUNT_V1;i++)initial[i]=ldl_le_p(bytes+4*i);
        if(o62m_clock_controls_init(&s->usb_clocks,true,o62m_usb_clock_defs_v1,O62M_USB_CLOCK_COUNT_V1,initial,O62M_USB_CLOCK_COUNT_V1)!=O62M_CLOCK_CONTROLS_OK) {error_report("USB clockcatalog/input invalid");exit(EXIT_FAILURE);}
        error_report("MARCONI_USB_CLOCK_INPUTS exact9-ROMwords-11fields=true independent-fullword-inputs=true fixed-rate-only-software-declaration=true no-POR-lock-or-hostUSB-claim=true");
    }
    (void)o62m_clock_controls_init(&s->mipi_clocks,false,NULL,0,NULL,0);
    if(s->mipi_clock_inputs) {
        if(!s->pm_bit8_profile||!s->tee_reset||!s->tee_ap_image) {error_report("mipi-clock-inputs requires explicitdomain8profile andoriginalTEE/AP");exit(EXIT_FAILURE);}
        g_autofree gchar *bytes=NULL;gsize length=0;
        if(!g_file_get_contents(s->mipi_clock_inputs,&bytes,&length,NULL)||length!=4*O62M_MIPI_CLOCK_COUNT_V1) {error_report("mipi-clock-inputs exact6fullwordinitialinputs required");exit(EXIT_FAILURE);}
        uint32_t initial[O62M_MIPI_CLOCK_COUNT_V1];for(unsigned i=0;i<O62M_MIPI_CLOCK_COUNT_V1;i++)initial[i]=ldl_le_p(bytes+4*i);
        if(o62m_clock_controls_init(&s->mipi_clocks,true,o62m_mipi_clock_defs_v1,O62M_MIPI_CLOCK_COUNT_V1,initial,O62M_MIPI_CLOCK_COUNT_V1)!=O62M_CLOCK_CONTROLS_OK) {error_report("MIPI clockcatalog/input invalid");exit(EXIT_FAILURE);}
        error_report("MARCONI_MIPI_CLOCK_INPUTS exact6-ROMwords-11fields=true independent-fullword-inputs=true fixed-rate-only-software-declaration=true no-POR-lock-or-hostMIPI-claim=true");
    }
    (void)o62m_clock_controls_init(&s->pubap_clocks,false,NULL,0,NULL,0);
    if(s->pubap_clock_inputs) {
        if(!s->tee_reset||!s->tee_ap_image||!s->pm_domain4_profile) {error_report("pubap-clock-inputs require originalTEE/AP pluscanonicalPMdomain4 profile");exit(EXIT_FAILURE);}
        g_autofree gchar *bytes=NULL;gsize length=0;
        if(!g_file_get_contents(s->pubap_clock_inputs,&bytes,&length,NULL)||length!=4*O62M_PUBAP_CLOCK_COUNT_V1) {
            error_report("pubap-clock-inputs require exact40 independent fullu32LE initialwords");exit(EXIT_FAILURE);
        }
        if(!s->explore_seed) {error_report("pubap-clock-inputs requires canonicallegacybootclock initialseed");exit(EXIT_FAILURE);}
        char *seed_end;uint64_t legacy_seed=g_ascii_strtoull(s->explore_seed,&seed_end,0);
        if(!*s->explore_seed||*seed_end||legacy_seed>UINT32_MAX) {error_report("pubap legacybootclock initialseed invalid");exit(EXIT_FAILURE);}
        uint32_t initial[O62M_PUBAP_CLOCK_COUNT_V1];unsigned count=0,shared=0;
        for(unsigned i=0;i<O62M_PUBAP_CLOCK_COUNT_V1;i++) {
            uint32_t word=ldl_le_p(bytes+4*i);
            int old=o62m_boot_clock_index(o62m_pubap_clock_defs_v1[i].address);
            if(old>=0) {
                if(word!=(uint32_t)legacy_seed) {error_report("pubap initialinput conflicts withsolelegacybootclockowner");exit(EXIT_FAILURE);}
                shared++;continue;
            }
            s->pubap_owned_defs[count]=o62m_pubap_clock_defs_v1[i];initial[count++]=word;
        }
        if(count!=30||shared!=10) {error_report("pubap exact30new/10legacyowner geometry conflict");exit(EXIT_FAILURE);}
        if(o62m_clock_controls_init(&s->pubap_clocks,true,s->pubap_owned_defs,count,initial,count)!=O62M_CLOCK_CONTROLS_OK) {
            error_report("pubap-clock-inputs invalidoriginalfieldcatalog/independentinitialwords");exit(EXIT_FAILURE);
        }
        error_report("MARCONI_PUBAP_CLOCK_INPUTS exact40-originalROMwords=true independent-new-initial-inputs=true "
                     "new-owned-words=30 shared-legacybootclock-words=10 shared-initial-conflict-rejected=true "
                     "legacy-auto21-preserved-no-second-owner=true physical-reset-and-clock-output-unknown=true");
    }
    (void)o62m_clock_controls_init(&s->pubcp_clocks,false,NULL,0,NULL,0);
    if(s->pubcp_clock_inputs) {
        s->lps_recovery_control=0;s->lps_pubcp_auto88=0;
        memset(s->psram_init_word,0,sizeof(s->psram_init_word));
        memset(s->psram_init_written,0,sizeof(s->psram_init_written));
        memset(s->psram_init_history,0,sizeof(s->psram_init_history));s->psram_init_count=0;
        s->psram_controller_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_psram_controller_callback,s);
        o62m_psram_controller_reset_v1(&s->psram_controller);
        s->psram_cal_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_psram_cal_callback,s);
        o62m_psram_cal_reset_v1(&s->psram_cal);
        qemu_register_reset(marconi_psram_cal_reset,s);
        s->psram_init_exit.notify=marconi_psram_init_exit;qemu_add_exit_notifier(&s->psram_init_exit);
        if(!s->tee_reset||!s->tee_ap_image||!s->pm_domain5_profile) {error_report("pubcp-clock-inputs require originalTEE/AP pluscanonicalPMdomain5 profile");exit(EXIT_FAILURE);}
        g_autofree gchar *bytes=NULL;gsize length=0;
        if(!g_file_get_contents(s->pubcp_clock_inputs,&bytes,&length,NULL)||length!=4*O62M_PUBCP_CLOCK_COUNT_V1) {
            error_report("pubcp-clock-inputs require exact34 independent fullu32LE initialwords");exit(EXIT_FAILURE);
        }
        if(!s->explore_seed) {error_report("pubcp-clock-inputs requires canonicallegacybootclock initialseed");exit(EXIT_FAILURE);}
        char *seed_end;uint64_t legacy_seed=g_ascii_strtoull(s->explore_seed,&seed_end,0);
        if(!*s->explore_seed||*seed_end||legacy_seed>UINT32_MAX) {error_report("pubcp legacybootclock initialseed invalid");exit(EXIT_FAILURE);}
        uint32_t initial[O62M_PUBCP_CLOCK_COUNT_V1];unsigned count=0,shared=0;
        for(unsigned i=0;i<O62M_PUBCP_CLOCK_COUNT_V1;i++) {
            uint32_t word=ldl_le_p(bytes+4*i);
            int old=o62m_boot_clock_index(o62m_pubcp_clock_defs_v1[i].address);
            if(old>=0) {
                if(word!=(uint32_t)legacy_seed) {error_report("pubcp initialinput conflicts withsolelegacybootclockowner");exit(EXIT_FAILURE);}
                shared++;continue;
            }
            s->pubcp_owned_defs[count]=o62m_pubcp_clock_defs_v1[i];initial[count++]=word;
        }
        if(count!=26||shared!=8) {error_report("pubcp exact26new/8legacyowner geometry conflict");exit(EXIT_FAILURE);}
        if(o62m_clock_controls_init(&s->pubcp_clocks,true,s->pubcp_owned_defs,count,initial,count)!=O62M_CLOCK_CONTROLS_OK) {
            error_report("pubcp-clock-inputs invalidoriginalfieldcatalog/independentinitialwords");exit(EXIT_FAILURE);
        }
        error_report("MARCONI_PUBCP_CLOCK_INPUTS exact34-originalROMwords=true independent-new-initial-inputs=true "
                     "new-owned-words=26 shared-legacybootclock-words=8 shared-initial-conflict-rejected=true "
                     "legacy-auto21-preserved-no-second-owner=true physical-reset-and-clock-output-unknown=true");
    }
    if(s->pm_domain4_profile) {
        if(!s->tee_reset||!s->tee_ap_image||!s->ap_runtime_config) {error_report("pm-domain4-profile requires originalTEE/AP andcanonicaltimingconfigowner");exit(EXIT_FAILURE);}
        g_auto(GStrv) words=g_strsplit(s->pm_domain4_profile,":",-1);uint64_t inputs[3];
        if(g_strv_length(words)!=3) {error_report("pm-domain4-profile requires explicit-onmask:configNs:transitionNs");exit(EXIT_FAILURE);}
        for(unsigned i=0;i<3;i++) {
            char *end;if(!g_ascii_isdigit(words[i][0])) {error_report("pm-domain4 unsignedinputs required");exit(EXIT_FAILURE);}
            inputs[i]=g_ascii_strtoull(words[i],&end,0);if(*end||inputs[i]>INT64_MAX) {error_report("pm-domain4 inputrange invalid");exit(EXIT_FAILURE);}
        }
        if(inputs[0]>UINT32_MAX||!o62m_pm_domain4_init_v1(&s->pm_domain4,inputs[0],inputs[1],inputs[2],qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
            error_report("pm-domain4 supports onlymask0 or0x10 andpositiveexplicitlatencies");exit(EXIT_FAILURE);
        }
        s->pm_domain4_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_pm_domain4_callback,s);
        error_report("MARCONI_PM_DOMAIN4_PROFILE explicit-handover-on-mask=%u independent-guard-zero=true "
                     "initial-selected-state-not-produced-completion=true actual-three-timing-writes-and-f0-commit-required=true "
                     "config-never-powers-on=true subsequent-up-down-require-real-request-and-timer=true physical-rails-unknown=true",(unsigned)inputs[0]);
    }
    if(s->pm_domain5_profile) {
        if(!s->tee_reset||!s->tee_ap_image||!s->ap_runtime_config||!s->pm_domain4_profile) {error_report("pm-domain5-profile requires originalTEE/AP andcanonicaltimingconfigowner");exit(EXIT_FAILURE);}
        g_auto(GStrv) words=g_strsplit(s->pm_domain5_profile,":",-1);uint64_t inputs[3];
        if(g_strv_length(words)!=3) {error_report("pm-domain5-profile requires explicit-onmask:configNs:transitionNs");exit(EXIT_FAILURE);}
        for(unsigned i=0;i<3;i++) {
            char *end;if(!g_ascii_isdigit(words[i][0])) {error_report("pm-domain5 unsignedinputs required");exit(EXIT_FAILURE);}
            inputs[i]=g_ascii_strtoull(words[i],&end,0);if(*end||inputs[i]>INT64_MAX) {error_report("pm-domain5 inputrange invalid");exit(EXIT_FAILURE);}
        }
        if((inputs[0]!=0&&inputs[0]!=32)||!o62m_pm_domain4_init_v1(&s->pm_domain5,inputs[0]?16:0,inputs[1],inputs[2],qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
            error_report("pm-domain5 supports onlymask0 or0x20 andpositiveexplicitlatencies");exit(EXIT_FAILURE);
        }
        s->pm_domain5_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_pm_domain5_callback,s);
        qemu_register_reset(marconi_pm_domain5_cancel,s);
        s->pm_domain5_exit.notify=marconi_pm_domain5_exit;
        qemu_add_exit_notifier(&s->pm_domain5_exit);
        error_report("MARCONI_PM_DOMAIN5_PROFILE explicit-handover-on-mask=%u independent-guard-zero=true "
                     "initial-selected-state-not-produced-completion=true actual-three-timing-writes-and-f0-commit-required=true "
                     "config-never-powers-on=true subsequent-up-down-require-real-request-and-timer=true physical-rails-unknown=true",(unsigned)inputs[0]);
    }
    if(s->pm_domain7_profile) {
        if(!s->tee_reset||!s->tee_ap_image||!s->ap_runtime_config||!s->pm_domain4_profile) {error_report("pm-domain7-profile requires originalTEE/AP andcanonicaltimingconfigowner");exit(EXIT_FAILURE);}
        g_auto(GStrv) words=g_strsplit(s->pm_domain7_profile,":",-1);uint64_t inputs[3];
        if(g_strv_length(words)!=3) {error_report("pm-domain7-profile requires explicit-onmask:configNs:transitionNs");exit(EXIT_FAILURE);}
        for(unsigned i=0;i<3;i++) {
            char *end;if(!g_ascii_isdigit(words[i][0])) {error_report("pm-domain7 unsignedinputs required");exit(EXIT_FAILURE);}
            inputs[i]=g_ascii_strtoull(words[i],&end,0);if(*end||inputs[i]>INT64_MAX) {error_report("pm-domain7 inputrange invalid");exit(EXIT_FAILURE);}
        }
        if((inputs[0]!=0&&inputs[0]!=128)||!o62m_pm_domain4_init_v1(&s->pm_domain7,inputs[0]?16:0,inputs[1],inputs[2],qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
            error_report("pm-domain7 supports onlymask0 or0x80 andpositiveexplicitlatencies");exit(EXIT_FAILURE);
        }
        s->pm_domain7_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_pm_domain7_callback,s);
        error_report("MARCONI_PM_DOMAIN7_PROFILE explicit-handover-on-mask=%u independent-guard-zero=true "
                     "initial-selected-state-not-produced-completion=true actual-e4-e8-ec-timing-writes-and-f0-commit-required=true "
                     "config-never-powers-on=true subsequent-up-down-require-real-request-and-timer=true physical-rails-unknown=true",(unsigned)inputs[0]);
    }
    if(s->dpu_initial_b0) {
        if(!s->tee_reset||!s->tee_ap_image||strcmp(s->dpu_initial_b0,"0")) {
            error_report("dpu-initial-b0 onlyexplicit0 initialquery input supported; mode1/active not synthesised");exit(EXIT_FAILURE);
        }
        s->dpu_initial_b0_word=0;s->dpu_control164=0;s->dpu_control164_writes=0;
        memset(s->dpu_boot_config,0,sizeof(s->dpu_boot_config));
        s->dpu_irq_pending=0;s->dpu_irq_mask=0;
        error_report("MARCONI_DPU_INITIAL_B0_PROFILE explicit-unknown-reset-input-zero=true exact-read-only-query=true "
                     "writes-rendering-status-transitions-and-IRQ-strict=true physical-reset-not-claimed=true");
    }
    if(s->ap_runtime_config) {
        if(!s->tee_reset||!s->tee_ap_image||!s->explore_seed||!s->clock_controls_fixture||strcmp(s->ap_runtime_config,"zero-v1")) {
            error_report("ap-runtime-config requires originalTEE/AP existingcanonicalclockowner andexplicit zero-v1 inputpolicy");exit(EXIT_FAILURE);
        }
        memset(s->ap_runtime_words,0,sizeof(s->ap_runtime_words));
        error_report("MARCONI_AP_RUNTIME_CONFIG_PROFILE independent-new-config-words=67 explicit-zero-initial-inputs=true "
                     "existing132-clockwords-retain-original-canonical-owner=true "
                     "exact-original-BIC-PC-and-masks-only=true physical-reset-and-hardware-effects-unknown=true");
    }
    if(s->ap_board_config) {
        if(!s->tee_reset||!s->tee_ap_image||!s->explore_seed) {error_report("ap-board-config originalTEE/AP+explore required");exit(EXIT_FAILURE);}
        g_auto(GStrv) words=g_strsplit(s->ap_board_config,":",-1);
        unsigned count=g_strv_length(words);
        if(count<2||count>6) {error_report("ap-board-config requires initial009c:0120[:01e8[:USB-PLL-config[:idle-config[:boot-reason]]]]");exit(EXIT_FAILURE);}
        s->ap_board_usb_config_present=count>=3;
        s->ap_board_usb_clock_present=count>=4;
        s->ap_board_idle_config_present=count>=5;
        s->ap_board_boot_reason_present=count==6;
        for(unsigned i=0;i<count;i++) {
            char *end;if(!g_ascii_isdigit(words[i][0])) {error_report("ap-board-config unsigned input required");exit(EXIT_FAILURE);}
            uint64_t word=g_ascii_strtoull(words[i],&end,0);
            if(*end||word>UINT32_MAX) {error_report("ap-board-config fullword range invalid");exit(EXIT_FAILURE);}
            s->ap_board_words[i==5?9:i>=2?i+1:i]=(uint32_t)word;
        }
        error_report("MARCONI_AP_BOARD_CONFIG_PROFILE exact-board-inputs-plus-0114-and-idle-trace-written-backing=true independent-fullword-inputs=true "
                     "retained-config-only=true physical-reset-effects-aliases-status-and-ready-unknown=true");
    }
    if(s->pm_functional_profile) {
        if(!s->tee_reset||!s->tee_ap_image||!s->explore_seed||strcmp(s->pm_functional_profile,"set-clear-v1")) {
            error_report("pm-functional-profile requires originalTEE/AP explicitexplore andset-clear-v1 policy");exit(EXIT_FAILURE);
        }
        memset(&s->pm_functional,0,sizeof(s->pm_functional));
        error_report("MARCONI_PM_FUNCTIONAL_PROFILE explicit-synthetic-zero-config-and-request-inputs=true "
                     "plus300SET-plus600CLEAR-virtual-assumption=true no-status-wake-or-IRQ-implementation=true "
                     "physical-register-semantics-reset-and-power-effects-unknown=true");
    }
    if(s->pm_counter_fixture) {
        if(!s->tee_reset||!s->tee_ap_image||!s->explore_seed) {
            error_report("pm-counter-fixture requires originalTEE/AP andexplicitexplore");exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words=g_strsplit(s->pm_counter_fixture,":",-1);
        uint64_t inputs[3];
        if(g_strv_length(words)!=3) {error_report("pm-counter-fixture requires ticks:fraction-numerator:reference-virtual-ns");exit(EXIT_FAILURE);}
        for(unsigned i=0;i<3;i++) {
            char *end;
            if(!g_ascii_isdigit(words[i][0])) {error_report("pm-counter-fixture unsignedinputs required");exit(EXIT_FAILURE);}
            inputs[i]=g_ascii_strtoull(words[i],&end,0);
            if(*end||inputs[i]>INT64_MAX) {error_report("pm-counter-fixture inputrange exceeded");exit(EXIT_FAILURE);}
        }
        O62MPM32KResult result=o62m_pm32k_init(&s->pm_counter,inputs[0],inputs[1],inputs[2]);
        if(result!=O62M_PM32K_OK) {error_report("pm-counter-fixture invalid: %s",o62m_pm32k_reason(result));exit(EXIT_FAILURE);}
        error_report("MARCONI_PM_COUNTER_FIXTURE words=%s hz=32768 explicit-initial-input=true "
                     "independent4c0302c4-only=true always-running-virtual-policy=true "
                     "physical-oscillator-gate-reset-wrap-unknown=true",s->pm_counter_fixture);
    }
    if(s->rtc32k_reference_profile) {
        if((strcmp(s->rtc32k_reference_profile,"available-v1")&&strcmp(s->rtc32k_reference_profile,"unavailable-v1"))||
           !s->rtc_seconds_fixture||!s->tee_reset||!s->tee_ap_image||!s->adc_fixture) {
            error_report("rtc32k-reference requires available-v1 or unavailable-v1 and original AP/SPMI/RTC profile");exit(EXIT_FAILURE);
        }
        s->rtc32k_reference_available=!strcmp(s->rtc32k_reference_profile,"available-v1");
        memset(&s->rtc32k_reference,0,sizeof(s->rtc32k_reference));s->rtc32k_reads=0;
        memset(s->rtc_calibration_config,0,sizeof(s->rtc_calibration_config));
        s->rtc_calibration_config_written=1;s->rtc_calibration_config_commits=0;
        memset(&s->rtc_calibration,0,sizeof(s->rtc_calibration));
        s->rtc_calibration_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_rtc_calibration_callback,s);
        s->rtc32k_reference_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_rtc32k_reference_callback,s);
        qemu_register_reset(marconi_rtc32k_reference_reset,s);
        s->rtc32k_reference_exit.notify=marconi_rtc32k_reference_exit;qemu_add_exit_notifier(&s->rtc32k_reference_exit);
    }
    if(s->rtc_seconds_fixture) {
        if(!s->tee_reset||!s->tee_ap_image||!s->explore_seed||!s->adc_fixture) {
            error_report("rtc-seconds-fixture requires originalTEE/AP, explicitexplore andexistingSPMI/ADCselectedslave");exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words=g_strsplit(s->rtc_seconds_fixture,":",-1);
        uint64_t inputs[3];
        if(g_strv_length(words)!=3) {error_report("rtc-seconds-fixture requires seconds:phase-ns:reference-virtual-ns");exit(EXIT_FAILURE);}
        for(unsigned i=0;i<3;i++) {
            char *end;
            if(!g_ascii_isdigit(words[i][0])) {error_report("rtc-seconds-fixture unsignedinputs required");exit(EXIT_FAILURE);}
            inputs[i]=g_ascii_strtoull(words[i],&end,0);
            if(*end||inputs[i]>INT64_MAX) {error_report("rtc-seconds-fixture inputrange exceeded");exit(EXIT_FAILURE);}
        }
        O62MRTCResult result=o62m_rtc_init(&s->rtc_seconds,inputs[0],inputs[1],inputs[2]);
        if(result!=O62M_RTC_OK) {error_report("rtc-seconds-fixture invalid: %s",o62m_rtc_reason(result));exit(EXIT_FAILURE);}
        s->pmic_poweron_seconds=s->rtc_seconds.seconds;
        error_report("MARCONI_RTC_SECONDS_FIXTURE words=%s explicit-external-epoch=true "
                     "always-running-virtual-clock-policy=true "
                     "physical-reset-oscillator-latch-calibration-unknown=true "
                     "setter-alarm-IRQ-unimplemented=true",s->rtc_seconds_fixture);
    }
    if(s->pmu_init_fixture) {
        if((!s->tee_reset&&!s->ota_reset)||!s->explore_seed) {
            error_report("pmu-init-fixture requires originalTEE orOTA andexplicit explore-seed");exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words=g_strsplit(s->pmu_init_fixture,":",-1);
        uint64_t inputs[O62M_PMU_INITIAL_WORDS];
        if(g_strv_length(words)!=O62M_PMU_INITIAL_WORDS) {
            error_report("pmu-init-fixture requires N:CTRL:CNTEN=0:OVS:CCNTR:8eventcounterwords");exit(EXIT_FAILURE);
        }
        for(unsigned i=0;i<O62M_PMU_INITIAL_WORDS;i++) {
            char *end;
            if(!g_ascii_isdigit(words[i][0])) {error_report("pmu-init-fixture unsigned inputs required");exit(EXIT_FAILURE);}
            inputs[i]=g_ascii_strtoull(words[i],&end,0);
            if(*end||inputs[i]>UINT32_MAX) {error_report("pmu-init-fixture inputs mustfitfullu32");exit(EXIT_FAILURE);}
        }
        O62MPMUResult result=o62m_pmu_init(&s->pmu_init,inputs,O62M_PMU_INITIAL_WORDS);
        if(result!=O62M_PMU_OK) {error_report("pmu-init-fixture invalid: %s",o62m_pmu_reason(result));exit(EXIT_FAILURE);}
        error_report("MARCONI_PMU_INIT_FIXTURE words=%s explicit-initial-state=true "
                     "unbanked=true live-counting-DWT-DEMCR-IRQ-unimplemented=true",s->pmu_init_fixture);
    }
    if (s->otp_read_fixture) {
        if (!s->tee_reset || !s->explore_seed) {
            error_report("otp-read-fixture requires original TEE reset and explicit explore-seed");
            exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words = g_strsplit(s->otp_read_fixture, ":", -1);
        uint32_t seeds[O62M_OTP_INPUT_WORDS+1];
        if (g_strv_length(words)!=O62M_OTP_INPUT_WORDS+1) {
            error_report("otp-read-fixture requires initial-key and128 explicit independent fullu32 data words");
            exit(EXIT_FAILURE);
        }
        for (unsigned i=0;i<O62M_OTP_INPUT_WORDS+1;i++) {
            char *end;
            if (!g_ascii_isdigit(words[i][0])) {
                error_report("otp-read-fixture words require explicit unsigned values"); exit(EXIT_FAILURE);
            }
            uint64_t word = g_ascii_strtoull(words[i], &end, 0);
            if (*end || word>UINT32_MAX) {
                error_report("otp-read-fixture words must fit fullu32"); exit(EXIT_FAILURE);
            }
            seeds[i]=(uint32_t)word;
        }
        assert(o62m_otp_read_init(&s->otp_read,seeds[0],&seeds[1],O62M_OTP_INPUT_WORDS));
        s->otp_program_exit.notify=marconi_otp_program_exit;qemu_add_exit_notifier(&s->otp_program_exit);
        error_report("MARCONI_OTP_READ_FIXTURE explicit-independent-words=128 "
            "original-device-data-missing=true virtual-synchronous-bitset-programming=true");
    }
    if(s->ap_efuse_read_view) {
        if(strcmp(s->ap_efuse_read_view,"shared-board-128-v1")||!s->tee_reset||!s->tee_ap_image||!s->otp_read_fixture||!s->otp_read.initialized||
           !o62m_ap_efuse_read_view_init_v1(&s->ap_efuse_view)) {
            error_report("ap-efuse-read-view requires shared-board-128-v1 originalTEE/AP andexistinginitializedexplicit128wordboardOTPowner");exit(EXIT_FAILURE);
        }
        error_report("MARCONI_AP_EFUSE_READ_VIEW_PROFILE optin-shared-board-128-v1=true APkey30-independent-initial0-locked=true same-old-explicit-word-owner=true TEEkey64-unchanged=true exact-original-PC-consumers-only=true no-programming-status-POR-or-physical-secure-NS-alias-claim=true");
    }
    (void)o62m_clock_controls_init(&s->board_controls, false, NULL, 0, NULL, 0);
    if (s->board_controls_fixture) {
        if ((!s->tee_reset&&!s->ota_reset) || !s->explore_seed) {
            error_report("board-controls-fixture requires original TEE orOTA reset and explicit explore-seed");
            exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words = g_strsplit(s->board_controls_fixture, ":", -1);
        uint32_t seeds[O62M_BOARD_CONTROL_COUNT];
        if (g_strv_length(words) != O62M_BOARD_CONTROL_COUNT) {
            error_report("board-controls-fixture requires exactly %u independent full u32 words",
                         O62M_BOARD_CONTROL_COUNT);
            exit(EXIT_FAILURE);
        }
        for (unsigned i = 0; i < O62M_BOARD_CONTROL_COUNT; ++i) {
            char *end;
            if (!g_ascii_isdigit(words[i][0])) {
                error_report("board-controls-fixture words require explicit unsigned values");
                exit(EXIT_FAILURE);
            }
            uint64_t word = g_ascii_strtoull(words[i], &end, 0);
            if (*end || word > UINT32_MAX) {
                error_report("board-controls-fixture words must fit full u32");
                exit(EXIT_FAILURE);
            }
            seeds[i] = word;
        }
        O62MClockControlsResult result = o62m_clock_controls_init(
            &s->board_controls, true, o62m_board_control_defs,
            O62M_BOARD_CONTROL_COUNT, seeds, O62M_BOARD_CONTROL_COUNT);
        assert(result == O62M_CLOCK_CONTROLS_OK);
        error_report("MARCONI_BOARD_CONTROL_FIXTURE words=%s explicit-independent-initial-words=true "
            "known-RMW-fields-only=true hardware-meaning-and-effects-unverified=true",
            s->board_controls_fixture);
    }
    if (s->thermal_observed_writes && (!s->tee_reset || !s->explore_seed)) {
        error_report("thermal-observed-writes requires original TEE reset and explicit explore-seed");
        exit(EXIT_FAILURE);
    }
    if(s->dvfs_vote_config) {
        if(strcmp(s->dvfs_vote_config,"independent-d0-v1")||!s->tee_reset||!s->tee_ap_image||!s->explore_seed||!s->aon120_config_inputs) {
            error_report("dvfs-vote-config requires independent-d0-v1 originalTEE/AP exploreseed andDVFSconfigowner");exit(EXIT_FAILURE);
        }
        error_report("MARCONI_DVFS_VOTE_CONFIG_PROFILE independent-d0-v1=true initially-unwritten=true actual-original-write-readback-only=true no-DFS-state-clock-transition-completion-or-IRQ=true");
    }
    if(s->ap_pll_rate_view) {
        if(strcmp(s->ap_pll_rate_view,"shared-four-v1")||!s->tee_reset||!s->tee_ap_image||!s->explore_seed||!s->pll_rate_fixture) {
            error_report("ap-pll-rate-view requires shared-four-v1 originalTEE/AP exploreseed andexistingexplicitPLL12wordowner");exit(EXIT_FAILURE);
        }
        error_report("MARCONI_AP_PLL_RATE_PROFILE shared-four-v1=true exact12originalAP-word-to-TEEcanonical-read-view=true original-guest-computes-frequency=true no-newseed-physicalalias-lock-ready-reset-or-writes=true");
    }
    if(s->dvfs_main_clock_initial) {
        char *end;
        if(!s->tee_reset||!s->tee_ap_image||!s->explore_seed||!s->aon120_config_inputs||!g_ascii_isdigit(s->dvfs_main_clock_initial[0])) {
            error_report("dvfs-main-clock-initial requires originalTEE/AP exploreseed DVFSconfigowner and unsignedfullword");exit(EXIT_FAILURE);
        }
        uint64_t initial=g_ascii_strtoull(s->dvfs_main_clock_initial,&end,0);
        if(*end||initial>UINT32_MAX) {error_report("dvfs-main-clock-initial mustfit fullu32");exit(EXIT_FAILURE);}
        s->dvfs_main_clock_word=(uint32_t)initial;
        error_report("MARCONI_DVFS_MAIN_CLOCK_PROFILE exactdc-independent-initial-read-view=true original-physical-reset-unknown=true no-DFS-state-completion-ready-or-frequency-update=true");
    }
    if(s->ap_aon_iosleep_config) {
        if(strcmp(s->ap_aon_iosleep_config,"zero-eight-v1")||!s->tee_reset||
           !s->tee_ap_image||!s->explore_seed) {
            error_report("ap-aon-iosleep-config requires zero-eight-v1 andoriginalTEE/AP exploreseed");exit(EXIT_FAILURE);
        }
        memset(s->ap_aon_iosleep_words,0,sizeof(s->ap_aon_iosleep_words));
        memset(s->ap_aon_iosleep_capture_written,0,sizeof(s->ap_aon_iosleep_capture_written));
        error_report("MARCONI_AP_AON_IOSLEEP_PROFILE eight-explicit-zero-RW-inputs=true "
            "three-extra-write-only-captures-initially-invalid=true "
            "actual-two-caller-R1-zero-or-one-scope=true R2-ge32-idempotent-preserved=true "
            "physical-reset-alias-or-wakeup-effects-not-claimed=true no-ready-or-IRQ=true");
    }
    if(s->thermal_mode0_profile&&(!s->ap_thermal_config_view||!s->tee_reset||
       !s->tee_ap_image||!s->explore_seed||!s->thermal_observed_writes)) {
        error_report("thermal-mode0-profile requires originalTEE/AP andshared-eight-config-view existingcanonicalowner");exit(EXIT_FAILURE);
    }
    if(s->ap_thermal_config_view) {
        if(strcmp(s->ap_thermal_config_view,"shared-eight-v1")||!s->tee_reset||!s->tee_ap_image||!s->explore_seed||!s->thermal_observed_writes) {
            error_report("ap-thermal-config-view requires shared-eight-v1 originalTEE/AP exploreseed and existingTEEthermalcanonicalowner");exit(EXIT_FAILURE);
        }
        error_report("MARCONI_AP_THERMAL_CONFIG_PROFILE shared-eight-v1=true exact-original-AP-PC-write-view=true same-TEE-canonical-eight-words=true no-second-storage-initial-seed-read-measurement-ready-or-IRQ=true software-counterpart-not-physical-alias-proof=true");
    }
    if (s->context_observed_writes && (!s->tee_reset || !s->explore_seed)) {
        error_report("context-observed-writes requires original TEE reset and explicit explore-seed");
        exit(EXIT_FAILURE);
    }
    if (s->dtgu_lut0_observed_write && (!s->tee_reset || !s->explore_seed)) {
        error_report("dtgu-lut0-observed-write requires original TEE reset and explicit explore-seed");
        exit(EXIT_FAILURE);
    }
    memset(&s->ap_domain0,0,sizeof(s->ap_domain0));
    if (s->ap_domain_status_fixture) {
        s->ap_domain0_guard=0;
        s->pm_domain1_available=false;s->pm_domain1_on=false;s->pm_domain1_timing_written=0;
        if ((!s->tee_reset&&!s->ota_reset) || !s->explore_seed) {
            error_report("ap-domain-status-fixture requires original TEE orOTA reset and explicit explore-seed");
            exit(EXIT_FAILURE);
        }
        char *end;
        if (!g_ascii_isdigit(s->ap_domain_status_fixture[0])) {
            error_report("ap-domain-status-fixture requires one explicit full u32 word");
            exit(EXIT_FAILURE);
        }
        uint64_t word = g_ascii_strtoull(s->ap_domain_status_fixture, &end, 0);
        if (*end || word > UINT32_MAX || !(word & 2)) {
            error_report("ap-domain-status-fixture requires u32 and a stable initial bit1; transient policy unsupported");
            exit(EXIT_FAILURE);
        }
        s->ap_domain0.initial=(uint32_t)word;s->ap_domain0.status=(uint32_t)word;
        s->ap_domain0_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_ap_domain0_callback,s);
        qemu_register_reset(marconi_ap_domain0_reset,s);
        s->ap_domain0_exit.notify=marconi_ap_domain0_exit;qemu_add_exit_notifier(&s->ap_domain0_exit);
        marconi_public_report("MARCONI_AP_DOMAIN0_PROFILE explicit-stable-handover=true guard-input-zero=true "
            "already-on-request-service-ns=1000 no-electrical-powerup-claim=true");
    }
    (void)o62m_clock_controls_init(&s->clock_controls, false, NULL, 0, NULL, 0);
    if (s->clock_controls_fixture) {
        if ((!s->tee_reset&&!s->ota_reset) || !s->explore_seed) {
            error_report("clock-controls-fixture requires original TEE orOTA reset and explicit explore-seed");
            exit(EXIT_FAILURE);
        }
        if (s->aon144_read_fixture || s->aon_prediv_query_fixture || s->clock48_fixture) {
            error_report("clock-controls-fixture is mutually exclusive with old AON/clock48 projections; shared backing required");
            exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words = g_strsplit(s->clock_controls_fixture, ":", -1);
        uint32_t seeds[O62M_AON_CLOCK_CONTROL_COUNT];
        if (g_strv_length(words) != O62M_AON_CLOCK_CONTROL_COUNT) {
            error_report("clock-controls-fixture requires exactly %u independent full u32 words",
                         O62M_AON_CLOCK_CONTROL_COUNT);
            exit(EXIT_FAILURE);
        }
        for (unsigned i = 0; i < O62M_AON_CLOCK_CONTROL_COUNT; ++i) {
            char *end;
            if (!g_ascii_isdigit(words[i][0])) {
                error_report("clock-controls-fixture words require explicit unsigned integers");
                exit(EXIT_FAILURE);
            }
            uint64_t value = g_ascii_strtoull(words[i], &end, 0);
            if (*end || value > UINT32_MAX) {
                error_report("clock-controls-fixture words must fit full u32 range");
                exit(EXIT_FAILURE);
            }
            seeds[i] = (uint32_t)value;
        }
        O62MClockControlsResult result = o62m_clock_controls_init(
            &s->clock_controls, true, o62m_aon_clock_control_defs,
            O62M_AON_CLOCK_CONTROL_COUNT, seeds, O62M_AON_CLOCK_CONTROL_COUNT);
        if (result != O62M_CLOCK_CONTROLS_OK) {
            error_report("Invalid clock control definitions/inputs: %s", o62m_clock_controls_reason(result));
            exit(EXIT_FAILURE);
        }
        error_report("MARCONI_CLOCK_CONTROLS_FIXTURE count=%u full-independent-words=true "
            "repeat-reads-and-masked-field-writes=true no-PLL-ready-clock-or-reset-proof=true",
            O62M_AON_CLOCK_CONTROL_COUNT);
    }
    if (s->aon144_read_fixture && s->aon_prediv_query_fixture) {
        error_report("aon144-read-fixture and aon-prediv-query-fixture are mutually exclusive");
        exit(EXIT_FAILURE);
    }
    (void)o62m_aon_prediv_query_init(&s->aon_prediv_query, false, NULL, 0);
    if (s->aon_prediv_query_fixture) {
        if (!s->tee_reset || !s->explore_seed) {
            error_report("aon-prediv-query-fixture requires original TEE reset and explicit explore-seed");
            exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words = g_strsplit(s->aon_prediv_query_fixture, ":", -1);
        uint32_t seeds[26];
        if (g_strv_length(words) != 26) {
            error_report("aon-prediv-query-fixture requires exactly 26 independent u32 words in registration order");
            exit(EXIT_FAILURE);
        }
        for (unsigned i = 0; i < 26; ++i) {
            char *end;
            if (!g_ascii_isdigit(words[i][0])) {
                error_report("aon-prediv-query-fixture words must be explicit unsigned integers");
                exit(EXIT_FAILURE);
            }
            uint64_t value = g_ascii_strtoull(words[i], &end, 0);
            if (*end || value > UINT32_MAX) {
                error_report("aon-prediv-query-fixture words must fit the full u32 range");
                exit(EXIT_FAILURE);
            }
            seeds[i] = (uint32_t)value;
        }
        O62MAonPredivQueryResult result = o62m_aon_prediv_query_init(
            &s->aon_prediv_query, true, seeds, 26);
        if (result != O62M_AON_PREDIV_QUERY_OK) {
            error_report("Invalid AON prediv query projection: %s", o62m_aon_prediv_query_reason(result));
            exit(EXIT_FAILURE);
        }
        error_report("MARCONI_AON_PREDIV_FIXTURE words=%s "
            "26-independent-full-u32-in-original-registration-order=true "
            "read-only=true all-writes-and-repeat-or-wrong-order-reads-rejected=true "
            "no-clock-effects-PLL-ready-or-reset-proof=true", s->aon_prediv_query_fixture);
    }
    (void)o62m_aon144_read_init(&s->aon144_read_projection, false, false, 0);
    if (s->aon144_read_fixture) {
        if (!s->tee_reset || !s->explore_seed) {
            error_report("aon144-read-fixture requires original TEE reset and explicit explore-seed");
            exit(EXIT_FAILURE);
        }
        char *end;
        if (!g_ascii_isdigit(s->aon144_read_fixture[0])) {
            error_report("aon144-read-fixture requires one explicit unsigned u32 word");
            exit(EXIT_FAILURE);
        }
        uint64_t initial = g_ascii_strtoull(s->aon144_read_fixture, &end, 0);
        if (*end || initial > UINT32_MAX) {
            error_report("aon144-read-fixture requires one full-range u32 word");
            exit(EXIT_FAILURE);
        }
        O62MAon144ReadResult result = o62m_aon144_read_init(
            &s->aon144_read_projection, true, true, initial);
        if (result != O62M_AON144_READ_OK) {
            error_report("Invalid AON144 read projection: %s", o62m_aon144_read_reason(result));
            exit(EXIT_FAILURE);
        }
        error_report("MARCONI_AON144_READ_FIXTURE word=%08x "
            "one-query-only=true unknown-silicon-initial-value=true "
            "no-PLL-lock-clock-ready-or-gate-side-effect-implemented=true",
            s->aon144_read_projection.word);
    }
    if(s->timer0_one_shot_experiment&&!s->timer0_runtime_fixture) {
        error_report("timer0-one-shot-experiment requires explicit timer0-runtime-fixture");exit(EXIT_FAILURE);
    }
    if(s->timer0_runtime_fixture&&(!s->tee_reset||!s->explore_seed||!s->clock_controls_fixture||s->timer0_stop_fixture)) {
        error_report("timer0-runtime-fixture requires original TEE, explicit explore-seed, fullclock provider, and no bounded stop profile");exit(EXIT_FAILURE);
    }
    (void)o62m_timer0_stop_init(&s->timer0_stop_projection, false, false, 0);
    if (s->timer0_stop_fixture) {
        if (!s->tee_reset || !s->explore_seed) {
            error_report("timer0-stop-fixture requires original TEE reset and explicit explore-seed");
            exit(EXIT_FAILURE);
        }
        char *end;
        if (!g_ascii_isdigit(s->timer0_stop_fixture[0])) {
            error_report("timer0-stop-fixture requires one explicit unsigned u32 word");
            exit(EXIT_FAILURE);
        }
        uint64_t initial = g_ascii_strtoull(s->timer0_stop_fixture, &end, 0);
        if (*end || initial > UINT32_MAX) {
            error_report("timer0-stop-fixture requires one full-range u32 word");
            exit(EXIT_FAILURE);
        }
        O62MTimer0StopResult result = o62m_timer0_stop_init(
            &s->timer0_stop_projection, true, true, initial);
        if (result != O62M_TIMER0_STOP_OK) {
            error_report("Invalid timer0 stop projection: %s", o62m_timer0_stop_reason(result));
            exit(EXIT_FAILURE);
        }
        error_report("MARCONI_TIMER0_STOP_FIXTURE initial=%08x "
            "independent-explicit-control-word=true one-read-BIC1-write-only=true "
            "count-busy-IRQ-clocks-reset-and-other-addresses-unsupported=true",
            s->timer0_stop_projection.initial);
    }
    (void)o62m_clock48_projection_init(&s->clock48_projection, false, NULL, 0);
    if (s->clock48_fixture) {
        if (!s->tee_reset || !s->explore_seed) {
            error_report("clock48-fixture requires original TEE reset and explicit explore-seed");
            exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words = g_strsplit(s->clock48_fixture, ":", -1);
        uint32_t seeds[5];
        if (g_strv_length(words) != 5) {
            error_report("clock48-fixture requires exactly five independent u32 words");
            exit(EXIT_FAILURE);
        }
        for (unsigned i = 0; i < 5; ++i) {
            char *end;
            if (!g_ascii_isdigit(words[i][0])) {
                error_report("clock48-fixture words must be explicit unsigned integers");
                exit(EXIT_FAILURE);
            }
            uint64_t value = g_ascii_strtoull(words[i], &end, 0);
            if (*end || value > UINT32_MAX) {
                error_report("clock48-fixture words must fit the full u32 range");
                exit(EXIT_FAILURE);
            }
            seeds[i] = (uint32_t)value;
        }
        O62MClock48Result result = o62m_clock48_projection_init(
            &s->clock48_projection, true, seeds, 5);
        if (result != O62M_CLOCK48_OK) {
            error_report("Invalid clock48 projection: %s", o62m_clock48_reason(result));
            exit(EXIT_FAILURE);
        }
        error_report("MARCONI_CLOCK48_FIXTURE words=%s "
            "five-independent-explicit-u32-inputs=true logical-control-only=true "
            "no-physical-alias-clock-status-ready-or-reset-proof=true",
            s->clock48_fixture);
    }
    if (s->dfs_config_fixture) {
        if (!s->tee_reset || !s->explore_seed) {
            error_report("dfs-config-fixture requires original TEE reset and explicit explore-seed");
            exit(EXIT_FAILURE);
        }
        g_auto(GStrv) words = g_strsplit(s->dfs_config_fixture, ":", -1);
        struct dfs_config_inputs inputs = {0};
        if (g_strv_length(words) != 6) {
            error_report("dfs-config-fixture requires exactly six independent u32 words");
            exit(EXIT_FAILURE);
        }
        for (unsigned i = 0; i < 6; ++i) {
            char *end;
            if (!g_ascii_isdigit(words[i][0])) {
                error_report("dfs-config-fixture words must be explicit unsigned integers");
                exit(EXIT_FAILURE);
            }
            uint64_t value = g_ascii_strtoull(words[i], &end, 0);
            if (*end || value > UINT32_MAX) {
                error_report("dfs-config-fixture words must fit the full u32 range");
                exit(EXIT_FAILURE);
            }
            inputs.word[i / 3][i % 3].present = true;
            inputs.word[i / 3][i % 3].value = value;
        }
        enum dfs_config_result result = dfs_config_start(&s->dfs_config, true, &inputs);
        if (result != DFS_CONFIG_OK) {
            error_report("Invalid DFS config experiment: %s", dfs_config_result_name(result));
            exit(EXIT_FAILURE);
        }
        error_report("MARCONI_DFS_CONFIG_FIXTURE words=%s "
            "six-independent-explicit-u32-inputs=true logical-config-projection=true "
            "enable-word=observation-only status-and-ready=unsupported "
            "fresh-VM-boot-only=true hardware-reset-retention-unverified=true",
            s->dfs_config_fixture);
        if(s->ap_runtime_config&&s->pm_domain4_profile) {
            s->pubap_quiesce_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_pubap_quiesce_callback,s);
            qemu_register_reset(marconi_pubap_quiesce_reset,s);
            s->pubap_quiesce_exit.notify=marconi_pubap_quiesce_exit;
            qemu_add_exit_notifier(&s->pubap_quiesce_exit);
        }
    }





    if(s->i2c_nopeer_profile) {
        g_auto(GStrv) words=g_strsplit(s->i2c_nopeer_profile,":",-1);
        if(g_strv_length(words)!=2||strcmp(words[0],"no-peer-v1")||
           !s->ap_i2c_empty_config||!s->normal_emmc_profile||!g_ascii_isdigit(words[1][0])) {
            error_report("i2c-nopeer-profile requires no-peer-v1:Hz andcontrollednormalmedia+emptyI2Cprofile");exit(EXIT_FAILURE);
        }
        char *end;uint64_t hz=g_ascii_strtoull(words[1],&end,0);
        if(*end||!hz||hz>UINT32_MAX) {error_report("i2c-nopeer-profile clockmustpositiveu32");exit(EXIT_FAILURE);}
        s->i2c_nopeer_hz=(uint32_t)hz;
        error_report("MARCONI_I2C_NOPEER_PROFILE defaultoff-optin=true explicit-functional-clock-and-address-phase-timing=true "
            "real16TXFIFO-deadline-QEMUbus-START-required=true actualNACK-before-abort-only=true no-ACK-RXbyte-success-IRQ-or-physical-device-claim=true");
    }

    if(s->ap_i2c_empty_config) {
        if(strcmp(s->ap_i2c_empty_config,"fresh-five-v1")||!s->tee_reset||!s->tee_ap_image||
           !s->explore_seed||!s->ap_i2c_soc_commands) {
            error_report("ap-i2c-empty-config requires fresh-five-v1 originalTEE/AP exploreseed andtwoSOCcommands");exit(EXIT_FAILURE);
        }
        memset(s->ap_i2c_empty,0,sizeof(s->ap_i2c_empty));
        for(unsigned i=0;i<5;i++)if(!o62m_i2c_empty_config_valid_v3(&s->ap_i2c_empty[i])) {
            error_report("invalid freshI2C virtualinitialstate");exit(EXIT_FAILURE);
        }
        error_report("MARCONI_AP_I2C_EMPTY_PROFILE defaultoff-optin=true original-five-ROM-base-id-ops-catalog=true "
            "explicit-zero-config-empty16FIFO-no-job-initialstate=true functional-clock-declared-available-not-physical-calibration=true "
            "STATUS-derived-from-real-empty-state=true nonzeroMASK-DATA-INTR-peer-IRQ-strict=true RAW-emptyFIFOlevel-comparator-explicit-policy=true no-physical-POR-or-transfer-completion=true");
    }

    if(s->ap_i2c_soc_commands) {
        if(strcmp(s->ap_i2c_soc_commands,"two-port-v1")||!s->tee_reset||!s->tee_ap_image||!s->explore_seed) {
            error_report("ap-i2c-soc-commands requires two-port-v1 originalTEE/AP exploreseed");exit(EXIT_FAILURE);
        }
        error_report("MARCONI_AP_I2C_SOC_COMMAND_PROFILE defaultoff-optin=true exact-two-original-WO-callers=true "
                     "onehot-bit9-through31-only=true no-initialreadback-SET-CLEAR-reset-ready-transfer-or-IRQ-claim=true");
    }

    if(s->wdt4012_profile) {
        g_auto(GStrv) words=g_strsplit(s->wdt4012_profile,":",-1);
        uint32_t input[3]={0};
        if(g_strv_length(words)!=3||!s->tee_reset||!s->tee_ap_image||!s->explore_seed||!s->normal_emmc_profile) {
            error_report("wdt4012-profile requires Hz:initialCR:initialTORR originalTEE/AP exploreseed and controllednormalmedia");exit(EXIT_FAILURE);
        }
        for(unsigned i=0;i<3;i++) {
            char *end;
            if(!g_ascii_isdigit(words[i][0])) {error_report("wdt4012-profile unsignedu32required");exit(EXIT_FAILURE);}
            uint64_t word=g_ascii_strtoull(words[i],&end,0);
            if(*end||word>UINT32_MAX) {error_report("wdt4012-profile inputs mustfitu32");exit(EXIT_FAILURE);}
            input[i]=(uint32_t)word;
        }
        int result=o62m_wdt4012_init_v1(&s->wdt4012,true,input[0],input[1],input[2],
            o62m_wdt4012_original_top_cycles_v1,15,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        if(result!=O62M_WDT4012_OK_V1) {error_report("wdt4012-profile invalid explicitinitialstate reason=%d",result);exit(EXIT_FAILURE);}
        s->wdt4012_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_wdt4012_callback,s);
        error_report("MARCONI_WDT4012_PROFILE defaultoff-optin=true explicit-alwaysavailable-virtual-clock-not-physical-gate=true "
                     "original15TOP=true disabled-preload-and-CCVR-explicit-policy=true real-QEMU-virtual-deadline-timer=true "
                     "first-expiry-strict-no-IRQ-reset-ready=true no-physical-POR-or-clock-calibration-claim=true");
    }

    if(s->ap_watchdog_init) {
        g_auto(GStrv) words=g_strsplit(s->ap_watchdog_init,":",-1);
        unsigned count=g_strv_length(words);
        if((count!=1&&count!=2)||strcmp(words[0],"aon44c-v1")||!s->tee_reset||!s->tee_ap_image||!s->explore_seed) {
            error_report("ap-watchdog-init requires aon44c-v1[:initialSOC40050000] originalTEE/AP exploreseed");exit(EXIT_FAILURE);
        }
        if(count==2) {
            char *end;
            if(!g_ascii_isdigit(words[1][0])) {error_report("ap-watchdog-init SOCword unsignedu32required");exit(EXIT_FAILURE);}
            uint64_t word=g_ascii_strtoull(words[1],&end,0);
            if(*end||word>UINT32_MAX) {error_report("ap-watchdog-init SOCword mustfitu32");exit(EXIT_FAILURE);}
            s->ap_watchdog_soc_word=(uint32_t)word;s->ap_watchdog_soc_present=true;
        }
        error_report("MARCONI_AP_WATCHDOG_AON_PROFILE defaultoff-optin=true exact-original44c-W84-only=true "
                     "optional-independent-SOCword-fourstep-mask10000=true no-initialread-or-watchdogcounter-reset-IRQ-ready-model=true");
    }

    if(s->ap_spi_config) {
        if(strcmp(s->ap_spi_config,"empty-401b-v1")||!s->tee_reset||!s->tee_ap_image||!s->explore_seed) {
            error_report("ap-spi-config requires empty-401b-v1 originalTEE/AP exploreseed");exit(EXIT_FAILURE);
        }
        error_report("MARCONI_AP_SPI_EMPTY_PROFILE defaultoff-optin=true exact-original-init-config-ports=true "
                     "declared-zero-config-and-no-error-initial=true no-SER-IMR-enable-or-DR-transfer=true "
                     "ICR-only-in-fresh-no-error-state=true physical-POR-IRQ-ACK-clock-peer-unmodeled=true");
    }
    if(s->ap_gpio_config) {
        if(strcmp(s->ap_gpio_config,"original-five-config-v4")||!s->tee_reset||!s->tee_ap_image||!s->explore_seed) {
            error_report("ap-gpio-config requires original-five-config-v4 originalTEE/AP exploreseed");exit(EXIT_FAILURE);
        }
        memset(s->ap_gpio_direction,0,sizeof(s->ap_gpio_direction));
        memset(s->ap_gpio_output_latch,0,sizeof(s->ap_gpio_output_latch));
        memset(s->ap_gpio_irq_config,0,sizeof(s->ap_gpio_irq_config));
        memset(s->ap_gpio_route,0,sizeof(s->ap_gpio_route));
        error_report("MARCONI_AP_GPIO_CONFIG_PROFILE defaultoff-optin=true exact-five-original-base-plus60-W1-capture=true "
                     "direction-plus4-independent-input0-reset=true output-latch0-SET-CLEAR=true no-input-peer-IRQ-clock-effect-or-ready-model=true");
    }
    if(s->ap_te_host1_config) {
        uint32_t initial[10]={0};
        if(strcmp(s->ap_te_host1_config,"independent-empty-v1")||!s->ap_te_platform_view||
           s->te_context.hash_nhost<2||s->te_context.sca_nhost<2||
           !o62m_symc_empty_init_v1(&s->ap_te_host1_symc,&s->te_capabilities,initial,10)) {
            error_report("ap-te-host1-config requires independent-empty-v1 explicitAPpublicview andexistinghost1 geometry");exit(EXIT_FAILURE);
        }
        error_report("MARCONI_AP_TE_HOST1_CONFIG_PROFILE independent-host1-empty-queues=true declared-zero-SYMC-and-ACA-control-mask-suspend-initial=true "
                     "RNP-mask-initially-unwritten=true shared-const-caps-only=true no-POR-attestation-or-entropy-work-IRQ=true");
    }
    if(s->ap_te_platform_view) {
        if(strcmp(s->ap_te_platform_view,"shared-public-v1")||!s->tee_reset||!s->tee_ap_image||
           !s->explore_seed||!s->board_controls_fixture||!s->te_context_fixture||
           !s->trng_virtual_profile||!s->virtual_otp_profile||!s->te_capability_inputs) {
            error_report("ap-te-platform-view requires shared-public-v1 originalTEE/AP exploreseed canonicalboard COMMON real-AO andvirtual-OTP owners");exit(EXIT_FAILURE);
        }
        error_report("MARCONI_AP_TE_PUBLIC_VIEW_PROFILE defaultoff-optin=true exact-AP-clock-RMW-and-GLOBAL-RO-only=true "
                     "same-existing-canonical-owners=true original-secure-guards-unchanged=true no-new-ready-key-or-seed=true physical-alias-not-proven=true");
    }
    if(s->ap_timer64_view) {
        if(strcmp(s->ap_timer64_view,"independent-dvfs-timer64-v2")||!s->timer64||
           !s->tee_reset||!s->tee_ap_image||!s->explore_seed||!s->clock_controls_fixture) {
            error_report("ap-timer64-view requires independent-dvfs-timer64-v2 originalTEE/AP exploreseed canonicalclock andexistingtimer64owner");exit(EXIT_FAILURE);
        }
        error_report("MARCONI_AP_TIMER64_PROFILE independent-DVFS-counter=true "
            "shared-canonical-source-and-divider=true explicit-cold-disabled-zero=true "
            "RTOS-one-shot-state-and-IRQ-independent=true mode1-and-plus14-and-expiry-strict=true");
    }
    if (s->timer64 && !s->explore_seed) {
        error_report("timer64 requires explicit explore-seed"); exit(EXIT_FAILURE);
    }
    if (s->qos_observed_write_experiment) {
        if (!s->explore_seed) {
            error_report("qos-observed-write-experiment requires explicit explore-seed");
            exit(EXIT_FAILURE);
        }
        error_report("MARCONI_CONDITIONAL_QOS_ASSUMPTION A8-zero-write=accepted "
                     "A8-meaning-and-side-effects=unknown priority-reset=unknown "
                     "arbitration=unmodeled valid-hardware-model=false full-boot=false");
    }
    if (s->lowpage_response_experiment) {
        if (!s->sbl_reset || !s->explore_seed ||
            (strcmp(s->lowpage_response_experiment, "retain-written") &&
             strcmp(s->lowpage_response_experiment, "write-ignore"))) {
            error_report("lowpage-response-experiment requires SBL, explore-seed "
                         "and retain-written or write-ignore");
            exit(EXIT_FAILURE);
        }
        error_report("MARCONI_CONDITIONAL_LOWPAGE_ASSUMPTION mode=%s range=0..fff "
                     "initial-bytes=unknown unread-bytes=reject physical-size=unknown "
                     "TCM-and-BootROM=not-modeled task-pointer=not-injected full-boot=false",
                     s->lowpage_response_experiment);
        qemu_register_reset(marconi_conditional_lowpage_reset, s);
    }
    if (s->uart_tx_fixture && (!s->explore_seed || !s->uart16550 || !s->uart_dw_config)) {
        error_report("uart-tx-fixture requires explore-seed, uart16550 and uart-dw-config");
        exit(EXIT_FAILURE);
    }
    if (s->explore_seed) {
        char *end;
        uint64_t seed = g_ascii_strtoull(s->explore_seed, &end, 0);
        if (*end || !*s->explore_seed || seed > UINT32_MAX) {
            error_report("explore-seed requires explicit unsigned 32-bit seed");
            exit(EXIT_FAILURE);
        }
        s->ctrl54 = s->spmi_ctrl = s->uart_clock = seed;
        s->pubap_dfs_control = s->pubcp_dfs_control = seed;
        o62m_boot_clock_create(&s->boot_clocks, seed);
        if (s->clock_controls_fixture) {
            for (size_t i = 0; i < s->clock_controls.count; ++i) {
                marconi_clock_control_mirror(s, s->clock_controls.defs[i].address,
                                             s->clock_controls.word[i]);
            }
        }
        o62m_hwspinlock_init(&s->hwlock, seed);
        if (s->uart_tx_fixture) {
            uint64_t depth = g_ascii_strtoull(s->uart_tx_fixture, &end, 0);
            if (*end || !*s->uart_tx_fixture || depth > 256 || !o62m_uart_tx_init(&s->tx, depth)) {
                error_report("uart-tx-fixture requires explicit power-of-two depth1..256");
                exit(EXIT_FAILURE);
            }
            s->uart_clock |= 0x10000;
            s->uart_pclk = (uint32_t)seed | 0x20000;
            if (s->clock_controls_fixture) {
                uint32_t word;
                if (o62m_clock_controls_read(&s->clock_controls, 0x4a09000c, 4, &word) == O62M_CLOCK_CONTROLS_OK) {
                    s->uart_clock = word;
                }
                if (o62m_clock_controls_read(&s->clock_controls, 0x4a0900e4, 4, &word) == O62M_CLOCK_CONTROLS_OK) {
                    s->uart_pclk = word;
                }
            }
            s->tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, marconi_tx_timer, s);
            error_report("MARCONI_EXPLORE_UART_FIXTURE depth=%" PRIu64
                         " initial-uart-gate=%u initial-pclk-gate=%u upstream-sources=fixture-available "
                         "physical-depth-and-gate-reset=unknown tx=timed-8n1 rx=unsupported", depth,
                         !!(s->uart_clock & 0x10000), !!(s->uart_pclk & 0x20000));
        }
        s->transaction_ns = 1000;
        if (s->explore_latency) {
            s->transaction_ns = g_ascii_strtoull(s->explore_latency, &end, 0);
            if (*end || !*s->explore_latency || !s->transaction_ns || s->transaction_ns > 1000000000) {
                error_report("explore-latency requires 1..1000000000 virtual ns");
                exit(EXIT_FAILURE);
            }
        }
        if (s->adc_fixture) {
            unsigned ch3, ch4; char extra;
            if (sscanf(s->adc_fixture, "%u:%u%c", &ch3, &ch4, &extra) != 2 ||
                ch3 > 4095 || ch4 > 4095 ||
                !o62m_pmic_adc_init(&s->pmic_adc, seed, ch3, ch4, 10000)) {
                error_report("adc-fixture requires explicit raw12bit channel3:channel4 inputs");
                exit(EXIT_FAILURE);
            }
            error_report("MARCONI_EXPLORE_ADC synthetic-strap-ch3=%u ch4=%u "
                         "conversion-latency-fixture-ns=10000 read-transport=synchronous "
                         "physical-board-id=unknown", ch3, ch4);
        }
        if(s->pmic_regulator_inputs) {
            g_autofree gchar *bytes=NULL;gsize length=0;
            if(!g_file_get_contents(s->pmic_regulator_inputs,&bytes,&length,NULL)||length!=74) {
                error_report("pmic-regulator-inputs requires exact74BLE37independentfullu16words inascendingcatalogorder");exit(EXIT_FAILURE);
            }
            uint32_t initial[O62M_PMIC_REGULATOR_WORD_COUNT_V1];
            for(unsigned i=0;i<O62M_PMIC_REGULATOR_WORD_COUNT_V1;i++) {initial[i]=lduw_le_p(bytes+2*i);}
            int result=o62m_pmic_regulator_init_v1(&s->pmic_regulators,initial,O62M_PMIC_REGULATOR_WORD_COUNT_V1);
            if(result) {error_report("pmic-regulator-inputs invalid: %s",o62m_pmic_regulator_reason_v1(result));exit(EXIT_FAILURE);}
            uint16_t canonical68;
            result=o62m_pmic_regulator_read_v1(&s->pmic_regulators,0x68,&canonical68);
            assert(!result);
            s->pmic_adc.control_68=canonical68;
            g_autofree gchar *digest=g_compute_checksum_for_data(G_CHECKSUM_SHA256,(const guchar *)bytes,length);
            error_report("MARCONI_PMIC_REGULATOR_INPUTS bytes=74 words=37 sha256=%s "
                         "canonical68-owns-ADC-mirror=true base-v1-alias-read-strict=true optin-separate-command-read-policy-explicit=true "
                         "physical-reset-rail-voltage-feedback-and-ready-unmodeled=true",digest);
        }
        if(s->pmic_clock_inputs) {
            g_autofree gchar *bytes=NULL;gsize length=0;
            if(!g_file_get_contents(s->pmic_clock_inputs,&bytes,&length,NULL)||length!=12) {
                error_report("pmic-clock-inputs requires exact12BLEsixindependentu16words canonical0004,0018,001c,1000,1004,1014");exit(EXIT_FAILURE);
            }
            uint32_t initial[O62M_PMIC_CLOCK_WORDS_V2];
            for(unsigned i=0;i<O62M_PMIC_CLOCK_WORDS_V2;i++) {initial[i]=lduw_le_p(bytes+2*i);}
            int result=o62m_pmic_clock_init_v2(&s->pmic_clocks,initial,O62M_PMIC_CLOCK_WORDS_V2);
            if(result) {error_report("pmic-clock-inputs invalid: %s",o62m_pmic_clock_reason_v2(result));exit(EXIT_FAILURE);}
            /* Canonical1c word owns the existingADC storage mirror. Physical
             * gate/divider coupling remains unmodeled; active changes strict. */
            s->pmic_adc.config_1c=s->pmic_clocks.word[2];
            g_autofree gchar *digest=g_compute_checksum_for_data(G_CHECKSUM_SHA256,(const guchar *)bytes,length);
            error_report("MARCONI_PMIC_CLOCK_INPUTS bytes=12 words=6 sha256=%s original-fields=33 "
                         "initial1c-and-ADC-mirror-single-backing=true only-single-proven-field-delta=true "
                         "physical-reset-clock-output-and-ADC-divider-effects-unmodeled=true",digest);
        }
        if(s->auxadc_channel_profile) {
            if(!s->tee_reset||!s->tee_ap_image||!s->adc_fixture||!s->virtual_pmic_bc_config||!s->pmic_regulator_inputs||!s->pmic_clock_inputs) {
                error_report("auxadc-channel-profile originalTEE/AP/ADC/canonicalBC/PMIC/clock owners required");exit(EXIT_FAILURE);
            }
            g_auto(GStrv) words=g_strsplit(s->auxadc_channel_profile,":",-1);uint64_t inputs[8];
            if(g_strv_length(words)!=8){error_report("auxadc-channel-profile requires presentMask:raw0:raw1:raw2:raw5:raw6:raw7:raw15");exit(EXIT_FAILURE);}
            for(unsigned i=0;i<8;i++) {
                char *end;if(!g_ascii_isdigit(words[i][0])){error_report("AUXADCprofile unsignedinputs required");exit(EXIT_FAILURE);}
                inputs[i]=g_ascii_strtoull(words[i],&end,0);if(*end||inputs[i]>(i?4095:UINT16_MAX)){error_report("AUXADCprofile raw12/maskrange invalid");exit(EXIT_FAILURE);}
            }
            if((inputs[0]&~O62M_AUXADC_EXTRA_INPUT_MASK_V1)||!o62m_pmic_adc_ap_init_v1(&s->auxadc_ap)){error_report("AUXADCprofile unsupportedinputchannelmask");exit(EXIT_FAILURE);}
            static const unsigned channels[7]={0,1,2,5,6,7,15};
            for(unsigned i=0;i<7;i++) {
                if(inputs[0]&(UINT64_C(1)<<channels[i])) {
                    if(!o62m_pmic_adc_ap_set_input_v1(&s->pmic_adc,&s->auxadc_ap,channels[i],inputs[i+1])){error_report("AUXADCprofile inputprovider rejected");exit(EXIT_FAILURE);}
                } else if(inputs[i+1]){error_report("AUXADCprofile absentinputs mustbezero");exit(EXIT_FAILURE);}
            }
            s->auxadc_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_auxadc_callback,s);
            error_report("MARCONI_AUXADC_CHANNEL_PROFILE extra-present-mask=%04x explicit-raw12-virtual-signals=true legacy3-4-single-owner=true one-canonical-work-config-result-core=true actual-conversion-deadline-required=true IRQ-external82-from-CC1-ready-local100-globalBC2=true physical-inputs-clocks-polarity-and-level-ACK-unclaimed=true no-host-guestflag-write=true",s->auxadc_ap.present_mask);
        }
        if (s->timer64) {
            uint32_t csr = seed, pclk = seed;
            if (s->clock_controls_fixture) {
                bool csr_ok = o62m_boot_clock_read(&s->boot_clocks, 0x4006005c, &csr);
                bool pclk_ok = o62m_boot_clock_read(&s->boot_clocks, 0x400600f8, &pclk);
                assert(csr_ok && pclk_ok);
            }
            bool ok = o62m_timer64_create(&s->timer64_model, 0, 0, 0,
                         (uint32_t)seed & ~5u, (uint32_t)seed & ~14u, s->transaction_ns,
                         (csr & 1) ? 26000000 : 32000, ((csr >> 8) & 255) + 1,
                         (csr & 0x10000) && (pclk & 0x20000), O62M_TIMER64_HOLD_FIRST_EXPIRY);
            assert(ok);
            s->timer64_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, marconi_timer64_callback, s);
            if (s->ap_timer64_view) {
                ok = o62m_timer64_create(&s->dvfs_timer64_model, 0, 0, 0, 0, 0,
                         s->transaction_ns, (csr & 1) ? 26000000 : 32000,
                         ((csr >> 8) & 255) + 1,
                         (csr & 0x10000) && (pclk & 0x20000), O62M_TIMER64_HOLD_FIRST_EXPIRY);
                assert(ok);
                s->dvfs_timer64_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                                   marconi_dvfs_timer64_callback, s);
            }
            if(s->timer64_terminal_profile) {
                s->timer64_exit.notify=marconi_timer64_exit;qemu_add_exit_notifier(&s->timer64_exit);
                error_report("MARCONI_TIMER64_TERMINAL_PROFILE virtual-HOLD_FIRST_EXPIRY=true real-countdown-raw-IRQ-mask-and-delayed-ACK=true external=10 physical-terminal-behavior-not-claimed=true");
            }
            error_report("MARCONI_EXPLORE_TIMER64_FIXTURE initial-load=0 snapshot=0 "
                         "initial-disabled=1 sync-latency-ns=%" PRIu64
                         " clock-CSR=%08x clock-PCLK=%08x shared-controller=%u "
                         "parent-availability=fixture upstream-gates=unmodeled "
                         "first-expiry-policy=%s",
                         s->transaction_ns, csr, pclk, !!s->clock_controls_fixture,s->timer64_terminal_profile?"virtual-hold-IRQ":"strict-stop");
        }
        if(s->timer0_runtime_fixture) {
            g_auto(GStrv) words=g_strsplit(s->timer0_runtime_fixture,":",-1);
            uint64_t initial[7];
            if(g_strv_length(words)!=7) {
                error_report("timer0-runtime-fixture requires explicit load64:snapshot64:control:status:snapreq:ackreq:rawirq");exit(EXIT_FAILURE);
            }
            for(unsigned i=0;i<7;i++) {
                char *end;
                if(!g_ascii_isdigit(words[i][0])) {error_report("timer0-runtime-fixture requires unsigned values");exit(EXIT_FAILURE);}
                initial[i]=g_ascii_strtoull(words[i],&end,0);
                if(*end||(i>=2&&initial[i]>UINT32_MAX)||(i==6&&initial[i]>1)) {
                    error_report("timer0-runtime-fixture word exceeds its64/32/1bit input width");exit(EXIT_FAILURE);
                }
            }
            uint32_t csr,pclk;marconi_timer0_runtime_clock_words(s,&csr,&pclk);
            O62MShotInit in={.now_ns=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),.load=initial[0],.snapshot=initial[1],
                .latency_ns=s->transaction_ns,.source_hz=(csr&1u)?26000000:32000,
                .control=initial[2],.status_opaque=initial[3],.divisor=((csr>>8)&255u)+1u,
                .snapshot_request_opaque=initial[4],.ack_request_opaque=initial[5],
                .gate=(csr&0x10000u)&&(pclk&0x20000u),.raw_irq=initial[6],
                .policy=s->timer0_one_shot_experiment?O62M_SHOT_HOLD_ZERO_WITH_IRQ:O62M_SHOT_FIRST_EXPIRY_UNSUPPORTED};
            O62MShotResult r=o62m_shot_timer_create(&s->timer0_runtime,&in);
            if(r!=O62M_SHOT_OK) {error_report("timer0-runtime-fixture invalid: %s",o62m_shot_timer_reason(r));exit(EXIT_FAILURE);}
            s->timer0_runtime_timer=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_timer0_runtime_callback,s);
            error_report("MARCONI_TIMER0_RUNTIME_FIXTURE words=%s irq=4 sync-latency-ns=%" PRIu64
                " policy=%s physical-reset-timing-and-terminal-behavior-unverified=true no-periodic-mode=true",
                s->timer0_runtime_fixture,s->transaction_ns,s->timer0_one_shot_experiment?"explicit-virtual-one-shot":"strict-first-expiry-stop");
        }
        if(s->timer4014_inputs) {
            g_autofree gchar *bytes=NULL;gsize length=0;
            if(!g_file_get_contents(s->timer4014_inputs,&bytes,&length,NULL)||length!=40) {
                error_report("timer4014-inputs requires exact40B LEtenu32(load,current,fullCTRL,EOIvalue,rawIRQbool)x2");exit(EXIT_FAILURE);
            }
            O62MDw32InputV1 input[2]={0};
            for(unsigned i=0;i<2;i++) {
                const gchar *row=bytes+i*20;
                uint32_t raw=ldl_le_p(row+16);
                if(raw>1) {error_report("timer4014-inputs rawIRQ mustbe0or1");exit(EXIT_FAILURE);}
                input[i].load=ldl_le_p(row);input[i].current=ldl_le_p(row+4);
                input[i].control=ldl_le_p(row+8);input[i].eoi_value=ldl_le_p(row+12);input[i].raw_irq=raw;
            }
            uint32_t csr0,csr1,pclk;O62MDw32ClockV1 clock={0};
            marconi_timer4014_words(s,&csr0,&csr1,&pclk);
            if(!marconi_timer4014_clock(s,&s->clock_controls,csr0,csr1,pclk,&clock)) {exit(EXIT_FAILURE);}
            int result=o62m_dw32_init_v1(&s->timer4014,input,&clock,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                s->timer4014_load_plus_one?O62M_DW32_VIRTUAL_LOAD_PLUS_ONE_V1:O62M_DW32_STRICT_FIRST_EXPIRY_V1);
            if(result) {error_report("timer4014-inputs invalid: %s",o62m_dw32_reason_v1(result));exit(EXIT_FAILURE);}
            s->timer4014_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_timer4014_callback,s);
            g_autofree gchar *digest=g_compute_checksum_for_data(G_CHECKSUM_SHA256,(const guchar *)bytes,length);
            error_report("MARCONI_TIMER4014_INPUTS sha256=%s policy=%s nvic-external=8,9 source0-Hz=32000 "
                         "software-object-Hz=32768 rational-source-divider=true gates-from-original-controller=true "
                         "physical-reset-and-load-terminal-offbyone-unverified=true no-64bit-timer-alias=true",
                         digest,s->timer4014_load_plus_one?"explicit-virtual-load-plus-one":"strict-first-expiry");
        }
        error_report("MARCONI_EXPLORE_ASSUMPTIONS opaque-control-seed=0x%08x "
                     "transaction-latency-ns=%" PRIu64 " silicon-reset-and-timing=unknown "
                     "pmic-data=bounded-adc-only-when-explicit dlf-upper-bits-fixture=0 full-boot=false", (uint32_t)seed, s->transaction_ns);
    }
    if (s->uart_dw_config && !s->uart16550) {
        error_report("uart-dw-config requires uart16550=on");
        exit(EXIT_FAILURE);
    }

    if (!machine->kernel_filename ||
        !g_file_get_contents(machine->kernel_filename, &ap, &bytes, NULL)) {
        error_report("Marconi probe requires an O62M firmware image");
        exit(EXIT_FAILURE);
    }
    if (s->tee_reset) {
        g_autofree gchar *checksum = g_compute_checksum_for_data(
            G_CHECKSUM_SHA256, (const guchar *)ap, bytes);

        /* No modified image may be called an original TEE boot probe.
         * VTOR literal is at reset+0x54; stack covers real TEE BSS/idle stack. */
        layout_matches = bytes == 770048 &&
            !strcmp(checksum, MARCONI_TEE_SHA256) &&
            ldl_le_p(ap) == 0x73ebf550 &&
            ldl_le_p(ap + 4) == 0x73e11201 &&
            ldl_le_p(ap + 0x11254) == MARCONI_TEE_ALIAS;
    } else if (s->sbl_reset) {
        layout_matches = bytes == 733568 &&
            ldl_le_p(ap + 0x1000) == 0x600c8b30 &&
            ldl_le_p(ap + 0x1004) == 0x60013d71 &&
            ldl_le_p(ap + 0x12d9c) == MARCONI_SBL_VTOR;
    } else if(s->ota_reset) {
        g_autofree gchar *checksum=g_compute_checksum_for_data(G_CHECKSUM_SHA256,(const guchar *)ap,bytes);
        layout_matches=bytes==MARCONI_OTA_IMAGE_BYTES&&!strcmp(checksum,MARCONI_OTA_SHA256)&&
            ldl_le_p(ap)==0x60688dd0&&ldl_le_p(ap+4)==0x60540821&&
            ldl_le_p(ap+0x1084c)==MARCONI_OTA_BASE&&ldl_le_p(ap+0x10858)==0x60688dd0;
    } else {
        layout_matches = bytes >= 0x83221c && bytes <= MARCONI_AP_BYTES &&
            ldl_le_p(ap) == 0x61745d00 && ldl_le_p(ap + 4) == 0x60df21e1 &&
            ldl_le_p(ap + 0x83220c) == MARCONI_AP_BASE;
    }
    if (!layout_matches) {
        error_report("Marconi probe requires the O62M %s vector/VTOR layout%s",
                     stage, s->tee_reset ? " and exact original TEE SHA256" : "");
        exit(EXIT_FAILURE);
    }

    if (s->tee_ap_image) {
        g_autofree gchar *checksum = NULL;
        bool valid = g_file_get_contents(s->tee_ap_image, &handoff_ap,
                                         &handoff_ap_bytes, NULL);
        if (valid) {
            checksum = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                (const guchar *)handoff_ap, handoff_ap_bytes);
        }
        if (!valid || handoff_ap_bytes != MARCONI_AP_IMAGE_BYTES ||
            strcmp(checksum, MARCONI_AP_SHA256) ||
            ldl_le_p(handoff_ap) != 0x61745d00 ||
            ldl_le_p(handoff_ap + 4) != 0x60df21e1 ||
            ldl_le_p(handoff_ap + 0x83220c) != MARCONI_AP_BASE ||
            ldl_le_p(handoff_ap + 0x832218) != 0x61745d00) {
            error_report("tee-ap-image requires the exact original O62M AP SHA256/size/vector layout");
            exit(EXIT_FAILURE);
        }
    }

    memory_region_init_io(&s->missing, OBJECT(machine), &marconi_missing_ops,
                          s, "marconi.unimplemented", UINT64_C(0x100000000));
    memory_region_add_subregion_overlap(get_system_memory(), 0,
                                        &s->missing, -10);
    memory_region_init_ram(&s->ap_ram, NULL,
                           s->tee_reset ? "marconi.tee-ram" :
                           s->sbl_reset ? "marconi.sbl-ram" : s->ota_reset ? "marconi.ota-ram" : "marconi.ap-ram",
                           ram_bytes, &error_fatal);
    memory_region_add_subregion(get_system_memory(), load_base,
                                &s->ap_ram);
    if(s->ota_reset&&s->ota_sbl_argument_fixture) {
        /* Same original mmap ap_cp_sm physical RAM as the SBL producer.
         * Only modeword is reconstructed; SBL/hardware snapshots unmodeled. */
        memory_region_init_ram(&s->ap_cp_sm_ram,NULL,"marconi.ota-sbl-argument-ram",MARCONI_SBL_BYTES,&error_fatal);
        memory_region_add_subregion(get_system_memory(),MARCONI_SBL_BASE,&s->ap_cp_sm_ram);
        uint8_t mode[4];stl_le_p(mode,s->ota_sbl_boot_mode);
        rom_add_blob_fixed("marconi.ota-original-sbl-mode-input",mode,sizeof mode,0x600c2800);
        error_report("MARCONI_OTA_ORIGINAL_LAYOUT base=60530000 end=60b30000 "
                     "RAM-end-proven-by-original-heapgetter=true "
                     "bootarg=600c2800 mode=%u original-SBL-executed=false",s->ota_sbl_boot_mode);
    }
    if(s->ota_retained_footer_image) {
        /* Exact four RAM words observed after unmodified TEE CPU executed
         * its timed ADC sampling and payload-then-marker stores. This does
         * not synthesize a valid marker or initialize unobserved prefix RAM. */
        g_autofree gchar *footer=NULL;
        gsize footer_bytes=0;
        bool read=g_file_get_contents(s->ota_retained_footer_image,&footer,&footer_bytes,NULL);
        g_autofree gchar *digest=read?g_compute_checksum_for_data(G_CHECKSUM_SHA256,(const guchar *)footer,footer_bytes):NULL;
        if(!read||footer_bytes!=16||strcmp(digest,"56922d94b2e9b55f684646354c175c6e6a5e10dbe2bde6dc7fd0bbb4358db5b1")) {
            error_report("ota-retained-footer-image requires exactcaptured16BoriginalTEEcachecapsule SHA56922d94b2e9b55f684646354c175c6e6a5e10dbe2bde6dc7fd0bbb4358db5b1");exit(EXIT_FAILURE);
        }
        memory_region_init_ram(&s->ota_retained_footer_ram,NULL,"marconi.actual-tee-retained-footer",16,&error_fatal);
        memory_region_add_subregion(get_system_memory(),0x22004ff0,&s->ota_retained_footer_ram);
        rom_add_blob_fixed("marconi.actual-tee-produced-retained-footer",footer,footer_bytes,0x22004ff0);
        error_report("MARCONI_OTA_RETAINED_FOOTER physical=22004ff0 bytes=16 sha256=%s "
                     "source=original-TEE-timed-ADC-actual-RAM-capture "
                     "standalone-retained-software-state-experiment=true "
                     "unobserved-prefix-RAM-and-full-SBL-state-unimplemented=true "
                     "normal-watch-boot-accepted=false",digest);
    }
    if(s->ota_retained_hwlock_image) {
        g_autofree gchar *state=NULL;
        gsize state_bytes=0;
        bool read=g_file_get_contents(s->ota_retained_hwlock_image,&state,&state_bytes,NULL);
        g_autofree gchar *digest=read?g_compute_checksum_for_data(G_CHECKSUM_SHA256,(const guchar *)state,state_bytes):NULL;
        if(!read||state_bytes!=O62M_HWLOCK_CAPSULE_BYTES||strcmp(digest,"caeaef71cd380f14f07ff8f478fa2d7d334da7cad24d945315a208c23e37daf3")) {
            error_report("ota-retained-hwlock-image requires exactv34actual88Bstatecapture SHAcaeaef71cd380f14f07ff8f478fa2d7d334da7cad24d945315a208c23e37daf3");exit(EXIT_FAILURE);
        }
        O62MHwLockRestoreResult result=o62m_hwlock_restore(&s->hwlock,(const uint8_t *)state,state_bytes,s->hwlock.config_seed);
        if(result!=O62M_HWLOCK_RESTORE_OK) {error_report("ota-retained-hwlock-image invalid: %s",o62m_hwlock_restore_reason(result));exit(EXIT_FAILURE);}
        error_report("MARCONI_OTA_RETAINED_HWLOCK sha256=%s seed=%08x config=%08x owned=%08x external=%08x "
                     "complete=%u asserted=%u rebased-assert-ns=%" PRId64 " rebased-release-ns=%" PRId64 " "
                     "source=actual-original-TEE-modelstate atomic-owner-operations-unchanged=true "
                     "explicit-retained-modelstate-experiment=true physical-retention-unverified=true",
                     digest,s->hwlock.config_seed,s->hwlock.config,s->hwlock.owned,s->hwlock.external_owned,
                     s->hwlock.reset_complete,s->hwlock.reset_asserted,s->hwlock.asserted_ns,s->hwlock.released_ns);
    }
    if (s->tee_ap_image) {
        /* Host provides the exact original AP image that SBL would load.
         * TEE still starts at its own Secure vector and must reach its real
         * 73e69426 BLXNS; no second CPU/reset/call is installed for AP. */
        memory_region_init_ram(&s->handoff_ap_ram, NULL,
                               "marconi.handoff-ap-ram", MARCONI_AP_BYTES,
                               &error_fatal);
        memory_region_add_subregion(get_system_memory(), MARCONI_AP_BASE,
                                    &s->handoff_ap_ram);
        memory_region_init_alias(&s->handoff_ap_alias, OBJECT(machine),
                                 "marconi.handoff-ap-secure", &s->handoff_ap_ram,
                                 0, MARCONI_AP_BYTES);
        memory_region_add_subregion(get_system_memory(), MARCONI_AP_SECURE_ALIAS,
                                    &s->handoff_ap_alias);
        /* Load these already verified bytes, not a second file read. A ROM
         * loader blob initializes writable RAM; the image can initialize BSS
         * normally. It does not create an immutable hardware ROM region. */
        rom_add_blob_fixed("marconi.original-ap-image", handoff_ap,
                           handoff_ap_bytes, MARCONI_AP_BASE);
        error_report("MARCONI_PROBE_AP_IMAGE physical=0x605c0000 "
                     "secure-alias=0x705c0000 bytes=%zu sha256=%s "
                     "provision=host-verified-original entry=original-TEE-BLXNS-only "
                     "SBL-loader=not-executed full-boot=false",
                     handoff_ap_bytes, MARCONI_AP_SHA256);
    }
    if ((!s->sbl_reset && !s->tee_reset && !s->ota_reset) || s->tee_ap_image) {
        /* AP heap allocator605cb58a..592 uses end638f4000; actual MPU
         * RLAR638f3ff9 (literal60c49968) independently includes this1KiB.
         * Keep mmap image-capacity3333c00 unchanged; this is physical heap RAM. */
        memory_region_init_ram(&s->heap_tail, NULL, "marconi.ap-heap-tail",
                               0x400, &error_fatal);
        memory_region_add_subregion(get_system_memory(), 0x638f3c00, &s->heap_tail);
        if (s->tee_ap_image) {
            memory_region_init_alias(&s->handoff_heap_alias, OBJECT(machine),
                                     "marconi.handoff-ap-heap-secure", &s->heap_tail,
                                     0, 0x400);
            memory_region_add_subregion(get_system_memory(), 0x738f3c00,
                                        &s->handoff_heap_alias);
        }
        error_report("MARCONI_PROBE_HEAP_TAIL base=0x638f3c00 size=0x400 "
                     "evidence=original-heap-end-and-MPU-RLAR mmap-capacity=unchanged");
    }
    if (s->tee_reset) {
        static const struct { const char *name; hwaddr base; uint32_t size; } aon[] = {
            { "cp-reserved", 0x22002c00, 0x400 },
            { "mailbox-ram", 0x22003800, 0x400 },
            { "lps-ree", 0x22003c00, 0x1000 },
            { "clk-cnt-ram", 0x22004c00, 0x400 },
        };
        for (unsigned i = 0; i < G_N_ELEMENTS(aon); i++) {
            g_autofree char *name = g_strdup_printf("marconi.%s", aon[i].name);
            g_autofree char *alias = g_strdup_printf("%s-secure", name);
            memory_region_init_ram(&s->aon_ram[i], NULL, name, aon[i].size, &error_fatal);
            memory_region_add_subregion(get_system_memory(), aon[i].base, &s->aon_ram[i]);
            memory_region_init_alias(&s->aon_alias[i], OBJECT(machine), alias,
                                     &s->aon_ram[i], 0, aon[i].size);
            memory_region_add_subregion(get_system_memory(), aon[i].base + 0x10000000,
                                        &s->aon_alias[i]);
        }
        /* mmap sp_ap_sm22000800+2400: actualTEEshared-buffer initialization. */
        memory_region_init_ram(&s->sp_ap_shared, NULL, "marconi.sp-ap-shared", 0x2400, &error_fatal);
        memory_region_add_subregion(get_system_memory(), 0x22000800, &s->sp_ap_shared);
        memory_region_init_alias(&s->sp_ap_shared_alias, OBJECT(machine), "marconi.sp-ap-shared-secure",
                                 &s->sp_ap_shared, 0, 0x2400);
        memory_region_add_subregion(get_system_memory(), 0x32000800, &s->sp_ap_shared_alias);
        /* mmap tee_ree_log63df4000+4000 and tee_ree_sm63df8000+8000. */
        memory_region_init_ram(&s->tee_ree_ram, NULL, "marconi.tee-ree-shared", 0xc000, &error_fatal);
        memory_region_add_subregion(get_system_memory(), 0x63df4000, &s->tee_ree_ram);
        memory_region_init_alias(&s->tee_ree_alias, OBJECT(machine), "marconi.tee-ree-secure",
                                 &s->tee_ree_ram, 0, 0xc000);
        memory_region_add_subregion(get_system_memory(), 0x73df4000, &s->tee_ree_alias);
        /* Original mmap adjacent dsp_cp_sm,dsp_offload,reserved3 cover
         *21000000+10000; originalTEE MPU and stores use31000000 alias. */
        memory_region_init_ram(&s->dsp_cp_ram, NULL, "marconi.dsp-cp-ram", 0x10000, &error_fatal);
        memory_region_add_subregion(get_system_memory(), 0x21000000, &s->dsp_cp_ram);
        memory_region_init_alias(&s->dsp_cp_alias, OBJECT(machine), "marconi.dsp-cp-secure",
                                 &s->dsp_cp_ram, 0, 0x10000);
        memory_region_add_subregion(get_system_memory(), 0x31000000, &s->dsp_cp_alias);
        /* mmap ap_dsp_sm_ucache60529000+800 and ap_dsp_sm60529800+16800
         * are adjoining physical shared RAM. TEE addresses secure aliases. */
        memory_region_init_ram(&s->dsp_shared, NULL, "marconi.ap-dsp-shared", 0x17000, &error_fatal);
        memory_region_add_subregion(get_system_memory(), 0x60529000, &s->dsp_shared);
        memory_region_init_alias(&s->dsp_shared_alias, OBJECT(machine), "marconi.ap-dsp-shared-secure",
                                 &s->dsp_shared, 0, 0x17000);
        memory_region_add_subregion(get_system_memory(), 0x70529000, &s->dsp_shared_alias);
        /* mmap CP_log_buf at record200, real securealias used by TEE. */
        memory_region_init_ram(&s->cp_log, NULL, "marconi.cp-log", 0x40000, &error_fatal);
        memory_region_add_subregion(get_system_memory(), 0x60240000, &s->cp_log);
        memory_region_init_alias(&s->cp_log_alias, OBJECT(machine), "marconi.cp-log-secure",
                                 &s->cp_log, 0, 0x40000);
        memory_region_add_subregion(get_system_memory(), 0x70240000, &s->cp_log_alias);
        /* Original mmap CP/CP_f, CP_boot/CPbootf and CRF/CRF_f share
         * these physical RAM regions. Original media loaders populate them. */
        if(s->normal_emmc_profile) {
            memory_region_init_ram(&s->cp_image_ram,NULL,"marconi.cp-image-ram",0xd00000,&error_fatal);
            memory_region_add_subregion(get_system_memory(),0x64000000,&s->cp_image_ram);
            memory_region_init_ram(&s->cp_boot_ram,NULL,"marconi.cp-boot-ram",0x400,&error_fatal);
            memory_region_add_subregion(get_system_memory(),0x22000000,&s->cp_boot_ram);
            memory_region_init_ram(&s->cp_crf_ram,NULL,"marconi.cp-crf-ram",0x10000,&error_fatal);
            memory_region_add_subregion(get_system_memory(),0x64df0000,&s->cp_crf_ram);
            memory_region_init_alias(&s->cp_image_secure,OBJECT(machine),"marconi.cp-image-secure",&s->cp_image_ram,0,0xd00000);
            memory_region_add_subregion(get_system_memory(),0x74000000,&s->cp_image_secure);
            memory_region_init_alias(&s->cp_boot_secure,OBJECT(machine),"marconi.cp-boot-secure",&s->cp_boot_ram,0,0x400);
            memory_region_add_subregion(get_system_memory(),0x32000000,&s->cp_boot_secure);
            memory_region_init_alias(&s->cp_crf_secure,OBJECT(machine),"marconi.cp-crf-secure",&s->cp_crf_ram,0,0x10000);
            memory_region_add_subregion(get_system_memory(),0x74df0000,&s->cp_crf_secure);
            s->cp_image_ram_created=true;
            s->cp_image_ram_exit.notify=marconi_cp_image_ram_exit;
            qemu_add_exit_notifier(&s->cp_image_ram_exit);
            marconi_public_report("MARCONI_CP_IMAGE_RAM original-mmap-regions=3 exact-secure-aliases=3 cold-zero=true "
                "original-media-loader-required=true CPU-created=false preloaded=false");
        }
        /* mmap ap_cp_sm: TEE intentionally clears/reuses this shared RAM.
         * No SBL image or precomputed handoff state is injected here. */
        memory_region_init_ram(&s->ap_cp_sm_ram, NULL, "marconi.ap-cp-shared",
                               MARCONI_SBL_BYTES, &error_fatal);
        memory_region_add_subregion(get_system_memory(), MARCONI_SBL_BASE,
                                    &s->ap_cp_sm_ram);
        memory_region_init_alias(&s->ap_cp_sm_alias, OBJECT(machine),
                                 "marconi.ap-cp-shared-secure", &s->ap_cp_sm_ram,
                                 0, MARCONI_SBL_BYTES);
        memory_region_add_subregion(get_system_memory(), MARCONI_SBL_BASE + 0x10000000,
                                    &s->ap_cp_sm_alias);
        if(s->tee_sbl_argument_fixture) {
            uint8_t boot_mode[4];stl_le_p(boot_mode,s->tee_sbl_boot_mode);
            /* Original SBL's BSS boot-mode word, already inside existing
             * AP/CP shared RAM. This adds data, not code or a lowpage alias. */
            rom_add_blob_fixed("marconi.original-sbl-boot-mode-input",boot_mode,sizeof boot_mode,0x600c2800);
            error_report("MARCONI_TEE_SBL_ARGUMENT_DATA physical=600c2800 secure-alias=700c2800 "
                "bytes=4 original-SBL-mode-index=%u SBL-boot-chain-executed=false",s->tee_sbl_boot_mode);
        }
        /* SAU controls security attributes, not address translation. Both
         * addresses below refer to the same RAM bytes; do not duplicate them.
         * These aliases do not set an IDAU or classify the whole map NS. */
        memory_region_init_alias(&s->tee_alias, OBJECT(machine),
                                 "marconi.tee-secure-alias", &s->ap_ram,
                                 0, MARCONI_TEE_BYTES);
        memory_region_add_subregion(get_system_memory(), MARCONI_TEE_ALIAS,
                                    &s->tee_alias);
        memory_region_init_ram(&s->lps_tee_ram, NULL, "marconi.lps-tee-ram",
                               MARCONI_LPS_TEE_BYTES, &error_fatal);
        memory_region_add_subregion(get_system_memory(), MARCONI_LPS_TEE_BASE,
                                    &s->lps_tee_ram);
        memory_region_init_alias(&s->lps_tee_alias, OBJECT(machine),
                                 "marconi.lps-tee-secure-alias", &s->lps_tee_ram,
                                 0, MARCONI_LPS_TEE_BYTES);
        memory_region_add_subregion(get_system_memory(), MARCONI_LPS_TEE_ALIAS,
                                    &s->lps_tee_alias);
        error_report("MARCONI_PROBE_TEE_MEMORY physical=0x63e00000 "
                     "secure-alias=0x73e00000 size=0x200000 "
                     "lps-physical=0x22005000 lps-alias=0x32005000 "
                     "lps-size=0x5000 ap-image=%s boot-chain=not-executed",
                     s->tee_ap_image ? "verified-original-host-preload" : "absent");
    }
    if(s->ap_boot_buffer_enabled) {
        if(!s->tee_reset||!s->tee_ap_image||!s->explore_seed) {
            error_report("ap-boot-buffer-ram requires originalTEE/AP/explore");exit(EXIT_FAILURE);
        }
        /* Original callers 60e48ce6/60e48d74 pass this full span to the
         * allocator setup at60e47174; original60e4726a clears its bytes. */
        memory_region_init_ram(&s->ap_boot_buffer_ram,NULL,"marconi.ap-boot-buffer-ram",UINT64_C(0x10000),&error_fatal);
        memory_region_add_subregion(get_system_memory(),UINT64_C(0x2012c000),&s->ap_boot_buffer_ram);
        error_report("MARCONI_AP_BOOT_BUFFER_RAM base=2012c000 bytes=10000 original-static-caller-span=true independent-volatile-RAM=true CPU-initializes-own-metadata=true virtual-initialzero=true");
    }
    if(s->ap_uncache_ram_profile&&(!s->mmap_image||!s->tee_reset||!s->tee_ap_image||!s->explore_seed||
       strcmp(s->ap_uncache_ram_profile,"original-mmap-v1"))) {
        error_report("ap-uncache-ram requires original-mmap-v1 originalTEE/AP exploreseed andoriginalmmap");exit(EXIT_FAILURE);
    }
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
        if(s->ap_uncache_ram_profile) {
            g_autofree gchar *map_sha=g_compute_checksum_for_data(G_CHECKSUM_SHA256,(const guchar *)map,map_bytes);
            if(strcmp(map_sha,"c16f0ec3a35cca7a652fafaa87eed4252482b3c18d3fd2a502a2f5794d5c49b8")||
               memcmp(map+0x2c0,"ap_uncache",sizeof("ap_uncache"))||
               ldl_le_p(map+0x2d4)!=UINT32_C(0x638f4000)||
               ldl_le_p(map+0x2d8)!=UINT32_C(0x00500000)||ldl_le_p(map+0x2dc)!=0) {
                error_report("ap-uncache-ram requires exactoriginalmmaphash andtypedrecord2c0");exit(EXIT_FAILURE);
            }
            if(s->ap_boot_buffer_enabled) {
                if(memcmp(map+0x5c0,"gpu_sys",sizeof("gpu_sys"))||
                   ldl_le_p(map+0x5d4)!=UINT32_C(0x20000000)||
                   ldl_le_p(map+0x5d8)!=UINT32_C(0x002a0000)||ldl_le_p(map+0x5dc)!=0) {
                    error_report("gpu_sys RAM requires original mmap record05c0");exit(EXIT_FAILURE);
                }
                memory_region_init_ram(&s->gpu_sys_low_ram,NULL,"marconi.gpu-sys-low-ram",UINT64_C(0x12c000),&error_fatal);
                memory_region_add_subregion(get_system_memory(),UINT64_C(0x20000000),&s->gpu_sys_low_ram);
                memory_region_init_ram(&s->gpu_sys_high_ram,NULL,"marconi.gpu-sys-high-ram",UINT64_C(0x164000),&error_fatal);
                memory_region_add_subregion(get_system_memory(),UINT64_C(0x2013c000),&s->gpu_sys_high_ram);
                error_report("MARCONI_GPU_SYS_RAM original-mmap-base=20000000 bytes=2a0000 existing-middle-owner-preserved=true "
                    "low-bytes=12c000 middle-bytes=10000 high-bytes=164000 volatile-RAM=true guest-populates-bytes=true no-GPU-work-or-frame-generated=true");
            }
            /* A separate public mmap partition, never another view of AP or
             * TEE bytes. Initialzero is QEMUvirtualRAM policy, not siliconPOR.
             * The original CPU must initialize its own heap metadata. */
            memory_region_init_ram(&s->ap_uncache_ram,NULL,"marconi.ap-uncache-ram",UINT64_C(0x00500000),&error_fatal);
            memory_region_add_subregion(get_system_memory(),UINT64_C(0x638f4000),&s->ap_uncache_ram);
            s->ap_uncache_ram_created=true;
            if(memcmp(map+0x320,"ipc_nv_regions",sizeof("ipc_nv_regions"))||
               ldl_le_p(map+0x334)!=UINT32_C(0x64d70000)||
               ldl_le_p(map+0x338)!=UINT32_C(0x00070000)||ldl_le_p(map+0x33c)!=0) {
                error_report("IPC NV RAM requires original mmap record0320");exit(EXIT_FAILURE);
            }
            memory_region_init_ram(&s->ipc_nv_ram,NULL,"marconi.ipc-nv-ram",UINT64_C(0x70000),&error_fatal);
            memory_region_add_subregion(get_system_memory(),UINT64_C(0x64d70000),&s->ipc_nv_ram);
            if(memcmp(map+0x340,"ims_delta_nv",sizeof("ims_delta_nv"))||
               ldl_le_p(map+0x354)!=UINT32_C(0x64de0000)||
               ldl_le_p(map+0x358)!=UINT32_C(0x00010000)||ldl_le_p(map+0x35c)!=0) {
                error_report("IMS NV RAM requires original mmap record0340");exit(EXIT_FAILURE);
            }
            memory_region_init_ram(&s->ims_nv_ram,NULL,"marconi.ims-nv-ram",UINT64_C(0x10000),&error_fatal);
            memory_region_add_subregion(get_system_memory(),UINT64_C(0x64de0000),&s->ims_nv_ram);
            error_report("MARCONI_IMS_NV_RAM base=64de0000 bytes=10000 exact-original-mmap-record=true "
                         "independent-volatile-RAM=true virtual-initialzero=true no-IMS-peer-or-work=true");
            error_report("MARCONI_IPC_NV_RAM base=64d70000 bytes=70000 exact-original-mmap-record=true "
                         "independent-volatile-RAM=true virtual-initialzero=true guest-memset-unmodified=true no-storage-data-or-peer-ACK=true");
            error_report("MARCONI_AP_UNCACHE_RAM_PROFILE exact-original-mmap-record=true independent-volatile-5MiB=true "
                         "no-AP-TEE-or-secure-alias=true virtual-initial-zero-not-physical-POR=true CPU-heap-init-unmodified=true");
        }
        /* Exact mmap dfx_info record +0x440: AP startup clears this RAM.
         * No adjacent efuse/boot-ROM-produced data is synthesized. */
        memory_region_init_ram(&s->dfx_ram, NULL, "marconi.dfx-info-ram",
                               0x800, &error_fatal);
        memory_region_init_alias(&s->dfx_view,OBJECT(machine),"marconi.dfx-view",
                                 &s->dfx_ram,0x200,0x600);
        memory_region_add_subregion(get_system_memory(), 0x22003200, &s->dfx_view);
        s->dfx_exit.notify=marconi_dfx_exit;qemu_add_exit_notifier(&s->dfx_exit);
        if (s->tee_reset) {
            memory_region_init_alias(&s->dfx_alias, OBJECT(machine), "marconi.dfx-secure",
                                     &s->dfx_ram, 0x200, 0x600);
            memory_region_add_subregion(get_system_memory(), 0x32003200, &s->dfx_alias);
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
    if (!s->tee_reset) {
        qdev_prop_set_uint32(cpu, "init-nsvtor", vtor);
    }
    /* TEE runs its original Secure reset. NS VTOR stays at architectural
     * reset; original AP reset must eventually initialize it after BLXNS.
     * Optional tee-ap-image adds bytes/RAM only. The original firmware must
     * reach BLXNS and initialize MSP_NS/VTOR_NS. No handoff shortcut, register
     * patch, hardware-lock preseed or forced nonsecure IDAU is installed.
     * The opt-in SBL argument initializes R0 only before the original TEE reset. */
    qdev_connect_clock_in(cpu, "cpuclk", s->cpuclk);
    object_property_set_link(OBJECT(cpu), "memory", OBJECT(get_system_memory()),
                             &error_fatal);
    sysbus_realize(SYS_BUS_DEVICE(cpu), &error_fatal);
    if(s->normal_emmc_profile) {
        if(!s->cp_image_ram_created){error_report("CP cold model requires original CP RAM owners");exit(EXIT_FAILURE);}
        memory_region_init(&s->cp_container,OBJECT(machine),"marconi.cp-private-view",UINT64_MAX);
        memory_region_init_alias(&s->cp_system_alias,OBJECT(machine),"marconi.cp-system-view",get_system_memory(),0,UINT64_MAX);
        memory_region_add_subregion(&s->cp_container,0,&s->cp_system_alias);
        memory_region_init_alias(&s->cp_boot_low,OBJECT(machine),"marconi.cp-original-reset",&s->cp_boot_ram,0,0x400);
        memory_region_add_subregion_overlap(&s->cp_container,0,&s->cp_boot_low,1);
        memory_region_init_ram(&s->cp_atcm,NULL,"marconi.cp-atcm",0x40000,&error_fatal);
        memory_region_set_enabled(&s->cp_atcm,false);
        memory_region_add_subregion_overlap(&s->cp_container,0,&s->cp_atcm,2);
        object_initialize_child(OBJECT(machine),"cp-cluster",&s->cp_cluster,TYPE_CPU_CLUSTER);
        qdev_prop_set_uint32(DEVICE(&s->cp_cluster),"cluster-id",1);
        object_initialize_child(OBJECT(&s->cp_cluster),"cp-cpu",&s->cp_cpu,ARM_CPU_TYPE_NAME("cortex-r5"));
        define_arm_cp_regs_with_opaque(&s->cp_cpu,marconi_cp_atcm_regs,s);
        object_property_set_bool(OBJECT(&s->cp_cpu),"start-powered-off",true,&error_fatal);
        object_property_set_int(OBJECT(&s->cp_cpu),"mp-affinity",0x100,&error_fatal);
        object_property_set_int(OBJECT(&s->cp_cpu),"core-count",1,&error_fatal);
        object_property_set_link(OBJECT(&s->cp_cpu),"memory",OBJECT(&s->cp_container),&error_fatal);
        qdev_realize(DEVICE(&s->cp_cluster),NULL,&error_fatal);
        qdev_realize(DEVICE(&s->cp_cpu),NULL,&error_fatal);
        ARMCPRegInfo tcmtr={.name="TCMTR",.cp=15,.opc1=0,.crn=0,.crm=0,.opc2=2,
            .access=PL1_R,.type=ARM_CP_CONST|ARM_CP_OVERRIDE,.resetvalue=0x00010001};
        define_one_arm_cp_reg_with_opaque(&s->cp_cpu,&tcmtr,s);
        s->cp_atcm_exit.notify=marconi_cp_atcm_exit;qemu_add_exit_notifier(&s->cp_atcm_exit);
        s->cp_l2=qdev_new("l2x0");
        sysbus_realize_and_unref(SYS_BUS_DEVICE(s->cp_l2),&error_fatal);
        s->cp_l2_mmio=sysbus_mmio_get_region(SYS_BUS_DEVICE(s->cp_l2),0);
        qemu_register_reset(marconi_cp_l2_reset,s);
        qemu_register_reset(marconi_spmi_irq_reset,s);
        qemu_register_reset(marconi_cp_axidma_reset,s);
        marconi_cp_sim_config_reset(s);qemu_register_reset(marconi_cp_sim_config_reset,s);
        marconi_cp_eac_reset(s);qemu_register_reset(marconi_cp_eac_reset,s);
        qemu_register_reset(marconi_cp_control398_reset,s);
        qemu_register_reset(marconi_cp_domain_qos_reset,s);
        s->cp_wdt_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_cp_wdt_callback,s);
        marconi_cp_wdt_reset(s);qemu_register_reset(marconi_cp_wdt_reset,s);
        s->cp_wdt_exit.notify=marconi_cp_wdt_exit;qemu_add_exit_notifier(&s->cp_wdt_exit);
        qemu_register_reset(marconi_cp_dfs_request_reset,s);
        qemu_register_reset(marconi_cp_clock_init_reset,s);
        qemu_register_reset(marconi_cp_intc_config_reset,s);
        s->cp_intc_config_exit.notify=marconi_cp_intc_config_exit;qemu_add_exit_notifier(&s->cp_intc_config_exit);
        s->cp_rfspim_emitted=g_new0(uint32_t,MARCONI_RFSPIM_RECORD_WORDS);
        s->cp_rfspim_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_rfspim_callback,s);
        marconi_cp_ssi_reset(s);
        qemu_register_reset(marconi_cp_ssi_reset,s);
        qemu_register_reset(marconi_cp_aon_reset,s);
        s->cp_aon_exit.notify=marconi_cp_aon_exit;qemu_add_exit_notifier(&s->cp_aon_exit);
        s->cp_ssi_exit.notify=marconi_cp_ssi_exit;qemu_add_exit_notifier(&s->cp_ssi_exit);
        s->bbpll_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_bbpll_callback,s);
        qemu_register_reset(marconi_bbpll_reset,s);
        s->bbpll_exit.notify=marconi_bbpll_exit;qemu_add_exit_notifier(&s->bbpll_exit);
        s->cp_clock_init_exit.notify=marconi_cp_clock_init_exit;qemu_add_exit_notifier(&s->cp_clock_init_exit);
        s->cp_dfs_request_exit.notify=marconi_cp_dfs_request_exit;qemu_add_exit_notifier(&s->cp_dfs_request_exit);
        s->spmi_irq_exit.notify=marconi_spmi_irq_exit;qemu_add_exit_notifier(&s->spmi_irq_exit);
        s->cp_axidma_exit.notify=marconi_cp_axidma_exit;qemu_add_exit_notifier(&s->cp_axidma_exit);
        s->cp_sim_config_exit.notify=marconi_cp_sim_config_exit;qemu_add_exit_notifier(&s->cp_sim_config_exit);
        s->cp_eac_exit.notify=marconi_cp_eac_exit;qemu_add_exit_notifier(&s->cp_eac_exit);
        s->cp_control398_exit.notify=marconi_cp_control398_exit;qemu_add_exit_notifier(&s->cp_control398_exit);
        s->cp_domain_qos_exit.notify=marconi_cp_domain_qos_exit;qemu_add_exit_notifier(&s->cp_domain_qos_exit);
        s->cp_l2_exit.notify=marconi_cp_l2_exit;qemu_add_exit_notifier(&s->cp_l2_exit);
        s->cp_timer_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_cp_timer_callback,s);
        s->cp_timer_created=true;marconi_cp_timer_reset(s);qemu_register_reset(marconi_cp_timer_reset,s);
        s->cp_timer_exit.notify=marconi_cp_timer_exit;qemu_add_exit_notifier(&s->cp_timer_exit);
        marconi_cp_uart2_reset(s);qemu_register_reset(marconi_cp_uart2_reset,s);
        s->cp_uart2_exit.notify=marconi_cp_uart2_exit;qemu_add_exit_notifier(&s->cp_uart2_exit);
        s->cp_created=true;s->cp_cold_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_cp_cold_callback,s);
        s->cp_ports_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_cp_ports_callback,s);
        qemu_register_reset(marconi_cp_ports_reset,s);s->cp_ports_exit.notify=marconi_cp_ports_exit;qemu_add_exit_notifier(&s->cp_ports_exit);
        qemu_register_reset(marconi_cp_cold_reset,s);s->cp_cold_exit.notify=marconi_cp_cold_exit;qemu_add_exit_notifier(&s->cp_cold_exit);
        memory_region_init(&s->crf_container,OBJECT(machine),"marconi.crf-private",UINT64_C(0x100000000));
        memory_region_init_io(&s->crf_missing,OBJECT(machine),&marconi_crf_ops,s,"marconi.crf-ports",UINT64_C(0x100000000));
        memory_region_add_subregion(&s->crf_container,0,&s->crf_missing);
        memory_region_init_ram(&s->crf_ram,NULL,"marconi.crf-downloaded-ram",0xc000,&error_fatal);
        memory_region_add_subregion_overlap(&s->crf_container,0,&s->crf_ram,1);
        address_space_init(&s->crf_as,&s->crf_container,"marconi.crf-transport");
        s->crf_clock=clock_new(OBJECT(machine),"crf-clock");clock_set_hz(s->crf_clock,26000000);
        object_initialize_child(OBJECT(machine),"crf-armv7m",&s->crf_armv7m,TYPE_ARMV7M);
        DeviceState *crf=DEVICE(&s->crf_armv7m);
        qdev_prop_set_string(crf,"cpu-type",TYPE_MARCONI_CRF_CPU);
        qdev_prop_set_uint32(crf,"num-irq",32);object_property_set_bool(OBJECT(crf),"start-powered-off",true,&error_fatal);
        qdev_connect_clock_in(crf,"cpuclk",s->crf_clock);
        object_property_set_link(OBJECT(crf),"memory",OBJECT(&s->crf_container),&error_fatal);
        sysbus_realize(SYS_BUS_DEVICE(crf),&error_fatal);s->crf_created=true;
        qemu_register_reset(marconi_pmic_config1328_reset,s);
        s->pmic_config1328_exit.notify=marconi_pmic_config1328_exit;qemu_add_exit_notifier(&s->pmic_config1328_exit);
        qemu_register_reset(marconi_crf_reset,s);
        s->crf_exit.notify=marconi_crf_exit;qemu_add_exit_notifier(&s->crf_exit);
        marconi_public_report("MARCONI_CP_CORE_CREATED virtual-ARMv7R=cortex-r5 cluster=1 virtual-affinity=100 powered-off=true original-reset0-alias=true CPU-register-patch=false");
    }
    if(!marconi_timer0_runtime_refresh(s)) {exit(EXIT_FAILURE);}
    memory_region_init_io(&s->memsysctl_guard, OBJECT(machine),
                          &marconi_memsysctl_ops, s,
                          "marconi-unimplemented-m55-memsysctl", 0x1000);
    memory_region_add_subregion_overlap(&s->armv7m.container, 0xe001e000,
                                       &s->memsysctl_guard, 1);
    if(s->pmu_init_fixture) {
        memory_region_init_io(&s->pmu_guard,OBJECT(machine),&marconi_pmu_ops,s,
                              "marconi-pmu-initialization-subset",0x1000);
        memory_region_add_subregion_overlap(&s->armv7m.container,0xe0003000,
                                           &s->pmu_guard,1);
    }
    armv7m_load_kernel(s->armv7m.cpu, machine->kernel_filename,
                       load_base, ram_bytes);
    if(s->auxadc_channel_profile)qemu_register_reset(marconi_auxadc_callback,s);
    if(s->timer4014_inputs) {
        /* Registered after CPU/kernel reset setup: drive explicit pending
         * lines at initial reset, without editing CPU state or fabricating
         * a hardware reset/retention sequence. This only refreshes IRQ lines. */
        qemu_register_reset(marconi_timer4014_callback,s);
    }
    if(s->tee_sbl_argument_fixture) qemu_register_reset(marconi_tee_sbl_argument_reset,s);
    if(s->ota_sbl_argument_fixture) qemu_register_reset(marconi_ota_sbl_argument_reset,s);
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
    if(s->thermal_mode0_profile)marconi_thermal_mode0_create(s);
    if(s->normal_emmc_profile)marconi_normal_emmc_create(s);
    if(s->i2c_nopeer_profile)marconi_i2c_nopeer_create(s);
    s->research_deadline = timer_new_ms(QEMU_CLOCK_REALTIME,
                                      marconi_research_deadline, s);
    timer_mod(s->research_deadline, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 600000);
    error_report("MARCONI_PROBE_RESET core=cortex-m55 stage=%s vtor=0x%08x "
                 "ram=0x%08" HWADDR_PRIx "+0x%08" HWADDR_PRIx " irq-lines=149 "
                 "counter32k=%s uart16550=%s uart-dw-config=%s "
                 "uart-external-irq=57 uart-data-transfer=%s "
                 "other-peripherals=unimplemented "
                 "full-watch=false", stage, vtor,
                 load_base, ram_bytes, s->counter32k ? "on" : "off",
                 s->uart16550 ? "initialization-subset" : "off",
                 s->uart_dw_config ? "on" : "off",
                 s->uart_tx_fixture ? "fixture-timed-tx" : "unsupported");
}

static char *marconi_get_normal_emmc_tuning_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->normal_emmc_tuning_profile); }
static void marconi_set_normal_emmc_tuning_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->normal_emmc_tuning_profile);s->normal_emmc_tuning_profile=g_strdup(value); }
static char *marconi_get_normal_emmc_fixed_supply(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->normal_emmc_fixed_supply); }
static void marconi_set_normal_emmc_fixed_supply(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->normal_emmc_fixed_supply);s->normal_emmc_fixed_supply=g_strdup(value); }
static char *marconi_get_normal_emmc_fault_test(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->normal_emmc_fault_test); }
static void marconi_set_normal_emmc_fault_test(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->normal_emmc_fault_test);s->normal_emmc_fault_test=g_strdup(value); }
static char *marconi_get_normal_emmc_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->normal_emmc_profile); }
static void marconi_set_normal_emmc_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->normal_emmc_profile);s->normal_emmc_profile=g_strdup(value); }
static char *marconi_get_lcd_frame_file(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->lcd_frame_file); }
static void marconi_set_lcd_frame_file(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->lcd_frame_file);s->lcd_frame_file=g_strdup(value); }
static char *marconi_get_normal_emmc_writeback(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->normal_emmc_writeback); }
static void marconi_set_normal_emmc_writeback(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->normal_emmc_writeback);s->normal_emmc_writeback=g_strdup(value); }
static char *marconi_get_normal_emmc_media(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->normal_emmc_media); }
static void marconi_set_normal_emmc_media(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->normal_emmc_media);s->normal_emmc_media=g_strdup(value); }
static char *marconi_get_ap_soc4005_2c0_initial(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ap_soc4005_2c0_initial); }
static void marconi_set_ap_soc4005_2c0_initial(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->ap_soc4005_2c0_initial);s->ap_soc4005_2c0_initial=g_strdup(value); }
static char *marconi_get_soc4005_config_inputs(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->soc4005_config_inputs); }
static void marconi_set_soc4005_config_inputs(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->soc4005_config_inputs);s->soc4005_config_inputs=g_strdup(value);
}

static char *marconi_get_pmic_regulator_inputs(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pmic_regulator_inputs); }
static void marconi_set_pmic_regulator_inputs(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->pmic_regulator_inputs);s->pmic_regulator_inputs=g_strdup(value);
}

static char *marconi_get_pmic_clock_inputs(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pmic_clock_inputs); }
static void marconi_set_pmic_clock_inputs(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->pmic_clock_inputs);s->pmic_clock_inputs=g_strdup(value);
}

static char *marconi_get_pmic1000_initial(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pmic1000_initial); }
static void marconi_set_pmic1000_initial(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->pmic1000_initial);s->pmic1000_initial=g_strdup(value);
}

static char *marconi_get_timer4014_inputs(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->timer4014_inputs); }
static void marconi_set_timer4014_inputs(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->timer4014_inputs);s->timer4014_inputs=g_strdup(value);
}
static bool marconi_get_timer4014_load_plus_one(Object *obj,Error **errp)
{ return ((MarconiProbeState *)obj)->timer4014_load_plus_one; }
static void marconi_set_timer4014_load_plus_one(Object *obj,bool value,Error **errp)
{ ((MarconiProbeState *)obj)->timer4014_load_plus_one=value; }

static char *marconi_get_ota_config_body_inputs(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ota_config_body_inputs); }
static void marconi_set_ota_config_body_inputs(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->ota_config_body_inputs);s->ota_config_body_inputs=g_strdup(value);
}

static bool marconi_get_ota_config_write_observations(Object *obj,Error **errp)
{ return ((MarconiProbeState *)obj)->ota_config_write_observations; }
static void marconi_set_ota_config_write_observations(Object *obj,bool value,Error **errp)
{ ((MarconiProbeState *)obj)->ota_config_write_observations=value; }

static char *marconi_get_ota_retained_hwlock_image(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ota_retained_hwlock_image); }
static void marconi_set_ota_retained_hwlock_image(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->ota_retained_hwlock_image);s->ota_retained_hwlock_image=g_strdup(value);
}

static char *marconi_get_ota_retained_footer_image(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ota_retained_footer_image); }
static void marconi_set_ota_retained_footer_image(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->ota_retained_footer_image);s->ota_retained_footer_image=g_strdup(value);
}

static char *marconi_get_ota_invalid_clock_marker_fixture(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ota_invalid_clock_marker_fixture); }
static void marconi_set_ota_invalid_clock_marker_fixture(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->ota_invalid_clock_marker_fixture);s->ota_invalid_clock_marker_fixture=g_strdup(value);
}

static bool marconi_get_ota_reset(Object *obj,Error **errp)
{ return ((MarconiProbeState *)obj)->ota_reset; }
static void marconi_set_ota_reset(Object *obj,bool value,Error **errp)
{ ((MarconiProbeState *)obj)->ota_reset=value; }
static char *marconi_get_ota_sbl_argument_fixture(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ota_sbl_argument_fixture); }
static void marconi_set_ota_sbl_argument_fixture(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->ota_sbl_argument_fixture);s->ota_sbl_argument_fixture=g_strdup(value);
}

static bool marconi_get_tee_reset(Object *obj, Error **errp)
{
    return ((MarconiProbeState *)obj)->tee_reset;
}

static void marconi_set_tee_reset(Object *obj, bool value, Error **errp)
{
    ((MarconiProbeState *)obj)->tee_reset = value;
}

static bool marconi_get_sbl_reset(Object *obj, Error **errp)
{
    return ((MarconiProbeState *)obj)->sbl_reset;
}

static char *marconi_get_tee_ap_image(Object *obj, Error **errp)
{
    return g_strdup(((MarconiProbeState *)obj)->tee_ap_image);
}

static void marconi_set_tee_ap_image(Object *obj, const char *value, Error **errp)
{
    MarconiProbeState *s = (MarconiProbeState *)obj;
    g_free(s->tee_ap_image);
    s->tee_ap_image = g_strdup(value);
}

static bool marconi_get_ap_boot_buffer_enabled(Object *obj,Error **errp)
{ return ((MarconiProbeState *)obj)->ap_boot_buffer_enabled; }
static void marconi_set_ap_boot_buffer_enabled(Object *obj,bool value,Error **errp)
{ ((MarconiProbeState *)obj)->ap_boot_buffer_enabled=value; }
static char *marconi_get_ap_uncache_ram_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ap_uncache_ram_profile); }
static void marconi_set_ap_uncache_ram_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->ap_uncache_ram_profile);s->ap_uncache_ram_profile=g_strdup(value); }
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

static char *marconi_get_explore_seed(Object *obj, Error **errp)
{
    return g_strdup(((MarconiProbeState *)obj)->explore_seed);
}
static void marconi_set_explore_seed(Object *obj, const char *value, Error **errp)
{
    MarconiProbeState *s = (MarconiProbeState *)obj;
    g_free(s->explore_seed);
    s->explore_seed = g_strdup(value);
}
static char *marconi_get_explore_latency(Object *obj, Error **errp)
{
    return g_strdup(((MarconiProbeState *)obj)->explore_latency);
}
static void marconi_set_explore_latency(Object *obj, const char *value, Error **errp)
{
    MarconiProbeState *s = (MarconiProbeState *)obj;
    g_free(s->explore_latency);
    s->explore_latency = g_strdup(value);
}

static char *marconi_get_adc_fixture(Object *obj, Error **errp)
{
    return g_strdup(((MarconiProbeState *)obj)->adc_fixture);
}
static void marconi_set_adc_fixture(Object *obj, const char *value, Error **errp)
{
    MarconiProbeState *s = (MarconiProbeState *)obj;
    g_free(s->adc_fixture);
    s->adc_fixture = g_strdup(value);
}

static char *marconi_get_uart_tx_fixture(Object *obj, Error **errp)
{
    return g_strdup(((MarconiProbeState *)obj)->uart_tx_fixture);
}
static void marconi_set_uart_tx_fixture(Object *obj, const char *value, Error **errp)
{
    MarconiProbeState *s = (MarconiProbeState *)obj;
    g_free(s->uart_tx_fixture);
    s->uart_tx_fixture = g_strdup(value);
}

static char *marconi_get_i2c_nopeer_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->i2c_nopeer_profile); }
static void marconi_set_i2c_nopeer_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->i2c_nopeer_profile);s->i2c_nopeer_profile=g_strdup(value); }
static char *marconi_get_ap_i2c_empty_config(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ap_i2c_empty_config); }
static void marconi_set_ap_i2c_empty_config(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->ap_i2c_empty_config);s->ap_i2c_empty_config=g_strdup(value); }
static char *marconi_get_ap_i2c_soc_commands(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ap_i2c_soc_commands); }
static void marconi_set_ap_i2c_soc_commands(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->ap_i2c_soc_commands);s->ap_i2c_soc_commands=g_strdup(value); }
static char *marconi_get_wdt4012_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->wdt4012_profile); }
static void marconi_set_wdt4012_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->wdt4012_profile);s->wdt4012_profile=g_strdup(value); }
static char *marconi_get_ap_watchdog_init(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ap_watchdog_init); }
static void marconi_set_ap_watchdog_init(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->ap_watchdog_init);s->ap_watchdog_init=g_strdup(value); }
static char *marconi_get_ap_spi_config(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ap_spi_config); }
static void marconi_set_ap_spi_config(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->ap_spi_config);s->ap_spi_config=g_strdup(value); }
static char *marconi_get_ap_gpio_config(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ap_gpio_config); }
static void marconi_set_ap_gpio_config(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->ap_gpio_config);s->ap_gpio_config=g_strdup(value); }
static char *marconi_get_ap_te_host1_config(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ap_te_host1_config); }
static void marconi_set_ap_te_host1_config(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->ap_te_host1_config);s->ap_te_host1_config=g_strdup(value); }
static char *marconi_get_ap_te_platform_view(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ap_te_platform_view); }
static void marconi_set_ap_te_platform_view(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->ap_te_platform_view);s->ap_te_platform_view=g_strdup(value); }
static char *marconi_get_ap_timer64_view(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ap_timer64_view); }
static void marconi_set_ap_timer64_view(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->ap_timer64_view);s->ap_timer64_view=g_strdup(value); }
static bool marconi_get_timer64(Object *obj, Error **errp)
{
    return ((MarconiProbeState *)obj)->timer64;
}
static void marconi_set_timer64(Object *obj, bool value, Error **errp)
{
    ((MarconiProbeState *)obj)->timer64 = value;
}

static bool marconi_get_qos_observed_write_experiment(Object *obj, Error **errp)
{
    return ((MarconiProbeState *)obj)->qos_observed_write_experiment;
}

static char *marconi_get_lowpage_response_experiment(Object *obj, Error **errp)
{
    return g_strdup(((MarconiProbeState *)obj)->lowpage_response_experiment);
}
static void marconi_set_lowpage_response_experiment(Object *obj, const char *value, Error **errp)
{
    MarconiProbeState *s = (MarconiProbeState *)obj;
    g_free(s->lowpage_response_experiment);
    s->lowpage_response_experiment = g_strdup(value);
}
static void marconi_set_qos_observed_write_experiment(Object *obj, bool value, Error **errp)
{
    ((MarconiProbeState *)obj)->qos_observed_write_experiment = value;
}

static char *marconi_get_dfs_config_fixture(Object *obj, Error **errp)
{
    return g_strdup(((MarconiProbeState *)obj)->dfs_config_fixture);
}

static void marconi_set_dfs_config_fixture(Object *obj, const char *value, Error **errp)
{
    MarconiProbeState *s = (MarconiProbeState *)obj;
    g_free(s->dfs_config_fixture);
    s->dfs_config_fixture = g_strdup(value);
}

static char *marconi_get_clock48_fixture(Object *obj, Error **errp)
{
    return g_strdup(((MarconiProbeState *)obj)->clock48_fixture);
}

static void marconi_set_clock48_fixture(Object *obj, const char *value, Error **errp)
{
    MarconiProbeState *s = (MarconiProbeState *)obj;
    g_free(s->clock48_fixture);
    s->clock48_fixture = g_strdup(value);
}

static char *marconi_get_timer0_runtime_fixture(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->timer0_runtime_fixture); }
static void marconi_set_timer0_runtime_fixture(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->timer0_runtime_fixture);s->timer0_runtime_fixture=g_strdup(value);
}
static bool marconi_get_timer0_one_shot_experiment(Object *obj,Error **errp)
{ return ((MarconiProbeState *)obj)->timer0_one_shot_experiment; }
static void marconi_set_timer0_one_shot_experiment(Object *obj,bool value,Error **errp)
{ ((MarconiProbeState *)obj)->timer0_one_shot_experiment=value; }

static char *marconi_get_timer0_stop_fixture(Object *obj, Error **errp)
{
    return g_strdup(((MarconiProbeState *)obj)->timer0_stop_fixture);
}

static void marconi_set_timer0_stop_fixture(Object *obj, const char *value, Error **errp)
{
    MarconiProbeState *s = (MarconiProbeState *)obj;
    g_free(s->timer0_stop_fixture);
    s->timer0_stop_fixture = g_strdup(value);
}

static char *marconi_get_aon144_read_fixture(Object *obj, Error **errp)
{
    return g_strdup(((MarconiProbeState *)obj)->aon144_read_fixture);
}

static void marconi_set_aon144_read_fixture(Object *obj, const char *value, Error **errp)
{
    MarconiProbeState *s = (MarconiProbeState *)obj;
    g_free(s->aon144_read_fixture);
    s->aon144_read_fixture = g_strdup(value);
}

static char *marconi_get_aon_prediv_query_fixture(Object *obj, Error **errp)
{
    return g_strdup(((MarconiProbeState *)obj)->aon_prediv_query_fixture);
}

static void marconi_set_aon_prediv_query_fixture(Object *obj, const char *value, Error **errp)
{
    MarconiProbeState *s = (MarconiProbeState *)obj;
    g_free(s->aon_prediv_query_fixture);
    s->aon_prediv_query_fixture = g_strdup(value);
}

static char *marconi_get_clock_controls_fixture(Object *obj, Error **errp)
{
    return g_strdup(((MarconiProbeState *)obj)->clock_controls_fixture);
}

static void marconi_set_clock_controls_fixture(Object *obj, const char *value, Error **errp)
{
    MarconiProbeState *s = (MarconiProbeState *)obj;
    g_free(s->clock_controls_fixture);
    s->clock_controls_fixture = g_strdup(value);
}

static char *marconi_get_ap_domain_status_fixture(Object *obj, Error **errp)
{
    return g_strdup(((MarconiProbeState *)obj)->ap_domain_status_fixture);
}

static void marconi_set_ap_domain_status_fixture(Object *obj, const char *value, Error **errp)
{
    MarconiProbeState *s = (MarconiProbeState *)obj;
    g_free(s->ap_domain_status_fixture);
    s->ap_domain_status_fixture = g_strdup(value);
}

static bool marconi_get_dtgu_lut0_observed_write(Object *obj, Error **errp)
{
    return ((MarconiProbeState *)obj)->dtgu_lut0_observed_write;
}

static void marconi_set_dtgu_lut0_observed_write(Object *obj, bool value, Error **errp)
{
    ((MarconiProbeState *)obj)->dtgu_lut0_observed_write = value;
}

static char *marconi_get_board_controls_fixture(Object *obj, Error **errp)
{
    return g_strdup(((MarconiProbeState *)obj)->board_controls_fixture);
}

static void marconi_set_board_controls_fixture(Object *obj, const char *value, Error **errp)
{
    MarconiProbeState *s = (MarconiProbeState *)obj;
    g_free(s->board_controls_fixture);
    s->board_controls_fixture = g_strdup(value);
}

static char *marconi_get_resource_attribute_fixture(Object *obj, Error **errp)
{
    return g_strdup(((MarconiProbeState *)obj)->resource_attribute_fixture);
}

static void marconi_set_resource_attribute_fixture(Object *obj, const char *value, Error **errp)
{
    MarconiProbeState *s = (MarconiProbeState *)obj;
    g_free(s->resource_attribute_fixture);
    s->resource_attribute_fixture = g_strdup(value);
}

static char *marconi_get_range_configuration_fixture(Object *obj, Error **errp)
{
    return g_strdup(((MarconiProbeState *)obj)->range_configuration_fixture);
}

static void marconi_set_range_configuration_fixture(Object *obj, const char *value, Error **errp)
{
    MarconiProbeState *s = (MarconiProbeState *)obj;
    g_free(s->range_configuration_fixture);
    s->range_configuration_fixture = g_strdup(value);
}

static char *marconi_get_dvfs_vote_config(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->dvfs_vote_config); }
static void marconi_set_dvfs_vote_config(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->dvfs_vote_config);s->dvfs_vote_config=g_strdup(value); }
static char *marconi_get_ap_pll_rate_view(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ap_pll_rate_view); }
static void marconi_set_ap_pll_rate_view(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->ap_pll_rate_view);s->ap_pll_rate_view=g_strdup(value); }
static char *marconi_get_dvfs_main_clock_initial(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->dvfs_main_clock_initial); }
static void marconi_set_dvfs_main_clock_initial(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->dvfs_main_clock_initial);s->dvfs_main_clock_initial=g_strdup(value); }
static char *marconi_get_thermal_mode0_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->thermal_mode0_profile); }
static void marconi_set_thermal_mode0_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->thermal_mode0_profile);s->thermal_mode0_profile=g_strdup(value); }
static char *marconi_get_ap_aon_iosleep_config(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ap_aon_iosleep_config); }
static void marconi_set_ap_aon_iosleep_config(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->ap_aon_iosleep_config);s->ap_aon_iosleep_config=g_strdup(value); }
static char *marconi_get_ap_thermal_config_view(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ap_thermal_config_view); }
static void marconi_set_ap_thermal_config_view(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->ap_thermal_config_view);s->ap_thermal_config_view=g_strdup(value); }
static char *marconi_get_ap_efuse_read_view(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ap_efuse_read_view); }
static void marconi_set_ap_efuse_read_view(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->ap_efuse_read_view);s->ap_efuse_read_view=g_strdup(value); }
static char *marconi_get_auxadc_channel_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->auxadc_channel_profile); }
static void marconi_set_auxadc_channel_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->auxadc_channel_profile);s->auxadc_channel_profile=g_strdup(value); }
static char *marconi_get_timer64_terminal_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->timer64_terminal_profile); }
static void marconi_set_timer64_terminal_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->timer64_terminal_profile);s->timer64_terminal_profile=g_strdup(value); }
static char *marconi_get_pmic_voltage_source_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pmic_voltage_source_profile); }
static void marconi_set_pmic_voltage_source_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->pmic_voltage_source_profile);s->pmic_voltage_source_profile=g_strdup(value); }
static char *marconi_get_pmic_boot_events_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pmic_boot_events_profile); }
static void marconi_set_pmic_boot_events_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->pmic_boot_events_profile);s->pmic_boot_events_profile=g_strdup(value); }
static char *marconi_get_pmic_vbus_input(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pmic_vbus_input); }
static void marconi_set_pmic_vbus_input(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->pmic_vbus_input);s->pmic_vbus_input=g_strdup(value); }
static char *marconi_get_adsp_publication_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->adsp_publication_profile); }
static void marconi_set_adsp_publication_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->adsp_publication_profile);s->adsp_publication_profile=g_strdup(value); }
static char *marconi_get_ap_mailbox_config(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ap_mailbox_config); }
static void marconi_set_ap_mailbox_config(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->ap_mailbox_config);s->ap_mailbox_config=g_strdup(value); }
static char *marconi_get_venc_device_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->venc_device_profile); }
static void marconi_set_venc_device_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->venc_device_profile);s->venc_device_profile=g_strdup(value); }
static char *marconi_get_vdec_device_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->vdec_device_profile); }
static void marconi_set_vdec_device_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->vdec_device_profile);s->vdec_device_profile=g_strdup(value); }
static char *marconi_get_gpu_boot_identity_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->gpu_boot_identity_profile); }
static void marconi_set_gpu_boot_identity_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->gpu_boot_identity_profile);s->gpu_boot_identity_profile=g_strdup(value); }
static char *marconi_get_gpu_boot_reset_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->gpu_boot_reset_profile); }
static void marconi_set_gpu_boot_reset_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->gpu_boot_reset_profile);s->gpu_boot_reset_profile=g_strdup(value); }
static char *marconi_get_pmic_sync107c_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pmic_sync107c_profile); }
static void marconi_set_pmic_sync107c_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->pmic_sync107c_profile);s->pmic_sync107c_profile=g_strdup(value); }
static char *marconi_get_pmic_button_config_initial(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pmic_button_config_initial); }
static void marconi_set_pmic_button_config_initial(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->pmic_button_config_initial);s->pmic_button_config_initial=g_strdup(value); }
static char *marconi_get_pmic_alias_read_policy(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pmic_alias_read_policy); }
static void marconi_set_pmic_alias_read_policy(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->pmic_alias_read_policy);s->pmic_alias_read_policy=g_strdup(value); }
static char *marconi_get_pmic_mode_config_inputs(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pmic_mode_config_inputs); }
static void marconi_set_pmic_mode_config_inputs(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->pmic_mode_config_inputs);s->pmic_mode_config_inputs=g_strdup(value); }
static char *marconi_get_virtual_pmic_bc_config(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->virtual_pmic_bc_config); }
static void marconi_set_virtual_pmic_bc_config(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->virtual_pmic_bc_config);s->virtual_pmic_bc_config=g_strdup(value); }
static char *marconi_get_virtual_auxadc_reset_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->virtual_auxadc_reset_profile); }
static void marconi_set_virtual_auxadc_reset_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->virtual_auxadc_reset_profile);s->virtual_auxadc_reset_profile=g_strdup(value); }
static char *marconi_get_virtual_dvs_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->virtual_dvs_profile); }
static void marconi_set_virtual_dvs_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->virtual_dvs_profile);s->virtual_dvs_profile=g_strdup(value); }
static char *marconi_get_aon120_config_inputs(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->aon120_config_inputs); }
static void marconi_set_aon120_config_inputs(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->aon120_config_inputs);s->aon120_config_inputs=g_strdup(value);
}

static char *marconi_get_pm_bit9_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pm_bit9_profile); }
static void marconi_set_pm_bit9_profile(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->pm_bit9_profile);s->pm_bit9_profile=g_strdup(value);
}
static char *marconi_get_pm_bit8_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pm_bit8_profile); }
static void marconi_set_pm_bit8_profile(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->pm_bit8_profile);s->pm_bit8_profile=g_strdup(value);
}
static char *marconi_get_vpu_clock_inputs(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->vpu_clock_inputs); }
static void marconi_set_vpu_clock_inputs(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->vpu_clock_inputs);s->vpu_clock_inputs=g_strdup(value);
}
static char *marconi_get_usb_clock_inputs(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->usb_clock_inputs); }
static void marconi_set_usb_clock_inputs(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->usb_clock_inputs);s->usb_clock_inputs=g_strdup(value);
}

static char *marconi_get_mipi_clock_inputs(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->mipi_clock_inputs); }
static void marconi_set_mipi_clock_inputs(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->mipi_clock_inputs);s->mipi_clock_inputs=g_strdup(value);
}

static char *marconi_get_pubap_clock_inputs(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pubap_clock_inputs); }
static void marconi_set_pubap_clock_inputs(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->pubap_clock_inputs);s->pubap_clock_inputs=g_strdup(value);
}

static char *marconi_get_pubcp_clock_inputs(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pubcp_clock_inputs); }
static void marconi_set_pubcp_clock_inputs(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->pubcp_clock_inputs);s->pubcp_clock_inputs=g_strdup(value);
}

static char *marconi_get_pm_domain4_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pm_domain4_profile); }
static void marconi_set_pm_domain4_profile(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->pm_domain4_profile);s->pm_domain4_profile=g_strdup(value);
}

static char *marconi_get_pm_domain5_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pm_domain5_profile); }
static void marconi_set_pm_domain5_profile(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->pm_domain5_profile);s->pm_domain5_profile=g_strdup(value);
}

static char *marconi_get_pm_domain7_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pm_domain7_profile); }
static void marconi_set_pm_domain7_profile(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->pm_domain7_profile);s->pm_domain7_profile=g_strdup(value);
}

static char *marconi_get_dpu_initial_b0(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->dpu_initial_b0); }
static void marconi_set_dpu_initial_b0(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->dpu_initial_b0);s->dpu_initial_b0=g_strdup(value);
}

static char *marconi_get_ap_runtime_config(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ap_runtime_config); }
static void marconi_set_ap_runtime_config(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->ap_runtime_config);s->ap_runtime_config=g_strdup(value);
}

static char *marconi_get_ap_board_config(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->ap_board_config); }
static void marconi_set_ap_board_config(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->ap_board_config);s->ap_board_config=g_strdup(value);
}

static char *marconi_get_pm_functional_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pm_functional_profile); }
static void marconi_set_pm_functional_profile(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->pm_functional_profile);s->pm_functional_profile=g_strdup(value);
}

static char *marconi_get_pm_counter_fixture(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pm_counter_fixture); }
static void marconi_set_pm_counter_fixture(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->pm_counter_fixture);s->pm_counter_fixture=g_strdup(value);
}

static char *marconi_get_rtc32k_reference(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->rtc32k_reference_profile); }
static void marconi_set_rtc32k_reference(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->rtc32k_reference_profile);s->rtc32k_reference_profile=g_strdup(value); }
static char *marconi_get_rtc_seconds_fixture(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->rtc_seconds_fixture); }
static void marconi_set_rtc_seconds_fixture(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->rtc_seconds_fixture);s->rtc_seconds_fixture=g_strdup(value);
}

static char *marconi_get_pmu_init_fixture(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pmu_init_fixture); }
static void marconi_set_pmu_init_fixture(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->pmu_init_fixture);s->pmu_init_fixture=g_strdup(value);
}

static char *marconi_get_otp_read_fixture(Object *obj, Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->otp_read_fixture); }
static void marconi_set_otp_read_fixture(Object *obj, const char *value, Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->otp_read_fixture); s->otp_read_fixture=g_strdup(value);
}

static char *marconi_get_tee_sbl_argument_fixture(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->tee_sbl_argument_fixture); }
static void marconi_set_tee_sbl_argument_fixture(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->tee_sbl_argument_fixture);s->tee_sbl_argument_fixture=g_strdup(value);
}

static char *marconi_get_rc_measurement_fixture(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->rc_measurement_fixture); }
static void marconi_set_rc_measurement_fixture(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->rc_measurement_fixture);s->rc_measurement_fixture=g_strdup(value);
}

static bool marconi_get_mbox_outbound_experiment(Object *obj,Error **errp)
{ return ((MarconiProbeState *)obj)->mbox_outbound_experiment; }
static void marconi_set_mbox_outbound_experiment(Object *obj,bool value,Error **errp)
{ ((MarconiProbeState *)obj)->mbox_outbound_experiment=value; }

static char *marconi_get_wdt_policy_fixture(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->wdt_policy_fixture); }
static void marconi_set_wdt_policy_fixture(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->wdt_policy_fixture);s->wdt_policy_fixture=g_strdup(value);
}

static char *marconi_get_trng_virtual_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->trng_virtual_profile); }
static void marconi_set_trng_virtual_profile(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->trng_virtual_profile);s->trng_virtual_profile=g_strdup(value);
}

static char *marconi_get_trng_unkeyed_fixture(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->trng_unkeyed_fixture); }
static void marconi_set_trng_unkeyed_fixture(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->trng_unkeyed_fixture);s->trng_unkeyed_fixture=g_strdup(value);
}

static char *marconi_get_hash_init_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->hash_init_profile); }
static void marconi_set_hash_init_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->hash_init_profile);s->hash_init_profile=g_strdup(value); }

static char *marconi_get_aca_config_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->aca_config_profile); }
static void marconi_set_aca_config_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->aca_config_profile);s->aca_config_profile=g_strdup(value); }

static char *marconi_get_symc_empty_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->symc_empty_profile); }
static void marconi_set_symc_empty_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->symc_empty_profile);s->symc_empty_profile=g_strdup(value); }

static char *marconi_get_rnp_postkey_config(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->rnp_postkey_config); }
static void marconi_set_rnp_postkey_config(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->rnp_postkey_config);s->rnp_postkey_config=g_strdup(value); }

static char *marconi_get_rnp_mask_config(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->rnp_mask_config); }
static void marconi_set_rnp_mask_config(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->rnp_mask_config);s->rnp_mask_config=g_strdup(value); }

static char *marconi_get_virtual_otp_profile(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->virtual_otp_profile); }
static void marconi_set_virtual_otp_profile(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->virtual_otp_profile);s->virtual_otp_profile=g_strdup(value); }
static char *marconi_get_virtual_otp_public_template(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->virtual_otp_public_template); }
static void marconi_set_virtual_otp_public_template(Object *obj,const char *value,Error **errp)
{ MarconiProbeState *s=(MarconiProbeState *)obj;g_free(s->virtual_otp_public_template);s->virtual_otp_public_template=g_strdup(value); }

static char *marconi_get_te_capability_inputs(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->te_capability_inputs); }
static void marconi_set_te_capability_inputs(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->te_capability_inputs);s->te_capability_inputs=g_strdup(value);
}

static char *marconi_get_te_context_fixture(Object *obj,Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->te_context_fixture); }
static void marconi_set_te_context_fixture(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->te_context_fixture);s->te_context_fixture=g_strdup(value);
}

static char *marconi_get_te_platform_fatal_fixture(Object *obj, Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->te_platform_fatal_fixture); }
static void marconi_set_te_platform_fatal_fixture(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->te_platform_fatal_fixture);s->te_platform_fatal_fixture=g_strdup(value);
}

static char *marconi_get_pll_rate_fixture(Object *obj, Error **errp)
{ return g_strdup(((MarconiProbeState *)obj)->pll_rate_fixture); }
static void marconi_set_pll_rate_fixture(Object *obj,const char *value,Error **errp)
{
    MarconiProbeState *s=(MarconiProbeState *)obj;
    g_free(s->pll_rate_fixture);s->pll_rate_fixture=g_strdup(value);
}

static bool marconi_get_thermal_observed_writes(Object *obj, Error **errp)
{ return ((MarconiProbeState *)obj)->thermal_observed_writes; }
static void marconi_set_thermal_observed_writes(Object *obj, bool value, Error **errp)
{ ((MarconiProbeState *)obj)->thermal_observed_writes=value; }

static bool marconi_get_context_observed_writes(Object *obj, Error **errp)
{ return ((MarconiProbeState *)obj)->context_observed_writes; }
static void marconi_set_context_observed_writes(Object *obj, bool value, Error **errp)
{ ((MarconiProbeState *)obj)->context_observed_writes = value; }

static void marconi_probe_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    object_class_property_add_bool(oc,"ota-reset",marconi_get_ota_reset,marconi_set_ota_reset);
    object_class_property_add_str(oc,"soc4005-config-inputs",marconi_get_soc4005_config_inputs,marconi_set_soc4005_config_inputs);
    object_class_property_add_str(oc,"ap-soc4005-2c0-initial",marconi_get_ap_soc4005_2c0_initial,marconi_set_ap_soc4005_2c0_initial);
    object_class_property_add_str(oc,"normal-emmc-profile",marconi_get_normal_emmc_profile,marconi_set_normal_emmc_profile);
    object_class_property_add_str(oc,"normal-emmc-fixed-supply",marconi_get_normal_emmc_fixed_supply,marconi_set_normal_emmc_fixed_supply);
    object_class_property_add_str(oc,"normal-emmc-tuning-profile",marconi_get_normal_emmc_tuning_profile,marconi_set_normal_emmc_tuning_profile);
    object_class_property_add_str(oc,"normal-emmc-fault-test",marconi_get_normal_emmc_fault_test,marconi_set_normal_emmc_fault_test);
    object_class_property_add_str(oc,"lcd-frame-file",marconi_get_lcd_frame_file,marconi_set_lcd_frame_file);
    object_class_property_add_str(oc,"normal-emmc-writeback",marconi_get_normal_emmc_writeback,marconi_set_normal_emmc_writeback);
    object_class_property_add_str(oc,"normal-emmc-media",marconi_get_normal_emmc_media,marconi_set_normal_emmc_media);
    object_class_property_add_str(oc,"pmic-regulator-inputs",marconi_get_pmic_regulator_inputs,marconi_set_pmic_regulator_inputs);
    object_class_property_add_str(oc,"pmic-clock-inputs",marconi_get_pmic_clock_inputs,marconi_set_pmic_clock_inputs);
    object_class_property_add_str(oc,"pmic1000-initial",marconi_get_pmic1000_initial,marconi_set_pmic1000_initial);
    object_class_property_add_str(oc,"timer4014-inputs",marconi_get_timer4014_inputs,marconi_set_timer4014_inputs);
    object_class_property_add_bool(oc,"timer4014-load-plus-one",marconi_get_timer4014_load_plus_one,marconi_set_timer4014_load_plus_one);
    object_class_property_add_str(oc,"ota-config-body-inputs",
                                 marconi_get_ota_config_body_inputs,marconi_set_ota_config_body_inputs);
    object_class_property_add_bool(oc,"ota-config-write-observations",
                                  marconi_get_ota_config_write_observations,marconi_set_ota_config_write_observations);
    object_class_property_add_str(oc,"ota-retained-hwlock-image",
                                 marconi_get_ota_retained_hwlock_image,marconi_set_ota_retained_hwlock_image);
    object_class_property_add_str(oc,"ota-retained-footer-image",
                                 marconi_get_ota_retained_footer_image,marconi_set_ota_retained_footer_image);
    object_class_property_add_str(oc,"ota-invalid-clock-marker",
                                 marconi_get_ota_invalid_clock_marker_fixture,marconi_set_ota_invalid_clock_marker_fixture);
    object_class_property_add_str(oc,"ota-sbl-argument-fixture",
                                 marconi_get_ota_sbl_argument_fixture,marconi_set_ota_sbl_argument_fixture);
    object_class_property_add_bool(oc, "timer64", marconi_get_timer64, marconi_set_timer64);
    object_class_property_add_str(oc,"i2c-nopeer-profile",marconi_get_i2c_nopeer_profile,marconi_set_i2c_nopeer_profile);
    object_class_property_add_str(oc,"ap-i2c-empty-config",marconi_get_ap_i2c_empty_config,marconi_set_ap_i2c_empty_config);
    object_class_property_add_str(oc,"ap-i2c-soc-commands",marconi_get_ap_i2c_soc_commands,marconi_set_ap_i2c_soc_commands);
    object_class_property_add_str(oc,"wdt4012-profile",marconi_get_wdt4012_profile,marconi_set_wdt4012_profile);
    object_class_property_add_str(oc,"ap-watchdog-init",marconi_get_ap_watchdog_init,marconi_set_ap_watchdog_init);
    object_class_property_add_str(oc,"ap-spi-config",marconi_get_ap_spi_config,marconi_set_ap_spi_config);
    object_class_property_add_str(oc,"ap-gpio-config",marconi_get_ap_gpio_config,marconi_set_ap_gpio_config);
    object_class_property_add_str(oc,"ap-te-host1-config",marconi_get_ap_te_host1_config,marconi_set_ap_te_host1_config);
    object_class_property_add_str(oc,"ap-te-platform-view",marconi_get_ap_te_platform_view,marconi_set_ap_te_platform_view);
    object_class_property_add_str(oc,"ap-timer64-view",marconi_get_ap_timer64_view,marconi_set_ap_timer64_view);
    object_class_property_add_str(oc,"ap-efuse-read-view",marconi_get_ap_efuse_read_view,marconi_set_ap_efuse_read_view);
    object_class_property_add_str(oc,"ap-thermal-config-view",marconi_get_ap_thermal_config_view,marconi_set_ap_thermal_config_view);
    object_class_property_add_str(oc,"ap-aon-iosleep-config",marconi_get_ap_aon_iosleep_config,marconi_set_ap_aon_iosleep_config);
    object_class_property_add_str(oc,"thermal-mode0-profile",marconi_get_thermal_mode0_profile,marconi_set_thermal_mode0_profile);
    object_class_property_add_str(oc,"dvfs-main-clock-initial",marconi_get_dvfs_main_clock_initial,marconi_set_dvfs_main_clock_initial);
    object_class_property_add_str(oc,"ap-pll-rate-view",marconi_get_ap_pll_rate_view,marconi_set_ap_pll_rate_view);
    object_class_property_add_str(oc,"dvfs-vote-config",marconi_get_dvfs_vote_config,marconi_set_dvfs_vote_config);
    object_class_property_add_str(oc,"auxadc-channel-profile",marconi_get_auxadc_channel_profile,marconi_set_auxadc_channel_profile);
    object_class_property_add_str(oc,"timer64-terminal",marconi_get_timer64_terminal_profile,marconi_set_timer64_terminal_profile);
    object_class_property_add_str(oc,"pmic-boot-events",marconi_get_pmic_boot_events_profile,marconi_set_pmic_boot_events_profile);
    object_class_property_add_str(oc,"pmic-voltage-source",marconi_get_pmic_voltage_source_profile,marconi_set_pmic_voltage_source_profile);
    object_class_property_add_str(oc,"pmic-vbus-input",marconi_get_pmic_vbus_input,marconi_set_pmic_vbus_input);
    object_class_property_add_str(oc,"adsp-publication",marconi_get_adsp_publication_profile,marconi_set_adsp_publication_profile);
    object_class_property_add_str(oc,"ap-mailbox-config",marconi_get_ap_mailbox_config,marconi_set_ap_mailbox_config);
    object_class_property_add_str(oc,"venc-device",marconi_get_venc_device_profile,marconi_set_venc_device_profile);
    object_class_property_add_str(oc,"vdec-device",marconi_get_vdec_device_profile,marconi_set_vdec_device_profile);
    object_class_property_add_str(oc,"gpu-boot-identity",marconi_get_gpu_boot_identity_profile,marconi_set_gpu_boot_identity_profile);
    object_class_property_add_str(oc,"gpu-boot-reset-profile",marconi_get_gpu_boot_reset_profile,marconi_set_gpu_boot_reset_profile);
    object_class_property_add_str(oc,"pmic-sync107c-profile",marconi_get_pmic_sync107c_profile,marconi_set_pmic_sync107c_profile);
    object_class_property_add_str(oc,"pmic-button-config-initial",marconi_get_pmic_button_config_initial,marconi_set_pmic_button_config_initial);
    object_class_property_add_str(oc,"pmic-alias-read-policy",marconi_get_pmic_alias_read_policy,marconi_set_pmic_alias_read_policy);
    object_class_property_add_str(oc,"pmic-mode-config-inputs",marconi_get_pmic_mode_config_inputs,marconi_set_pmic_mode_config_inputs);
    object_class_property_add_str(oc,"virtual-pmic-bc-config",marconi_get_virtual_pmic_bc_config,marconi_set_virtual_pmic_bc_config);
    object_class_property_add_str(oc,"virtual-auxadc-reset-profile",marconi_get_virtual_auxadc_reset_profile,marconi_set_virtual_auxadc_reset_profile);
    object_class_property_add_str(oc,"virtual-dvs-profile",marconi_get_virtual_dvs_profile,marconi_set_virtual_dvs_profile);
    object_class_property_add_str(oc,"aon120-config-inputs",marconi_get_aon120_config_inputs,marconi_set_aon120_config_inputs);
    object_class_property_add_str(oc,"pm-bit8-profile",marconi_get_pm_bit8_profile,marconi_set_pm_bit8_profile);
    object_class_property_add_str(oc,"mipi-clock-inputs",marconi_get_mipi_clock_inputs,marconi_set_mipi_clock_inputs);
    object_class_property_add_str(oc,"pm-domain7-profile",marconi_get_pm_domain7_profile,marconi_set_pm_domain7_profile);
    object_class_property_add_str(oc,"vpu-clock-inputs",marconi_get_vpu_clock_inputs,marconi_set_vpu_clock_inputs);
    object_class_property_add_str(oc,"pm-bit9-profile",marconi_get_pm_bit9_profile,marconi_set_pm_bit9_profile);
    object_class_property_add_str(oc,"usb-clock-inputs",marconi_get_usb_clock_inputs,marconi_set_usb_clock_inputs);
    object_class_property_add_str(oc,"pubcp-clock-inputs",marconi_get_pubcp_clock_inputs,marconi_set_pubcp_clock_inputs);
    object_class_property_add_str(oc,"pm-domain5-profile",marconi_get_pm_domain5_profile,marconi_set_pm_domain5_profile);
    object_class_property_add_str(oc,"pubap-clock-inputs",marconi_get_pubap_clock_inputs,marconi_set_pubap_clock_inputs);
    object_class_property_add_str(oc,"pm-domain4-profile",marconi_get_pm_domain4_profile,marconi_set_pm_domain4_profile);
    object_class_property_add_str(oc,"dpu-initial-b0",marconi_get_dpu_initial_b0,marconi_set_dpu_initial_b0);
    object_class_property_add_str(oc,"ap-runtime-config",marconi_get_ap_runtime_config,marconi_set_ap_runtime_config);
    object_class_property_add_str(oc,"ap-board-config",marconi_get_ap_board_config,marconi_set_ap_board_config);
    object_class_property_add_str(oc,"pm-functional-profile",marconi_get_pm_functional_profile,marconi_set_pm_functional_profile);
    object_class_property_add_str(oc,"pm-counter-fixture",
                                 marconi_get_pm_counter_fixture,marconi_set_pm_counter_fixture);
    object_class_property_add_str(oc,"rtc32k-reference",marconi_get_rtc32k_reference,marconi_set_rtc32k_reference);
    object_class_property_add_str(oc,"rtc-seconds-fixture",
                                 marconi_get_rtc_seconds_fixture,marconi_set_rtc_seconds_fixture);
    object_class_property_add_str(oc,"pmu-init-fixture",
                                 marconi_get_pmu_init_fixture,marconi_set_pmu_init_fixture);
    object_class_property_add_str(oc, "otp-read-fixture",
                                 marconi_get_otp_read_fixture, marconi_set_otp_read_fixture);
    object_class_property_add_str(oc,"tee-sbl-argument-fixture",
                                 marconi_get_tee_sbl_argument_fixture,marconi_set_tee_sbl_argument_fixture);
    object_class_property_add_str(oc,"rc-measurement-fixture",
                                 marconi_get_rc_measurement_fixture,marconi_set_rc_measurement_fixture);
    object_class_property_add_bool(oc,"mbox-outbound-experiment",
                                  marconi_get_mbox_outbound_experiment,marconi_set_mbox_outbound_experiment);
    object_class_property_add_str(oc,"wdt-policy-fixture",
                                 marconi_get_wdt_policy_fixture,marconi_set_wdt_policy_fixture);
    object_class_property_add_str(oc,"virtual-ao-csprng",marconi_get_trng_virtual_profile,marconi_set_trng_virtual_profile);
    object_class_property_add_str(oc,"trng-unkeyed-config-fixture",
                                 marconi_get_trng_unkeyed_fixture,marconi_set_trng_unkeyed_fixture);
    object_class_property_add_str(oc,"hash-init-ladder128",marconi_get_hash_init_profile,marconi_set_hash_init_profile);
    object_class_property_add_str(oc,"aca-config",marconi_get_aca_config_profile,marconi_set_aca_config_profile);
    object_class_property_add_str(oc,"symc-empty-hosts",marconi_get_symc_empty_profile,marconi_set_symc_empty_profile);
    object_class_property_add_str(oc,"rnp-postkey-config",marconi_get_rnp_postkey_config,marconi_set_rnp_postkey_config);
    object_class_property_add_str(oc,"rnp-mask-config",marconi_get_rnp_mask_config,marconi_set_rnp_mask_config);
    object_class_property_add_str(oc,"virtual-otp-poweron",marconi_get_virtual_otp_profile,marconi_set_virtual_otp_profile);
    object_class_property_add_str(oc,"virtual-otp-public-template",marconi_get_virtual_otp_public_template,marconi_set_virtual_otp_public_template);
    object_class_property_add_str(oc,"te-capability-inputs",marconi_get_te_capability_inputs,marconi_set_te_capability_inputs);
    object_class_property_add_str(oc,"te-context-fixture",
                                 marconi_get_te_context_fixture,marconi_set_te_context_fixture);
    object_class_property_add_str(oc, "te-platform-fatal-fixture",
                                 marconi_get_te_platform_fatal_fixture,marconi_set_te_platform_fatal_fixture);
    object_class_property_add_str(oc, "pll-rate-fixture",
                                 marconi_get_pll_rate_fixture,marconi_set_pll_rate_fixture);
    object_class_property_add_bool(oc, "thermal-observed-writes",
                                  marconi_get_thermal_observed_writes,
                                  marconi_set_thermal_observed_writes);
    object_class_property_add_bool(oc, "context-observed-writes",
                                  marconi_get_context_observed_writes,
                                  marconi_set_context_observed_writes);
    object_class_property_add_bool(oc, "qos-observed-write-experiment",
                                  marconi_get_qos_observed_write_experiment,
                                  marconi_set_qos_observed_write_experiment);
    object_class_property_add_str(oc, "dfs-config-fixture",
                                 marconi_get_dfs_config_fixture,
                                 marconi_set_dfs_config_fixture);
    object_class_property_add_str(oc, "clock48-fixture",
                                 marconi_get_clock48_fixture,
                                 marconi_set_clock48_fixture);
    object_class_property_add_str(oc,"timer0-runtime-fixture",
                                 marconi_get_timer0_runtime_fixture,marconi_set_timer0_runtime_fixture);
    object_class_property_add_bool(oc,"timer0-one-shot-experiment",
                                  marconi_get_timer0_one_shot_experiment,marconi_set_timer0_one_shot_experiment);
    object_class_property_add_str(oc, "timer0-stop-fixture",
                                 marconi_get_timer0_stop_fixture,
                                 marconi_set_timer0_stop_fixture);
    object_class_property_add_str(oc, "aon144-read-fixture",
                                 marconi_get_aon144_read_fixture,
                                 marconi_set_aon144_read_fixture);
    object_class_property_add_str(oc, "aon-prediv-query-fixture",
                                 marconi_get_aon_prediv_query_fixture,
                                 marconi_set_aon_prediv_query_fixture);
    object_class_property_add_str(oc, "clock-controls-fixture",
                                 marconi_get_clock_controls_fixture,
                                 marconi_set_clock_controls_fixture);
    object_class_property_add_str(oc, "ap-domain-status-fixture",
                                 marconi_get_ap_domain_status_fixture,
                                 marconi_set_ap_domain_status_fixture);
    object_class_property_add_bool(oc, "dtgu-lut0-observed-write",
                                  marconi_get_dtgu_lut0_observed_write,
                                  marconi_set_dtgu_lut0_observed_write);
    object_class_property_add_str(oc, "board-controls-fixture",
                                 marconi_get_board_controls_fixture,
                                 marconi_set_board_controls_fixture);
    object_class_property_add_str(oc, "resource-attribute-fixture",
                                 marconi_get_resource_attribute_fixture,
                                 marconi_set_resource_attribute_fixture);
    object_class_property_add_str(oc, "range-configuration-fixture",
                                 marconi_get_range_configuration_fixture,
                                 marconi_set_range_configuration_fixture);
    object_class_property_set_description(oc, "qos-observed-write-experiment",
        "Conditional reachability only: accept observed A8=0 writes; hardware semantics unknown");
    object_class_property_add_str(oc, "lowpage-response-experiment",
                                 marconi_get_lowpage_response_experiment,
                                 marconi_set_lowpage_response_experiment);
    object_class_property_set_description(oc, "lowpage-response-experiment",
        "Conditional bus response only: retain-written or write-ignore; unknown initial bytes reject");
    object_class_property_add_str(oc, "uart-tx-fixture", marconi_get_uart_tx_fixture,
                                 marconi_set_uart_tx_fixture);
    object_class_property_add_str(oc, "adc-fixture", marconi_get_adc_fixture,
                                 marconi_set_adc_fixture);
    object_class_property_add_str(oc, "explore-seed", marconi_get_explore_seed,
                                 marconi_set_explore_seed);
    object_class_property_add_str(oc, "explore-latency", marconi_get_explore_latency,
                                 marconi_set_explore_latency);
    mc->desc = "Experimental O62M Marconi staged boot probe (stops on missing MMIO)";
    mc->init = marconi_probe_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m55");
    mc->max_cpus = 3;
    mc->ignore_memory_transaction_failures = false;
    object_class_property_add_bool(oc, "tee-reset", marconi_get_tee_reset,
                                   marconi_set_tee_reset);
    object_class_property_set_description(oc, "tee-reset",
        "Probe exact original TEE Secure reset with real RAM aliases, not the boot chain");
    object_class_property_add_bool(oc, "sbl-reset", marconi_get_sbl_reset,
                                   marconi_set_sbl_reset);
    object_class_property_set_description(oc, "sbl-reset",
        "Probe unmodified SBL reset using its verified container/vector mapping");
    object_class_property_add_str(oc, "tee-ap-image", marconi_get_tee_ap_image,
                                 marconi_set_tee_ap_image);
    object_class_property_set_description(oc, "tee-ap-image",
        "Provision exact original AP RAM/image for natural TEE BLXNS; requires tee-reset");
    object_class_property_add_bool(oc,"ap-boot-buffer-ram",marconi_get_ap_boot_buffer_enabled,marconi_set_ap_boot_buffer_enabled);
    object_class_property_add_str(oc,"ap-uncache-ram",marconi_get_ap_uncache_ram_profile,marconi_set_ap_uncache_ram_profile);
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
    type_register_static(&marconi_touch_type);
    type_register_static(&marconi_crf_cpu_type);
    type_register_static(&marconi_probe_type);
}
type_init(marconi_probe_register_types)
