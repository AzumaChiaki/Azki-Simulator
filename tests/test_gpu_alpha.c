/* SPDX-License-Identifier: GPL-2.0-or-later
 * Regression for the observed BES/LVGL premultiplied image command path.
 * Exact guest capture: A00=10001181 A25=01000107 target=109;
 * source image 144x80, pixel (4,40) RGBA=(127,0,0,128).
 * SDK input remains straight RGBA=(255,0,0,128); the guest premultiplies it.
 * Tests feed the actual decoder, including DATA/END, with fixed blend vectors.
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
static bool read_ram(uint32_t a,void*out,unsigned n){if(a<0x20000000||(uint64_t)a+n>0x20000000+sizeof(ram))return false;memcpy(out,ram+(a-0x20000000),n);return true;}
static uint32_t bits(float x){uint32_t b;memcpy(&b,&x,4);return b;}
static unsigned checks;
static void check(const char*name,uint32_t mode,unsigned target,bool linear,const uint8_t source[8],uint32_t tint,const uint8_t expected[4]){
    uint32_t regs[BES_GPU_REGS]={0};uint8_t command[80]={0};unsigned command_bytes=24;memset(ram,0,sizeof(ram));
    /* Fixed background RGB=(32,40,56), alpha=255. */
    if(target==0x101)stw_le_p(ram,0x2147);else{ram[0]=56;ram[1]=40;ram[2]=32;ram[3]=255;}
    memcpy(ram+256,source,8);regs[0xa00]=mode;regs[0xa02]=tint;regs[0xa10]=target;regs[0xa11]=0x20000000;
    regs[0xa12]=target==0x101?2:4;regs[0xa13]=0x10001;
    regs[0xa25]=linear?0x01020107:0x01000107;regs[0xa29]=0x20000100;regs[0xa2b]=8;regs[0xa2f]=0x00010002;
    regs[0xa18]=bits(linear?0.5f:0.25f);regs[0xa19]=bits(0.5f);regs[0xa1a]=bits(1);regs[0xa1f]=1;
    stl_le_p(command,0x40000001);stw_le_p(command+12,1);stw_le_p(command+14,1);
    if((mode&255)==2){
        uint32_t path[]={2,0,0,4,bits(0.5f),0,4,bits(0.5f),bits(1),4,0,bits(1),1,0};
        regs[0xa34]=0x03300000;regs[0xa3a]=0x10001;regs[0xa40]=regs[0xa44]=bits(1);
        stl_le_p(command,0x40000000|sizeof(path)/8);for(unsigned n=0;n<G_N_ELEMENTS(path);n++)stl_le_p(command+8+n*4,path[n]);command_bytes=sizeof(path)+16;
    }
    const char*error=NULL;BesGpuWork*w=gpu_sw_prepare(regs,command,command_bytes,read_ram,&error);
    if(!w){fprintf(stderr,"FAIL %s: %s\n",name,error);exit(1);}const uint8_t*p=w->targets[0].pixels;uint8_t actual[4];
    if(target==0x101){unsigned v=lduw_le_p(p);actual[0]=(v>>11)*8;actual[1]=((v>>5)&63)*4;actual[2]=(v&31)*8;actual[3]=255;}
    else{actual[0]=p[2];actual[1]=p[1];actual[2]=p[0];actual[3]=target==0x103?p[3]:255;}
    if(memcmp(actual,expected,4)){fprintf(stderr,"FAIL %s target=%x linear=%d actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n",name,target,linear,actual[0],actual[1],actual[2],actual[3],expected[0],expected[1],expected[2],expected[3]);exit(1);}
    checks++;gpu_sw_free(w);
}
int main(void){
    const uint8_t half_red[]={0,0,127,128,0,0,127,128};
    const uint8_t half_green[]={0,127,0,128,0,127,0,128};
    const uint8_t half_blue[]={127,0,0,128,127,0,0,128};
    const uint8_t mixed[]={30,50,100,128,30,50,100,128};
    const uint8_t edge[]={0,0,127,128,0,0,0,0};
    const uint8_t transparent[]={0,0,0,0,0,0,0,0};
    const uint8_t opaque[]={0,0,255,255,0,0,255,255};
    for(unsigned i=0;i<2;i++)for(unsigned j=0;j<3;j++){
        unsigned target=(unsigned[]){0x109,0x103,0x101}[j];bool packed=target==0x101;
        check("captured premultiplied half red",0x10001181,target,i,half_red,0,(const uint8_t[4]){packed?136:143,20,packed?24:28,255});
        check("premultiplied half green",0x10001181,target,i,half_green,0,(const uint8_t[4]){16,packed?144:147,packed?24:28,255});
        check("premultiplied half blue",0x10001181,target,i,half_blue,0,(const uint8_t[4]){16,20,packed?152:155,255});
        check("premultiplied mixed color",0x10001181,target,i,mixed,0,(const uint8_t[4]){packed?112:116,packed?68:70,packed?56:58,255});
        check("legacy straight mixed color",0x2181,target,i,mixed,0,(const uint8_t[4]){packed?64:66,packed?44:45,packed?40:43,255});
        check("opaque unchanged",0x10001181,target,i,opaque,0,(const uint8_t[4]){packed?248:255,0,0,255});
        check("transparent unchanged",0x10001181,target,i,transparent,0,(const uint8_t[4]){32,40,56,255});
        check("multiply opacity once",0x10002181,target,i,half_red,0x80ffffff,(const uint8_t[4]){packed?80:87,packed?28:30,packed?40:42,255});
    }
    check("linear transparent edge",0x10001181,0x109,true,edge,0,(const uint8_t[4]){87,30,42,255});
    for(unsigned i=0;i<2;i++){
        check("half path coverage",0x10001102,0x109,i,half_red,0,(const uint8_t[4]){87,30,42,255});
        check("half coverage and multiply opacity",0x10002102,0x109,i,half_red,0x80ffffff,(const uint8_t[4]){60,35,49,255});
        check("colored multiply",0x10002181,0x109,i,mixed,0xffc08040,(const uint8_t[4]){41,45,50,255});
        check("colored multiply half coverage",0x10002102,0x109,i,mixed,0xffc08040,(const uint8_t[4]){37,43,53,255});
    }
    printf("GPU alpha regression: %u checks passed\n",checks);return 0;
}
