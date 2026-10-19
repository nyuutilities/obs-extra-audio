// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "recording-manager.h"
struct ea_aac_recorder;
struct ea_lane_user;
// Silence the encoders get after their last output stops. An output that
// began up to this much before the run it joined starts with that silence.
#define EA_LANE_FLUSH_FRAMES (12 * AUDIO_OUTPUT_FRAMES)
// Extra-track encoders shared by every output on one native audio encoder and
// kept for later outputs; see encoder-lane.c. All calls but ea_lane_packet,
// ea_lane_offset, ea_lane_pause and ea_lane_unpause are for the UI thread.
void ea_lanes_configure(const struct ea_record_config *config, struct ea_source_recorder *source);
// Treat every output as stopped, so pending recordings and clips finish now.
void ea_lanes_end_outputs(void);
// Every user must have left.
void ea_lanes_close(void);
void ea_lanes_tick(void);
bool ea_lanes_need_capture(void);
uint64_t ea_lanes_bytes(void);
// The caller keeps `output` referenced, and stops calling ea_lane_packet for
// it, before ea_lane_leave.
struct ea_lane_user *ea_lane_join(obs_output_t *output, bool vertical, unsigned seconds);
void ea_lane_leave(struct ea_lane_user *user);
// Track 1 audio packets of the user's output, from its packet callback.
void ea_lane_packet(struct ea_lane_user *user, const struct encoder_packet *packet);
// The output started again, so its sample 0 moved.
void ea_lane_restart(struct ea_lane_user *user);
// The encoder frame of the output's sample 0; the first frame of the encoders'
// run that contains it; and the first frame of captured audio in that run
// (INT64_MAX until written, INT64_MIN once a later run began).
bool ea_lane_offset(struct ea_lane_user *user, int64_t *offset, int64_t *base, int64_t *first_audio);
struct ea_aac_recorder *ea_lane_audio(struct ea_lane_user *user);
int64_t ea_lane_position(struct ea_lane_user *user);
// Seconds of encoded audio this user needs kept in RAM.
void ea_lane_retain(struct ea_lane_user *user, unsigned seconds);
bool ea_lane_failed(struct ea_lane_user *user);
// From the pause and unpause signals of any pausable output.
void ea_lane_pause(obs_encoder_t *encoder);
void ea_lane_unpause(obs_encoder_t *encoder);
