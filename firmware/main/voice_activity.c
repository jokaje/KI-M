#include "voice_activity.h"
void kim_vad_reset(kim_vad_t *v) { *v = (kim_vad_t){.noise_floor=120}; }
bool kim_vad_feed(kim_vad_t *v, const int16_t *samples, size_t count) {
    if (!count) return v->speech;
    uint64_t total=0;
    for (size_t i=0;i<count;i++) { int32_t s=samples[i]; total += (uint32_t)(s<0?-s:s); }
    unsigned level=(unsigned)(total/count);
    unsigned threshold=v->noise_floor*3u+180u;
    if (threshold<420u) threshold=420u;
    if (level>threshold) { if(v->active_frames<5) ++v->active_frames; v->silence_frames=0; }
    else { v->active_frames=0; if(v->silence_frames<1000) ++v->silence_frames;
        if(!v->speech) v->noise_floor=(v->noise_floor*31u+level)/32u; }
    if(v->active_frames>=3) v->speech=true;
    if(v->silence_frames>=45) v->speech=false; /* 450ms at 10ms frames */
    return v->speech;
}
