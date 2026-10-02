#pragma once

#include <stdint.h>

namespace ca {

enum
{
	kChannels = 44,
	kStreams = 3,
	kMaxBlock = 4096,
	kPoolSlots = 96,
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
