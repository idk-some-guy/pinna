// mkdir -p build/tests && clang++ -std=gnu++14 -Wall -Wextra -I src tests/audio_track_cache_test.cpp -o build/tests/audio_track_cache_test && build/tests/audio_track_cache_test
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include "CaTrackCache.h"

using namespace pinna;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static std::vector<uint8_t>
Written(const TrackLengthCache &c)
{
	static uint8_t raw[4096];
	FILE *w = fmemopen(raw, sizeof(raw), "wb");
	bool ok = c.write(w);
	long n = w ? ftell(w) : 0;
	if(w)
		fclose(w);
	return ok ? std::vector<uint8_t>(raw, raw + n) : std::vector<uint8_t>();
}

static bool
Read(TrackLengthCache &c, std::vector<uint8_t> buf)
{
	FILE *f = buf.empty() ? NULL : fmemopen(buf.data(), buf.size(), "rb");
	bool ok = c.read(f);
	if(f)
		fclose(f);
	return ok;
}

int
main(void)
{
	TrackLengthCache first;
	CHECK(!first.changed());
	first.keep("/mp3/a.m4a", 4096, 1700000000123456789ll, 240000);
	first.keep("/mp3/protected.m4a", 5000, -5, 0);
	first.keep("/mp3/b.flac", 106445101ull * 40, 1600000000000000001ll, 1000);
	CHECK(first.changed());
	std::vector<uint8_t> good = Written(first);
	CHECK(good.size() == 16 + 3 * 24 + strlen("/mp3/a.m4a") + strlen("/mp3/protected.m4a") + strlen("/mp3/b.flac"));
	CHECK(memcmp(good.data(), "CAUT", 4) == 0);
	uint32_t word;
	memcpy(&word, &good[4], 4);
	CHECK(word == kTrackCacheVersion);
	memcpy(&word, &good[8], 4);
	CHECK(word == kLengthRulesVersion);
	memcpy(&word, &good[12], 4);
	CHECK(word == 3);

	TrackLengthCache warm;
	CHECK(Read(warm, good));
	uint32_t ms = 7;
	CHECK(warm.find("/mp3/a.m4a", 4096, 1700000000123456789ll, &ms) && ms == 240000);
	CHECK(warm.find("/mp3/b.flac", 106445101ull * 40, 1600000000000000001ll, &ms) && ms == 1000);
	ms = 7;
	CHECK(warm.find("/mp3/protected.m4a", 5000, -5, &ms) && ms == 0);
	CHECK(!warm.find("/mp3/a.m4a", 4097, 1700000000123456789ll, &ms));
	CHECK(!warm.find("/mp3/a.m4a", 4096, 1700000000123456790ll, &ms));
	CHECK(!warm.find("/mp3/a.m4a", 4096, 1700000001123456789ll, &ms));
	CHECK(!warm.find("/mp3/protected.m4a", 5001, -5, &ms));
	CHECK(!warm.find("/mp3/c.mp3", 4096, 1700000000123456789ll, &ms));
	CHECK(!warm.find("/mp3/a.m4", 4096, 1700000000123456789ll, &ms));

	CHECK(warm.changed());
	warm.keep("/mp3/a.m4a", 4096, 1700000000123456789ll, 240000);
	warm.keep("/mp3/protected.m4a", 5000, -5, 0);
	warm.keep("/mp3/b.flac", 106445101ull * 40, 1600000000000000001ll, 1000);
	CHECK(!warm.changed());
	warm.keep("/mp3/b.flac", 106445101ull * 40, 1600000000000000002ll, 1000);
	CHECK(warm.changed());

	TrackLengthCache gone;
	CHECK(Read(gone, good));
	gone.keep("/mp3/a.m4a", 4096, 1700000000123456789ll, 240000);
	gone.keep("/mp3/b.flac", 106445101ull * 40, 1600000000000000001ll, 1000);
	CHECK(gone.changed());
	std::vector<uint8_t> two = Written(gone);
	memcpy(&word, &two[12], 4);
	CHECK(word == 2);
	TrackLengthCache after;
	CHECK(Read(after, two));
	CHECK(!after.find("/mp3/protected.m4a", 5000, -5, &ms));
	CHECK(after.find("/mp3/a.m4a", 4096, 1700000000123456789ll, &ms) && ms == 240000);

	TrackLengthCache bad;
	std::vector<uint8_t> b(good);
	word = kTrackCacheVersion + 1;
	memcpy(&b[4], &word, 4);
	CHECK(!Read(bad, b));
	CHECK(!bad.find("/mp3/a.m4a", 4096, 1700000000123456789ll, &ms));
	word = kTrackCacheVersion - 1;
	memcpy(&b[4], &word, 4);
	CHECK(!Read(bad, b));
	b = good;
	word = kLengthRulesVersion + 1;
	memcpy(&b[8], &word, 4);
	CHECK(!Read(bad, b));
	CHECK(!bad.find("/mp3/a.m4a", 4096, 1700000000123456789ll, &ms));
	b = good;
	b[0] = 'X';
	CHECK(!Read(bad, b));
	b = good;
	word = 4;
	memcpy(&b[12], &word, 4);
	CHECK(!Read(bad, b));
	b = good;
	word = 2;
	memcpy(&b[12], &word, 4);
	CHECK(!Read(bad, b));
	CHECK(!bad.find("/mp3/a.m4a", 4096, 1700000000123456789ll, &ms));
	for(size_t cut = 1; cut < good.size(); cut++){
		std::vector<uint8_t> shorter(good.begin(), good.begin() + cut);
		TrackLengthCache t;
		if(Read(t, shorter) || t.find("/mp3/a.m4a", 4096, 1700000000123456789ll, &ms)){
			printf("FAIL a cache cut to %zu bytes is trusted\n", cut);
			failures++;
			break;
		}
	}
	b = good;
	word = 0xffffffff;
	memcpy(&b[16 + 4], &word, 4);
	CHECK(!Read(bad, b));
	b = good;
	word = 0;
	memcpy(&b[16 + 4], &word, 4);
	CHECK(!Read(bad, b));
	b = good;
	b.push_back(0);
	CHECK(!Read(bad, b));
	TrackLengthCache none;
	CHECK(!none.read(NULL));
	CHECK(!none.write(NULL));
	CHECK(!none.changed());

	TrackLengthCache reread;
	CHECK(Read(reread, good));
	CHECK(!Read(reread, std::vector<uint8_t>(good.begin(), good.end() - 1)));
	CHECK(!reread.find("/mp3/a.m4a", 4096, 1700000000123456789ll, &ms));

	const char *file = "build/tests/audio_track_cache_test.cache";
	remove(file);
	TrackLengthCache loaded;
	CHECK(!LoadTrackCache(file, &loaded));
	CHECK(StoreTrackCache(file, first));
	CHECK(LoadTrackCache(file, &loaded));
	CHECK(loaded.find("/mp3/a.m4a", 4096, 1700000000123456789ll, &ms) && ms == 240000);
	remove(file);
	CHECK(StoreTrackCache(file, none));
	CHECK(fopen(file, "rb") == NULL);
	CHECK(!StoreTrackCache("build/tests/no such folder/track.cache", first));
	CHECK(StoreTrackCache("build/tests/no such folder/track.cache", none));
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all track cache tests passed\n");
	return 0;
}
