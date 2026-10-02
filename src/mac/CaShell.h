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

extern bool gShellOffline;

bool ShellStart(Shared *shared, const char *deviceUid);
void ShellStop(void);
bool ShellStartOffline(Shared *shared, double rate);
void ShellRenderOffline(float *L, float *R, uint32_t frames);
bool ShellPassThroughTest(const float *inL, const float *inR, float *outL, float *outR, uint32_t frames);
ShellInfo ShellGetInfo(void);

}
