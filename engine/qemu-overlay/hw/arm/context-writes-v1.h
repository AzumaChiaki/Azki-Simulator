#ifndef O62M_CONTEXT_WRITES_V1_H
#define O62M_CONTEXT_WRITES_V1_H
#include <stdbool.h>
#include <stdint.h>
/* Original instructions only; no reset/ready/IRQ, data reads, or execution of
 * the hardware context engine. Preserve actual fullword writes for diagnostics.
 * Every other transaction fails without altering state or caller output. */
#define O62M_CONTEXT_WRITE_COUNT 6u
typedef struct O62MContextWrites { uint32_t word[6]; bool written[6]; } O62MContextWrites;
static inline bool o62m_context_write(O62MContextWrites *m, uint64_t address,
        unsigned size, uint64_t value, uint64_t pc) {
    static const uint64_t addresses[6]={0x5c0101a4,0x5c01008c,0x5c010180,0x5c010090,0x5c010094,0x5c010184};
    static const uint64_t pcs[6]={0x73e5c702,0x73e5c220,0x73e5c22a,0x73e5c234,0x73e5c23e,0x73e5c248};
    if (!m || size!=4 || value>UINT32_MAX) return false;
    for (unsigned i=0;i<6;i++) if (address==addresses[i]) {
        if (pc!=pcs[i] || (!i && value!=0x000fffff)) return false;
        m->word[i]=(uint32_t)value; m->written[i]=true; return true;
    }
    return false;
}
#endif
