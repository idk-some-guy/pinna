#ifdef AUDIO_CA
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <string.h>
#include "CaShell.h"

namespace ca {

bool gShellOffline;

static Shared *sShared;
static Mixer *sMixer;
static AudioUnit sOutput, sSpatial;
static bool sSpatialBypass;
static double sRate;
static uint32_t sBuffer;
static uint32_t sRebuilds;
static Float64 sSampleTime;
static char sDeviceName[128];
static float sDryL[kMaxBlock], sDryR[kMaxBlock], sSend[kMaxBlock], sStrL[kMaxBlock], sStrR[kMaxBlock];
static const float *sFeed[2] = { sDryL, sDryR };

static AudioStreamBasicDescription
StereoFormat(double rate)
{
	AudioStreamBasicDescription f;
	memset(&f, 0, sizeof(f));
	f.mSampleRate = rate;
	f.mFormatID = kAudioFormatLinearPCM;
	f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
	f.mBytesPerPacket = 4;
	f.mFramesPerPacket = 1;
	f.mBytesPerFrame = 4;
	f.mChannelsPerFrame = 2;
	f.mBitsPerChannel = 32;
	return f;
}

static AudioUnit
MakeUnit(OSType type, OSType subType)
{
	AudioComponentDescription d = { type, subType, kAudioUnitManufacturer_Apple, 0, 0 };
	AudioComponent c = AudioComponentFindNext(NULL, &d);
	AudioUnit u = NULL;
	if(c == NULL || AudioComponentInstanceNew(c, &u) != noErr)
		return NULL;
	return u;
}

static void
DisposeUnit(AudioUnit *u)
{
	if(*u){
		AudioUnitUninitialize(*u);
		AudioComponentInstanceDispose(*u);
		*u = NULL;
	}
}

static OSStatus
FeedStereo(void *ref, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32 frames, AudioBufferList *io)
{
	const float *const *src = (const float *const *)ref;
	memcpy(io->mBuffers[0].mData, src[0], frames * sizeof(float));
	memcpy(io->mBuffers[1].mData, src[1], frames * sizeof(float));
	return noErr;
}

static bool
SetupSpatial(double rate)
{
	sSpatial = MakeUnit(kAudioUnitType_Mixer, kAudioUnitSubType_SpatialMixer);
	if(sSpatial == NULL)
		return false;
	UInt32 one = 1, maxFrames = kMaxBlock, bypass = kSpatialMixerSourceMode_Bypass, noFlags = 0;
	AudioStreamBasicDescription f = StereoFormat(rate);
	bool ok = AudioUnitSetProperty(sSpatial, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, &one, sizeof(one)) == noErr;
	ok = ok && AudioUnitSetProperty(sSpatial, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof(maxFrames)) == noErr;
	ok = ok && AudioUnitSetProperty(sSpatial, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof(f)) == noErr;
	ok = ok && AudioUnitSetProperty(sSpatial, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof(f)) == noErr;
	ok = ok && AudioUnitSetProperty(sSpatial, kAudioUnitProperty_SpatialMixerSourceMode, kAudioUnitScope_Input, 0, &bypass, sizeof(bypass)) == noErr;
	ok = ok && AudioUnitSetProperty(sSpatial, kAudioUnitProperty_SpatialMixerRenderingFlags, kAudioUnitScope_Input, 0, &noFlags, sizeof(noFlags)) == noErr;
	AURenderCallbackStruct cb = { FeedStereo, (void *)sFeed };
	ok = ok && AudioUnitSetProperty(sSpatial, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb)) == noErr;
	ok = ok && AudioUnitInitialize(sSpatial) == noErr;
	if(!ok)
		DisposeUnit(&sSpatial);
	return ok;
}

static bool
PullStereo(AudioUnit u, float *L, float *R, UInt32 frames)
{
	AudioTimeStamp ts;
	memset(&ts, 0, sizeof(ts));
	ts.mSampleTime = sSampleTime;
	ts.mFlags = kAudioTimeStampSampleTimeValid;
	struct { AudioBufferList list; AudioBuffer second; } abl;
	abl.list.mNumberBuffers = 2;
	abl.list.mBuffers[0].mNumberChannels = 1;
	abl.list.mBuffers[0].mDataByteSize = frames * 4;
	abl.list.mBuffers[0].mData = L;
	abl.second.mNumberChannels = 1;
	abl.second.mDataByteSize = frames * 4;
	abl.second.mData = R;
	AudioUnitRenderActionFlags flags = 0;
	return AudioUnitRender(u, &flags, &ts, 0, frames, &abl.list) == noErr;
}

static void
RenderBlock(float *L, float *R, UInt32 frames)
{
	sMixer->render(sDryL, sDryR, sSend, sStrL, sStrR, frames, (float)sRate);
	if(sSpatialBypass || sSpatial == NULL || !PullStereo(sSpatial, L, R, frames)){
		memcpy(L, sDryL, frames * sizeof(float));
		memcpy(R, sDryR, frames * sizeof(float));
	}
	for(UInt32 i = 0; i < frames; i++){
		L[i] += sStrL[i];
		R[i] += sStrR[i];
	}
	sSampleTime += frames;
}

static OSStatus
OutputRender(void *, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32 frames, AudioBufferList *io)
{
	float *L = (float *)io->mBuffers[0].mData;
	float *R = (float *)io->mBuffers[1].mData;
	for(UInt32 done = 0; done < frames;){
		UInt32 n = frames - done < (UInt32)kMaxBlock ? frames - done : (UInt32)kMaxBlock;
		RenderBlock(L + done, R + done, n);
		done += n;
	}
	return noErr;
}

static AudioObjectID
DefaultOutputDevice(void)
{
	AudioObjectPropertyAddress a = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
	AudioObjectID dev = kAudioObjectUnknown;
	UInt32 size = sizeof(dev);
	AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, &dev);
	return dev;
}

static void
ReadDeviceName(AudioObjectID dev, char *out, size_t size)
{
	AudioObjectPropertyAddress a = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
	CFStringRef name = NULL;
	UInt32 sz = sizeof(name);
	out[0] = '\0';
	if(AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &name) == noErr && name){
		CFStringGetCString(name, out, size, kCFStringEncodingUTF8);
		CFRelease(name);
	}
}

static bool
SetupSpatialOrBypass(double rate)
{
	DisposeUnit(&sSpatial);
	sSpatialBypass = !SetupSpatial(rate);
	if(sSpatialBypass)
		printf("sampman_ca: spatial mixer unavailable, classic stereo bypasses it\n");
	return true;
}

static bool
OpenOutput(AudioObjectID dev)
{
	sOutput = MakeUnit(kAudioUnitType_Output, kAudioUnitSubType_HALOutput);
	if(sOutput == NULL)
		return false;
	AudioObjectPropertyAddress rateAddr = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
	AudioObjectPropertyAddress bufAddr = { kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
	Float64 rate = 48000.0;
	UInt32 size = sizeof(rate), buffer = 512, maxFrames = kMaxBlock;
	AudioObjectGetPropertyData(dev, &rateAddr, 0, NULL, &size, &rate);
	size = sizeof(buffer);
	AudioObjectGetPropertyData(dev, &bufAddr, 0, NULL, &size, &buffer);
	AudioStreamBasicDescription f = StereoFormat(rate);
	AURenderCallbackStruct cb = { OutputRender, NULL };
	bool ok = AudioUnitSetProperty(sOutput, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof(dev)) == noErr;
	ok = ok && AudioUnitSetProperty(sOutput, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof(f)) == noErr;
	ok = ok && AudioUnitSetProperty(sOutput, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof(maxFrames)) == noErr;
	ok = ok && AudioUnitSetProperty(sOutput, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb)) == noErr;
	sRate = rate;
	sBuffer = buffer;
	ReadDeviceName(dev, sDeviceName, sizeof(sDeviceName));
	ok = ok && SetupSpatialOrBypass(rate);
	ok = ok && AudioUnitInitialize(sOutput) == noErr && AudioOutputUnitStart(sOutput) == noErr;
	if(!ok)
		DisposeUnit(&sOutput);
	return ok;
}

bool
ShellStart(Shared *shared, const char *)
{
	sShared = shared;
	sMixer = new Mixer(shared);
	sSampleTime = 0;
	AudioObjectID dev = DefaultOutputDevice();
	if(!OpenOutput(dev)){
		printf("sampman_ca: cannot open the output device\n");
		ShellStop();
		return false;
	}
	printf("sampman_ca: output \"%s\" %.0f Hz, buffer %u%s\n", sDeviceName, sRate, sBuffer, sSpatialBypass ? ", spatial mixer bypassed" : "");
	return true;
}

void
ShellStop(void)
{
	if(sOutput)
		AudioOutputUnitStop(sOutput);
	DisposeUnit(&sOutput);
	DisposeUnit(&sSpatial);
	delete sMixer;
	sMixer = NULL;
	sShared = NULL;
}

bool
ShellStartOffline(Shared *shared, double rate)
{
	sShared = shared;
	sMixer = new Mixer(shared);
	sRate = rate;
	sBuffer = 512;
	sSampleTime = 0;
	strcpy(sDeviceName, "offline");
	return SetupSpatialOrBypass(rate);
}

void
ShellRenderOffline(float *L, float *R, uint32_t frames)
{
	for(uint32_t done = 0; done < frames;){
		uint32_t n = frames - done < (uint32_t)kMaxBlock ? frames - done : (uint32_t)kMaxBlock;
		RenderBlock(L + done, R + done, n);
		done += n;
	}
}

bool
ShellPassThroughTest(const float *inL, const float *inR, float *outL, float *outR, uint32_t frames)
{
	if(sSpatial == NULL || frames > (uint32_t)kMaxBlock)
		return false;
	memcpy(sDryL, inL, frames * sizeof(float));
	memcpy(sDryR, inR, frames * sizeof(float));
	bool ok = PullStereo(sSpatial, outL, outR, frames);
	sSampleTime += frames;
	return ok;
}

ShellInfo
ShellGetInfo(void)
{
	ShellInfo i;
	i.rate = sRate;
	i.buffer = sBuffer;
	i.rebuilds = sRebuilds;
	i.spatialBypassed = sSpatialBypass;
	strncpy(i.device, sDeviceName, sizeof(i.device) - 1);
	i.device[sizeof(i.device) - 1] = '\0';
	return i;
}

}
#endif
