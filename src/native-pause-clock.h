// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <obs.h>
// Read only, x64: the encoder's own pause position, or 0 when its layout doesn't check out
// (another OBS version) or it isn't paused. Called synchronously from the output's pause signal.
uint64_t ea_native_pause_start(obs_encoder_t *encoder);
// Where OBS placed a pause that was just requested: the encoder's own position when it can be
// read, else placed on OBS's public video clock the way OBS does (*estimated is then set).
// 0 only when the encoder isn't paused. Call it from the output's pause signal.
uint64_t ea_pause_start(obs_encoder_t *encoder, bool *estimated);
// OBS starts a pause on a video frame boundary: two frame intervals after the request, rounded
// down to the frame grid of frame_ts (obs-output.c, get_closest_v_ts).
uint64_t ea_pause_grid_start(uint64_t now, uint64_t frame_ts, uint64_t interval);
// Whether any pause since OBS started was estimated, for the Status box.
bool ea_pause_estimated(void);
