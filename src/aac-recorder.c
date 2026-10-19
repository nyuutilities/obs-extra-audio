// SPDX-License-Identifier: GPL-2.0-or-later
// Public OBS outputs drive the selected native encoder. PCM stays in bounded RAM.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "aac-recorder.h"
#include "mp4-clock.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <util/deque.h>
#include <math.h>
#include <string.h>

#define QUEUE_BLOCKS 64u
#define GROUPS 2u
#define TEE_BATCH 64u
#define PACKET_LIMIT (256ULL*1024*1024)
struct ea_aac_recorder;
struct aac_track {
    obs_encoder_t *encoder;
    AVStream *stream;
    unsigned input;
    // Packets keep the encoder's own timestamps; readers subtract this, the
    // measured encoder delay minus the priming OBS reports (see ea_aac_calibrate).
    int64_t correction;
    int64_t last_end;
    struct deque replay;
    // Packets ever added; the deque holds the newest. `trimmed` is set once
    // the first packet this encoder produced has been dropped.
    uint64_t pushed;
    bool trimmed;
};
struct encoder_group {
    struct ea_aac_recorder *owner;
    audio_t *audio;
    obs_output_t *sink;
    unsigned first, count;
    atomic_uint read;
};
struct ea_aac_recorder {
    AVFormatContext *output; // Holds the stream parameters; never written.
    struct aac_track tracks[EA_MAX_EXTRA_TRACKS];
    struct encoder_group groups[GROUPS];
    // Blocks waiting for the encoders, each `block_floats` long. It exists
    // whenever a block is waiting; ea_aac_release_queue frees it when idle.
    _Atomic(float *) queue;
    float pending[EA_MAX_EXTRA_TRACKS][2][AUDIO_OUTPUT_FRAMES];
    unsigned count, group_count, buffered, rate;
    size_t block_floats;
    atomic_uint write;
    int64_t position;
    CRITICAL_SECTION mutex;
    // Signalled when an encoder group takes a block, a packet arrives, or a
    // sink output finishes stopping, so waiting threads never poll.
    HANDLE room, progress, stopped;
    atomic_bool failed;
    bool limit_logged;
    unsigned retention_seconds;
    uint64_t replay_bytes;
    struct ea_extra_track bindings[EA_MAX_EXTRA_TRACKS];
    char encoder_id[128];
    obs_data_t *encoder_settings;
};

static bool check(atomic_bool *failed,int result,const char *step)
{
    if(result>=0) return true;
    char error[AV_ERROR_MAX_STRING_SIZE]; av_strerror(result,error,sizeof(error));
    blog(LOG_ERROR,"[extra-audio] Recording AAC %s: %s",step,error);
    atomic_store(failed,true); return false;
}
static int64_t corrected(const struct aac_track *t,int64_t pts) {return pts-t->correction;}
static bool pull(void *opaque,uint64_t start,uint64_t end,uint64_t *timestamp,uint32_t active,struct audio_output_data *mixes)
{
    (void)start;(void)end;
    struct encoder_group *g=opaque; struct ea_aac_recorder *w=g->owner;
    unsigned read=atomic_load(&g->read), write=atomic_load_explicit(&w->write,memory_order_acquire);
    if(!active || read==write || atomic_load(&w->failed)) return false;
    const float *block=atomic_load_explicit(&w->queue,memory_order_acquire)+(size_t)(read%QUEUE_BLOCKS)*w->block_floats;
    *timestamp=1000000000ULL+audio_frames_to_ns(w->rate,(uint64_t)read*AUDIO_OUTPUT_FRAMES);
    for(unsigned m=0;m<g->count;++m) if(active&(1u<<m))
        for(unsigned c=0;c<2;++c) memcpy(mixes[m].data[c],block+((size_t)(g->first+m)*2+c)*AUDIO_OUTPUT_FRAMES,AUDIO_OUTPUT_FRAMES*sizeof(float));
    atomic_store_explicit(&g->read,read+1,memory_order_release); SetEvent(w->room); return true;
}
static const char *sink_name(void *unused) { (void)unused; return "Extra Audio recording encoder"; }
static void *sink_create(obs_data_t *settings,obs_output_t *output)
{
    struct encoder_group *g=(void *)(uintptr_t)obs_data_get_int(settings,"context"); g->sink=output; return g;
}
static void sink_destroy(void *data) { (void)data; }
static bool sink_start(void *data)
{
    struct encoder_group *g=data;
    return obs_output_initialize_encoders(g->sink,OBS_OUTPUT_AUDIO) && obs_output_begin_data_capture(g->sink,OBS_OUTPUT_AUDIO);
}
static void sink_stop(void *data,uint64_t ts) { (void)ts; obs_output_end_data_capture(((struct encoder_group *)data)->sink); }
static void drop_front(struct ea_aac_recorder *w,struct aac_track *t)
{
    AVPacket *first;deque_pop_front(&t->replay,&first,sizeof(first));
    w->replay_bytes-=first->size;av_packet_free(&first);t->trimmed=true;
}
static void sink_packet(void *data,struct encoder_packet *packet)
{
    struct encoder_group *g=data; struct ea_aac_recorder *w=g->owner;
    if(!packet || packet->track_idx>=g->count || atomic_load(&w->failed)) return;
    struct aac_track *t=&w->tracks[g->first+packet->track_idx];
    AVRational timebase={packet->timebase_num,packet->timebase_den};
    int64_t duration=(int64_t)obs_encoder_get_frame_size(t->encoder);
    int64_t pts=packet->pts, dts=packet->dts;
    AVPacket *copy=av_packet_alloc();
    if(!copy || !check(&w->failed,av_new_packet(copy,(int)packet->size),"packet allocation")) { av_packet_free(&copy); atomic_store(&w->failed,true); return; }
    memcpy(copy->data,packet->data,packet->size); copy->pts=pts;copy->dts=dts;copy->duration=duration;
    copy->flags=AV_PKT_FLAG_KEY; copy->stream_index=t->stream->index;
    av_packet_rescale_ts(copy,timebase,t->stream->time_base);
    EnterCriticalSection(&w->mutex);
    deque_push_back(&t->replay,&copy,sizeof(copy));w->replay_bytes+=copy->size;++t->pushed;
    int64_t oldest=copy->pts-(int64_t)w->retention_seconds*t->stream->codecpar->sample_rate;
    // At the RAM limit keep the newest audio: outputs reading these packets
    // continue, and only clips reaching further back lose their extra tracks.
    bool full=false;
    while(t->replay.size>sizeof(copy)) {
        AVPacket *first;deque_peek_front(&t->replay,&first,sizeof(first));
        bool over=w->replay_bytes>PACKET_LIMIT;
        if(first->pts+first->duration>=oldest && !over)break;
        full|=over;drop_front(w,t);
    }
    if(full && !w->limit_logged) {
        w->limit_logged=true;
        blog(LOG_WARNING,"[extra-audio] Compressed extra audio reached its 256 MiB RAM limit; older audio is dropped, so clips reaching further back keep only their native tracks.");
    }
    t->last_end=av_rescale_q(pts+(int64_t)obs_encoder_get_frame_size(t->encoder),timebase,(AVRational){1,(int)w->rate});
    LeaveCriticalSection(&w->mutex);
    SetEvent(w->progress);
}
static void sink_deactivated(void *data,calldata_t *unused) { (void)unused; SetEvent(((struct ea_aac_recorder *)data)->stopped); }
void ea_aac_register(void)
{
    struct obs_output_info info={.id="extra_audio_encoder_sink",.flags=OBS_OUTPUT_AUDIO|OBS_OUTPUT_ENCODED|OBS_OUTPUT_MULTI_TRACK_AUDIO,
        .get_name=sink_name,.create=sink_create,.destroy=sink_destroy,.start=sink_start,.stop=sink_stop,
        .encoded_packet=sink_packet,.encoded_audio_codecs="aac"};
    obs_register_output(&info);
}

struct ea_aac_recorder *ea_aac_open(unsigned rate,const struct ea_extra_track *tracks,unsigned count,
    bool vertical,bool backup_all,const char *encoder_id,obs_data_t *encoder_settings,unsigned seconds)
{
    struct ea_aac_recorder *w=av_mallocz(sizeof(*w)); if(!w) return NULL;
    InitializeCriticalSection(&w->mutex); atomic_init(&w->failed,false);atomic_init(&w->write,0);atomic_init(&w->queue,NULL);
    w->room=CreateEventW(NULL,FALSE,FALSE,NULL);w->progress=CreateEventW(NULL,FALSE,FALSE,NULL);w->stopped=CreateEventW(NULL,FALSE,FALSE,NULL);
    if(!w->room || !w->progress || !w->stopped) goto fail;
    w->rate=rate; w->retention_seconds=seconds?seconds:30;
    snprintf(w->encoder_id,sizeof(w->encoder_id),"%s",encoder_id);w->encoder_settings=encoder_settings;obs_data_addref(encoder_settings);
    const char *codec=obs_get_encoder_codec(encoder_id);
    if(!codec || strcmp(codec,"aac")) { blog(LOG_ERROR,"[extra-audio] Selected main recording encoder '%s' is unavailable or not AAC; no substitute encoder will be used.",encoder_id); goto fail; }
    w->output=avformat_alloc_context();if(!w->output)goto fail;
    for(unsigned i=0;i<count && i<EA_MAX_EXTRA_TRACKS;++i) if(backup_all || (vertical?tracks[i].vertical:tracks[i].main)) {
        w->bindings[w->count]=tracks[i];w->tracks[w->count++].input=i;
    }
    if(!w->count) goto fail;
    w->block_floats=(size_t)w->count*2*AUDIO_OUTPUT_FRAMES;
    w->group_count=(w->count+MAX_AUDIO_MIXES-1)/MAX_AUDIO_MIXES;
    for(unsigned i=0;i<w->group_count;++i) {
        struct encoder_group *g=&w->groups[i];g->owner=w;g->first=i*MAX_AUDIO_MIXES;g->count=w->count-g->first;
        if(g->count>MAX_AUDIO_MIXES) g->count=MAX_AUDIO_MIXES;
        atomic_init(&g->read,0);
        struct audio_output_info ai={.name="Extra Audio recording clock",.samples_per_sec=rate,.format=AUDIO_FORMAT_FLOAT_PLANAR,
            .speakers=SPEAKERS_STEREO,.input_callback=pull,.input_param=g};
        if(audio_output_open(&g->audio,&ai)!=AUDIO_OUTPUT_SUCCESS) goto fail;
        obs_data_t *settings=obs_data_create();obs_data_set_int(settings,"context",(long long)(uintptr_t)g);
        char name[128];snprintf(name,sizeof(name),"Extra Audio %s encoder group %u",vertical?"Vertical":"Main",i+1);
        g->sink=obs_output_create("extra_audio_encoder_sink",name,settings,NULL);obs_data_release(settings);if(!g->sink) goto fail;
        signal_handler_connect(obs_output_get_signal_handler(g->sink),"deactivate",sink_deactivated,w);
        for(unsigned m=0;m<g->count;++m) {
            struct aac_track *t=&w->tracks[g->first+m];const struct ea_extra_track *track=&tracks[t->input];
            settings=obs_encoder_defaults(encoder_id); if(!settings) settings=obs_data_create();
            if(encoder_settings) obs_data_apply(settings,encoder_settings); obs_data_set_int(settings,"bitrate",track->bitrate);
            snprintf(name,sizeof(name),"Extra Audio %s Track %u",vertical?"Vertical":"Main",track->number);
            t->encoder=obs_audio_encoder_create(encoder_id,name,settings,m,NULL);obs_data_release(settings); if(!t->encoder) goto fail;
            obs_encoder_set_audio(t->encoder,g->audio);obs_output_set_audio_encoder(g->sink,t->encoder,m);
        }
        if(!obs_output_initialize_encoders(g->sink,OBS_OUTPUT_AUDIO)) goto fail;
    }
    for(unsigned i=0;i<w->count;++i) {
        struct aac_track *t=&w->tracks[i];const struct ea_extra_track *track=&tracks[t->input];
        t->stream=avformat_new_stream(w->output,NULL); if(!t->stream) goto fail;
        AVCodecParameters *p=t->stream->codecpar;p->codec_type=AVMEDIA_TYPE_AUDIO;p->codec_id=AV_CODEC_ID_AAC;
        p->sample_rate=obs_encoder_get_sample_rate(t->encoder);p->bit_rate=track->bitrate*1000;
        // Packet times and output frames share one clock.
        if(p->sample_rate!=(int)rate) { blog(LOG_ERROR,"[extra-audio] Extra track encoder runs at %d Hz instead of %u Hz",p->sample_rate,rate); goto fail; }
        // ea_aac_calibrate replaces this with the measured delay.
        p->frame_size=(int)obs_encoder_get_frame_size(t->encoder);p->initial_padding=(int)obs_encoder_get_priming_samples(t->encoder);
        av_channel_layout_default(&p->ch_layout,2);t->stream->time_base=(AVRational){1,p->sample_rate};
        uint8_t *extra=NULL;size_t size=0;
        if(!obs_encoder_get_extra_data(t->encoder,&extra,&size) || !size) goto fail;
        p->extradata=av_mallocz(size+AV_INPUT_BUFFER_PADDING_SIZE);if(!p->extradata) goto fail;
        memcpy(p->extradata,extra,size);p->extradata_size=(int)size;
        char label[320];snprintf(label,sizeof(label),*track->name?"Track %u - %s":"Track %u",track->number,track->name);
        av_dict_set(&t->stream->metadata,"handler_name",label,0);av_dict_set(&t->stream->metadata,"title",label,0);
        av_dict_set(&t->stream->metadata,"encoder",obs_encoder_get_display_name(encoder_id),0);
        blog(LOG_INFO,"[extra-audio] %s: %s, encoder=%s, bitrate=%u",vertical?"Vertical":"Main",label,encoder_id,track->bitrate);
    }
    for(unsigned i=0;i<w->group_count;++i) if(!obs_output_start(w->groups[i].sink)) goto fail;
    return w;
fail:
    atomic_store(&w->failed,true);ea_aac_close(w);return NULL;
}
#define EA_BITRATE_CACHE 4u
static struct { char id[128]; unsigned rate; size_t count; unsigned values[EA_MAX_BITRATES]; } bitrate_cache[EA_BITRATE_CACHE];
static unsigned bitrate_cache_next;

static int compare_bitrates(const void *a, const void *b)
{
    unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
    return x < y ? -1 : x > y;
}

size_t ea_encoder_bitrates(const char *id, unsigned rate, unsigned *bitrates, size_t max)
{
    for (unsigned i = 0; i < EA_BITRATE_CACHE; ++i) {
        if (bitrate_cache[i].rate != rate || strcmp(bitrate_cache[i].id, id)) continue;
        size_t count = bitrate_cache[i].count < max ? bitrate_cache[i].count : max;
        memcpy(bitrates, bitrate_cache[i].values, count * sizeof(*bitrates)); return count;
    }
    obs_properties_t *props = obs_get_encoder_properties(id);
    if (!props) return 0;
    obs_data_t *settings = obs_encoder_defaults(id);
    if (!settings) settings = obs_data_create();
    // CoreAudio fills its bitrate list for the selected sample rate when its
    // settings are applied, as OBS's own Settings window does.
    obs_data_set_int(settings, "samplerate", rate);
    obs_properties_apply_settings(props, settings);
    unsigned found[EA_MAX_BITRATES]; size_t count = 0;
    obs_property_t *p = obs_properties_get(props, "bitrate");
    if (p && obs_property_get_type(p) == OBS_PROPERTY_LIST && obs_property_list_format(p) == OBS_COMBO_FORMAT_INT) {
        for (size_t i = 0; i < obs_property_list_item_count(p) && count < EA_MAX_BITRATES; ++i)
            if (!obs_property_list_item_disabled(p, i) && obs_property_list_item_int(p, i) > 0)
                found[count++] = (unsigned)obs_property_list_item_int(p, i);
    } else if (p && obs_property_get_type(p) == OBS_PROPERTY_INT) {
        int low = obs_property_int_min(p), high = obs_property_int_max(p), step = obs_property_int_step(p);
        for (int value = low > 0 ? low : step; step > 0 && value <= high && count < EA_MAX_BITRATES; value += step) found[count++] = (unsigned)value;
    }
    obs_data_release(settings); obs_properties_destroy(props);
    qsort(found, count, sizeof(*found), compare_bitrates);
    size_t unique = 0;
    for (size_t i = 0; i < count; ++i) if (!unique || found[unique - 1] != found[i]) found[unique++] = found[i];
    // Listing CoreAudio's bitrates creates converters, each of which keeps some
    // memory inside Apple's library; ask once per encoder and sample rate.
    if (unique) {
        unsigned slot = bitrate_cache_next++ % EA_BITRATE_CACHE;
        snprintf(bitrate_cache[slot].id, sizeof(bitrate_cache[slot].id), "%s", id);
        bitrate_cache[slot].rate = rate; bitrate_cache[slot].count = unique;
        memcpy(bitrate_cache[slot].values, found, unique * sizeof(*found));
    }
    if (unique > max) unique = max;
    memcpy(bitrates, found, unique * sizeof(*bitrates)); return unique;
}

unsigned ea_aac_track_count(const struct ea_aac_recorder *w) {return w->count;}
// Encoder delay: where input sample 0 appears in the decoded stream, counted
// from the first packet. OBS reports it as the encoder's priming samples, but
// its CoreAudio wrapper reports the AAC-LC value for HE-AAC as well. Encode a
// short sweep, decode it with FFmpeg's AAC decoder (the decoder the timing
// tests use) and find the sweep, instead of assuming a number.
#define SWEEP_FRAMES 4096
#define LAG_FRAMES (12 * AUDIO_OUTPUT_FRAMES)
#define DECODED_FRAMES (SWEEP_FRAMES + LAG_FRAMES)
#define CALIBRATION_FRAMES (32 * AUDIO_OUTPUT_FRAMES)
static float sweep(unsigned n,unsigned rate)
{
    // 300 Hz to 3 kHz, inside the core band even of low-bitrate HE-AAC.
    double t=(double)n/rate,length=(double)SWEEP_FRAMES/rate,f0=300,f1=3000;
    double phase=2*3.141592653589793*(f0*t+(f1-f0)*t*t/(2*length));
    double window=0.5-0.5*cos(2*3.141592653589793*n/(SWEEP_FRAMES-1));
    return (float)(0.5*window*sin(phase));
}
// Decodes channel 0 of every packet; returns the decoded sample count or -1.
static int64_t decode_track(const AVCodecParameters *parameters,AVPacket **packets,size_t count,float *out,int64_t limit,unsigned rate)
{
    const AVCodec *codec=avcodec_find_decoder(AV_CODEC_ID_AAC);AVCodecContext *decoder=codec?avcodec_alloc_context3(codec):NULL;
    AVFrame *frame=av_frame_alloc();int64_t written=-1;
    if(!decoder || !frame || avcodec_parameters_to_context(decoder,parameters)<0 || avcodec_open2(decoder,codec,NULL)<0) goto done;
    written=0;
    for(size_t i=0;i<=count && written>=0;++i) {
        if(avcodec_send_packet(decoder,i<count?packets[i]:NULL)<0) {written=-1;break;}
        while(avcodec_receive_frame(decoder,frame)>=0) {
            if(frame->format!=AV_SAMPLE_FMT_FLTP || frame->sample_rate!=(int)rate) {
                blog(LOG_WARNING,"[extra-audio] Delay measurement decoded %d Hz, format %d; expected %u Hz planar float",frame->sample_rate,frame->format,rate);
                written=-1;break;
            }
            for(int n=0;n<frame->nb_samples && written<limit;++n) out[written++]=((const float *)frame->extended_data[0])[n];
            av_frame_unref(frame);
        }
    }
done:
    av_frame_free(&frame);avcodec_free_context(&decoder);return written;
}
int ea_aac_encoder_delay(struct ea_aac_recorder *w,unsigned index)
{
    EnterCriticalSection(&w->mutex);int delay=index<w->count?w->tracks[index].stream->codecpar->initial_padding:-1;
    LeaveCriticalSection(&w->mutex);return delay;
}
// Measures each encoder's delay into `delays`; leaves silence after the sweep.
static bool measure(struct ea_aac_recorder *w,int64_t *delays)
{
    struct ea_stem_block *block=bzalloc(sizeof(*block));float *decoded=bmalloc(DECODED_FRAMES*sizeof(float)),*signal=bmalloc(SWEEP_FRAMES*sizeof(float));
    double signal_energy=0;
    for(unsigned n=0;n<SWEEP_FRAMES;++n) {signal[n]=sweep(n,w->rate);signal_energy+=(double)signal[n]*signal[n];}
    bool ok=true;
    for(unsigned start=0;ok && start<SWEEP_FRAMES;start+=AUDIO_OUTPUT_FRAMES) {
        for(unsigned i=0;i<w->count;++i)for(unsigned c=0;c<2;++c)
            memcpy(block->pcm[w->tracks[i].input][c],signal+start,AUDIO_OUTPUT_FRAMES*sizeof(float));
        ok=ea_aac_append(w,block,0,AUDIO_OUTPUT_FRAMES);
    }
    // Silence after the sweep flushes it out of the encoders, and leaves them
    // clean for the first output.
    ok=ok && ea_aac_silence_to(w,CALIBRATION_FRAMES);
    DWORD began=GetTickCount();
    for(bool done=false;ok && !done;) {
        done=true;EnterCriticalSection(&w->mutex);
        // Decoders can hold back part of the first frames; wait for two more.
        for(unsigned i=0;i<w->count;++i)if(w->tracks[i].last_end<DECODED_FRAMES+2*w->tracks[i].stream->codecpar->frame_size)done=false;
        LeaveCriticalSection(&w->mutex);
        if(done)break;
        if(atomic_load(&w->failed) || GetTickCount()-began>5000) { blog(LOG_ERROR,"[extra-audio] Extra-track encoders did not encode the delay measurement"); ok=false; break; }
        WaitForSingleObject(w->progress,50);
    }
    for(unsigned i=0;ok && i<w->count;++i) {
        struct aac_track *t=&w->tracks[i];AVPacket *packets[512];size_t count=0;
        EnterCriticalSection(&w->mutex);
        for(size_t n=0;n<t->replay.size/sizeof(AVPacket *) && count<512;++n) {
            packets[count]=av_packet_clone(*(AVPacket **)deque_data(&t->replay,n*sizeof(AVPacket *)));
            if(packets[count])++count;
        }
        AVCodecParameters *parameters=avcodec_parameters_alloc();
        if(parameters)avcodec_parameters_copy(parameters,t->stream->codecpar);
        LeaveCriticalSection(&w->mutex);
        int64_t length=parameters?decode_track(parameters,packets,count,decoded,DECODED_FRAMES,w->rate):-1;
        for(size_t n=0;n<count;++n)av_packet_free(&packets[n]);avcodec_parameters_free(&parameters);
        if(length<DECODED_FRAMES) {
            blog(LOG_ERROR,"[extra-audio] Could not decode the delay measurement for Track %u (%lld of %d samples from %zu packets)",
                w->bindings[i].number,(long long)length,DECODED_FRAMES,count);
            ok=false; break;
        }
        // Normalised cross-correlation; the decoded energy slides with the lag.
        double best=-1,energy=0;int64_t delay=0;
        for(unsigned n=0;n<SWEEP_FRAMES;++n)energy+=(double)decoded[n]*decoded[n];
        for(int64_t lag=0;lag<LAG_FRAMES;++lag) {
            double dot=0;
            for(unsigned n=0;n<SWEEP_FRAMES;++n)dot+=(double)signal[n]*decoded[lag+n];
            double score=energy>1e-12?dot/sqrt(energy*signal_energy):0;
            if(score>best) {best=score;delay=lag;}
            energy+=(double)decoded[lag+SWEEP_FRAMES]*decoded[lag+SWEEP_FRAMES]-(double)decoded[lag]*decoded[lag];
        }
        if(best<0.5) { blog(LOG_ERROR,"[extra-audio] Track %u delay measurement is inconclusive (match %.2f)",w->bindings[i].number,best); ok=false; break; }
        delays[i]=delay;
    }
    bfree(block);bfree(decoded);bfree(signal);return ok;
}
// The AAC-LC frame length (ISO/IEC 14496-3); HE-AAC frames are twice as long.
#define AAC_LC_FRAME 1024
bool ea_aac_calibrate(struct ea_aac_recorder *w,int64_t *end)
{
    int64_t delays[EA_MAX_EXTRA_TRACKS],reference=0;bool found=false;
    bool ok=measure(w,delays);
    // Timestamps follow OBS's priming value, which matches an AAC-LC encoder
    // and so the native tracks. Correct only what differs from AAC-LC on the
    // same encoder, such as CoreAudio's HE-AAC filter delay.
    for(unsigned i=0;ok && i<w->count && !found;++i)
        if(w->tracks[i].stream->codecpar->frame_size==AAC_LC_FRAME) {reference=delays[i];found=true;}
    if(ok && !found) {
        // No AAC-LC track here: measure one at the highest bitrate the encoder offers.
        unsigned bitrates[EA_MAX_BITRATES];size_t offered=ea_encoder_bitrates(w->encoder_id,w->rate,bitrates,EA_MAX_BITRATES);
        struct ea_extra_track lc={.number=w->bindings[0].number,.bitrate=offered?bitrates[offered-1]:0,.main=true};
        struct ea_aac_recorder *probe=offered?ea_aac_open(w->rate,&lc,1,false,false,w->encoder_id,w->encoder_settings,10):NULL;
        ok=probe && probe->tracks[0].stream->codecpar->frame_size==AAC_LC_FRAME && measure(probe,&reference);
        if(!ok)blog(LOG_ERROR,"[extra-audio] No AAC-LC reference for the extra-track encoder delay");
        ea_aac_close(probe);
    }
    for(unsigned i=0;ok && i<w->count;++i) {
        struct aac_track *t=&w->tracks[i];int64_t correction=delays[i]-reference,reported=obs_encoder_get_priming_samples(t->encoder);
        EnterCriticalSection(&w->mutex);
        t->correction=correction;t->stream->codecpar->initial_padding=(int)(reported+correction);
        LeaveCriticalSection(&w->mutex);
        blog(LOG_INFO,"[extra-audio] Track %u encoder delay: %lld samples more than AAC-LC on this encoder%s",
            w->bindings[i].number,(long long)correction,correction?"; timestamps corrected":"");
    }
    // The sweep is never part of an output; keep the silence after it as preroll.
    if(ok)ea_aac_replay_trim(w,CALIBRATION_FRAMES/2);
    *end=w->position;return ok;
}

bool ea_aac_failed(struct ea_aac_recorder *w) {return atomic_load(&w->failed);}

static bool enqueue(struct ea_aac_recorder *w)
{
    float *queue=atomic_load_explicit(&w->queue,memory_order_relaxed);
    if(!queue) {
        queue=av_malloc(QUEUE_BLOCKS*w->block_floats*sizeof(float));
        if(!queue) { atomic_store(&w->failed,true);return false; }
        atomic_store_explicit(&w->queue,queue,memory_order_release);
    }
    DWORD start=GetTickCount();unsigned write=atomic_load(&w->write);
    for(;;) {
        bool room=true;
        for(unsigned i=0;i<w->group_count;++i) if(write-atomic_load_explicit(&w->groups[i].read,memory_order_acquire)>=QUEUE_BLOCKS) room=false;
        if(room) break;
        if(atomic_load(&w->failed) || GetTickCount()-start>2000) { atomic_store(&w->failed,true);return false; } WaitForSingleObject(w->room,50);
    }
    float *slot=queue+(size_t)(write%QUEUE_BLOCKS)*w->block_floats;
    for(unsigned i=0;i<w->count;++i) memcpy(slot+(size_t)i*2*AUDIO_OUTPUT_FRAMES,w->pending[i],sizeof(w->pending[i]));
    atomic_store_explicit(&w->write,write+1,memory_order_release);w->buffered=0;return !atomic_load(&w->failed);
}
bool ea_aac_release_queue(struct ea_aac_recorder *w)
{
    unsigned write=atomic_load(&w->write);
    for(unsigned i=0;i<w->group_count;++i) if(atomic_load_explicit(&w->groups[i].read,memory_order_acquire)!=write) return false;
    // Every block is taken, so the encoder threads no longer read the queue.
    av_free(atomic_exchange(&w->queue,NULL));return true;
}
int64_t ea_aac_position(const struct ea_aac_recorder *w) { return w->position; }
bool ea_aac_append(struct ea_aac_recorder *w,const struct ea_stem_block *block,unsigned start,unsigned count)
{
    if(atomic_load(&w->failed) || start>AUDIO_OUTPUT_FRAMES || count>AUDIO_OUTPUT_FRAMES-start) return false;
    while(count) {
        unsigned take=AUDIO_OUTPUT_FRAMES-w->buffered;if(take>count)take=count;
        for(unsigned i=0;i<w->count;++i) for(unsigned c=0;c<2;++c) {
            float *target=w->pending[i][c]+w->buffered;
            if(block)memcpy(target,block->pcm[w->tracks[i].input][c]+start,take*sizeof(float));else memset(target,0,take*sizeof(float));
        }
        w->position+=take;w->buffered+=take;start+=take;count-=take;
        if(w->buffered==AUDIO_OUTPUT_FRAMES && !enqueue(w))return false;
    }
    return true;
}
bool ea_aac_silence_to(struct ea_aac_recorder *w,int64_t frame)
{
    while(w->position<frame) {
        unsigned count=frame-w->position>AUDIO_OUTPUT_FRAMES?AUDIO_OUTPUT_FRAMES:(unsigned)(frame-w->position);
        if(!ea_aac_append(w,NULL,0,count))return false;
    }
    return true;
}
bool ea_aac_close(struct ea_aac_recorder *w)
{
    if(!w)return false;
    for(unsigned i=0;i<w->group_count;++i) {
        struct encoder_group *g=&w->groups[i];
        if(g->sink && obs_output_active(g->sink)) {
            // OBS signals deactivate just before it clears the active flag.
            ResetEvent(w->stopped);obs_output_force_stop(g->sink);WaitForSingleObject(w->stopped,5000);
            while(obs_output_active(g->sink))SwitchToThread();
        }
        if(g->audio) { audio_output_close(g->audio);g->audio=NULL; }
        if(g->sink) { signal_handler_disconnect(obs_output_get_signal_handler(g->sink),"deactivate",sink_deactivated,w);obs_output_release(g->sink); }
    }
    for(unsigned i=0;i<w->count;++i) {
        if(w->tracks[i].encoder)obs_encoder_release(w->tracks[i].encoder);
        while(w->tracks[i].replay.size) {AVPacket *p;deque_pop_front(&w->tracks[i].replay,&p,sizeof(p));av_packet_free(&p);}
        deque_free(&w->tracks[i].replay);
    }
    bool ok=!atomic_load(&w->failed);obs_data_release(w->encoder_settings);avformat_free_context(w->output);av_free(atomic_load(&w->queue));DeleteCriticalSection(&w->mutex);
    if(w->room)CloseHandle(w->room);if(w->progress)CloseHandle(w->progress);if(w->stopped)CloseHandle(w->stopped);
    av_free(w);return ok;
}
void ea_aac_wait_progress(struct ea_aac_recorder *w,unsigned milliseconds) { WaitForSingleObject(w->progress,milliseconds); }
void ea_aac_replay_retain(struct ea_aac_recorder *w,unsigned seconds)
{EnterCriticalSection(&w->mutex);w->retention_seconds=seconds;LeaveCriticalSection(&w->mutex);}
void ea_aac_replay_trim(struct ea_aac_recorder *w,int64_t frame)
{
    EnterCriticalSection(&w->mutex);
    for(unsigned i=0;i<w->count;++i) {
        struct aac_track *t=&w->tracks[i];
        while(t->replay.size) {
            AVPacket *first;deque_peek_front(&t->replay,&first,sizeof(first));
            if(corrected(t,first->pts)+first->duration>=frame)break;
            drop_front(w,t);
        }
    }
    LeaveCriticalSection(&w->mutex);
}
uint64_t ea_aac_replay_bytes(struct ea_aac_recorder *w)
{EnterCriticalSection(&w->mutex);uint64_t bytes=w->replay_bytes;LeaveCriticalSection(&w->mutex);return bytes;}
int64_t ea_aac_replay_ready(struct ea_aac_recorder *w)
{
    int64_t ready=INT64_MAX;EnterCriticalSection(&w->mutex);
    if(atomic_load(&w->failed))ready=-1;
    else for(unsigned i=0;i<w->count;++i)if(corrected(&w->tracks[i],w->tracks[i].last_end)<ready)ready=corrected(&w->tracks[i],w->tracks[i].last_end);
    LeaveCriticalSection(&w->mutex);return ready;
}
bool ea_aac_replay_export(struct ea_aac_recorder *w,const char *path,bool vertical,bool all,int64_t start,int64_t frames)
{
    AVFormatContext *out=NULL;struct deque packets={0};bool ok=false,header=false;
    if(!w || frames<=0 || avformat_alloc_output_context2(&out,NULL,"mp4",path)<0)return false;
    out->avoid_negative_ts=AVFMT_AVOID_NEG_TS_DISABLED;
    EnterCriticalSection(&w->mutex);
    for(unsigned i=0;i<w->count;++i)if(all || (vertical?w->bindings[i].vertical:w->bindings[i].main)) {
        struct aac_track *t=&w->tracks[i];AVStream *stream=avformat_new_stream(out,NULL);if(!stream)goto unlock;
        if(avcodec_parameters_copy(stream->codecpar,t->stream->codecpar)<0)goto unlock;
        stream->time_base=t->stream->time_base;av_dict_copy(&stream->metadata,t->stream->metadata,0);
        bool found=false;int64_t last=INT64_MIN;
        // Decoder preroll stays in the MP4 edit list; the visible clip starts at zero.
        for(size_t n=0;n<t->replay.size/sizeof(AVPacket *);++n) {
            AVPacket *p=*(AVPacket **)deque_data(&t->replay,n*sizeof(p));int64_t pts=corrected(t,p->pts);
            if(pts+p->duration<start-(int64_t)w->rate/2)continue;
            if(pts>=start+frames)break;
            if(!found && pts>start)goto unlock;
            AVPacket *copy=av_packet_clone(p);if(!copy)goto unlock;
            copy->pts=pts-start;copy->dts=corrected(t,p->dts)-start;copy->stream_index=stream->index;
            if(copy->pts+copy->duration>frames)copy->duration=frames-copy->pts;
            last=copy->pts+copy->duration;found=true;deque_push_back(&packets,&copy,sizeof(copy));
        }
        if(!found || last<frames)goto unlock;
    }
    ok=out->nb_streams && !atomic_load(&w->failed);
unlock:
    LeaveCriticalSection(&w->mutex);
    if(ok && avio_open(&out->pb,path,AVIO_FLAG_WRITE)>=0) {
        AVDictionary *options=NULL;av_dict_set_int(&options,"movie_timescale",1000,0);
        ok=avformat_write_header(out,&options)>=0;av_dict_free(&options);header=ok;
        while(ok && packets.size) {
            AVPacket *p;deque_pop_front(&packets,&p,sizeof(p));
            ok=av_interleaved_write_frame(out,p)>=0;av_packet_free(&p);
        }
        if(header && av_write_trailer(out)<0)ok=false;
    }else ok=false;
    while(packets.size) {AVPacket *p;deque_pop_front(&packets,&p,sizeof(p));av_packet_free(&p);}deque_free(&packets);
    if(out->pb && avio_closep(&out->pb)<0)ok=false;
    avformat_free_context(out);return ok && ea_mp4_set_audio_duration(path,w->rate,frames);
}
bool ea_recording_audio_frames(const char *path,unsigned rate,int64_t *frames)
{
    if(ea_mp4_audio_frames(path,rate,frames))return true;
    AVFormatContext *input=NULL;
    // Header/index only: never scan or decode the native video/audio payload.
    if(avformat_open_input(&input,path,NULL,NULL)<0)return false;
    bool found=false;
    for(unsigned i=0;i<input->nb_streams;++i) {
        AVStream *s=input->streams[i];
        if(s->codecpar->codec_type!=AVMEDIA_TYPE_AUDIO)continue;
        if(s->duration>0 && s->duration!=AV_NOPTS_VALUE) {
            *frames=av_rescale_q(s->duration,s->time_base,(AVRational){1,(int)rate});found=true;
        }
        break;
    }
    avformat_close_input(&input);return found;
}

struct tee_track {
    unsigned source;
    AVStream *stream;
    uint64_t next; // Index of the next packet to copy, counted like aac_track.pushed.
    bool started;
    int64_t written_end;
};
struct ea_aac_tee {
    struct ea_aac_recorder *source;
    AVFormatContext *output;
    int64_t offset;
    struct tee_track tracks[EA_MAX_EXTRA_TRACKS];
    unsigned count;
    bool header, wrote;
    atomic_bool failed;
};
struct ea_aac_tee *ea_aac_tee_open(struct ea_aac_recorder *source,const char *path,bool vertical,int64_t offset)
{
    struct ea_aac_tee *tee=av_mallocz(sizeof(*tee)); if(!tee) return NULL;
    tee->source=source;tee->offset=offset;atomic_init(&tee->failed,false);
    if(!check(&tee->failed,avformat_alloc_output_context2(&tee->output,NULL,"mp4",path),"container")) goto fail;
    tee->output->avoid_negative_ts=AVFMT_AVOID_NEG_TS_DISABLED;
    for(unsigned i=0;i<source->count;++i) if(vertical?source->bindings[i].vertical:source->bindings[i].main) {
        struct tee_track *t=&tee->tracks[tee->count++];const AVStream *from=source->tracks[i].stream;t->source=i;
        t->stream=avformat_new_stream(tee->output,NULL);if(!t->stream) goto fail;
        if(avcodec_parameters_copy(t->stream->codecpar,from->codecpar)<0) goto fail;
        t->stream->time_base=from->time_base;av_dict_copy(&t->stream->metadata,from->metadata,0);
    }
    if(!tee->count) goto fail;
    if(!check(&tee->failed,avio_open(&tee->output->pb,path,AVIO_FLAG_WRITE),"file")) goto fail;
    AVDictionary *options=NULL;av_dict_set(&options,"movflags","+hybrid_fragmented",0);
    av_dict_set_int(&options,"frag_duration",1000000,0);av_dict_set_int(&options,"movie_timescale",1000,0);
    bool ok=check(&tee->failed,avformat_write_header(tee->output,&options),"header");av_dict_free(&options);if(!ok) goto fail;
    tee->header=true;return tee;
fail:
    atomic_store(&tee->failed,true);ea_aac_tee_finish(tee,0);return NULL;
}
// Source mutex held. Finds the last packet at or before the output's start
// minus the encoder delay, once a later packet shows it is the last one.
static int tee_start(struct ea_aac_tee *tee,struct tee_track *t)
{
    const struct aac_track *s=&tee->source->tracks[t->source];size_t count=s->replay.size/sizeof(AVPacket *);
    int64_t want=tee->offset-s->stream->codecpar->initial_padding;
    size_t low=0,high=count;
    while(low<high) {
        size_t middle=(low+high)/2;const AVPacket *p=*(AVPacket **)deque_data((struct deque *)&s->replay,middle*sizeof(p));
        if(corrected(s,p->pts)>want) high=middle; else low=middle+1;
    }
    if(low==count) return 0;
    uint64_t front=s->pushed-count;
    if(low) { t->next=front+low-1; return 1; }
    // A new encoder has nothing earlier: its first packet already starts with
    // the full encoder delay.
    const AVPacket *first=*(AVPacket **)deque_data((struct deque *)&s->replay,0);
    if(!s->trimmed && corrected(s,first->pts)<=tee->offset) { t->next=front; return 1; }
    return -1;
}
static bool tee_copy(struct ea_aac_tee *tee,int64_t frames,bool final)
{
    struct ea_aac_recorder *w=tee->source;
    for(unsigned i=0;i<tee->count && !atomic_load(&tee->failed);++i) {
        struct tee_track *t=&tee->tracks[i];struct aac_track *s=&w->tracks[t->source];
        for(;;) {
            AVPacket *batch[TEE_BATCH];unsigned taken=0;bool more=false;const char *lost=NULL;
            EnterCriticalSection(&w->mutex);
            if(!t->started) {
                int found=tee_start(tee,t);
                if(found<0) lost="the recording's start";
                t->started=found>0;
            }
            if(t->started && !lost) {
                size_t count=s->replay.size/sizeof(AVPacket *);uint64_t front=s->pushed-count;
                if(t->next<front) lost="part of the recording";
                for(size_t n=lost?count:(size_t)(t->next-front);n<count;++n) {
                    if(taken==TEE_BATCH) { more=true; break; }
                    const AVPacket *p=*(AVPacket **)deque_data(&s->replay,n*sizeof(p));
                    int64_t pts=corrected(s,p->pts)-tee->offset;
                    if(final?pts>=frames:pts+p->duration>frames) break;
                    AVPacket *copy=av_packet_clone(p);if(!copy) { lost="memory for a packet"; break; }
                    copy->pts=pts;copy->dts=corrected(s,p->dts)-tee->offset;copy->stream_index=t->stream->index;
                    if(copy->pts+copy->duration>frames) copy->duration=frames-copy->pts;
                    batch[taken++]=copy;++t->next;
                }
            }
            LeaveCriticalSection(&w->mutex);
            if(lost) { blog(LOG_ERROR,"[extra-audio] Shared extra audio is missing %s; the original recording is retained.",lost); atomic_store(&tee->failed,true); }
            for(unsigned n=0;n<taken;++n) {
                t->written_end=batch[n]->pts+batch[n]->duration;
                if(!atomic_load(&tee->failed) && check(&tee->failed,av_interleaved_write_frame(tee->output,batch[n]),"write")) tee->wrote=true;
                av_packet_free(&batch[n]);
            }
            if(!more || atomic_load(&tee->failed)) break;
        }
    }
    return !atomic_load(&tee->failed);
}
bool ea_aac_tee_pump(struct ea_aac_tee *tee,int64_t frames) { return tee_copy(tee,frames,false); }
bool ea_aac_tee_finish(struct ea_aac_tee *tee,int64_t frames)
{
    if(!tee) return false;
    bool ok=!atomic_load(&tee->failed);
    if(ok && frames>0) {
        ok=tee_copy(tee,frames,true);
        for(unsigned i=0;ok && i<tee->count;++i) if(tee->tracks[i].written_end<frames) {
            blog(LOG_ERROR,"[extra-audio] Shared extra audio ended %lld samples before the recording",(long long)(frames-tee->tracks[i].written_end));
            ok=false;
        }
    }
    if(ok && tee->header && tee->wrote) ok=check(&tee->failed,av_write_trailer(tee->output),"finish");
    if(tee->output && tee->output->pb && avio_closep(&tee->output->pb)<0) ok=false;
    avformat_free_context(tee->output);av_free(tee);return ok;
}
