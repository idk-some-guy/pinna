#include <string.h>
#include "CaImaWav.h"

namespace pinna {

static const uint16_t kStepTable[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
	73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
	494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
	2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
	10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

struct ImaState
{
	int16_t sample, index;
};

static int16_t
DecodeNibble(ImaState &s, uint8_t adpcm)
{
	uint16_t step = kStepTable[s.index];
	if(adpcm & 4)
		s.index += ((adpcm & 3) + 1) * 2;
	else
		s.index--;
	s.index = s.index < 0 ? 0 : s.index > 88 ? 88 : s.index;
	int delta = step >> 3;
	if(adpcm & 1) delta += step >> 2;
	if(adpcm & 2) delta += step >> 1;
	if(adpcm & 4) delta += step;
	if(adpcm & 8) delta = -delta;
	int v = s.sample + delta;
	s.sample = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
	return s.sample;
}

ImaWavDecoder::ImaWavDecoder(void) : mFile(NULL), mAdpcm(false), mRate(0), mChannels(0), mBlockAlign(0), mSamplesPerBlock(0),
	mDataStart(0), mDataSize(0), mFrames(0), mBlockFrames(0), mBlockPos(0)
{
}

ImaWavDecoder::~ImaWavDecoder(void)
{
	close();
}

void
ImaWavDecoder::close(void)
{
	if(mFile)
		fclose(mFile);
	mFile = NULL;
}

bool
ImaWavDecoder::open(const char *path)
{
	close();
	mFile = fopen(path, "rb");
	if(mFile == NULL)
		return false;
	uint8_t hdr[12];
	if(fread(hdr, 1, 12, mFile) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0){
		close();
		return false;
	}
	bool haveFmt = false;
	for(;;){
		uint8_t ck[8];
		if(fread(ck, 1, 8, mFile) != 8){
			close();
			return false;
		}
		uint32_t size = ck[4] | ck[5] << 8 | ck[6] << 16 | (uint32_t)ck[7] << 24;
		if(memcmp(ck, "fmt ", 4) == 0){
			uint8_t f[20] = {};
			if(size > 20 || fread(f, 1, size, mFile) != size){
				close();
				return false;
			}
			uint16_t format = f[0] | f[1] << 8;
			mChannels = f[2] | f[3] << 8;
			mRate = f[4] | f[5] << 8 | f[6] << 16 | (uint32_t)f[7] << 24;
			mBlockAlign = f[12] | f[13] << 8;
			uint16_t bits = f[14] | f[15] << 8;
			if(format == 0x11 || format == 0x69){
				mAdpcm = true;
				mSamplesPerBlock = (mBlockAlign / mChannels - 4) * 2 + 1;
			}else if(format == 1 && bits == 16){
				mAdpcm = false;
				mSamplesPerBlock = 1;
			}else{
				close();
				return false;
			}
			haveFmt = true;
		}else if(memcmp(ck, "data", 4) == 0){
			if(!haveFmt || mChannels < 1 || mChannels > 2 || mBlockAlign == 0 || mBlockAlign > sizeof(mRaw) || mSamplesPerBlock > 4096){
				close();
				return false;
			}
			mDataStart = (uint32_t)ftell(mFile);
			mDataSize = size;
			mFrames = (uint64_t)(size / mBlockAlign) * mSamplesPerBlock;
			mBlockFrames = mBlockPos = 0;
			return true;
		}else
			fseek(mFile, size, SEEK_CUR);
	}
}

bool
ImaWavDecoder::seekFrame(uint64_t frame, bool)
{
	if(mFile == NULL)
		return false;
	uint64_t block = frame / mSamplesPerBlock;
	mBlockFrames = mBlockPos = 0;
	return fseek(mFile, (long)(mDataStart + block * mBlockAlign), SEEK_SET) == 0;
}

bool
ImaWavDecoder::decodeBlock(void)
{
	long at = ftell(mFile);
	if(at < 0 || (uint32_t)at + mBlockAlign > mDataStart + mDataSize)
		return false;
	if(fread(mRaw, 1, mBlockAlign, mFile) != mBlockAlign)
		return false;
	if(!mAdpcm){
		memcpy(mBlock, mRaw, mBlockAlign);
		mBlockFrames = 1;
		mBlockPos = 0;
		return true;
	}
	ImaState st[2];
	const uint8_t *p = mRaw;
	for(uint32_t c = 0; c < mChannels; c++){
		st[c].sample = (int16_t)(p[0] | p[1] << 8);
		st[c].index = (int16_t)(p[2] | p[3] << 8);
		mBlock[c] = st[c].sample;
		p += 4;
	}
	uint32_t frame = 1;
	for(uint32_t s = 1; s < mSamplesPerBlock; s += 8){
		for(uint32_t c = 0; c < mChannels; c++){
			for(int b = 0; b < 4; b++){
				mBlock[(frame + b * 2) * mChannels + c] = DecodeNibble(st[c], p[b] & 0xf);
				mBlock[(frame + b * 2 + 1) * mChannels + c] = DecodeNibble(st[c], p[b] >> 4);
			}
			p += 4;
		}
		frame += 8;
	}
	mBlockFrames = mSamplesPerBlock;
	mBlockPos = 0;
	return true;
}

uint32_t
ImaWavDecoder::readPcm16(int16_t *out, uint32_t frames)
{
	uint32_t done = 0;
	while(done < frames){
		if(mBlockPos >= mBlockFrames && !decodeBlock())
			break;
		uint32_t n = mBlockFrames - mBlockPos;
		if(n > frames - done)
			n = frames - done;
		memcpy(out + done * mChannels, mBlock + mBlockPos * mChannels, n * mChannels * sizeof(int16_t));
		mBlockPos += n;
		done += n;
	}
	return done;
}

uint32_t
ImaWavDecoder::read(float *stereo, uint32_t frames)
{
	int16_t tmp[2 * 1024];
	uint32_t done = 0;
	while(done < frames){
		uint32_t want = frames - done < 1024 ? frames - done : 1024;
		uint32_t got = readPcm16(tmp, want);
		for(uint32_t i = 0; i < got; i++){
			float l = tmp[i * mChannels] / 32768.0f;
			float r = mChannels == 2 ? tmp[i * 2 + 1] / 32768.0f : l;
			stereo[(done + i) * 2] = l;
			stereo[(done + i) * 2 + 1] = r;
		}
		done += got;
		if(got < want)
			break;
	}
	return done;
}

}
