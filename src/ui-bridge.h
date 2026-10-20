// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
char *ea_ui_snapshot(void);
// vod_used: OBS's Twitch VOD track option is on and names a track other than the stream's;
// otherwise VODs get the stream's audio.
struct ea_ui_roles { unsigned live, vod; bool vod_used; char encoder[128]; };
bool ea_ui_read_roles(struct ea_ui_roles *roles);
// The recorder's current status, as written to the OBS log, and whether outputs are active.
void ea_ui_status(char *text, size_t size, bool *busy);
void ea_ui_free(char *json);
// What a main recording's MP3 backup for this track would be called now, as Extra Audio\\Main\\<name>.mp3.
bool ea_ui_backup_example(const char *pattern, unsigned number, const char *name, char *out, size_t size);
bool ea_ui_save(const char *json, char *error, size_t size);
bool ea_ui_assign(const char *uuid, unsigned number, bool checked, char *error, size_t size);
void *ea_ui_parent(void);
void ea_qt_init(void *parent);
void ea_qt_open(void *unused);
void ea_qt_close(void);
void ea_qt_shutdown(void);
// Run a task from the Qt event loop, never inside the caller's stack.
void ea_qt_defer(void (*task)(void *), void *param);
#ifdef __cplusplus
}
#endif
