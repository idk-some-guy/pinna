#pragma once

#include <stdint.h>
#include <string.h>

namespace ca {

struct DeviceEntry
{
	uint32_t id;
	char uid[128];
	char name[64];
	uint32_t outputChannels;
	bool alive;
};

struct DeviceChoice
{
	uint32_t id;
	bool fellBack;
};

struct PinState
{
	bool present;
	bool isCurrent;
};

enum
{
	EVENT_DEFAULT_CHANGED = 0,
	EVENT_RATE_CHANGED,
	EVENT_DEVICE_GONE,
	EVENT_DEVICES_CHANGED,
	EVENT_WAKE,
	EVENT_PIN,
	EVENT_START
};

inline bool
FollowsDefault(const char *pinnedUid)
{
	return pinnedUid == 0 || pinnedUid[0] == '\0' || strcmp(pinnedUid, "default") == 0;
}

inline bool
Usable(const DeviceEntry &d)
{
	return d.alive && d.outputChannels > 0;
}

inline DeviceChoice
ChooseDevice(const char *pinnedUid, const DeviceEntry *list, int count, uint32_t defaultId)
{
	DeviceChoice c = { defaultId, false };
	if(FollowsDefault(pinnedUid))
		return c;
	for(int i = 0; i < count; i++)
		if(Usable(list[i]) && strcmp(list[i].uid, pinnedUid) == 0){
			c.id = list[i].id;
			return c;
		}
	c.fellBack = true;
	return c;
}

inline PinState
FindPin(const char *pinnedUid, const DeviceEntry *list, int count, uint32_t currentId)
{
	PinState p = { false, false };
	if(FollowsDefault(pinnedUid))
		return p;
	for(int i = 0; i < count; i++)
		if(Usable(list[i]) && strcmp(list[i].uid, pinnedUid) == 0){
			p.present = true;
			p.isCurrent = list[i].id == currentId;
		}
	return p;
}

inline bool
NeedsRebuild(int event, bool followingDefault, uint32_t currentId, uint32_t eventId, bool pinnedPresent, bool pinnedIsCurrent)
{
	switch(event){
	case EVENT_DEFAULT_CHANGED:
		return followingDefault || !pinnedPresent;
	case EVENT_RATE_CHANGED:
	case EVENT_DEVICE_GONE:
		return eventId == currentId;
	case EVENT_DEVICES_CHANGED:
		return !followingDefault && pinnedPresent && !pinnedIsCurrent;
	default:
		return true;
	}
}

inline bool
DecideRebuild(int event, const char *pinnedUid, const DeviceEntry *list, int count, uint32_t currentId, uint32_t eventId)
{
	PinState p = FindPin(pinnedUid, list, count, currentId);
	return NeedsRebuild(event, FollowsDefault(pinnedUid), currentId, eventId, p.present, p.isCurrent);
}

inline void
MenuName(const char *utf8, char *out, size_t size)
{
	size_t n = 0;
	for(; utf8[n] != '\0' && n + 1 < size; n++){
		unsigned char ch = (unsigned char)utf8[n];
		out[n] = ch >= 0x80 ? '?' : ch >= 'a' && ch <= 'z' ? (char)(ch - 'a' + 'A') : (char)ch;
	}
	out[n] = '\0';
}

}
