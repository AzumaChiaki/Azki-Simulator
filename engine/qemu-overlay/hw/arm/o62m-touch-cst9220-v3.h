/* SPDX-License-Identifier: MIT
 * CST9220 virtual peer. Firmware metadata describes the original vendor
 * image supplied with this board profile. Reports are host contact input.
 */
#ifndef O62M_TOUCH_CST9220_V3_H
#define O62M_TOUCH_CST9220_V3_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef struct O62MTouchCST9220V3 {
    uint8_t tx[16], tx_count, report[10], queue[16][10];
    uint8_t head,count,mode,sleep,mem_type;
    uint16_t pointer,mem_address,x,y;
    bool bootloader,mem_ready,host_down,rearm,powered,irq_low,pending;
    uint64_t inputs,acks,resets,reads,writes;
    uint64_t heartbeat_ns, heartbeat_last_ns;
    bool heartbeat_running;
} O62MTouchCST9220V3;
static inline void o62m_touch_reset_v3(O62MTouchCST9220V3 *t)
{
    uint64_t inputs=t->inputs,acks=t->acks,resets=t->resets+1;
    uint64_t reads=t->reads,writes=t->writes;
    memset(t,0,sizeof(*t));t->inputs=inputs;t->acks=acks;t->resets=resets;t->reads=reads;t->writes=writes;
    t->report[6]=0xab;
}
static inline void o62m_touch_clock_v3(O62MTouchCST9220V3 *t,uint64_t now)
{
    if(t->heartbeat_running&&now>=t->heartbeat_last_ns)t->heartbeat_ns+=now-t->heartbeat_last_ns;
    t->heartbeat_last_ns=now;
    t->heartbeat_running=t->powered&&!t->bootloader&&!t->sleep;
}
static inline bool o62m_touch_scanning_v3(const O62MTouchCST9220V3 *t)
{
    return t->powered&&!t->bootloader&&!t->sleep&&(t->mode==0||t->mode==9);
}
static inline bool o62m_touch_contact_v3(O62MTouchCST9220V3 *t,unsigned x,unsigned y,bool down)
{
    if(!o62m_touch_scanning_v3(t)||x>=466||y>=466)return false;
    if((!down&&!t->host_down)||(down==t->host_down&&x==t->x&&y==t->y))return true;
    bool replace_move=down&&t->host_down&&t->count>=2&&
        t->queue[(t->head+t->count-1)%16][0]==7;
    /* Keep one slot for the release of the currently accepted contact. */
    if(!replace_move&&(t->count==16||(down&&t->count==15)))return false;
    unsigned tail=(t->head+t->count-(replace_move?1:0))%16;
    uint8_t *r=t->queue[tail];memset(r,0,10);
    r[0]=down?(t->host_down?7:6):0;r[1]=x>>4;r[2]=y>>4;r[3]=(x<<4)|(y&15);
    r[4]=(r[0]+r[1]+r[2]+r[3])&15;r[5]=1;r[6]=0xab;
    if(!replace_move)t->count++;t->inputs++;t->x=x;t->y=y;t->host_down=down;
    if(t->count==1){memcpy(t->report,r,10);t->rearm=true;}
    return true;
}
static inline bool o62m_touch_read_v3(O62MTouchCST9220V3 *t,uint16_t reg,uint8_t *out)
{
    uint32_t v;
    if(reg>=0xd000&&reg<0xd00a){*out=t->report[reg-0xd000];return true;}
    if(reg>=0xa002&&reg<0xa004&&t->bootloader){*out=reg==0xa002?0x55:0xb0;return true;}
    if(reg==0xa004&&t->mem_ready){*out=0;return true;}
    if(reg>=0xa018&&reg<0xa01c&&t->mem_ready){
        if(t->mem_type==1&&t->mem_address==0x077c)v=0xcaca9220;
        else if(t->mem_type==0&&t->mem_address==0x7fc0)v=1;
        else return false;
        *out=v>>((reg-0xa018)*8);return true;
    }
    if(reg==0xd048&&t->powered){*out=0x20|((t->heartbeat_ns/100000000u)&1u)*0x80;return true;}
    if(reg==2||reg==3){*out=reg==2?t->sleep:t->mode;return true;}
    switch(reg&~3u){
    case 0xd1fc:v=0xcbcb4e20;break;
    case 0xd204:v=0x9220320a;break;
    case 0xd208:v=0x01070000;break;
    case 0xd21c:v=0xb49cd5d4;break;
    case 0xd20c:v=0x2a317fef;break;
    case 0xd1f4:v=0x0014000a;break;
    default:return false;
    }
    *out=v>>((reg&3)*8);return true;
}
static inline bool o62m_touch_packet_v3(O62MTouchCST9220V3 *t)
{
    if(!t->tx_count)return true;
    if(t->tx_count<2)return false;
    uint16_t reg=(t->tx[0]<<8)|t->tx[1];unsigned n=t->tx_count-2;
    uint8_t v=t->tx[2];bool ok=false;
    if(!n){
        switch(reg){
        case 0xd109:t->mode=9;t->sleep=0;break;
        case 0xd11e:t->mode=0x1e;t->sleep=0;break;
        case 0xd101:t->mode=1;t->sleep=0;break;
        case 0xd105:t->mode=5;t->sleep=1;break;
        case 0xd107:t->mode=7;t->sleep=2;break;
        case 0xd104:t->mode=4;t->sleep=3;break;
        default:break;
        }
        ok=true;
    } else if(reg==0xa001&&n==1&&(v==0xaa||v==0)){
        t->bootloader=v==0xaa;ok=true;
    } else if(reg==0xa010&&n==1&&v<=1){
        t->mem_type=v;t->mem_address=0;t->mem_ready=false;ok=true;
    } else if(reg==0xa00c&&n==2){
        uint16_t addr=t->tx[2]|(t->tx[3]<<8);
        if((t->mem_type==1&&addr==0x077c)||(t->mem_type==0&&addr==0x7fc0)){
            t->mem_address=addr;t->mem_ready=false;ok=true;
        }
    } else if(reg==0xa004&&n==1&&v==0xe4&&t->mem_address){t->mem_ready=true;ok=true;
    } else if(reg==5&&n==1&&v==0){
        if(t->count){
            memset(t->queue[t->head],0,10);t->head=(t->head+1)%16;t->count--;t->acks++;
            if(t->count)memcpy(t->report,t->queue[t->head],10);
            else {memset(t->report,0,10);t->report[6]=0xab;}
        }
        t->irq_low=false;t->rearm=t->count!=0;ok=true;
    }
    if(!ok)return false;
    t->pointer=reg;t->tx_count=0;t->writes++;return true;
}
#endif
