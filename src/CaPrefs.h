#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "CaConfig.h"
#include "CaDevice.h"
#include "CaSpatial.h"

namespace ca {

struct AudioEnginePrefs
{
	int32_t mode;
	int32_t outputType;
	char outputDevice[128];
	int32_t algorithm;
};

extern AudioEnginePrefs gAudioEnginePrefs;

static const int32_t kModeUnknown = -1;
static const int32_t kAlgorithmUnknown = -1;
static const int32_t kDefaultMode = MODE_SPATIAL;

enum { kMaxProviders = 64 };

struct ProviderList
{
	int32_t count;
	uint32_t generation;
	char name[kMaxProviders][64];
	char base[kMaxProviders][64];
	char uid[kMaxProviders][128];
};

static const char *const kModeNames[NUM_MODES] = { "classic", "spatial", "reimagined" };
static const char *const kAlgorithmNames[2] = { "auto", "hrtfhq" };
static_assert(kSpatialUseOutputType == 0 && kSpatialHRTFHQ == 1, "algorithm names");
static const char *const kOutputNames[NUM_OUTPUTS] = { "auto", "headphones", "builtin", "external", "multichannel" };
static const char *const kOutputMenuLabels[NUM_OUTPUTS] = { "AUTO (NOT USED YET)", "HEADPHONES (NOT USED YET)", "BUILT-IN (NOT USED YET)",
	"EXTERNAL (NOT USED YET)", "MULTICHANNEL (NOT USED YET)" };

inline void
PrefsDefaults(AudioEnginePrefs &p)
{
	p.mode = kDefaultMode;
	p.outputType = OUTPUT_AUTO;
	strcpy(p.outputDevice, "default");
	p.algorithm = kSpatialUseOutputType;
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

inline const char *
OutputTypeMenuLabel(int32_t t)
{
	return t >= 0 && t < NUM_OUTPUTS ? kOutputMenuLabels[t] : kOutputMenuLabels[OUTPUT_AUTO];
}

inline int32_t
ResolveMode(int32_t requested, bool *fellBack)
{
	*fellBack = requested != MODE_CLASSIC && requested != MODE_SPATIAL;
	return requested == MODE_SPATIAL || requested == MODE_REIMAGINED ? MODE_SPATIAL : MODE_CLASSIC;
}

inline int32_t
OutputTypeFromSpeakers(int32_t speakers)
{
	return speakers == 1 ? OUTPUT_HEADPHONES : speakers == 2 ? OUTPUT_MULTICHANNEL : OUTPUT_AUTO;
}

inline int32_t
SpeakersFromOutputType(int32_t type)
{
	return type == OUTPUT_HEADPHONES ? 1 : type == OUTPUT_MULTICHANNEL ? 2 : 0;
}

inline int32_t
ReconcileOutputType(int32_t type, int32_t speakers)
{
	return SpeakersFromOutputType(type) == speakers ? type : OutputTypeFromSpeakers(speakers);
}

template<class Read>
inline void
PrefsLoad(AudioEnginePrefs &p, int8_t *speakers, Read read)
{
	char buf[128];
	if(read("Mode", buf, (int)sizeof(buf)))
		p.mode = ModeFromName(buf, kModeUnknown);
	if(read("OutputType", buf, (int)sizeof(buf))){
		p.outputType = OutputTypeFromName(buf, OUTPUT_AUTO);
		*speakers = (int8_t)SpeakersFromOutputType(p.outputType);
	}else
		p.outputType = OutputTypeFromSpeakers(*speakers);
	read("OutputDevice", p.outputDevice, (int)sizeof(p.outputDevice));
	if(read("SpatialAlgorithm", buf, (int)sizeof(buf)))
		p.algorithm = AlgorithmFromName(buf, kAlgorithmUnknown);
}

template<class Store>
inline void
PrefsSave(AudioEnginePrefs &p, int8_t speakers, Store store)
{
	p.outputType = ReconcileOutputType(p.outputType, speakers);
	store("Mode", ModeName(p.mode));
	store("OutputType", OutputTypeName(p.outputType));
	store("OutputDevice", FollowsDefault(p.outputDevice) ? "default" : p.outputDevice);
	store("SpatialAlgorithm", AlgorithmName(p.algorithm));
}

inline void
ProviderListReset(ProviderList &l, uint32_t generation)
{
	strcpy(l.name[0], "DEFAULT OUTPUT");
	strcpy(l.base[0], "DEFAULT OUTPUT");
	strcpy(l.uid[0], "default");
	l.count = 1;
	l.generation = generation;
}

inline bool
ProviderNameTaken(const ProviderList &l, const char *name)
{
	for(int32_t i = 0; i < l.count; i++)
		if(strcmp(l.name[i], name) == 0)
			return true;
	return false;
}

inline void
ProviderListAdd(ProviderList &l, const char *name, const char *uid)
{
	if(l.count >= kMaxProviders)
		return;
	snprintf(l.base[l.count], sizeof(l.base[l.count]), "%s", name);
	snprintf(l.uid[l.count], sizeof(l.uid[l.count]), "%s", uid);
	int32_t same = 1;
	for(int32_t i = 0; i < l.count; i++)
		same += strcmp(l.base[i], l.base[l.count]) == 0;
	snprintf(l.name[l.count], sizeof(l.name[l.count]), "%s", l.base[l.count]);
	if(same == 1 && ProviderNameTaken(l, l.name[l.count]))
		same = 2;
	for(int32_t number = same; number > 1; number++){
		char suffix[16];
		int len = snprintf(suffix, sizeof(suffix), " %d", number);
		snprintf(l.name[l.count], sizeof(l.name[l.count]), "%.*s%s", (int)sizeof(l.name[l.count]) - 1 - len, l.base[l.count], suffix);
		if(!ProviderNameTaken(l, l.name[l.count]))
			break;
	}
	l.count++;
}

inline int32_t
ProviderIndexClamped(const ProviderList &l, int32_t index)
{
	return index < 0 ? 0 : index >= l.count ? l.count - 1 : index;
}

inline int32_t
ProviderIndexForUid(const ProviderList &l, const char *uid)
{
	if(FollowsDefault(uid))
		return 0;
	for(int32_t i = 1; i < l.count; i++)
		if(strcmp(l.uid[i], uid) == 0)
			return i;
	return 0;
}

inline const char *
ProviderUid(const ProviderList &l, int32_t index)
{
	return l.uid[ProviderIndexClamped(l, index)];
}

inline bool
ProviderChoiceChanges(const char *pinned, const char *chosen)
{
	if(FollowsDefault(pinned) || FollowsDefault(chosen))
		return FollowsDefault(pinned) != FollowsDefault(chosen);
	return strcmp(pinned, chosen) != 0;
}

inline int32_t
ProviderIndexRemap(const ProviderList &from, const ProviderList &to, int32_t index, const char *currentUid)
{
	if(index < 0)
		return index;
	if(to.count == 0)
		return 0;
	if(index < from.count)
		for(int32_t i = 0; i < to.count; i++)
			if(strcmp(to.uid[i], from.uid[index]) == 0)
				return i;
	return ProviderIndexForUid(to, currentUid);
}

template<class Ch, class Measure>
inline void
FitMenuText(Ch *s, Measure width, float maxWidth)
{
	if(width(s) <= maxWidth)
		return;
	enum { kMax = 255, kSuffix = 15 };
	Ch orig[kMax + 1], suffix[kSuffix + 1];
	size_t n = 0;
	for(; s[n] != 0 && n < kMax; n++)
		orig[n] = s[n];
	orig[n] = 0;
	size_t keep = n, j = n;
	while(j > 0 && orig[j - 1] >= '0' && orig[j - 1] <= '9')
		j--;
	if(j < n && j > 1 && orig[j - 1] == ' ' && n - (j - 1) <= kSuffix)
		keep = j - 1;
	size_t sn = 0;
	for(size_t i = keep; i < n; i++)
		suffix[sn++] = orig[i];
	if(keep < n)
		for(size_t cut = keep; cut-- > 0;){
			size_t end = cut;
			while(end > 0 && orig[end - 1] == ' ')
				end--;
			if(end == 0)
				break;
			for(size_t i = 0; i < end; i++)
				s[i] = orig[i];
			for(size_t i = 0; i < sn; i++)
				s[end + i] = suffix[i];
			s[end + sn] = 0;
			if(width(s) <= maxWidth)
				return;
		}
	for(size_t i = 0; i <= n; i++)
		s[i] = orig[i];
	for(size_t len = n; len > 0 && width(s) > maxWidth;){
		s[--len] = 0;
		while(len > 0 && s[len - 1] == ' ')
			s[--len] = 0;
	}
}

}
