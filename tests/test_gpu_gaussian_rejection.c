/* SPDX-License-Identifier: GPL-2.0-or-later
 * Unsupported filtering/state must reject transactionally. No default
 * Gaussian kernel is inferred and no firmware is executed by this test.
 */
#define main tiling_contract_unused_main
#include "test_gpu_tiling.c"
#undef main

static BesGpuWork *clear_with_stale_sampler(unsigned tile,const char **error) {
    uint32_t regs[BES_GPU_REGS]={0}; uint8_t command[24]={0};
    regs[0xa00]=0x10000001; regs[0xa02]=0xff102030;
    regs[0xa10]=0x103; regs[0xa11]=RAM_BASE;
    regs[0xa12]=4; regs[0xa13]=0x10001;
    regs[0xa25]=0x01030107 | tile<<12;
    stl_le_p(command,0x40000001); stw_le_p(command+12,1); stw_le_p(command+14,1);
    return gpu_sw_prepare(regs,command,sizeof(command),read_ram,error);
}

static void reject_invalid_state(bool after_draw) {
    uint32_t regs[BES_GPU_REGS]={0}; uint8_t command[40]={0};
    regs[0xa00]=0x10001181; regs[0xa02]=0xff102030;
    regs[0xa10]=0x103; regs[0xa11]=RAM_BASE; regs[0xa12]=4; regs[0xa13]=0x10001;
    regs[0xa25]=0x01000107; regs[0xa29]=RAM_BASE+SOURCE_OFFSET;
    regs[0xa2b]=16; regs[0xa2f]=0x20004;
    regs[0xa18]=bits(.25f); regs[0xa19]=bits(.25f);
    regs[0xa1a]=bits(1); regs[0xa1f]=1;
    unsigned offset=0;
    if(after_draw) {
        stl_le_p(command,0x40000001); stw_le_p(command+12,1); stw_le_p(command+14,1);
        offset=16;
    }
    /* AD3 is the public Gaussian centre coefficient, currently unsupported.
     * A partial preceding draw must not become visible on this error. */
    stl_le_p(command+offset,0x30010ad3); stl_le_p(command+offset+4,128);
    fixture(4,2); uint8_t before[sizeof(ram)]; memcpy(before,ram,sizeof(ram));
    const char *error=NULL;
    BesGpuWork *work=gpu_sw_prepare(regs,command,offset+16,read_ram,&error);
    vectors++; expect(work==NULL,"invalid STATE","unsupported AD3 accepted");
    expect(error && strstr(error,"STATE"),"invalid STATE","missing state rejection reason");
    expect(!memcmp(before,ram,sizeof(ram)),"invalid STATE","guest RAM changed on rejected transaction");
    gpu_sw_free(work);
}
int main(void) {
    for(unsigned tile=0;tile<4;tile++) {
        active_tile=tile; fixture(4,2); uint8_t before[sizeof(ram)]; memcpy(before,ram,sizeof(ram));
        const char *error=NULL; BesGpuWork *work=draw(4,2,tile,3,.25f,.25f,&error);
        vectors++; expect(work==NULL,"Gaussian","unimplemented Gaussian silently accepted");
        expect(error!=NULL,"Gaussian","missing rejection reason");
        expect(!memcmp(before,ram,sizeof(ram)),"Gaussian","rejected prepare mutated RAM");
        gpu_sw_free(work);
#ifndef GENERIC_COORDS
        /* The generic backend has no watch-style clear rectangle command. */
        error=NULL; work=clear_with_stale_sampler(tile,&error); vectors++;
        expect(work!=NULL,"clear","unused stale Gaussian sampler rejected");
        expect(!memcmp(before,ram,sizeof(ram)),"clear","prepare mutated RAM");
        if(work) {
            expect(work->target_count==1,"clear","target count");
            if(work->target_count==1)
                expect(!memcmp(work->targets[0].pixels,(uint8_t[]){16,32,48,255},4),"clear","incorrect clear color");
        }
        gpu_sw_free(work);
#endif
    }
    active_tile=0; reject_invalid_state(false); reject_invalid_state(true);
    printf("Gaussian/state controls: %u vectors, %u checks, %u failures; guest_executed=false\n",vectors,checks,failures);
    return failures?1:0;
}
