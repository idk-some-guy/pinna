#ifdef AUDIO_CA
#import <AudioToolbox/AudioToolbox.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <algorithm>
#include "CaShell.h"
#ifdef CA_SHELL_OFFLINE
#include "CaShellOffline.h"
#endif

namespace ca {

struct XorReader
{
	FILE *file;
	SInt64 size;
};

static OSStatus
ReadXor(void *ref, SInt64 position, UInt32 request, void *buffer, UInt32 *actual)
{
	XorReader *x = (XorReader *)ref;
	if(fseeko(x->file, position, SEEK_SET) != 0){
		*actual = 0;
		return kAudioFilePositionError;
	}
	size_t n = fread(buffer, 1, request, x->file);
	uint8_t *b = (uint8_t *)buffer;
	for(size_t i = 0; i < n; i++)
		b[i] ^= 0x22;
	*actual = (UInt32)n;
	return noErr;
}

static SInt64
SizeXor(void *ref)
{
	return ((XorReader *)ref)->size;
}

static bool
HasExtension(const char *path, const char *ext)
{
	size_t n = strlen(path), e = strlen(ext);
	return n >= e && strcasecmp(path + n - e, ext) == 0;
}

static bool
OpenAudioFile(const char *path, XorReader *x, AudioFileID *file)
{
	if(HasExtension(path, ".adf")){
		x->file = fopen(path, "rb");
		if(x->file == NULL)
			return false;
		if(fseeko(x->file, 0, SEEK_END) != 0 || (x->size = ftello(x->file)) < 0)
			return false;
		return AudioFileOpenWithCallbacks(x, ReadXor, NULL, SizeXor, NULL, kAudioFileMP3Type, file) == noErr;
	}
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
	if(url == NULL)
		return false;
	OSStatus e = AudioFileOpenURL(url, kAudioFileReadPermission, 0, file);
	CFRelease(url);
	return e == noErr;
}

static const uint32_t kMpegDecoderDelay = 529;

static bool
ReadBytes(const char *path, XorReader *x, SInt64 position, uint8_t *buffer, UInt32 request, UInt32 *actual)
{
	if(x->file)
		return ReadXor(x, position, request, buffer, actual) == noErr;
	FILE *f = fopen(path, "rb");
	if(f == NULL)
		return false;
	bool ok = fseeko(f, position, SEEK_SET) == 0;
	*actual = ok ? (UInt32)fread(buffer, 1, request, f) : 0;
	fclose(f);
	return ok;
}

class AppleDecoder : public Decoder
{
public:
	AppleDecoder(void) : mFile(NULL), mExt(NULL), mRate(0), mChannels(0), mLength(0), mDelay(0), mPos(0), mPad(0), mQuantum(1)
	{
		mXor.file = NULL;
		mXor.size = 0;
	}
	~AppleDecoder(void)
	{
		if(mExt)
			ExtAudioFileDispose(mExt);
		if(mFile)
			AudioFileClose(mFile);
		if(mXor.file)
			fclose(mXor.file);
	}
	bool open(const char *path, bool corrections)
	{
		if(!OpenAudioFile(path, &mXor, &mFile) || ExtAudioFileWrapAudioFileID(mFile, false, &mExt) != noErr)
			return false;
		AudioStreamBasicDescription file;
		UInt32 size = sizeof(file);
		if(ExtAudioFileGetProperty(mExt, kExtAudioFileProperty_FileDataFormat, &size, &file) != noErr)
			return false;
		mRate = (uint32_t)file.mSampleRate;
		mChannels = file.mChannelsPerFrame;
		if(mChannels < 1 || mChannels > 2)
			return false;
		AudioStreamBasicDescription client;
		memset(&client, 0, sizeof(client));
		client.mSampleRate = file.mSampleRate;
		client.mFormatID = kAudioFormatLinearPCM;
		client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
		client.mChannelsPerFrame = mChannels;
		client.mBitsPerChannel = 32;
		client.mBytesPerFrame = 4 * mChannels;
		client.mFramesPerPacket = 1;
		client.mBytesPerPacket = 4 * mChannels;
		if(ExtAudioFileSetProperty(mExt, kExtAudioFileProperty_ClientDataFormat, sizeof(client), &client) != noErr)
			return false;
		SInt64 frames = 0;
		size = sizeof(frames);
		if(ExtAudioFileGetProperty(mExt, kExtAudioFileProperty_FileLengthFrames, &size, &frames) != noErr || frames <= 0)
			return false;
		mLength = (uint64_t)frames;
		if(corrections && (HasExtension(path, ".mp3") || HasExtension(path, ".adf")))
			correctMpeg(path);
		if(corrections && HasExtension(path, ".wav") && file.mFramesPerPacket > 0)
			mQuantum = file.mFramesPerPacket;
		return mLength > 0 && seekFrame(0, true);
	}
	uint32_t rate(void) { return mRate; }
	uint32_t channels(void) { return mChannels; }
	uint64_t lengthFrames(void) { return mLength; }
	uint64_t leadFrames(void) { return mDelay; }
	bool seekFrame(uint64_t frame, bool padded)
	{
		frame = frame / mQuantum * mQuantum;
		uint64_t delay = padded ? mDelay : 0;
		if(ExtAudioFileSeek(mExt, (SInt64)(frame < delay ? 0 : frame - delay)) != noErr){
			mPos = mLength;
			mPad = 0;
			return false;
		}
		mPos = frame;
		mPad = frame < delay ? delay - frame : 0;
		return true;
	}
	uint32_t read(float *stereo, uint32_t frames)
	{
		uint32_t done = 0;
		while(done < frames && mPos < mLength){
			uint64_t left = mLength - mPos;
			if(mPad){
				uint32_t n = (uint32_t)std::min<uint64_t>(std::min<uint64_t>(mPad, frames - done), left);
				memset(stereo + done * 2, 0, n * 2 * sizeof(float));
				done += n;
				mPos += n;
				mPad -= n;
				continue;
			}
			UInt32 want = (UInt32)std::min<uint64_t>(std::min<uint64_t>(frames - done, kChunk), left);
			AudioBufferList abl;
			abl.mNumberBuffers = 1;
			abl.mBuffers[0].mNumberChannels = mChannels;
			abl.mBuffers[0].mDataByteSize = want * mChannels * sizeof(float);
			abl.mBuffers[0].mData = mTmp;
			UInt32 got = want;
			if(ExtAudioFileRead(mExt, &got, &abl) != noErr || got == 0)
				break;
			for(UInt32 i = 0; i < got; i++){
				stereo[(done + i) * 2] = mTmp[i * mChannels];
				stereo[(done + i) * 2 + 1] = mTmp[i * mChannels + mChannels - 1];
			}
			done += got;
			mPos += got;
		}
		return done;
	}
private:
	enum { kChunk = 1024 };
	void correctMpeg(const char *path)
	{
		AudioFilePacketTableInfo table;
		UInt32 size = sizeof(table);
		if(AudioFileGetProperty(mFile, kAudioFilePropertyPacketTableInfo, &size, &table) != noErr){
			mDelay = kMpegDecoderDelay;
			return;
		}
		uint8_t head[4096];
		UInt32 n = 0;
		uint32_t frames = 0, samplesPerFrame = 0;
		if(ReadBytes(path, &mXor, 0, head, 10, &n) && ReadBytes(path, &mXor, Id3Size(head, n), head, sizeof(head), &n) &&
		   MpegInfoFrames(head, n, &frames, &samplesPerFrame))
			mLength = MpegCappedLength(mLength, MpegTrimmedFrames(frames, samplesPerFrame, table.mPrimingFrames, table.mRemainderFrames), samplesPerFrame);
	}
	XorReader mXor;
	AudioFileID mFile;
	ExtAudioFileRef mExt;
	uint32_t mRate, mChannels;
	uint64_t mLength, mDelay, mPos, mPad, mQuantum;
	float mTmp[kChunk * 2];
};

static Decoder *
OpenApple(const char *path, bool corrections)
{
	AppleDecoder *d = new AppleDecoder();
	if(!d->open(path, corrections)){
		delete d;
		return NULL;
	}
	return d;
}

Decoder *
OpenDecoder(const char *path)
{
	if(HasExtension(path, ".wav") || HasExtension(path, ".mp3") || HasExtension(path, ".adf"))
		return OpenApple(path, true);
	return NULL;
}

#ifdef CA_SHELL_OFFLINE
Decoder *
OpenAppleDecoderForTest(const char *path, bool corrections)
{
	return OpenApple(path, corrections);
}
#endif

uint32_t
ProbeLengthMs(const char *path)
{
	Decoder *d = OpenDecoder(path);
	uint32_t ms = d ? LengthMs(d->lengthFrames(), d->rate()) : 0;
	delete d;
	return ms;
}

uint32_t
ProbeUserTrackMs(const char *path)
{
	if(!HasExtension(path, ".wav") && !HasExtension(path, ".mp3") && !HasExtension(path, ".adf"))
		return 0;
	XorReader x = { NULL, 0 };
	AudioFileID file = NULL;
	Float64 seconds = 0.0;
	AudioStreamBasicDescription format;
	UInt32 size = sizeof(format);
	bool ok = OpenAudioFile(path, &x, &file) && AudioFileGetProperty(file, kAudioFilePropertyDataFormat, &size, &format) == noErr &&
		format.mChannelsPerFrame >= 1 && format.mChannelsPerFrame <= 2;
	size = sizeof(seconds);
	ok = ok && AudioFileGetProperty(file, kAudioFilePropertyEstimatedDuration, &size, &seconds) == noErr && seconds > 0.0;
	if(file)
		AudioFileClose(file);
	if(x.file)
		fclose(x.file);
	return ok ? (uint32_t)(seconds * 1000.0) : 0;
}

uint32_t
LoadStreamLengths(const char *cachePath, const char *const *paths, uint32_t count, uint32_t *lengths)
{
	FILE *f = fopen(cachePath, "rb");
	bool cached = ReadLengthCache(f, lengths, count);
	if(f)
		fclose(f);
	if(cached)
		return 0;
	bool complete = true;
	for(uint32_t i = 0; i < count; i++){
		lengths[i] = ProbeLengthMs(paths[i]);
		complete = complete && lengths[i] != 0;
	}
	if(!complete)
		return count;
	f = fopen(cachePath, "wb");
	WriteLengthCache(f, lengths, count);
	if(f)
		fclose(f);
	return count;
}

}
#endif
