// SPDX-License-Identifier: GPL-2.0-or-later
#include <obs-frontend-api.h>
#include <obs-module.h>
#include "recording-manager.h"
#include <stdlib.h>
#include <string.h>

static bool scan_item(obs_scene_t *scene, obs_sceneitem_t *item, void *data)
{
	(void)scene;
	struct ea_record_config *c = data;
	obs_source_t *source = obs_sceneitem_get_source(item);
	obs_scene_t *nested = obs_scene_from_source(source);
	if (nested) { obs_scene_enum_items(nested, scan_item, data); return true; }
	if (!(obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) || c->count >= EA_MAX_STEMS) return true;
	const char *uuid = obs_source_get_uuid(source);
	for (unsigned i = 0; i < c->count; ++i) if (strcmp(c->stems[i].uuid, uuid) == 0) return true;
	struct ea_stem_binding *s = &c->stems[c->count++];
	snprintf(s->uuid, sizeof(s->uuid), "%s", uuid);
	snprintf(s->name, sizeof(s->name), "%s", obs_source_get_name(source));
	return true;
}

static int source_order(const void *a, const void *b)
{
	const struct ea_stem_binding *x = a, *y = b;
	unsigned nx = (unsigned)strtoul(x->name, NULL, 10), ny = (unsigned)strtoul(y->name, NULL, 10);
	if (nx && ny && nx != ny) return nx < ny ? -1 : 1;
	return strcmp(x->name, y->name);
}

static unsigned source_index(const struct ea_record_config *c, const char *uuid)
{
	for (unsigned i = 0; i < c->count; ++i) if (strcmp(c->stems[i].uuid, uuid) == 0) return i;
	return EA_MAX_STEMS;
}

bool ea_record_scene_scan(const char *uuid, struct ea_record_config *c)
{
	obs_source_t *source = obs_get_source_by_uuid(uuid);
	obs_scene_t *scene = obs_scene_from_source(source);
	if (!scene) { obs_source_release(source); return false; }
	struct ea_record_config *old = bmemdup(c, sizeof(*c));
	c->count = 0; memset(c->stems, 0, sizeof(c->stems));
	obs_scene_enum_items(scene, scan_item, c);
	snprintf(c->scene_uuid, sizeof(c->scene_uuid), "%s", uuid);
	qsort(c->stems, c->count, sizeof(c->stems[0]), source_order);
	// Refresh the input list without creating tracks or changing existing mixes.
	for (unsigned t = 0; t < c->track_count; ++t) {
		c->tracks[t].sources = 0;
		for (unsigned i = 0; i < old->count; ++i) if (old->tracks[t].sources & (1u << i)) {
			unsigned next = source_index(c, old->stems[i].uuid);
			if (next == EA_MAX_STEMS && c->count < EA_MAX_STEMS) {
				next = c->count++; c->stems[next] = old->stems[i];
			}
			if (next < EA_MAX_STEMS) c->tracks[t].sources |= 1u << next;
		}
	}
	bfree(old); obs_source_release(source);
	return c->count != 0;
}

static bool find_audio_scene(void *data, obs_source_t *source)
{
	const char *name = obs_source_get_name(source);
	if (strstr(name, "AUDIO") || strstr(name, "Audio")) return !ea_record_scene_scan(obs_source_get_uuid(source), data);
	return true;
}

struct ea_extra_track *ea_record_track_find(struct ea_record_config *c, unsigned number)
{
	for (unsigned i = 0; i < c->track_count; ++i) if (c->tracks[i].number == number) return &c->tracks[i];
	return NULL;
}

bool ea_record_track_enable(struct ea_record_config *c, unsigned number, bool enabled)
{
	if (number < EA_FIRST_EXTRA_TRACK || number >= EA_FIRST_EXTRA_TRACK + EA_MAX_EXTRA_TRACKS) return false;
	struct ea_extra_track *track = ea_record_track_find(c, number);
	if (track) { track->enabled = enabled; return true; }
	if (!enabled) return true;
	if (c->track_count >= EA_MAX_EXTRA_TRACKS) return false;
	unsigned index = 0;
	while (index < c->track_count && c->tracks[index].number < number) ++index;
	memmove(c->tracks + index + 1, c->tracks + index, (c->track_count++ - index) * sizeof(c->tracks[0]));
	track = &c->tracks[index]; memset(track, 0, sizeof(*track));
	track->number = number; track->bitrate = EA_DEFAULT_RECORD_BITRATE;
	track->enabled = track->main = track->vertical = true;
	return true;
}

void ea_record_track_add(struct ea_record_config *c)
{
	for (unsigned n = EA_FIRST_EXTRA_TRACK; n < EA_FIRST_EXTRA_TRACK + EA_MAX_EXTRA_TRACKS; ++n) {
		struct ea_extra_track *t = ea_record_track_find(c, n);
		if (!t || !t->enabled) { ea_record_track_enable(c, n, true); return; }
	}
}

void ea_record_config_active(struct ea_record_config *c)
{
	unsigned active = 0;
	for (unsigned i = 0; i < c->track_count; ++i) if (c->tracks[i].enabled) c->tracks[active++] = c->tracks[i];
	c->track_count = active;
}

bool ea_record_capture_routes_valid(const struct ea_record_config *c, char *error, size_t size)
{
    if (!ea_record_config_valid(c, error, size)) return false;
    uint32_t capture = 0;
    for (unsigned i = 0; i < c->track_count; ++i)
        if (c->tracks[i].enabled && c->enabled && (c->tracks[i].main || c->tracks[i].vertical || c->backup_mp3)) capture |= c->tracks[i].sources;
    for (unsigned i = 0; i < c->count; ++i) if (capture & (1u << i)) {
        obs_source_t *source = obs_get_source_by_uuid(c->stems[i].uuid);
        bool available = source && (obs_source_get_audio_mixers(source) & 63u);
        obs_source_release(source);
        if (!available) {
            if (error && size) snprintf(error, size, obs_module_text("Config.NoNativeTrack"), c->stems[i].name);
            return false;
        }
    }
    return true;
}

void ea_record_audio_encoder(config_t *profile, char *id, size_t size)
{
    const char *selected=config_get_string(profile,"AdvOut","RecAudioEncoder");
    if(!selected || !*selected || !strcmp(selected,"none")) selected=config_get_string(profile,"AdvOut","AudioEncoder");
    if(!selected || !*selected) selected="ffmpeg_aac";
    snprintf(id,size,"%s",selected);
}

bool ea_record_config_valid(const struct ea_record_config *c, char *error, size_t size)
{
	const char *message = NULL;
	if (c->invalid || c->count > EA_MAX_STEMS || c->track_count > EA_MAX_EXTRA_TRACKS) message = obs_module_text("Config.TooMany");
	if(!message && c->backup_mp3 && strpbrk(c->backup_format,"/\\"))message=obs_module_text("Config.FormatFolders");
	uint32_t allowed = c->count >= 32 ? UINT32_MAX : ((1u << c->count) - 1u);
	for (unsigned i = 0; !message && i < c->track_count; ++i) {
		if (c->tracks[i].sources & ~allowed) message = obs_module_text("Config.UnknownSource");
		else if (c->tracks[i].number < 7 || c->tracks[i].number > 16) message = obs_module_text("Config.TrackRange");
		// Which bitrates are valid depends on the encoder; ea_ui_save checks that list.
		else if (!c->tracks[i].bitrate) message = obs_module_text("Config.NoBitrate");
		for (unsigned j = 0; j < i; ++j) if (c->tracks[j].number == c->tracks[i].number) message = obs_module_text("Config.DuplicateTrack");
	}
	if (error && size) snprintf(error, size, "%s", message ? message : "");
	return message == NULL;
}

static void load_tracks(obs_data_t *data, struct ea_record_config *c)
{
	snprintf(c->scene_uuid, sizeof(c->scene_uuid), "%s", obs_data_get_string(data, "scene_uuid"));
	obs_data_array_t *sources = obs_data_get_array(data, "sources");
	int64_t version = obs_data_get_int(data, "version");
	// Only the current settings format is read; anything else is refused rather than guessed at.
	if (version != 3 || obs_data_array_count(sources) > EA_MAX_STEMS) c->invalid = true;
	for (size_t i = 0; i < obs_data_array_count(sources) && c->count < EA_MAX_STEMS; ++i) {
		obs_data_t *item = obs_data_array_item(sources, i);
		struct ea_stem_binding *s = &c->stems[c->count++];
		snprintf(s->uuid, sizeof(s->uuid), "%s", obs_data_get_string(item, "uuid"));
		snprintf(s->name, sizeof(s->name), "%s", obs_data_get_string(item, "name"));
		obs_data_release(item);
	}
	obs_data_array_release(sources);
	obs_data_array_t *tracks = obs_data_get_array(data, "tracks");
	if (obs_data_array_count(tracks) > EA_MAX_EXTRA_TRACKS) c->invalid = true;
	for (size_t i = 0; i < obs_data_array_count(tracks) && c->track_count < EA_MAX_EXTRA_TRACKS; ++i) {
		obs_data_t *item = obs_data_array_item(tracks, i);
		struct ea_extra_track *t = &c->tracks[c->track_count++];
		t->number = (unsigned)obs_data_get_int(item, "number");
		t->enabled = obs_data_get_bool(item, "enabled");
		t->bitrate = (unsigned)obs_data_get_int(item, "bitrate");
		snprintf(t->name, sizeof(t->name), "%s", obs_data_get_string(item, "name"));
		t->main = obs_data_get_bool(item, "main"); t->vertical = obs_data_get_bool(item, "vertical");
		obs_data_array_t *members = obs_data_get_array(item, "source_uuids");
		for (size_t j = 0; j < obs_data_array_count(members); ++j) {
			obs_data_t *member = obs_data_array_item(members, j);
			unsigned index = source_index(c, obs_data_get_string(member, "uuid"));
			if (index < EA_MAX_STEMS) t->sources |= 1u << index;
			else c->invalid = true;
			obs_data_release(member);
		}
		obs_data_array_release(members);
		obs_data_release(item);
	}
	obs_data_array_release(tracks);
}

void ea_record_config_load(config_t *profile, struct ea_record_config *c)
{
	memset(c, 0, sizeof(*c));
	c->enabled = config_get_bool(profile, "ExtraAudio", "RecordEnabled");
	c->backup_mp3 = config_get_bool(profile, "ExtraAudio", "BackupMP3");
	const char *backup_format=config_get_string(profile,"ExtraAudio","BackupFilenameFormat");
	snprintf(c->backup_format,sizeof(c->backup_format),"%s",backup_format && *backup_format ? backup_format : EA_DEFAULT_BACKUP_FORMAT);
	c->browser_audio = config_get_bool(profile, "ExtraAudio", "ControlBrowserAudio");
	const char *json = config_get_string(profile, "ExtraAudio", "RecordingTracks");
	obs_data_t *data = json && *json ? obs_data_create_from_json(json) : NULL;
	if (json && *json && !data) { c->invalid = true; return; }
	if (data) { load_tracks(data, c); obs_data_release(data); }
	if (!c->count) obs_enum_scenes(find_audio_scene, c);
}

bool ea_record_config_save(config_t *profile, const struct ea_record_config *c)
{
	if (!ea_record_config_valid(c, NULL, 0)) return false;
	obs_data_t *data = obs_data_create(); obs_data_set_int(data, "version", 3);
	obs_data_set_string(data, "scene_uuid", c->scene_uuid);
	obs_data_array_t *sources = obs_data_array_create(), *tracks = obs_data_array_create();
	for (unsigned i = 0; i < c->count; ++i) {
		obs_data_t *s = obs_data_create(); obs_data_set_string(s, "uuid", c->stems[i].uuid); obs_data_set_string(s, "name", c->stems[i].name);
		obs_data_array_push_back(sources, s); obs_data_release(s);
	}
	for (unsigned i = 0; i < c->track_count; ++i) {
		const struct ea_extra_track *t = &c->tracks[i]; obs_data_t *item = obs_data_create();
		obs_data_set_int(item, "number", t->number); obs_data_set_string(item, "name", t->name);
		obs_data_set_int(item, "bitrate", t->bitrate); obs_data_set_bool(item, "enabled", t->enabled);
		obs_data_set_bool(item, "main", t->main); obs_data_set_bool(item, "vertical", t->vertical);
		obs_data_array_t *members = obs_data_array_create();
		for (unsigned j = 0; j < c->count; ++j) if (t->sources & (1u << j)) {
			obs_data_t *member = obs_data_create(); obs_data_set_string(member, "uuid", c->stems[j].uuid);
			obs_data_array_push_back(members, member); obs_data_release(member);
		}
		obs_data_set_array(item, "source_uuids", members); obs_data_array_release(members);
		obs_data_array_push_back(tracks, item); obs_data_release(item);
	}
	obs_data_set_array(data, "sources", sources); obs_data_set_array(data, "tracks", tracks);
	obs_data_array_release(sources); obs_data_array_release(tracks);
	config_set_bool(profile, "ExtraAudio", "RecordEnabled", c->enabled);
	config_set_bool(profile,"ExtraAudio","BackupMP3",c->backup_mp3);
	config_set_string(profile,"ExtraAudio","BackupFilenameFormat",*c->backup_format?c->backup_format:EA_DEFAULT_BACKUP_FORMAT);
	config_set_bool(profile, "ExtraAudio", "ControlBrowserAudio", c->browser_audio);
	config_set_string(profile, "ExtraAudio", "RecordingTracks", obs_data_get_json(data));
	obs_data_release(data);
	return config_save_safe(profile, "tmp", "bak") == CONFIG_SUCCESS;
}
