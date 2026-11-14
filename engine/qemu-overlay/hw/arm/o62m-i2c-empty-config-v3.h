/* SPDX-License-Identifier: MIT
 * Explicit virtual fresh-empty DW-style I2C config view. Original AP init
 * and local pinned U-Boot corroborate offsets. No peer or DR transaction
 * is implemented, so no transfer completion/IRQ is ever fabricated.
 */
#ifndef O62M_I2C_EMPTY_CONFIG_V3_H
#define O62M_I2C_EMPTY_CONFIG_V3_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define O62M_I2C_EMPTY_CFG_COUNT_V3 12u
typedef struct O62MI2CEmptyConfigV3 {
    uint32_t config[O62M_I2C_EMPTY_CFG_COUNT_V3];
    uint8_t tx_count,rx_count;
    bool active;
    uint64_t reads,writes;
} O62MI2CEmptyConfigV3;
/* FIFO capacity is a declared virtual capability, not inferred from RXTL7. */
#define O62M_I2C_EMPTY_FIFO_CAPACITY_V3 16u
static inline int o62m_i2c_empty_config_index_v3(uint32_t offset)
{
    static const uint8_t offsets[O62M_I2C_EMPTY_CFG_COUNT_V3]={
        0x00,0x04,0x14,0x18,0x1c,0x20,0x24,0x28,0x30,0x38,0x3c,0x6c
    };
    for(unsigned i=0;i<O62M_I2C_EMPTY_CFG_COUNT_V3;i++)if(offset==offsets[i])return (int)i;
    return -1;
}
static inline bool o62m_i2c_empty_config_valid_v3(const O62MI2CEmptyConfigV3 *s)
{
    /* All DR accesses remain unsupported: fresh real queues have no entries
     * and no active job. Do not silently report idle over a nonempty state. */
    return s&&!s->tx_count&&!s->rx_count&&!s->active&&
           !(s->config[0]&~UINT32_C(0x3ff))&&!(s->config[1]&~UINT32_C(0x13ff))&&
           !s->config[8]&&s->config[9]<O62M_I2C_EMPTY_FIFO_CAPACITY_V3&&
           s->config[10]<O62M_I2C_EMPTY_FIFO_CAPACITY_V3&&s->config[11]<=1;
}
static inline bool o62m_i2c_empty_config_access_v3(O62MI2CEmptyConfigV3 *s,
                                                  uint32_t offset,unsigned size,
                                                  bool write,uint64_t *value)
{
    if(!value||!o62m_i2c_empty_config_valid_v3(s)||size!=4||(offset&3)||
       (write&&*value>UINT32_MAX)||(write?s->writes:s->reads)==UINT64_MAX)return false;
    int index=o62m_i2c_empty_config_index_v3(offset);
    uint32_t result=0;
    if(write) {
        if(index<0)return false;
        uint32_t data=(uint32_t)*value;
        if((offset==0x00&&(data&~UINT32_C(0x3ff)))||
           (offset==0x04&&(data&~UINT32_C(0x13ff)))||
           (offset==0x30&&data)||
           ((offset==0x38||offset==0x3c)&&data>=O62M_I2C_EMPTY_FIFO_CAPACITY_V3)||
           (offset==0x6c&&data>1))return false;
        /* Control/timing updates require disabled state. Enable alone has
         * no queued DR work and therefore never creates activity/completion. */
        if(s->config[11]&&offset!=0x6c&&offset!=0x30)return false;
        s->config[index]=data;s->writes++;return true;
    }
    if(index>=0)result=s->config[index];
    else if(offset==0x70) {
        if(s->active)result|=1u;
        if(s->tx_count<O62M_I2C_EMPTY_FIFO_CAPACITY_V3)result|=2u;
        if(!s->tx_count)result|=4u;
        if(s->rx_count)result|=8u;
        if(s->rx_count==O62M_I2C_EMPTY_FIFO_CAPACITY_V3)result|=16u;
    } else if(offset==0x34) {
        /* Explicit functional level-comparator policy, not a physical-IP
         * timing attestation. No latched error source/job has been created
         * in this strict fresh-empty profile. CLR40 cannot erase a level. */
        if(s->tx_count<=s->config[10])result|=UINT32_C(0x10);
        if(s->rx_count>s->config[9])result|=UINT32_C(0x04);
    } else if(offset==0x74)result=s->tx_count;
    else if(offset==0x78)result=s->rx_count;
    else if(offset==0x40) {
        /* Only fresh-empty/no error/no implemented event source, not a claim
         * about future all-event ACK semantics or TX_EMPTY comparator. */
        result=0;
    } else return false; /* DR10/INTR2c/otherclear/ID/holes strict. */
    *value=result;s->reads++;return true;
}
#endif
