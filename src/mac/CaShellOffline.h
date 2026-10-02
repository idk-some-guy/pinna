#pragma once

#include "CaShell.h"

#ifndef CA_SHELL_OFFLINE
#error "CaShellOffline.h is for the offline tools only"
#endif

namespace ca {

extern bool gShellOffline;

bool ShellStartOffline(Shared *shared, double rate);
void ShellRenderOffline(float *L, float *R, uint32_t frames);
bool ShellPassThroughTest(const float *inL, const float *inR, float *outL, float *outR, uint32_t frames);

}
