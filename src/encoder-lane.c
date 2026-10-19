// SPDX-License-Identifier: GPL-2.0-or-later
// One set of extra-track encoders per native audio encoder. A recording and
// the Replay Buffer that shares its encoders (or Aitum's vertical recording
// and Backtrack) take their extra tracks from the same set, and the set stays
// running for later outputs: OBS rebuilds an encoder's context on every
// start, and CoreAudio keeps about 0.5 MB each time it does.
//
// A lane's encoders see one continuous timeline. While any of its outputs is
// active, captured audio is placed on it by OBS's clock, skipping paused time
// exactly as the native encoder does; each output knows the lane frame of its
// own sample 0. When every output has stopped, the lane encodes up to the last
// output's end, then silence that flushes the encoder delay and separates this
// run from the next.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <libavutil/mathematics.h>
#include "encoder-lane.h"
#include "aac-recorder.h"
#include "native-pause-clock.h"
#include <string.h>

#define LANES 4u
#define USERS 8u
#define SPANS 256u
// Outputs that start together are placed together: wait briefly after a join,
// and up to half a second for an active output's first packet.
#define SETTLE_NS 30000000ULL
#define FIRST_PACKET_NS 500000000ULL
#define FLUSH_FRAMES EA_LANE_FLUSH_FRAMES
// A run starts this far before its earliest output's sample 0. Native AAC
// packets begin with encoder priming before sample 0 (2112 samples for
// CoreAudio, 1024 for FFmpeg AAC), and a replay saved soon after it starts
// still holds that first packet; its clip needs audio from there.
#define LEAD_FRAMES (4 * AUDIO_OUTPUT_FRAMES)
#define MIN_RETENTION 10u

struct span { int64_t start, end, origin; };
struct ea_lane;
struct ea_lane_user {
	struct ea_lane *lane;
	obs_output_t *output;
	bool used, frame0_known;
	int64_t frame0; // OBS clock time of the output's sample 0, in the current pause span.
	int64_t offset, base;
	unsigned offset_epoch, retention; // Epoch 0: offset unknown.
};
struct ea_lane {
	bool used, vertical, queue_held;
	bool calibrated; // The encoders' delay is measured before any output runs.
	char encoder_id[128];
	char *settings;
	struct ea_aac_recorder *audio;
	obs_encoder_t *clock; // Compared, never dereferenced after its outputs stop.
	int64_t frame_ns;
	struct ea_lane_user users[USERS];
	unsigned user_count;
	uint64_t joined;
	CRITICAL_SECTION mutex; // Guards the clock and users' clock fields.
	struct span spans[SPANS];
	unsigned span_count, epoch;
	bool anchored, paused, closing, closing_forced;
	int64_t pause_start, pause_offset, last_ns, closing_end;
	int64_t base, first_audio; // base is written by the thread; the next run starts there.
	atomic_llong position;
	HANDLE thread, wake;
	atomic_bool stopping, failed;
};
static struct {
	bool ready, mutex_ready;
	CRITICAL_SECTION mutex; // Guards the lane table and users joining or leaving.
	struct ea_record_config config;
	struct ea_source_recorder *source;
	unsigned rate;
	struct ea_lane lanes[LANES];
} L;

static const char *side(const struct ea_lane *lane) { return lane->vertical ? "Vertical" : "Main"; }

// Lane mutex held.
static bool any_active(const struct ea_lane *lane)
{
	for (unsigned i = 0; i < USERS; ++i)
		if (lane->users[i].used && obs_output_active(lane->users[i].output)) return true;
	return false;
}

// Lane mutex held. The run's first frame comes LEAD_FRAMES before the
// earliest active output's sample 0.
static void try_anchor(struct ea_lane *lane)
{
	if (lane->anchored || lane->closing_forced || !lane->calibrated) return;
	int64_t first = INT64_MAX;
	bool waiting = false;
	for (unsigned i = 0; i < USERS; ++i) {
		const struct ea_lane_user *u = &lane->users[i];
		if (!u->used || !obs_output_active(u->output)) continue;
		if (!u->frame0_known) waiting = true;
		else if (u->frame0 < first) first = u->frame0;
	}
	uint64_t age = os_gettime_ns() - lane->joined;
	if (first == INT64_MAX || age < SETTLE_NS || (waiting && age < FIRST_PACKET_NS)) return;
	first -= av_rescale(LEAD_FRAMES, 1000000000, L.rate);
	lane->spans[0] = (struct span){.start = first, .end = lane->paused ? lane->pause_start : INT64_MAX,
		.origin = first - av_rescale(lane->base, 1000000000, L.rate)};
	lane->span_count = 1; lane->anchored = true; lane->closing = false; ++lane->epoch; lane->first_audio = INT64_MAX;
	for (unsigned i = 0; i < USERS; ++i) {
		struct ea_lane_user *u = &lane->users[i];
		if (!u->used || !u->frame0_known || !obs_output_active(u->output)) continue;
		u->offset = lane->base + av_rescale_rnd(u->frame0 - first, L.rate, 1000000000, AV_ROUND_NEAR_INF);
		u->base = lane->base; u->offset_epoch = lane->epoch;
	}
}

// The first source block that ends after `time`, at most 64 blocks back.
static unsigned cursor_at(int64_t time, struct ea_stem_block *block, int64_t block_ns)
{
	unsigned end = ea_source_recorder_write_pos(L.source), first = ea_source_recorder_first_pos(L.source), position = end;
	while (position != first && end - position < 64 && ea_source_recorder_read(L.source, position - 1, block) &&
		(int64_t)block->timestamp + block_ns > time)
		--position;
	return position;
}

static DWORD WINAPI lane_thread(void *data)
{
	struct ea_lane *lane = data;
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	unsigned rate = L.rate, epoch = 0, cursor = 0;
	const int64_t block_ns = (int64_t)audio_frames_to_ns(rate, AUDIO_OUTPUT_FRAMES);
	int64_t written_until = INT64_MIN;
	uint64_t overruns = ea_source_recorder_overruns(L.source);
	struct ea_stem_block *block = bmalloc(sizeof(*block));
	if (!lane->calibrated) {
		// New encoders: measure their delay first. Outputs that start meanwhile
		// are placed once it is known; the source history covers the wait.
		int64_t end = 0;
		bool ok = ea_aac_calibrate(lane->audio, &end);
		EnterCriticalSection(&lane->mutex);
		if (ok) { lane->base = end; lane->calibrated = true; }
		LeaveCriticalSection(&lane->mutex);
		atomic_store(&lane->position, ea_aac_position(lane->audio));
		if (!ok) {
			blog(LOG_ERROR, "[extra-audio] %s extra-track encoder delay could not be measured; outputs keep their native tracks", side(lane));
			atomic_store(&lane->failed, true);
		}
	}
	// Woken by new audio, packets, pauses and leaving users; the timeout is a safety net.
	ea_source_recorder_subscribe(L.source, lane->wake);
	while (!atomic_load(&lane->stopping) && !atomic_load(&lane->failed) && !ea_aac_failed(lane->audio)) {
		int64_t position = ea_aac_position(lane->audio);
		EnterCriticalSection(&lane->mutex);
		bool closing = lane->closing_forced || !any_active(lane);
		try_anchor(lane);
		bool anchored = lane->anchored;
		unsigned current = lane->epoch;
		int64_t start = lane->spans[0].start;
		// Every output stopped: encode up to the last one's end, no further.
		if (anchored && closing && !lane->closing) {
			lane->closing = true;
			lane->closing_end = lane->paused ? lane->pause_start : lane->last_ns + lane->frame_ns;
		}
		if (!closing) lane->closing = false;
		int64_t closing_end = lane->closing_end;
		LeaveCriticalSection(&lane->mutex);
		if (!anchored) { WaitForSingleObject(lane->wake, 50); continue; }
		if (current != epoch) {
			epoch = current; cursor = cursor_at(start, block, block_ns);
			written_until = INT64_MIN; overruns = ea_source_recorder_overruns(L.source);
		}
		unsigned end = ea_source_recorder_write_pos(L.source);
		if (end - cursor >= EA_HISTORY || ea_source_recorder_overruns(L.source) != overruns) {
			blog(LOG_ERROR, "[extra-audio] %s extra audio capture history overrun: queued=%u", side(lane), end - cursor);
			atomic_store(&lane->failed, true); break;
		}
		bool have = cursor != end && ea_source_recorder_read(L.source, cursor, block);
		if (closing && (have ? (int64_t)block->timestamp >= closing_end : written_until >= closing_end)) {
			EnterCriticalSection(&lane->mutex);
			bool still = lane->closing_forced || !any_active(lane);
			int64_t base = position + FLUSH_FRAMES;
			if (still) { lane->base = base; lane->anchored = false; lane->closing = false; }
			LeaveCriticalSection(&lane->mutex);
			if (still && !ea_aac_silence_to(lane->audio, base)) atomic_store(&lane->failed, true);
			atomic_store(&lane->position, ea_aac_position(lane->audio));
			continue;
		}
		// A read fails only while the audio thread is writing that slot.
		if (!have) { WaitForSingleObject(lane->wake, cursor == end ? 50 : 1); continue; }
		struct span spans[SPANS];
		EnterCriticalSection(&lane->mutex);
		// Spans cover only short input history; discard them as it drains.
		while (lane->span_count > 1 && lane->spans[0].end <= (int64_t)block->timestamp) {
			memmove(lane->spans, lane->spans + 1, (lane->span_count - 1) * sizeof(*lane->spans)); --lane->span_count;
		}
		unsigned count = lane->span_count;
		memcpy(spans, lane->spans, count * sizeof(*spans));
		bool paused = lane->paused;
		int64_t safe_end = paused ? lane->pause_start : lane->last_ns + lane->frame_ns;
		LeaveCriticalSection(&lane->mutex);
		// Encoded packets can lag the exact pause boundary by several frames.
		// Keep input up to that boundary before discarding paused time.
		if (!closing && !paused && (int64_t)block->timestamp + block_ns > safe_end) { WaitForSingleObject(lane->wake, 50); continue; }
		if (closing) safe_end = closing_end;
		for (unsigned n = 0; n < count; ++n) {
			int64_t low = spans[n].start, high = spans[n].end; if (high > safe_end) high = safe_end;
			int64_t from = 0, to = AUDIO_OUTPUT_FRAMES;
			if ((int64_t)block->timestamp < low) from = av_rescale_rnd(low - (int64_t)block->timestamp, rate, 1000000000LL, AV_ROUND_DOWN);
			if (high != INT64_MAX) to = av_rescale_rnd(high - (int64_t)block->timestamp, rate, 1000000000LL, AV_ROUND_DOWN);
			if (to > AUDIO_OUTPUT_FRAMES) to = AUDIO_OUTPUT_FRAMES; if (from < 0) from = 0; if (to <= from) continue;
			int64_t target = av_rescale_rnd((int64_t)block->timestamp - spans[n].origin, rate, 1000000000LL, AV_ROUND_NEAR_INF) + from;
			int64_t now = ea_aac_position(lane->audio); if (target + to - from <= now) continue;
			if (target > now && !ea_aac_silence_to(lane->audio, target)) { atomic_store(&lane->failed, true); break; }
			now = ea_aac_position(lane->audio);
			unsigned skip = target < now ? (unsigned)(now - target) : 0;
			EnterCriticalSection(&lane->mutex);
			if (lane->epoch == epoch && lane->first_audio == INT64_MAX) lane->first_audio = now;
			LeaveCriticalSection(&lane->mutex);
			if (!ea_aac_append(lane->audio, block, (unsigned)from + skip, (unsigned)(to - from) - skip)) { atomic_store(&lane->failed, true); break; }
		}
		written_until = (int64_t)block->timestamp + block_ns;
		atomic_store(&lane->position, ea_aac_position(lane->audio));
		++cursor;
	}
	ea_source_recorder_unsubscribe(L.source, lane->wake);
	bfree(block);
	return 0;
}

static void stop_thread(struct ea_lane *lane)
{
	if (!lane->thread) return;
	atomic_store(&lane->stopping, true); SetEvent(lane->wake);
	WaitForSingleObject(lane->thread, INFINITE); CloseHandle(lane->thread); lane->thread = NULL;
	lane->queue_held = true;
}

static void close_lane(struct ea_lane *lane)
{
	stop_thread(lane);
	if (lane->user_count) blog(LOG_ERROR, "[extra-audio] Closing %s extra-track encoders with %u outputs attached", side(lane), lane->user_count);
	ea_aac_close(lane->audio);
	blog(LOG_INFO, "[extra-audio] Released the %s extra-track encoders", side(lane));
	bfree(lane->settings); CloseHandle(lane->wake); DeleteCriticalSection(&lane->mutex);
	memset(lane, 0, sizeof(*lane));
}

void ea_lanes_configure(const struct ea_record_config *config, struct ea_source_recorder *source)
{
	if (!L.mutex_ready) { InitializeCriticalSection(&L.mutex); L.mutex_ready = true; }
	ea_lanes_close();
	L.config = *config; L.source = source; L.rate = ea_source_recorder_rate(source); L.ready = true;
}

void ea_lanes_end_outputs(void)
{
	if (!L.ready) return;
	EnterCriticalSection(&L.mutex);
	for (unsigned i = 0; i < LANES; ++i) {
		struct ea_lane *lane = &L.lanes[i];
		if (!lane->used) continue;
		EnterCriticalSection(&lane->mutex); lane->closing_forced = true; LeaveCriticalSection(&lane->mutex);
		SetEvent(lane->wake);
	}
	LeaveCriticalSection(&L.mutex);
}

void ea_lanes_close(void)
{
	if (!L.mutex_ready) return;
	EnterCriticalSection(&L.mutex);
	for (unsigned i = 0; i < LANES; ++i) if (L.lanes[i].used) close_lane(&L.lanes[i]);
	L.ready = false; L.source = NULL;
	LeaveCriticalSection(&L.mutex);
}

void ea_lanes_tick(void)
{
	if (!L.ready) return;
	EnterCriticalSection(&L.mutex);
	for (unsigned i = 0; i < LANES; ++i) {
		struct ea_lane *lane = &L.lanes[i];
		if (!lane->used) continue;
		unsigned seconds = MIN_RETENTION;
		for (unsigned u = 0; u < USERS; ++u) if (lane->users[u].used && lane->users[u].retention > seconds) seconds = lane->users[u].retention;
		ea_aac_replay_retain(lane->audio, seconds);
		bool failed = atomic_load(&lane->failed) || ea_aac_failed(lane->audio);
		if (lane->user_count) continue;
		EnterCriticalSection(&lane->mutex);
		bool drained = !lane->anchored;
		LeaveCriticalSection(&lane->mutex);
		if (lane->thread && (drained || failed)) {
			stop_thread(lane);
			// Idle: keep only the flush silence, as decoder preroll for the next run.
			ea_aac_replay_trim(lane->audio, lane->base - 2 * FLUSH_FRAMES);
			lane->clock = NULL; lane->paused = false;
		}
		if (lane->thread) continue;
		if (failed) { close_lane(lane); continue; }
		if (lane->queue_held && ea_aac_release_queue(lane->audio)) lane->queue_held = false;
	}
	LeaveCriticalSection(&L.mutex);
}

bool ea_lanes_need_capture(void)
{
	for (unsigned i = 0; i < LANES; ++i) if (L.lanes[i].used && L.lanes[i].thread) return true;
	return false;
}

uint64_t ea_lanes_bytes(void)
{
	uint64_t bytes = 0;
	if (!L.ready) return 0;
	EnterCriticalSection(&L.mutex);
	for (unsigned i = 0; i < LANES; ++i) if (L.lanes[i].used) bytes += ea_aac_replay_bytes(L.lanes[i].audio);
	LeaveCriticalSection(&L.mutex);
	return bytes;
}

static bool same_setup(const struct ea_lane *lane, bool vertical, const char *id, const char *settings)
{
	return lane->vertical == vertical && !strcmp(lane->encoder_id, id) && !strcmp(lane->settings, settings);
}

static struct ea_lane *open_lane(bool vertical, const char *id, obs_data_t *settings, const char *json)
{
	struct ea_lane *lane = NULL;
	// Idle encoders built for other settings would not be used again.
	for (unsigned i = 0; i < LANES; ++i) {
		struct ea_lane *idle = &L.lanes[i];
		if (idle->used && !idle->user_count && !idle->thread && idle->vertical == vertical && !same_setup(idle, vertical, id, json)) close_lane(idle);
	}
	for (unsigned i = 0; i < LANES && !lane; ++i) if (!L.lanes[i].used) lane = &L.lanes[i];
	for (unsigned i = 0; i < LANES && !lane; ++i)
		if (!L.lanes[i].user_count && !L.lanes[i].thread) { close_lane(&L.lanes[i]); lane = &L.lanes[i]; }
	if (!lane) { blog(LOG_ERROR, "[extra-audio] Too many outputs need separate extra-track encoders"); return NULL; }
	struct ea_aac_recorder *audio = ea_aac_open(L.rate, L.config.tracks, L.config.track_count, vertical, L.config.backup_mp3, id, settings, MIN_RETENTION);
	HANDLE wake = audio ? CreateEventW(NULL, FALSE, FALSE, NULL) : NULL;
	if (!wake) { ea_aac_close(audio); return NULL; }
	memset(lane, 0, sizeof(*lane));
	lane->used = true; lane->vertical = vertical; lane->audio = audio; lane->wake = wake; lane->settings = bstrdup(json);
	snprintf(lane->encoder_id, sizeof(lane->encoder_id), "%s", id);
	InitializeCriticalSection(&lane->mutex);
	atomic_init(&lane->position, 0); atomic_init(&lane->stopping, false); atomic_init(&lane->failed, false);
	blog(LOG_INFO, "[extra-audio] Started %u %s extra-track encoders; outputs on the same native encoder share them, and later outputs reuse them",
		ea_aac_track_count(audio), side(lane));
	return lane;
}

struct ea_lane_user *ea_lane_join(obs_output_t *output, bool vertical, unsigned seconds)
{
	obs_encoder_t *encoder = obs_output_get_audio_encoder(output, 0);
	if (!L.ready || !encoder) return NULL;
	char id[128];
	ea_record_audio_encoder(obs_frontend_get_profile_config(), id, sizeof(id));
	obs_output_t *main = obs_frontend_get_recording_output();
	obs_encoder_t *native = main ? obs_output_get_audio_encoder(main, 0) : NULL;
	obs_data_t *settings = native && !strcmp(obs_encoder_get_id(native), id) ? obs_encoder_get_settings(native) : NULL;
	obs_output_release(main);
	const char *json = settings ? obs_data_get_json(settings) : "";
	struct ea_lane_user *user = NULL;
	EnterCriticalSection(&L.mutex);
	struct ea_lane *lane = NULL;
	bool shared = false;
	// Another output on this native encoder: share its encoders.
	for (unsigned i = 0; i < LANES && !lane; ++i) {
		struct ea_lane *l = &L.lanes[i];
		if (l->used && l->clock == encoder && !atomic_load(&l->failed) && same_setup(l, vertical, id, json)) { lane = l; shared = l->user_count != 0; }
	}
	// Otherwise reuse idle encoders with the same settings.
	for (unsigned i = 0; i < LANES && !lane; ++i) {
		struct ea_lane *l = &L.lanes[i];
		if (l->used && !l->user_count && !l->thread && !atomic_load(&l->failed) && same_setup(l, vertical, id, json)) lane = l;
	}
	bool created = !lane;
	if (!lane) lane = open_lane(vertical, id, settings, json);
	for (unsigned i = 0; lane && i < USERS && !user; ++i) if (!lane->users[i].used) user = &lane->users[i];
	if (user) {
		EnterCriticalSection(&lane->mutex);
		if (!lane->user_count && !lane->thread) {
			lane->clock = encoder; lane->anchored = false; lane->paused = false; lane->closing = false; lane->span_count = 0; lane->last_ns = 0;
			uint32_t frames = (uint32_t)obs_encoder_get_frame_size(encoder);
			lane->frame_ns = (int64_t)audio_frames_to_ns(L.rate, frames ? frames : 1024);
		}
		memset(user, 0, sizeof(*user));
		user->lane = lane; user->output = output; user->used = true; user->retention = seconds;
		++lane->user_count; lane->joined = os_gettime_ns();
		// Paused before this output joined: its pause signal already passed.
		if (!lane->paused && obs_encoder_paused(encoder)) {
			uint64_t start = ea_native_pause_start(encoder);
			if (start) { lane->paused = true; lane->pause_start = (int64_t)start; lane->pause_offset = (int64_t)obs_encoder_get_pause_offset(encoder); }
		}
		LeaveCriticalSection(&lane->mutex);
		if (!lane->thread) {
			atomic_store(&lane->stopping, false);
			lane->thread = CreateThread(NULL, 0, lane_thread, lane, 0, NULL);
			if (!lane->thread) atomic_store(&lane->failed, true);
		}
		blog(LOG_INFO, "[extra-audio] %s takes its extra tracks from the %s encoders (%s)", obs_output_get_name(output), side(lane),
			created ? "new" : shared ? "shared with another output" : "reused");
	} else if (lane) {
		blog(LOG_ERROR, "[extra-audio] Too many outputs share the %s extra-track encoders", side(lane));
	}
	LeaveCriticalSection(&L.mutex);
	obs_data_release(settings);
	return user;
}

void ea_lane_leave(struct ea_lane_user *user)
{
	if (!user) return;
	struct ea_lane *lane = user->lane;
	EnterCriticalSection(&L.mutex);
	EnterCriticalSection(&lane->mutex);
	user->used = false; user->output = NULL; --lane->user_count;
	LeaveCriticalSection(&lane->mutex);
	SetEvent(lane->wake);
	LeaveCriticalSection(&L.mutex);
}

void ea_lane_packet(struct ea_lane_user *user, const struct encoder_packet *packet)
{
	struct ea_lane *lane = user->lane;
	unsigned rate = L.rate;
	int64_t global = packet->sys_dts_usec * 1000;
	int64_t frame = av_rescale_q(packet->pts, (AVRational){packet->timebase_num, packet->timebase_den}, (AVRational){1, (int)rate});
	EnterCriticalSection(&lane->mutex);
	if (global > lane->last_ns) lane->last_ns = global;
	user->frame0 = global - av_rescale(frame, 1000000000, rate); user->frame0_known = true;
	if (!lane->anchored) try_anchor(lane);
	else if (user->offset_epoch != lane->epoch) {
		// Joined a running lane: place it by this packet's span.
		unsigned n = lane->span_count - 1;
		while (n && lane->spans[n].start > global) --n;
		user->offset = av_rescale_rnd(global - lane->spans[n].origin, rate, 1000000000, AV_ROUND_NEAR_INF) - frame;
		user->base = lane->base; user->offset_epoch = lane->epoch;
	}
	LeaveCriticalSection(&lane->mutex);
	SetEvent(lane->wake);
}

void ea_lane_restart(struct ea_lane_user *user)
{
	struct ea_lane *lane = user->lane;
	EnterCriticalSection(&lane->mutex);
	user->frame0_known = false; user->offset_epoch = 0;
	LeaveCriticalSection(&lane->mutex);
}

bool ea_lane_offset(struct ea_lane_user *user, int64_t *offset, int64_t *base, int64_t *first_audio)
{
	struct ea_lane *lane = user->lane;
	EnterCriticalSection(&lane->mutex);
	bool known = user->offset_epoch != 0;
	if (known) {
		*offset = user->offset;
		if (base) *base = user->base;
		if (first_audio) *first_audio = user->offset_epoch == lane->epoch ? lane->first_audio : INT64_MIN;
	}
	LeaveCriticalSection(&lane->mutex);
	return known;
}

struct ea_aac_recorder *ea_lane_audio(struct ea_lane_user *user) { return user->lane->audio; }
int64_t ea_lane_position(struct ea_lane_user *user) { return atomic_load(&user->lane->position); }
void ea_lane_retain(struct ea_lane_user *user, unsigned seconds) { user->retention = seconds; }
bool ea_lane_failed(struct ea_lane_user *user) { return atomic_load(&user->lane->failed) || ea_aac_failed(user->lane->audio); }

void ea_lane_pause(obs_encoder_t *encoder)
{
	if (!L.ready || !encoder) return;
	bool estimated;
	uint64_t start = ea_pause_start(encoder, &estimated);
	if (!start) return;
	EnterCriticalSection(&L.mutex);
	for (unsigned i = 0; i < LANES; ++i) {
		struct ea_lane *lane = &L.lanes[i];
		if (!lane->used || lane->clock != encoder || !lane->user_count) continue;
		EnterCriticalSection(&lane->mutex);
		if (!lane->paused) {
			lane->pause_start = (int64_t)start; lane->pause_offset = (int64_t)obs_encoder_get_pause_offset(encoder); lane->paused = true;
			if (lane->anchored) lane->spans[lane->span_count - 1].end = (int64_t)start;
			if (estimated) blog(LOG_WARNING, "[extra-audio] %s extra audio paused; OBS's exact pause position isn't readable here, so it is placed from the video clock", side(lane));
			else blog(LOG_INFO, "[extra-audio] %s extra audio paused at native sample boundary", side(lane));
		}
		LeaveCriticalSection(&lane->mutex);
		SetEvent(lane->wake);
	}
	LeaveCriticalSection(&L.mutex);
}

void ea_lane_unpause(obs_encoder_t *encoder)
{
	if (!L.ready || !encoder) return;
	EnterCriticalSection(&L.mutex);
	for (unsigned i = 0; i < LANES; ++i) {
		struct ea_lane *lane = &L.lanes[i];
		if (!lane->used || lane->clock != encoder || !lane->user_count) continue;
		EnterCriticalSection(&lane->mutex);
		if (lane->paused) {
			int64_t gap = (int64_t)obs_encoder_get_pause_offset(encoder) - lane->pause_offset;
			if (lane->anchored) {
				struct span *last = &lane->spans[lane->span_count - 1];
				if (gap < 0 || (gap && lane->span_count == SPANS)) {
					atomic_store(&lane->failed, true);
					blog(LOG_ERROR, "[extra-audio] Native resume clock unavailable; %s extra audio stops and original files are retained", side(lane));
				} else if (gap) {
					lane->spans[lane->span_count++] = (struct span){.start = lane->pause_start + gap, .end = INT64_MAX, .origin = last->origin + gap};
				} else last->end = INT64_MAX;
			}
			lane->paused = false;
			blog(LOG_INFO, "[extra-audio] %s extra audio resumed; skipped %.3f seconds", side(lane), gap / 1e9);
		}
		LeaveCriticalSection(&lane->mutex);
		SetEvent(lane->wake);
	}
	LeaveCriticalSection(&L.mutex);
}
