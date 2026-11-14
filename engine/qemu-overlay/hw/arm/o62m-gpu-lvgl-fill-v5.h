/* SPDX-License-Identifier: MIT */
#ifndef O62M_GPU_LVGL_FILL_V5_H
#define O62M_GPU_LVGL_FILL_V5_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#define O62M_FILL_MAX_TILES 64
#define O62M_FILL_MAX_EDGES 1024
#define O62M_FILL_MAX_DIM 1024
#define O62M_FILL_MAX_BYTES (4u*1024u*1024u)
typedef struct { double x,y; } O62MFillPointV1;
typedef struct { O62MFillPointV1 a,b; } O62MFillEdgeV1;
typedef struct { unsigned x,y,w,h; } O62MFillTileV1;
typedef struct {
    uint32_t final_state[256];bool final_valid[256];
    uint32_t target,stride,width,height,color,control,format;
    uint32_t tess_address,tess_bytes,count_address,count_bytes;
    uint32_t matrix_words[6];
    O62MFillTileV1 tiles[O62M_FILL_MAX_TILES];
    O62MFillEdgeV1 edges[O62M_FILL_MAX_EDGES];
    unsigned tile_count,edge_count;
    uint64_t clip_pixels,covered_samples,changed_pixels;
    uint8_t path[136];
} O62MGpuFillV1;
enum { O62M_FILL_OK, O62M_FILL_FRAMING, O62M_FILL_STATE, O62M_FILL_MODE,
       O62M_FILL_TARGET, O62M_FILL_PATH, O62M_FILL_TILE, O62M_FILL_ORDER };
static inline uint32_t o62m_fill_u32_v1(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static inline double o62m_fill_float_v1(uint32_t u)
{ float f;memcpy(&f,&u,4);return f; }
static inline bool o62m_fill_edge_v1(O62MGpuFillV1 *p,O62MFillPointV1 a,O62MFillPointV1 b)
{
    if(a.y==b.y)return true;
    if(p->edge_count==O62M_FILL_MAX_EDGES)return false;
    p->edges[p->edge_count++]=(O62MFillEdgeV1){a,b};return true;
}
static inline O62MFillPointV1 o62m_fill_mid_v1(O62MFillPointV1 a,O62MFillPointV1 b)
{ return (O62MFillPointV1){(a.x+b.x)*.5,(a.y+b.y)*.5}; }
static inline bool o62m_fill_cubic_v1(O62MGpuFillV1 *p,O62MFillPointV1 a,O62MFillPointV1 b,
                                     O62MFillPointV1 c,O62MFillPointV1 d,unsigned depth)
{
    double dx=d.x-a.x,dy=d.y-a.y,chord=hypot(dx,dy);
    double poly=hypot(b.x-a.x,b.y-a.y)+hypot(c.x-b.x,c.y-b.y)+hypot(d.x-c.x,d.y-c.y);
    double error=chord>0?fmax(fabs((b.x-a.x)*dy-(b.y-a.y)*dx),fabs((c.x-a.x)*dy-(c.y-a.y)*dx))/chord:poly;
    if(error<=.0625&&poly-chord<=.0625)return o62m_fill_edge_v1(p,a,d);
    if(depth==12)return false;
    O62MFillPointV1 ab=o62m_fill_mid_v1(a,b),bc=o62m_fill_mid_v1(b,c),cd=o62m_fill_mid_v1(c,d);
    O62MFillPointV1 abc=o62m_fill_mid_v1(ab,bc),bcd=o62m_fill_mid_v1(bc,cd),mid=o62m_fill_mid_v1(abc,bcd);
    return o62m_fill_cubic_v1(p,a,ab,abc,mid,depth+1)&&o62m_fill_cubic_v1(p,mid,bcd,cd,d,depth+1);
}
static inline bool o62m_fill_point_v1(const uint8_t *raw,const double *m,O62MFillPointV1 *p)
{
    double x=o62m_fill_float_v1(o62m_fill_u32_v1(raw)),y=o62m_fill_float_v1(o62m_fill_u32_v1(raw+4));
    p->x=m[0]*x+m[1]*y+m[2];p->y=m[3]*x+m[4]*y+m[5];
    return isfinite(x)&&isfinite(y)&&isfinite(p->x)&&isfinite(p->y)&&fabs(p->x)<=1048576&&fabs(p->y)<=1048576;
}
static inline bool o62m_fill_path_v1(O62MGpuFillV1 *p)
{
    double m[6];for(unsigned i=0;i<6;i++){m[i]=o62m_fill_float_v1(p->matrix_words[i]);if(!isfinite(m[i]))return false;}
    const uint8_t *r=p->path;size_t at=0;O62MFillPointV1 start,last;
    if(r[at++]!=2)return false;at=(at+3)&~(size_t)3;
    if(!o62m_fill_point_v1(r+at,m,&start))return false;at+=8;last=start;
    for(unsigned i=0;i<4;i++) {
        if(r[at++]!=8)return false;at=(at+3)&~(size_t)3;
        O62MFillPointV1 b,c,d;
        if(!o62m_fill_point_v1(r+at,m,&b)||!o62m_fill_point_v1(r+at+8,m,&c)||!o62m_fill_point_v1(r+at+16,m,&d)||
           !o62m_fill_cubic_v1(p,last,b,c,d,0))return false;
        last=d;at+=24;
    }
    if(r[at++]!=1)return false;at=(at+3)&~(size_t)3;
    if(r[at++]!=0||!o62m_fill_edge_v1(p,last,start))return false;
    for(;at<sizeof(p->path);at++)if(r[at])return false;
    return p->edge_count!=0;
}
static inline bool o62m_fill_state_v1(unsigned reg,uint32_t value)
{
    switch(reg) {
    case 0:case 2:case 0x10:case 0x11:case 0x12:case 0x13:
    case 0x34:case 0x35:case 0x39:case 0x3a:case 0x3b:case 0x3c:case 0x3d:
    case 0x40:case 0x41:case 0x42:case 0x43:case 0x44:case 0x45:
    case 0xc8:case 0xcb:case 0xcc:case 0xcd:case 0xce:return true;
    case 0x1b:return value==1||value==0x11000;
    case 0x90:case 0x91:case 0x92:case 0x93:case 0x94:case 0x95:case 0x96:case 0x97:return value==0;
    default:return false;
    }
}
static inline unsigned o62m_fill_data_v1(O62MGpuFillV1 *p,const uint8_t *raw,const uint32_t *s,const bool *v)
{
    const unsigned required[]={0,2,0x10,0x11,0x12,0x13,0x34,0x35,0x39,0x3a,0x3b,0x3c,0x3d,
        0x40,0x41,0x42,0x43,0x44,0x45,0xc8,0xcb,0xcc,0xcd,0xce};
    for(unsigned i=0;i<sizeof(required)/sizeof(required[0]);i++)if(!v[required[i]])return O62M_FILL_STATE;
    if(s[0]!=UINT32_C(0x10000102)||s[0x10]!=UINT32_C(0x103)||s[0x34]!=(UINT32_C(0x03300013))||
       s[0x3b]!=0x3f800000||s[0x3c]||s[0xcd]!=s[0x42]||s[0xce]!=s[0x45])return O62M_FILL_MODE;
    unsigned width=s[0x13]&0xffff,height=s[0x13]>>16,stride=s[0x12];
    if(!width||!height||width>O62M_FILL_MAX_DIM||height>O62M_FILL_MAX_DIM||stride<width*4||
       (stride&3)||(uint64_t)stride*height>O62M_FILL_MAX_BYTES)return O62M_FILL_TARGET;
    if(!s[0xc8]||!s[0xcc]||!s[0x3d]||(uint64_t)s[0x3d]*64>s[0xc8])return O62M_FILL_TARGET;
    if(!p->tile_count) {
        p->target=s[0x11];p->stride=stride;p->width=width;p->height=height;p->color=s[2];p->control=s[0];p->format=s[0x10];
        p->tess_address=s[0x35];p->tess_bytes=s[0xc8];p->count_address=s[0xcb];p->count_bytes=s[0xcc];
        memcpy(p->matrix_words,s+0x40,sizeof(p->matrix_words));memcpy(p->path,raw,136);
        if(!o62m_fill_path_v1(p))return O62M_FILL_PATH;
    } else if(p->target!=s[0x11]||p->stride!=stride||p->width!=width||p->height!=height||p->color!=s[2]||
       p->control!=s[0]||p->format!=s[0x10]||p->tess_address!=s[0x35]||p->tess_bytes!=s[0xc8]||
       p->count_address!=s[0xcb]||p->count_bytes!=s[0xcc]||memcmp(p->matrix_words,s+0x40,sizeof(p->matrix_words))||memcmp(p->path,raw,136))return O62M_FILL_MODE;
    O62MFillTileV1 tile={s[0x39]&0xffff,s[0x39]>>16,s[0x3a]&0xffff,s[0x3a]>>16};
    if(!tile.w||!tile.h||p->tile_count==O62M_FILL_MAX_TILES)return O62M_FILL_TILE;
    unsigned right=tile.x+tile.w,bottom=tile.y+tile.h;
    if(right>width)right=width;if(bottom>height)bottom=height;
    tile.w=tile.x<right?right-tile.x:0;tile.h=tile.y<bottom?bottom-tile.y:0;
    for(unsigned i=0;i<p->tile_count;i++) {
        O62MFillTileV1 a=p->tiles[i];
        if(tile.w&&tile.h&&a.w&&a.h&&tile.x<a.x+a.w&&a.x<tile.x+tile.w&&tile.y<a.y+a.h&&a.y<tile.y+tile.h)return O62M_FILL_TILE;
    }
    p->tiles[p->tile_count++]=tile;p->clip_pixels+=(uint64_t)tile.w*tile.h;return O62M_FILL_OK;
}
static inline unsigned o62m_gpu_fill_parse_v1(O62MGpuFillV1 *p,const uint8_t *raw,size_t bytes,const uint32_t *bank,const bool *known)
{
    uint32_t state[256];bool valid[256];
    memcpy(state,bank,sizeof(state));memcpy(valid,known,sizeof(valid));size_t at=0;unsigned phase=0,result=O62M_FILL_FRAMING;
    memset(p,0,sizeof(*p));
    if(!bytes||bytes>65536||(bytes&7))return result;
    while(at<bytes) {
        uint32_t cmd=o62m_fill_u32_v1(raw+at),word=o62m_fill_u32_v1(raw+at+4);size_t step=8;
        if((cmd>>28)==3&&!phase) {
            unsigned count=(cmd>>16)&0xfff,reg=cmd&0xffff;
            step=4*((count+2u)&~1u);
            if(!count||reg<0xa00||reg+count>0xb00||step>bytes-at)break;
            for(unsigned i=0;i<count;i++) {
                unsigned r=reg-0xa00+i;uint32_t value=o62m_fill_u32_v1(raw+at+4+4*i);
                if(!o62m_fill_state_v1(r,value)){result=O62M_FILL_STATE;goto finish;}
                state[r]=value;valid[r]=true;
            }
        } else if(cmd==0x40000011&&!word&&!phase) {
            step=144;if(step>bytes-at)break;
            result=o62m_fill_data_v1(p,raw+at+8,state,valid);if(result)goto finish;
            result=O62M_FILL_FRAMING;
        } else if(cmd==0x10000007&&!word&&!phase&&p->tile_count&&valid[0x1b]&&state[0x1b]==1)phase=1;
        else if(cmd==0x20000007&&!word&&phase==1)phase=2;
        else if(!cmd&&!word&&phase==2&&at+8==bytes){result=O62M_FILL_OK;goto finish;}
        else {result=O62M_FILL_ORDER;goto finish;}
        at+=step;
    }
finish:
    if(result==O62M_FILL_OK){memcpy(p->final_state,state,sizeof(state));memcpy(p->final_valid,valid,sizeof(valid));}
    memset(state,0,sizeof(state));memset(valid,0,sizeof(valid));return result;
}
static inline int o62m_fill_compare_v1(const void *a,const void *b)
{ double x=*(const double *)a,y=*(const double *)b;return (x>y)-(x<y); }
static inline unsigned o62m_fill_round_v1(double x)
{ return x<=0?0:x>=255?255:(unsigned)floor(x+.5); }
static inline void o62m_fill_blend_v1(uint8_t *dst,uint32_t color,unsigned coverage)
{
    double cov=coverage/16.0,sa=(color>>24)*cov,inv=1.0-sa/255.0,da=dst[3];
    double outa=sa+da*inv;
    for(unsigned i=0;i<3;i++) {
        unsigned shift=(2-i)*8;double src=((color>>shift)&255)*cov,d=dst[i];
        double out=src+d*inv;
        dst[i]=(uint8_t)o62m_fill_round_v1(out);
    }
    dst[3]=(uint8_t)o62m_fill_round_v1(outa);
}
static inline bool o62m_gpu_fill_render_v1(O62MGpuFillV1 *p,uint8_t *pixels,size_t bytes)
{
    if(bytes!=(size_t)p->stride*p->height)return false;
    double intersections[O62M_FILL_MAX_EDGES];uint8_t coverage[O62M_FILL_MAX_DIM];
    p->covered_samples=0;p->changed_pixels=0;
    for(unsigned t=0;t<p->tile_count;t++) {
        O62MFillTileV1 clip=p->tiles[t];
        for(unsigned y=clip.y;y<clip.y+clip.h;y++) {
            memset(coverage,0,sizeof(coverage));
            for(unsigned sy=0;sy<4;sy++) {
                double scan=y+(sy+.5)/4.0;unsigned n=0;
                for(unsigned e=0;e<p->edge_count;e++) {
                    O62MFillPointV1 a=p->edges[e].a,b=p->edges[e].b;
                    if((a.y<=scan&&scan<b.y)||(b.y<=scan&&scan<a.y))intersections[n++]=a.x+(scan-a.y)*(b.x-a.x)/(b.y-a.y);
                }
                qsort(intersections,n,sizeof(double),o62m_fill_compare_v1);
                if(n&1)return false;
                for(unsigned j=0;j<n;j+=2) {
                    double left=fmax(intersections[j],clip.x),right=fmin(intersections[j+1],clip.x+clip.w);
                    if(right<=left)continue;
                    unsigned first=(unsigned)floor(left),last=(unsigned)ceil(right);
                    for(unsigned x=first;x<last;x++)for(unsigned sx=0;sx<4;sx++) {
                        double sample=x+(sx+.5)/4.0;
                        if(sample>=left&&sample<right)++coverage[x];
                    }
                }
            }
            for(unsigned x=clip.x;x<clip.x+clip.w;x++)if(coverage[x]) {
                if(coverage[x]>16)return false;
                uint8_t *dst=pixels+(size_t)y*p->stride+x*4,old[4];memcpy(old,dst,4);
                if(p->format==0x102)dst[3]=255;
                o62m_fill_blend_v1(dst,p->color,coverage[x]);
                if(p->format==0x102)dst[3]=255;
                p->covered_samples+=coverage[x];p->changed_pixels+=memcmp(old,dst,4)!=0;
            }
        }
    }
    memset(intersections,0,sizeof(intersections));memset(coverage,0,sizeof(coverage));return true;
}
#endif
