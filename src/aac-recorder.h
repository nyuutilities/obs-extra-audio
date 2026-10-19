// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "source-recorder.h"
struct ea_aac_recorder;
void ea_aac_register(void);
// The bitrates (kbps) OBS's Settings offer for an encoder at this sample rate,
// read from the encoder's own "bitrate" property; 0 when it has none.
#define EA_MAX_BITRATES 64u
size_t ea_encoder_bitrates(const char *encoder_id, unsigned rate, unsigned *bitrates, size_t max);
// Extra-track encoders that keep running between outputs. Encoded packets stay
// in RAM for `seconds` of media time; a tee or a clip export copies them out.
struct ea_aac_recorder *ea_aac_open(unsigned rate,const struct ea_extra_track *tracks,unsigned count,
    bool vertical,bool backup_all,const char *encoder_id,obs_data_t *encoder_settings,unsigned seconds);
bool ea_aac_close(struct ea_aac_recorder *writer);
unsigned ea_aac_track_count(const struct ea_aac_recorder *writer);
bool ea_aac_failed(struct ea_aac_recorder *writer);
// Writer thread, before any output uses the encoders: measures each
// encoder's delay relative to AAC-LC on the same encoder, and returns the
// position after the measurement.
bool ea_aac_calibrate(struct ea_aac_recorder *writer,int64_t *end);
// Total delay of one encoder, in samples, as used for its edit lists.
int ea_aac_encoder_delay(struct ea_aac_recorder *writer,unsigned index);
// Written by one thread only.
int64_t ea_aac_position(const struct ea_aac_recorder *writer);
bool ea_aac_silence_to(struct ea_aac_recorder *writer, int64_t frame);
bool ea_aac_append(struct ea_aac_recorder *writer, const struct ea_stem_block *block, unsigned start, unsigned count);
// Frees the PCM queue once the encoders have taken every block; the next
// append allocates it again. Returns false while blocks are still queued.
bool ea_aac_release_queue(struct ea_aac_recorder *writer);
bool ea_aac_replay_export(struct ea_aac_recorder *writer,const char *path,bool vertical,bool all,
    int64_t start_frame,int64_t frames);
void ea_aac_replay_retain(struct ea_aac_recorder *writer,unsigned seconds);
// Drops packets that end before `frame`.
void ea_aac_replay_trim(struct ea_aac_recorder *writer,int64_t frame);
int64_t ea_aac_replay_ready(struct ea_aac_recorder *writer);
// Waits until the next encoded packet arrives, or the timeout passes.
void ea_aac_wait_progress(struct ea_aac_recorder *writer, unsigned milliseconds);
uint64_t ea_aac_replay_bytes(struct ea_aac_recorder *writer);
bool ea_recording_audio_frames(const char *path, unsigned rate, int64_t *frames);
// Writes one output's tracks from running encoders into an MP4 sidecar. The
// output's sample 0 is the encoders' frame `offset`; earlier packets become
// decoder preroll behind the MP4 edit list, as with a newly started encoder.
struct ea_aac_tee;
struct ea_aac_tee *ea_aac_tee_open(struct ea_aac_recorder *source,const char *path,bool vertical,int64_t offset);
// Copies packets that end by `frames` of the output's own timeline.
bool ea_aac_tee_pump(struct ea_aac_tee *tee,int64_t frames);
// Writes the rest up to `frames`, trimming the last packet, and closes the
// file. With frames <= 0 it only closes it.
bool ea_aac_tee_finish(struct ea_aac_tee *tee,int64_t frames);
