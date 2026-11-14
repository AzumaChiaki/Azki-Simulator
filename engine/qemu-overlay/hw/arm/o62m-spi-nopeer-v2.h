#ifndef O62M_SPI_NOPEER_V2_H
#define O62M_SPI_NOPEER_V2_H
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#define O62M_SPI_DEPTH 64u
#define O62M_SPI_HISTORY 4096u
typedef struct { uint8_t tx, outcome; } O62MSPIFrameV1;
typedef struct {
    O62MSPIFrameV1 history[O62M_SPI_HISTORY];
    uint16_t tx[O62M_SPI_DEPTH], active;
    uint8_t rx[O62M_SPI_DEPTH];
    unsigned tx_head,tx_count,rx_head,rx_count,history_count;
    uint64_t generation,completed,cancelled,received;
    uint32_t control,baud;
    int64_t deadline_ns;
    bool pending;
} O62MSPINoPeerV1;
static inline bool o62m_spi_mode_v1(uint32_t control,uint32_t baud)
{
    return ((control>>16)&31)==7 && !(control&((3u<<21)|(3u<<8)|(3u<<4)|(1u<<11))) &&
        (baud&65535)>=2 && !(baud&1);
}
static inline bool o62m_spi_start_v1(O62MSPINoPeerV1 *s,int64_t now,uint32_t control,uint32_t baud)
{
    uint64_t duration=(UINT64_C(8000000000)*(baud&65535)+25999999)/26000000;
    if(s->pending||!s->tx_count||!o62m_spi_mode_v1(control,baud)||now<0||
       duration>(uint64_t)(INT64_MAX-now)||s->generation==UINT64_MAX)return false;
    s->active=s->tx[s->tx_head];s->tx_head=(s->tx_head+1)%64;--s->tx_count;
    s->control=control;s->baud=baud;s->deadline_ns=now+(int64_t)duration;
    s->pending=true;++s->generation;return true;
}
static inline bool o62m_spi_write_v1(O62MSPINoPeerV1 *s,uint32_t value,int64_t now,uint32_t control,uint32_t baud)
{
    if(s->tx_count==64||s->history_count==O62M_SPI_HISTORY||!o62m_spi_mode_v1(control,baud))return false;
    O62MSPINoPeerV1 trial=*s;
    unsigned index=trial.history_count++;
    trial.history[index]=(O62MSPIFrameV1){(uint8_t)value,0};
    trial.tx[(trial.tx_head+trial.tx_count)%64]=(uint16_t)index;++trial.tx_count;
    if(!trial.pending&&!o62m_spi_start_v1(&trial,now,control,baud))return false;
    *s=trial;return true;
}
static inline bool o62m_spi_complete_v1(O62MSPINoPeerV1 *s,uint64_t generation,int64_t now,uint32_t control,uint32_t baud)
{
    if(!s->pending||generation!=s->generation||now<s->deadline_ns||
       control!=s->control||baud!=s->baud||s->rx_count==64)return false;
    O62MSPINoPeerV1 trial=*s;
    trial.history[trial.active].outcome=1;++trial.completed;
    trial.rx[(trial.rx_head+trial.rx_count)%64]=255;++trial.rx_count;trial.pending=false;
    if(trial.tx_count&&!o62m_spi_start_v1(&trial,now,control,baud))return false;
    *s=trial;return true;
}
static inline bool o62m_spi_read_v1(O62MSPINoPeerV1 *s,uint32_t *value)
{
    if(!s->rx_count)return false;
    *value=s->rx[s->rx_head];s->rx_head=(s->rx_head+1)%64;--s->rx_count;++s->received;return true;
}
static inline uint32_t o62m_spi_risr_v1(const O62MSPINoPeerV1 *s,uint32_t tx_threshold,uint32_t rx_threshold)
{
    return (s->tx_count<=tx_threshold?1u:0u)|(s->rx_count>rx_threshold?16u:0u);
}
static inline void o62m_spi_cancel_v1(O62MSPINoPeerV1 *s)
{
    if(s->pending){s->history[s->active].outcome=2;++s->cancelled;}
    while(s->tx_count){s->history[s->tx[s->tx_head]].outcome=2;++s->cancelled;
        s->tx_head=(s->tx_head+1)%64;--s->tx_count;}
    s->pending=false;s->rx_count=0;s->rx_head=0;s->tx_head=0;
    if(s->generation!=UINT64_MAX)++s->generation;
}
#endif
