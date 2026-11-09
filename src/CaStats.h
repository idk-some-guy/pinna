#pragma once

#include <stdio.h>
#include <stdint.h>
#include "CaLog.h"

namespace ca {

struct StatsInfo
{
	uint32_t voices, channels, streams, underruns, rebuilds, rate, buffer;
	const char *mode, *output, *device;
	const char *detail;
};

inline int
FormatStats(char *buf, size_t size, const StatsInfo &s)
{
	bool detail = s.detail && s.detail[0];
	return snprintf(buf, size, "%s stats voices %u/%u streams %u underruns %u rebuilds %u mode %s%s%s output %s rate %u buffer %u device \"%s\"\n",
		LogPrefix(), s.voices, s.channels, s.streams, s.underruns, s.rebuilds, s.mode, detail ? " " : "", detail ? s.detail : "", s.output, s.rate, s.buffer, s.device);
}

}
