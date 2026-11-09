// mkdir -p build/tests && clang++ -std=gnu++14 -Wall -Wextra -I src -I tests/support tests/audio_ima_test.cpp tests/support/CaImaWav.cpp -o build/tests/audio_ima_test && build/tests/audio_ima_test
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include "CaImaWav.h"

using namespace pinna;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static const int kStep[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
	73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
	724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
	4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
	27086, 29794, 32767 };
static const int kIndex[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static int
RefNibble(int nib, int *pred, int *idx)
{
	int step = kStep[*idx];
	int diff = step >> 3;
	if(nib & 1) diff += step >> 2;
	if(nib & 2) diff += step >> 1;
	if(nib & 4) diff += step;
	*pred += nib & 8 ? -diff : diff;
	*pred = *pred < -32768 ? -32768 : *pred > 32767 ? 32767 : *pred;
	*idx += kIndex[nib & 7];
	*idx = *idx < 0 ? 0 : *idx > 88 ? 88 : *idx;
	return *pred;
}

static void
Put16(std::vector<uint8_t> &v, uint16_t x) { v.push_back(x & 0xff); v.push_back(x >> 8); }
static void
Put32(std::vector<uint8_t> &v, uint32_t x) { Put16(v, x & 0xffff); Put16(v, x >> 16); }

static std::vector<int16_t>
WriteIma(const char *path, int blocks, int extraBytes)
{
	const int align = 36, spb = (align - 4) * 2 + 1;
	std::vector<uint8_t> data;
	std::vector<int16_t> expect;
	unsigned seed = 99;
	for(int b = 0; b < blocks; b++){
		int pred = (b * 1000) - 500, idx = 10 + b;
		Put16(data, (uint16_t)(int16_t)pred);
		Put16(data, (uint16_t)idx);
		expect.push_back((int16_t)pred);
		for(int k = 0; k < align - 4; k++){
			seed = seed * 1103515245u + 12345u;
			uint8_t byte = (seed >> 16) & 0xff;
			data.push_back(byte);
			expect.push_back((int16_t)RefNibble(byte & 0xf, &pred, &idx));
			expect.push_back((int16_t)RefNibble(byte >> 4, &pred, &idx));
		}
	}
	for(int k = 0; k < extraBytes; k++)
		data.push_back(0x77);
	std::vector<uint8_t> f;
	f.insert(f.end(), { 'R', 'I', 'F', 'F' });
	Put32(f, 4 + 8 + 20 + 8 + (uint32_t)data.size());
	f.insert(f.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
	Put32(f, 20);
	Put16(f, 0x11); Put16(f, 1); Put32(f, 22050); Put32(f, 11100); Put16(f, align); Put16(f, 4); Put16(f, 2); Put16(f, spb);
	f.insert(f.end(), { 'd', 'a', 't', 'a' });
	Put32(f, (uint32_t)data.size());
	f.insert(f.end(), data.begin(), data.end());
	FILE *out = fopen(path, "wb");
	fwrite(f.data(), 1, f.size(), out);
	fclose(out);
	return expect;
}

static void
WriteFile(const char *path, uint16_t format, uint16_t channels, uint16_t align, uint16_t bits, const std::vector<uint8_t> &data)
{
	std::vector<uint8_t> f;
	f.insert(f.end(), { 'R', 'I', 'F', 'F' });
	Put32(f, 4 + 8 + 16 + 8 + (uint32_t)data.size());
	f.insert(f.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
	Put32(f, 16);
	Put16(f, format); Put16(f, channels); Put32(f, 32000); Put32(f, 32000 * align); Put16(f, align); Put16(f, bits);
	f.insert(f.end(), { 'd', 'a', 't', 'a' });
	Put32(f, (uint32_t)data.size());
	f.insert(f.end(), data.begin(), data.end());
	FILE *out = fopen(path, "wb");
	fwrite(f.data(), 1, f.size(), out);
	fclose(out);
}

static std::vector<int16_t>
WriteImaStereo(const char *path, int blocks)
{
	const int align = 72;
	std::vector<uint8_t> data;
	std::vector<int16_t> expect;
	unsigned seed = 7;
	for(int b = 0; b < blocks; b++){
		int pred[2] = { 300 * b - 2000, 4000 - 700 * b }, idx[2] = { 20 + b, 40 - b };
		for(int c = 0; c < 2; c++){
			Put16(data, (uint16_t)(int16_t)pred[c]);
			Put16(data, (uint16_t)idx[c]);
		}
		std::vector<int16_t> ch[2];
		for(int c = 0; c < 2; c++)
			ch[c].push_back((int16_t)pred[c]);
		for(int line = 0; line < 8; line++)
			for(int c = 0; c < 2; c++)
				for(int k = 0; k < 4; k++){
					seed = seed * 1103515245u + 12345u;
					uint8_t byte = (seed >> 16) & 0xff;
					data.push_back(byte);
					ch[c].push_back((int16_t)RefNibble(byte & 0xf, &pred[c], &idx[c]));
					ch[c].push_back((int16_t)RefNibble(byte >> 4, &pred[c], &idx[c]));
				}
		for(int i = 0; i < 65; i++){
			expect.push_back(ch[0][i]);
			expect.push_back(ch[1][i]);
		}
	}
	WriteFile(path, 0x11, 2, align, 4, data);
	return expect;
}

int
main(void)
{
	const char *path = "build/tests/ima_test.wav";
	std::vector<int16_t> expect = WriteIma(path, 3, 10);
	ImaWavDecoder d;
	CHECK(d.open(path));
	CHECK(d.rate() == 22050 && d.channels() == 1);
	CHECK(d.lengthFrames() == 3 * 65);
	std::vector<int16_t> got(400, 0);
	uint32_t n = d.readPcm16(got.data(), 400);
	CHECK(n == 3 * 65);
	bool same = n == expect.size();
	for(uint32_t i = 0; same && i < n; i++)
		same = got[i] == expect[i];
	CHECK(same);
	CHECK(d.seekFrame(100, true));
	n = d.readPcm16(got.data(), 10);
	CHECK(n == 10 && got[0] == expect[65]);
	float st[20];
	d.seekFrame(0, true);
	CHECK(d.read(st, 10) == 10 && st[0] == expect[0] / 32768.0f && st[1] == st[0]);
	CHECK(LengthMs(3 * 65, 22050) == (uint32_t)(float(195) * 1000.0f / float(22050)));
	std::vector<int16_t> stereo = WriteImaStereo("build/tests/ima_stereo_test.wav", 2);
	CHECK(d.open("build/tests/ima_stereo_test.wav"));
	CHECK(d.channels() == 2 && d.lengthFrames() == 2 * 65);
	n = d.readPcm16(got.data(), 200);
	CHECK(n == 2 * 65 && memcmp(got.data(), stereo.data(), stereo.size() * sizeof(int16_t)) == 0);
	d.seekFrame(65, true);
	CHECK(d.read(st, 2) == 2 && st[0] == stereo[130] / 32768.0f && st[1] == stereo[131] / 32768.0f && st[2] == stereo[132] / 32768.0f);
	std::vector<uint8_t> pcm;
	for(int i = 0; i < 2 * 5; i++)
		Put16(pcm, (uint16_t)(int16_t)(i * 1111 - 4000));
	pcm.push_back(0x55);
	WriteFile("build/tests/pcm_test.wav", 1, 2, 4, 16, pcm);
	CHECK(d.open("build/tests/pcm_test.wav"));
	CHECK(d.channels() == 2 && d.rate() == 32000 && d.lengthFrames() == 5);
	d.seekFrame(3, true);
	CHECK(d.read(st, 4) == 2 && st[0] == (3 * 2 * 1111 - 4000) / 32768.0f && st[3] == (4 * 2 * 1111 + 1111 - 4000) / 32768.0f);
	WriteFile("build/tests/pcm8_test.wav", 1, 1, 1, 8, pcm);
	CHECK(!d.open("build/tests/pcm8_test.wav"));
	CHECK(!d.open("build/tests/no_such_file.wav"));
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all ima tests passed\n");
	return 0;
}
