#ifndef O62M_RFSPIM_PROGRESS_V1_H
#define O62M_RFSPIM_PROGRESS_V1_H
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
typedef struct {
 uint64_t remaining,rate_hz;
 int64_t last_ns;
 bool active;
} O62MRFSPIMProgressV1;
/* Work is measured in billionths of a functional clock cycle. */
static inline bool o62m_rfspim_progress_start_v1(O62MRFSPIMProgressV1 *p,int64_t now,uint64_t cycles,uint64_t rate)
{
 if(p->active||now<0||!cycles||cycles>UINT64_MAX/1000000000)return false;
 p->remaining=cycles*1000000000;p->rate_hz=rate;p->last_ns=now;p->active=true;return true;
}
static inline bool o62m_rfspim_progress_settle_v1(O62MRFSPIMProgressV1 *p,int64_t now)
{
 if(!p->active||now<p->last_ns)return false;
 uint64_t dt=(uint64_t)(now-p->last_ns);
 if(p->rate_hz&&p->remaining) {
  uint64_t due=p->remaining/p->rate_hz+(p->remaining%p->rate_hz!=0);
  if(dt>=due)p->remaining=0;
  else p->remaining-=dt*p->rate_hz;
 }
 p->last_ns=now;return true;
}
static inline bool o62m_rfspim_progress_rate_v1(O62MRFSPIMProgressV1 *p,int64_t now,uint64_t rate)
{
 if(!o62m_rfspim_progress_settle_v1(p,now))return false;
 p->rate_hz=rate;return true;
}
static inline bool o62m_rfspim_progress_deadline_v1(const O62MRFSPIMProgressV1 *p,int64_t *deadline)
{
 if(!p->active||(!p->rate_hz&&p->remaining))return false;
 uint64_t delta=p->remaining?(p->remaining/p->rate_hz+(p->remaining%p->rate_hz!=0)):0;
 if(delta>(uint64_t)(INT64_MAX-p->last_ns))return false;
 *deadline=p->last_ns+(int64_t)delta;return true;
}
static inline void o62m_rfspim_progress_reset_v1(O62MRFSPIMProgressV1 *p)
{*p=(O62MRFSPIMProgressV1){0};}
#endif
