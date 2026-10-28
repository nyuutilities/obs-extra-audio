// SPDX-License-Identifier: GPL-2.0-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <libavutil/mathematics.h>
#include "replay-manager.h"
#include "aac-recorder.h"
#include "encoder-lane.h"
#include "mp3-backup.h"
// Read-only adapters pinned to OBS 32.2.2. Never modify native state.
#include "obs-ffmpeg-mux.h"
#include <string.h>

#define WATCHES 8u
#define CLIPS 8u
#define PAUSE_OUTPUTS 16u
struct clip_window {int64_t start_frame,end_frame,request;bool valid;};
struct replay_watch {
    obs_output_t *output;bool vertical,wanted,probing;
    // Extra tracks come from the encoders shared with this output's recording.
    // Packet callbacks use `user` while the UI thread may leave the lane.
    CRITICAL_SECTION user_lock;struct ea_lane_user *user;
    atomic_llong oldest_frame;atomic_bool failed;
    struct clip_window pending;
};
struct pause_watch {obs_output_t *output;};
struct replay_clip {
    bool used,vertical,exported,done,backup_failed;struct clip_window window;struct replay_watch *owner;
    char native[8192],audio[8192],job[8192],state[256];HANDLE thread,process;
};
static struct {
    bool initialized;atomic_bool closing;CRITICAL_SECTION mutex;
    struct ea_record_config config;struct ea_source_recorder *source;
    struct replay_watch watches[WATCHES];struct pause_watch pause_outputs[PAUSE_OUTPUTS];struct replay_clip clips[CLIPS];
    char helper[8192],status[512],vertical_name[256],backup_directory[8192];
} r;

static int64_t packet_frame(const struct encoder_packet *p,unsigned rate)
{return av_rescale_q(p->pts,(AVRational){p->timebase_num,p->timebase_den},(AVRational){1,(int)rate});}
static struct encoder_packet *native_packet(struct ffmpeg_muxer *s,size_t n)
{return deque_data(&s->packets,n*sizeof(struct encoder_packet));}
static bool keyframe(const struct encoder_packet *p) {return p->type==OBS_ENCODER_VIDEO && p->keyframe;}
static void virtual_purge(struct ffmpeg_muxer *s,size_t *first,size_t count,int *keys,int64_t *bytes)
{
    const struct encoder_packet *p=native_packet(s,*first);bool key=keyframe(p);
    *bytes-=(int64_t)p->size;if(key)--*keys;++*first;
    if(key)while(*first<count && !keyframe(native_packet(s,*first))) {
        *bytes-=(int64_t)native_packet(s,*first)->size;++*first;
    }
}
// user_lock held.
static struct clip_window snapshot_window(struct replay_watch *w,struct ffmpeg_muxer *s,const struct encoder_packet *incoming)
{
    struct clip_window window={.request=s->save_ts};size_t count=s->packets.size/sizeof(struct encoder_packet),first=0;
    int keys=s->keyframes;int64_t bytes=s->cur_size;
    if(s->max_size && count && keys>2)
        while(first<count && bytes+(int64_t)incoming->size>s->max_size)virtual_purge(s,&first,count,&keys,&bytes);
    if(count && keys>2)
        while(first<count && incoming->dts_usec-native_packet(s,first)->dts_usec>s->max_time)virtual_purge(s,&first,count,&keys,&bytes);
    const struct encoder_packet *start=NULL,*last=NULL;
    for(size_t n=first;n<count;++n) {
        const struct encoder_packet *p=native_packet(s,n);
        if(p->type==OBS_ENCODER_AUDIO && p->track_idx==0) {if(!start)start=p;last=p;}
    }
    if(incoming->type==OBS_ENCODER_AUDIO && incoming->track_idx==0) {if(!start)start=incoming;last=incoming;}
    int64_t offset,base;
    if(!start || !last || !w->user || !ea_lane_offset(w->user,&offset,&base,NULL))return window;
    unsigned rate=ea_source_recorder_rate(r.source);
    unsigned frames=(unsigned)obs_encoder_get_frame_size(obs_output_get_audio_encoder(s->output,0));
    window.start_frame=packet_frame(start,rate)+offset;
    window.end_frame=packet_frame(last,rate)+offset+(frames?frames:1024);
    window.valid=window.start_frame>=base && window.end_frame>window.start_frame;
    return window;
}
static void native_started(void *data,calldata_t *unused)
{
    (void)unused;struct replay_watch *w=data;
    EnterCriticalSection(&w->user_lock);if(w->user)ea_lane_restart(w->user);LeaveCriticalSection(&w->user_lock);
}
static void native_pause(void *data,calldata_t *params)
{
    (void)data;obs_output_t *output=calldata_ptr(params,"output");
    if(output)ea_lane_pause(obs_output_get_audio_encoder(output,0));
}
static void native_unpause(void *data,calldata_t *params)
{
    (void)data;obs_output_t *output=calldata_ptr(params,"output");
    if(output)ea_lane_unpause(obs_output_get_audio_encoder(output,0));
}
// Outputs signal "starting" right after their encoders start; OBS frontend
// STARTING events come earlier for OBS's own outputs.
static void output_starting(void *data,calldata_t *unused) {(void)data;(void)unused;ea_record_manager_prepare();}
static bool watch_pause_output(void *unused,obs_output_t *output)
{
    // Replay buffers share their recording's encoders and are never paused
    // directly. Referencing them here as well would keep released ones alive.
    (void)unused;if(!obs_output_can_pause(output) || !strcmp(obs_output_get_id(output),"replay_buffer"))return true;
    for(unsigned i=0;i<PAUSE_OUTPUTS;++i)if(r.pause_outputs[i].output==output)return true;
    for(unsigned i=0;i<PAUSE_OUTPUTS;++i)if(!r.pause_outputs[i].output) {
        struct pause_watch *w=&r.pause_outputs[i];w->output=obs_output_get_ref(output);
        signal_handler_t *signals=obs_output_get_signal_handler(output);
        signal_handler_connect(signals,"pause",native_pause,w);signal_handler_connect(signals,"unpause",native_unpause,w);
        signal_handler_connect(signals,"starting",output_starting,w);break;
    }
    return true;
}
static void replay_packet(obs_output_t *out,struct encoder_packet *packet,struct encoder_packet_time *time,void *data)
{
    (void)time;struct replay_watch *w=data;if(!w->wanted || atomic_load(&r.closing))return;
    struct ffmpeg_muxer *native=obs_obj_get_data(out);if(!native || native->output!=out || !native->active)return;
    bool save=native->save_ts && !native->muxing && packet->sys_dts_usec>=native->save_ts;
    struct clip_window window={0};
    EnterCriticalSection(&w->user_lock);
    if(w->user && packet->type==OBS_ENCODER_AUDIO && packet->track_idx==0) {
        ea_lane_packet(w->user,packet);
        int64_t offset;
        // Retain media time, not wall time: a long pause cannot grow RAM.
        if(ea_lane_offset(w->user,&offset,NULL,NULL)) {
            unsigned rate=ea_source_recorder_rate(r.source);
            const struct encoder_packet *first=native->packets.size?native_packet(native,0):packet;
            int64_t age=av_rescale_q(packet->dts_usec-first->dts_usec,(AVRational){1,1000000},(AVRational){1,(int)rate});
            atomic_store(&w->oldest_frame,packet_frame(packet,rate)+offset-age);
        }
    }
    if(save)window=snapshot_window(w,native,packet);
    LeaveCriticalSection(&w->user_lock);
    if(save) {EnterCriticalSection(&r.mutex);w->pending=window;LeaveCriticalSection(&r.mutex);}
}
static void replay_saved(void *data,calldata_t *unused)
{
    (void)unused;struct replay_watch *w=data;if(!w->wanted || atomic_load(&r.closing))return;
    calldata_t cd={0};proc_handler_call(obs_output_get_proc_handler(w->output),"get_last_replay",&cd);
    const char *path=calldata_string(&cd,"path");EnterCriticalSection(&r.mutex);struct replay_clip *clip=NULL;
    for(unsigned i=0;i<CLIPS;++i)if(!r.clips[i].used) {clip=&r.clips[i];break;}
    if(clip && path && *path && strlen(path)<7800) {
        memset(clip,0,sizeof(*clip));clip->used=true;clip->vertical=w->vertical;clip->owner=w;clip->window=w->pending;
        snprintf(clip->native,sizeof(clip->native),"%s",path);snprintf(clip->audio,sizeof(clip->audio),"%s.extra-audio.m4a",path);
        snprintf(clip->job,sizeof(clip->job),"%s.extra-audio-job.json",path);snprintf(clip->state,sizeof(clip->state),obs_module_text("Clip.Waiting"));
        blog(LOG_INFO,"[extra-audio] %s clip saved: %s; media frames=%lld..%lld; valid=%d",w->vertical?"Aitum Backtrack":"OBS replay",path,
            (long long)clip->window.start_frame,(long long)clip->window.end_frame,clip->window.valid);
        if(w->user)blog(LOG_INFO,"[extra-audio] %s compressed replay audio in RAM: %llu bytes",w->vertical?"Aitum":"OBS",(unsigned long long)ea_aac_replay_bytes(ea_lane_audio(w->user)));
    }else blog(LOG_ERROR,"[extra-audio] Could not queue replay clip; original retained: %s",path?path:"");
    LeaveCriticalSection(&r.mutex);calldata_free(&cd);
}
static bool watch_output(void *main,obs_output_t *output)
{
    if(strcmp(obs_output_get_id(output),"replay_buffer"))return true;
    const char *name=obs_output_get_name(output);bool vertical=!strncmp(name,r.vertical_name,strlen(r.vertical_name));
    if(vertical && !ea_aitum_supported())return true;
    if(!vertical && output!=main && strcmp(name,"adv_replay_buffer") && strcmp(name,"simple_replay_buffer"))return true;
    for(unsigned i=0;i<WATCHES;++i)if(r.watches[i].output==output)return true;
    for(unsigned i=0;i<WATCHES;++i)if(!r.watches[i].output) {
        struct replay_watch *w=&r.watches[i];InitializeCriticalSection(&w->user_lock);w->output=obs_output_get_ref(output);w->vertical=vertical;
        w->wanted=r.config.backup_mp3;for(unsigned t=0;t<r.config.track_count;++t)w->wanted|=vertical?r.config.tracks[t].vertical:r.config.tracks[t].main;
        atomic_init(&w->oldest_frame,0);atomic_init(&w->failed,false);
        obs_output_add_packet_callback(output,replay_packet,w);signal_handler_t *signals=obs_output_get_signal_handler(output);
        signal_handler_connect(signals,"saved",replay_saved,w);signal_handler_connect(signals,"start",native_started,w);
        signal_handler_connect(signals,"starting",output_starting,w);
        blog(LOG_INFO,"[extra-audio] Watching native %s: %s",vertical?"Backtrack":"Replay Buffer",name);break;
    }
    return true;
}
static DWORD WINAPI export_clip(void *data)
{
    struct replay_clip *clip=data;struct replay_watch *w=clip->owner;SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    struct ea_aac_recorder *audio=w->user?ea_lane_audio(w->user):NULL;
    if(!clip->window.valid || !audio || atomic_load(&w->failed) || ea_lane_failed(w->user))goto fail;
    if(os_file_exists(clip->audio) || os_file_exists(clip->job))goto fail;
    unsigned rate=ea_source_recorder_rate(r.source);int64_t frames,start=clip->window.start_frame;
    if(!ea_recording_audio_frames(clip->native,rate,&frames))goto fail;
    if(!ea_lane_wait_ready(w->user,start+frames))goto fail;
    if(r.config.backup_mp3) {
        char backup[8192];snprintf(backup,sizeof(backup),"%s.extra-audio-backup.m4a",clip->native);
        // Each clip output has its own folder beside the recordings' Main and Vertical.
        char directory[8300]="";if(*r.backup_directory)snprintf(directory,sizeof(directory),"%s/%s",r.backup_directory,clip->vertical?"Vertical Backtrack":"Replay Buffer");
        bool saved=!os_file_exists(backup) && ea_aac_replay_export(audio,backup,clip->vertical,true,start,frames) &&
            ea_mp3_from_aac(backup,directory,r.config.backup_format,clip->native,rate,r.config.tracks,r.config.track_count,frames);
        if(saved) {wchar_t path[8192];MultiByteToWideChar(CP_UTF8,0,backup,-1,path,8192);DeleteFileW(path);}
        else {clip->backup_failed=true;blog(LOG_ERROR,"[extra-audio] Replay MP3 backup failed; native clip and any encoded recovery audio retained: %s",clip->native);}
    }
    unsigned selected=0;for(unsigned i=0;i<r.config.track_count;++i)selected+=clip->vertical?r.config.tracks[i].vertical:r.config.tracks[i].main;
    if(!selected) {snprintf(clip->state,sizeof(clip->state),obs_module_text("Clip.NoTracks"));clip->done=true;return 0;}
    if(!ea_aac_replay_export(audio,clip->audio,clip->vertical,false,start,frames))goto fail;
    const char *dot=strrchr(clip->native,'.');
    if(!dot || _stricmp(dot,".mp4")) {snprintf(clip->state,sizeof(clip->state),obs_module_text("Clip.CompanionOnly"));clip->done=true;return 0;}
    obs_data_t *job=obs_data_create();obs_data_set_string(job,"native_path",clip->native);obs_data_set_string(job,"audio_path",clip->audio);
    obs_data_set_bool(job,"replay_clip",true);
    bool saved=obs_data_save_json_safe(job,clip->job,"tmp",NULL);obs_data_release(job);if(!saved)goto fail;
    clip->exported=true;snprintf(clip->state,sizeof(clip->state),obs_module_text("Clip.Embedding"));return 0;
fail:
    snprintf(clip->state,sizeof(clip->state),obs_module_text("Clip.Failed"));
    blog(LOG_ERROR,"[extra-audio] Replay export failed: %s (valid clock=%d, buffer failed=%d)",clip->native,clip->window.valid,atomic_load(&w->failed));clip->done=true;return 1;
}
static void launch_clip(struct replay_clip *clip)
{
    wchar_t exe[8192],args[17000],directory[8192];MultiByteToWideChar(CP_UTF8,0,r.helper,-1,exe,8192);swprintf(args,17000,L"\"%ls\" \"",exe);
    size_t used=wcslen(args);MultiByteToWideChar(CP_UTF8,0,clip->job,-1,args+used,17000-(int)used);wcscat(args,L"\"");
    wcscpy(directory,exe);wchar_t *slash=wcsrchr(directory,L'/'),*back=wcsrchr(directory,L'\\');if(back && (!slash || back>slash))slash=back;if(slash)*slash=0;
    STARTUPINFOW si={.cb=sizeof(si),.dwFlags=STARTF_USESHOWWINDOW,.wShowWindow=SW_HIDE};PROCESS_INFORMATION pi={0};
    if(CreateProcessW(exe,args,NULL,NULL,FALSE,CREATE_NO_WINDOW|BELOW_NORMAL_PRIORITY_CLASS,NULL,directory,&si,&pi)) {CloseHandle(pi.hThread);clip->process=pi.hProcess;}
    else {clip->done=true;snprintf(clip->state,sizeof(clip->state),obs_module_text("Clip.LaunchFailed"));}
}
static void close_buffer(struct replay_watch *w)
{
    EnterCriticalSection(&w->user_lock);struct ea_lane_user *user=w->user;w->user=NULL;LeaveCriticalSection(&w->user_lock);
    ea_lane_leave(user);atomic_store(&w->oldest_frame,0);
}
void ea_replay_configure(const struct ea_record_config *config,struct ea_source_recorder *source,const char *helper)
{
    ea_replay_close();memset(&r,0,sizeof(r));InitializeCriticalSection(&r.mutex);r.initialized=true;atomic_init(&r.closing,false);
    r.config=*config;r.source=source;snprintf(r.helper,sizeof(r.helper),"%s",helper);obs_module_t *aitum=obs_get_module("vertical-canvas");
    const char *vertical=aitum?obs_module_get_locale_text(aitum,"Vertical"):"Vertical",*backtrack=aitum?obs_module_get_locale_text(aitum,"Backtrack"):"Backtrack";
    snprintf(r.vertical_name,sizeof(r.vertical_name),"%s %s",vertical,backtrack);
    const char *directory=config_get_string(obs_frontend_get_profile_config(),"AdvOut","RecFilePath");if(directory && *directory)snprintf(r.backup_directory,sizeof(r.backup_directory),"%s/Extra Audio",directory);
    snprintf(r.status,sizeof(r.status),obs_module_text("Replay.Ready"));
}
static bool watch_idle(const struct replay_watch *w)
{
    if(!w->output || w->user || obs_output_active(w->output))return false;
    for(unsigned i=0;i<CLIPS;++i)if(r.clips[i].used && r.clips[i].owner==w)return false;
    return true;
}
// OBS recreates its outputs whenever output settings are saved, and Aitum can
// recreate its own. Probe an idle output by dropping only this plugin's
// reference: one its owner already released is destroyed here, not kept.
static obs_output_t *owner_retained(obs_output_t *output)
{
    obs_weak_output_t *weak=obs_output_get_weak_output(output);obs_output_release(output);
    output=obs_weak_output_get_output(weak);obs_weak_output_release(weak);return output;
}
static void prune_released(void)
{
    for(unsigned i=0;i<WATCHES;++i) {
        struct replay_watch *w=&r.watches[i];EnterCriticalSection(&r.mutex);
        bool idle=watch_idle(w);if(idle)w->probing=true;LeaveCriticalSection(&r.mutex);
        if(!idle)continue;
        // Outside r.mutex: destroying a replay buffer joins its mux thread,
        // and that thread's saved signal takes r.mutex.
        obs_output_t *output=owner_retained(w->output);EnterCriticalSection(&r.mutex);
        if(output) {w->output=output;w->probing=false;}
        else {
            for(unsigned j=0;j<CLIPS;++j)if(r.clips[j].used && r.clips[j].owner==w) {
                struct replay_clip *clip=&r.clips[j];clip->owner=NULL;clip->done=true;
                snprintf(clip->state,sizeof(clip->state),obs_module_text("Clip.Unavailable"));
            }
            blog(LOG_INFO,"[extra-audio] Released a %s output that %s no longer uses",w->vertical?"Backtrack":"Replay Buffer",w->vertical?"Aitum":"OBS");
            DeleteCriticalSection(&w->user_lock);memset(w,0,sizeof(*w));
        }
        LeaveCriticalSection(&r.mutex);
    }
    for(unsigned i=0;i<PAUSE_OUTPUTS;++i) {
        struct pause_watch *p=&r.pause_outputs[i];
        if(p->output && !obs_output_active(p->output))p->output=owner_retained(p->output);
    }
}
void ea_replay_tick(void)
{
    if(!r.initialized || !r.source || !r.config.enabled)return;
    prune_released();
    obs_output_t *main=obs_frontend_get_replay_buffer_output();obs_enum_outputs(watch_output,main);obs_output_release(main);obs_enum_outputs(watch_pause_output,NULL);
    // Start capture before taking r.mutex: adding an audio connection waits for
    // OBS's audio thread, whose replay packets can take r.mutex during a save.
    for(unsigned i=0;i<WATCHES;++i) {
        struct replay_watch *w=&r.watches[i];
        if(w->output && w->wanted && !w->user && !atomic_load(&w->failed) && obs_output_active(w->output)) {ea_record_manager_prepare();break;}
    }
    bool worker=false;EnterCriticalSection(&r.mutex);
    for(unsigned i=0;i<CLIPS;++i) {
        struct replay_clip *clip=&r.clips[i];if(!clip->used)continue;
        if(clip->process && WaitForSingleObject(clip->process,0)==WAIT_OBJECT_0) {
            DWORD code;GetExitCodeProcess(clip->process,&code);CloseHandle(clip->process);clip->process=NULL;clip->done=true;
            snprintf(clip->state,sizeof(clip->state),code?obs_module_text("Clip.EmbedFailed"):obs_module_text("Clip.Embedded"));
            blog(code?LOG_ERROR:LOG_INFO,"[extra-audio] Replay finalizer exited %lu: %s",(unsigned long)code,clip->native);
        }
        if(clip->thread && WaitForSingleObject(clip->thread,0)==WAIT_OBJECT_0) {CloseHandle(clip->thread);clip->thread=NULL;if(clip->exported)launch_clip(clip);}
        worker|=clip->thread!=NULL;
        if(clip->done && !clip->thread && !clip->process)clip->used=false;
    }
    unsigned rate=ea_source_recorder_rate(r.source);
    for(unsigned i=0;i<WATCHES;++i) {
        struct replay_watch *w=&r.watches[i];if(!w->output || !w->wanted)continue;
        bool active=obs_output_active(w->output),pending=false;unsigned seconds=30;
        int64_t current=w->user?ea_lane_position(w->user):0,oldest=atomic_load(&w->oldest_frame);
        obs_data_t *settings=obs_output_get_settings(w->output);unsigned length=(unsigned)obs_data_get_int(settings,"max_time_sec");obs_data_release(settings);if(length+30>seconds)seconds=length+30;
        if(current>oldest && (uint64_t)((current-oldest)/rate)+30>seconds)seconds=(unsigned)((current-oldest)/rate)+30;
        for(unsigned j=0;j<CLIPS;++j)if(r.clips[j].used && r.clips[j].owner==w && !r.clips[j].done && !r.clips[j].exported) {
            pending=true;int64_t age=current-r.clips[j].window.start_frame;if(age>0 && (uint64_t)(age/rate)+30>seconds)seconds=(unsigned)(age/rate)+30;
        }
        if(active && !w->user && !atomic_load(&w->failed) && ea_source_recorder_capturing(r.source)) {
            config_t *profile=obs_frontend_get_profile_config();
            const char *directory=config_get_string(profile,"AdvOut","RecFilePath");if(directory && *directory)snprintf(r.backup_directory,sizeof(r.backup_directory),"%s/Extra Audio",directory);
            struct ea_lane_user *user=ea_lane_join(w->output,w->vertical,seconds);
            if(user) {
                EnterCriticalSection(&w->user_lock);w->user=user;LeaveCriticalSection(&w->user_lock);
                blog(LOG_INFO,"[extra-audio] %s replay audio started in RAM; retention=%u media seconds; tracks=%u",w->vertical?"Aitum":"OBS",seconds,ea_aac_track_count(ea_lane_audio(user)));
                snprintf(r.status,sizeof(r.status),obs_module_text("Replay.Running"));
            }else {atomic_store(&w->failed,true);snprintf(r.status,sizeof(r.status),obs_module_text("Replay.StartFailed"));}
        }
        if(w->user) {
            int64_t offset,base;
            // An output older than the shared encoders' current run would map
            // its earliest audio onto the previous run's.
            if(!atomic_load(&w->failed) && ea_lane_offset(w->user,&offset,&base,NULL) && offset<base) {
                atomic_store(&w->failed,true);
                blog(LOG_ERROR,"[extra-audio] %s replay started before its shared extra-track encoders; its clips keep only native tracks",w->vertical?"Aitum":"OBS");
            }
            ea_lane_retain(w->user,atomic_load(&w->failed)?0:seconds);
        }
        if(!active && !pending && w->user)close_buffer(w);
        if(!active && !pending && !w->user)atomic_store(&w->failed,false);
    }
    for(unsigned i=0;i<CLIPS && !worker;++i)if(r.clips[i].used && !r.clips[i].done && !r.clips[i].exported && !r.clips[i].thread) {
        struct replay_clip *clip=&r.clips[i];if(clip->owner->user) {clip->thread=CreateThread(NULL,0,export_clip,clip,0,NULL);worker=clip->thread!=NULL;}
        if(!worker)clip->done=true;
    }
    if(ea_lanes_bytes()>256ULL*1024*1024)for(unsigned i=0;i<WATCHES;++i)if(r.watches[i].user && !atomic_exchange(&r.watches[i].failed,true))
        blog(LOG_ERROR,"[extra-audio] Combined compressed replay audio reached its 256 MiB RAM limit; original clips retained");
    LeaveCriticalSection(&r.mutex);
}
void ea_replay_close(void)
{
    if(!r.initialized)return;atomic_store(&r.closing,true);
    for(unsigned i=0;i<PAUSE_OUTPUTS;++i)if(r.pause_outputs[i].output) {
        obs_output_t *out=r.pause_outputs[i].output;signal_handler_t *signals=obs_output_get_signal_handler(out);
        signal_handler_disconnect(signals,"pause",native_pause,&r.pause_outputs[i]);signal_handler_disconnect(signals,"unpause",native_unpause,&r.pause_outputs[i]);
        signal_handler_disconnect(signals,"starting",output_starting,&r.pause_outputs[i]);obs_output_release(out);
    }
    for(unsigned i=0;i<WATCHES;++i)if(r.watches[i].output) {
        struct replay_watch *w=&r.watches[i];obs_output_remove_packet_callback(w->output,replay_packet,w);signal_handler_t *signals=obs_output_get_signal_handler(w->output);
        signal_handler_disconnect(signals,"saved",replay_saved,w);signal_handler_disconnect(signals,"start",native_started,w);
        signal_handler_disconnect(signals,"starting",output_starting,w);
    }
    // Pending clips finish from the shared encoders, which encode up to the
    // last output's end once every output is treated as stopped.
    for(unsigned i=0;i<CLIPS;++i)if(r.clips[i].used) {
        struct replay_clip *clip=&r.clips[i];if(clip->thread) {WaitForSingleObject(clip->thread,INFINITE);CloseHandle(clip->thread);}
        else if(!clip->exported && !clip->done && clip->owner && clip->owner->user)export_clip(clip);
        if(clip->exported && !clip->process)launch_clip(clip);if(clip->process)CloseHandle(clip->process);
    }
    for(unsigned i=0;i<WATCHES;++i)if(r.watches[i].output) {close_buffer(&r.watches[i]);obs_output_release(r.watches[i].output);DeleteCriticalSection(&r.watches[i].user_lock);}
    DeleteCriticalSection(&r.mutex);r.initialized=false;
}
bool ea_replay_needs_capture(void)
{
    if(!r.initialized)return false;
    for(unsigned i=0;i<WATCHES;++i) {
        const struct replay_watch *w=&r.watches[i];
        if(w->output && w->wanted && !w->probing && obs_output_active(w->output))return true;
    }
    return false;
}
void ea_replay_status(char *text,size_t size)
{
    if(!r.initialized)return;size_t used=strlen(text);snprintf(text+used,size-used,"\n%s",r.status);EnterCriticalSection(&r.mutex);
    for(unsigned i=0;i<WATCHES;++i)if(r.watches[i].user) {used=strlen(text);snprintf(text+used,size-used,"\n");used=strlen(text);
        snprintf(text+used,size-used,r.watches[i].vertical?obs_module_text("Replay.RamVertical"):obs_module_text("Replay.RamMain"),ea_aac_replay_bytes(ea_lane_audio(r.watches[i].user))/1048576.0);
        if(atomic_load(&r.watches[i].failed)) {used=strlen(text);snprintf(text+used,size-used,"%s",obs_module_text("Replay.CaptureFailedSuffix"));}}
    for(unsigned i=0;i<CLIPS;++i)if(r.clips[i].used) {used=strlen(text);snprintf(text+used,size-used,"\n%s: %s",r.clips[i].vertical?obs_module_text("Clip.LabelVertical"):obs_module_text("Clip.LabelMain"),r.clips[i].state);}
    LeaveCriticalSection(&r.mutex);
}
