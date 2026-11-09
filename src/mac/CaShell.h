#pragma once

#include <stddef.h>
#include <stdint.h>
#include "CaDecoder.h"
#include "CaLog.h"
#include "CaMixer.h"
#include "CaTrackCache.h"

namespace pinna {

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
	int32_t outputType;
	bool fellBack;
	char device[128];
};

bool ShellStart(Shared *shared, const char *deviceUid, int32_t outputPref = OUTPUT_AUTO, int32_t algorithm = kSpatialUseOutputType, const ShellConfig *config = NULL);
void ShellStop(void);
void ShellService(void);
ShellInfo ShellGetInfo(void);
int32_t ShellOutputType(void);
bool ShellEchoReady(void);
double ShellRate(void);
void ShellSetDevice(const char *uid);
void ShellSetSpatial(bool on);
static const int32_t kKeepConfig = -1;
void ShellSetOutputConfig(int32_t algorithm, int32_t outputPref, int32_t headTracking, const char *device = NULL);
void ShellSetAlgorithm(int32_t algorithm);
void ShellSetOutputPref(int32_t outputPref);
void ShellSetHeadTracking(bool on);
int ShellDeviceCount(void);
bool ShellDeviceAt(int index, char *name, size_t nameSize, char *uid, size_t uidSize);
uint32_t ShellDeviceListGeneration(void);

}
