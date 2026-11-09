#pragma once

#include <stdint.h>

#ifdef PINNA_CONFIG_HEADER
#include PINNA_CONFIG_HEADER
#endif

#ifndef PINNA_CHANNELS
#define PINNA_CHANNELS 44
#endif
#ifndef PINNA_STREAMS
#define PINNA_STREAMS 3
#endif
#ifndef PINNA_POOL_SLOTS
#define PINNA_POOL_SLOTS 96
#endif

namespace ca {

enum
{
	kChannels = PINNA_CHANNELS,
	kStreams = PINNA_STREAMS,
	kMaxBlock = 4096,
	kPoolSlots = PINNA_POOL_SLOTS,
	kOalFadeFrames = 64,
	kOalResamplerEdge = 24,
	kOalMaxPitch = 10,
	kOalStepOne = 65536,
	kFallbackRate = 48000
};

enum
{
	MODE_CLASSIC = 0,
	MODE_SPATIAL,
	MODE_REIMAGINED,
	NUM_MODES
};

enum
{
	OUTPUT_AUTO = 0,
	OUTPUT_HEADPHONES,
	OUTPUT_BUILTIN,
	OUTPUT_EXTERNAL,
	OUTPUT_MULTICHANNEL,
	NUM_OUTPUTS
};

}
