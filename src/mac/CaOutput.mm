#ifdef AUDIO_CA
#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "CaShell.h"
#ifdef CA_SHELL_OFFLINE
#include "CaShellOffline.h"
#endif
#include "CaRules.h"
#include "CaDevice.h"
#include "CaPrefs.h"
#include "CaDrive.h"
#include "CaFade.h"
#include "CaClock.h"
#include "OalPanTable.h"

namespace ca {

enum { kMaxDevices = 32, kBypassInput = kChannels + kEchoTaps, kSpatial3DInputs = kBypassInput + 1, kSpatialPullFailLimit = 8, kMaxOutChannels = 8 };
static_assert(kMaxOutChannels == kMaxLayoutChannels, "output channel limit");
static_assert(kLayoutStereo == kAudioChannelLayoutTag_Stereo && kLayoutMono == kAudioChannelLayoutTag_Mono && kLayoutQuadraphonic == kAudioChannelLayoutTag_Quadraphonic &&
	kLayoutMpeg50A == kAudioChannelLayoutTag_MPEG_5_0_A && kLayoutMpeg51A == kAudioChannelLayoutTag_MPEG_5_1_A && kLayoutMpeg51D == kAudioChannelLayoutTag_MPEG_5_1_D &&
	kLayoutMpeg61A == kAudioChannelLayoutTag_MPEG_6_1_A && kLayoutMpeg71C == kAudioChannelLayoutTag_MPEG_7_1_C, "layout tags");

static Shared *sShared;
static Mixer *sMixer;
static AudioUnit sOutput, sSpatial, sReverb, sSpatial3D;
static bool sSpatialBypass, sSpatial3DUnavailable;
static SpatialRender sSpatialRender = { kSpatialUseOutputType };
static int32_t sSpatialAlgorithm = kSpatialUseOutputType;
static int32_t sOutputPref = OUTPUT_AUTO;
static std::atomic<int32_t> sOutputType(OUTPUT_AUTO);
static std::atomic<bool> sHeadTrackingWanted(false);
static bool sHeadTrackingBuilt;
static OutputFade sFade;
static uint32_t sOutChannels = 2, sLayoutTag;
static int sFrontL = 0, sFrontR = 1;
static float sSetAz[kBypassInput], sSetEl[kBypassInput];
static EchoLines sEcho;
static float sSilence[kMaxBlock];
static std::atomic<uint64_t> sSpatialFrames;
static std::atomic<bool> sSpatialFailed;
static std::atomic<bool> sEchoReady;
static std::atomic<int32_t> sClockSpatial(-1);
static uint32_t sFailedPulls;
static int32_t sHrtfStart = -1, sHrtfLater = -1;
static uint32_t sHrtfReadings, sHrtfArms;
static double sRate;
static std::atomic<double> sRateNow(48000.0);
static uint32_t sBuffer;
static uint32_t sRebuilds;
static Float64 sSampleTime;
static char sDeviceName[128];
static float sDryL[kMaxBlock], sDryR[kMaxBlock], sSend[kMaxBlock], sStrL[kMaxBlock], sStrR[kMaxBlock];
static float sWetL[kMaxBlock], sWetR[kMaxBlock];
static AudioUnit sEnvUnit[NUM_ENVS];
static EnvBank sEnvBank;
static std::atomic<bool> sEnvOk, sEnvLogPulls;
static std::atomic<int32_t> sEnvHeld(-1);
static uint32_t sEnvFailedPulls, sEnvHold;
static bool sEnvPulled[NUM_ENVS];
static float sCaveSend[kMaxBlock], sEnvSend[NUM_ENVS][kMaxBlock];
static float *sEnvSendPtr[NUM_ENVS] = { sEnvSend[0], sEnvSend[1], sEnvSend[2], sEnvSend[3], sEnvSend[4] };
static float sEnvL[NUM_ENVS][kMaxBlock], sEnvR[NUM_ENVS][kMaxBlock];
static const float *sEnvFeed[NUM_ENVS][2] = { { sEnvSend[0], sEnvSend[0] }, { sEnvSend[1], sEnvSend[1] }, { sEnvSend[2], sEnvSend[2] },
	{ sEnvSend[3], sEnvSend[3] }, { sEnvSend[4], sEnvSend[4] } };
static const float *sFeed[2] = { sDryL, sDryR };
static const float *sFeed3D[2] = { sDryL, sDryR };
static UInt32 sChunkAt;
static float sSpatialCalib = kSpatialCalibHeadphones;
static const float *sSendFeed[2] = { sCaveSend, sCaveSend };

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
static bool sFailEnvOnce[NUM_ENVS];
static std::atomic<uint32_t> sFailEnvPulls[NUM_ENVS];
static std::atomic<uint64_t> sEnvPullCount, sEnvPullNanos;
static std::atomic<uint64_t> sMixNanos, sEchoNanos;
static std::atomic<uint64_t> sClassicFrames;
static std::atomic<uint32_t> sConfigCalls;
static std::atomic<uint32_t> sDeviceCalls;
static char sConfigDevice[128];
static uint32_t sStopQuiet, sStopTail;
static ClockTapFn sClockTap;
static void *sClockTapCtx;
static BlockTapFn sBlockTap;
static void *sBlockTapCtx;
typedef void (*EnvTapFn)(void *ctx, const float *envL, const float *envR, const float *caveL, const float *caveR, const float *envIn, uint32_t frames);
static EnvTapFn sEnvTap;
static float sTapL[kMaxBlock], sTapR[kMaxBlock], sTapIn[kMaxBlock];
static void *sEnvTapCtx;
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
	{ kAudioDevicePropertyPreferredChannelLayout, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain },
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

static void
CopyFeed(const AudioBuffer &dst, const float *src, UInt32 frames)
{
	UInt32 room = kMaxBlock - sChunkAt, cap = dst.mDataByteSize / sizeof(float);
	frames = frames < cap ? frames : cap;
	UInt32 n = frames < room ? frames : room;
	memcpy(dst.mData, src, n * sizeof(float));
	memset((float *)dst.mData + n, 0, (frames - n) * sizeof(float));
}

static OSStatus
FeedStereo(void *ref, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32 frames, AudioBufferList *io)
{
	const float *const *src = (const float *const *)ref;
	CopyFeed(io->mBuffers[0], src[0], frames);
	CopyFeed(io->mBuffers[1], src[1], frames);
	return noErr;
}

static OSStatus
FeedMono(void *ref, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32 frames, AudioBufferList *io)
{
	intptr_t c = (intptr_t)ref;
	const float *src = c < kChannels ? sMixer->mono((int)c) + sChunkAt : sEcho.live((int)c - kChannels) ? sEcho.out((int)c - kChannels) + sChunkAt : sSilence;
	CopyFeed(io->mBuffers[0], src, frames);
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

static void
SetHeadTrackingProperty(bool on)
{
	UInt32 tracking = on ? 1 : 0;
	OSStatus err = AudioUnitSetProperty(sSpatial3D, kAudioUnitProperty_SpatialMixerEnableHeadTracking, kAudioUnitScope_Global, 0, &tracking, sizeof(tracking));
	if(err != noErr)
		printf("%s head tracking %s failed (%d)\n", LogPrefix(), on ? "on" : "off", (int)err);
}

static bool
SetupSpatial3D(double rate, int32_t algorithm, int32_t type)
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
	bool multichannel = type == OUTPUT_MULTICHANNEL && sLayoutTag != 0;
	if(type != OUTPUT_HEADPHONES)
		algorithm = kSpatialUseOutputType;
	UInt32 alg = algorithm == kSpatialHRTFHQ ? kSpatializationAlgorithm_HRTFHQ : kSpatializationAlgorithm_UseOutputType;
	UInt32 outputType = type == OUTPUT_BUILTIN ? kSpatialMixerOutputType_BuiltInSpeakers : type == OUTPUT_MULTICHANNEL ? kSpatialMixerOutputType_ExternalSpeakers : kSpatialMixerOutputType_Headphones;
	AudioStreamBasicDescription stereo = StereoFormat(rate), mono = StereoFormat(rate, 1), out = StereoFormat(rate, multichannel ? sOutChannels : 2);
	bool ok = AudioUnitSetProperty(sSpatial3D, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, &inputs, sizeof(inputs)) == noErr;
	ok = ok && AudioUnitSetProperty(sSpatial3D, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof(maxFrames)) == noErr;
	ok = ok && AudioUnitSetProperty(sSpatial3D, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &out, sizeof(out)) == noErr;
	if(multichannel){
		AudioChannelLayout layout;
		memset(&layout, 0, sizeof(layout));
		layout.mChannelLayoutTag = sLayoutTag;
		ok = ok && AudioUnitSetProperty(sSpatial3D, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Output, 0, &layout, sizeof(layout)) == noErr;
	}
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
	sHeadTrackingBuilt = type == OUTPUT_HEADPHONES && sHeadTrackingWanted.load(std::memory_order_relaxed);
	if(sHeadTrackingBuilt)
		SetHeadTrackingProperty(true);
	sSpatialRender.algorithm = algorithm;
	sSpatialCalib = SpatialCalibFor(type);
	sFailedPulls = 0;
	sSpatialFailed.store(false, std::memory_order_relaxed);
	for(int c = 0; c < kBypassInput; c++)
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

static bool
InitReverb(AudioUnit u, double rate, const float **feed)
{
	UInt32 maxFrames = kMaxBlock;
	AudioStreamBasicDescription f = StereoFormat(rate);
	AURenderCallbackStruct cb = { FeedStereo, (void *)feed };
	bool ok = AudioUnitSetProperty(u, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof(maxFrames)) == noErr;
	ok = ok && AudioUnitSetProperty(u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof(f)) == noErr;
	ok = ok && AudioUnitSetProperty(u, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof(f)) == noErr;
	ok = ok && AudioUnitSetProperty(u, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb)) == noErr;
	return ok && AudioUnitInitialize(u) == noErr;
}

// Every write restarts the unit, so a preset is written once at setup and never on the render thread.
static bool
ApplyEnvPreset(AudioUnit u, int place)
{
	const EnvPreset &p = kEnvPresets[EnvClampPlace(place)];
	const ReverbParam params[] = {
		{ kReverb2Param_DryWetMix, 100.0f },
		{ kReverb2Param_Gain, p.gainDb },
		{ kReverb2Param_MinDelayTime, p.minDelay },
		{ kReverb2Param_MaxDelayTime, p.maxDelay },
		{ kReverb2Param_DecayTimeAt0Hz, p.decay0 },
		{ kReverb2Param_DecayTimeAtNyquist, p.decayNyquist },
	};
	bool ok = true;
	for(size_t i = 0; ok && i < sizeof(params) / sizeof(params[0]); i++)
		ok = AudioUnitSetParameter(u, params[i].id, kAudioUnitScope_Global, 0, params[i].value, 0) == noErr;
	return ok;
}

static void
DisposeEnvBank(void)
{
	for(int u = 0; u < NUM_ENVS; u++)
		DisposeUnit(&sEnvUnit[u]);
	sEnvOk.store(false, std::memory_order_relaxed);
}

static void
SetupEnvBank(double rate)
{
	DisposeEnvBank();
	bool ok = true;
	for(int u = 0; ok && u < NUM_ENVS; u++){
#ifdef CA_SHELL_OFFLINE
		if(sFailEnvOnce[u]){
			sFailEnvOnce[u] = false;
			ok = false;
			break;
		}
#endif
		sEnvUnit[u] = MakeUnit(kAudioUnitType_Effect, kAudioUnitSubType_Reverb2);
		ok = sEnvUnit[u] && InitReverb(sEnvUnit[u], rate, sEnvFeed[u]) && ApplyEnvPreset(sEnvUnit[u], u);
	}
	int place = sEnvHeld.load(std::memory_order_relaxed);
	if(place < 0)
		place = sShared ? EnvClampPlace(sShared->envPlace.load(std::memory_order_relaxed)) : ENV_OPEN;
	sEnvBank.init(place);
	sEnvFailedPulls = 0;
	sEnvHold = 0;
	if(!ok){
		DisposeEnvBank();
		printf("%s reverb units unavailable, spatial mode has no reverb\n", LogPrefix());
	}
	sEnvHeld.store(ok ? place : -1, std::memory_order_relaxed);
	sEnvOk.store(ok, std::memory_order_relaxed);
}

static void
SetupReverb(double rate)
{
	DisposeUnit(&sReverb);
	sReverb = MakeUnit(kAudioUnitType_Effect, kAudioUnitSubType_Reverb2);
	if(sReverb == NULL){
		printf("%s reverb unavailable\n", LogPrefix());
		return;
	}
	bool ok = InitReverb(sReverb, rate, sSendFeed);
	for(size_t i = 0; ok && i < sizeof(kCaveOnReverb2) / sizeof(kCaveOnReverb2[0]); i++)
		ok = AudioUnitSetParameter(sReverb, kCaveOnReverb2[i].id, kAudioUnitScope_Global, 0, kCaveOnReverb2[i].value, 0) == noErr;
	if(!ok){
		DisposeUnit(&sReverb);
		printf("%s reverb unavailable\n", LogPrefix());
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

static bool
PullMulti(AudioUnit u, float *const *ch, UInt32 count, UInt32 frames, UInt32 at)
{
	AudioTimeStamp ts;
	memset(&ts, 0, sizeof(ts));
	ts.mSampleTime = sSampleTime + at;
	ts.mFlags = kAudioTimeStampSampleTimeValid;
	struct { AudioBufferList list; AudioBuffer more[kMaxOutChannels - 1]; } abl;
	abl.list.mNumberBuffers = count;
	for(UInt32 c = 0; c < count; c++){
		abl.list.mBuffers[c].mNumberChannels = 1;
		abl.list.mBuffers[c].mDataByteSize = frames * 4;
		abl.list.mBuffers[c].mData = ch[c] + at;
	}
	AudioUnitRenderActionFlags flags = 0;
	return AudioUnitRender(u, &flags, &ts, 0, frames, &abl.list) == noErr;
}

static void
ZeroRest(float *const *out, UInt32 count, UInt32 at, UInt32 frames)
{
	for(UInt32 c = 0; c < count; c++)
		if((int)c != sFrontL && (int)c != sFrontR)
			memset(out[c] + at, 0, frames * sizeof(float));
}

static void
CountUnderrun(void)
{
	sShared->underruns.fetch_add(1, std::memory_order_relaxed);
}

static void
CountRouting(bool spatial, uint32_t frames)
{
#ifdef CA_SHELL_OFFLINE
	if(!spatial)
		sClassicFrames.fetch_add(frames, std::memory_order_relaxed);
#else
	(void)spatial;
	(void)frames;
#endif
}

static bool
RenderEnvBank(uint32_t from, uint32_t to, UInt32 frames)
{
	if(!sEnvOk.load(std::memory_order_relaxed))
		return false;
	sEnvBank.observe(sMixer->env());
	sEnvHeld.store(sEnvBank.state().current, std::memory_order_relaxed);
	if(!EnvPullBlock(sEnvHold, to > from ? to - from : 0, frames, (float)sRate)){
		sEnvBank.settle();
		return false;
	}
	sEnvBank.split(sSend, from, to, sMixer->env(), sEnvSendPtr, frames, (float)sRate);
	bool pulled = true;
#ifdef CA_SHELL_OFFLINE
	auto pullStart = std::chrono::steady_clock::now();
#endif
	for(int u = 0; u < NUM_ENVS; u++){
		sEnvPulled[u] = false;
#ifdef CA_SHELL_OFFLINE
		sEnvPullCount.fetch_add(1, std::memory_order_relaxed);
		if(sFailEnvPulls[u].load(std::memory_order_relaxed)){
			sFailEnvPulls[u].fetch_sub(1, std::memory_order_relaxed);
			pulled = false;
			continue;
		}
#endif
		sEnvPulled[u] = PullStereo(sEnvUnit[u], sEnvL[u], sEnvR[u], frames);
		pulled = sEnvPulled[u] && pulled;
	}
#ifdef CA_SHELL_OFFLINE
	sEnvPullNanos.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - pullStart).count(), std::memory_order_relaxed);
#endif
	if(pulled)
		sEnvFailedPulls = 0;
	else{
		CountUnderrun();
		if(++sEnvFailedPulls >= kSpatialPullFailLimit){
			sEnvOk.store(false, std::memory_order_relaxed);
			sEnvHeld.store(-1, std::memory_order_relaxed);
			sEnvLogPulls.store(true, std::memory_order_relaxed);
		}
	}
	return true;
}

static void
RenderBlock(float *L, float *R, UInt32 frames, float *const *out = NULL, UInt32 count = 2)
{
#ifdef CA_SHELL_OFFLINE
	auto mixStart = std::chrono::steady_clock::now();
#endif
	bool spatial = sMixer->render(sDryL, sDryR, sSend, sStrL, sStrR, frames, (float)sRate, sSpatial3D && !sSpatialFailed.load(std::memory_order_relaxed) ? &sSpatialRender : NULL);
#ifdef CA_SHELL_OFFLINE
	sMixNanos.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mixStart).count(), std::memory_order_relaxed);
	if(sBlockTap)
		sBlockTap(sBlockTapCtx, *sMixer, spatial, sStrL, sStrR, frames);
#endif
	CountRouting(spatial, frames);
	RouteSpan route = sMixer->route();
	uint32_t from = route.before ? 0 : route.after ? route.split : 0, to = route.after ? frames : route.before ? route.split : 0;
	if(spatial){
#ifdef CA_SHELL_OFFLINE
		auto echoStart = std::chrono::steady_clock::now();
#endif
		sEcho.render(*sMixer, sSpatialRender.algorithm, (float)sRate, from, to, frames);
#ifdef CA_SHELL_OFFLINE
		sEchoNanos.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - echoStart).count(), std::memory_order_relaxed);
#endif
		UInt32 chunk = SpatialChunkFrames((float)sRate);
		bool failed = false;
		for(uint32_t k = 0; k < sMixer->chunks(); k++){
			UInt32 at = k * chunk, n = frames - at < chunk ? frames - at : chunk;
			for(int c = 0; c < kBypassInput; c++){
				if(c >= kChannels && !sEcho.live(c - kChannels))
					continue;
				Direction d = c < kChannels ? sMixer->direction(c, k) : sEcho.direction(c - kChannels, k);
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
			sFeed3D[0] = count > 2 ? sSilence : sDryL + at;
			sFeed3D[1] = count > 2 ? sSilence : sDryR + at;
			bool pulled = true;
#ifdef CA_SHELL_OFFLINE
			if(sFailSpatial3DPulls.load(std::memory_order_relaxed)){
				sFailSpatial3DPulls.fetch_sub(1, std::memory_order_relaxed);
				pulled = false;
			}
#endif
			if(!pulled || !(count > 2 ? PullMulti(sSpatial3D, out, count, n, at) : PullStereo(sSpatial3D, L + at, R + at, n, at))){
				failed = true;
				if(count > 2)
					ZeroRest(out, count, at, n);
				memcpy(L + at, sDryL + at, n * sizeof(float));
				memcpy(R + at, sDryR + at, n * sizeof(float));
				float gl = kOalPanCentre[0] / sSpatialCalib, gr = kOalPanCentre[1] / sSpatialCalib;
				for(int c = 0; c < kBypassInput; c++){
					if(c >= kChannels && !sEcho.live(c - kChannels))
						continue;
					const float *m = (c < kChannels ? sMixer->mono(c) : sEcho.out(c - kChannels)) + at;
					for(UInt32 i = 0; i < n; i++){
						L[at + i] += m[i] * gl;
						R[at + i] += m[i] * gr;
					}
				}
			}else if(count > 2){
				for(UInt32 i = 0; i < n; i++){
					L[at + i] += sDryL[at + i];
					R[at + i] += sDryR[at + i];
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
		sEcho.cut();
		AudioUnit mix = sSpatialBypass ? NULL : sSpatial;
		if(count > 2)
			ZeroRest(out, count, 0, frames);
		if(mix == NULL || !PullStereo(mix, L, R, frames)){
			if(mix)
				CountUnderrun();
			memcpy(L, sDryL, frames * sizeof(float));
			memcpy(R, sDryR, frames * sizeof(float));
		}
	}
	for(UInt32 i = 0; i < frames; i++)
		sCaveSend[i] = i >= from && i < to ? 0.0f : sSend[i];
	bool caveOk = false;
	if(sReverb){
		caveOk = PullStereo(sReverb, sWetL, sWetR, frames);
		if(caveOk)
			for(UInt32 i = 0; i < frames; i++){
				L[i] += sWetL[i];
				R[i] += sWetR[i];
			}
		else
			CountUnderrun();
	}
	bool envRan = RenderEnvBank(from, to, frames);
	if(envRan)
		for(int u = 0; u < NUM_ENVS; u++)
			if(sEnvPulled[u])
				for(UInt32 i = 0; i < frames; i++){
					L[i] += sEnvL[u][i];
					R[i] += sEnvR[u][i];
				}
#ifdef CA_SHELL_OFFLINE
	if(sEnvTap){
		if(!caveOk){
			memset(sWetL, 0, frames * sizeof(float));
			memset(sWetR, 0, frames * sizeof(float));
		}
		for(UInt32 i = 0; i < frames; i++){
			float l = 0.0f, r = 0.0f, in = 0.0f;
			for(int u = 0; envRan && u < NUM_ENVS; u++){
				l += sEnvPulled[u] ? sEnvL[u][i] : 0.0f;
				r += sEnvPulled[u] ? sEnvR[u][i] : 0.0f;
				in += sEnvSend[u][i];
			}
			sTapL[i] = l;
			sTapR[i] = r;
			sTapIn[i] = in;
		}
		sEnvTap(sEnvTapCtx, sTapL, sTapR, sWetL, sWetR, sTapIn, frames);
	}
#endif
	for(UInt32 i = 0; i < frames; i++){
		L[i] += sStrL[i];
		R[i] += sStrR[i];
	}
	sSampleTime += frames;
}

static OSStatus
OutputRender(void *, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32 frames, AudioBufferList *io)
{
	UInt32 count = io->mNumberBuffers < (UInt32)kMaxOutChannels ? io->mNumberBuffers : (UInt32)kMaxOutChannels;
	float *out[kMaxOutChannels];
	for(UInt32 c = 0; c < count; c++)
		out[c] = (float *)io->mBuffers[c].mData;
	if(count < 2){
		for(UInt32 c = 0; c < count; c++)
			memset(out[c], 0, frames * sizeof(float));
		return noErr;
	}
	int e = sDrive.EnterUnit();
	if(e != Drive::RENDER){
		for(UInt32 c = 0; c < count; c++)
			memset(out[c], 0, frames * sizeof(float));
		if(e == Drive::BLOCKED)
			CountUnderrun();
		return noErr;
	}
	bool wide = count > 2 && count == sOutChannels;
	float *L = out[wide ? sFrontL : 0], *R = out[wide ? sFrontR : 1];
	for(UInt32 c = 2; !wide && c < count; c++)
		memset(out[c], 0, frames * sizeof(float));
	FlushDenormals ftz;
	for(UInt32 done = 0; done < frames;){
		UInt32 n = frames - done < (UInt32)kMaxBlock ? frames - done : (UInt32)kMaxBlock;
		float *at[kMaxOutChannels];
		for(UInt32 c = 0; c < count; c++)
			at[c] = out[c] + done;
		RenderBlock(L + done, R + done, n, at, wide ? count : 2);
		done += n;
	}
	sFade.Apply(out, count, frames, sRateNow.load(std::memory_order_relaxed));
	sDrive.LeaveUnit(frames);
	return noErr;
}

static void
ClockRender(void *, uint32_t frames, double rate)
{
	FlushDenormals ftz;
	SpatialRender held = { sClockSpatial.load(std::memory_order_relaxed) };
	for(uint32_t done = 0; done < frames;){
		uint32_t n = frames - done < (uint32_t)kMaxBlock ? frames - done : (uint32_t)kMaxBlock;
		CountRouting(sMixer->render(sDryL, sDryR, sSend, sStrL, sStrR, n, (float)rate, held.algorithm >= 0 ? &held : NULL), n);
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

static int
ReadUInt32Checked(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope, uint32_t *out)
{
	AudioObjectPropertyAddress a = { sel, scope, kAudioObjectPropertyElementMain };
	UInt32 v = 0, size = sizeof(v);
	*out = 0;
	if(!AudioObjectHasProperty(obj, &a))
		return 0;
	if(AudioObjectGetPropertyData(obj, &a, 0, NULL, &size, &v) != noErr)
		return -1;
	*out = v;
	return 1;
}

static uint32_t
ReadUInt32(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope)
{
	uint32_t v;
	ReadUInt32Checked(obj, sel, scope, &v);
	return v;
}

static bool
FirstLayoutLog(const char *name)
{
	static std::mutex lock;
	static std::vector<std::string> seen;
	std::lock_guard<std::mutex> guard(lock);
	for(const std::string &n : seen)
		if(n == name)
			return false;
	seen.push_back(name);
	return true;
}

static uint32_t
LayoutTagOf(AudioChannelLayout *layout, UInt32 size, const char *name = NULL)
{
	uint32_t tag = layout->mChannelLayoutTag;
	if(tag != kAudioChannelLayoutTag_UseChannelDescriptions && tag != kAudioChannelLayoutTag_UseChannelBitmap)
		return tag;
	UInt32 reduced = 0, out = sizeof(reduced);
	if(AudioFormatGetProperty(kAudioFormatProperty_TagForChannelLayout, size, layout, &out, &reduced) == noErr)
		return reduced;
	UInt32 used = 0;
	if(tag == kAudioChannelLayoutTag_UseChannelDescriptions)
		for(UInt32 i = 0; i < layout->mNumberChannelDescriptions; i++)
			used += layout->mChannelDescriptions[i].mChannelLabel != kAudioChannelLabel_Unused;
	if(used > 4 && name && FirstLayoutLog(name))
		printf("%s speaker layout of %u channels on \"%s\" has no standard tag, the channel count picks the order\n", LogPrefix(), (unsigned)used, name);
	return used > 0 && used <= 4 ? kAudioChannelLayoutTag_Stereo : 0;
}

static bool
MixerAcceptsLayout(uint32_t tag)
{
#ifdef CA_SHELL_OFFLINE
	if(sFailSpatial3DOnce){
		sFailSpatial3DOnce = false;
		return false;
	}
#endif
	AudioUnit unit = MakeUnit(kAudioUnitType_Mixer, kAudioUnitSubType_SpatialMixer);
	if(unit == NULL)
		return false;
	AudioStreamBasicDescription out = StereoFormat(48000.0, tag & 0xFFFF);
	AudioChannelLayout layout;
	memset(&layout, 0, sizeof(layout));
	layout.mChannelLayoutTag = tag;
	bool ok = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &out, sizeof(out)) == noErr;
	ok = ok && AudioUnitSetProperty(unit, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Output, 0, &layout, sizeof(layout)) == noErr;
	DisposeUnit(&unit);
	return ok;
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
	strlcpy(e.name, name, sizeof(e.name));
	a.mSelector = kAudioDevicePropertyDeviceIsAlive;
	UInt32 alive = 0;
	size = sizeof(alive);
	AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &alive);
	e.alive = alive != 0;
	e.transport = ReadUInt32(dev, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal);
	e.dataSource = ReadUInt32(dev, kAudioDevicePropertyDataSource, kAudioObjectPropertyScopeOutput);
	AudioObjectPropertyAddress sa = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
	AudioStreamID streams[16];
	uint32_t terminals[16];
	size = sizeof(streams);
	int count = 0;
	if(AudioObjectGetPropertyData(dev, &sa, 0, NULL, &size, streams) == noErr)
		for(UInt32 i = 0; i < size / sizeof(AudioStreamID) && i < 16; i++)
			terminals[count++] = ReadUInt32(streams[i], kAudioStreamPropertyTerminalType, kAudioObjectPropertyScopeGlobal);
	e.terminal = SummariseTerminals(terminals, count);
	AudioObjectPropertyAddress la = { kAudioDevicePropertyPreferredChannelLayout, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
	size = 0;
	if(AudioObjectGetPropertyDataSize(dev, &la, 0, NULL, &size) == noErr && size >= sizeof(AudioChannelLayout)){
		std::vector<uint8_t> raw(size);
		AudioChannelLayout *layout = (AudioChannelLayout *)raw.data();
		if(AudioObjectGetPropertyData(dev, &la, 0, NULL, &size, layout) == noErr)
			e.layoutTag = LayoutTagOf(layout, size, name);
	}
	return true;
}

static int
ReadDeviceList(DeviceEntry *out, uint32_t *defaultId)
{
#ifdef CA_SHELL_OFFLINE
	if(sFake){
		for(int i = 0; i < sFakeCount; i++)
			out[i] = sFakeList[i];
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
		sRateNow.store(rate, std::memory_order_relaxed);
		sBuffer = buffer;
		strlcpy(sDeviceName, device, sizeof(sDeviceName));
	}
	if(sShared)
		sShared->streamLatency.store(latency, std::memory_order_relaxed);
	sFade.SetTail(latency > buffer ? latency - buffer : 0);
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
	sFade.SetTail(latency > buffer ? latency - buffer : 0);
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

static uint32_t
CurrentLayout(AudioObjectID dev)
{
	for(int i = 0; i < sNumDevices; i++)
		if(sDevices[i].id == dev)
			return sDevices[i].layoutTag;
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
		case kAudioDevicePropertyPreferredChannelLayout:
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
	int event = DeviceBatchEvent(gone, format, sRate, sChannels, rate, gone ? 0 : e.outputChannels, CurrentLayout(dev), gone ? 0 : e.layoutTag);
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
FadeOutput(void)
{
	if(sDrive.Owner() != Drive::OWNER_UNIT)
		return;
	sFade.Close();
	double rate = sRate > 0.0 ? sRate : 48000.0;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds((int64_t)(1e6 * (OutputFade::kSeconds + (4.0 * sBuffer + sFade.Tail()) / rate)) + 8000);
	while(!sFade.Closed() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::microseconds(500));
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
	sEchoReady.store(false, std::memory_order_relaxed);
	DisposeUnit(&sReverb);
	DisposeEnvBank();
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
		printf("%s spatial mixer unavailable, classic stereo bypasses it\n", LogPrefix());
	DisposeUnit(&sSpatial3D);
	int32_t type = sOutputType.load(std::memory_order_relaxed);
	bool unavailable = OutputRoutesSpatial(type) && !SetupSpatial3D(rate, sSpatialAlgorithm, type);
	bool echoReady = sSpatial3D != NULL && sEcho.allocate((float)rate);
	if(!echoReady)
		sEcho.release();
	sEchoReady.store(echoReady, std::memory_order_relaxed);
	sClockSpatial.store(sSpatial3D ? sSpatialRender.algorithm : -1, std::memory_order_relaxed);
	sSpatialFrames.store(0, std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(sInfoLock);
		sSpatial3DUnavailable = unavailable;
	}
	if(unavailable)
		printf("%s spatial mixer for positioned voices unavailable, spatial mode uses classic routing\n", LogPrefix());
	SetupReverb(rate);
	SetupEnvBank(rate);
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

struct DeviceReadout
{
	uint32_t transport, dataSource;
	int transportRead, dataSourceRead, streamsRead, streams;
	uint32_t terminal[16];
	int terminalRead[16];
};

static void
ReadReadout(AudioObjectID dev, DeviceReadout &r)
{
#ifdef CA_SHELL_OFFLINE
	if(sFake){
		for(int i = 0; i < sFakeCount; i++)
			if(sFakeList[i].id == dev){
				r.transport = sFakeList[i].transport;
				r.transportRead = 1;
				r.dataSource = sFakeList[i].dataSource;
				r.dataSourceRead = r.dataSource != 0;
				r.terminal[0] = sFakeList[i].terminal;
				r.terminalRead[0] = 1;
				r.streams = 1;
				r.streamsRead = 1;
			}
		return;
	}
#endif
	r.transportRead = ReadUInt32Checked(dev, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, &r.transport);
	r.dataSourceRead = ReadUInt32Checked(dev, kAudioDevicePropertyDataSource, kAudioObjectPropertyScopeOutput, &r.dataSource);
	AudioObjectPropertyAddress sa = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
	AudioStreamID ids[16];
	UInt32 size = sizeof(ids);
	r.streamsRead = AudioObjectGetPropertyData(dev, &sa, 0, NULL, &size, ids) == noErr ? 1 : -1;
	for(UInt32 i = 0; r.streamsRead > 0 && i < size / sizeof(AudioStreamID) && i < 16; i++, r.streams++)
		r.terminalRead[r.streams] = ReadUInt32Checked(ids[i], kAudioStreamPropertyTerminalType, kAudioObjectPropertyScopeGlobal, &r.terminal[r.streams]);
}

static void
CodeText(char *out, size_t size, uint32_t v, int read)
{
	if(read < 0){
		strlcpy(out, "failed", size);
		return;
	}
	if(read == 0){
		strlcpy(out, "none", size);
		return;
	}
	char c[5] = { (char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v, 0 };
	bool text = c[0] > ' ' && c[0] < 127;
	for(int i = 1; i < 4; i++)
		text = text && c[i] >= ' ' && c[i] < 127;
	if(!text){
		snprintf(out, size, v ? "0x%x" : "%u", v);
		return;
	}
	for(int i = 3; i > 0 && c[i] == ' '; i--)
		c[i] = 0;
	strlcpy(out, c, size);
}

static void
LogDevice(AudioObjectID dev, const char *name)
{
	DeviceReadout r;
	memset(&r, 0, sizeof(r));
	ReadReadout(dev, r);
	char transport[16], source[16], terminals[16 * 12] = "", code[16];
	CodeText(transport, sizeof(transport), r.transport, r.transportRead);
	CodeText(source, sizeof(source), r.dataSource, r.dataSourceRead);
	if(r.streamsRead < 0 || r.streams == 0)
		strlcpy(terminals, r.streamsRead < 0 ? "failed" : "none", sizeof(terminals));
	for(int i = 0; i < r.streams; i++){
		CodeText(code, sizeof(code), r.terminal[i], r.terminalRead[i]);
		if(i > 0)
			strlcat(terminals, " ", sizeof(terminals));
		strlcat(terminals, code, sizeof(terminals));
	}
	printf("%s device \"%s\" transport %s, terminals %s, data source %s, channels %u, output %s\n", LogPrefix(), name, transport, terminals, source,
		CurrentChannels(dev), OutputTypeName(sOutputType.load(std::memory_order_relaxed)));
}

static void
FrontPair(uint32_t tag, int *left, int *right)
{
	UInt32 size = 0;
	if(AudioFormatGetPropertyInfo(kAudioFormatProperty_ChannelLayoutForTag, sizeof(tag), &tag, &size) != noErr || size < sizeof(AudioChannelLayout))
		return;
	std::vector<uint8_t> raw(size);
	AudioChannelLayout *layout = (AudioChannelLayout *)raw.data();
	if(AudioFormatGetProperty(kAudioFormatProperty_ChannelLayoutForTag, sizeof(tag), &tag, &size, layout) != noErr)
		return;
	int l = -1, r = -1;
	for(UInt32 i = 0; i < layout->mNumberChannelDescriptions && i < (UInt32)kMaxOutChannels; i++){
		if(layout->mChannelDescriptions[i].mChannelLabel == kAudioChannelLabel_Left && l < 0)
			l = (int)i;
		if(layout->mChannelDescriptions[i].mChannelLabel == kAudioChannelLabel_Right && r < 0)
			r = (int)i;
	}
	if(l >= 0 && r >= 0){
		*left = l;
		*right = r;
	}
}

static void
ApplyOutputType(const DeviceEntry &e)
{
	int32_t type = EffectiveOutputType(sOutputPref, e);
	uint32_t tag = type == OUTPUT_MULTICHANNEL ? MultichannelLayoutTag(e) : 0;
	if(tag != 0 && !MixerAcceptsLayout(tag))
		tag = 0;
	if(sOutputPref == OUTPUT_MULTICHANNEL && tag == 0){
		printf("%s multichannel not used, %u channels, layout tag %u, %s\n", LogPrefix(), e.outputChannels, e.layoutTag,
			e.outputChannels > 2 ? "no layout the spatial mixer supports" : "the device has two channels or fewer");
		type = DetectOutputType(e);
	}
	sLayoutTag = tag;
	sOutChannels = tag ? tag & 0xFFFF : 2;
	sFrontL = 0;
	sFrontR = 1;
	if(tag)
		FrontPair(tag, &sFrontL, &sFrontR);
	sOutputType.store(type, std::memory_order_relaxed);
}

static void
ResolveOutputType(AudioObjectID dev)
{
	DeviceEntry e;
	memset(&e, 0, sizeof(e));
	for(int i = 0; i < sNumDevices; i++)
		if(sDevices[i].id == dev)
			e = sDevices[i];
	ApplyOutputType(e);
}

static bool
OpenOutput(AudioObjectID dev)
{
	char name[128];
	ReadDeviceName(dev, name, sizeof(name));
	ResolveOutputType(dev);
	LogDevice(dev, name);
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
	AudioStreamBasicDescription f = StereoFormat(rate, sOutChannels);
	AURenderCallbackStruct cb = { OutputRender, NULL };
	bool ok = AudioUnitSetProperty(sOutput, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof(dev)) == noErr;
	ok = ok && AudioUnitSetProperty(sOutput, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof(f)) == noErr;
	if(sLayoutTag){
		AudioChannelLayout layout;
		memset(&layout, 0, sizeof(layout));
		layout.mChannelLayoutTag = sLayoutTag;
		OSStatus st = ok ? AudioUnitSetProperty(sOutput, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Input, 0, &layout, sizeof(layout)) : noErr;
		if(st != noErr)
			printf("%s output unit refused the channel layout (%d), channels follow the device order\n", LogPrefix(), (int)st);
	}
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
	sClockSpatial.store(sSpatial3D && !sSpatialFailed.load(std::memory_order_relaxed) ? sSpatialRender.algorithm : -1, std::memory_order_relaxed);
	FadeOutput();
#ifdef CA_SHELL_OFFLINE
	sStopQuiet = sFade.Quiet();
	sStopTail = sFade.Tail();
#endif
	StopOutput();
	sFade.Restart(event != EVENT_START);
	DeviceChoice c = ChooseDevice(sPinned, sDevices, sNumDevices, sDefaultId);
	bool ok = c.id != kAudioObjectUnknown && OpenOutput(c.id);
	if(!ok && c.id != kAudioObjectUnknown){
		char name[128];
		ReadDeviceName(c.id, name, sizeof(name));
		printf("%s output failed on \"%s\"%s\n", LogPrefix(), name, c.id != sDefaultId ? ", using the default" : "");
		if(c.id != sDefaultId && sDefaultId != kAudioObjectUnknown){
			ok = OpenOutput(sDefaultId);
			c.fellBack = true;
		}
	}
	if(!ok){
		sOutputType.store(OUTPUT_AUTO, std::memory_order_relaxed);
		sClockSpatial.store(-1, std::memory_order_relaxed);
		OutputFormat(sRate, sBuffer, 0, "none");
	}
	if(!ok && c.id != kAudioObjectUnknown && event != EVENT_RETRY)
		ScheduleRetry();
	{
		std::lock_guard<std::mutex> lock(sInfoLock);
		sFellBack = !FollowsDefault(sPinned) && (c.fellBack || !ok);
		if(event != EVENT_START)
			sRebuilds++;
	}
	if(!ok)
		printf("%s no output device (%s), a silent clock drives the engine at %.0f Hz\n", LogPrefix(), why, sRate);
	else if(event == EVENT_START)
		printf("%s output \"%s\" %.0f Hz, buffer %u%s%s\n", LogPrefix(), sDeviceName, sRate, sBuffer, sSpatialBypass ? ", spatial mixer bypassed" : "",
			c.fellBack ? ", pinned device missing, using the default" : "");
	else
		printf("%s output rebuilt (%s): \"%s\" %.0f Hz, buffer %u%s\n", LogPrefix(), why, sDeviceName, sRate, sBuffer,
			c.fellBack ? ", pinned device missing, using the default" : "");
}

static void
EnsureControl(const char *label = NULL)
{
	if(sControl == nil)
		sControl = dispatch_queue_create(label ? label : kDefaultQueueLabel, DISPATCH_QUEUE_SERIAL);
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
ShellStart(Shared *shared, const char *deviceUid, int32_t outputPref, int32_t algorithm, const ShellConfig *config)
{
	if(sMixer == NULL)
		SetLogPrefix(config ? config->logPrefix : NULL);
#ifdef CA_SHELL_OFFLINE
	if(gShellOffline){
		sOutputPref = outputPref;
		sSpatialAlgorithm = algorithm;
		return ShellStartOffline(shared, 48000.0);
	}
	if(!sFake){
		printf("%s the offline tools never open a real device\n", LogPrefix());
		return false;
	}
#endif
	sShared = shared;
	sMixer = new Mixer(shared);
	sSampleTime = 0;
	sSpatialFailed.store(false, std::memory_order_relaxed);
	sEnvHeld.store(-1, std::memory_order_relaxed);
	sClockSpatial.store(-1, std::memory_order_relaxed);
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
	EnsureControl(QueueLabelOf(config));
	const char *pin = deviceUid && deviceUid[0] ? deviceUid : "default";
	dispatch_sync(sControl, ^{
		sStopping = false;
		sSpatialWanted = false;
		sOutputPref = outputPref;
		sSpatialAlgorithm = algorithm;
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
	DisposeEnvBank();
	sEchoReady.store(false, std::memory_order_relaxed);
	sEcho.release();
	delete sMixer;
	sMixer = NULL;
	sShared = NULL;
}

void
ShellService(void)
{
	if(sEnvLogPulls.exchange(false, std::memory_order_relaxed))
		printf("%s reverb units stopped after repeated failed renders, spatial mode has no reverb until the output is rebuilt\n", LogPrefix());
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
ShellSetOutputConfig(int32_t algorithm, int32_t outputPref, int32_t headTracking, const char *device)
{
#ifdef CA_SHELL_OFFLINE
	sConfigCalls.fetch_add(1, std::memory_order_relaxed);
	strlcpy(sConfigDevice, device ? device : "keep", sizeof(sConfigDevice));
#endif
	if(headTracking != kKeepConfig)
		sHeadTrackingWanted.store(headTracking != 0, std::memory_order_relaxed);
	if(sControl == nil)
		return;
	struct Pin { char uid[128]; bool set; } pin;
	pin.set = device != NULL;
	strlcpy(pin.uid, device && device[0] ? device : "default", sizeof(pin.uid));
	dispatch_async(sControl, ^{
		if(sStopping)
			return;
		bool changed = false;
		if(pin.set && DeviceChoiceChanges(sPinned, pin.uid)){
			strlcpy(sPinned, pin.uid, sizeof(sPinned));
			changed = true;
		}
		if(algorithm != kKeepConfig && sSpatialAlgorithm != algorithm){
			sSpatialAlgorithm = algorithm;
			changed = true;
		}
		if(outputPref != kKeepConfig && sOutputPref != outputPref){
			sOutputPref = outputPref;
			changed = true;
		}
		bool tracking = sHeadTrackingWanted.load(std::memory_order_relaxed) && sOutputType.load(std::memory_order_relaxed) == OUTPUT_HEADPHONES;
		if(sSpatial3D != NULL && tracking != sHeadTrackingBuilt)
			changed = true;
		if(changed)
			Rebuild(EVENT_PIN, 0, "output configuration changed");
	});
}

void
ShellSetAlgorithm(int32_t algorithm)
{
	ShellSetOutputConfig(algorithm, kKeepConfig, kKeepConfig);
}

void
ShellSetHeadTracking(bool on)
{
	ShellSetOutputConfig(kKeepConfig, kKeepConfig, on ? 1 : 0);
}

void
ShellSetOutputPref(int32_t outputPref)
{
	ShellSetOutputConfig(kKeepConfig, outputPref, kKeepConfig);
}

void
ShellSetDevice(const char *uid)
{
#ifdef CA_SHELL_OFFLINE
	sDeviceCalls.fetch_add(1, std::memory_order_relaxed);
#endif
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
	sEnvHeld.store(-1, std::memory_order_relaxed);
	sOutChannels = 2;
	sLayoutTag = 0;
	sFrontL = 0;
	sFrontR = 1;
	if(sOutputPref == OUTPUT_MULTICHANNEL){
		DeviceEntry stereo;
		memset(&stereo, 0, sizeof(stereo));
		stereo.outputChannels = 2;
		ApplyOutputType(stereo);
	}else
		sOutputType.store(sOutputPref > OUTPUT_AUTO && sOutputPref < NUM_OUTPUTS ? sOutputPref : OUTPUT_HEADPHONES, std::memory_order_relaxed);
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
	DisposeEnvBank();
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
	else if(unit >= 2 && unit < 2 + NUM_ENVS)
		sFailEnvPulls[unit - 2].store(blocks, std::memory_order_relaxed);
	else if(sSpatial)
		AudioUnitUninitialize(sSpatial);
}

void
ShellFailSpatialOnceForTest(int unit)
{
	if(unit == 1)
		sFailSpatial3DOnce = true;
	else if(unit >= 2 && unit < 2 + NUM_ENVS)
		sFailEnvOnce[unit - 2] = true;
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
ShellBlockTapForTest(BlockTapFn tap, void *ctx)
{
	sBlockTap = tap;
	sBlockTapCtx = ctx;
}

void
ShellEnvTapForTest(EnvTapFn tap, void *ctx)
{
	sEnvTap = tap;
	sEnvTapCtx = ctx;
}

static const AudioUnitParameterID kEnvParamIds[6] = { kReverb2Param_DryWetMix, kReverb2Param_Gain, kReverb2Param_MinDelayTime, kReverb2Param_MaxDelayTime,
	kReverb2Param_DecayTimeAt0Hz, kReverb2Param_DecayTimeAtNyquist };

bool
ShellEnvParamsForTest(int unit, float out[6])
{
	if(unit < 0 || unit >= NUM_ENVS || sEnvUnit[unit] == NULL)
		return false;
	for(int i = 0; i < 6; i++){
		AudioUnitParameterValue v;
		if(AudioUnitGetParameter(sEnvUnit[unit], kEnvParamIds[i], kAudioUnitScope_Global, 0, &v) != noErr)
			return false;
		out[i] = v;
	}
	return true;
}

void
ShellEnvPoisonForTest(int unit)
{
	static const float poison[6] = { 50.0f, 3.0f, 0.02f, 0.09f, 5.0f, 4.0f };
	for(int i = 0; unit >= 0 && unit < NUM_ENVS && sEnvUnit[unit] && i < 6; i++)
		AudioUnitSetParameter(sEnvUnit[unit], kEnvParamIds[i], kAudioUnitScope_Global, 0, poison[i], 0);
}

uint64_t
ShellEnvPullsForTest(void)
{
	return sEnvPullCount.load(std::memory_order_relaxed);
}

uint64_t
ShellEnvPullNanosForTest(void)
{
	return sEnvPullNanos.load(std::memory_order_relaxed);
}

uint64_t
ShellMixNanosForTest(void)
{
	return sMixNanos.load(std::memory_order_relaxed);
}

uint64_t
ShellEchoNanosForTest(void)
{
	return sEchoNanos.load(std::memory_order_relaxed);
}

EnvState
ShellEnvStateForTest(void)
{
	return sEnvBank.state();
}

float
ShellEnvDecayForTest(int unit)
{
	AudioUnitParameterValue v = -1.0f;
	if(unit < 0 || unit >= NUM_ENVS || sEnvUnit[unit] == NULL || AudioUnitGetParameter(sEnvUnit[unit], kReverb2Param_DecayTimeAt0Hz, kAudioUnitScope_Global, 0, &v) != noErr)
		return -1.0f;
	return v;
}

void
ShellSpatialUnitForTest(int32_t *outputType, int32_t *algorithm, float *calib)
{
	EnsureControl();
	dispatch_sync(sControl, ^{
		*outputType = *algorithm = -1;
		*calib = sSpatialCalib;
		UInt32 v = 0, size = sizeof(v);
		if(sSpatial3D && AudioUnitGetProperty(sSpatial3D, kAudioUnitProperty_SpatialMixerOutputType, kAudioUnitScope_Global, 0, &v, &size) == noErr)
			*outputType = v == kSpatialMixerOutputType_Headphones ? OUTPUT_HEADPHONES : v == kSpatialMixerOutputType_BuiltInSpeakers ? OUTPUT_BUILTIN : OUTPUT_MULTICHANNEL;
		size = sizeof(v);
		if(sSpatial3D && AudioUnitGetProperty(sSpatial3D, kAudioUnitProperty_SpatializationAlgorithm, kAudioUnitScope_Input, 0, &v, &size) == noErr)
			*algorithm = v == kSpatializationAlgorithm_HRTFHQ ? kSpatialHRTFHQ : v == kSpatializationAlgorithm_UseOutputType ? kSpatialUseOutputType : 99;
	});
}

int32_t
ShellHeadTrackingForTest(void)
{
	EnsureControl();
	__block int32_t value = -1;
	dispatch_sync(sControl, ^{
		UInt32 v = 0, size = sizeof(v);
		if(sSpatial3D && AudioUnitGetProperty(sSpatial3D, kAudioUnitProperty_SpatialMixerEnableHeadTracking, kAudioUnitScope_Global, 0, &v, &size) == noErr)
			value = (int32_t)v;
	});
	return value;
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

bool
ShellFakeUnitPullMultiForTest(float *const *channels, uint32_t count, uint32_t frames)
{
	uint64_t beats = sDrive.UnitBeats();
	struct { AudioBufferList list; AudioBuffer more[15]; } abl;
	abl.list.mNumberBuffers = count;
	for(uint32_t c = 0; c < count; c++){
		abl.list.mBuffers[c].mNumberChannels = 1;
		abl.list.mBuffers[c].mDataByteSize = frames * 4;
		abl.list.mBuffers[c].mData = channels[c];
	}
	AudioUnitRenderActionFlags flags = 0;
	OutputRender(NULL, &flags, NULL, 0, frames, &abl.list);
	return sDrive.UnitBeats() != beats;
}

uint32_t
ShellLayoutTagForLabelsForTest(const uint32_t *labels, uint32_t count)
{
	std::vector<uint8_t> raw(offsetof(AudioChannelLayout, mChannelDescriptions) + count * sizeof(AudioChannelDescription));
	AudioChannelLayout *layout = (AudioChannelLayout *)raw.data();
	layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
	layout->mNumberChannelDescriptions = count;
	for(uint32_t i = 0; i < count; i++){
		memset(&layout->mChannelDescriptions[i], 0, sizeof(AudioChannelDescription));
		layout->mChannelDescriptions[i].mChannelLabel = labels[i];
	}
	return LayoutTagOf(layout, (UInt32)raw.size());
}

void
ShellMultichannelForTest(uint32_t *channels, uint32_t *layoutTag)
{
	EnsureControl();
	dispatch_sync(sControl, ^{
		*channels = *layoutTag = 0;
		if(sSpatial3D == NULL)
			return;
		AudioStreamBasicDescription f;
		UInt32 size = sizeof(f);
		if(AudioUnitGetProperty(sSpatial3D, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, &size) == noErr)
			*channels = f.mChannelsPerFrame;
		AudioChannelLayout layout;
		size = sizeof(layout);
		if(AudioUnitGetProperty(sSpatial3D, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Output, 0, &layout, &size) == noErr)
			*layoutTag = layout.mChannelLayoutTag;
	});
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

void
ShellFadeAtStopForTest(uint32_t *quiet, uint32_t *tail)
{
	*quiet = sStopQuiet;
	*tail = sStopTail;
}

void
ShellQueueLabelForTest(char *out, size_t size)
{
	strlcpy(out, sControl ? dispatch_queue_get_label(sControl) : "", size);
}

void
ShellDropControlForTest(void)
{
	if(sMixer == NULL)
		sControl = nil;
}

void
ShellConfigDeviceForTest(char *out, size_t size)
{
	strlcpy(out, sConfigDevice, size);
}

uint32_t
ShellDeviceCallsForTest(void)
{
	return sDeviceCalls.load(std::memory_order_relaxed);
}

uint32_t
ShellConfigCallsForTest(void)
{
	return sConfigCalls.load(std::memory_order_relaxed);
}

void
ShellFeedForTest(uint32_t chunkAt, uint32_t frames, uint32_t capacity, float *mono, float *stereoL, float *stereoR)
{
	UInt32 saved = sChunkAt;
	const float *saveL = sFeed3D[0], *saveR = sFeed3D[1];
	sChunkAt = chunkAt;
	sFeed3D[0] = sDryL + chunkAt;
	sFeed3D[1] = sDryR + chunkAt;
	AudioUnitRenderActionFlags flags = 0;
	struct { AudioBufferList list; AudioBuffer second; } abl;
	abl.list.mNumberBuffers = 1;
	abl.list.mBuffers[0].mNumberChannels = 1;
	abl.list.mBuffers[0].mDataByteSize = capacity * sizeof(float);
	abl.list.mBuffers[0].mData = mono;
	FeedMono((void *)(intptr_t)0, &flags, NULL, 0, frames, &abl.list);
	abl.list.mNumberBuffers = 2;
	abl.list.mBuffers[0].mData = stereoL;
	abl.second.mNumberChannels = 1;
	abl.second.mDataByteSize = capacity * sizeof(float);
	abl.second.mData = stereoR;
	FeedStereo((void *)sFeed3D, &flags, NULL, 0, frames, &abl.list);
	sFeed3D[0] = saveL;
	sFeed3D[1] = saveR;
	sChunkAt = saved;
}

uint64_t
ShellClassicFramesForTest(void)
{
	return sClassicFrames.load(std::memory_order_relaxed);
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
	i.outputType = sOutputType.load(std::memory_order_relaxed);
	i.fellBack = sFellBack;
	strncpy(i.device, sDeviceName, sizeof(i.device) - 1);
	i.device[sizeof(i.device) - 1] = '\0';
	return i;
}

int32_t
ShellOutputType(void)
{
	return sOutputType.load(std::memory_order_relaxed);
}

bool
ShellEchoReady(void)
{
	return sEchoReady.load(std::memory_order_relaxed) && !sSpatialFailed.load(std::memory_order_relaxed);
}

double
ShellRate(void)
{
	return sRateNow.load(std::memory_order_relaxed);
}

}
#endif
