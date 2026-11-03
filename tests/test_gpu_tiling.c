/* SPDX-License-Identifier: GPL-2.0-or-later
 * Offline production decoder contract; no firmware, guest or QMP execution.
 * Independent integer texel addressing oracle, with existing per-family
 * coordinate conventions made explicit rather than inferred from production.
 * Opaque sources isolate tile addressing from unrelated alpha rounding.
 */
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
static unsigned lduw_le_p(const void *v) {
    const uint8_t *p=v; return p[0] | p[1]<<8;
}
static uint32_t ldl_le_p(const void *v) {
    const uint8_t *p=v;
    return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static void stw_le_p(void *v,unsigned n) {
    uint8_t *p=v; p[0]=n; p[1]=n>>8;
}
static void stl_le_p(void *v,uint32_t n) {
    uint8_t *p=v; p[0]=n; p[1]=n>>8; p[2]=n>>16; p[3]=n>>24;
}
#define LOG_GUEST_ERROR 0
#define qemu_log_mask(mask, ...) ((void)(mask))
#ifndef BES_GPU_HEADER
#error Define BES_GPU_HEADER
#endif
#include BES_GPU_HEADER

#define RAM_BASE 0x20000000u
#define SOURCE_OFFSET 256
static uint8_t ram[1024];
static unsigned checks,failures,vectors;
static unsigned group_checks[4],group_failures[4],group_vectors[4];
static unsigned active_tile;
static const char *tile_names[]={"COLOR","PAD","REPEAT","REFLECT"};
static const uint8_t palette[8][4]={
    {16,32,48,255},{64,80,96,255},{112,128,144,255},{160,176,192,255},
    {48,64,80,255},{96,112,128,255},{144,160,176,255},{192,208,224,255}
};
static const uint8_t old[4]={3,5,7,255};
static bool read_ram(uint32_t address,void *out,unsigned bytes) {
    if(address<RAM_BASE || (uint64_t)address+bytes>(uint64_t)RAM_BASE+sizeof(ram)) return false;
    memcpy(out,ram+(address-RAM_BASE),bytes); return true;
}
static uint32_t bits(float f) { uint32_t u; memcpy(&u,&f,4); return u; }
static void expect(bool pass,const char *name,const char *reason) {
    checks++; group_checks[active_tile]++;
    if(!pass) {
        failures++; group_failures[active_tile]++;
        if(group_failures[active_tile]<=12) fprintf(stderr,"FAIL %s: %s\n",name,reason);
    }
}
static void fixture(unsigned width,unsigned height) {
    memset(ram,0xa5,sizeof(ram)); memcpy(ram,old,4);
    for(unsigned y=0;y<height;y++) for(unsigned x=0;x<width;x++)
        memcpy(ram+SOURCE_OFFSET+(y*width+x)*4,palette[y*4+x],4);
}
/* OpenVG-style texel mirror includes the last edge texel on either side of
 * the reflection axis: width=4 yields ... 3 2 1 0 | 0 1 2 3 | 3 2 1 0 ... . */
static int address_texel(int index,unsigned size,unsigned tile) {
    if(tile==0) return index<0 || index>=(int)size ? -1 : index;
    if(tile==1) return index<0 ? 0 : index>=(int)size ? (int)size-1 : index;
    int period=tile==2 ? (int)size : (int)size*2;
    int folded=index%period;
    if(folded<0) folded+=period;
    if(tile==3 && folded>=(int)size) folded=period-1-folded;
    return folded;
}
static void oracle(unsigned width,unsigned height,unsigned tile,unsigned filter,
                   float u,float v,uint8_t result[4]) {
    int ix=(int)floor(u), iy=(int)floor(v);
    double fx=u-ix,fy=v-iy;
    if(!filter) {
#ifdef GENERIC_COORDS
        ix=(int)floor(u+0.5); iy=(int)floor(v+0.5);
#endif
        fx=fy=0;
    }
    double channels[3]={0},alpha=0;
    for(unsigned dy=0;dy<2;dy++) for(unsigned dx=0;dx<2;dx++) {
        int x=address_texel(ix+dx,width,tile), y=address_texel(iy+dy,height,tile);
        double weight=(dx?fx:1-fx)*(dy?fy:1-fy);
        if(x<0 || y<0) continue;
        const uint8_t *p=palette[y*4+x];
        alpha+=weight*255;
        for(unsigned c=0;c<3;c++) channels[c]+=weight*p[c];
    }
    if(alpha==0) { memcpy(result,old,4); return; }
    /* Non-COLOR tiles are opaque; COLOR control vectors are deliberately
     * wholly inside or outside, so this test makes no alpha rounding claim. */
    for(unsigned c=0;c<3;c++) result[c]=(unsigned)(channels[c]+0.5);
    result[3]=255;
}
static BesGpuWork *draw(unsigned width,unsigned height,unsigned tile,unsigned filter,
                       float u,float v,const char **error) {
    uint32_t regs[BES_GPU_REGS]={0}; uint8_t command[24]={0};
    regs[0xa00]=0x10001181; regs[0xa02]=0xff102030;
    regs[0xa10]=0x103; regs[0xa11]=RAM_BASE; regs[0xa12]=4; regs[0xa13]=0x10001;
    regs[0xa25]=0x01000107 | tile<<12 | filter<<16;
    regs[0xa29]=RAM_BASE+SOURCE_OFFSET; regs[0xa2b]=width*4;
    regs[0xa2f]=height<<16|width;
#ifdef GENERIC_COORDS
    regs[0xa18]=bits((u+0.5f)/width); regs[0xa19]=bits((v+0.5f)/height);
#else
    /* Existing watch LINEAR-PAD gradient command already supplies its
     * half-texel adjustment. All other filtered watch paths subtract .5. */
    float offset=filter && !(filter==1 && tile==1) ? 0.5f : 0;
    regs[0xa18]=bits(u+offset); regs[0xa19]=bits(v+offset);
#endif
    regs[0xa1a]=bits(1); regs[0xa1f]=1;
    stl_le_p(command,0x40000001); stw_le_p(command+12,1); stw_le_p(command+14,1);
    return gpu_sw_prepare(regs,command,sizeof(command),read_ram,error);
}
static void vector(unsigned width,unsigned height,unsigned tile,unsigned filter,float u,float v) {
    char name[128],why[128];
    snprintf(name,sizeof(name),"%s filter=%u %ux%u u=%.2f v=%.2f",tile_names[tile],filter,width,height,u,v);
    active_tile=tile; vectors++; group_vectors[tile]++;
    fixture(width,height); uint8_t before[sizeof(ram)]; memcpy(before,ram,sizeof(ram));
    uint8_t expected[4]; oracle(width,height,tile,filter,u,v,expected);
    const char *error=NULL; BesGpuWork *work=draw(width,height,tile,filter,u,v,&error);
    expect(work!=NULL,name,error?error:"rejected");
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
int main(void) {
    static const float coords[][2]={
        {0,0},{.25f,.25f},{2.25f,.25f},{3.25f,1.25f},{3.75f,1.75f},
        {-.25f,.25f},{-1.25f,.25f},{-4,.25f},{-4.25f,-2.25f},{-8.25f,-4.25f},
        {4.25f,.25f},{8.75f,.25f},{16.25f,8.25f},{.25f,-.25f},{.25f,2.25f},
        {4.25f,2.25f},{3.75f,-.25f},{2048.25f,-2048.25f}
    };
    for(unsigned filter=0;filter<3;filter++) {
        /* Existing COLOR transparent border controls, avoiding fractional
         * alpha interactions unrelated to address mode changes. */
        vector(4,2,0,filter,0,0); vector(4,2,0,filter,.25f,.25f);
        vector(4,2,0,filter,2.25f,.25f); vector(4,2,0,filter,-4,-4);
        vector(4,2,0,filter,8,8); vector(1,1,0,filter,0,0);
        for(unsigned tile=1;tile<4;tile++) for(unsigned i=0;i<G_N_ELEMENTS(coords);i++) {
            vector(4,2,tile,filter,coords[i][0],coords[i][1]);
            vector(1,1,tile,filter,coords[i][0],coords[i][1]);
        }
    }
    for(unsigned tile=0;tile<4;tile++) printf("%s: %u vectors, %u checks, %u failures\n",
        tile_names[tile],group_vectors[tile],group_checks[tile],group_failures[tile]);
    printf("TOTAL: %u vectors, %u checks, %u failures; guest_executed=false\n",vectors,checks,failures);
    return failures?1:0;
}
