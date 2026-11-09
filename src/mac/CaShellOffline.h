#pragma once

#include "CaShell.h"
#include "CaDevice.h"
#include "CaClock.h"

#ifndef CA_SHELL_OFFLINE
#error "CaShellOffline.h is for the offline tools only"
#endif

namespace pinna {

extern bool gShellOffline;

bool ShellStartOffline(Shared *shared, double rate);
void ShellRenderOffline(float *L, float *R, uint32_t frames);
bool ShellPassThroughTest(const float *inL, const float *inR, float *outL, float *outR, uint32_t frames);
bool ShellRebuildOfflineForTest(double rate);
void ShellFailPullsForTest(int unit = 0, uint32_t blocks = 0);
void ShellFailSpatialOnceForTest(int unit = 0);
typedef void (*ClockTapFn)(void *ctx, const float *dryL, const float *dryR, const float *send, const float *strL, const float *strR, uint32_t frames, double rate);
// Set only while the clock is stopped or asleep on a test clock time.
void ShellClockTapForTest(ClockTapFn tap, void *ctx);
typedef void (*BlockTapFn)(void *ctx, const Mixer &mixer, bool spatial, const float *strL, const float *strR, uint32_t frames);
void ShellBlockTapForTest(BlockTapFn tap, void *ctx);
void ShellSpatialUnitForTest(int32_t *outputType, int32_t *algorithm, float *calib);
int32_t ShellHeadTrackingForTest(void);
void ShellFakeDevicesForTest(const DeviceEntry *list, const double *rates, int count, uint32_t defaultId);
void ShellPostEventForTest(int event, uint32_t id);
void ShellBufferEventForTest(uint32_t frames);
void ShellWaitControlForTest(void);
bool ShellFakeUnitPullForTest(float *L, float *R, uint32_t frames);
bool ShellFakeUnitPullMultiForTest(float *const *channels, uint32_t count, uint32_t frames);
void ShellMultichannelForTest(uint32_t *channels, uint32_t *layoutTag);
uint32_t ShellLayoutTagForLabelsForTest(const uint32_t *labels, uint32_t count);
uint64_t ShellFramesForTest(void);
uint32_t ShellConfigCallsForTest(void);
uint32_t ShellDeviceCallsForTest(void);
void ShellConfigDeviceForTest(char *out, size_t size);
void ShellQueueLabelForTest(char *out, size_t size);
void ShellDropControlForTest(void);
void ShellFadeAtStopForTest(uint32_t *quiet, uint32_t *tail);
void ShellFeedForTest(uint32_t chunkAt, uint32_t frames, uint32_t capacity, float *mono, float *stereoL, float *stereoR);
uint64_t ShellClassicFramesForTest(void);
bool ShellSettleStreamsForTest(uint32_t frames, int timeoutMs, double *waitedMs);
bool ShellWaitPrefilledForTest(uint8_t stream, int timeoutMs, double *waitedMs);
int ShellOwnerForTest(void);
void ShellClockTimeForTest(ClockTime *time);
// With a test clock time set, a retry waits on that time and runs only from here.
int ShellRunDueRetriesForTest(void);
int ShellPendingRetriesForTest(void);
Decoder *OpenAppleDecoderForTest(const char *path, bool corrections);
uint32_t UserTrackProbesForTest(void);
uint32_t ShellHrtfArmsForTest(void);

}
