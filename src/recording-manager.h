// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "source-recorder.h"
#include <util/config-file.h>

struct ea_record_config {
	bool enabled, browser_audio, backup_mp3;
	char backup_format[512];
	char scene_uuid[64];
	unsigned count;
	struct ea_stem_binding stems[EA_MAX_STEMS];
	unsigned track_count;
	struct ea_extra_track tracks[EA_MAX_EXTRA_TRACKS];
	bool invalid;
};

void ea_record_config_load(config_t *profile, struct ea_record_config *config);
bool ea_record_config_save(config_t *profile, const struct ea_record_config *config);
bool ea_record_scene_scan(const char *uuid, struct ea_record_config *config);
bool ea_record_config_valid(const struct ea_record_config *config, char *error, size_t size);
// Check that assigned inputs can be captured. Never mutate native routing.
bool ea_record_capture_routes_valid(const struct ea_record_config *config, char *error, size_t size);
// Read the user's main recording encoder selection without modifying it.
void ea_record_audio_encoder(config_t *profile, char *id, size_t size);
void ea_record_track_add(struct ea_record_config *config);
struct ea_extra_track *ea_record_track_find(struct ea_record_config *config, unsigned number);
bool ea_record_track_enable(struct ea_record_config *config, unsigned number, bool enabled);
void ea_record_config_active(struct ea_record_config *config);
void ea_record_manager_init(const char *plugin_binary_path);
void ea_record_manager_reload(void);
void ea_record_manager_close(void);
void ea_record_manager_status(char *text, size_t size);
// Drop references to the current scene collection's sources before it unloads.
void ea_record_manager_release_sources(void);
// Start source capture now: an output is about to start or has just started.
void ea_record_manager_prepare(void);
// True only when the loaded Aitum Vertical is the validated 1.6.6.
bool ea_aitum_supported(void);
