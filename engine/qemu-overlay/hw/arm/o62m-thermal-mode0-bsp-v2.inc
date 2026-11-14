/* Explicit mode0 virtual thermal sampling. Canonical configuration remains
 * in s->thermal_config; no measured analog/clock/status reset claim.
 * SPDX-License-Identifier: GPL-2.0-only */
static bool marconi_thermal_mode0_failure(MarconiProbeState *s,
                                         O62MThermalMode0ResultV2 result)
{
    if(!s->reported)marconi_public_report("MARCONI_THERMAL_MODE0_BOUNDARY reason=%s "
        "public-pc=%08" PRIx64 " raw-config-and-result-redacted=true no-fake-ready=true",
        o62m_thermal_mode0_reason_v2(result),
        marconi_public_code_pc(current_cpu?CPU_GET_CLASS(current_cpu)->get_pc(current_cpu):UINT64_MAX));
    if(s->thermal_mode0_event)timer_del(s->thermal_mode0_event);
    marconi_owned_runtime_failure_stop(s);
    return false;
}

static bool marconi_thermal_mode0_refresh(MarconiProbeState *s)
{
    O62MThermalMode0JobV2 job;
    O62MThermalMode0ResultV2 result;
    bool due;
    if(!s->thermal_mode0_profile)return true;
    if(s->normal_emmc_bound&&s->reported)return false;
    int64_t now=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    result=o62m_thermal_mode0_job_due_v2(&s->thermal_mode0,now,&job,&due);
    if(result!=O62M_THERMAL_MODE0_OK_V2)return marconi_thermal_mode0_failure(s,result);
    if(!job.generation) {timer_del(s->thermal_mode0_event);return true;}
    if(!s->thermal_mode0_clock) {
        /* Explicit fixed unavailable-clock input retains the pending job.
         * No read can complete it. The original HAL may timeout/stop it. */
        timer_del(s->thermal_mode0_event);return true;
    }
    if(!due) {timer_mod_ns(s->thermal_mode0_event,job.deadline_ns);return true;}
    result=o62m_thermal_mode0_apply_due_v2(&s->thermal_mode0,now,&job,
                                          s->thermal_mode0_clock);
    if(result!=O62M_THERMAL_MODE0_OK_V2)return marconi_thermal_mode0_failure(s,result);
    timer_del(s->thermal_mode0_event);
    marconi_public_report("MARCONI_THERMAL_MODE0_COMPLETED jobs=%" PRIu64
        " actual-start-configuration-and-raw-input-snapshot=true actual-deadline-result-copy=true "
        "raw-and-temperature-redacted=true no-IRQ-or-guest-cache-write=true",
        s->thermal_mode0.jobs_completed);
    return true;
}

static void marconi_thermal_mode0_callback(void *opaque)
{ (void)marconi_thermal_mode0_refresh(opaque); }

static bool marconi_thermal_mode0_access(MarconiProbeState *s,uint64_t offset,
                                         unsigned size,bool write,uint64_t *value)
{
    if(!marconi_thermal_mode0_refresh(s))return false;
    uint64_t *counter=!write&&offset==0x18?&s->thermal_mode0_status_reads:
                       !write&&offset==0x34?&s->thermal_mode0_result_reads:NULL;
    if(counter&&*counter==UINT64_MAX)
        return marconi_thermal_mode0_failure(s,O62M_THERMAL_MODE0_COUNTER_OVERFLOW_V2);
    O62MThermalMode0ResultV2 result=o62m_thermal_mode0_access_v2(
        &s->thermal_mode0,&s->thermal_config,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
        offset,size,write,value);
    if(result!=O62M_THERMAL_MODE0_OK_V2)return marconi_thermal_mode0_failure(s,result);
    if(counter)++*counter;
    if(write)marconi_public_report("MARCONI_THERMAL_MODE0_CONTROL starts=%" PRIu64
        " stops=%" PRIu64 " cancelled=%" PRIu64 " captured-written-mask=%u"
        " actual-original-control-write=true shared-eight-config-owner=true "
        "raw-and-configuration-redacted=true",
        s->thermal_mode0.jobs_started,s->thermal_mode0.stops,s->thermal_mode0.jobs_cancelled,
        s->thermal_mode0.captured_written_mask);
    return marconi_thermal_mode0_refresh(s);
}

static void marconi_thermal_mode0_exit(Notifier *notify,void *opaque)
{
    MarconiProbeState *s=container_of(notify,MarconiProbeState,thermal_mode0_exit);
    (void)opaque;
    marconi_public_report("MARCONI_THERMAL_MODE0_EXIT starts=%" PRIu64
        " completed=%" PRIu64 " cancelled=%" PRIu64 " stops=%" PRIu64
        " status-reads=%" PRIu64 " result-reads=%" PRIu64
        " pending=%u result-valid=%u declared-functional-clock=%u "
        "raw-and-temperature-not-output=true physical-sampling-and-IRQ-not-claimed=true",
        s->thermal_mode0.jobs_started,s->thermal_mode0.jobs_completed,
        s->thermal_mode0.jobs_cancelled,s->thermal_mode0.stops,
        s->thermal_mode0_status_reads,s->thermal_mode0_result_reads,
        s->thermal_mode0.phase==O62M_THERMAL_MODE0_PENDING_PHASE_V2,
        s->thermal_mode0.result_valid,s->thermal_mode0_clock);
}

static void marconi_thermal_mode0_create(MarconiProbeState *s)
{
    g_auto(GStrv) words=g_strsplit(s->thermal_mode0_profile,":",-1);
    uint64_t input[3];
    if(g_strv_length(words)!=3) {
        error_report("thermal-mode0-profile requires explicitraw_u32:latency_ns:functionalclock0or1");exit(EXIT_FAILURE);
    }
    for(unsigned i=0;i<3;i++) {
        char *end;
        if(!g_ascii_isdigit(words[i][0])) {
            error_report("thermal-mode0-profile requires unsigned explicitinputs");exit(EXIT_FAILURE);
        }
        errno=0;input[i]=g_ascii_strtoull(words[i],&end,0);
        if(errno||*end) {error_report("thermal-mode0-profile invalid unsignedinput");exit(EXIT_FAILURE);}
    }
    if(input[2]>1) {error_report("thermal-mode0-profile functionalclock mustbe0or1");exit(EXIT_FAILURE);}
    O62MThermalMode0SpecV2 spec={.explicit_enable=true,.instance_id=UINT64_C(0x54484d30),
        .latency_ns=input[1],.raw_input_u32=input[0],
        .now_ns=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)};
    O62MThermalMode0ResultV2 result=o62m_thermal_mode0_init_v2(&s->thermal_mode0,&spec);
    if(result!=O62M_THERMAL_MODE0_OK_V2) {
        error_report("thermal-mode0-profile invalid: %s",o62m_thermal_mode0_reason_v2(result));exit(EXIT_FAILURE);
    }
    s->thermal_mode0_clock=input[2];
    s->thermal_mode0_event=timer_new_ns(QEMU_CLOCK_VIRTUAL,marconi_thermal_mode0_callback,s);
    s->thermal_mode0_exit.notify=marconi_thermal_mode0_exit;
    qemu_add_exit_notifier(&s->thermal_mode0_exit);
    error_report("MARCONI_THERMAL_MODE0_PROFILE explicit-raw-input-and-positive-latency=true "
        "declared-functional-clock=%u physical-clock-mapping-unknown=true initial-ready=false "
        "clear-on-new-start-and-stop=true retain-on-read=true mode50-IRQ-and-analog-unimplemented=true "
        "shared-eight-canonical-config-owner=true seven-mode0-fields-required-optional-threshold-validity-retained=true "
        "original-FP-temperature-computation-unchanged=true",
        s->thermal_mode0_clock);
}
