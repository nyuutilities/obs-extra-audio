// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>
bool ea_mp4_audio_frames(const char *path,unsigned rate,int64_t *frames);
bool ea_mp4_set_audio_duration(const char *path,unsigned rate,int64_t frames);
