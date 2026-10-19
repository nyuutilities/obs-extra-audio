// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct ea_mp4_edit {
	HANDLE file;
	uint64_t original_size, old_moov, old_moov_size, size, position;
	uint64_t native_view_read_bytes;
	unsigned char *moov;
	uint32_t timescale, track_count;
	wchar_t journal[32768];
	bool pending, appended, completed;
};

// Exclusive writer lock is owned by the caller. No original media is copied.
bool ea_mp4_begin(struct ea_mp4_edit *edit, HANDLE file, const char *path);
bool ea_mp4_append(struct ea_mp4_edit *edit, const char *audio_path);
// A virtual read view hides the old index for validation BEFORE committing.
int ea_mp4_read(void *edit, uint8_t *buffer, int size);
int64_t ea_mp4_seek(void *edit, int64_t offset, int whence);
bool ea_mp4_commit(struct ea_mp4_edit *edit);
bool ea_mp4_rollback(struct ea_mp4_edit *edit);
