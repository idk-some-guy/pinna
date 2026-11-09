#pragma once

#include <stdint.h>

namespace pinna {

struct PlaylistEntry
{
	uint32_t length, streamPos;
};

inline uint32_t
PlaylistLayout(PlaylistEntry *e, uint32_t count)
{
	uint32_t total = 0;
	for(uint32_t i = 0; i < count; i++){
		e[i].streamPos = total;
		total += e[i].length;
	}
	return total;
}

inline bool
PlaylistFind(const PlaylistEntry *e, uint32_t count, uint32_t *pos, uint32_t *index)
{
	for(uint32_t i = 0; i < count; i++)
		if(*pos >= e[i].streamPos && *pos < e[i].length + e[i].streamPos){
			*pos -= e[i].streamPos;
			*index = i;
			return true;
		}
	*pos = 0;
	*index = 0;
	return false;
}

}
