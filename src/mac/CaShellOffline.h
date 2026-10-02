#pragma once

#include "CaShell.h"
#include "CaDevice.h"

#ifndef CA_SHELL_OFFLINE
#error "CaShellOffline.h is for the offline tools only"
#endif

namespace ca {

extern bool gShellOffline;

bool ShellStartOffline(Shared *shared, double rate);
void ShellRenderOffline(float *L, float *R, uint32_t frames);
bool ShellPassThroughTest(const float *inL, const float *inR, float *outL, float *outR, uint32_t frames);
bool ShellRebuildOfflineForTest(double rate);
void ShellFailPullsForTest(void);
void ShellFakeDevicesForTest(const DeviceEntry *list, const double *rates, int count, uint32_t defaultId);
void ShellPostEventForTest(int event, uint32_t id);
void ShellWaitControlForTest(void);
bool ShellFakeUnitPullForTest(float *L, float *R, uint32_t frames);
uint64_t ShellFramesForTest(void);
int ShellOwnerForTest(void);
Decoder *OpenAppleDecoderForTest(const char *path, bool corrections);

}
