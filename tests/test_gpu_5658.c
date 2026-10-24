/* SPDX-License-Identifier: GPL-2.0-or-later
 * BGRA5658 is BGR565 in bits 15:0 and alpha in bits 23:16.
 * NXP gpu-vglite VGLite/vg_lite.c convert_source_format gives 0x40000000,
 * convert_target_format gives 0x0a; vg_lite_convert_planar copies RGB then A.
 * ABGR8565 is a distinct format (0x40000010 / 0x1a), not an alias.
 * These fixed byte vectors exercise the production decoder, not a copied
 * implementation. Existing premultiplied blending semantics are preserved.
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
static uint8_t ram[512];
static unsigned checks;
static bool read_ram(uint32_t a,void*out,unsigned n){
    if(a<0x20000000||(uint64_t)a+n>0x20000000+sizeof(ram))return false;
    memcpy(out,ram+(a-0x20000000),n);return true;
}
static uint32_t bits(float x){uint32_t b;memcpy(&b,&x,4);return b;}
static void require(bool ok,const char *why){if(!ok){fprintf(stderr,"FAIL BGRA5658: %s\n",why);exit(1);}checks++;}
static void bytes_equal(const uint8_t *got,const uint8_t *want,unsigned n,const char *why){
    if(memcmp(got,want,n)){fprintf(stderr,"%s got:",why);for(unsigned i=0;i<n;i++)fprintf(stderr," %02x",got[i]);fprintf(stderr,"\n");}
    require(!memcmp(got,want,n),why);
}
static void target_vectors(void){
    const BesGpuTarget t={.format=0x10a,.bpp=3};
    const uint8_t blue[]={0x1f,0x00,0x40};
    BesGpuColor c=gpu_sw_target_load(&t,blue);
    require(c.r==0&&c.g==0&&c.b==255&&c.a==64,"alpha-last target load");
    for(unsigned a=0;a<=255;a+=51){
        uint8_t p[]={0x00,0xf8,a};c=gpu_sw_target_load(&t,p);
        require(c.r==255&&c.g==0&&c.b==0&&c.a==a,"target alpha independent of red");
    }
    uint8_t out[3];gpu_sw_target_store(&t,out,(BesGpuColor){255,128,0,64});
    bytes_equal(out,(const uint8_t[]){0x00,0xfc,0x40},3,"alpha-last target store");
}
static void fixture(uint32_t *r,uint8_t *command,unsigned height){
    memset(r,0,BES_GPU_REGS*sizeof(*r));memset(ram,0,sizeof(ram));memset(command,0,24);
    r[0xa00]=0x10001181;r[0xa10]=0x103;r[0xa11]=0x20000000;r[0xa12]=4;r[0xa13]=(height<<16)|1;
    r[0xa25]=0x41000100;r[0xa29]=0x20000100;r[0xa2b]=4;r[0xa2f]=0x10001;
    r[0xa18]=r[0xa19]=bits(0.5f);r[0xa1a]=bits(1);r[0xa1f]=1;
    stl_le_p(command,0x40000001);stw_le_p(command+12,1);stw_le_p(command+14,height);
}
static BesGpuWork *draw(uint32_t *r,uint8_t *command){
    const char *error=NULL;BesGpuWork*w=gpu_sw_prepare(r,command,24,read_ram,&error);
    if(!w)fprintf(stderr,"decoder rejected: %s\n",error);
    require(w!=NULL,"valid packed source accepted");return w;
}
static void source_vectors(void){
    const uint8_t colors[][3]={{0x00,0xf8,255},{0xe0,0x07,255},{0x1f,0x00,255},{0xff,0xff,255},{0,0,0}};
    const uint8_t expected[][4]={{0,0,255,255},{0,255,0,255},{255,0,0,255},{255,255,255,255},{0,0,0,0}};
    for(unsigned linear=0;linear<2;linear++)for(unsigned i=0;i<G_N_ELEMENTS(colors);i++){
        uint32_t r[BES_GPU_REGS];uint8_t command[24];fixture(r,command,1);r[0xa25]|=linear?0x20000:0;
        memcpy(ram+256,colors[i],3);BesGpuWork*w=draw(r,command);
        bytes_equal(w->targets[0].pixels,expected[i],4,"independent color vector");gpu_sw_free(w);
    }
    /* Premultiplied RGB=(32,32,32) is valid at every selected alpha.
     * Vary alpha alone: output RGB must stay fixed and output A must vary. */
    for(unsigned a=64;a<=256;a+=64){
        uint32_t r[BES_GPU_REGS];uint8_t command[24];fixture(r,command,1);
        unsigned alpha=MIN(a,255);memcpy(ram+256,(uint8_t[]){0x04,0x21,alpha},3);
        BesGpuWork*w=draw(r,command);bytes_equal(w->targets[0].pixels,(uint8_t[]){32,32,32,alpha},4,"independent alpha vector");gpu_sw_free(w);
    }
    /* Halfway between premultiplied red(65,0,0,A=128) and transparent. */
    uint32_t r[BES_GPU_REGS];uint8_t command[24];fixture(r,command,1);
    r[0xa25]=0x41020100;r[0xa2b]=8;r[0xa2f]=0x10002;
    memcpy(ram+256,(uint8_t[]){0x00,0x40,128,0,0,0},6);
    /* Existing two-stage integer blend rounds 65/2 to 32 here. */
    BesGpuWork*w=draw(r,command);bytes_equal(w->targets[0].pixels,(uint8_t[]){0,0,32,64},4,"linear transparent edge");gpu_sw_free(w);
    /* Hardware stride 8 means six packed bytes, including three padding
     * bytes. Rows must not mix color/alpha or sample their padding. */
    fixture(r,command,2);r[0xa2b]=8;r[0xa2f]=0x20001;r[0xa19]=bits(0.25f);r[0xa21]=bits(0.5f);
    memcpy(ram+256,(uint8_t[]){0,0xf8,255,0xac,0xbd,0xce,0xe0,7,255,0x12,0x34,0x56},12);
    w=draw(r,command);bytes_equal(w->targets[0].pixels,(uint8_t[]){0,0,255,255,0,255,0,255},8,"packed row stride");gpu_sw_free(w);
    /* Draw into the same format to exercise target load and store in blend. */
    fixture(r,command,1);r[0xa10]=0x10a;
    memcpy(ram,(uint8_t[]){0x1f,0,255},3);memcpy(ram+256,(uint8_t[]){0,0x40,128},3);
    w=draw(r,command);bytes_equal(w->targets[0].pixels,(uint8_t[]){0x0f,0x40,255},3,"packed target source-over");gpu_sw_free(w);
    /* An alpha-first ABGR8565 value must stay unsupported, not get silently
     * decoded as BGRA5658 just because both have three bytes per pixel. */
    for(unsigned target=0;target<2;target++){
        fixture(r,command,1);if(target)r[0xa10]=0x11a;else r[0xa25]=0x41000110;
        const char*error=NULL;w=gpu_sw_prepare(r,command,24,read_ram,&error);
        require(!w&&error,"distinct alpha-first format rejected");
    }
}
int main(int argc,char **argv){
    if(argc==1||strcmp(argv[1],"source"))target_vectors();
    source_vectors();printf("GPU BGRA5658 regression: %u checks passed\n",checks);return 0;
}
