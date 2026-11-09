// mkdir -p build/tests && clang++ -std=gnu++14 -Wall -Wextra -I src tests/audio_length_cache_test.cpp -o build/tests/audio_length_cache_test && build/tests/audio_length_cache_test
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include "CaDecoder.h"

using namespace pinna;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static bool
Read(std::vector<uint8_t> buf, LengthEntry *entries, uint32_t count)
{
	FILE *f = fmemopen(buf.data(), buf.size(), "rb");
	bool ok = ReadLengthCache(f, entries, count);
	if(f)
		fclose(f);
	return ok;
}

static void
Put32(std::vector<uint8_t> &b, uint32_t v)
{
	b.insert(b.end(), (uint8_t *)&v, (uint8_t *)&v + 4);
}

static bool
Same(const LengthEntry *a, const LengthEntry *b, uint32_t count)
{
	for(uint32_t i = 0; i < count; i++)
		if(a[i].ms != b[i].ms || a[i].size != b[i].size || a[i].mtimeNs != b[i].mtimeNs)
			return false;
	return true;
}

int
main(void)
{
	const LengthEntry entries[3] = {
		{ 1000, 0, 4096, 1700000000123456789ll },
		{ 106445101 / 32, 0, 106445101ull * 40, 1600000000000000001ll },
		{ 7, 0, 44, -5 },
	};
	uint8_t raw[128];
	memset(raw, 0xaa, sizeof(raw));
	FILE *w = fmemopen(raw, sizeof(raw), "wb");
	CHECK(WriteLengthCache(w, entries, 3));
	long written = w ? ftell(w) : -1;
	if(w)
		fclose(w);
	CHECK(written == 12 + 3 * 24);
	CHECK(memcmp(raw, "CALC", 4) == 0);
	uint32_t version;
	memcpy(&version, raw + 4, 4);
	CHECK(version == kLengthRulesVersion && kLengthRulesVersion == 3);
	uint64_t size;
	memcpy(&size, raw + 12 + 24 + 8, 8);
	CHECK(size == 106445101ull * 40);
	std::vector<uint8_t> good(raw, raw + written);

	LengthEntry got[3] = {};
	CHECK(Read(good, got, 3) && Same(got, entries, 3));

	std::vector<uint8_t> old;
	for(int i = 0; i < 3; i++)
		Put32(old, entries[i].ms);
	CHECK(!Read(old, got, 3));
	std::vector<uint8_t> v2;
	Put32(v2, 0);
	memcpy(v2.data(), "CALC", 4);
	Put32(v2, 2);
	Put32(v2, 3);
	for(int i = 0; i < 3; i++)
		Put32(v2, entries[i].ms);
	CHECK(!Read(v2, got, 3));

	std::vector<uint8_t> bad(good);
	uint32_t v = kLengthRulesVersion + 1;
	memcpy(&bad[4], &v, 4);
	CHECK(!Read(bad, got, 3));
	v = kLengthRulesVersion - 1;
	memcpy(&bad[4], &v, 4);
	CHECK(!Read(bad, got, 3));

	bad = good;
	bad[0] = 'X';
	CHECK(!Read(bad, got, 3));

	bad = good;
	v = 4;
	memcpy(&bad[8], &v, 4);
	CHECK(!Read(bad, got, 3));

	CHECK(!Read(good, got, 2));
	std::vector<uint8_t> shorter(good.begin(), good.end() - 4);
	CHECK(!Read(shorter, got, 3));
	CHECK(!ReadLengthCache(NULL, got, 3));
	CHECK(!WriteLengthCache(NULL, entries, 3));

	CHECK(LengthEntryCurrent(entries[0], 4096, 1700000000123456789ll));
	CHECK(!LengthEntryCurrent(entries[0], 4097, 1700000000123456789ll));
	CHECK(!LengthEntryCurrent(entries[0], 4096, 1700000000123456790ll));
	CHECK(!LengthEntryCurrent(entries[0], 4096, 1700000001123456789ll));
	LengthEntry none = { 0, 0, 0, 0 };
	CHECK(!LengthEntryCurrent(none, 0, 0));
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all length cache tests passed\n");
	return 0;
}
