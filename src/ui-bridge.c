// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui-bridge.h"
#include "recording-manager.h"
#include "routing.h"
#include "aac-recorder.h"
#include "mp3-backup.h"
#include <util/platform.h>
#include <time.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <string.h>

struct source_snapshot { obs_data_array_t *array; const struct ea_record_config *config; };

static bool source_item(void *data, obs_source_t *source)
{
	struct source_snapshot *s = data;
	uint32_t flags = obs_source_get_output_flags(source);
	if (!(flags & OBS_SOURCE_AUDIO) || (flags & OBS_SOURCE_COMPOSITE)) return true;
	const char *uuid = obs_source_get_uuid(source), *id = obs_source_get_unversioned_id(source);
	uint32_t routes = 0;
	for (unsigned i = 0; i < s->config->count; ++i) if (!strcmp(uuid, s->config->stems[i].uuid))
		for (unsigned t = 0; t < s->config->track_count; ++t) if (s->config->tracks[t].number >= 7 &&
			s->config->tracks[t].number <= 16 && (s->config->tracks[t].sources & (1u << i)))
			routes |= 1u << (s->config->tracks[t].number - 7);
	obs_data_t *settings = obs_source_get_settings(source);
	bool independent = !((!strcmp(id, "browser_source") && !obs_data_get_bool(settings, "reroute_audio")) ||
		(!strcmp(id, "window_capture") && !obs_data_get_bool(settings, "capture_audio")));
	obs_data_release(settings);
	obs_data_t *item = obs_data_create();
	obs_data_set_string(item, "uuid", uuid); obs_data_set_string(item, "name", obs_source_get_name(source));
	obs_data_set_int(item, "routes", routes); obs_data_set_bool(item, "independent", independent);
	obs_data_array_push_back(s->array, item); obs_data_release(item); return true;
}

bool ea_ui_read_roles(struct ea_ui_roles *roles)
{
	config_t *profile = obs_frontend_get_profile_config();
	if (!profile) return false;
	roles->live = (unsigned)config_get_uint(profile, "AdvOut", "TrackIndex");
	if (!roles->live) roles->live = 1;
	roles->vod = (unsigned)config_get_uint(profile, "AdvOut", "VodTrackIndex");
	if (!roles->vod) roles->vod = roles->live == 2 ? 1 : 2;
	char id[128];ea_record_audio_encoder(profile,id,sizeof(id));const char *name=obs_encoder_get_display_name(id);
	snprintf(roles->encoder,sizeof(roles->encoder),"%s",name?name:id);
	return true;
}

void ea_ui_status(char *text, size_t size, bool *busy)
{
	ea_record_manager_status(text, size);
	*busy = ea_outputs_busy();
}

// The selected recording encoder's bitrates, at OBS's output sample rate.
static size_t encoder_bitrates(config_t *profile, unsigned *bitrates)
{
	char id[128]; ea_record_audio_encoder(profile, id, sizeof(id));
	struct obs_audio_info ai = {0};
	return ea_encoder_bitrates(id, obs_get_audio_info(&ai) ? ai.samples_per_sec : 0, bitrates, EA_MAX_BITRATES);
}

char *ea_ui_snapshot(void)
{
	config_t *profile = obs_frontend_get_profile_config();
	if (!profile) return bstrdup("{}");
	struct ea_record_config c; ea_record_config_load(profile, &c);
	obs_data_t *data = obs_data_create(); obs_data_array_t *tracks = obs_data_array_create(), *sources = obs_data_array_create();
	char *path = obs_frontend_get_current_profile_path(), *collection = obs_frontend_get_current_scene_collection();
	obs_data_set_string(data, "profile", path ? path : ""); obs_data_set_string(data, "collection", collection ? collection : "");
	bfree(path); bfree(collection);
	struct ea_ui_roles roles; ea_ui_read_roles(&roles);
	obs_data_set_int(data, "live", roles.live); obs_data_set_int(data, "vod", roles.vod);
	obs_data_set_bool(data, "busy", ea_outputs_busy());
	obs_data_set_string(data,"encoder",roles.encoder);
	unsigned bitrates[EA_MAX_BITRATES]; size_t offered = encoder_bitrates(profile, bitrates);
	obs_data_array_t *options = obs_data_array_create();
	for (size_t i = 0; i < offered; ++i) {
		obs_data_t *option = obs_data_create(); obs_data_set_int(option, "kbps", bitrates[i]);
		obs_data_array_push_back(options, option); obs_data_release(option);
	}
	obs_data_set_array(data, "bitrates", options); obs_data_array_release(options);
	obs_data_set_bool(data,"backup_mp3",c.backup_mp3);
	obs_data_set_string(data,"backup_format",c.backup_format);
	obs_data_set_bool(data, "vertical_supported", ea_aitum_supported());
	for (unsigned n = 7; n <= 16; ++n) {
		struct ea_extra_track *t = ea_record_track_find(&c, n);
		struct ea_extra_track fallback = {.number = n, .bitrate = EA_DEFAULT_RECORD_BITRATE, .main = true, .vertical = true};
		if (!t) t = &fallback;
		obs_data_t *item = obs_data_create();
		obs_data_set_int(item, "number", n); obs_data_set_bool(item, "enabled", t->enabled);
		obs_data_set_string(item, "name", t->name); obs_data_set_int(item, "bitrate", t->bitrate);
		obs_data_set_bool(item, "main", t->main); obs_data_set_bool(item, "vertical", t->vertical);
		obs_data_array_push_back(tracks, item); obs_data_release(item);
	}
	struct source_snapshot inputs = {sources, &c}; obs_enum_sources(source_item, &inputs);
	obs_data_set_array(data, "tracks", tracks); obs_data_set_array(data, "sources", sources);
	obs_data_array_release(tracks); obs_data_array_release(sources);
	char status[8192]; ea_record_manager_status(status, sizeof(status)); obs_data_set_string(data, "status", status);
	char *json = bstrdup(obs_data_get_json(data)); obs_data_release(data); return json;
}

void ea_ui_free(char *json) { bfree(json); }

bool ea_ui_backup_example(const char *pattern, unsigned number, const char *name, char *out, size_t size)
{
	// The recording name comes from OBS's own filename format, as it would be for a recording started now.
	config_t *profile = obs_frontend_get_profile_config();
	const char *format = profile ? config_get_string(profile, "Output", "FilenameFormatting") : NULL;
	char *recording = os_generate_formatted_filename("mp4", true, format && *format ? format : "%CCYY-%MM-%DD %hh-%mm-%ss");
	struct ea_extra_track track = {.number = number}; snprintf(track.name, sizeof(track.name), "%s", name ? name : "");
	char base[2048];
	bool ok = recording && ea_backup_name(base, sizeof(base), pattern && *pattern ? pattern : EA_DEFAULT_BACKUP_FORMAT, recording, &track, time(NULL));
	bfree(recording);
	if (ok) snprintf(out, size, "Extra Audio\\Main\\%s.mp3", base);
	return ok;
}
void *ea_ui_parent(void) { return obs_frontend_get_main_window(); }

static bool commit(config_t *profile, const struct ea_record_config *next, char *error, size_t size)
{
	if (!ea_record_config_valid(next, error, size) || !ea_record_capture_routes_valid(next, error, size)) return false;
	if (!ea_record_config_save(profile, next)) { snprintf(error, size, "%s", obs_module_text("Error.SaveFailed")); return false; }
	obs_frontend_save(); ea_record_manager_reload(); return true;
}

bool ea_ui_save(const char *json, char *error, size_t size)
{
	if (ea_outputs_busy()) { snprintf(error, size, "%s", obs_module_text("Error.BusySave")); return false; }
	obs_data_t *data = obs_data_create_from_json(json);
	if (!data) { snprintf(error, size, "%s", obs_module_text("Error.InvalidSettings")); return false; }
	char *path = obs_frontend_get_current_profile_path(), *collection = obs_frontend_get_current_scene_collection();
	bool current = path && collection && !strcmp(path, obs_data_get_string(data, "profile")) && !strcmp(collection, obs_data_get_string(data, "collection"));
	bfree(path); bfree(collection);
	if (!current) { obs_data_release(data); snprintf(error, size, "%s", obs_module_text("Error.ContextChanged")); return false; }
	config_t *profile = obs_frontend_get_profile_config();
	struct ea_record_config previous, c; ea_record_config_load(profile, &previous); c = previous;
	obs_data_array_t *tracks = obs_data_get_array(data, "tracks"); bool valid = obs_data_array_count(tracks) == 10; uint32_t seen = 0;
	for (size_t i = 0; valid && i < obs_data_array_count(tracks); ++i) {
		obs_data_t *item = obs_data_array_item(tracks, i);
		unsigned n = (unsigned)obs_data_get_int(item, "number");
		if (n < 7 || n > 16 || (seen & (1u << (n - 7)))) valid = false;
		else {
			seen |= 1u << (n - 7); bool enabled = obs_data_get_bool(item, "enabled");
			struct ea_extra_track *t = ea_record_track_find(&c, n);
			if (enabled && !t) { ea_record_track_enable(&c, n, true); t = ea_record_track_find(&c, n); }
			if (t) {
				const char *name = obs_data_get_string(item, "name");
				if (strlen(name) >= sizeof(t->name)) valid = false;
				else snprintf(t->name, sizeof(t->name), "%s", name);
				t->enabled = enabled; t->bitrate = (unsigned)obs_data_get_int(item, "bitrate");
				t->main = obs_data_get_bool(item, "main"); t->vertical = obs_data_get_bool(item, "vertical");
			}
		}
		obs_data_release(item);
	}
	obs_data_array_release(tracks);
	c.backup_mp3=obs_data_get_bool(data,"backup_mp3");
	const char *pattern=obs_data_get_string(data,"backup_format");
	if(strlen(pattern)>=sizeof(c.backup_format)) {
		obs_data_release(data);snprintf(error,size,"%s", obs_module_text("Error.FormatTooLong"));return false;
	}
	snprintf(c.backup_format,sizeof(c.backup_format),"%s",*pattern?pattern:EA_DEFAULT_BACKUP_FORMAT);
	obs_data_release(data);
	if (!valid) { snprintf(error, size, "%s", obs_module_text("Error.TrackList")); return false; }
	// Only bitrates the selected encoder offers. Without a list (the encoder is
	// unavailable) recording reports that, so other settings may still be saved.
	unsigned bitrates[EA_MAX_BITRATES]; size_t offered = encoder_bitrates(profile, bitrates);
	for (unsigned i = 0; offered && i < c.track_count; ++i) {
		if (!c.tracks[i].enabled) continue;
		bool listed = false;
		for (size_t j = 0; j < offered; ++j) listed |= bitrates[j] == c.tracks[i].bitrate;
		if (!listed) { snprintf(error, size, obs_module_text("Error.BitrateNotOffered"), c.tracks[i].number); return false; }
	}
	c.enabled = true;
	// Native OBS track assignments own live/VOD membership.
	return commit(profile, &c, error, size);
}

bool ea_ui_assign(const char *uuid, unsigned number, bool checked, char *error, size_t size)
{
	if (ea_outputs_busy()) { snprintf(error, size, "%s", obs_module_text("Error.BusyAssign")); return false; }
	config_t *profile = obs_frontend_get_profile_config();
	struct ea_record_config c; ea_record_config_load(profile, &c);
	struct ea_extra_track *t = ea_record_track_find(&c, number);
	if (!t || !t->enabled) { snprintf(error, size, "%s", obs_module_text("Error.TrackDisabled")); return false; }
	obs_source_t *source = obs_get_source_by_uuid(uuid);
	if (!source || !(obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO)) { obs_source_release(source); snprintf(error, size, "%s", obs_module_text("Error.SourceGone")); return false; }
	unsigned index = 0; for (; index < c.count && strcmp(c.stems[index].uuid, uuid); ++index) {}
	if (index == c.count) {
		if (c.count == EA_MAX_STEMS) { obs_source_release(source); snprintf(error, size, "%s", obs_module_text("Error.TooManySources")); return false; }
		snprintf(c.stems[index].uuid, sizeof(c.stems[index].uuid), "%s", uuid);
		snprintf(c.stems[index].name, sizeof(c.stems[index].name), "%s", obs_source_get_name(source)); ++c.count;
	}
	if (checked) t->sources |= 1u << index; else t->sources &= ~(1u << index);
	// Native Tracks 1-6, including the live mix, belong to Advanced Audio Properties.
	obs_source_release(source);
	return commit(profile, &c, error, size);
}
