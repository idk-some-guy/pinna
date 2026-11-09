#pragma once

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "CaConfig.h"
#include "CaDevice.h"
#include "CaSpatial.h"

namespace pinna {

struct AudioEnginePrefs
{
	int32_t mode;
	int32_t outputType;
	char outputDevice[128];
	int32_t algorithm;
	bool reverb;
	bool echo;
	bool headTracking;
	float reverbAmount;
};

static const int32_t kModeUnknown = -1;
static const int32_t kAlgorithmUnknown = -1;
static const int32_t kDefaultMode = MODE_SPATIAL;
static const bool kDefaultReverb = true;
static const bool kDefaultEcho = true;
static const bool kDefaultHeadTracking = false;
static const float kDefaultReverbAmount = 1.0f;

static const char *const kModeNames[NUM_MODES] = { "classic", "spatial", "reimagined" };
static const char *const kAlgorithmNames[2] = { "auto", "hrtfhq" };
static_assert(kSpatialUseOutputType == 0 && kSpatialHRTFHQ == 1, "algorithm names");
static const char *const kOutputNames[NUM_OUTPUTS] = { "auto", "headphones", "builtin", "external", "multichannel" };

inline void
PrefsDefaults(AudioEnginePrefs &p)
{
	p.mode = kDefaultMode;
	p.outputType = OUTPUT_AUTO;
	strcpy(p.outputDevice, "default");
	p.algorithm = kSpatialUseOutputType;
	p.reverb = kDefaultReverb;
	p.echo = kDefaultEcho;
	p.headTracking = kDefaultHeadTracking;
	p.reverbAmount = kDefaultReverbAmount;
}

inline void
PrefsRestoreDefaults(AudioEnginePrefs &p)
{
	AudioEnginePrefs kept = p;
	PrefsDefaults(p);
	p.reverb = kept.reverb;
	p.echo = kept.echo;
	p.headTracking = kept.headTracking;
	p.reverbAmount = kept.reverbAmount;
}

inline const char *
AlgorithmName(int32_t a)
{
	return a == kSpatialHRTFHQ ? kAlgorithmNames[1] : kAlgorithmNames[0];
}

inline int32_t
AlgorithmFromName(const char *s, int32_t fallback)
{
	for(int32_t i = 0; i < 2; i++)
		if(strcasecmp(s, kAlgorithmNames[i]) == 0)
			return i;
	return fallback;
}

inline int32_t
ResolveAlgorithm(int32_t requested, bool *fellBack)
{
	*fellBack = requested != kSpatialUseOutputType && requested != kSpatialHRTFHQ;
	return *fellBack ? kSpatialUseOutputType : requested;
}

inline const char *
ModeName(int32_t m)
{
	return m >= 0 && m < NUM_MODES ? kModeNames[m] : kModeNames[MODE_CLASSIC];
}

inline int32_t
ModeFromName(const char *s, int32_t fallback)
{
	for(int32_t i = 0; i < NUM_MODES; i++)
		if(strcasecmp(s, kModeNames[i]) == 0)
			return i;
	return fallback;
}

inline const char *
OutputTypeName(int32_t t)
{
	return t >= 0 && t < NUM_OUTPUTS ? kOutputNames[t] : kOutputNames[OUTPUT_AUTO];
}

inline int32_t
OutputTypeFromName(const char *s, int32_t fallback)
{
	for(int32_t i = 0; i < NUM_OUTPUTS; i++)
		if(strcasecmp(s, kOutputNames[i]) == 0)
			return i;
	return fallback;
}


inline int32_t
ResolveMode(int32_t requested, bool *fellBack)
{
	*fellBack = requested != MODE_CLASSIC && requested != MODE_SPATIAL;
	return requested == MODE_SPATIAL || requested == MODE_REIMAGINED ? MODE_SPATIAL : MODE_CLASSIC;
}

inline int32_t
NextOutputType(int32_t type, int32_t step)
{
	if(type < 0 || type >= NUM_OUTPUTS)
		type = OUTPUT_AUTO;
	return ((type + step) % NUM_OUTPUTS + NUM_OUTPUTS) % NUM_OUTPUTS;
}

inline int32_t
ToggledMode(int32_t stored)
{
	bool fellBack;
	return ResolveMode(stored, &fellBack) == MODE_SPATIAL ? MODE_CLASSIC : MODE_SPATIAL;
}

inline bool
OnOffFromName(const char *s, bool fallback)
{
	if(strcasecmp(s, "on") == 0 || strcmp(s, "1") == 0 || strcasecmp(s, "true") == 0)
		return true;
	if(strcasecmp(s, "off") == 0 || strcmp(s, "0") == 0 || strcasecmp(s, "false") == 0)
		return false;
	return fallback;
}

inline float
ClampReverbAmount(float v)
{
	return !isfinite(v) ? kDefaultReverbAmount : v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

inline float
ReverbAmountFromText(const char *s, float fallback)
{
	char *end;
	float v = strtof(s, &end);
	if(end == s || *end != '\0' || !isfinite(v))
		return fallback;
	return ClampReverbAmount(v);
}

inline const char *
HrtfReadingName(int32_t v)
{
	return v < 0 ? "not read" : v ? "on" : "off";
}

inline void
FormatEngineState(char *out, size_t size, int32_t mode, int32_t algorithm, int32_t outputType, bool unitUnavailable, int32_t hrtfAtStart,
	int32_t hrtfAfterSecond)
{
	const char *unit = !OutputRoutesSpatial(outputType) ? "not built (speakers)" : unitUnavailable ? "unavailable" : "available";
	snprintf(out, size, "audio state: mode %s, algorithm %s, output %s, spatial unit %s, personalised HRTF %s at start and %s after 1 s",
		ModeName(mode), AlgorithmName(algorithm), OutputTypeName(outputType), unit, HrtfReadingName(hrtfAtStart), HrtfReadingName(hrtfAfterSecond));
}

template<class Read>
inline bool
PrefsLoad(AudioEnginePrefs &p, Read read)
{
	char buf[128];
	if(read("Mode", buf, (int)sizeof(buf)))
		p.mode = ModeFromName(buf, kModeUnknown);
	bool typeRead = read("OutputType", buf, (int)sizeof(buf));
	if(typeRead)
		p.outputType = OutputTypeFromName(buf, OUTPUT_AUTO);
	read("OutputDevice", p.outputDevice, (int)sizeof(p.outputDevice));
	if(read("SpatialAlgorithm", buf, (int)sizeof(buf)))
		p.algorithm = AlgorithmFromName(buf, kAlgorithmUnknown);
	if(read("Reverb", buf, (int)sizeof(buf)))
		p.reverb = OnOffFromName(buf, kDefaultReverb);
	if(read("Echo", buf, (int)sizeof(buf)))
		p.echo = OnOffFromName(buf, kDefaultEcho);
	if(read("HeadTracking", buf, (int)sizeof(buf)))
		p.headTracking = OnOffFromName(buf, kDefaultHeadTracking);
	if(read("ReverbAmount", buf, (int)sizeof(buf)))
		p.reverbAmount = ReverbAmountFromText(buf, kDefaultReverbAmount);
	return typeRead;
}

template<class Store>
inline void
PrefsSave(const AudioEnginePrefs &p, Store store)
{
	store("Mode", ModeName(p.mode));
	store("OutputType", OutputTypeName(p.outputType));
	store("OutputDevice", FollowsDefault(p.outputDevice) ? "default" : p.outputDevice);
	store("SpatialAlgorithm", AlgorithmName(p.algorithm));
	store("Reverb", p.reverb ? "on" : "off");
	store("Echo", p.echo ? "on" : "off");
	store("HeadTracking", p.headTracking ? "on" : "off");
	char amount[16];
	snprintf(amount, sizeof(amount), "%.3f", ClampReverbAmount(p.reverbAmount));
	store("ReverbAmount", amount);
}

}
