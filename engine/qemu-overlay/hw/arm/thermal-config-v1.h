#ifndef O62M_THERMAL_CONFIG_V1_H
#define O62M_THERMAL_CONFIG_V1_H
#include <stdbool.h>
#include <stdint.h>
/* Original 60dd0 initialization writes only. Record the values the original
 * floating-point driver computed, with no invented measurement/ready/IRQ. */
typedef struct O62MThermalConfig {uint32_t word[8];bool written[8];} O62MThermalConfig;
static inline bool o62m_thermal_config_write(O62MThermalConfig *m,uint64_t address,
 unsigned size,uint64_t value,uint64_t pc) {
 static const uint32_t offsets[8]={0x4c,0x2c,0x3c,0x44,0x40,0x04,0x48,0x08};
 static const uint64_t pcs[8]={0x73e60df4,0x73e60e02,0x73e60e06,0x73e60e0c,0x73e60ea4,0x73e60eae,0x73e60f06,0x73e60f0a};
 if(!m||size!=4||value>UINT32_MAX) return false;
 for(unsigned i=0;i<8;i++)if(address==0x5a110000+offsets[i]) {
  bool original=pc==pcs[i]||(i==2&&pc==0x73e60f40)||(i==7&&pc==0x73e60f44);
  if(!original) return false;
  m->word[i]=(uint32_t)value;m->written[i]=true;return true;
 }
 return false;
}
#endif
