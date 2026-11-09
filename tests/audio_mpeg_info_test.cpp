// mkdir -p build/tests && clang++ -std=gnu++14 -Wall -Wextra -I src tests/audio_mpeg_info_test.cpp -o build/tests/audio_mpeg_info_test && build/tests/audio_mpeg_info_test
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include "CaDecoder.h"

using namespace pinna;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static std::vector<uint8_t>
Frame(uint8_t b1, uint8_t b2, uint8_t b3, size_t tagAt, const char *tag, uint32_t flags, uint32_t frames)
{
	std::vector<uint8_t> f(600, 0);
	f[0] = 0xff; f[1] = b1; f[2] = b2; f[3] = b3;
	if(tag){
		memcpy(&f[tagAt], tag, 4);
		for(int i = 0; i < 4; i++){
			f[tagAt + 4 + i] = (uint8_t)(flags >> (24 - 8 * i));
			f[tagAt + 8 + i] = (uint8_t)(frames >> (24 - 8 * i));
		}
	}
	return f;
}

int
main(void)
{
	uint32_t frames = 0, spf = 0;
	std::vector<uint8_t> f = Frame(0xfb, 0x98, 0x00, 36, "Info", 0xf, 92402);
	CHECK(MpegInfoFrames(f.data(), f.size(), &frames, &spf) && frames == 92402 && spf == 1152);
	f = Frame(0xfb, 0x98, 0xc0, 21, "Xing", 0x1, 77);
	CHECK(MpegInfoFrames(f.data(), f.size(), &frames, &spf) && frames == 77 && spf == 1152);
	f = Frame(0xf3, 0x88, 0xc0, 13, "Info", 0x3, 172554);
	CHECK(MpegInfoFrames(f.data(), f.size(), &frames, &spf) && frames == 172554 && spf == 576);
	f = Frame(0xf3, 0x88, 0x00, 21, "Info", 0x1, 5);
	CHECK(MpegInfoFrames(f.data(), f.size(), &frames, &spf) && frames == 5 && spf == 576);
	f = Frame(0xfa, 0x98, 0x00, 38, "Info", 0x1, 9);
	CHECK(MpegInfoFrames(f.data(), f.size(), &frames, &spf) && frames == 9);
	f = Frame(0xfb, 0x98, 0x00, 36, "Info", 0x2, 9);
	CHECK(!MpegInfoFrames(f.data(), f.size(), &frames, &spf));
	f = Frame(0xfb, 0x98, 0x00, 36, NULL, 0, 0);
	CHECK(!MpegInfoFrames(f.data(), f.size(), &frames, &spf));
	f = Frame(0xfb, 0x98, 0x00, 38, "Info", 0x1, 9);
	CHECK(!MpegInfoFrames(f.data(), f.size(), &frames, &spf));
	f = Frame(0xfd, 0x98, 0x00, 36, "Info", 0x1, 9);
	CHECK(!MpegInfoFrames(f.data(), f.size(), &frames, &spf));
	f = Frame(0xeb, 0x98, 0x00, 21, "Info", 0x1, 9);
	CHECK(!MpegInfoFrames(f.data(), f.size(), &frames, &spf));
	f = Frame(0xfb, 0x98, 0x00, 36, "Info", 0x1, 9);
	CHECK(!MpegInfoFrames(f.data(), 47, &frames, &spf));
	CHECK(MpegInfoFrames(f.data(), 48, &frames, &spf));

	uint8_t id3[10] = { 'I', 'D', '3', 3, 0, 0, 0, 0, 1, 0x52 };
	CHECK(Id3Size(id3, 10) == 10 + 128 + 82);
	id3[5] = 0x10;
	CHECK(Id3Size(id3, 10) == 20 + 128 + 82);
	CHECK(Id3Size(id3, 9) == 0);
	CHECK(Id3Size(f.data(), f.size()) == 0);

	CHECK(MpegTrimmedFrames(92402, 1152, 576, 1427) == 106445101);
	CHECK(MpegTrimmedFrames(1, 1152, 576, 1000) == 0);

	CHECK(MpegCappedLength(106446253, 106445101, 1152) == 106445101);
	CHECK(MpegCappedLength(1000 + 2 * 576, 1000, 576) == 1000);
	CHECK(MpegCappedLength(1001 + 2 * 576, 1000, 576) == 1001 + 2 * 576);
	CHECK(MpegCappedLength(106446253, 0, 1152) == 106446253);
	CHECK(MpegCappedLength(106446253, 106446253, 1152) == 106446253);
	CHECK(MpegCappedLength(106446253, 106446254, 1152) == 106446253);
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all mpeg info tests passed\n");
	return 0;
}
