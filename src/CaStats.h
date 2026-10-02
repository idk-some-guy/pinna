#pragma once

#include <stdio.h>
#include <stdint.h>

namespace ca {

struct StatsInfo
{
	uint32_t voices, channels, streams, underruns, rebuilds, rate, buffer;
	const char *mode, *output, *device;
};

inline int
FormatStats(char *buf, size_t size, const StatsInfo &s)
{
	return snprintf(buf, size, "sampman_ca: stats voices %u/%u streams %u underruns %u rebuilds %u mode %s output %s rate %u buffer %u device \"%s\"\n",
		s.voices, s.channels, s.streams, s.underruns, s.rebuilds, s.mode, s.output, s.rate, s.buffer, s.device);
}

}
