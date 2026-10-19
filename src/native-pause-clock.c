// SPDX-License-Identifier: GPL-2.0-or-later
#include "native-pause-clock.h"
#include <obs-internal.h>
#include <util/util_uint64.h>
#include <stdatomic.h>
#include <stddef.h>
_Static_assert(offsetof(struct obs_encoder,pause.ts_start)%8==0,"Native x64 pause clock must be aligned");
static atomic_bool estimated_once;

uint64_t ea_native_pause_start(obs_encoder_t *encoder)
{
    // OBS emits pause after releasing its encoder locks and before the caller
    // can resume. The aligned x64 load is atomic; never write native state.
    // The layout is OBS 32.2.2's: the offset must match OBS's public value
    // and the start must be near now, or another layout is assumed.
    if(!encoder || !obs_encoder_paused(encoder))return 0;
    if(*(volatile uint64_t *)&encoder->pause.ts_offset!=obs_encoder_get_pause_offset(encoder))return 0;
    uint64_t start=*(volatile uint64_t *)&encoder->pause.ts_start,now=os_gettime_ns();
    return start && start<now+1000000000ULL && now<start+5000000000ULL?start:0;
}

uint64_t ea_pause_grid_start(uint64_t now, uint64_t frame_ts, uint64_t interval)
{
    if(!interval || !frame_ts)return now;
    // A frame time just after now is on the same grid; step back past now.
    if(now<frame_ts)frame_ts-=((frame_ts-now)/interval+1)*interval;
    return frame_ts+(now-frame_ts+2*interval)/interval*interval;
}

uint64_t ea_pause_start(obs_encoder_t *encoder, bool *estimated)
{
    *estimated=false;
    if(!encoder || !obs_encoder_paused(encoder))return 0;
#ifndef EA_TEST_NEWER_VERSIONS // test builds act as if the layout had changed
    uint64_t start=ea_native_pause_start(encoder);
    if(start)return start;
#endif
    // Called from the pause signal, right after OBS placed the pause, so now
    // is on the same side of the next frame boundary as OBS's own request.
    struct obs_video_info ovi;
    uint64_t interval=obs_get_video_info(&ovi) && ovi.fps_num?util_mul_div64(1000000000ULL,ovi.fps_den,ovi.fps_num):0;
    uint64_t now=os_gettime_ns(),frame_ts=obs_get_video_frame_time();
    *estimated=true; atomic_store(&estimated_once,true);
    return ea_pause_grid_start(now,frame_ts,interval);
}

bool ea_pause_estimated(void) { return atomic_load(&estimated_once); }
