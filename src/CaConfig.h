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

static_assert(kChannels >= 1, "PINNA_CHANNELS must be at least 1");
static_assert(kStreams >= 1 && kStreams <= 255, "PINNA_STREAMS must be 1 to 255, stream indices are uint8_t");
static_assert(kPoolSlots >= 1 && kPoolSlots <= 127, "PINNA_POOL_SLOTS must be 1 to 127, pool slots are int8_t");
static_assert(kPoolSlots > 2 * kChannels, "PINNA_POOL_SLOTS must exceed twice PINNA_CHANNELS, or a start fails once every channel holds a current and a reader slot");

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
