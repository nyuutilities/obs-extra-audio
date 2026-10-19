// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <obs.h>
#include <stdatomic.h>
#include <stdio.h>
#include "track-defaults.h"

#define EA_MAX_STEMS 32u
#define EA_FIRST_EXTRA_TRACK 7u
#define EA_MAX_EXTRA_TRACKS 10u
#define EA_HISTORY 512u

struct ea_stem_binding {
	char uuid[64], name[256];
};

struct ea_extra_track {
	char name[256];
	uint32_t sources;
	unsigned number, bitrate;
	bool main, vertical, enabled;
};

// One history block: the OBS audio timestamp and planar float stereo for each extra track.
struct ea_stem_block {
	uint64_t timestamp;
	float pcm[EA_MAX_EXTRA_TRACKS][2][AUDIO_OUTPUT_FRAMES];
};

struct ea_source_recorder;
void ea_source_recorder_register(void);
// Missing or non-audio sources record silence instead of failing the recorder.
struct ea_source_recorder *ea_source_recorder_open_tracks(const struct ea_stem_binding *sources, unsigned source_count,
	const struct ea_extra_track *tracks, unsigned track_count);
void ea_source_recorder_close(struct ea_source_recorder *recorder);
// Capture runs, and its history exists, only between start and stop. Stop only
// when no reader remains; restarting begins a new range at first_pos.
bool ea_source_recorder_start(struct ea_source_recorder *recorder);
void ea_source_recorder_stop(struct ea_source_recorder *recorder);
bool ea_source_recorder_capturing(struct ea_source_recorder *recorder);
unsigned ea_source_recorder_first_pos(struct ea_source_recorder *recorder);
// A Windows event signalled after each published block. Unsubscribe before
// closing it.
bool ea_source_recorder_subscribe(struct ea_source_recorder *recorder, void *event);
void ea_source_recorder_unsubscribe(struct ea_source_recorder *recorder, void *event);
// UI thread only. Releases every captured source, or only those removed from
// OBS; released sources record silence until the recorder is reopened.
void ea_source_recorder_release_sources(struct ea_source_recorder *recorder, bool all);
unsigned ea_source_recorder_missing(struct ea_source_recorder *recorder);
unsigned ea_source_recorder_write_pos(struct ea_source_recorder *recorder);
// Copies one stable history block. Used by worker threads, never the OBS audio thread.
bool ea_source_recorder_read(struct ea_source_recorder *recorder, unsigned position, struct ea_stem_block *block);
uint64_t ea_source_recorder_overruns(struct ea_source_recorder *recorder);
unsigned ea_source_recorder_rate(struct ea_source_recorder *recorder);
