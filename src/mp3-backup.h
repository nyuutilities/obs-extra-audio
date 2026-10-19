// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "source-recorder.h"
#include <time.h>
struct ea_mp3_recorder;
// The backup's file name without folder, extension or collision suffix; false for an unusable pattern.
bool ea_backup_name(char *name,size_t size,const char *pattern,const char *recording,const struct ea_extra_track *track,time_t started);
bool ea_backup_filename(char *path,size_t size,const char *directory,const char *pattern,
    const char *recording,const struct ea_extra_track *track);
bool ea_backup_filename_at(char *path,size_t size,const char *directory,const char *pattern,
    const char *recording,const struct ea_extra_track *track,time_t started);
struct ea_mp3_recorder *ea_mp3_open(const char *directory,const char *pattern,const char *recording,
    unsigned rate,const struct ea_extra_track *tracks,unsigned count);
bool ea_mp3_append(struct ea_mp3_recorder *writer,const struct ea_stem_block *block,unsigned start,unsigned count);
bool ea_mp3_close(struct ea_mp3_recorder *writer);
bool ea_mp3_finish(struct ea_mp3_recorder *writer,int64_t frames);
bool ea_mp3_from_aac(const char *audio,const char *directory,const char *pattern,const char *recording,
    unsigned rate,const struct ea_extra_track *tracks,unsigned count,int64_t frames);
