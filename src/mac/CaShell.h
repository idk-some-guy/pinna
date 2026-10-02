#pragma once

#include <stdint.h>
#include "CaDecoder.h"

namespace ca {

uint32_t ProbeLengthMs(const char *path);
uint32_t LoadStreamLengths(const char *cachePath, const char *const *paths, uint32_t count, uint32_t *lengths);

}
