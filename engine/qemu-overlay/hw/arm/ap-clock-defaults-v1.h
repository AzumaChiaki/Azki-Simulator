#ifndef O62M_AP_CLOCK_DEFAULTS_V1_H
#define O62M_AP_CLOCK_DEFAULTS_V1_H
#include "clock-controls.h"
#include "boot-clock-model.h"
/* Original AP6f44 copies67 ROM(addr,value) pairs and writes fullwords at6f6c.
 * Authorize that exact pair/PC only; compound source/divider/gate/auto fields
 * share the current authoritative word. Unknown bits still cannot change.
 * Other writers retain the strict independent-field contract. */
static inline bool o62m_ap_clock_defaults_write(O62MClockControls *m,uint64_t address,
 unsigned size,uint64_t value,uint64_t pc) {
 if(!m||size!=4||value>UINT32_MAX||address>UINT32_MAX||pc!=0x60c46f6c)return false;
 size_t i;
 if(o62m_clock_controls_index(m,address,size,&i)!=O62M_CLOCK_CONTROLS_OK)return false;
 int old=o62m_boot_clock_index((uint32_t)address);
 if(old<0||old>=O62M_BOOT_CLOCK_TABLE_COUNT)return false;
 const O62MBootClockDef *d=&o62m_boot_clock_defs[old];
 if(value!=d->boot_value||((m->word[i]^(uint32_t)value)&~d->mask))return false;
 m->word[i]=(uint32_t)value;return true;
}
#endif
