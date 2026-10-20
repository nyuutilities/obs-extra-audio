// SPDX-License-Identifier: GPL-2.0-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>
#include "recording-manager.h"
#include "routing.h"
#include "aac-recorder.h"
#include "mp3-backup.h"
#include "replay-manager.h"
#include "encoder-lane.h"
#include "native-pause-clock.h"
#include <libavutil/mathematics.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EA_JOBS 8u
#define EA_CLOCK_SEGMENTS 256u
struct clock_segment { int64_t start, origin; atomic_llong end; };
struct recording_job {
	obs_output_t *output;
	bool vertical, started, finalized, recovering, backup, backup_failed, aac_failed;
	unsigned embedded_count, native_frame_size;
	char backup_directory[8192];
	// The extra tracks come from encoders shared with other outputs.
	struct ea_lane_user *lane;
	atomic_bool stopping, failed;
	atomic_uint segment_count;
	CRITICAL_SECTION clock_mutex;
	bool paused;
	int64_t pause_start, pause_offset;
	struct clock_segment segments[EA_CLOCK_SEGMENTS];
	atomic_llong last_audio_ns, last_audio_frame;
	unsigned start_position, stop_position;
	unsigned checkpoint_segments;
	unsigned manifest_retries;
	// Wakes the writer for new audio, native packets, pauses and stop.
	HANDLE thread, process, wake;
	char native_path[8192], aac_path[8192], job_path[8192], final_path[8192];
	char state[512];
};
static struct ea_record_config config;
static struct ea_source_recorder *recorder;
static struct recording_job jobs[EA_JOBS];
static UINT_PTR timer;
static char helper[8192];
static char manager_state[1024] = "Extra source recording is disabled.";
// The last finished recording on each canvas (main, vertical), until that canvas records again.
static char finished[2][8800];
static bool reload_pending;
static uint64_t loaded_config_hash;
static bool aitum_supported, aitum_tested;
static char aitum_version[32];

// Vertical support relies on Aitum's output names and on its recording and Backtrack
// sharing audio encoders, as in 1.6.6. Older versions are skipped; newer ones are used
// as not yet tested, and vertical gets no extra tracks if their outputs aren't found.
static void check_aitum(void)
{
	aitum_supported = aitum_tested = false; aitum_version[0] = 0;
	obs_module_t *module = obs_get_module("vertical-canvas");
	const char *path = module ? obs_get_module_binary_path(module) : NULL;
	if (!path || !*path) return;
	wchar_t wide[8192]; DWORD handle = 0, size = 0;
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 8192)) size = GetFileVersionInfoSizeW(wide, &handle);
	void *info = size ? bmalloc(size) : NULL; VS_FIXEDFILEINFO *fixed = NULL; UINT length = 0;
	snprintf(aitum_version, sizeof(aitum_version), "unknown");
	if (info && GetFileVersionInfoW(wide, 0, size, info) && VerQueryValueW(info, L"\\", (void **)&fixed, &length) && fixed && length >= sizeof(*fixed)) {
		unsigned major = HIWORD(fixed->dwFileVersionMS), minor = LOWORD(fixed->dwFileVersionMS), patch = HIWORD(fixed->dwFileVersionLS);
		snprintf(aitum_version, sizeof(aitum_version), "%u.%u.%u", major, minor, patch);
		enum ea_support support = ea_version_support(MAKE_SEMANTIC_VERSION(major, minor, patch), EA_TESTED_AITUM);
		aitum_supported = support != EA_TOO_OLD; aitum_tested = support == EA_TESTED;
	}
	bfree(info);
	if (aitum_tested) blog(LOG_INFO, "[extra-audio] Aitum Vertical %s detected; vertical recordings and Backtrack clips are supported", aitum_version);
	else if (aitum_supported) blog(LOG_WARNING, "[extra-audio] Not yet tested with Aitum Vertical %s (tested with " EA_TESTED_AITUM_STRING "); vertical recordings and Backtrack clips get extra tracks", aitum_version);
	else blog(LOG_WARNING, "[extra-audio] Aitum Vertical %s isn't supported (needs " EA_TESTED_AITUM_STRING " or newer); vertical recordings and Backtrack clips get no extra tracks", aitum_version);
}

bool ea_aitum_supported(void) { return aitum_supported; }

// Source capture runs only while a recording or replay output needs it.
// STARTING hooks may run on other threads, so start and stop share a lock.
#define EA_PREPARE_NS 10000000000ULL
#define EA_IDLE_STOP_NS 5000000000ULL
static CRITICAL_SECTION capture_lock;
static bool capture_lock_ready;
static atomic_ullong prepare_until;
static uint64_t idle_since;

static void capture_lock_init(void)
{
	if (!capture_lock_ready) { InitializeCriticalSection(&capture_lock); atomic_init(&prepare_until, 0); capture_lock_ready = true; }
}

static bool capture_start(void)
{
	EnterCriticalSection(&capture_lock);
	bool ok = recorder && ea_source_recorder_start(recorder);
	if (ok) idle_since = 0;
	LeaveCriticalSection(&capture_lock);
	return ok;
}

// Called before an output starts (OBS frontend STARTING events) or as it
// starts (output "starting" signals), so capture covers the first sample.
void ea_record_manager_prepare(void)
{
	if (!capture_lock_ready || !config.enabled) return;
	atomic_store(&prepare_until, os_gettime_ns() + EA_PREPARE_NS);
	capture_start();
}

static void file_changed(void *data, calldata_t *params)
{
	(void)params;
	struct recording_job *job = data;
	atomic_store(&job->failed, true);
	blog(LOG_ERROR, "[extra-audio] File splitting needs a new capture timeline; preserving original files and recovery audio for %s", job->native_path);
}

static uint64_t profile_hash(void)
{
	config_t *p = obs_frontend_get_profile_config();
	if (!p) return 0;
	const char *keys[] = {"RecordEnabled", "RecordingTracks", "ControlBrowserAudio", "BackupMP3", "BackupFilenameFormat"};
	uint64_t hash = 1469598103934665603ULL;
	for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
		const char *value = config_get_string(p, "ExtraAudio", keys[i]);
		for (; value && *value; ++value) hash = (hash ^ (unsigned char)*value) * 1099511628211ULL;
	}
	return hash;
}

static FILE *open_utf8(const char *path, const wchar_t *mode)
{
	wchar_t wide[8192];
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 8192)) return NULL;
	return _wfopen(wide, mode);
}

static void packet_clock(obs_output_t *output, struct encoder_packet *packet, struct encoder_packet_time *time, void *data)
{
	(void)output; (void)time;
	struct recording_job *job = data;
	if (packet->type != OBS_ENCODER_AUDIO || packet->track_idx != 0 || !job->started) return;
	EnterCriticalSection(&job->clock_mutex);
	int64_t global = packet->sys_dts_usec * 1000;
	int64_t origin = global - packet->pts * 1000000000LL * packet->timebase_num / packet->timebase_den;
	unsigned count = atomic_load_explicit(&job->segment_count, memory_order_relaxed);
	if (!count) {
		job->segments[0] = (struct clock_segment){.start = global, .end = job->paused ? job->pause_start : INT64_MAX, .origin = origin};
		atomic_store_explicit(&job->segment_count, 1, memory_order_release);
	}
	atomic_store(&job->last_audio_frame, av_rescale_q(packet->pts, (AVRational){packet->timebase_num, packet->timebase_den}, (AVRational){1, (int)ea_source_recorder_rate(recorder)}) + job->native_frame_size);
	atomic_store(&job->last_audio_ns, global);
	LeaveCriticalSection(&job->clock_mutex);
	if (job->lane) ea_lane_packet(job->lane, packet);
	SetEvent(job->wake);
}

static void recording_paused(void *data, calldata_t *unused)
{
	(void)unused;
	struct recording_job *job = data;
	obs_encoder_t *encoder = obs_output_get_audio_encoder(job->output, 0);
	bool estimated;
	uint64_t start = ea_pause_start(encoder, &estimated);
	EnterCriticalSection(&job->clock_mutex);
	if (!start) {
		atomic_store(&job->failed, true);
		blog(LOG_ERROR, "[extra-audio] Recording paused, but its audio encoder isn't; original recording retained");
	} else if (!job->paused) {
		job->pause_start = (int64_t)start;
		job->pause_offset = (int64_t)obs_encoder_get_pause_offset(encoder);
		job->paused = true;
		unsigned count = atomic_load(&job->segment_count);
		if (count) atomic_store(&job->segments[count - 1].end, (int64_t)start);
		if (estimated) blog(LOG_WARNING, "[extra-audio] %s recording audio paused; OBS's exact pause position isn't readable here, so it is placed from the video clock", job->vertical ? "Aitum" : "OBS");
		else blog(LOG_INFO, "[extra-audio] %s recording audio paused at native sample boundary", job->vertical ? "Aitum" : "OBS");
	}
	LeaveCriticalSection(&job->clock_mutex);
	SetEvent(job->wake);
}

static void recording_resumed(void *data, calldata_t *unused)
{
	(void)unused;
	struct recording_job *job = data;
	obs_encoder_t *encoder = obs_output_get_audio_encoder(job->output, 0);
	EnterCriticalSection(&job->clock_mutex);
	if (job->paused) {
		int64_t gap = (int64_t)obs_encoder_get_pause_offset(encoder) - job->pause_offset;
		unsigned count = atomic_load(&job->segment_count);
		if (gap < 0 || !count || (gap && count >= EA_CLOCK_SEGMENTS)) {
			atomic_store(&job->failed, true);
			blog(LOG_ERROR, "[extra-audio] Native recording resume clock unavailable; original recording retained");
		} else if (gap) {
			job->segments[count] = (struct clock_segment){.start = job->pause_start + gap, .end = INT64_MAX,
				.origin = job->segments[count - 1].origin + gap};
			atomic_store_explicit(&job->segment_count, count + 1, memory_order_release);
		} else {
			atomic_store(&job->segments[count - 1].end, INT64_MAX);
		}
		job->paused = false;
		blog(LOG_INFO, "[extra-audio] %s recording audio resumed; skipped %.3f seconds", job->vertical ? "Aitum" : "OBS", gap / 1e9);
	}
	LeaveCriticalSection(&job->clock_mutex);
	SetEvent(job->wake);
}

static void disconnect_recording(struct recording_job *job)
{
	obs_output_remove_packet_callback(job->output, packet_clock, job);
	signal_handler_t *signals = obs_output_get_signal_handler(job->output);
	signal_handler_disconnect(signals, "file_changed", file_changed, job);
	signal_handler_disconnect(signals, "pause", recording_paused, job);
	signal_handler_disconnect(signals, "unpause", recording_resumed, job);
}

struct capture_writer {
    struct ea_mp3_recorder *mp3;
    struct recording_job *job;
    int64_t position;
};
// PCM feeds only the MP3 contingency; the extra AAC tracks come from the lane.
static bool write_audio(struct capture_writer *writer,const struct ea_stem_block *block,unsigned start,unsigned count)
{
    if(writer->mp3 && !ea_mp3_append(writer->mp3,block,start,count)) {
        ea_mp3_close(writer->mp3);writer->mp3=NULL;writer->job->backup_failed=true;
        blog(LOG_ERROR,"[extra-audio] MP3 contingency failed; main/vertical embedded recording continues.");
        if(!writer->job->lane || writer->job->aac_failed)return false;
    }
    writer->position+=count;return true;
}
static bool write_silence(struct capture_writer *writer,int64_t target)
{
    while(writer->position<target) {
        unsigned count=target-writer->position>AUDIO_OUTPUT_FRAMES?AUDIO_OUTPUT_FRAMES:(unsigned)(target-writer->position);
        if(!write_audio(writer,NULL,0,count))return false;
    }
    return true;
}
static void aac_failed(struct recording_job *job,struct ea_aac_tee **tee)
{
    ea_aac_tee_finish(*tee,0);*tee=NULL;job->aac_failed=true;
    blog(LOG_ERROR,"[extra-audio] Extra AAC capture failed; MP3 contingency continues if enabled.");
}
// Opens the extra-track file once the shared encoders have placed this output.
static struct ea_aac_tee *open_tee(struct recording_job *job,int64_t *offset)
{
    int64_t base;
    if(!ea_lane_offset(job->lane,offset,&base,NULL))return NULL;
    // An output that began before the encoders' current run maps its first
    // samples onto that run's leading silence, or before it.
    if(*offset<base && (base<EA_LANE_FLUSH_FRAMES || base-*offset>EA_LANE_FLUSH_FRAMES)) {
        blog(LOG_ERROR,"[extra-audio] Recording began %.1f ms before its shared extra-track encoders; original recording retained: %s",
            (base-*offset)*1000.0/ea_source_recorder_rate(recorder),job->native_path);
        job->aac_failed=true;return NULL;
    }
    struct ea_aac_tee *tee=ea_aac_tee_open(ea_lane_audio(job->lane),job->aac_path,job->vertical,*offset);
    if(!tee) {job->aac_failed=true;blog(LOG_ERROR,"[extra-audio] Could not open the extra-track file: %s",job->aac_path);}
    return tee;
}

static DWORD WINAPI write_stems(void *data)
{
    struct recording_job *job=data;
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    FILE *existing=open_utf8(job->aac_path,L"rb");
    if(existing) { fclose(existing); atomic_store(&job->failed,true); return 1; }
    unsigned rate=ea_source_recorder_rate(recorder);
    struct capture_writer state={.job=job}, *writer=&state;
    if(job->backup) {
        writer->mp3=ea_mp3_open(job->backup_directory,config.backup_format,job->native_path,rate,config.tracks,config.track_count);
        if(!writer->mp3) { job->backup_failed=true;blog(LOG_ERROR,"[extra-audio] Could not start MP3 contingency files; original recording continues."); }
    }
    if(job->embedded_count && !job->lane)job->aac_failed=true;
    if(job->aac_failed && !writer->mp3) { atomic_store(&job->failed,true);return 1; }
    struct ea_aac_tee *tee=NULL;
    int64_t offset=0;
    bool late_start_logged=false,subscribed=false;
    struct ea_stem_block *block=bmalloc(sizeof(*block));
    unsigned position=job->start_position;
    uint64_t overruns=ea_source_recorder_overruns(recorder);
    // Wait on events, not timers: the 50 ms timeout is only a safety net.
    if(writer->mp3)subscribed=ea_source_recorder_subscribe(recorder,job->wake);
    while(!atomic_load(&job->failed)) {
        bool stopping=atomic_load_explicit(&job->stopping,memory_order_acquire);
        if(job->lane && !job->aac_failed) {
            if(!tee)tee=open_tee(job,&offset);
            // OBS can drop the last packets it delivered when the recording
            // stops; write only audio a second older until the MP4 confirms its end.
            if(tee && !ea_aac_tee_pump(tee,atomic_load(&job->last_audio_frame)-rate))aac_failed(job,&tee);
            int64_t first_audio,ignored;
            // Capture that started after the native audio leaves the start silent.
            if(tee && !late_start_logged && ea_lane_offset(job->lane,&ignored,&ignored,&first_audio) && first_audio!=INT64_MAX) {
                late_start_logged=true;
                if(first_audio!=INT64_MIN && first_audio>offset+2)
                    blog(LOG_WARNING,"[extra-audio] Extra audio began %.1f ms after the native audio; that start is silent: %s",(first_audio-offset)*1000.0/rate,job->native_path);
            }
            if(job->aac_failed && !writer->mp3) { atomic_store(&job->failed,true); break; }
        }
        if(!writer->mp3) { if(stopping) break; WaitForSingleObject(job->wake,50); continue; }
        unsigned end=stopping?job->stop_position:ea_source_recorder_write_pos(recorder);
        if(end-position>=EA_HISTORY || ea_source_recorder_overruns(recorder)!=overruns) {
            blog(LOG_ERROR,"[extra-audio] Recording capture history overrun: queued=%u, %s",end-position,job->native_path);
            atomic_store(&job->failed,true); break;
        }
        if(position==end) { if(stopping) break; WaitForSingleObject(job->wake,50); continue; }
        struct {int64_t start,end,origin;} spans[EA_CLOCK_SEGMENTS];
        EnterCriticalSection(&job->clock_mutex);
        unsigned segments=atomic_load_explicit(&job->segment_count,memory_order_acquire);
        bool paused=job->paused;
        int64_t safe_end=paused?job->pause_start:atomic_load(&job->last_audio_ns)+(int64_t)audio_frames_to_ns(rate,job->native_frame_size);
        for(unsigned n=0;n<segments;++n) {
            spans[n].start=job->segments[n].start;spans[n].end=atomic_load(&job->segments[n].end);spans[n].origin=job->segments[n].origin;
        }
        LeaveCriticalSection(&job->clock_mutex);
        if(!segments) { if(stopping) { atomic_store(&job->failed,true); break; } WaitForSingleObject(job->wake,50); continue; }
        // A read fails only while the audio thread is writing that slot.
        if(!ea_source_recorder_read(recorder,position,block)) { WaitForSingleObject(job->wake,1); continue; }
        // Encoded packets can lag the exact pause boundary by several frames.
        // Preserve input up to that boundary before discarding paused time.
        if(!stopping && !paused && (int64_t)block->timestamp+(int64_t)audio_frames_to_ns(rate,AUDIO_OUTPUT_FRAMES)>safe_end) { WaitForSingleObject(job->wake,50); continue; }
        for(unsigned n=0;n<segments;++n) {
            int64_t low=n?spans[n].start:INT64_MIN;
            int64_t high=spans[n].end; if(high>safe_end) high=safe_end;
            int64_t start=0,finish=AUDIO_OUTPUT_FRAMES;
            if((int64_t)block->timestamp<low) start=av_rescale_rnd(low-(int64_t)block->timestamp,rate,1000000000LL,AV_ROUND_UP);
            if(high!=INT64_MAX) finish=av_rescale_rnd(high-(int64_t)block->timestamp,rate,1000000000LL,AV_ROUND_DOWN);
            if(finish>AUDIO_OUTPUT_FRAMES) finish=AUDIO_OUTPUT_FRAMES;
            if(start<0) start=0;
            if(finish<=start) continue;
            int64_t target=av_rescale_rnd((int64_t)block->timestamp-spans[n].origin,rate,1000000000LL,AV_ROUND_NEAR_INF)+start;
            int64_t current=writer->position;
            if(target+finish-start<=current) continue;
            if(!n && !current && target>0 && !late_start_logged && !job->lane) {
                late_start_logged=true;
                blog(LOG_WARNING,"[extra-audio] Extra audio began %.1f ms after the native audio; that start is silent: %s",target*1000.0/rate,job->native_path);
            }
            if(target>current && !write_silence(writer,target)) { atomic_store(&job->failed,true); break; }
            current=writer->position;
            unsigned skip=target<current?(unsigned)(current-target):0;
            if(!write_audio(writer,block,(unsigned)start+skip,(unsigned)(finish-start)-skip)) { atomic_store(&job->failed,true); break; }
        }
        ++position;
    }
    if(subscribed)ea_source_recorder_unsubscribe(recorder,job->wake);
    int64_t final_frames=writer->position;
    if(!atomic_load(&job->failed)) {
        bool known=false;
        // OBS gives no signal when the native MP4 index is complete; retry briefly.
        for(unsigned attempt=0;attempt<100 && !known;++attempt) {
            known=ea_recording_audio_frames(job->native_path,rate,&final_frames);if(!known)Sleep(10);
        }
        if(!known || (writer->mp3 && !write_silence(writer,final_frames)))atomic_store(&job->failed,true);
        else blog(LOG_INFO,"[extra-audio] Native recording timeline: %lld samples at %u Hz; %s",(long long)final_frames,rate,job->native_path);
    }
    if(tee) {
        bool complete=false;
        if(!atomic_load(&job->failed)) {
            // The shared encoders finish this output's audio while other
            // outputs keep them running, or as they flush after the last stops.
            struct ea_aac_recorder *audio=ea_lane_audio(job->lane);DWORD began=GetTickCount();
            while(ea_aac_replay_ready(audio)<offset+final_frames && !ea_lane_failed(job->lane) && GetTickCount()-began<5000)ea_aac_wait_progress(audio,50);
            complete=ea_aac_replay_ready(audio)>=offset+final_frames;
            if(!complete)blog(LOG_ERROR,"[extra-audio] Shared extra-track encoders did not finish this recording's timeline: %s",job->native_path);
        }
        if(!ea_aac_tee_finish(tee,complete?final_frames:0) || !complete)job->aac_failed=true;
    } else if(job->embedded_count) job->aac_failed=true;
    if(job->aac_failed)atomic_store(&job->failed,true);
    if(writer->mp3 && !ea_mp3_finish(writer->mp3,final_frames))job->backup_failed=true;
    bfree(block); return atomic_load(&job->failed)?1:0;
}

static bool save_manifest(struct recording_job *job)
{
	unsigned segments = atomic_load_explicit(&job->segment_count, memory_order_acquire);
	if (!segments) return false;
	obs_data_t *data = obs_data_create();
	bool capture_failed = atomic_load_explicit(&job->failed, memory_order_acquire);
	// Exactly what the MP4 helper reads; it is saved when the recording starts, for crash recovery, and at Stop.
	obs_data_set_bool(data, "capture_failed", capture_failed);
	obs_data_set_string(data, "native_path", job->native_path); obs_data_set_string(data, "audio_path", job->aac_path);
	bool saved = obs_data_save_json_safe(data, job->job_path, "tmp", NULL); obs_data_release(data);
	if (saved) job->checkpoint_segments = segments;
	return saved;
}

static void launch_finalizer(struct recording_job *job)
{
    if(!job->embedded_count) {
        job->finalized=true;snprintf(job->state,sizeof(job->state),job->backup_failed || atomic_load(&job->failed)?
            obs_module_text("Job.BackupFailed"):obs_module_text("Job.BackupOnly"));
        wchar_t path[8192];MultiByteToWideChar(CP_UTF8,0,job->job_path,-1,path,8192);DeleteFileW(path);return;
    }
	if (!job->recovering && !save_manifest(job)) {
		++job->manifest_retries;
		if(job->manifest_retries<100) {
			if(job->manifest_retries==1) blog(LOG_WARNING,"[extra-audio] Job checkpoint save retry: %s",job->job_path);
			snprintf(job->state,sizeof(job->state),obs_module_text("Job.SavingCheckpoint"));
			return;
		}
		blog(LOG_ERROR,"[extra-audio] Could not save recording job after retries: %s",job->job_path);
		snprintf(job->state,sizeof(job->state),obs_module_text("Job.CheckpointFailed")); job->finalized=true; return;
	}
	if (!job->recovering && atomic_load(&job->failed)) {
		snprintf(job->state, sizeof(job->state), job->backup && !job->backup_failed ? obs_module_text("Job.AacFailedBackupKept") : obs_module_text("Job.CaptureFailed"));
		job->finalized = true; return;
	}
	wchar_t exe[8192], command[17000], directory[8192];
	MultiByteToWideChar(CP_UTF8, 0, helper, -1, exe, 8192);
	char args[17000]; snprintf(args, sizeof(args), "\"%s\" %s\"%s\"", helper, job->recovering ? "--recover " : "", job->job_path);
	MultiByteToWideChar(CP_UTF8, 0, args, -1, command, 17000);
	wcscpy(directory, exe); wchar_t *slash = wcsrchr(directory, L'\\'), *forward = wcsrchr(directory, L'/');
	if (forward && (!slash || forward > slash)) slash = forward; if (slash) *slash = 0;
	STARTUPINFOW si = {.cb = sizeof(si), .dwFlags = STARTF_USESHOWWINDOW, .wShowWindow = SW_HIDE};
	PROCESS_INFORMATION pi = {0};
	if (CreateProcessW(exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL, directory, &si, &pi)) {
		CloseHandle(pi.hThread); job->process = pi.hProcess;
		snprintf(job->state, sizeof(job->state), obs_module_text("Job.Embedding"));
		blog(LOG_INFO, "[extra-audio] Finalizing %s", job->final_path);
	} else { snprintf(job->state, sizeof(job->state), obs_module_text("Job.LaunchFailed"), GetLastError()); job->finalized = true; }
}

#define EA_CANDIDATES 8u
struct candidates { obs_output_t *outputs[EA_CANDIDATES]; unsigned count; };

// Only collect here: starting capture inside obs_enum_outputs would hold
// OBS's output list lock while adding a view and audio connections.
static bool find_recordings(void *data, obs_output_t *output)
{
	struct candidates *found = data;
	const char *name = obs_output_get_name(output), *id = obs_output_get_id(output);
	bool vertical = strncmp(name, "vertical_canvas_record", 22) == 0;
	if ((!vertical && strcmp(name, "adv_file_output") != 0 && strcmp(name, "simple_file_output") != 0) ||
		(strcmp(id, "mp4_output") != 0 && strcmp(id, "ffmpeg_muxer") != 0)) return true;
	if (vertical && !aitum_supported) return true;
	if (!config.enabled || !obs_output_active(output)) return true;
	if (found->count < EA_CANDIDATES) found->outputs[found->count++] = obs_output_get_ref(output);
	return true;
}

static void start_job(obs_output_t *output)
{
	bool vertical = strncmp(obs_output_get_name(output), "vertical_canvas_record", 22) == 0;
	unsigned selected = 0;
	for (unsigned i = 0; i < config.track_count; ++i) selected += vertical ? config.tracks[i].vertical : config.tracks[i].main;
	// Each output owns a different start, pause and stop clock. Its existing
	// capture worker also writes its MP3 set from the same timestamped PCM.
	bool backup=config.backup_mp3;
    if (!selected && !backup) return;
	obs_data_t *settings = obs_output_get_settings(output);
	const char *path = obs_data_get_string(settings, "path");
	if (!path || !*path || strlen(path) > 7800) { obs_data_release(settings); return; }
	for (unsigned i = 0; i < EA_JOBS; ++i)
		if (jobs[i].output == output && strcmp(jobs[i].native_path, path) == 0) { obs_data_release(settings); return; }
	struct recording_job *job = NULL;
	for (unsigned i = 0; i < EA_JOBS; ++i) if (!jobs[i].output) { job = &jobs[i]; break; }
	if (!job) { obs_data_release(settings); snprintf(manager_state, sizeof(manager_state), obs_module_text("Status.TooManyJobs")); return; }
	memset(job, 0, sizeof(*job));
	InitializeCriticalSection(&job->clock_mutex);
	job->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
	job->output = obs_output_get_ref(output); job->vertical = vertical; job->started = true;
	finished[vertical][0] = 0;
    job->backup=backup;job->embedded_count=selected;
    obs_encoder_t *clock_encoder=obs_output_get_audio_encoder(output,0);
    job->native_frame_size=clock_encoder?(unsigned)obs_encoder_get_frame_size(clock_encoder):1024;
    if(!job->native_frame_size)job->native_frame_size=1024;
    config_t *profile=obs_frontend_get_profile_config();
    const char *directory=config_get_string(profile,"AdvOut","RecFilePath");
    if(directory && *directory)snprintf(job->backup_directory,sizeof(job->backup_directory),"%s/Extra Audio/%s",directory,vertical?"Vertical":"Main");
	atomic_init(&job->failed, false); atomic_init(&job->stopping, false); atomic_init(&job->segment_count, 0); atomic_init(&job->last_audio_ns, 0); atomic_init(&job->last_audio_frame,0);
	snprintf(job->native_path, sizeof(job->native_path), "%s", path);
	snprintf(job->aac_path, sizeof(job->aac_path), "%s.extra-audio.m4a", path);
	snprintf(job->job_path, sizeof(job->job_path), "%s.extra-audio-job.json", path);
	snprintf(job->final_path, sizeof(job->final_path), "%s", path);
	obs_data_release(settings);
	// Normally already running from a STARTING event; otherwise the start is late.
	if (!job->wake || !capture_start()) {
		atomic_store(&job->failed, true);
		blog(LOG_ERROR, "[extra-audio] Could not start source capture; original recording retained: %s", job->native_path);
	}
	unsigned published = ea_source_recorder_write_pos(recorder), first = ea_source_recorder_first_pos(recorder);
	job->start_position = published - first > 64 ? published - 64 : first;
	// Encoders shared with this output's Replay Buffer or Backtrack, or kept
	// running from an earlier recording.
	if (selected && !atomic_load(&job->failed)) {
		job->lane = ea_lane_join(output, vertical, 0);
		if (!job->lane) { job->aac_failed = true; blog(LOG_ERROR, "[extra-audio] No extra-track encoders for %s", job->native_path); }
	}
	obs_output_add_packet_callback(output, packet_clock, job);
	signal_handler_connect(obs_output_get_signal_handler(output), "file_changed", file_changed, job);
	signal_handler_connect(obs_output_get_signal_handler(output), "pause", recording_paused, job);
	signal_handler_connect(obs_output_get_signal_handler(output), "unpause", recording_resumed, job);
	if (obs_output_paused(output)) recording_paused(job, NULL);
	job->thread = CreateThread(NULL, 0, write_stems, job, 0, NULL);
	if (!job->thread) atomic_store(&job->failed, true);
	snprintf(job->state, sizeof(job->state), obs_module_text("Job.Capturing"), config.track_count);
	blog(LOG_INFO, "[extra-audio] Recording extension started: %s; extra_tracks=%u", job->native_path, config.track_count);
}

static void release_job(struct recording_job *job)
{
	obs_output_release(job->output); ea_lane_leave(job->lane);
	if (job->wake) CloseHandle(job->wake);
	DeleteCriticalSection(&job->clock_mutex); memset(job, 0, sizeof(*job));
}

static bool capture_needed(void)
{
	for (unsigned i = 0; i < EA_JOBS; ++i) if (jobs[i].output && (jobs[i].thread || !atomic_load(&jobs[i].stopping))) return true;
	return ea_replay_needs_capture() || ea_lanes_need_capture() || os_gettime_ns() < atomic_load(&prepare_until);
}

// Stop capturing and free its history once nothing has needed it for a while.
static void stop_idle_capture(void)
{
	EnterCriticalSection(&capture_lock);
	if (ea_source_recorder_capturing(recorder)) {
		uint64_t now = os_gettime_ns();
		if (capture_needed()) idle_since = 0;
		else if (!idle_since) idle_since = now;
		else if (now - idle_since > EA_IDLE_STOP_NS) { ea_source_recorder_stop(recorder); idle_since = 0; }
	}
	LeaveCriticalSection(&capture_lock);
}

static VOID CALLBACK tick(HWND window, UINT message, UINT_PTR id, DWORD time)
{
	(void)window; (void)message; (void)id; (void)time;
	if (profile_hash() != loaded_config_hash && !ea_outputs_busy()) ea_record_manager_reload();
	if (!recorder) return;
	// A source deleted in OBS stays alive while referenced; let it close now.
	ea_source_recorder_release_sources(recorder, false);
	ea_replay_tick();
	for (unsigned i = 0; i < EA_JOBS; ++i) {
		struct recording_job *job = &jobs[i];
		if (!job->output || !job->finalized) continue;
		if (!obs_output_active(job->output)) {
			// Outputs can be recreated between sessions. Reclaim completed slots
			// outside obs_enum_outputs, whose mutex also protects output removal.
			snprintf(finished[job->vertical], sizeof(finished[0]), "%s\n%s", job->state, job->final_path);
			release_job(job);
		} else {
			obs_data_t *s = obs_output_get_settings(job->output);
			bool changed = strcmp(obs_data_get_string(s, "path"), job->native_path) != 0; obs_data_release(s);
			if (changed) release_job(job);
		}
	}
	struct candidates found = {0};
	obs_enum_outputs(find_recordings, &found);
	for (unsigned i = 0; i < found.count; ++i) { start_job(found.outputs[i]); obs_output_release(found.outputs[i]); }
	for (unsigned i = 0; i < EA_JOBS; ++i) {
		struct recording_job *job = &jobs[i];
		if (!job->output) continue;
		unsigned segments = atomic_load_explicit(&job->segment_count, memory_order_acquire);
		if (segments && !job->checkpoint_segments) save_manifest(job);
		obs_data_t *s = obs_output_get_settings(job->output);
		bool changed = strcmp(obs_data_get_string(s, "path"), job->native_path) != 0; obs_data_release(s);
		if (!atomic_load(&job->stopping) && (!obs_output_active(job->output) || changed)) {
			job->stop_position = ea_source_recorder_write_pos(recorder);
			atomic_store_explicit(&job->stopping, true, memory_order_release);
			disconnect_recording(job);
			SetEvent(job->wake);
		}
		if (atomic_load(&job->stopping) && job->thread && WaitForSingleObject(job->thread, 0) == WAIT_OBJECT_0) {
			CloseHandle(job->thread); job->thread = NULL;
		}
		if(atomic_load(&job->stopping) && !job->thread && !job->process && !job->finalized) launch_finalizer(job);
		if (job->process && WaitForSingleObject(job->process, 0) == WAIT_OBJECT_0) {
			DWORD code; GetExitCodeProcess(job->process, &code); CloseHandle(job->process); job->process = NULL;
			if(code && !job->recovering) {
				job->recovering=true; launch_finalizer(job);
				if(job->process) continue;
			}
			job->finalized = true;
			snprintf(job->state, sizeof(job->state), !job->recovering && code == 0 ? obs_module_text("Job.Complete") :
				job->recovering && code == 0 ? obs_module_text("Job.Restored") :
				obs_module_text("Job.RecoveryFailed"));
			if(job->backup_failed)strncat(job->state,obs_module_text("Job.BackupFailedSuffix"),sizeof(job->state)-strlen(job->state)-1);
			blog(code == 0 ? LOG_INFO : LOG_ERROR, "[extra-audio] Finalizer exited %lu for %s", code, job->final_path);
			if (code == 0 && !job->recovering) {
				wchar_t path[8192];
				MultiByteToWideChar(CP_UTF8, 0, job->aac_path, -1, path, 8192); DeleteFileW(path);
				MultiByteToWideChar(CP_UTF8, 0, job->job_path, -1, path, 8192); DeleteFileW(path);
			}
		}
	}
	ea_lanes_tick();
	stop_idle_capture();
	if (reload_pending) ea_record_manager_reload();
}

void ea_record_manager_reload(void)
{
	reload_pending = true;
	if (ea_outputs_busy()) return;
	for (unsigned i = 0; i < EA_JOBS; ++i) if (jobs[i].output && !jobs[i].finalized) return;
	reload_pending = false;
	capture_lock_init();
	ea_replay_close();
	for (unsigned i = 0; i < EA_JOBS; ++i) {
		if (jobs[i].output) release_job(&jobs[i]);
		else memset(&jobs[i], 0, sizeof(jobs[i]));
	}
	ea_lanes_close();
	EnterCriticalSection(&capture_lock);
	if (recorder) { ea_source_recorder_close(recorder); recorder = NULL; }
	LeaveCriticalSection(&capture_lock);
	ea_record_config_load(obs_frontend_get_profile_config(), &config);
	loaded_config_hash = profile_hash();
	char error[512];
	if (!ea_record_config_valid(&config, error, sizeof(error))) { snprintf(manager_state, sizeof(manager_state), "%s", error); return; }
	ea_record_config_active(&config);
	blog(LOG_INFO, "[extra-audio] Recorder configuration: enabled=%d sources=%u extra_tracks=%u", config.enabled, config.count, config.track_count);
	if (!config.track_count) { snprintf(manager_state, sizeof(manager_state), obs_module_text("Status.NoTracks")); return; }
	if (!config.enabled) { snprintf(manager_state, sizeof(manager_state), obs_module_text("Status.Disabled")); return; }
	EnterCriticalSection(&capture_lock);
	recorder = ea_source_recorder_open_tracks(config.stems, config.count, config.tracks, config.track_count);
	LeaveCriticalSection(&capture_lock);
	if(recorder) { ea_lanes_configure(&config, recorder); ea_replay_configure(&config,recorder,helper); }
	snprintf(manager_state, sizeof(manager_state), recorder ? obs_module_text("Status.Ready") :
		obs_module_text("Status.CaptureFailed"), config.track_count);
	unsigned missing = recorder ? ea_source_recorder_missing(recorder) : 0;
	if (missing) {
		size_t used = strlen(manager_state);
		snprintf(manager_state + used, sizeof(manager_state) - used, "\n");
		used = strlen(manager_state);
		snprintf(manager_state + used, sizeof(manager_state) - used, missing == 1 ? obs_module_text("Status.MissingOne") :
			obs_module_text("Status.MissingMany"), missing);
	}
	blog(recorder ? LOG_INFO : LOG_ERROR, "[extra-audio] %s", manager_state);
}

void ea_record_manager_release_sources(void) { ea_source_recorder_release_sources(recorder, true); }

void ea_record_manager_init(const char *binary)
{
	if (timer) KillTimer(NULL, timer);
	capture_lock_init();
	check_aitum();
	snprintf(helper, sizeof(helper), "%s", binary);
	for (unsigned i = 0; i < 3; ++i) {
		char *a = strrchr(helper, '/'), *b = strrchr(helper, '\\');
		char *slash = a && (!b || a > b) ? a : b;
		if (!slash) { helper[0] = 0; break; } *slash = 0;
	}
	strncat(helper, "/bin/64bit/obs-extra-audio-mux.exe", sizeof(helper) - strlen(helper) - 1);
	char absolute[8192];
	if (GetFullPathNameA(helper, sizeof(absolute), absolute, NULL)) snprintf(helper, sizeof(helper), "%s", absolute);
	timer = SetTimer(NULL, 0, 100, tick);
	blog(LOG_INFO, "[extra-audio] Recorder manager initialized; helper=%s", helper);
	ea_record_manager_reload();
}

void ea_record_manager_close(void)
{
	if (timer) { KillTimer(NULL, timer); timer = 0; }
	// Pending recordings and clips finish from encoders that flush now.
	ea_lanes_end_outputs();
	ea_replay_close();
	for (unsigned i = 0; i < EA_JOBS; ++i) {
		struct recording_job *job = &jobs[i];
		if (!job->output) continue;
		disconnect_recording(job);
		job->stop_position = recorder ? ea_source_recorder_write_pos(recorder) : 0;
		atomic_store(&job->stopping, true);
		SetEvent(job->wake);
		if (job->thread) { WaitForSingleObject(job->thread, INFINITE); CloseHandle(job->thread); job->thread = NULL; }
		if (!job->finalized && !job->process) launch_finalizer(job);
		if (job->process) CloseHandle(job->process); // The child safely finishes after OBS exits.
		release_job(job);
	}
	ea_lanes_close();
	if (!capture_lock_ready) return;
	EnterCriticalSection(&capture_lock);
	if (recorder) { ea_source_recorder_close(recorder); recorder = NULL; }
	LeaveCriticalSection(&capture_lock);
}

void ea_record_manager_status(char *text, size_t size)
{
	snprintf(text, size, "%s", manager_state);
	if (recorder && !ea_source_recorder_capturing(recorder)) {
		size_t used = strlen(text);
		snprintf(text + used, size - used, "\n%s", obs_module_text("Status.Idle"));
	}
	if (*aitum_version && !aitum_tested) {
		size_t used = strlen(text);
		snprintf(text + used, size - used, "\n");
		used = strlen(text);
		const char *format = aitum_supported ? obs_module_text("Status.AitumUntested") : obs_module_text("Status.AitumUnsupported");
		snprintf(text + used, size - used, format, aitum_version, EA_TESTED_AITUM_STRING);
	}
	if (ea_version_support(obs_get_version(), EA_TESTED_OBS) == EA_UNTESTED) {
		size_t used = strlen(text);
		snprintf(text + used, size - used, "\n");
		used = strlen(text);
		snprintf(text + used, size - used, obs_module_text("Status.ObsUntested"), obs_get_version_string(), EA_TESTED_OBS_STRING);
	}
	if (ea_pause_estimated()) {
		size_t used = strlen(text);
		snprintf(text + used, size - used, "\n%s", obs_module_text("Status.PauseEstimated"));
	}
	ea_replay_status(text,size);
	uint32_t used = 0;
	for (unsigned i = 0; i < config.track_count; ++i) used |= config.tracks[i].sources;
	for (unsigned i = 0; i < config.count; ++i) {
		if (!(used & (1u << i))) continue;
		obs_source_t *source = obs_get_source_by_uuid(config.stems[i].uuid);
		if (source && strcmp(obs_source_get_unversioned_id(source), "browser_source") == 0) {
			obs_data_t *settings = obs_source_get_settings(source);
			if (!obs_data_get_bool(settings, "reroute_audio")) {
				size_t used = strlen(text);
				snprintf(text + used, size - used, "\n");
					used = strlen(text);
					snprintf(text + used, size - used, obs_module_text("Status.BrowserAudioOff"), config.stems[i].name);
			}
			obs_data_release(settings);
		}
		obs_source_release(source);
	}
	for (unsigned v = 0; v < 2; ++v) if (*finished[v]) {
		size_t used = strlen(text);
		snprintf(text + used, size - used, "\n\n%s: %s", v ? obs_module_text("Status.JobVertical") : obs_module_text("Status.JobMain"), finished[v]);
	}
	for (unsigned i = 0; i < EA_JOBS; ++i) if (jobs[i].output) {
		size_t used = strlen(text);
		snprintf(text + used, size - used, "\n\n%s: %s\n%s", jobs[i].vertical ? obs_module_text("Status.JobVertical") : obs_module_text("Status.JobMain"), jobs[i].state, jobs[i].final_path);
	}
}
