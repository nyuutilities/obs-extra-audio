// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "recording-manager.h"
void ea_replay_configure(const struct ea_record_config *config,struct ea_source_recorder *source,const char *helper);
void ea_replay_tick(void);
void ea_replay_close(void);
void ea_replay_status(char *text,size_t size);
// True while a replay output or its capture thread still needs source audio.
bool ea_replay_needs_capture(void);
