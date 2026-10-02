#pragma once

#include <stdint.h>
#include <stdio.h>

namespace ca {

inline uint32_t
LengthMs(uint64_t frames, uint32_t rate)
{
	return rate ? (uint32_t)(float(frames) * 1000.0f / float(rate)) : 0;
}

inline bool
ReadLengthCache(FILE *f, uint32_t *lengths, uint32_t count)
{
	if(f == NULL || fseek(f, 0, SEEK_END) != 0 || ftell(f) != (long)count * (long)sizeof(uint32_t))
		return false;
	rewind(f);
	return fread(lengths, sizeof(uint32_t), count, f) == count;
}

inline bool
WriteLengthCache(FILE *f, const uint32_t *lengths, uint32_t count)
{
	return f != NULL && fwrite(lengths, sizeof(uint32_t), count, f) == count;
}

}
