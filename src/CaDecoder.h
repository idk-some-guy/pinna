#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

namespace ca {

// One thread uses a decoder at a time.
// A read shorter than asked means the stream has ended, at its end or on a decode or I/O error.
class Decoder
{
public:
	virtual ~Decoder(void) {}
	virtual uint32_t rate(void) = 0;
	virtual uint32_t channels(void) = 0;
	virtual uint64_t lengthFrames(void) = 0;
	virtual uint64_t leadFrames(void) = 0;
	virtual bool seekFrame(uint64_t frame, bool padded) = 0;
	virtual uint32_t read(float *stereo, uint32_t frames) = 0;
};

typedef Decoder *(*DecoderFactory)(const char *path);

struct DecodeFormat
{
	bool accepted;
	bool transformed;
	const char *decodeAs;
};

struct DecodeHooks
{
	DecodeFormat (*describe)(const char *path, void *ctx);
	void (*transform)(void *bytes, size_t n, uint64_t offset, void *ctx);
	void *ctx;
};

// Null restores the defaults: .wav and .mp3 accepted and plain. Refused with a logged line while a decoder is open.
bool SetDecodeHooks(const DecodeHooks *hooks);

inline bool
HasExtension(const char *path, const char *ext)
{
	size_t n = strlen(path), e = strlen(ext);
	return n >= e && strcasecmp(path + n - e, ext) == 0;
}

inline DecodeFormat
DefaultDescribe(const char *path)
{
	DecodeFormat f = { HasExtension(path, ".wav") || HasExtension(path, ".mp3"), false, NULL };
	return f;
}

inline DecodeFormat
HooksDescribe(const DecodeHooks &h, const char *path)
{
	return h.describe ? h.describe(path, h.ctx) : DefaultDescribe(path);
}

inline void
HooksTransform(const DecodeHooks &h, void *bytes, size_t n, uint64_t offset)
{
	if(h.transform)
		h.transform(bytes, n, offset, h.ctx);
}

inline uint32_t
LengthMs(uint64_t frames, uint32_t rate)
{
	return rate ? (uint32_t)(float(frames) * 1000.0f / float(rate)) : 0;
}

inline uint32_t
Id3Size(const uint8_t *h, size_t n)
{
	if(n < 10 || h[0] != 'I' || h[1] != 'D' || h[2] != '3')
		return 0;
	uint32_t size = (h[6] & 0x7f) << 21 | (h[7] & 0x7f) << 14 | (h[8] & 0x7f) << 7 | (h[9] & 0x7f);
	return 10 + size + (h[5] & 0x10 ? 10 : 0);
}

inline bool
MpegInfoFrames(const uint8_t *p, size_t n, uint32_t *frames, uint32_t *samplesPerFrame)
{
	if(n < 4 || p[0] != 0xff || (p[1] & 0xe0) != 0xe0)
		return false;
	int version = (p[1] >> 3) & 3, layer = (p[1] >> 1) & 3;
	if(version == 1 || layer != 1)
		return false;
	bool mono = (p[3] >> 6) == 3;
	size_t at = 4 + (version == 3 ? (mono ? 17 : 32) : (mono ? 9 : 17)) + ((p[1] & 1) ? 0 : 2);
	if(n < at + 12 || (memcmp(p + at, "Xing", 4) != 0 && memcmp(p + at, "Info", 4) != 0) || !(p[at + 7] & 1))
		return false;
	*frames = (uint32_t)p[at + 8] << 24 | p[at + 9] << 16 | p[at + 10] << 8 | p[at + 11];
	*samplesPerFrame = version == 3 ? 1152 : 576;
	return true;
}

inline uint64_t
MpegTrimmedFrames(uint32_t frames, uint32_t samplesPerFrame, uint32_t priming, uint32_t remainder)
{
	uint64_t total = (uint64_t)frames * samplesPerFrame;
	return total > (uint64_t)priming + remainder ? total - priming - remainder : 0;
}

inline uint64_t
MpegCappedLength(uint64_t length, uint64_t cap, uint32_t samplesPerFrame)
{
	return cap < length && length - cap <= 2ull * samplesPerFrame ? cap : length;
}

// Bump when the MP3 decoder delay, the Info-tag cap, the WAV length rules or the cache layout change.
static const uint32_t kLengthRulesVersion = 3;

struct LengthEntry
{
	uint32_t ms, reserved;
	uint64_t size;
	int64_t mtimeNs;
};

static_assert(sizeof(LengthEntry) == 24, "the cache stores 24 bytes per track");

inline bool
LengthEntryCurrent(const LengthEntry &e, uint64_t size, int64_t mtimeNs)
{
	return e.ms != 0 && e.size == size && e.mtimeNs == mtimeNs;
}

inline bool
ReadLengthCache(FILE *f, LengthEntry *entries, uint32_t count)
{
	uint32_t header[3];
	if(f == NULL || fseek(f, 0, SEEK_END) != 0 || ftell(f) != (long)sizeof(header) + (long)count * (long)sizeof(LengthEntry))
		return false;
	rewind(f);
	if(fread(header, sizeof(uint32_t), 3, f) != 3 || memcmp(header, "CALC", 4) != 0 || header[1] != kLengthRulesVersion || header[2] != count)
		return false;
	return fread(entries, sizeof(LengthEntry), count, f) == count;
}

inline bool
WriteLengthCache(FILE *f, const LengthEntry *entries, uint32_t count)
{
	uint32_t header[3] = { 0, kLengthRulesVersion, count };
	memcpy(header, "CALC", 4);
	return f != NULL && fwrite(header, sizeof(uint32_t), 3, f) == 3 && fwrite(entries, sizeof(LengthEntry), count, f) == count;
}

}
