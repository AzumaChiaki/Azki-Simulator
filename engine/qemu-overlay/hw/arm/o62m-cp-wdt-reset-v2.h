#ifndef O62M_CP_WDT_RESET_V2_H
#define O62M_CP_WDT_RESET_V2_H
#include "o62m-wdt4012-functional-v2.h"
typedef struct O62MCPWdtResetV2 {
    O62MWdt4012V2 core;
    uint32_t control;
    uint64_t assertions,releases;
    bool held;
} O62MCPWdtResetV2;
static inline void o62m_cp_wdt_reset_init_v2(O62MCPWdtResetV2 *m)
{
    memset(m,0,sizeof(*m));
    m->control=4;
    o62m_wdt4012_init_v2(&m->core,false,0,0,0,NULL,0,0);
}
static inline bool o62m_cp_wdt_reset_write_v2(O62MCPWdtResetV2 *m,uint32_t value)
{
    if((value^m->control)&~UINT32_C(4))return false;
    if(!(value&4)) {
        o62m_wdt4012_init_v2(&m->core,false,0,0,0,NULL,0,0);
        if(!m->held)++m->assertions;
        m->held=true;
    } else if(m->held) {
        m->held=false;++m->releases;
    }
    m->control=value;
    return true;
}
#endif
