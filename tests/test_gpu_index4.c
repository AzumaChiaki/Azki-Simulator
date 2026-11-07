#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define qemu_log_mask(...) ((void)0)
static unsigned lduw_le_p(const void *v){const uint8_t*p=v;return p[0]|p[1]<<8;}
static uint32_t ldl_le_p(const void *v){const uint8_t*p=v;return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void stw_le_p(void*v,unsigned n){uint8_t*p=v;p[0]=n;p[1]=n>>8;}
static void stl_le_p(void*v,uint32_t n){uint8_t*p=v;p[0]=n;p[1]=n>>8;p[2]=n>>16;p[3]=n>>24;}
#include "../engine/qemu-overlay/hw/arm/best1503-gpu.h"
#define BASE 0x20000000u
static uint8_t ram[8192], command[2048];
static unsigned used;
static bool read_ram(uint32_t a,void*out,unsigned n){
    if(a<BASE||(uint64_t)a+n>BASE+sizeof(ram))return false;
    memcpy(out,ram+(a-BASE),n);return true;
}
static uint32_t bits(float x){uint32_t b;memcpy(&b,&x,4);return b;}
static void state(unsigned reg,uint32_t value){
    stl_le_p(command+used,0x30010000|reg);stl_le_p(command+used+4,value);used+=8;
}
static void draw(unsigned source,unsigned target){
    state(0xa00,0x10001081);state(0xa02,0xffffffff);
    state(0xa10,0x103);state(0xa11,target);state(0xa12,16);state(0xa13,4|1<<16);
    state(0xa25,0x01000700);state(0xa29,source);state(0xa2b,4);state(0xa2f,4|1<<16);
    state(0xb01,0xff102030);state(0xb02,0xff405060);state(0xb03,0xff708090);state(0xb04,0xffa0b0c0);
    state(0xa18,bits(.125));state(0xa19,bits(.5));state(0xa1a,bits(1));
    state(0xa1c,bits(.25));state(0xa21,bits(1));state(0xa1f,1);
    stl_le_p(command+used,0x40000001);stl_le_p(command+used+4,0);
    stl_le_p(command+used+8,0);stl_le_p(command+used+12,4|1<<16);used+=16;
}
int main(void){
    /* Low nibble is the left pixel: indices 1,2 then 3,4. */
    const uint8_t source[]={0x21,0x43,0,0};
    const uint8_t expected[]={0x30,0x20,0x10,255,0x60,0x50,0x40,255,
                              0x90,0x80,0x70,255,0xc0,0xb0,0xa0,255};
    memset(ram,0xa5,sizeof(ram));memcpy(ram+4096,source,sizeof(source));
    draw(BASE+4096,BASE);
    stl_le_p(command+used,5);used+=8;
    uint32_t regs[BES_GPU_REGS]={0};const char*error=0;
    BesGpuWork*w=gpu_sw_prepare(regs,command,used,read_ram,&error);
    if(!w){fprintf(stderr,"%s\n",error);return 1;}
    if(memcmp(w->targets[0].pixels,expected,16)){
        for(unsigned i=0;i<16;i++)fprintf(stderr,"%02x ",w->targets[0].pixels[i]);
        fprintf(stderr,"\n4-bit indexed source failed\n");return 1;
    }
    gpu_sw_free(w);puts("GPU 4-bit indexed source passed");return 0;
}
