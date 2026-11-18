#include "scratchpad.h"
#include <limits.h>
#include <string.h>

SpLayout sp_default_layout(void) { return (SpLayout){12,70,75,90,85,SP_SPLIT_LONGEST,false,false}; }
bool sp_valid_layout(SpLayout o) {
    return o.gap>=0 && o.gap<=1000 && o.single_w>=1 && o.single_w<=100 &&
        o.single_h>=1 && o.single_h<=100 && o.multi_w>=1 && o.multi_w<=100 &&
        o.multi_h>=1 && o.multi_h<=100 && o.split>=SP_SPLIT_LONGEST && o.split<=SP_SPLIT_ALTERNATE;
}
SpStatus sp_layout_with(SpRect work, size_t count, SpRect *out, size_t capacity, SpLayout o) {
    SpRect rects[SP_CAPACITY];
    if (!count) return SP_OK;
    if (count > SP_CAPACITY) return SP_TOO_MANY;
    if (!sp_valid_layout(o) || !out || capacity<count || work.w<=0 || work.h<=0 ||
        (int64_t)work.x+work.w>INT32_MAX || (int64_t)work.y+work.h>INT32_MAX) return SP_BAD_ARGUMENT;
    int32_t w=(int32_t)(((int64_t)work.w*(count==1?o.single_w:o.multi_w)+50)/100);
    int32_t h=(int32_t)(((int64_t)work.h*(count==1?o.single_h:o.multi_h)+50)/100);
    int32_t x=work.x+(work.w-w)/2, y=work.y+(work.h-h)/2;
    SpRect region={x,y,w,h};
    for (size_t i=0;i<count;++i) {
        if (w<1 || h<1) return SP_TOO_SMALL;
        bool horizontal=o.split==SP_SPLIT_HORIZONTAL ||
            (o.split==SP_SPLIT_LONGEST && w>=h) || (o.split==SP_SPLIT_ALTERNATE && i%2==0);
        if (i+1==count) rects[i]=(SpRect){x,y,w,h};
        else if (horizontal) {
            if (w<o.gap+2) return SP_TOO_SMALL;
            int32_t half=(w-o.gap)/2;
            rects[i]=(SpRect){x,y,half,h}; x+=half+o.gap; w-=half+o.gap;
        } else {
            if (h<o.gap+2) return SP_TOO_SMALL;
            int32_t half=(h-o.gap)/2;
            rects[i]=(SpRect){x,y,w,half}; y+=half+o.gap; h-=half+o.gap;
        }
    }
    for (size_t i=0;i<count;++i) {
        if (o.mirror_x) rects[i].x=region.x+(region.w-(rects[i].x-region.x)-rects[i].w);
        if (o.mirror_y) rects[i].y=region.y+(region.h-(rects[i].y-region.y)-rects[i].h);
    }
    memcpy(out,rects,count*sizeof(*out)); return SP_OK;
}
SpStatus sp_layout(SpRect work, size_t count, SpRect *out, size_t capacity) {
    return sp_layout_with(work,count,out,capacity,sp_default_layout());
}
