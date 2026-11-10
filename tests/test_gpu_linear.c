/* SPDX-License-Identifier: GPL-2.0-or-later
 * Independent, host-only contract for generic VGLite horizontal LINEAR.
 * NXP SDK 25.06 blit_enumerations distinguishes horizontal LINEAR from a
 * BI_LINEAR 2x2 box. The normalized vg_lite_blit_rect coefficients in the
 * LVGL-maintained VGLite driver retain POINT's Y coefficients for LINEAR.
 * Constant source columns isolate vertical row selection from X filtering.
 * Quarter coordinates avoid any claim about exact half-texel tie behavior.
 */
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static unsigned lduw_le_p(const void *v)
{ const uint8_t *p=v; return p[0] | p[1]<<8; }
static uint32_t ldl_le_p(const void *v)
{ const uint8_t *p=v; return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static void stw_le_p(void *v,unsigned n)
{ uint8_t *p=v; p[0]=n; p[1]=n>>8; }
static void stl_le_p(void *v,uint32_t n)
{ uint8_t *p=v; p[0]=n; p[1]=n>>8; p[2]=n>>16; p[3]=n>>24; }
#define LOG_GUEST_ERROR 0
#define qemu_log_mask(mask, ...) ((void)(mask))
#ifndef BES_GPU_HEADER
#error Define BES_GPU_HEADER to the actual generic production header
#endif
#include BES_GPU_HEADER

#define RAM_BASE 0x20000000u
#define SOURCE_OFFSET 256
static uint8_t ram[1024];
static const uint8_t rows[2][4]={{16,32,48,255},{112,128,144,255}};
static const uint8_t old[4]={3,5,7,255};
static unsigned checks,failures,vectors,active_filter;
static unsigned filter_checks[3],filter_failures[3],filter_vectors[3];

static bool read_ram(uint32_t address,void *out,unsigned bytes)
{
    if(address<RAM_BASE || (uint64_t)address+bytes>(uint64_t)RAM_BASE+sizeof(ram)) return false;
    memcpy(out,ram+(address-RAM_BASE),bytes); return true;
}
static uint32_t bits(float f)
{ uint32_t n; memcpy(&n,&f,sizeof(n)); return n; }
static void expect(bool pass,const char *name,const char *reason)
{
    checks++; filter_checks[active_filter]++;
    if(!pass) {
        failures++; filter_failures[active_filter]++;
        if(filter_failures[active_filter]<=16) fprintf(stderr,"FAIL %s: %s\n",name,reason);
    }
}

/* This integer oracle is independent from the production tiling helper. */
static int row_index(int index,unsigned tile)
{
    if(tile==0) return index<0 || index>=2 ? -1 : index;
    if(tile==1) return index<0 ? 0 : index>1 ? 1 : index;
    int period=tile==2 ? 2 : 4;
    int folded=index%period;
    if(folded<0) folded+=period;
    return tile==3 && folded>=2 ? 3-folded : folded;
}

static BesGpuWork *draw(unsigned tile,unsigned filter,float ty,const char **error)
{
    const unsigned width=4,height=2; const float tx=.25f;
    uint32_t r[BES_GPU_REGS]={0}; uint8_t command[24]={0};
    r[0xa00]=0x10001181; r[0xa02]=0xff102030;
    r[0xa10]=0x103; r[0xa11]=RAM_BASE; r[0xa12]=4; r[0xa13]=0x10001;
    r[0xa25]=0x01000107 | tile<<12 | filter<<16;
    r[0xa29]=RAM_BASE+SOURCE_OFFSET; r[0xa2b]=width*4;
    r[0xa2f]=height<<16|width;
    /* SDK normalized inverse matrix [1 0 tx; 0 1 ty; 0 0 1].
     * Pixel center is +.5. LINEAR subtracts .5 from X only; BI from X/Y.
     * POINT and LINEAR consequently have identical unfiltered Y fields.
     * This derives the command from the API; it does not invert the decoder.
     */
    r[0xa18]=bits((.5f+tx-(filter ? .5f:0))/width);
    r[0xa19]=bits((.5f+ty-(filter==2 ? .5f:0))/height);
    r[0xa1a]=bits(1); r[0xa1c]=bits(1.f/width);
    r[0xa21]=bits(1.f/height); r[0xa1f]=1;
    stl_le_p(command,0x40000001); stw_le_p(command+12,1); stw_le_p(command+14,1);
    return gpu_sw_prepare(r,command,sizeof(command),read_ram,error);
}

static void vector(unsigned tile,unsigned filter,float ty)
{
    char name[96],why[160];
    snprintf(name,sizeof(name),"tile=%u filter=%u ty=%.2f",tile,filter,ty);
    active_filter=filter; vectors++; filter_vectors[filter]++;
    memset(ram,0xa5,sizeof(ram)); memcpy(ram,old,sizeof(old));
    for(unsigned y=0;y<2;y++) for(unsigned x=0;x<4;x++)
        memcpy(ram+SOURCE_OFFSET+(y*4+x)*4,rows[y],4);
    uint8_t before[sizeof(ram)],expected[4]; memcpy(before,ram,sizeof(ram));
    if(filter==2) {
        /* BI controls are interior ty=.25/.75: exact opaque weighted sums. */
        unsigned first=ty==.25f ? 40 : 88;
        expected[0]=first; expected[1]=first+16; expected[2]=first+32; expected[3]=255;
    } else {
        /* Same vertical POINT selection for both POINT and horizontal LINEAR.
         * At the tested non-ties, floor(ty+.5) chooses the native row center.
         * COLOR outside is transparent and preserves the old SRC_OVER target.
         */
        int row=row_index((int)floor(ty+.5f),tile);
        memcpy(expected,row<0 ? old : rows[row],4);
    }
    const char *error=NULL; BesGpuWork *work=draw(tile,filter,ty,&error);
    expect(work!=NULL,name,error?error:"decoder rejected");
    expect(!memcmp(ram,before,sizeof(ram)),name,"prepare mutated guest RAM");
    if(!work) return;
    expect(work->target_count==1,name,"target count");
    expect(work->event==1,name,"completion event");
    if(work->target_count==1) {
        expect(work->targets[0].bytes==4,name,"target extent");
        const uint8_t *actual=work->targets[0].pixels;
        snprintf(why,sizeof(why),"got BGRA %u,%u,%u,%u expected %u,%u,%u,%u",
                 actual[0],actual[1],actual[2],actual[3],expected[0],expected[1],expected[2],expected[3]);
        expect(!memcmp(actual,expected,4),name,why);
    }
    gpu_sw_free(work);
}

int main(void)
{
    static const float y[]={.25f,.75f,-.25f,-.75f,1.25f,1.75f,
                            2.25f,2.75f,-2.25f,-2.75f,3.25f,3.75f};
    for(unsigned tile=0;tile<4;tile++) {
        for(unsigned filter=0;filter<2;filter++)
            for(unsigned i=0;i<G_N_ELEMENTS(y);i++) vector(tile,filter,y[i]);
        vector(tile,2,.25f); vector(tile,2,.75f);
    }
    for(unsigned filter=0;filter<3;filter++)
        printf("filter=%u: %u vectors, %u checks, %u failures\n",filter,
               filter_vectors[filter],filter_checks[filter],filter_failures[filter]);
    printf("TOTAL: %u vectors, %u checks, %u failures; guest_executed=false\n",vectors,checks,failures);
    return failures?1:0;
}
