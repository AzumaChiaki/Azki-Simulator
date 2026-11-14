#ifndef O62M_UART1_BOOT_CONFIG_V6_H
#define O62M_UART1_BOOT_CONFIG_V6_H
#include "uart-tx-model.h"

typedef struct O62MUart1BootConfigV1 {
    uint8_t dll, dlm, lcr, ier, fcr, mcr, dlf, rx_count;
    O62MUartTx tx;
    uint64_t resets, accesses;
} O62MUart1BootConfigV1;

static inline void o62m_uart1_boot_init_v1(O62MUart1BootConfigV1 *s)
{
    memset(s,0,sizeof(*s));o62m_uart_tx_init(&s->tx,1);
}
static inline bool o62m_uart1_empty_irq_v1(const O62MUart1BootConfigV1 *s)
{
    return o62m_uart_tx_irq(&s->tx);
}
static inline bool o62m_uart1_send_v1(O62MUart1BootConfigV1 *s,uint8_t byte,
        int64_t now,uint32_t source_hz,bool cts)
{
    O62MUart1BootConfigV1 trial=*s;
    if((s->lcr&0x80)||(s->mcr&0x10)||
       !o62m_uart_tx_configure(&trial.tx,source_hz,s->dll|((uint16_t)s->dlm<<8),s->dlf,s->lcr,s->fcr&1)||
       !o62m_uart_tx_set_running(&trial.tx,!(s->mcr&0x20)||cts,now)||
       !o62m_uart_tx_write(&trial.tx,byte,now))return false;
    trial.accesses++;*s=trial;return true;
}
/* Configuration and status share the same holding queue and active shifter. */
static inline bool o62m_uart1_boot_access_v1(O62MUart1BootConfigV1 *state,
        unsigned offset,unsigned size,bool write,uint64_t *value)
{
    O62MUart1BootConfigV1 trial=*state,*s=&trial;
    uint8_t *reg=NULL;uint64_t data=*value;
    bool work=s->tx.active||s->tx.count;
    if(size!=4 || (offset&3) || (write&&data>255))return false;
    switch(offset) {
    case 0:
        if(!(s->lcr&0x80))return false;
        reg=&s->dll;break;
    case 4:
        if(s->lcr&0x80)reg=&s->dlm;
        else {if(write&&(data&~UINT64_C(0x8f)))return false;reg=&s->ier;}
        break;
    case 8:
        if(!write){*value=o62m_uart_tx_iir(&s->tx);break;}
        if(work&&((data^s->fcr)&1))return false;
        if(data&2)s->rx_count=0;
        if(data&4)o62m_uart_tx_fifo_reset(&s->tx);
        s->fcr=data&0xf9;break;
    case 0x0c:reg=&s->lcr;break;
    case 0x10:
        if(write&&(data&~UINT64_C(0x3f)))return false;
        reg=&s->mcr;break;
    case 0x14:
        if(write)return false;
        *value=o62m_uart_tx_lsr(&s->tx)|(s->rx_count?1u:0u);break;
    case 0xc0:
        if(write&&data>15)return false;
        reg=&s->dlf;break;
    case 0x7c:
        if(write)return false;
        *value=o62m_uart_tx_usr(&s->tx)|(s->rx_count?8u:0u)|(s->rx_count==1?16u:0u);break;
    case 0x98:
        if(write){if(data>1||(work&&data!=(s->fcr&1)))return false;s->fcr=(s->fcr&~1u)|data;}
        else *value=s->fcr&1u;
        break;
    case 0x9c:
        if(write){if(data>3)return false;s->fcr=(s->fcr&~0xc0u)|(data<<6);}
        else *value=(s->fcr>>6)&3u;
        break;
    case 0xa0:
        if(write){if(data>3)return false;s->fcr=(s->fcr&~0x30u)|(data<<4);}
        else *value=(s->fcr>>4)&3u;
        break;
    case 0x88:
        if(!write||(data&~UINT64_C(7)))return false;
        if(data&1) {
            uint64_t accesses=s->accesses,resets=s->resets;
            uint64_t discarded=s->tx.discarded+s->tx.count+s->tx.active;
            o62m_uart1_boot_init_v1(s);
            s->accesses=accesses;s->resets=resets;s->tx.discarded=discarded;
        }
        if(data&2)s->rx_count=0;
        if(data&4)o62m_uart_tx_fifo_reset(&s->tx);
        s->resets++;break;
    default:return false;
    }
    if(reg) {
        if(write) {
            if(work&&((reg==&s->dll||reg==&s->dlm||reg==&s->dlf)&&data!=*reg))return false;
            if(work&&reg==&s->lcr&&((data^*reg)&0x7f))return false;
            if(work&&reg==&s->mcr&&((data^*reg)&0x30))return false;
            *reg=data;
        }else *value=*reg;
    }
    s->tx.fifo=s->fcr&1;
    o62m_uart_tx_irq_config(&s->tx,s->ier&2,s->ier&0x80,(s->fcr>>4)&3);
    s->accesses++;*state=trial;return true;
}
#endif
