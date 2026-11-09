#ifdef PINNA_COREAUDIO
#import <AudioToolbox/AudioToolbox.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>
#include "CaShell.h"
#ifdef CA_SHELL_OFFLINE
#include "CaShellOffline.h"
#endif

namespace pinna {

struct TransformReader
{
	FILE *file;
	SInt64 size;
	void (*transform)(void *bytes, size_t n, uint64_t offset, void *ctx);
	void *ctx;
};

static std::mutex gHooksLock;
static DecodeHooks gHooks = { NULL, NULL, NULL };
static int gOpenDecoders;

static DecodeHooks
HooksNow(void)
{
	std::lock_guard<std::mutex> lock(gHooksLock);
	return gHooks;
}

static void
LogOnce(std::atomic<bool> &logged, const char *line)
{
	if(!logged.exchange(true))
		printf("%s %s\n", LogPrefix(), line);
}

bool
SetDecodeHooks(const DecodeHooks *hooks)
{
	static std::atomic<bool> logged(false);
	if(hooks && !hooks->describe){
		LogOnce(logged, "decode hooks have no describe, the installed hooks stay");
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(gHooksLock);
		if(gOpenDecoders == 0){
			gHooks = hooks ? *hooks : DecodeHooks{ NULL, NULL, NULL };
			return true;
		}
	}
	printf("%s decode hooks cannot change while a decoder is open, the installed hooks stay\n", LogPrefix());
	return false;
}

static OSStatus
ReadTransformed(void *ref, SInt64 position, UInt32 request, void *buffer, UInt32 *actual)
{
	TransformReader *r = (TransformReader *)ref;
	if(fseeko(r->file, position, SEEK_SET) != 0){
		*actual = 0;
		return kAudioFilePositionError;
	}
	size_t n = fread(buffer, 1, request, r->file);
	if(r->transform)
		r->transform(buffer, n, (uint64_t)position, r->ctx);
	*actual = (UInt32)n;
	return noErr;
}

static SInt64
SizeTransformed(void *ref)
{
	return ((TransformReader *)ref)->size;
}

static bool
OpensDirectly(const char *path)
{
	return HasExtension(path, ".m4a") || HasExtension(path, ".aac") || HasExtension(path, ".flac") || HasExtension(path, ".aif") ||
		HasExtension(path, ".aiff");
}

static AudioFileTypeID
ContentType(const char *ext)
{
	if(strcasecmp(ext, "mp3") == 0)
		return kAudioFileMP3Type;
	if(strcasecmp(ext, "wav") == 0)
		return kAudioFileWAVEType;
	return 0;
}

struct Source
{
	bool accepted, valid, transformed;
	const char *ext;
	AudioFileTypeID hint;
	void (*transform)(void *bytes, size_t n, uint64_t offset, void *ctx);
	void *ctx;
};

static Source
Describe(const DecodeHooks &hooks, const char *path)
{
	static std::atomic<bool> noTransform(false), unknownType(false), unknownPlainType(false);
	DecodeFormat f = HooksDescribe(hooks, path);
	const char *dot = strrchr(path, '.');
	Source s = { f.accepted, true, f.accepted && f.transformed, f.decodeAs ? f.decodeAs : dot ? dot + 1 : "", f.decodeAs ? ContentType(f.decodeAs) : 0, hooks.transform, hooks.ctx };
	if(s.transformed && !hooks.transform){
		LogOnce(noTransform, "a format is described as transformed but the hooks have no transform, its files are refused");
		s.valid = false;
	}
	else if(s.transformed && ContentType(s.ext) == 0){
		LogOnce(unknownType, "a format is described as decoded as a type the engine does not know, its files are refused");
		s.valid = false;
	}
	else if(s.accepted && !s.transformed && f.decodeAs && s.hint == 0){
		LogOnce(unknownPlainType, "a plain file is described as decoded as a type the engine does not know, its files are refused");
		s.valid = false;
	}
	return s;
}

static bool
IsAccepted(const Source &s)
{
	return s.accepted && s.valid;
}

static void
FloatClient(AudioStreamBasicDescription *client, Float64 rate, UInt32 channels)
{
	memset(client, 0, sizeof(*client));
	client->mSampleRate = rate;
	client->mFormatID = kAudioFormatLinearPCM;
	client->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
	client->mChannelsPerFrame = channels;
	client->mBitsPerChannel = 32;
	client->mBytesPerFrame = 4 * channels;
	client->mFramesPerPacket = 1;
	client->mBytesPerPacket = 4 * channels;
}

static bool
OpenAudioFile(const char *path, const Source &src, TransformReader *r, AudioFileID *file)
{
	if(src.transformed){
		r->file = fopen(path, "rb");
		if(r->file == NULL)
			return false;
		if(fseeko(r->file, 0, SEEK_END) != 0 || (r->size = ftello(r->file)) < 0)
			return false;
		r->transform = src.transform;
		r->ctx = src.ctx;
		if(AudioFileOpenWithCallbacks(r, ReadTransformed, NULL, SizeTransformed, NULL, ContentType(src.ext), file) != noErr)
			return false;
	}
	else{
		CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
		if(url == NULL)
			return false;
		OSStatus e = AudioFileOpenURL(url, kAudioFileReadPermission, src.hint, file);
		CFRelease(url);
		if(e != noErr)
			return false;
	}
	UInt32 hint = src.transformed ? ContentType(src.ext) : src.hint;
	UInt32 type = 0, size = sizeof(type);
	if(hint != 0 && (AudioFileGetProperty(*file, kAudioFilePropertyFileFormat, &size, &type) != noErr || type != hint)){
		AudioFileClose(*file);
		*file = NULL;
		return false;
	}
	return true;
}

static const uint32_t kMpegDecoderDelay = 529;

static bool
ReadBytes(const char *path, TransformReader *r, SInt64 position, uint8_t *buffer, UInt32 request, UInt32 *actual)
{
	if(r->file)
		return ReadTransformed(r, position, request, buffer, actual) == noErr;
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
		mReader.file = NULL;
		mReader.size = 0;
		mReader.transform = NULL;
		mReader.ctx = NULL;
		std::lock_guard<std::mutex> lock(gHooksLock);
		gOpenDecoders++;
	}
	~AppleDecoder(void)
	{
		if(mExt)
			ExtAudioFileDispose(mExt);
		if(mFile)
			AudioFileClose(mFile);
		if(mReader.file)
			fclose(mReader.file);
		std::lock_guard<std::mutex> lock(gHooksLock);
		gOpenDecoders--;
	}
	bool open(const char *path, bool corrections, const Source &src)
	{
		if(!src.valid || !OpenAudioFile(path, src, &mReader, &mFile) || ExtAudioFileWrapAudioFileID(mFile, false, &mExt) != noErr)
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
		FloatClient(&client, file.mSampleRate, mChannels);
		if(ExtAudioFileSetProperty(mExt, kExtAudioFileProperty_ClientDataFormat, sizeof(client), &client) != noErr)
			return false;
		SInt64 frames = 0;
		size = sizeof(frames);
		if(ExtAudioFileGetProperty(mExt, kExtAudioFileProperty_FileLengthFrames, &size, &frames) != noErr || frames <= 0)
			return false;
		mLength = (uint64_t)frames;
		if(corrections && strcasecmp(src.ext, "mp3") == 0)
			correctMpeg(path);
		if(corrections && strcasecmp(src.ext, "wav") == 0 && file.mFramesPerPacket > 0)
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
		if(ReadBytes(path, &mReader, 0, head, 10, &n) && ReadBytes(path, &mReader, Id3Size(head, n), head, sizeof(head), &n) &&
		   MpegInfoFrames(head, n, &frames, &samplesPerFrame))
			mLength = MpegCappedLength(mLength, MpegTrimmedFrames(frames, samplesPerFrame, table.mPrimingFrames, table.mRemainderFrames), samplesPerFrame);
	}
	TransformReader mReader;
	AudioFileID mFile;
	ExtAudioFileRef mExt;
	uint32_t mRate, mChannels;
	uint64_t mLength, mDelay, mPos, mPad, mQuantum;
	float mTmp[kChunk * 2];
};

static Decoder *
OpenApple(const char *path, bool corrections, const Source &src)
{
	AppleDecoder *d = new AppleDecoder();
	if(!d->open(path, corrections, src)){
		delete d;
		return NULL;
	}
	return d;
}

Decoder *
OpenDecoder(const char *path)
{
	Source src = Describe(HooksNow(), path);
	if(IsAccepted(src))
		return OpenApple(path, true, src);
	return NULL;
}

static uint32_t gUserTrackProbes;

#ifdef CA_SHELL_OFFLINE
uint32_t
UserTrackProbesForTest(void)
{
	return gUserTrackProbes;
}

Decoder *
OpenAppleDecoderForTest(const char *path, bool corrections)
{
	return OpenApple(path, corrections, Describe(HooksNow(), path));
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

static uint32_t
Verdict(bool *definitive, bool fromFile, uint32_t ms)
{
	if(definitive)
		*definitive = fromFile;
	return ms;
}

uint32_t
ProbeUserTrackMs(const char *path, bool *definitive)
{
	Source src = Describe(HooksNow(), path);
	bool direct = IsAccepted(src) && !src.transformed && OpensDirectly(path);
	if(direct && !HasExtension(path, ".aac")){
		AudioFileID file = NULL;
		AudioStreamBasicDescription format;
		AudioFilePacketTableInfo table;
		UInt64 packets = 0;
		UInt32 size = sizeof(format);
		TransformReader x = { NULL, 0, NULL, NULL };
		bool opened = OpenAudioFile(path, src, &x, &file) && AudioFileGetProperty(file, kAudioFilePropertyDataFormat, &size, &format) == noErr;
		bool ok = opened && format.mChannelsPerFrame >= 1 && format.mChannelsPerFrame <= 2 && format.mSampleRate > 0.0 && format.mFormatID != 'paac' &&
			format.mFormatID != 'drms';
		uint64_t frames = 0;
		size = sizeof(table);
		if(ok && AudioFileGetProperty(file, kAudioFilePropertyPacketTableInfo, &size, &table) == noErr && table.mNumberValidFrames > 0)
			frames = (uint64_t)table.mNumberValidFrames;
		else if(ok && format.mFormatID == kAudioFormatLinearPCM && format.mFramesPerPacket == 1){
			size = sizeof(packets);
			if(AudioFileGetProperty(file, kAudioFilePropertyAudioDataPacketCount, &size, &packets) == noErr)
				frames = packets;
		}
		if(file)
			AudioFileClose(file);
		if(!ok || frames > 0)
			return Verdict(definitive, opened, ok ? LengthMs(frames, (uint32_t)format.mSampleRate) : 0);
	}
	if(direct){
		Decoder *d = OpenApple(path, true, src);
		float first[64 * 2];
		uint32_t ms = d && d->read(first, 64) > 0 ? LengthMs(d->lengthFrames(), d->rate()) : 0;
		bool opened = d != NULL;
		delete d;
		return Verdict(definitive, opened, ms);
	}
	if(!IsAccepted(src))
		return Verdict(definitive, true, 0);
	TransformReader x = { NULL, 0, NULL, NULL };
	AudioFileID file = NULL;
	Float64 seconds = 0.0;
	AudioStreamBasicDescription format;
	UInt32 size = sizeof(format);
	bool opened = OpenAudioFile(path, src, &x, &file) && AudioFileGetProperty(file, kAudioFilePropertyDataFormat, &size, &format) == noErr;
	bool ok = opened && format.mChannelsPerFrame >= 1 && format.mChannelsPerFrame <= 2;
	size = sizeof(seconds);
	bool timed = ok && AudioFileGetProperty(file, kAudioFilePropertyEstimatedDuration, &size, &seconds) == noErr;
	if(file)
		AudioFileClose(file);
	if(x.file)
		fclose(x.file);
	return Verdict(definitive, opened && (!ok || timed), timed && seconds > 0.0 ? (uint32_t)(seconds * 1000.0) : 0);
}

static bool
FileStamp(const char *path, uint64_t *size, int64_t *mtimeNs)
{
	struct stat st;
	if(stat(path, &st) != 0){
		*size = 0;
		*mtimeNs = 0;
		return false;
	}
	*size = (uint64_t)st.st_size;
	*mtimeNs = (int64_t)st.st_mtimespec.tv_sec * 1000000000 + st.st_mtimespec.tv_nsec;
	return true;
}

uint32_t
UserTrackLengthMs(TrackLengthCache *cache, const char *path)
{
	uint64_t size;
	int64_t mtimeNs;
	uint32_t ms;
	bool definitive = true;
	bool stamped = FileStamp(path, &size, &mtimeNs);
	if(!stamped || !cache->find(path, size, mtimeNs, &ms)){
		ms = ProbeUserTrackMs(path, &definitive);
		gUserTrackProbes++;
	}
	if(stamped && (ms > 0 || definitive))
		cache->keep(path, size, mtimeNs, ms);
	return ms;
}

uint32_t
LoadStreamLengths(const char *cachePath, const char *const *paths, uint32_t count, uint32_t *lengths)
{
	std::vector<LengthEntry> entries(count);
	FILE *f = fopen(cachePath, "rb");
	bool cached = ReadLengthCache(f, entries.data(), count);
	if(f)
		fclose(f);
	uint32_t probed = 0;
	bool complete = true;
	for(uint32_t i = 0; i < count; i++){
		LengthEntry &e = entries[i];
		uint64_t size;
		int64_t mtimeNs;
		FileStamp(paths[i], &size, &mtimeNs);
		if(!cached || !LengthEntryCurrent(e, size, mtimeNs)){
			e.ms = ProbeLengthMs(paths[i]);
			e.reserved = 0;
			e.size = size;
			e.mtimeNs = mtimeNs;
			probed++;
		}
		lengths[i] = e.ms;
		complete = complete && e.ms != 0;
	}
	if(probed == 0 || !complete)
		return probed;
	f = fopen(cachePath, "wb");
	WriteLengthCache(f, entries.data(), count);
	if(f)
		fclose(f);
	return probed;
}

}
#endif
