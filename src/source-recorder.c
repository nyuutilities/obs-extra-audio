// SPDX-License-Identifier: GPL-2.0-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "source-recorder.h"
#include <string.h>

#define EA_WAKE_SLOTS 16u

struct stem_snapshot {
	uint64_t timestamp;
	bool ready;
	float pcm[2][AUDIO_OUTPUT_FRAMES];
};

struct ea_source_recorder {
	// Read on the OBS audio thread; cleared and released only on the UI thread.
	_Atomic(obs_source_t *) sources[EA_MAX_STEMS];
	obs_source_t *tap;
	obs_view_t *view;
	bool capturing;
	unsigned count, track_count, rate, missing;
	size_t block_bytes; // PCM for the enabled tracks in one history slot.
	struct ea_extra_track tracks[EA_MAX_EXTRA_TRACKS];
	struct stem_snapshot snapshot[EA_MAX_STEMS];
	struct ea_stem_block working;
	// History exists only while capturing and holds only the enabled tracks.
	uint64_t *timestamps;
	float *pcm;
	// A slot has a reader flag. The audio producer skips a busy slot instead of
	// waiting for the disk thread; the session must then report a capture fault.
	atomic_flag busy[EA_HISTORY];
	atomic_uint published, first;
	atomic_uint_fast64_t overruns;
	// Consumer events, signalled after each published block.
	_Atomic(void *) wake[EA_WAKE_SLOTS];
};

static bool snapshot_render(void *data, uint64_t *timestamp, struct obs_source_audio_mix *audio,
	uint32_t mixers, size_t channels, size_t rate)
{
	(void)audio; (void)channels; (void)rate;
	struct ea_source_recorder *r = data;
	for (unsigned i = 0; i < r->count; ++i) {
		struct stem_snapshot *s = &r->snapshot[i];
		obs_source_t *source = atomic_load_explicit(&r->sources[i], memory_order_acquire);
		s->ready = source && !obs_source_removed(source) && obs_source_active(source) && !obs_source_audio_pending(source);
		if (!s->ready) continue;
		s->timestamp = obs_source_get_audio_timestamp(source);
		struct obs_source_audio_mix rendered;
		obs_source_get_audio_mix(source, &rendered);
		uint32_t available = obs_source_get_audio_mixers(source) & mixers & 63u;
		unsigned mix = 0;
		for (; mix < 6 && !(available & (1u << mix)); ++mix) {}
		if (mix == 6 || !rendered.output[mix].data[0] || !rendered.output[mix].data[1]) {
			s->ready = false;
			continue;
		}
		for (unsigned c = 0; c < 2; ++c)
			memcpy(s->pcm[c], rendered.output[mix].data[c], sizeof(s->pcm[c]));
	}
	// A silent observer: no children, no routing changes, no contribution to
	// the native mix, and no new video encoder. It observes processed sources.
	*timestamp = 0;
	return false;
}

static const char *tap_name(void *unused) { (void)unused; return "Extra Audio Recording Observer"; }
static void *tap_create(obs_data_t *settings, obs_source_t *source)
{
	(void)source;
	return (void *)(uintptr_t)obs_data_get_int(settings, "recorder");
}
static void tap_destroy(void *data) { (void)data; }

void ea_source_recorder_register(void)
{
	struct obs_source_info info = {
		.id = "extra_audio_recording_observer", .type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_COMPOSITE | OBS_SOURCE_CAP_DISABLED,
		.get_name = tap_name, .create = tap_create, .destroy = tap_destroy, .audio_render = snapshot_render,
	};
	obs_register_source(&info);
}

static void capture(void *data, size_t mix, struct audio_data *audio)
{
	if (mix != 0 || audio->frames != AUDIO_OUTPUT_FRAMES) return;
	struct ea_source_recorder *r = data;
	unsigned position = atomic_load_explicit(&r->published, memory_order_relaxed);
	unsigned slot = position % EA_HISTORY;
	struct ea_stem_block *block = &r->working;
	block->timestamp = audio->timestamp;
	memset(block->pcm, 0, r->block_bytes);
	for (unsigned i = 0; i < r->count; ++i) {
		struct stem_snapshot *s = &r->snapshot[i];
		if (!s->ready || s->timestamp < audio->timestamp) continue;
		uint64_t delta = s->timestamp - audio->timestamp;
		if (delta >= 1000000000ULL) continue;
		unsigned offset = (unsigned)(delta * r->rate / 1000000000ULL);
		if (offset >= AUDIO_OUTPUT_FRAMES) continue;
		for (unsigned t = 0; t < r->track_count; ++t) if (r->tracks[t].sources & (1u << i))
			for (unsigned c = 0; c < 2; ++c)
				for (unsigned f = offset; f < AUDIO_OUTPUT_FRAMES; ++f) block->pcm[t][c][f] += s->pcm[c][f - offset];
	}
	// Disk readers never stall the OBS audio thread; a busy slot is an overrun.
	if (atomic_flag_test_and_set_explicit(&r->busy[slot], memory_order_acquire)) {
		atomic_fetch_add(&r->overruns, 1);
		return;
	}
	r->timestamps[slot] = block->timestamp;
	memcpy((char *)r->pcm + (size_t)slot * r->block_bytes, block->pcm, r->block_bytes);
	atomic_flag_clear_explicit(&r->busy[slot], memory_order_release);
	atomic_store_explicit(&r->published, position + 1, memory_order_release);
	for (unsigned i = 0; i < EA_WAKE_SLOTS; ++i) {
		void *event = atomic_load_explicit(&r->wake[i], memory_order_acquire);
		if (event) SetEvent(event);
	}
}

struct ea_source_recorder *ea_source_recorder_open_tracks(const struct ea_stem_binding *bindings, unsigned count,
	const struct ea_extra_track *tracks, unsigned track_count)
{
	struct obs_audio_info ai;
	if (!count || count > EA_MAX_STEMS || !track_count || track_count > EA_MAX_EXTRA_TRACKS ||
		!obs_get_audio_info(&ai) || ai.speakers != SPEAKERS_STEREO) {
		blog(LOG_ERROR, "[extra-audio] Recorder needs 1-32 source inputs, 1-10 extra tracks and initialized stereo audio");
		return NULL;
	}
	struct ea_source_recorder *r = bzalloc(sizeof(*r));
	r->count = count;
	r->track_count = track_count;
	r->block_bytes = track_count * sizeof(r->working.pcm[0]);
	memcpy(r->tracks, tracks, track_count * sizeof(tracks[0]));
	uint32_t used = 0;
	for (unsigned i = 0; i < track_count; ++i) used |= tracks[i].sources;
	r->rate = ai.samples_per_sec;
	atomic_init(&r->published, 0);
	atomic_init(&r->first, 0);
	atomic_init(&r->overruns, 0);
	for (unsigned i = 0; i < EA_HISTORY; ++i) atomic_flag_clear(&r->busy[i]);
	for (unsigned i = 0; i < EA_WAKE_SLOTS; ++i) atomic_init(&r->wake[i], NULL);
	for (unsigned i = 0; i < count; ++i) {
		atomic_init(&r->sources[i], NULL);
		if (!(used & (1u << i))) continue;
		obs_source_t *source = obs_get_source_by_uuid(bindings[i].uuid);
		// A source from another scene collection, or one since deleted, records
		// silence. It must not stop every other extra track from recording.
		if (!source || !(obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO)) {
			blog(LOG_WARNING, "[extra-audio] Configured source %u is unavailable and records silence: %s (%s)", i + 1, bindings[i].name, bindings[i].uuid);
			obs_source_release(source); ++r->missing; continue;
		}
		atomic_init(&r->sources[i], source);
	}
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "recorder", (long long)(uintptr_t)r);
	r->tap = obs_source_create_private("extra_audio_recording_observer", "Extra Audio Recording Observer", settings);
	obs_data_release(settings);
	if (!r->tap) goto fail;
	// Aitum reserves source channel 63 for its audio wrapper. Use a separate
	// tiny view, appended after the normal canvas views while capturing, to
	// observe completed source buffers without modifying either canvas.
	r->view = obs_view_create();
	obs_view_set_source(r->view, 0, r->tap);
	return r;
fail:
	ea_source_recorder_close(r);
	return NULL;
}

static void audio_barrier(void *unused) { (void)unused; }

static void free_history(struct ea_source_recorder *r)
{
	bfree(r->pcm); bfree(r->timestamps);
	r->pcm = NULL; r->timestamps = NULL;
}

bool ea_source_recorder_start(struct ea_source_recorder *r)
{
	if (!r) return false;
	if (r->capturing) return true;
	struct obs_video_info vi;
	if (!obs_get_video_info(&vi)) return false;
	r->timestamps = bzalloc(EA_HISTORY * sizeof(*r->timestamps));
	r->pcm = bzalloc(EA_HISTORY * r->block_bytes);
	for (unsigned i = 0; i < EA_HISTORY; ++i) atomic_flag_clear(&r->busy[i]);
	// Blocks from an earlier capture are gone; readers start from here.
	atomic_store_explicit(&r->first, atomic_load(&r->published), memory_order_release);
	obs_source_inc_active(r->tap);
	vi.base_width = vi.base_height = vi.output_width = vi.output_height = 32;
	if (!obs_view_add2(r->view, &vi)) {
		blog(LOG_ERROR, "[extra-audio] Cannot add the source recorder view");
		obs_source_dec_active(r->tap); free_history(r); return false;
	}
	struct audio_convert_info conversion = {.samples_per_sec = r->rate, .format = AUDIO_FORMAT_FLOAT_PLANAR,
		.speakers = SPEAKERS_STEREO, .allow_clipping = true};
	unsigned connected = 0;
	for (; connected < 6; ++connected)
		if (!audio_output_connect(obs_get_audio(), connected, &conversion, capture, r)) break;
	if (connected < 6) {
		blog(LOG_ERROR, "[extra-audio] Cannot connect source recorder clock tap %u", connected + 1);
		for (unsigned i = 0; i < connected; ++i) audio_output_disconnect(obs_get_audio(), i, capture, r);
		obs_view_remove(r->view); obs_source_dec_active(r->tap);
		obs_queue_task(OBS_TASK_AUDIO, audio_barrier, NULL, true);
		free_history(r); return false;
	}
	r->capturing = true;
	blog(LOG_INFO, "[extra-audio] Source capture started: %u extra tracks, %.1f MiB history", r->track_count,
	     (double)(EA_HISTORY * r->block_bytes) / 1048576.0);
	return true;
}

void ea_source_recorder_stop(struct ea_source_recorder *r)
{
	if (!r || !r->capturing) return;
	// Disconnecting waits for an audio callback already in progress.
	for (unsigned i = 0; i < 6; ++i) audio_output_disconnect(obs_get_audio(), i, capture, r);
	obs_view_remove(r->view);
	obs_source_dec_active(r->tap);
	obs_queue_task(OBS_TASK_AUDIO, audio_barrier, NULL, true);
	free_history(r);
	r->capturing = false;
	blog(LOG_INFO, "[extra-audio] Source capture stopped; history freed");
}

bool ea_source_recorder_capturing(struct ea_source_recorder *r) { return r && r->capturing; }

bool ea_source_recorder_subscribe(struct ea_source_recorder *r, void *event)
{
	for (unsigned i = 0; r && i < EA_WAKE_SLOTS; ++i) {
		void *empty = NULL;
		if (atomic_compare_exchange_strong(&r->wake[i], &empty, event)) return true;
	}
	return false;
}

void ea_source_recorder_unsubscribe(struct ea_source_recorder *r, void *event)
{
	if (!r || !event) return;
	for (unsigned i = 0; i < EA_WAKE_SLOTS; ++i) {
		void *expected = event;
		atomic_compare_exchange_strong(&r->wake[i], &expected, NULL);
	}
	// The audio thread may be signalling it now; let that callback finish
	// before the caller closes the event.
	obs_queue_task(OBS_TASK_AUDIO, audio_barrier, NULL, true);
}

void ea_source_recorder_release_sources(struct ea_source_recorder *r, bool all)
{
	if (!r) return;
	obs_source_t *released[EA_MAX_STEMS];
	unsigned count = 0;
	for (unsigned i = 0; i < r->count; ++i) {
		obs_source_t *source = atomic_load_explicit(&r->sources[i], memory_order_relaxed);
		if (!source || (!all && !obs_source_removed(source))) continue;
		atomic_store_explicit(&r->sources[i], NULL, memory_order_release);
		released[count++] = source;
	}
	if (!count) return;
	// The audio thread may still be rendering with these pointers. Its tasks
	// run after each render, so wait for one before dropping the references.
	// Never release on the audio thread: destroying a source locks OBS's
	// audio source list there.
	obs_queue_task(OBS_TASK_AUDIO, audio_barrier, NULL, true);
	for (unsigned i = 0; i < count; ++i) obs_source_release(released[i]);
	blog(LOG_INFO, "[extra-audio] Released %u captured source%s %s", count, count == 1 ? "" : "s",
	     all ? "while OBS unloads the scene collection" : "removed from OBS");
}

unsigned ea_source_recorder_missing(struct ea_source_recorder *r) { return r ? r->missing : 0; }

void ea_source_recorder_close(struct ea_source_recorder *r)
{
	if (!r) return;
	ea_source_recorder_stop(r);
	if (r->tap) obs_source_remove(r->tap);
	if (r->view) { obs_view_set_source(r->view, 0, NULL); obs_view_destroy(r->view); }
	obs_queue_task(OBS_TASK_AUDIO, audio_barrier, NULL, true);
	if (r->tap) obs_source_release(r->tap);
	// The render callback can still be finishing on the OBS audio thread.
	obs_wait_for_destroy_queue();
	for (unsigned i = 0; i < r->count; ++i) obs_source_release(atomic_load(&r->sources[i]));
	bfree(r);
}

unsigned ea_source_recorder_write_pos(struct ea_source_recorder *r)
{
	return atomic_load_explicit(&r->published, memory_order_acquire);
}

unsigned ea_source_recorder_first_pos(struct ea_source_recorder *r)
{
	return atomic_load_explicit(&r->first, memory_order_acquire);
}

bool ea_source_recorder_read(struct ea_source_recorder *r, unsigned position, struct ea_stem_block *block)
{
	unsigned published = ea_source_recorder_write_pos(r), first = ea_source_recorder_first_pos(r);
	if (!r->pcm || position - first >= published - first || published - position >= EA_HISTORY) return false;
	unsigned slot = position % EA_HISTORY;
	if (atomic_flag_test_and_set_explicit(&r->busy[slot], memory_order_acquire)) return false;
	// Recheck after taking the flag; the producer may have advanced meanwhile.
	published = ea_source_recorder_write_pos(r);
	bool valid = published - position < EA_HISTORY;
	if (valid) {
		block->timestamp = r->timestamps[slot];
		memcpy(block->pcm, (const char *)r->pcm + (size_t)slot * r->block_bytes, r->block_bytes);
	}
	atomic_flag_clear_explicit(&r->busy[slot], memory_order_release);
	return valid;
}

uint64_t ea_source_recorder_overruns(struct ea_source_recorder *r) { return atomic_load(&r->overruns); }
unsigned ea_source_recorder_rate(struct ea_source_recorder *r) { return r->rate; }
