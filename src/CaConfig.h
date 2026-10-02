#pragma once

#include <stdint.h>

namespace ca {

enum
{
	kChannels = 44,
	kChannels3D = 43,
	kPoliceRadio = 43,
	kStreams = 3,
	kMaxBlock = 4096,
	kPoolSlots = 96
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
