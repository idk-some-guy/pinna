#ifdef AUDIO_CA
#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <vector>
#include "CaShell.h"
#ifdef CA_SHELL_OFFLINE
#include <chrono>
#include <thread>
#include "CaShellOffline.h"
#endif
#include "CaRules.h"
#include "CaDevice.h"
#include "CaDrive.h"
#include "CaClock.h"
#include "OalPanTable.h"

namespace ca {

enum { kMaxDevices = 32, kSpatial3DInputs = 53, kBypassInput = 52, kSpatialPullFailLimit = 8 };

static Shared *sShared;
static Mixer *sMixer;
static AudioUnit sOutput, sSpatial, sReverb, sSpatial3D;
static bool sSpatialBypass, sSpatial3DUnavailable;
static SpatialRender sSpatialRender = { kSpatialUseOutputType };
static int32_t sSpatialAlgorithm = kSpatialUseOutputType;
static UInt32 sSpatialOutputType = kSpatialMixerOutputType_Headphones;
static float sSetAz[kChannels], sSetEl[kChannels];
static float sSilence[kMaxBlock];
static std::atomic<uint64_t> sSpatialFrames;
static std::atomic<bool> sSpatialFailed;
static uint32_t sFailedPulls;
static int32_t sHrtfStart = -1, sHrtfLater = -1;
static uint32_t sHrtfReadings, sHrtfArms;
static double sRate;
static uint32_t sBuffer;
static uint32_t sRebuilds;
static Float64 sSampleTime;
static char sDeviceName[128];
static float sDryL[kMaxBlock], sDryR[kMaxBlock], sSend[kMaxBlock], sStrL[kMaxBlock], sStrR[kMaxBlock];
static float sWetL[kMaxBlock], sWetR[kMaxBlock];
static const float *sFeed[2] = { sDryL, sDryR };
static const float *sFeed3D[2] = { sDryL, sDryR };
static UInt32 sChunkAt;
static float sSpatialCalib = kSpatialCalibHeadphones;
static const float *sSendFeed[2] = { sSend, sSend };

static Drive sDrive;
static Clock sClock;
static dispatch_queue_t sControl;
static bool sStopping, sSpatialWanted;
static std::mutex sInfoLock;
static DeviceEntry sDevices[kMaxDevices];
static int sNumDevices;
static uint32_t sListGeneration;
static bool sFellBack;
static uint32_t sDefaultId;
static char sPinned[128] = "default";
static AudioObjectID sCurrent;
static uint32_t sChannels;
static uint32_t sSession;
static AudioObjectPropertyListenerBlock sSystemListener, sDeviceListener;
static id sWakeObserver;

#ifdef CA_SHELL_OFFLINE
static ClockTime *sClockTime;
static bool sFake;
static DeviceEntry sFakeList[kMaxDevices];
static double sFakeRate[kMaxDevices];
static int sFakeCount;
static uint32_t sFakeDefault;
struct FakeRetry { int64_t due; uint32_t session; };
static std::vector<FakeRetry> sFakeRetries;
static bool sFailSpatialOnce, sFailSpatial3DOnce;
static std::atomic<uint32_t> sFailSpatial3DPulls;
static ClockTapFn sClockTap;
static void *sClockTapCtx;
#endif

struct ReverbParam
{
	AudioUnitParameterID id;
	AudioUnitParameterValue value;
};

static const float kOalLateTailTrimDb = -6.6f;

static const ReverbParam kCaveOnReverb2[] = {
	{ kReverb2Param_DryWetMix, 100.0f },
	{ kReverb2Param_Gain, kCaveRoomMb / 100.0f + kOalLateTailTrimDb },
	{ kReverb2Param_MinDelayTime, kCaveReflectionsDelay },
	{ kReverb2Param_MaxDelayTime, kCaveReflectionsDelay + kCaveLateDelay },
	{ kReverb2Param_DecayTimeAt0Hz, kCaveDecayTime },
	{ kReverb2Param_DecayTimeAtNyquist, kCaveDecayTime * kCaveDecayHFRatio },
};

static const AudioObjectPropertyAddress kSystemProps[] = {
	{ kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
	{ kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
};

static const AudioObjectPropertyAddress kDeviceProps[] = {
	{ kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
	{ kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain },
	{ kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
	{ kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
	{ kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
};

struct FlushDenormals
{
#if defined(__aarch64__)
	uint64_t saved;
	FlushDenormals(void) : saved(__builtin_arm_rsr64("fpcr")) { __builtin_arm_wsr64("fpcr", saved | (1ull << 24)); }
	~FlushDenormals(void) { __builtin_arm_wsr64("fpcr", saved); }
#endif
};

static AudioStreamBasicDescription
StereoFormat(double rate, UInt32 channels = 2)
{
	AudioStreamBasicDescription f;
	memset(&f, 0, sizeof(f));
	f.mSampleRate = rate;
	f.mFormatID = kAudioFormatLinearPCM;
	f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
	f.mBytesPerPacket = 4;
	f.mFramesPerPacket = 1;
	f.mBytesPerFrame = 4;
	f.mChannelsPerFrame = channels;
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

static OSStatus
FeedMono(void *ref, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32 frames, AudioBufferList *io)
{
	intptr_t c = (intptr_t)ref;
	memcpy(io->mBuffers[0].mData, c < kChannels ? sMixer->mono((int)c) + sChunkAt : sSilence, frames * sizeof(float));
	return noErr;
}

static bool
SetupSpatial(double rate)
{
#ifdef CA_SHELL_OFFLINE
	if(sFailSpatialOnce){
		sFailSpatialOnce = false;
		return false;
	}
#endif
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
SetupSpatial3D(double rate, int32_t algorithm, UInt32 outputType)
{
#ifdef CA_SHELL_OFFLINE
	if(sFailSpatial3DOnce){
		sFailSpatial3DOnce = false;
		return false;
	}
	UInt32 hrtf = kSpatialMixerPersonalizedHRTFMode_Off;
#else
	UInt32 hrtf = kSpatialMixerPersonalizedHRTFMode_Auto;
#endif
	sSpatial3D = MakeUnit(kAudioUnitType_Mixer, kAudioUnitSubType_SpatialMixer);
	if(sSpatial3D == NULL)
		return false;
	UInt32 inputs = kSpatial3DInputs, maxFrames = kMaxBlock, bypass = kSpatialMixerSourceMode_Bypass, noFlags = 0, off = 0;
	UInt32 flags = kSpatialMixerRenderingFlags_InterAuralDelay;
	UInt32 alg = algorithm == kSpatialHRTFHQ ? kSpatializationAlgorithm_HRTFHQ : kSpatializationAlgorithm_UseOutputType;
	AudioStreamBasicDescription stereo = StereoFormat(rate), mono = StereoFormat(rate, 1);
	bool ok = AudioUnitSetProperty(sSpatial3D, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, &inputs, sizeof(inputs)) == noErr;
	ok = ok && AudioUnitSetProperty(sSpatial3D, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof(maxFrames)) == noErr;
	ok = ok && AudioUnitSetProperty(sSpatial3D, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &stereo, sizeof(stereo)) == noErr;
	for(UInt32 b = 0; ok && b < kBypassInput; b++){
		AURenderCallbackStruct cb = { FeedMono, (void *)(intptr_t)b };
		ok = AudioUnitSetProperty(sSpatial3D, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, b, &mono, sizeof(mono)) == noErr;
		ok = ok && AudioUnitSetProperty(sSpatial3D, kAudioUnitProperty_SpatializationAlgorithm, kAudioUnitScope_Input, b, &alg, sizeof(alg)) == noErr;
		ok = ok && AudioUnitSetProperty(sSpatial3D, kAudioUnitProperty_SpatialMixerRenderingFlags, kAudioUnitScope_Input, b, &flags, sizeof(flags)) == noErr;
		ok = ok && AudioUnitSetProperty(sSpatial3D, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, b, &cb, sizeof(cb)) == noErr;
	}
	AURenderCallbackStruct cb = { FeedStereo, (void *)sFeed3D };
	ok = ok && AudioUnitSetProperty(sSpatial3D, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, kBypassInput, &stereo, sizeof(stereo)) == noErr;
	ok = ok && AudioUnitSetProperty(sSpatial3D, kAudioUnitProperty_SpatialMixerSourceMode, kAudioUnitScope_Input, kBypassInput, &bypass, sizeof(bypass)) == noErr;
	ok = ok && AudioUnitSetProperty(sSpatial3D, kAudioUnitProperty_SpatialMixerRenderingFlags, kAudioUnitScope_Input, kBypassInput, &noFlags, sizeof(noFlags)) == noErr;
	ok = ok && AudioUnitSetProperty(sSpatial3D, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, kBypassInput, &cb, sizeof(cb)) == noErr;
	ok = ok && AudioUnitSetProperty(sSpatial3D, kAudioUnitProperty_SpatialMixerOutputType, kAudioUnitScope_Global, 0, &outputType, sizeof(outputType)) == noErr;
	ok = ok && AudioUnitSetProperty(sSpatial3D, kAudioUnitProperty_UsesInternalReverb, kAudioUnitScope_Global, 0, &off, sizeof(off)) == noErr;
	if(ok)
		AudioUnitSetProperty(sSpatial3D, kAudioUnitProperty_SpatialMixerPersonalizedHRTFMode, kAudioUnitScope_Global, 0, &hrtf, sizeof(hrtf));
	ok = ok && AudioUnitInitialize(sSpatial3D) == noErr;
	for(UInt32 b = 0; ok && b < kBypassInput; b++){
		ok = AudioUnitSetParameter(sSpatial3D, kSpatialMixerParam_Distance, kAudioUnitScope_Input, b, 1.0f, 0) == noErr;
		ok = ok && AudioUnitSetParameter(sSpatial3D, kSpatialMixerParam_ReverbBlend, kAudioUnitScope_Input, b, 0.0f, 0) == noErr;
	}
	if(!ok){
		DisposeUnit(&sSpatial3D);
		return false;
	}
	sSpatialRender.algorithm = algorithm;
	sSpatialCalib = outputType == kSpatialMixerOutputType_BuiltInSpeakers ? kSpatialCalibBuiltInSpeakers : kSpatialCalibHeadphones;
	sFailedPulls = 0;
	for(int c = 0; c < kChannels; c++)
		sSetAz[c] = sSetEl[c] = NAN;
	return true;
}

static int32_t
ReadPersonalizedHrtf(void)
{
	UInt32 v = 0, size = sizeof(v);
	if(sSpatial3D == NULL || AudioUnitGetProperty(sSpatial3D, kAudioUnitProperty_SpatialMixerAnyInputIsUsingPersonalizedHRTF, kAudioUnitScope_Global, 0, &v, &size) != noErr)
		return -1;
	return (int32_t)v;
}

static void
SetupReverb(double rate)
{
	DisposeUnit(&sReverb);
	sReverb = MakeUnit(kAudioUnitType_Effect, kAudioUnitSubType_Reverb2);
	if(sReverb == NULL){
		printf("sampman_ca: reverb unavailable\n");
		return;
	}
	UInt32 maxFrames = kMaxBlock;
	AudioStreamBasicDescription f = StereoFormat(rate);
	AURenderCallbackStruct cb = { FeedStereo, (void *)sSendFeed };
	bool ok = AudioUnitSetProperty(sReverb, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof(maxFrames)) == noErr;
	ok = ok && AudioUnitSetProperty(sReverb, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof(f)) == noErr;
	ok = ok && AudioUnitSetProperty(sReverb, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof(f)) == noErr;
	ok = ok && AudioUnitSetProperty(sReverb, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb)) == noErr;
	ok = ok && AudioUnitInitialize(sReverb) == noErr;
	for(size_t i = 0; ok && i < sizeof(kCaveOnReverb2) / sizeof(kCaveOnReverb2[0]); i++)
		ok = AudioUnitSetParameter(sReverb, kCaveOnReverb2[i].id, kAudioUnitScope_Global, 0, kCaveOnReverb2[i].value, 0) == noErr;
	if(!ok){
		DisposeUnit(&sReverb);
		printf("sampman_ca: reverb unavailable\n");
	}
}

static bool
PullStereo(AudioUnit u, float *L, float *R, UInt32 frames, UInt32 at = 0)
{
	AudioTimeStamp ts;
	memset(&ts, 0, sizeof(ts));
	ts.mSampleTime = sSampleTime + at;
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
CountUnderrun(void)
{
	sShared->underruns.fetch_add(1, std::memory_order_relaxed);
}

static void
RenderBlock(float *L, float *R, UInt32 frames)
{
	bool spatial = sMixer->render(sDryL, sDryR, sSend, sStrL, sStrR, frames, (float)sRate, sSpatial3D && !sSpatialFailed.load(std::memory_order_relaxed) ? &sSpatialRender : NULL);
	if(spatial){
		UInt32 chunk = SpatialChunkFrames((float)sRate);
		bool failed = false;
		for(uint32_t k = 0; k < sMixer->chunks(); k++){
			UInt32 at = k * chunk, n = frames - at < chunk ? frames - at : chunk;
			for(int c = 0; c < kChannels; c++){
				Direction d = sMixer->direction(c, k);
				if(d.azimuth != sSetAz[c]){
					AudioUnitSetParameter(sSpatial3D, kSpatialMixerParam_Azimuth, kAudioUnitScope_Input, c, d.azimuth, 0);
					sSetAz[c] = d.azimuth;
				}
				if(d.elevation != sSetEl[c]){
					AudioUnitSetParameter(sSpatial3D, kSpatialMixerParam_Elevation, kAudioUnitScope_Input, c, d.elevation, 0);
					sSetEl[c] = d.elevation;
				}
			}
			sChunkAt = at;
			sFeed3D[0] = sDryL + at;
			sFeed3D[1] = sDryR + at;
			bool pulled = true;
#ifdef CA_SHELL_OFFLINE
			if(sFailSpatial3DPulls.load(std::memory_order_relaxed)){
				sFailSpatial3DPulls.fetch_sub(1, std::memory_order_relaxed);
				pulled = false;
			}
#endif
			if(!pulled || !PullStereo(sSpatial3D, L + at, R + at, n, at)){
				failed = true;
				memcpy(L + at, sDryL + at, n * sizeof(float));
				memcpy(R + at, sDryR + at, n * sizeof(float));
				float gl = kOalPanCentre[0] / sSpatialCalib, gr = kOalPanCentre[1] / sSpatialCalib;
				for(int c = 0; c < kChannels; c++){
					const float *m = sMixer->mono(c) + at;
					for(UInt32 i = 0; i < n; i++){
						L[at + i] += m[i] * gl;
						R[at + i] += m[i] * gr;
					}
				}
			}
		}
		sChunkAt = 0;
		if(failed){
			CountUnderrun();
			if(++sFailedPulls >= kSpatialPullFailLimit)
				sSpatialFailed.store(true, std::memory_order_relaxed);
		}else
			sFailedPulls = 0;
		sSpatialFrames.fetch_add(frames, std::memory_order_relaxed);
	}else{
		AudioUnit mix = sSpatialBypass ? NULL : sSpatial;
		if(mix == NULL || !PullStereo(mix, L, R, frames)){
			if(mix)
				CountUnderrun();
			memcpy(L, sDryL, frames * sizeof(float));
			memcpy(R, sDryR, frames * sizeof(float));
		}
	}
	if(sReverb){
		if(PullStereo(sReverb, sWetL, sWetR, frames))
			for(UInt32 i = 0; i < frames; i++){
				L[i] += sWetL[i];
				R[i] += sWetR[i];
			}
		else
			CountUnderrun();
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
	int e = sDrive.EnterUnit();
	if(e != Drive::RENDER){
		memset(L, 0, frames * sizeof(float));
		memset(R, 0, frames * sizeof(float));
		if(e == Drive::BLOCKED)
			CountUnderrun();
		return noErr;
	}
	FlushDenormals ftz;
	for(UInt32 done = 0; done < frames;){
		UInt32 n = frames - done < (UInt32)kMaxBlock ? frames - done : (UInt32)kMaxBlock;
		RenderBlock(L + done, R + done, n);
		done += n;
	}
	sDrive.LeaveUnit(frames);
	return noErr;
}

static void
ClockRender(void *, uint32_t frames, double rate)
{
	FlushDenormals ftz;
	for(uint32_t done = 0; done < frames;){
		uint32_t n = frames - done < (uint32_t)kMaxBlock ? frames - done : (uint32_t)kMaxBlock;
		sMixer->render(sDryL, sDryR, sSend, sStrL, sStrR, n, (float)rate);
#ifdef CA_SHELL_OFFLINE
		if(sClockTap)
			sClockTap(sClockTapCtx, sDryL, sDryR, sSend, sStrL, sStrR, n, rate);
#endif
		done += n;
	}
}

static AudioObjectID
DefaultOutputDevice(void)
{
	AudioObjectID dev = kAudioObjectUnknown;
	UInt32 size = sizeof(dev);
	AudioObjectGetPropertyData(kAudioObjectSystemObject, &kSystemProps[0], 0, NULL, &size, &dev);
	return dev;
}

static void
ReadDeviceName(AudioObjectID dev, char *out, size_t size)
{
	out[0] = '\0';
#ifdef CA_SHELL_OFFLINE
	if(sFake){
		for(int i = 0; i < sFakeCount; i++)
			if(sFakeList[i].id == dev)
				strlcpy(out, sFakeList[i].name, size);
		return;
	}
#endif
	AudioObjectPropertyAddress a = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
	CFStringRef name = NULL;
	UInt32 sz = sizeof(name);
	if(AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &name) == noErr && name){
		CFStringGetCString(name, out, size, kCFStringEncodingUTF8);
		CFRelease(name);
	}
}

static bool
ReadDevice(AudioObjectID dev, DeviceEntry &e)
{
	memset(&e, 0, sizeof(e));
	e.id = dev;
	AudioObjectPropertyAddress a = { kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
	UInt32 size = 0;
	if(AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &size) != noErr || size == 0)
		return false;
	AudioBufferList *list = (AudioBufferList *)malloc(size);
	if(list && AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, list) == noErr)
		for(UInt32 i = 0; i < list->mNumberBuffers; i++)
			e.outputChannels += list->mBuffers[i].mNumberChannels;
	free(list);
	if(e.outputChannels == 0)
		return false;
	a.mSelector = kAudioDevicePropertyDeviceUID;
	a.mScope = kAudioObjectPropertyScopeGlobal;
	CFStringRef uid = NULL;
	size = sizeof(uid);
	if(AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &uid) == noErr && uid){
		CFStringGetCString(uid, e.uid, sizeof(e.uid), kCFStringEncodingUTF8);
		CFRelease(uid);
	}
	char name[128];
	ReadDeviceName(dev, name, sizeof(name));
	MenuName(name, e.name, sizeof(e.name));
	a.mSelector = kAudioDevicePropertyDeviceIsAlive;
	UInt32 alive = 0;
	size = sizeof(alive);
	AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &alive);
	e.alive = alive != 0;
	return true;
}

static int
ReadDeviceList(DeviceEntry *out, uint32_t *defaultId)
{
#ifdef CA_SHELL_OFFLINE
	if(sFake){
		for(int i = 0; i < sFakeCount; i++){
			out[i] = sFakeList[i];
			MenuName(sFakeList[i].name, out[i].name, sizeof(out[i].name));
		}
		*defaultId = sFakeDefault;
		return sFakeCount;
	}
#endif
	*defaultId = DefaultOutputDevice();
	UInt32 size = 0;
	if(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &kSystemProps[1], 0, NULL, &size) != noErr)
		return 0;
	std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
	if(ids.empty() || AudioObjectGetPropertyData(kAudioObjectSystemObject, &kSystemProps[1], 0, NULL, &size, ids.data()) != noErr)
		return 0;
	int n = 0;
	for(UInt32 i = 0; i < size / sizeof(AudioObjectID) && n < kMaxDevices; i++)
		if(ReadDevice(ids[i], out[n]))
			n++;
	return n;
}

static void
RefreshDevices(void)
{
	DeviceEntry list[kMaxDevices];
	uint32_t def = kAudioObjectUnknown;
	int n = ReadDeviceList(list, &def);
	std::lock_guard<std::mutex> lock(sInfoLock);
	memcpy(sDevices, list, n * sizeof(DeviceEntry));
	sNumDevices = n;
	sDefaultId = def;
	sListGeneration++;
}

static uint32_t
ClockBlock(uint32_t buffer)
{
	return buffer < 64 ? 64 : buffer > (uint32_t)kMaxBlock ? (uint32_t)kMaxBlock : buffer;
}

static void
OutputFormat(double rate, uint32_t buffer, uint32_t latency, const char *device)
{
	{
		std::lock_guard<std::mutex> lock(sInfoLock);
		sRate = rate;
		sBuffer = buffer;
		strlcpy(sDeviceName, device, sizeof(sDeviceName));
	}
	if(sShared)
		sShared->streamLatency.store(latency, std::memory_order_relaxed);
	sClock.SetFormat(rate, ClockBlock(buffer));
}

static void
BufferSizeChanged(uint32_t buffer, uint32_t latency)
{
	{
		std::lock_guard<std::mutex> lock(sInfoLock);
		sBuffer = buffer;
	}
	if(sShared)
		sShared->streamLatency.store(latency, std::memory_order_relaxed);
	sClock.SetFormat(sRate, ClockBlock(buffer));
}

static uint32_t
DeviceLatency(AudioObjectID dev)
{
	UInt32 total = 0;
	AudioObjectPropertySelector sel[2] = { kAudioDevicePropertyLatency, kAudioDevicePropertySafetyOffset };
	for(int i = 0; i < 2; i++){
		AudioObjectPropertyAddress a = { sel[i], kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
		UInt32 v = 0, size = sizeof(v);
		if(AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &v) == noErr)
			total += v;
	}
	AudioObjectPropertyAddress sa = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
	AudioStreamID streams[16];
	UInt32 size = 0;
	if(AudioObjectGetPropertyDataSize(dev, &sa, 0, NULL, &size) != noErr || size < sizeof(AudioStreamID))
		return total;
	if(size > sizeof(streams))
		size = sizeof(streams);
	if(AudioObjectGetPropertyData(dev, &sa, 0, NULL, &size, streams) != noErr || size < sizeof(AudioStreamID))
		return total;
	AudioObjectPropertyAddress la = { kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
	UInt32 v = 0;
	size = sizeof(v);
	if(AudioObjectGetPropertyData(streams[0], &la, 0, NULL, &size, &v) == noErr)
		total += v;
	return total;
}

static void Rebuild(int event, uint32_t eventId, const char *why);

static uint32_t
CurrentChannels(AudioObjectID dev)
{
	for(int i = 0; i < sNumDevices; i++)
		if(sDevices[i].id == dev)
			return sDevices[i].outputChannels;
	return 0;
}

static void
DeviceEvent(AudioObjectID dev, UInt32 n, const AudioObjectPropertyAddress *a)
{
	if(sStopping)
		return;
	bool gone = false, format = false, buffer = false;
	for(UInt32 i = 0; i < n; i++)
		switch(a[i].mSelector){
		case kAudioDevicePropertyBufferFrameSize:
			buffer = true;
			break;
		case kAudioDeviceProcessorOverload:
			if(dev == sCurrent)
				CountUnderrun();
			break;
		case kAudioDevicePropertyNominalSampleRate:
		case kAudioDevicePropertyStreamConfiguration:
			format = true;
			break;
		case kAudioDevicePropertyDeviceIsAlive: {
			UInt32 alive = 0, size = sizeof(alive);
			if(AudioObjectGetPropertyData(dev, &kDeviceProps[2], 0, NULL, &size, &alive) != noErr || !alive)
				gone = true;
			break;
		}
		}
	if(buffer && !gone && !format && dev == sCurrent){
		UInt32 frames = 0, size = sizeof(frames);
		if(AudioObjectGetPropertyData(dev, &kDeviceProps[4], 0, NULL, &size, &frames) == noErr && frames)
			BufferSizeChanged(frames, 2 * frames + DeviceLatency(dev));
	}
	if(!gone && !format)
		return;
	Float64 rate = 0.0;
	UInt32 size = sizeof(rate);
	DeviceEntry e;
	if(!gone){
		AudioObjectGetPropertyData(dev, &kDeviceProps[0], 0, NULL, &size, &rate);
		ReadDevice(dev, e);
	}
	int event = DeviceBatchEvent(gone, format, sRate, sChannels, rate, gone ? 0 : e.outputChannels);
	if(event != EVENT_NONE)
		Rebuild(event, dev, gone ? "device lost" : rate != sRate ? "rate changed" : "format changed");
}

static void
WatchDevice(AudioObjectID dev)
{
#ifdef CA_SHELL_OFFLINE
	if(sFake)
		return;
#endif
	sDeviceListener = ^(UInt32 n, const AudioObjectPropertyAddress *a) { DeviceEvent(dev, n, a); };
	for(size_t i = 0; i < sizeof(kDeviceProps) / sizeof(kDeviceProps[0]); i++)
		AudioObjectAddPropertyListenerBlock(dev, &kDeviceProps[i], sControl, sDeviceListener);
}

static void
UnwatchDevice(AudioObjectID dev)
{
	if(sDeviceListener == nil)
		return;
	for(size_t i = 0; i < sizeof(kDeviceProps) / sizeof(kDeviceProps[0]); i++)
		AudioObjectRemovePropertyListenerBlock(dev, &kDeviceProps[i], sControl, sDeviceListener);
	sDeviceListener = nil;
}

static void
StopOutput(void)
{
	sDrive.ToClock();
	if(sOutput)
		AudioOutputUnitStop(sOutput);
	sDrive.WaitUnitIdle();
	UnwatchDevice(sCurrent);
	DisposeUnit(&sOutput);
	DisposeUnit(&sSpatial);
	DisposeUnit(&sSpatial3D);
	DisposeUnit(&sReverb);
	sCurrent = kAudioObjectUnknown;
}

static bool
SetupSpatialOrBypass(double rate)
{
	DisposeUnit(&sSpatial);
	bool bypass = !SetupSpatial(rate);
	{
		std::lock_guard<std::mutex> lock(sInfoLock);
		sSpatialBypass = bypass;
	}
	if(bypass)
		printf("sampman_ca: spatial mixer unavailable, classic stereo bypasses it\n");
	DisposeUnit(&sSpatial3D);
	bool unavailable = !SetupSpatial3D(rate, sSpatialAlgorithm, sSpatialOutputType);
	sSpatialFrames.store(0, std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(sInfoLock);
		sSpatial3DUnavailable = unavailable;
	}
	if(unavailable)
		printf("sampman_ca: spatial mixer for positioned voices unavailable, spatial mode uses classic routing\n");
	SetupReverb(rate);
	return true;
}

static void
CheckHrtfLater(uint32_t session, uint32_t readings)
{
	if(sStopping || session != sSession || !sSpatialWanted)
		return;
	{
		std::lock_guard<std::mutex> lock(sInfoLock);
		if(readings != sHrtfReadings)
			return;
	}
	if(sSpatial3D && sSpatialFrames.load(std::memory_order_relaxed) >= (uint64_t)sRate){
		int32_t v = ReadPersonalizedHrtf();
		std::lock_guard<std::mutex> lock(sInfoLock);
		sHrtfLater = v;
		sHrtfReadings++;
		return;
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), sControl, ^{ CheckHrtfLater(session, readings); });
}

static void
ArmHrtfReading(void)
{
	if(!sSpatialWanted || sSpatial3D == NULL || sCurrent == kAudioObjectUnknown)
		return;
	int32_t hrtf = ReadPersonalizedHrtf();
	uint32_t session = sSession, readings;
	{
		std::lock_guard<std::mutex> lock(sInfoLock);
		sHrtfStart = hrtf;
		sHrtfLater = -1;
		readings = ++sHrtfReadings;
		sHrtfArms++;
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), sControl, ^{ CheckHrtfLater(session, readings); });
}

static bool
OpenOutput(AudioObjectID dev)
{
	char name[128];
	ReadDeviceName(dev, name, sizeof(name));
#ifdef CA_SHELL_OFFLINE
	if(sFake){
		double rate = 0.0;
		for(int i = 0; i < sFakeCount; i++)
			if(sFakeList[i].id == dev && sFakeList[i].alive)
				rate = sFakeRate[i];
		if(rate <= 0.0)
			return false;
		SetupSpatialOrBypass(rate);
		OutputFormat(rate, 512, 2 * 512, name);
		sCurrent = dev;
		sChannels = CurrentChannels(dev);
		sDrive.Arm();
		ArmHrtfReading();
		return true;
	}
#endif
	sOutput = MakeUnit(kAudioUnitType_Output, kAudioUnitSubType_HALOutput);
	if(sOutput == NULL)
		return false;
	Float64 rate = kFallbackRate;
	UInt32 size = sizeof(rate), buffer = 512, maxFrames = kMaxBlock;
	AudioObjectGetPropertyData(dev, &kDeviceProps[0], 0, NULL, &size, &rate);
	AudioObjectPropertyAddress bufAddr = { kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
	size = sizeof(buffer);
	AudioObjectGetPropertyData(dev, &bufAddr, 0, NULL, &size, &buffer);
	AudioStreamBasicDescription f = StereoFormat(rate);
	AURenderCallbackStruct cb = { OutputRender, NULL };
	bool ok = AudioUnitSetProperty(sOutput, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof(dev)) == noErr;
	ok = ok && AudioUnitSetProperty(sOutput, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof(f)) == noErr;
	ok = ok && AudioUnitSetProperty(sOutput, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof(maxFrames)) == noErr;
	ok = ok && AudioUnitSetProperty(sOutput, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb)) == noErr;
	ok = ok && SetupSpatialOrBypass(rate);
	ok = ok && AudioUnitInitialize(sOutput) == noErr;
	if(ok){
		OutputFormat(rate, buffer, 2 * buffer + DeviceLatency(dev), name);
		sCurrent = dev;
		sChannels = CurrentChannels(dev);
		sDrive.Arm();
		ok = AudioOutputUnitStart(sOutput) == noErr;
	}
	if(!ok){
		StopOutput();
		return false;
	}
	WatchDevice(dev);
	ArmHrtfReading();
	return true;
}

static void Rebuild(int event, uint32_t eventId, const char *why);

static void
Retry(uint32_t session)
{
	if(session == sSession)
		Rebuild(EVENT_RETRY, 0, "retry");
}

static void
ScheduleRetry(void)
{
	uint32_t session = sSession;
#ifdef CA_SHELL_OFFLINE
	if(sClockTime){
		sFakeRetries.push_back(FakeRetry{ sClockTime->Now() + (int64_t)NSEC_PER_SEC, session });
		return;
	}
#endif
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), sControl, ^{ Retry(session); });
}

static void
Rebuild(int event, uint32_t eventId, const char *why)
{
	if(sStopping)
		return;
	RefreshDevices();
	if(!DecideRebuild(event, sPinned, sDevices, sNumDevices, sCurrent, eventId))
		return;
	StopOutput();
	DeviceChoice c = ChooseDevice(sPinned, sDevices, sNumDevices, sDefaultId);
	bool ok = c.id != kAudioObjectUnknown && OpenOutput(c.id);
	if(!ok && c.id != kAudioObjectUnknown){
		char name[128];
		ReadDeviceName(c.id, name, sizeof(name));
		printf("sampman_ca: output failed on \"%s\"%s\n", name, c.id != sDefaultId ? ", using the default" : "");
		if(c.id != sDefaultId && sDefaultId != kAudioObjectUnknown){
			ok = OpenOutput(sDefaultId);
			c.fellBack = true;
		}
	}
	if(!ok)
		OutputFormat(sRate, sBuffer, 0, "none");
	if(!ok && c.id != kAudioObjectUnknown && event != EVENT_RETRY)
		ScheduleRetry();
	{
		std::lock_guard<std::mutex> lock(sInfoLock);
		sFellBack = !FollowsDefault(sPinned) && (c.fellBack || !ok);
		if(event != EVENT_START)
			sRebuilds++;
	}
	if(!ok)
		printf("sampman_ca: no output device (%s), a silent clock drives the engine at %.0f Hz\n", why, sRate);
	else if(event == EVENT_START)
		printf("sampman_ca: output \"%s\" %.0f Hz, buffer %u%s%s\n", sDeviceName, sRate, sBuffer, sSpatialBypass ? ", spatial mixer bypassed" : "",
			c.fellBack ? ", pinned device missing, using the default" : "");
	else
		printf("sampman_ca: output rebuilt (%s): \"%s\" %.0f Hz, buffer %u%s\n", why, sDeviceName, sRate, sBuffer,
			c.fellBack ? ", pinned device missing, using the default" : "");
}

static void
EnsureControl(void)
{
	if(sControl == nil)
		sControl = dispatch_queue_create("reVC.audio.control", DISPATCH_QUEUE_SERIAL);
}

static void
WatchSystem(void)
{
#ifdef CA_SHELL_OFFLINE
	if(sFake)
		return;
#endif
	sSystemListener = ^(UInt32 n, const AudioObjectPropertyAddress *a) {
		for(UInt32 i = 0; i < n; i++)
			if(a[i].mSelector == kAudioHardwarePropertyDefaultOutputDevice)
				Rebuild(EVENT_DEFAULT_CHANGED, DefaultOutputDevice(), "default output changed");
			else if(a[i].mSelector == kAudioHardwarePropertyDevices)
				Rebuild(EVENT_DEVICES_CHANGED, 0, "devices changed");
	};
	for(size_t i = 0; i < sizeof(kSystemProps) / sizeof(kSystemProps[0]); i++)
		AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &kSystemProps[i], sControl, sSystemListener);
}

static void
WatchWake(void)
{
#ifdef CA_SHELL_OFFLINE
	if(sFake)
		return;
#endif
	sWakeObserver = [[[NSWorkspace sharedWorkspace] notificationCenter] addObserverForName:NSWorkspaceDidWakeNotification object:nil queue:nil usingBlock:^(NSNotification *) {
		dispatch_async(sControl, ^{ Rebuild(EVENT_WAKE, 0, "wake"); });
	}];
}

static void
UnwatchWake(void)
{
	if(sWakeObserver){
		[[[NSWorkspace sharedWorkspace] notificationCenter] removeObserver:sWakeObserver];
		sWakeObserver = nil;
	}
}

static void
UnwatchSystem(void)
{
	if(sSystemListener){
		for(size_t i = 0; i < sizeof(kSystemProps) / sizeof(kSystemProps[0]); i++)
			AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &kSystemProps[i], sControl, sSystemListener);
		sSystemListener = nil;
	}
}

bool
ShellStart(Shared *shared, const char *deviceUid)
{
#ifdef CA_SHELL_OFFLINE
	if(gShellOffline)
		return ShellStartOffline(shared, 48000.0);
	if(!sFake){
		printf("sampman_ca: the offline tools never open a real device\n");
		return false;
	}
#endif
	sShared = shared;
	sMixer = new Mixer(shared);
	sSampleTime = 0;
	sSpatialFailed.store(false, std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(sInfoLock);
		sRebuilds = 0;
	}
	OutputFormat(kFallbackRate, 512, 0, "none");
	sDrive.ToClock();
#ifdef CA_SHELL_OFFLINE
	sClock.Start(&sDrive, ClockRender, NULL, kFallbackRate, 512, sClockTime);
#else
	sClock.Start(&sDrive, ClockRender, NULL, kFallbackRate, 512);
#endif
	EnsureControl();
	const char *pin = deviceUid && deviceUid[0] ? deviceUid : "default";
	dispatch_sync(sControl, ^{
		sStopping = false;
		sSpatialWanted = false;
		sSession++;
		strlcpy(sPinned, pin, sizeof(sPinned));
		WatchSystem();
		Rebuild(EVENT_START, 0, "start");
	});
	WatchWake();
	return true;
}

void
ShellStop(void)
{
	UnwatchWake();
	if(sControl)
		dispatch_sync(sControl, ^{
			sStopping = true;
			sSession++;
			UnwatchSystem();
			StopOutput();
		});
	sDrive.Halt();
	sDrive.WaitIdle();
	sClock.Stop();
	if(sOutput)
		AudioOutputUnitStop(sOutput);
	DisposeUnit(&sOutput);
	DisposeUnit(&sSpatial);
	DisposeUnit(&sSpatial3D);
	DisposeUnit(&sReverb);
	delete sMixer;
	sMixer = NULL;
	sShared = NULL;
}

void
ShellSetSpatial(bool on)
{
	if(sControl == nil)
		return;
	dispatch_async(sControl, ^{
		if(sStopping || sSpatialWanted == on)
			return;
		sSpatialWanted = on;
		ArmHrtfReading();
	});
}

void
ShellSetDevice(const char *uid)
{
	if(sControl == nil)
		return;
	struct Pin { char uid[128]; } pin;
	strlcpy(pin.uid, uid && uid[0] ? uid : "default", sizeof(pin.uid));
	dispatch_async(sControl, ^{
		if(sStopping)
			return;
		strlcpy(sPinned, pin.uid, sizeof(sPinned));
		Rebuild(EVENT_PIN, 0, "device chosen");
	});
}

int
ShellDeviceCount(void)
{
	std::lock_guard<std::mutex> lock(sInfoLock);
	return sNumDevices;
}

bool
ShellDeviceAt(int index, char *name, size_t nameSize, char *uid, size_t uidSize)
{
	std::lock_guard<std::mutex> lock(sInfoLock);
	if(index < 0 || index >= sNumDevices)
		return false;
	strlcpy(name, sDevices[index].name, nameSize);
	strlcpy(uid, sDevices[index].uid, uidSize);
	return true;
}

uint32_t
ShellDeviceListGeneration(void)
{
	std::lock_guard<std::mutex> lock(sInfoLock);
	return sListGeneration;
}

#ifdef CA_SHELL_OFFLINE
bool gShellOffline;

bool
ShellStartOffline(Shared *shared, double rate)
{
	sShared = shared;
	sMixer = new Mixer(shared);
	sSampleTime = 0;
	sSpatialFailed.store(false, std::memory_order_relaxed);
	OutputFormat(rate, 512, 0, "offline");
	return SetupSpatialOrBypass(rate);
}

void
ShellRenderOffline(float *L, float *R, uint32_t frames)
{
	FlushDenormals ftz;
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

bool
ShellRebuildOfflineForTest(double rate)
{
	DisposeUnit(&sSpatial);
	DisposeUnit(&sSpatial3D);
	DisposeUnit(&sReverb);
	SetupSpatialOrBypass(rate);
	OutputFormat(rate, sBuffer, 0, "offline");
	std::lock_guard<std::mutex> lock(sInfoLock);
	sRebuilds++;
	return !sSpatialBypass;
}

void
ShellFailPullsForTest(int unit, uint32_t blocks)
{
	if(unit == 1)
		sFailSpatial3DPulls.store(blocks, std::memory_order_relaxed);
	else if(sSpatial)
		AudioUnitUninitialize(sSpatial);
}

void
ShellFailSpatialOnceForTest(int unit)
{
	if(unit == 1)
		sFailSpatial3DOnce = true;
	else
		sFailSpatialOnce = true;
}

void
ShellClockTapForTest(ClockTapFn tap, void *ctx)
{
	sClockTap = tap;
	sClockTapCtx = ctx;
}

void
ShellFakeDevicesForTest(const DeviceEntry *list, const double *rates, int count, uint32_t defaultId)
{
	EnsureControl();
	dispatch_sync(sControl, ^{
		sFake = true;
		sFakeCount = count < kMaxDevices ? count : kMaxDevices;
		for(int i = 0; i < sFakeCount; i++){
			sFakeList[i] = list[i];
			sFakeRate[i] = rates[i];
		}
		sFakeDefault = defaultId;
	});
}

void
ShellPostEventForTest(int event, uint32_t id)
{
	static const char *const why[] = { "default output changed", "rate changed", "device lost", "devices changed", "wake", "device chosen", "start", "retry" };
	dispatch_sync(sControl, ^{ Rebuild(event, id, why[event]); });
}

void
ShellBufferEventForTest(uint32_t frames)
{
	dispatch_sync(sControl, ^{ BufferSizeChanged(frames, 2 * frames); });
}

void
ShellWaitControlForTest(void)
{
	dispatch_sync(sControl, ^{});
}

bool
ShellFakeUnitPullForTest(float *L, float *R, uint32_t frames)
{
	uint64_t beats = sDrive.UnitBeats();
	struct { AudioBufferList list; AudioBuffer second; } abl;
	abl.list.mNumberBuffers = 2;
	abl.list.mBuffers[0].mNumberChannels = 1;
	abl.list.mBuffers[0].mDataByteSize = frames * 4;
	abl.list.mBuffers[0].mData = L;
	abl.second.mNumberChannels = 1;
	abl.second.mDataByteSize = frames * 4;
	abl.second.mData = R;
	AudioUnitRenderActionFlags flags = 0;
	OutputRender(NULL, &flags, NULL, 0, frames, &abl.list);
	return sDrive.UnitBeats() != beats;
}

uint32_t
ShellHrtfArmsForTest(void)
{
	std::lock_guard<std::mutex> lock(sInfoLock);
	return sHrtfArms;
}

uint64_t
ShellFramesForTest(void)
{
	return sDrive.Frames();
}

static uint32_t sLastChunkSeen[kStreams];

// A render takes less than one settle's worth from the ring, so every last chunk is seen here before the reader takes it.
static bool
StreamSettled(int s, ChunkRing &ring, const StreamStatus &st, const StreamSnap &snap, uint32_t gen, uint32_t frames)
{
	for(const Chunk *c = ring.peek(); c && GenBefore(c->gen, gen); c = ring.peek())
		ring.pop();
	if(snap.gen != gen || !snap.playing || st.failedGen.load(std::memory_order_acquire) == gen || st.drainedGen.load(std::memory_order_acquire) == gen)
		return true;
	uint32_t head = ring.head(), rate = 0;
	uint64_t have = 0;
	for(uint32_t i = ring.tail(); i != head; i++){
		const Chunk *c = ring.at(i);
		if(c->gen != gen)
			continue;
		rate = c->rate;
		have += c->frames;
		if(c->eof)
			sLastChunkSeen[s] = gen;
	}
	if(sLastChunkSeen[s] == gen)
		return true;
	if(st.prefilledGen.load(std::memory_order_acquire) != gen)
		return false;
	double step = OalStreamStep(rate, (float)sRate);
	return rate != 0 && have >= (uint64_t)(frames * step) + 2 * SincHalfFor(step) + kChunkFrames;
}

bool
ShellSettleStreamsForTest(uint32_t frames, int timeoutMs, double *waitedMs)
{
	auto t0 = std::chrono::steady_clock::now();
	bool settled = false;
	for(;;){
		settled = true;
		const Snapshot &snap = sShared->snapshot.read();
		for(int s = 0; s < kStreams; s++){
			uint32_t gen = sShared->status[s].postedGen.load(std::memory_order_acquire);
			if(gen != 0 && !StreamSettled(s, sShared->ring[s], sShared->status[s], snap.stream[s], gen, frames))
				settled = false;
		}
		double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		if(waitedMs)
			*waitedMs = ms;
		if(settled || ms >= timeoutMs)
			return settled;
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
}

bool
ShellWaitPrefilledForTest(uint8_t stream, int timeoutMs, double *waitedMs)
{
	auto t0 = std::chrono::steady_clock::now();
	const StreamStatus &st = sShared->status[stream];
	for(;;){
		uint32_t gen = st.postedGen.load(std::memory_order_acquire);
		bool done = st.prefilledGen.load(std::memory_order_acquire) == gen || st.failedGen.load(std::memory_order_acquire) == gen;
		double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		if(waitedMs)
			*waitedMs = ms;
		if(done || ms >= timeoutMs)
			return done;
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
}

int
ShellOwnerForTest(void)
{
	return sDrive.Owner();
}

void
ShellClockTimeForTest(ClockTime *time)
{
	sClockTime = time;
}

int
ShellRunDueRetriesForTest(void)
{
	__block int ran = 0;
	EnsureControl();
	dispatch_sync(sControl, ^{
		int64_t now = sClockTime ? sClockTime->Now() : 0;
		std::vector<FakeRetry> due;
		for(size_t i = 0; i < sFakeRetries.size();)
			if(sClockTime && sFakeRetries[i].due <= now){
				due.push_back(sFakeRetries[i]);
				sFakeRetries.erase(sFakeRetries.begin() + i);
			}else
				i++;
		for(size_t i = 0; i < due.size(); i++)
			Retry(due[i].session);
		ran = (int)due.size();
	});
	return ran;
}

int
ShellPendingRetriesForTest(void)
{
	__block int n = 0;
	EnsureControl();
	dispatch_sync(sControl, ^{
		for(size_t i = 0; i < sFakeRetries.size(); i++)
			if(sFakeRetries[i].session == sSession)
				n++;
	});
	return n;
}
#endif

ShellInfo
ShellGetInfo(void)
{
	std::lock_guard<std::mutex> lock(sInfoLock);
	ShellInfo i;
	i.rate = sRate;
	i.buffer = sBuffer;
	i.rebuilds = sRebuilds;
	i.stalls = sClock.Stalls();
	i.spatialBypassed = sSpatialBypass;
	i.spatialUnavailable = sSpatial3DUnavailable || sSpatialFailed.load(std::memory_order_relaxed);
	i.hrtfAtStart = sHrtfStart;
	i.hrtfAfterSecond = sHrtfLater;
	i.hrtfReadings = sHrtfReadings;
	i.fellBack = sFellBack;
	strncpy(i.device, sDeviceName, sizeof(i.device) - 1);
	i.device[sizeof(i.device) - 1] = '\0';
	return i;
}

}
#endif
