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
static uint8_t ram[12288], command[2048];
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
    state(0xa25,0x01000107);state(0xa29,source);state(0xa2b,16);state(0xa2f,4|1<<16);
    state(0xa18,bits(.125));state(0xa19,bits(.5));state(0xa1a,bits(1));
    state(0xa1c,bits(.25));state(0xa21,bits(1));state(0xa1f,1);
    stl_le_p(command+used,0x40000001);stl_le_p(command+used+4,0);
    stl_le_p(command+used+8,0);stl_le_p(command+used+12,4|1<<16);used+=16;
}
int main(void){
    const uint8_t source[]={0,0,0,0,20,40,60,128,255,127,0,255,8,16,24,32};
    memset(ram,0xa5,sizeof(ram));memcpy(ram+4096,source,sizeof(source));
    draw(BASE+4096,BASE);draw(BASE,BASE+8192);
    stl_le_p(command+used,5);used+=8;
    uint32_t regs[BES_GPU_REGS]={0};const char*error=0;
    BesGpuWork*w=gpu_sw_prepare(regs,command,used,read_ram,&error);
    if(!w){fprintf(stderr,"%s\n",error);return 1;}
    if(w->target_count!=2||memcmp(w->targets[0].pixels,source,16)||
       memcmp(w->targets[1].pixels,source,16)||ram[0]!=0xa5){
        fprintf(stderr,"NONE copy or ordered intermediate draw failed\n");return 1;
    }
    gpu_sw_free(w);puts("GPU transparent replacement and intermediate copy passed");return 0;
}
