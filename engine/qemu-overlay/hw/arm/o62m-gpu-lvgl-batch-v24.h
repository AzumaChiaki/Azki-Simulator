/* SPDX-License-Identifier: MIT */
#ifndef O62M_GPU_LVGL_BATCH_V24_H
#define O62M_GPU_LVGL_BATCH_V24_H
#include "o62m-gpu-lvgl-fill-v5.h"
#define O62M_BATCH_OPS 256
#define O62M_BATCH_STATES 512
/* The source sampler uses the original normalized C/X/Y words. */
typedef struct {
    O62MGpuFillV1 shape;
    bool image,nonzero,source_matches_completed_state_target;
    unsigned samples;
    uint32_t paint;
    bool image_multiply;
    uint32_t source,source_stride,source_width,source_height,source_config;
    double cxy[9];
    uint32_t palette[256];
} O62MGpuBatchOpV1;
typedef struct {
    uint32_t target,target_format,stride,width,height,tess_address,tess_bytes,count_address,count_bytes;
    uint32_t final_state[O62M_BATCH_STATES];bool final_valid[O62M_BATCH_STATES];
    O62MGpuBatchOpV1 ops[O62M_BATCH_OPS];
    unsigned op_count,tile_count,edge_count,image_ops,solid_ops;
    uint64_t clip_pixels,covered_samples,changed_pixels;
    uint32_t path_bytes,path_ops[27],path_unknown,mode_reject_mask,path_framing;
    bool reject_solid_gray,reject_solid_opaque;
    uint32_t reject_source_layout,reject_source_stride;
    uint32_t reject_target_format,reject_layout,reject_stride,reject_scale,reject_bias;
    uint32_t reject_command_class,reject_command_selector,reject_command_offset,reject_command_bytes,reject_command_ops;
    bool reject_command_word_zero,reject_command_waiting,reject_command_barrier;
    uint32_t reject_register,reject_control,reject_source_key,reject_source_config,reject_path_config;
} O62MGpuBatchV1;
enum { O62M_BATCH_SOURCE=8,O62M_BATCH_PALETTE,O62M_BATCH_CAPACITY };
static inline bool o62m_batch_state_v1(unsigned r,uint32_t v)
{
    if(r>=256)return r<512;
    if(o62m_fill_state_v1(r,v))return true;
    switch(r){
    case 0x18:case 0x19:case 0x1a:case 0x1c:case 0x1d:case 0x1e:
    case 0x20:case 0x21:case 0x22:case 0x25:case 0x29:case 0x2b:case 0x2f:return true;
    case 0x1f:return v==1;
    case 0x27:case 0x2d:case 0xd1:return v==0;
    default:return false;
    }
}
static inline bool o62m_batch_quad_v1(O62MGpuFillV1 *p,const uint8_t *r)
{
    double m[6];for(unsigned i=0;i<6;i++){m[i]=o62m_fill_float_v1(p->matrix_words[i]);if(!isfinite(m[i]))return false;}
    O62MFillPointV1 first,last,next;
    if(r[0]!=2||!o62m_fill_point_v1(r+4,m,&first))return false;last=first;
    for(unsigned i=0;i<3;i++){
        size_t at=12+i*12;if(r[at]!=4||!o62m_fill_point_v1(r+at+4,m,&next)||!o62m_fill_edge_v1(p,last,next))return false;last=next;
    }
    if(r[48]!=1||r[52]!=0||r[53]||r[54]||r[55])return false;
    return o62m_fill_edge_v1(p,last,first)&&p->edge_count;
}

/* S16 commands are byte opcodes followed by two-byte-aligned signed coordinates. */
static inline bool o62m_batch_s16_point_v2(const uint8_t *raw,const double *m,O62MFillPointV1 *p)
{
    int x=(int)(raw[0]|(unsigned)raw[1]<<8),y=(int)(raw[2]|(unsigned)raw[3]<<8);
    if(x&0x8000)x-=65536;if(y&0x8000)y-=65536;
    p->x=m[0]*x+m[1]*y+m[2];p->y=m[3]*x+m[4]*y+m[5];
    return isfinite(p->x)&&isfinite(p->y)&&fabs(p->x)<=1048576&&fabs(p->y)<=1048576;
}
static inline bool o62m_batch_solid_path_v6(O62MGpuBatchV1 *batch,O62MGpuFillV1 *g,const uint8_t *raw,size_t length,unsigned coord_bytes)
{
    double m[6];for(unsigned i=0;i<6;i++){m[i]=o62m_fill_float_v1(g->matrix_words[i]);if(!isfinite(m[i]))return false;}
    size_t at=0;bool contour=false,have_move=false;O62MFillPointV1 first={0},last={0};
    batch->path_bytes=(uint32_t)length;
    while(at<length){
        unsigned code=raw[at++];at=(at+coord_bytes-1)&~(size_t)(coord_bytes-1);
        if(code<27)++batch->path_ops[code];else batch->path_unknown=code;
        if(at>length)return false;
        if(code==0){
            if(contour&&!o62m_fill_edge_v1(g,last,first))return false;
            if(!have_move||!g->edge_count)return false;
            for(;at<length;at++)if(raw[at])return false;return true;
        }
        if(code==1){if(!contour||!o62m_fill_edge_v1(g,last,first))return false;contour=false;last=first;continue;}
        unsigned points=code==2||code==4?1:code==6?2:code==8?3:0;
        if(!points){batch->path_unknown=code;return false;}
        if(points*2*coord_bytes>length-at)return false;
        O62MFillPointV1 q[3];for(unsigned i=0;i<points;i++){
            const uint8_t *r=raw+at+2*coord_bytes*i;
            if(!(coord_bytes==2?o62m_batch_s16_point_v2(r,m,&q[i]):o62m_fill_point_v1(r,m,&q[i])))return false;}
        at+=points*2*coord_bytes;
        if(code==2){if(contour&&!o62m_fill_edge_v1(g,last,first))return false;first=last=q[0];contour=have_move=true;}
        else {if(!contour)return false;
            if(code==4){if(!o62m_fill_edge_v1(g,last,q[0]))return false;last=q[0];}
            else if(code==6){
                O62MFillPointV1 b={last.x+(q[0].x-last.x)*2/3,last.y+(q[0].y-last.y)*2/3};
                O62MFillPointV1 c={q[1].x+(q[0].x-q[1].x)*2/3,q[1].y+(q[0].y-q[1].y)*2/3};
                if(!o62m_fill_cubic_v1(g,last,b,c,q[1],0))return false;last=q[1];
            }else {if(!o62m_fill_cubic_v1(g,last,q[0],q[1],q[2],0))return false;last=q[2];}}
    }
    return false;
}
static inline void o62m_batch_path_classify_v6(O62MGpuBatchV1 *p,const uint8_t *raw,size_t length,unsigned coord_bytes)
{
    const unsigned coords[10]={0,0,2,2,2,2,4,4,6,6};size_t at=0;p->path_bytes=(uint32_t)length;
    memset(p->path_ops,0,sizeof(p->path_ops));p->path_unknown=0;p->path_framing=0;
    while(at<length){unsigned op=raw[at++];at=(at+coord_bytes-1)&~(size_t)(coord_bytes-1);if(op<27)++p->path_ops[op];
        if(op>9){p->path_unknown=op;return;}if(at>length||coords[op]*coord_bytes>length-at)return;at+=coords[op]*coord_bytes;
        if(!op){for(;at<length;at++)if(raw[at])return;p->path_framing=1;return;}}
}
static inline bool o62m_batch_translation_equal_v3(uint32_t a,uint32_t b)
{double x=o62m_fill_float_v1(a),y=o62m_fill_float_v1(b);return isfinite(x)&&isfinite(y)&&x==y;}
static inline unsigned o62m_batch_solid_data_v6(O62MGpuBatchV1 *batch,O62MGpuBatchOpV1 *op,const uint8_t *raw,size_t length,const uint32_t *s,const bool *v)
{
    const unsigned required[]={0,2,0x10,0x11,0x12,0x13,0x34,0x35,0x39,0x3a,0x3b,0x3c,0x3d,0x40,0x41,0x42,0x43,0x44,0x45,0xc8,0xcb,0xcc,0xcd,0xce};
    for(unsigned i=0;i<sizeof(required)/sizeof(required[0]);i++)if(!v[required[i]]){batch->reject_register=0xa00+required[i];return O62M_FILL_STATE;}
    unsigned coord_bytes=((s[0x34]>>20)&3)==1?2:4;
    batch->mode_reject_mask=(s[0]!=0x10000102?1:0)|(s[0x10]!=0x103&&s[0x10]!=0x102&&!(s[0x34]==0x03100003&&(s[0x10]==0x100||s[0x10]==0x106))?2:0)|((s[0x34]!=0x03100003&&s[0x34]!=0x03300003&&s[0x34]!=0x03300013&&s[0x34]!=0x03300010)?4:0)|(s[0x3b]!=0x3f800000?8:0)|(s[0x3c]?16:0)|(!o62m_batch_translation_equal_v3(s[0xcd],s[0x42])?32:0)|(!o62m_batch_translation_equal_v3(s[0xce],s[0x45])?64:0);
    if(s[0x10]==0x106&&!batch->reject_solid_gray)batch->mode_reject_mask|=128;
    if(batch->mode_reject_mask){o62m_batch_path_classify_v6(batch,raw,length,coord_bytes);return O62M_FILL_MODE;}
    unsigned w=s[0x13]&0xffff,h=s[0x13]>>16,st=s[0x12],bpp=(s[0x10]==0x103||s[0x10]==0x102)?4:1;
    if(!w||!h||w>1024||h>1024||(bpp==4&&(st&3))||st<w*bpp||(uint64_t)st*h>O62M_FILL_MAX_BYTES||!s[0xc8]||!s[0xcc]||!s[0x3d]||(uint64_t)s[0x3d]*64>s[0xc8])return O62M_FILL_TARGET;
    O62MGpuFillV1 *g=&op->shape;g->target=s[0x11];g->stride=st;g->width=w;g->height=h;g->color=s[2];g->control=s[0];g->format=s[0x10];
    g->tess_address=s[0x35];g->tess_bytes=s[0xc8];g->count_address=s[0xcb];g->count_bytes=s[0xcc];memcpy(g->matrix_words,s+0x40,sizeof(g->matrix_words));
    if(!o62m_batch_solid_path_v6(batch,g,raw,length,coord_bytes)){o62m_batch_path_classify_v6(batch,raw,length,coord_bytes);return O62M_FILL_PATH;}
    O62MFillTileV1 tile={s[0x39]&0xffff,s[0x39]>>16,s[0x3a]&0xffff,s[0x3a]>>16};if(!tile.w||!tile.h)return O62M_FILL_TILE;
    unsigned right=tile.x+tile.w,bottom=tile.y+tile.h;if(right>w)right=w;if(bottom>h)bottom=h;
    tile.w=tile.x<right?right-tile.x:0;tile.h=tile.y<bottom?bottom-tile.y:0;
    g->tiles[0]=tile;g->tile_count=1;g->clip_pixels=(uint64_t)tile.w*tile.h;op->samples=s[0x34]==0x03300010?1:4;op->nonzero=(s[0x34]&16)==0;return O62M_FILL_OK;
}
typedef struct {double x;int delta;} O62MBatchCrossV2;
static inline int o62m_batch_cross_compare_v2(const void *a,const void *b)
{double x=((const O62MBatchCrossV2 *)a)->x,y=((const O62MBatchCrossV2 *)b)->x;return (x>y)-(x<y);}
static inline bool o62m_batch_nonzero_render_v4(O62MGpuFillV1 *g,uint8_t *target,size_t bytes)
{
    if(bytes!=(size_t)g->stride*g->height)return false;
    unsigned bpp=(g->format==0x103||g->format==0x102)?4:1;
    O62MBatchCrossV2 cross[O62M_FILL_MAX_EDGES];uint8_t coverage[O62M_FILL_MAX_DIM];O62MFillTileV1 clip=g->tiles[0];
    g->covered_samples=g->changed_pixels=0;
    for(unsigned y=clip.y;y<clip.y+clip.h;y++){
        memset(coverage,0,sizeof(coverage));
        for(unsigned sy=0;sy<4;sy++){
            double scan=y+(sy+.5)/4;unsigned n=0;
            for(unsigned e=0;e<g->edge_count;e++){O62MFillPointV1 a=g->edges[e].a,b=g->edges[e].b;
                if((a.y<=scan&&scan<b.y)||(b.y<=scan&&scan<a.y))cross[n++]=(O62MBatchCrossV2){a.x+(scan-a.y)*(b.x-a.x)/(b.y-a.y),b.y>a.y?1:-1};}
            qsort(cross,n,sizeof(*cross),o62m_batch_cross_compare_v2);int winding=0;unsigned j=0;
            while(j<n){double x0=cross[j].x;do{winding+=cross[j++].delta;}while(j<n&&cross[j].x==x0);
                if(!winding||j==n)continue;double left=fmax(x0,clip.x),right=fmin(cross[j].x,clip.x+clip.w);if(right<=left)continue;
                for(unsigned x=(unsigned)floor(left);x<(unsigned)ceil(right);x++)for(unsigned sx=0;sx<4;sx++){double sample=x+(sx+.5)/4;if(sample>=left&&sample<right)++coverage[x];}}
            if(winding)return false;
        }
        for(unsigned x=clip.x;x<clip.x+clip.w;x++)if(coverage[x]){if(coverage[x]>16)return false;uint8_t *dst=target+(size_t)y*g->stride+x*bpp,old[4];memcpy(old,dst,bpp);
            if(bpp==1){double cov=coverage[x]/16.0,sa=(g->color>>24)*cov,src=g->format==0x100?sa:(g->color&255)*cov;dst[0]=(uint8_t)o62m_fill_round_v1(src+dst[0]*(1-sa/255.0));}
            else {if(g->format==0x102)dst[3]=255;o62m_fill_blend_v1(dst,g->color,coverage[x]);if(g->format==0x102)dst[3]=255;}g->covered_samples+=coverage[x];g->changed_pixels+=memcmp(old,dst,bpp)!=0;}
    }
    memset(cross,0,sizeof(cross));memset(coverage,0,sizeof(coverage));return true;
}

static inline unsigned o62m_batch_data_v1(O62MGpuBatchV1 *p,const uint8_t *raw,size_t length,const uint32_t *s,const bool *v)
{
    if(p->op_count==O62M_BATCH_OPS)return O62M_BATCH_CAPACITY;
    p->reject_source_layout=s[0x2f];p->reject_source_stride=s[0x2b];
    p->reject_solid_gray=(s[2]&255)==((s[2]>>8)&255)&&(s[2]&255)==((s[2]>>16)&255);p->reject_solid_opaque=(s[2]>>24)==255;
    p->reject_target_format=s[0x10];p->reject_layout=s[0x13];p->reject_stride=s[0x12];p->reject_scale=s[0x3b];p->reject_bias=s[0x3c];
    p->reject_control=s[0];p->reject_source_config=s[0x25];p->reject_source_key=s[0x25]&UINT32_C(0x60080e3f);p->reject_path_config=s[0x34];
    O62MGpuBatchOpV1 *op=&p->ops[p->op_count];O62MGpuFillV1 *g=&op->shape;
    unsigned result;
    if(v[0]&&s[0]==UINT32_C(0x10000102)){
        result=o62m_batch_solid_data_v6(p,op,raw,length,s,v);if(result)return result;++p->solid_ops;
    }else{
        const unsigned required[]={0,2,0x10,0x11,0x12,0x13,0x18,0x19,0x1a,0x1c,0x1d,0x1e,0x1f,0x20,0x21,0x22,0x25,0x27,0x29,0x2b,0x2d,0x2f};
        for(unsigned i=0;i<sizeof(required)/sizeof(required[0]);i++)if(!v[required[i]])return O62M_FILL_STATE;
        unsigned mode=s[0]&7;
        bool alpha_mask=s[0]==UINT32_C(0x00002101)&&s[0x25]==UINT32_C(0x01000102);
        bool multiply=alpha_mask||(s[0]&~UINT32_C(7))==UINT32_C(0x10002100);
        if((mode!=1&&mode!=2)||(!multiply&&(s[0]&~UINT32_C(7))!=UINT32_C(0x10001100))||(s[0x10]!=0x103&&s[0x10]!=0x102)||(!multiply&&s[2])||s[0x1f]!=1||s[0x27]||s[0x2d])return O62M_FILL_MODE;
        if(!alpha_mask&&s[0x25]!=UINT32_C(0x01000106)&&s[0x25]!=UINT32_C(0x01020106)&&s[0x25]!=UINT32_C(0x01020900)&&s[0x25]!=UINT32_C(0x01000900)&&s[0x25]!=UINT32_C(0x01020107)&&s[0x25]!=UINT32_C(0x01000107))return O62M_BATCH_SOURCE;
        bool indexed=s[0x25]==UINT32_C(0x01020900)||s[0x25]==UINT32_C(0x01000900);unsigned source_bpp=(indexed||alpha_mask)?1:4;
        if(indexed)for(unsigned i=256;i<512;i++)if(!v[i])return O62M_BATCH_PALETTE;
        unsigned w=s[0x13]&0xffff,h=s[0x13]>>16,st=s[0x12];
        if(!w||!h||w>1024||h>1024||(st&3)||st<w*4||(uint64_t)st*h>O62M_FILL_MAX_BYTES)return O62M_FILL_TARGET;
        g->target=s[0x11];g->stride=st;g->width=w;g->height=h;g->control=s[0];g->format=s[0x10];
        g->tess_address=s[0x35];g->tess_bytes=s[0xc8];g->count_address=s[0xcb];g->count_bytes=s[0xcc];
        op->image=true;op->image_multiply=multiply;op->paint=s[2];op->source=s[0x29];op->source_stride=s[0x2b];op->source_width=s[0x2f]&0xffff;op->source_height=s[0x2f]>>16;op->source_config=s[0x25];
        if(!op->source_width||!op->source_height||op->source_stride<op->source_width*source_bpp||(op->source_stride&0xf0000000)||
            (uint64_t)op->source_stride*op->source_height>O62M_FILL_MAX_BYTES)return O62M_BATCH_SOURCE;
        const unsigned cxy[]={0x18,0x19,0x1a,0x1c,0x1d,0x1e,0x20,0x21,0x22};
        for(unsigned i=0;i<9;i++){op->cxy[i]=o62m_fill_float_v1(s[cxy[i]]);if(!isfinite(op->cxy[i]))return O62M_BATCH_SOURCE;}
        if(indexed)memcpy(op->palette,s+256,sizeof(op->palette));
        O62MFillTileV1 tile;
        if(mode==1){
            if(length!=8||s[0x34])return O62M_FILL_PATH;
            tile=(O62MFillTileV1){raw[0]|raw[1]<<8,raw[2]|raw[3]<<8,raw[4]|raw[5]<<8,raw[6]|raw[7]<<8};op->samples=1;
            if(!tile.w||!tile.h||tile.x>w||tile.w>w-tile.x||tile.y>h||tile.h>h-tile.y)return O62M_FILL_TILE;
        }else{
            const unsigned path_required[]={0x34,0x35,0x39,0x3a,0x3b,0x3c,0x3d,0x40,0x41,0x42,0x43,0x44,0x45,0xc8,0xcb,0xcc,0xcd,0xce};
            for(unsigned i=0;i<sizeof(path_required)/sizeof(path_required[0]);i++)if(!v[path_required[i]])return O62M_FILL_STATE;
            if((length!=56&&length!=136)||s[0x34]!=UINT32_C(0x03300011)||s[0x3b]!=0x3f800000||s[0x3c]||s[0xcd]!=s[0x42]||s[0xce]!=s[0x45])return O62M_FILL_PATH;
            if(!g->tess_bytes||!g->count_bytes||!s[0x3d]||(uint64_t)s[0x3d]*64>g->tess_bytes)return O62M_FILL_TARGET;
            memcpy(g->matrix_words,s+0x40,sizeof(g->matrix_words));memcpy(g->path,raw,length);
            if(!(length==56?o62m_batch_quad_v1(g,raw):o62m_fill_path_v1(g))){o62m_batch_path_classify_v6(p,raw,length,4);return O62M_FILL_PATH;}
            tile=(O62MFillTileV1){s[0x39]&0xffff,s[0x39]>>16,s[0x3a]&0xffff,s[0x3a]>>16};op->samples=2;
            if(!tile.w||!tile.h)return O62M_FILL_TILE;
            unsigned right=tile.x+tile.w,bottom=tile.y+tile.h;if(right>w)right=w;if(bottom>h)bottom=h;
            tile.w=tile.x<right?right-tile.x:0;tile.h=tile.y<bottom?bottom-tile.y:0;
        }
        g->tiles[0]=tile;g->tile_count=1;g->clip_pixels=(uint64_t)tile.w*tile.h;++p->image_ops;
    }
    if(!p->op_count){p->target_format=g->format;p->target=g->target;p->stride=g->stride;p->width=g->width;p->height=g->height;
        p->tess_address=g->tess_address;p->tess_bytes=g->tess_bytes;p->count_address=g->count_address;p->count_bytes=g->count_bytes;}
    else if(p->target_format!=g->format||p->target!=g->target||p->stride!=g->stride||p->width!=g->width||p->height!=g->height||
        p->tess_address!=g->tess_address||p->tess_bytes!=g->tess_bytes||p->count_address!=g->count_address||p->count_bytes!=g->count_bytes)return O62M_FILL_TARGET;
    ++p->op_count;p->tile_count+=g->tile_count;p->edge_count+=g->edge_count;p->clip_pixels+=g->clip_pixels;return O62M_FILL_OK;
}
static inline unsigned o62m_gpu_batch_parse_v1(O62MGpuBatchV1 *p,const uint8_t *raw,size_t bytes,const uint32_t *bank,const bool *known)
{
    uint32_t s[512];bool v[512];memcpy(s,bank,sizeof(s));memcpy(v,known,sizeof(v));memset(p,0,sizeof(*p));
    unsigned result=O62M_FILL_FRAMING;size_t at=0;bool waiting=false,last_barrier=false;
    if(!bytes||bytes>65536||(bytes&7))goto finish;
    while(at<bytes){
        uint32_t cmd=o62m_fill_u32_v1(raw+at),word=o62m_fill_u32_v1(raw+at+4);size_t step=8;
        p->reject_command_class=cmd>>28;p->reject_command_selector=(cmd>>28)<=4?cmd&0xffff:0;
        p->reject_command_offset=at;p->reject_command_bytes=bytes;p->reject_command_ops=p->op_count;
        p->reject_command_word_zero=word==0;p->reject_command_waiting=waiting;p->reject_command_barrier=last_barrier;
        if((cmd>>28)==3&&!waiting){
            unsigned count=(cmd>>16)&0xfff,reg=cmd&0xffff;step=4*((count+2u)&~1u);
            if(!count||reg<0xa00||reg+count>0xc00||step>bytes-at){p->reject_register=reg;break;}
            for(unsigned i=0;i<count;i++){
                unsigned r=reg-0xa00+i;uint32_t value=o62m_fill_u32_v1(raw+at+4+4*i);
                if(!o62m_batch_state_v1(r,value)){p->reject_register=reg+i;result=O62M_FILL_STATE;goto finish;}s[r]=value;v[r]=true;
            }last_barrier=false;
        }else if((cmd&0xffff0000)==0x40000000&&!word&&!waiting){
            size_t n=(cmd&0xffff)*8u;step=8+n;if(!n||step>bytes-at)break;
            result=o62m_batch_data_v1(p,raw+at+8,n,s,v);if(result)goto finish;result=O62M_FILL_FRAMING;last_barrier=false;
        }else if(cmd==0x10000007&&!word&&!waiting){waiting=true;last_barrier=false;}
        else if(cmd==0x20000007&&!word&&waiting){waiting=false;last_barrier=true;}
        else if(!cmd&&!word&&!waiting&&last_barrier&&v[0x1b]&&s[0x1b]==1&&at+8==bytes){
            memcpy(p->final_state,s,sizeof(s));memcpy(p->final_valid,v,sizeof(v));result=O62M_FILL_OK;goto finish;
        }else {result=O62M_FILL_ORDER;goto finish;}at+=step;
    }
finish: memset(s,0,sizeof(s));memset(v,0,sizeof(v));return result;
}
static inline void o62m_batch_palette_v1(uint32_t word,double *bgra)
{
    bgra[3]=word>>24;
    for(unsigned i=0;i<3;i++)bgra[i]=((word>>(i*8))&255)*bgra[3]/255.0;
}
static inline bool o62m_batch_sample_v1(const O62MGpuBatchOpV1 *op,const uint8_t *source,unsigned x,unsigned y,double *color)
{
    double q[3];for(unsigned i=0;i<3;i++)q[i]=op->cxy[i]+x*op->cxy[3+i]+y*op->cxy[6+i];
    if(!isfinite(q[0])||!isfinite(q[1])||!isfinite(q[2])||q[2]==0)return false;
    double u=op->source_width*q[0]/q[2],v=op->source_height*q[1]/q[2];if(!isfinite(u)||!isfinite(v))return false;
    /* Explicit virtual image border policy: repeat the nearest edge texel. */
    u=fmin(fmax(u,0),op->source_width-1);v=fmin(fmax(v,0),op->source_height-1);
    unsigned x0=(unsigned)floor(u),y0=(unsigned)floor(v),x1=x0+1<op->source_width?x0+1:x0,y1=y0+1<op->source_height?y0+1:y0;
    if(op->source_config==UINT32_C(0x01000102)){
        double mask=source[(size_t)y0*op->source_stride+x0];
        for(unsigned i=0;i<4;i++)color[i]=mask;
        return true;
    }
    if(op->source_config==UINT32_C(0x01000900)){
        o62m_batch_palette_v1(op->palette[source[(size_t)y0*op->source_stride+x0]],color);return true;
    }
    if(op->source_config==UINT32_C(0x01000107)||op->source_config==UINT32_C(0x01000106)){
        const uint8_t *pixel=source+(size_t)y0*op->source_stride+4*x0;
        for(unsigned i=0;i<4;i++)color[i]=pixel[i];
        if(op->source_config==UINT32_C(0x01000106))color[3]=255;
        return true;
    }
    double fx=u-x0,fy=v-y0,w[4]={(1-fx)*(1-fy),fx*(1-fy),(1-fx)*fy,fx*fy};
    unsigned xx[4]={x0,x1,x0,x1},yy[4]={y0,y0,y1,y1};memset(color,0,4*sizeof(double));
    for(unsigned j=0;j<4;j++){double tap[4];
        if(op->source_config==UINT32_C(0x01020107)||op->source_config==UINT32_C(0x01020106)){
            const uint8_t *pixel=source+(size_t)yy[j]*op->source_stride+4*xx[j];
            /* This virtual BGRA pipeline stores premultiplied RGB throughout. */
            for(unsigned i=0;i<4;i++)tap[i]=pixel[i];
            if(op->source_config==UINT32_C(0x01020106))tap[3]=255;
        }else o62m_batch_palette_v1(op->palette[source[(size_t)yy[j]*op->source_stride+xx[j]]],tap);
        for(unsigned i=0;i<4;i++)color[i]+=tap[i]*w[j];}
    return true;
}
static inline unsigned o62m_gpu_batch_bpp_v4(const O62MGpuBatchV1 *p)
{return (p->target_format==0x103||p->target_format==0x102)?4:1;}
/* Quality-zero uses one pixel-center sample; BGRX stores an opaque X byte. */
static inline bool o62m_batch_center_render_v9(O62MGpuFillV1 *g,uint8_t *target,size_t bytes)
{
    if(bytes!=(size_t)g->stride*g->height||(g->format!=0x102&&g->format!=0x103))return false;
    double cross[O62M_FILL_MAX_EDGES];O62MFillTileV1 clip=g->tiles[0];
    g->covered_samples=g->changed_pixels=0;
    for(unsigned y=clip.y;y<clip.y+clip.h;y++) {
        unsigned n=0;double scan=y+.5;
        for(unsigned e=0;e<g->edge_count;e++) {
            O62MFillPointV1 a=g->edges[e].a,b=g->edges[e].b;
            if((a.y<=scan&&scan<b.y)||(b.y<=scan&&scan<a.y))cross[n++]=a.x+(scan-a.y)*(b.x-a.x)/(b.y-a.y);
        }
        qsort(cross,n,sizeof(double),o62m_fill_compare_v1);if(n&1)return false;
        for(unsigned j=0;j<n;j+=2) {
            double left=fmax(cross[j],clip.x),right=fmin(cross[j+1],clip.x+clip.w);
            if(right<=left)continue;
            for(unsigned x=(unsigned)floor(left);x<(unsigned)ceil(right);x++)if(x+.5>=left&&x+.5<right) {
                uint8_t *dst=target+(size_t)y*g->stride+4*x,old[4];memcpy(old,dst,4);
                if(g->format==0x102)dst[3]=255;
                o62m_fill_blend_v1(dst,g->color,16);
                if(g->format==0x102)dst[3]=255;
                ++g->covered_samples;g->changed_pixels+=memcmp(old,dst,4)!=0;
            }
        }
    }
    memset(cross,0,sizeof(cross));return true;
}
static inline bool o62m_gpu_batch_render_op_v1(O62MGpuBatchV1 *p,unsigned index,uint8_t *target,size_t bytes,const uint8_t *source,size_t source_bytes)
{
    if(index>=p->op_count||bytes!=(size_t)p->stride*p->height)return false;
    O62MGpuBatchOpV1 *op=&p->ops[index];O62MGpuFillV1 *g=&op->shape;
    if(!op->image){bool ok=op->samples==1?o62m_batch_center_render_v9(g,target,bytes):op->nonzero?o62m_batch_nonzero_render_v4(g,target,bytes):o62m_gpu_fill_render_v1(g,target,bytes);p->covered_samples+=g->covered_samples;p->changed_pixels+=g->changed_pixels;return ok;}
    if(!source||source_bytes!=(size_t)op->source_stride*op->source_height)return false;
    O62MFillTileV1 clip=g->tiles[0];unsigned ns=op->samples,total=ns*ns;double intersections[O62M_FILL_MAX_EDGES];uint8_t coverage[O62M_FILL_MAX_DIM];
    for(unsigned y=clip.y;y<clip.y+clip.h;y++){
        memset(coverage,0,sizeof(coverage));
        if(!g->edge_count)memset(coverage+clip.x,1,clip.w);
        else for(unsigned sy=0;sy<ns;sy++){
            double scan=y+(sy+.5)/ns;unsigned n=0;
            for(unsigned e=0;e<g->edge_count;e++){
                O62MFillPointV1 a=g->edges[e].a,b=g->edges[e].b;
                if((a.y<=scan&&scan<b.y)||(b.y<=scan&&scan<a.y))intersections[n++]=a.x+(scan-a.y)*(b.x-a.x)/(b.y-a.y);
            }
            qsort(intersections,n,sizeof(double),o62m_fill_compare_v1);if(n&1)return false;
            for(unsigned j=0;j<n;j+=2){double left=fmax(intersections[j],clip.x),right=fmin(intersections[j+1],clip.x+clip.w);if(right<=left)continue;
                for(unsigned x=(unsigned)floor(left);x<(unsigned)ceil(right);x++)for(unsigned sx=0;sx<ns;sx++){
                    double sample=x+(sx+.5)/ns;if(sample>=left&&sample<right)++coverage[x];}}
        }
        for(unsigned x=clip.x;x<clip.x+clip.w;x++)if(coverage[x]){
            if(coverage[x]>total)return false;double color[4];if(!o62m_batch_sample_v1(op,source,x,y,color))return false;
            /* VG Lite paint is premultiplied AABBGGRR; sampled pixels are BGRA. */
            if(op->image_multiply){
                color[0]*=((op->paint>>16)&255)/255.0;
                color[1]*=((op->paint>>8)&255)/255.0;
                color[2]*=(op->paint&255)/255.0;
                color[3]*=(op->paint>>24)/255.0;
            }
            double cov=coverage[x]/(double)total,inv=1-color[3]*cov/255.0;uint8_t *dst=target+(size_t)y*p->stride+x*4,old[4];memcpy(old,dst,4);
            if(p->target_format==0x102)dst[3]=255;
            for(unsigned i=0;i<4;i++)dst[i]=(uint8_t)o62m_fill_round_v1(color[i]*cov+dst[i]*inv);
            if(p->target_format==0x102)dst[3]=255;
            p->covered_samples+=coverage[x];p->changed_pixels+=memcmp(old,dst,4)!=0;
        }
    }
    memset(intersections,0,sizeof(intersections));memset(coverage,0,sizeof(coverage));return true;
}
#endif
