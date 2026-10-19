// SPDX-License-Identifier: GPL-2.0-or-later
#include "routing.h"
#include <string.h>

static bool check_output(void *data, obs_output_t *output)
{
	// This plugin's own extra-track encoders stay running between outputs.
	if (!strcmp(obs_output_get_id(output), "extra_audio_encoder_sink")) return true;
	if (obs_output_active(output) && (obs_output_get_flags(output) & OBS_OUTPUT_ENCODED))
		*(bool *)data = true;
	return true;
}

enum ea_support ea_version_support(uint32_t version, uint32_t tested)
{
	if (version < tested) return EA_TOO_OLD;
	return version == tested ? EA_TESTED : EA_UNTESTED;
}

bool ea_outputs_busy(void)
{
	bool busy = false;
	obs_enum_outputs(check_output, &busy);
	return busy;
}
