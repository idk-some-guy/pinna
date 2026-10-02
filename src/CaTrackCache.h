#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <string>
#include "CaDecoder.h"

namespace ca {

// Bump when the user-track probe rules or this cache's layout change; kLengthRulesVersion is checked as well.
static const uint32_t kTrackCacheVersion = 1;

struct TrackRecord
{
	uint32_t ms, pathLen;
	uint64_t size;
	int64_t mtimeNs;
};

static_assert(sizeof(TrackRecord) == 24, "the cache stores 24 bytes per track before its path");

class TrackLengthCache
{
	struct Entry
	{
		uint32_t ms;
		uint64_t size;
		int64_t mtimeNs;
		bool operator==(const Entry &o) const { return ms == o.ms && size == o.size && mtimeNs == o.mtimeNs; }
	};
	std::map<std::string, Entry> loaded, kept;

public:
	bool
	read(FILE *f)
	{
		loaded.clear();
		uint32_t header[4];
		if(f == NULL || fread(header, sizeof(uint32_t), 4, f) != 4 || memcmp(header, "CAUT", 4) != 0 || header[1] != kTrackCacheVersion ||
		   header[2] != kLengthRulesVersion)
			return false;
		std::map<std::string, Entry> got;
		for(uint32_t i = 0; i < header[3]; i++){
			TrackRecord r;
			if(fread(&r, sizeof(r), 1, f) != 1 || r.pathLen == 0 || r.pathLen > 4096)
				return false;
			std::string path(r.pathLen, '\0');
			if(fread(&path[0], 1, r.pathLen, f) != r.pathLen)
				return false;
			got[path] = Entry{ r.ms, r.size, r.mtimeNs };
		}
		if(fgetc(f) != EOF || got.size() != header[3])
			return false;
		loaded.swap(got);
		return true;
	}

	bool
	write(FILE *f) const
	{
		uint32_t header[4] = { 0, kTrackCacheVersion, kLengthRulesVersion, (uint32_t)kept.size() };
		memcpy(header, "CAUT", 4);
		if(f == NULL || fwrite(header, sizeof(uint32_t), 4, f) != 4)
			return false;
		for(const auto &k : kept){
			TrackRecord r = { k.second.ms, (uint32_t)k.first.size(), k.second.size, k.second.mtimeNs };
			if(fwrite(&r, sizeof(r), 1, f) != 1 || fwrite(k.first.data(), 1, k.first.size(), f) != k.first.size())
				return false;
		}
		return true;
	}

	bool
	find(const char *path, uint64_t size, int64_t mtimeNs, uint32_t *ms) const
	{
		auto it = loaded.find(path);
		if(it == loaded.end() || it->second.size != size || it->second.mtimeNs != mtimeNs)
			return false;
		*ms = it->second.ms;
		return true;
	}

	void keep(const char *path, uint64_t size, int64_t mtimeNs, uint32_t ms) { kept[path] = Entry{ ms, size, mtimeNs }; }

	bool changed(void) const { return !(kept == loaded); }
};

inline bool
LoadTrackCache(const char *path, TrackLengthCache *cache)
{
	FILE *f = fopen(path, "rb");
	bool ok = cache->read(f);
	if(f)
		fclose(f);
	return ok;
}

inline bool
StoreTrackCache(const char *path, const TrackLengthCache &cache)
{
	if(!cache.changed())
		return true;
	FILE *f = fopen(path, "wb");
	bool ok = cache.write(f);
	if(f)
		ok = fclose(f) == 0 && ok;
	return ok;
}

}
