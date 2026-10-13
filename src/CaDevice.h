#pragma once

#include <stdint.h>
#include <string.h>
#include "CaConfig.h"

namespace ca {

struct DeviceEntry
{
	uint32_t id;
	char uid[128];
	char name[64];
	uint32_t outputChannels;
	bool alive;
	uint32_t transport, terminal, dataSource;
};

constexpr uint32_t
FourCC(char a, char b, char c, char d)
{
	return (uint32_t)(uint8_t)a << 24 | (uint32_t)(uint8_t)b << 16 | (uint32_t)(uint8_t)c << 8 | (uint32_t)(uint8_t)d;
}

static const uint32_t kTransportBuiltIn = FourCC('b', 'l', 't', 'n');
static const uint32_t kTransportUSB = FourCC('u', 's', 'b', ' ');
static const uint32_t kTransportBluetooth = FourCC('b', 'l', 'u', 'e');
static const uint32_t kTransportBluetoothLE = FourCC('b', 'l', 'e', 'a');
static const uint32_t kTransportHDMI = FourCC('h', 'd', 'm', 'i');
static const uint32_t kTransportDisplayPort = FourCC('d', 'p', 'r', 't');
static const uint32_t kTransportVirtual = FourCC('v', 'i', 'r', 't');
static const uint32_t kTerminalHeadphones = FourCC('h', 'd', 'p', 'h');
static const uint32_t kTerminalSpeaker = FourCC('s', 'p', 'k', 'r');
static const uint32_t kTerminalLine = FourCC('l', 'i', 'n', 'e');
static const uint32_t kDataSourceHeadphones = FourCC('h', 'd', 'p', 'n');
static const uint32_t kDataSourceInternalSpeaker = FourCC('i', 's', 'p', 'k');

inline uint32_t
SummariseTerminals(const uint32_t *terminals, int count)
{
	for(int i = 0; i < count; i++)
		if(terminals[i] == kTerminalHeadphones)
			return kTerminalHeadphones;
	return count > 0 ? terminals[0] : 0;
}

inline int32_t
DetectOutputType(const DeviceEntry &d)
{
	if(d.terminal == kTerminalHeadphones || (d.transport == kTransportBuiltIn && d.dataSource == kDataSourceHeadphones))
		return OUTPUT_HEADPHONES;
	if(d.transport == kTransportBuiltIn && d.dataSource == kDataSourceInternalSpeaker)
		return OUTPUT_BUILTIN;
	bool usbOrBluetooth = d.transport == kTransportUSB || d.transport == kTransportBluetooth || d.transport == kTransportBluetoothLE;
	if(usbOrBluetooth && d.terminal == 0)
		return OUTPUT_HEADPHONES;
	return OUTPUT_EXTERNAL;
}

inline int32_t
EffectiveOutputType(int32_t pref, const DeviceEntry &d)
{
	if(pref == OUTPUT_MULTICHANNEL)
		return d.outputChannels > 2 ? OUTPUT_MULTICHANNEL : DetectOutputType(d);
	return pref > OUTPUT_AUTO && pref < NUM_OUTPUTS ? pref : DetectOutputType(d);
}

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
	EVENT_START,
	EVENT_RETRY,
	EVENT_NONE = -1
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
	if(currentId == 0)
		return true;
	switch(event){
	case EVENT_DEFAULT_CHANGED:
		return (followingDefault || !pinnedPresent) && eventId != currentId;
	case EVENT_RATE_CHANGED:
	case EVENT_DEVICE_GONE:
		return eventId == currentId;
	case EVENT_DEVICES_CHANGED:
		return !followingDefault && pinnedPresent && !pinnedIsCurrent;
	case EVENT_RETRY:
		return false;
	default:
		return true;
	}
}

inline int
DeviceBatchEvent(bool gone, bool formatTouched, double rate, uint32_t channels, double newRate, uint32_t newChannels)
{
	if(gone)
		return EVENT_DEVICE_GONE;
	return formatTouched && (newRate != rate || newChannels != channels) ? EVENT_RATE_CHANGED : EVENT_NONE;
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
	for(size_t i = 0; utf8[i] != '\0' && n + 1 < size; i++){
		unsigned char ch = (unsigned char)utf8[i];
		if(ch >= 0x80 && ch < 0xC0 && i > 0 && (unsigned char)utf8[i - 1] >= 0x80)
			continue;
		out[n++] = ch >= 0x80 ? '?' : ch == '~' ? ' ' : ch >= 'a' && ch <= 'z' ? (char)(ch - 'a' + 'A') : (char)ch;
	}
	out[n] = '\0';
}

}
