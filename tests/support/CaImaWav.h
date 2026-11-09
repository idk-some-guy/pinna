#pragma once

#include <stdio.h>
#include "CaDecoder.h"

namespace pinna {

class ImaWavDecoder : public Decoder
{
public:
	ImaWavDecoder(void);
	~ImaWavDecoder(void);
	bool open(const char *path);
	uint32_t rate(void) { return mRate; }
	uint32_t channels(void) { return mChannels; }
	uint64_t lengthFrames(void) { return mFrames; }
	uint64_t leadFrames(void) { return 0; }
	bool seekFrame(uint64_t frame, bool padded);
	uint32_t read(float *stereo, uint32_t frames);
	uint32_t readPcm16(int16_t *out, uint32_t frames);
private:
	void close(void);
	bool decodeBlock(void);
	FILE *mFile;
	bool mAdpcm;
	uint32_t mRate, mChannels, mBlockAlign, mSamplesPerBlock;
	uint32_t mDataStart, mDataSize;
	uint64_t mFrames;
	int16_t mBlock[2 * 4096];
	uint32_t mBlockFrames, mBlockPos;
	uint8_t mRaw[4096];
};

}
