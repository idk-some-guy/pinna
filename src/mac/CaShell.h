#pragma once

#include <stddef.h>
#include <stdint.h>
#include "CaDecoder.h"
#include "CaMixer.h"
#include "CaTrackCache.h"

namespace ca {

Decoder *OpenDecoder(const char *path);
uint32_t ProbeLengthMs(const char *path);
uint32_t ProbeUserTrackMs(const char *path, bool *definitive = NULL);
uint32_t UserTrackLengthMs(TrackLengthCache *cache, const char *path);
uint32_t LoadStreamLengths(const char *cachePath, const char *const *paths, uint32_t count, uint32_t *lengths);

struct ShellInfo
{
	double rate;
	uint32_t buffer;
	uint32_t rebuilds;
	uint32_t stalls;
	bool spatialBypassed;
	bool spatialUnavailable;
	int32_t hrtfAtStart, hrtfAfterSecond;
	uint32_t hrtfReadings;
	bool fellBack;
	char device[128];
};

bool ShellStart(Shared *shared, const char *deviceUid);
void ShellStop(void);
ShellInfo ShellGetInfo(void);
void ShellSetDevice(const char *uid);
void ShellSetSpatial(bool on);
int ShellDeviceCount(void);
bool ShellDeviceAt(int index, char *name, size_t nameSize, char *uid, size_t uidSize);
uint32_t ShellDeviceListGeneration(void);

}
