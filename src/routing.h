// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <obs.h>

#define EA_VERSION "0.11.15"

// The OBS and Aitum Vertical versions this build was made and tested with. Older versions are
// refused (OBS) or get no vertical extra tracks (Aitum), because the build may rely on what they
// lack; newer ones are used with a "not yet tested" note.
// Test builds with EA_TEST_NEWER_VERSIONS act as if both were newer than the tested versions,
// and as if OBS's internal layout had changed (see native-pause-clock.c).
#ifdef EA_TEST_NEWER_VERSIONS
#define EA_TESTED_OBS MAKE_SEMANTIC_VERSION(32, 2, 1)
#define EA_TESTED_OBS_STRING "32.2.1"
#define EA_TESTED_AITUM MAKE_SEMANTIC_VERSION(1, 6, 5)
#define EA_TESTED_AITUM_STRING "1.6.5"
#else
#define EA_TESTED_OBS MAKE_SEMANTIC_VERSION(32, 2, 2)
#define EA_TESTED_OBS_STRING "32.2.2"
#define EA_TESTED_AITUM MAKE_SEMANTIC_VERSION(1, 6, 6)
#define EA_TESTED_AITUM_STRING "1.6.6"
#endif
enum ea_support { EA_TOO_OLD, EA_TESTED, EA_UNTESTED };
enum ea_support ea_version_support(uint32_t version, uint32_t tested);

// Native live/VOD routing and track names belong to the user: the plugin only reads them to
// show them in its panel (ui-bridge.c), and never writes them.
bool ea_outputs_busy(void);
