#pragma once

#include <stdint.h>
#include "CaDecoder.h"
#include "CaMixer.h"

namespace ca {

uint32_t ProbeLengthMs(const char *path);
uint32_t LoadStreamLengths(const char *cachePath, const char *const *paths, uint32_t count, uint32_t *lengths);

struct ShellInfo
{
	double rate;
	uint32_t buffer;
	uint32_t rebuilds;
	bool spatialBypassed;
	char device[128];
};

bool ShellStart(Shared *shared, const char *deviceUid);
void ShellStop(void);
ShellInfo ShellGetInfo(void);

}
