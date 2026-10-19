// SPDX-License-Identifier: GPL-2.0-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include "routing.h"
#include "recording-manager.h"
#include "ui-bridge.h"
#include "aac-recorder.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-extra-audio", "en-US")
OBS_MODULE_AUTHOR("OBS Extra Audio contributors")

#define EA_RELOAD_HOTKEY "ExtraAudio.ReloadRecording"
static obs_hotkey_id reload_hotkey = OBS_INVALID_HOTKEY_ID;
static bool frontend_ready;

static void notice(const char *message)
{
	wchar_t wide[8192];
	MultiByteToWideChar(CP_UTF8, 0, message, -1, wide, 8192);
	MessageBoxW((HWND)obs_frontend_get_main_window_handle(), wide,
		    L"OBS Extra Audio", MB_OK | MB_ICONINFORMATION); // The module name stays untranslated, as in Plugin Manager.
}

// OBS Settings saves plugin frontend hotkeys in the profile's [Hotkeys]
// section but only loads its own, so restore this binding from there.
static void load_hotkey(void)
{
	config_t *profile = obs_frontend_get_profile_config();
	const char *json = profile ? config_get_string(profile, "Hotkeys", EA_RELOAD_HOTKEY) : NULL;
	obs_data_t *data = json && *json ? obs_data_create_from_json(json) : NULL;
	obs_data_array_t *bindings = data ? obs_data_get_array(data, "bindings") : NULL;
	// A profile without a saved binding clears the previous profile's.
	obs_hotkey_load(reload_hotkey, bindings);
	obs_data_array_release(bindings);
	obs_data_release(data);
}

static void on_event(enum obs_frontend_event event, void *param)
{
	(void)param;
	// OBS fires these before it starts the outputs, so capture is running
	// before their encoders take the first audio.
	if (event == OBS_FRONTEND_EVENT_RECORDING_STARTING || event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTING)
		ea_record_manager_prepare();
	if (event == OBS_FRONTEND_EVENT_PROFILE_CHANGING || event == OBS_FRONTEND_EVENT_EXIT) {
		ea_qt_close();
		ea_record_manager_close();
		if (event == OBS_FRONTEND_EVENT_EXIT) frontend_ready = false;
	}
	if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING) {
		// Let this collection's sources, such as audio devices, close with it.
		// Active recordings continue; those sources record silence meanwhile.
		ea_qt_close();
		ea_record_manager_release_sources();
	}
	// OBS clears scene data before EXIT on shutdown and logs any source still
	// referenced as an error, so release them during that cleanup too.
	if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP)
		ea_record_manager_release_sources();
	// Reloading waits until outputs stop, then resolves the new collection's sources.
	if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED && frontend_ready)
		ea_record_manager_reload();
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING || event == OBS_FRONTEND_EVENT_PROFILE_CHANGED) {
		ea_record_manager_init(obs_get_module_binary_path(obs_current_module()));
		ea_qt_init(obs_frontend_get_main_window());
		load_hotkey();
		frontend_ready = true;
	}
}

MODULE_EXPORT const char *obs_module_name(void) { return "OBS Extra Audio"; }
MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

static void reload_recording(void *unused)
{
	(void)unused;
	if (!frontend_ready || ea_outputs_busy()) return;
	struct ea_record_config config; config_t *profile = obs_frontend_get_profile_config();
	ea_record_config_load(profile, &config); char error[512];
	if (!ea_record_capture_routes_valid(&config, error, sizeof(error))) { notice(error); return; }
	obs_frontend_save(); ea_record_manager_reload();
}
static void recording_hotkey(void *unused, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	(void)unused; (void)id; (void)hotkey;
	// A pressed binding arrives on the UI thread under OBS's hotkey lock, and
	// reloading destroys sources whose own hotkeys need that lock to unregister.
	if (pressed) ea_qt_defer(reload_recording, NULL);
}

bool obs_module_load(void)
{
	enum ea_support support = ea_version_support(obs_get_version(), EA_TESTED_OBS);
	if (support == EA_TOO_OLD) {
		blog(LOG_ERROR, "[extra-audio] This build needs OBS " EA_TESTED_OBS_STRING " or newer; found %s.", obs_get_version_string());
		return false;
	}
	if (support == EA_UNTESTED)
		blog(LOG_WARNING, "[extra-audio] Not yet tested with OBS %s (tested with " EA_TESTED_OBS_STRING "); loading anyway.",
		     obs_get_version_string());
	ea_source_recorder_register();
	ea_aac_register();
	obs_frontend_add_event_callback(on_event, NULL);
	obs_frontend_add_tools_menu_item(obs_module_text("Panel.Title"), ea_qt_open, NULL);
	reload_hotkey = obs_hotkey_register_frontend(EA_RELOAD_HOTKEY, obs_module_text("Hotkey.Reload"), recording_hotkey, NULL);
	blog(LOG_INFO, "[extra-audio] Loaded " EA_VERSION "; extra recording tracks, OBS-selected AAC encoder, optional MP3 contingency; native routing is user-controlled");
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_event, NULL);
	if (reload_hotkey != OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(reload_hotkey);
	ea_qt_shutdown();
	ea_record_manager_close();
}
