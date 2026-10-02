#ifdef AUDIO_CA
#import <AudioToolbox/AudioToolbox.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "CaShell.h"

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

uint32_t
ProbeLengthMs(const char *path)
{
	XorReader x = { NULL, 0 };
	AudioFileID file = NULL;
	ExtAudioFileRef ext = NULL;
	uint32_t ms = 0;
	if(OpenAudioFile(path, &x, &file) && ExtAudioFileWrapAudioFileID(file, false, &ext) == noErr){
		AudioStreamBasicDescription fmt;
		UInt32 size = sizeof(fmt);
		SInt64 frames = 0;
		UInt32 fsize = sizeof(frames);
		if(ExtAudioFileGetProperty(ext, kExtAudioFileProperty_FileDataFormat, &size, &fmt) == noErr &&
		   ExtAudioFileGetProperty(ext, kExtAudioFileProperty_FileLengthFrames, &fsize, &frames) == noErr && frames > 0)
			ms = LengthMs((uint64_t)frames, (uint32_t)fmt.mSampleRate);
	}
	if(ext)
		ExtAudioFileDispose(ext);
	if(file)
		AudioFileClose(file);
	if(x.file)
		fclose(x.file);
	return ms;
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
