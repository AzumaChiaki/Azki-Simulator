/* SPDX-License-Identifier: GPL-2.0-or-later
 * Regression for the P67 solid-path destination-in command A00=10000402,
 * target A10=103. A25=01000107 in the failing guest is stale texture state:
 * vg_lite_draw uses A02 paint, with no image mode. Only paint alpha matters.
 *
 * NXP gpu-vglite vg_lite.c convert_blend(DST_IN)=0x400; vg_lite_path.c
 * vg_lite_draw emits the solid path and A02 color. OpenVG 1.1 sections 2.8
 * and 13.5 specify destination-in on premultiplied RGBA, then interpolation
 * with the original destination by path coverage. Expected bytes below are
 * independent fixed vectors, including zero-alpha erasure and edge coverage.
 */
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned lduw_le_p(const void *v){const uint8_t*p=v;return p[0]|p[1]<<8;}
static uint32_t ldl_le_p(const void *v){const uint8_t*p=v;return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void stw_le_p(void*v,unsigned n){uint8_t*p=v;p[0]=n;p[1]=n>>8;}
static void stl_le_p(void*v,uint32_t n){uint8_t*p=v;p[0]=n;p[1]=n>>8;p[2]=n>>16;p[3]=n>>24;}
#define LOG_GUEST_ERROR 0
#define qemu_log_mask(mask, ...) ((void)(mask))
#ifndef BES_GPU_HEADER
#define BES_GPU_HEADER "../engine/qemu-overlay/hw/arm/best1503-gpu.h"
#endif
#include BES_GPU_HEADER
static uint8_t ram[12];
static unsigned reads, checks;
static bool read_ram(uint32_t a,void*out,unsigned n){
    reads++;
    if(a!=0x20000000||n!=sizeof(ram))return false;
    memcpy(out,ram,n);return true;
}
static uint32_t bits(float x){uint32_t b;memcpy(&b,&x,4);return b;}
static void require(bool condition,const char*why){
    if(!condition){fprintf(stderr,"FAIL DST_IN: %s\n",why);exit(1);}checks++;
}
static unsigned fixture(uint32_t *regs,uint8_t *command,uint32_t color,
                        const uint8_t before[4],unsigned poison,bool clipped){
    memset(regs,0,BES_GPU_REGS*sizeof(*regs));memset(command,0,88);reads=0;
    for(unsigned i=0;i<3;i++)memcpy(ram+4*i,before,4);
    regs[0xa00]=0x10000402;regs[0xa02]=color;
    regs[0xa10]=0x103;regs[0xa11]=0x20000000;regs[0xa12]=12;regs[0xa13]=0x10003;
    /* Captured A25 plus inaccessible source, then deliberately invalid image
     * registers: neither may be read for this solid-path operation. */
    regs[0xa25]=poison?0xffffffff:0x01000107;regs[0xa29]=0x3c840580;regs[0xa2b]=0x540;
    if(poison==2){regs[0xa25]=0x41020100;regs[0xa2b]=3;} /* stale packed stride */
    regs[0xa2f]=0xffffffff;regs[0xa18]=0x7fc00000;regs[0xa27]=regs[0xa2d]=0xffffffff;
    regs[0xa34]=0x03300000;regs[0xa3a]=clipped?0x10001:0x10003;
    regs[0xa40]=regs[0xa44]=bits(1);
    uint32_t path[]={2,0,0,4,bits(1.5f),0,4,bits(1.5f),bits(1),4,0,bits(1),1,0};
    stl_le_p(command,0x40000000|sizeof(path)/8);
    for(unsigned i=0;i<G_N_ELEMENTS(path);i++)stl_le_p(command+8+i*4,path[i]);
    return sizeof(path)+16;
}
static void vector(uint32_t color,const uint8_t before[4],const uint8_t full[4],
                   const uint8_t half[4],unsigned poison,bool clipped){
    uint32_t regs[BES_GPU_REGS];uint8_t command[88];
    unsigned bytes=fixture(regs,command,color,before,poison,clipped);
    const char*error=NULL;BesGpuWork*w=gpu_sw_prepare(regs,command,bytes,read_ram,&error);
    if(!w){fprintf(stderr,"FAIL DST_IN vector: %s\n",error);exit(1);}
    require(w->target_count==1&&w->draws==1,"one staged destination");
    require(memcmp(w->targets[0].pixels,full,4)==0,"full coverage pixel");
    require(memcmp(w->targets[0].pixels+4,clipped?before:half,4)==0,"half coverage / clipped pixel");
    require(memcmp(w->targets[0].pixels+8,before,4)==0,"outside path unchanged");
    require(reads==1,"no stale texture RAM read");
    for(unsigned i=0;i<3;i++)require(memcmp(ram+4*i,before,4)==0,"prepare never writes guest RAM");
    gpu_sw_free(w);
}
static void rejected(unsigned kind){
    const uint8_t before[]={32,64,96,128};uint32_t regs[BES_GPU_REGS];uint8_t command[88];
    unsigned bytes=fixture(regs,command,0x80000000,before,false,false);
    if(kind==0)regs[0xa00]=0x10000502; /* multiply blend still unsupported */
    if(kind==1)regs[0xa00]=0x10001402; /* image DST_IN not added by this fix */
    if(kind==2)regs[0xad1]=1;         /* global alpha */
    if(kind==3)regs[0xa90]=1;         /* separate mask */
    if(kind==4)regs[0xa10]=0x109;     /* unobserved opaque destination */
    if(kind==5)regs[0xa34]=0;         /* invalid tessellation */
    if(kind==6)regs[0xa11]=0;         /* unsafe target */
    if(kind==7){                    /* fail after a valid draw: no commit */
        stl_le_p(command+bytes-8,0xf0000000);bytes+=8;
    }
    const char*error=NULL;BesGpuWork*w=gpu_sw_prepare(regs,command,bytes,read_ram,&error);
    require(!w&&error,"invalid state remains a rejection");
    for(unsigned i=0;i<3;i++)require(memcmp(ram+4*i,before,4)==0,"rejected submission never writes RAM");
}
int main(void){
    /* Buffers and fixed expectations are BGRA, with premultiplied color. */
    const uint8_t before[]={32,64,96,128};
    const uint8_t full[][4]={{0,0,0,0},{8,16,24,32},{16,32,48,64},{32,64,96,128}};
    const uint8_t half[][4]={{16,32,48,64},{20,40,60,80},{24,48,72,96},{32,64,96,128}};
    const unsigned alpha[]={0,64,128,255};
    for(unsigned i=0;i<4;i++)for(unsigned poison=0;poison<3;poison++)for(unsigned rgb=0;rgb<2;rgb++){
        vector((alpha[i]<<24)|(rgb?0x00a43ce1:0),before,full[i],half[i],poison,false);
    }
    vector(0x00000000,before,full[0],half[0],true,true);
    vector(0x80000000,(const uint8_t[]){40,100,180,255},
           (const uint8_t[]){20,50,90,128},(const uint8_t[]){30,75,135,192},false,false);
    for(unsigned i=0;i<8;i++)rejected(i);
    printf("GPU DST_IN regression: %u checks passed\n",checks);return 0;
}
