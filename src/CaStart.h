#pragma once

#include <string.h>
#include "CaMixer.h"
#include "CaRules.h"

namespace ca {

struct PoolStart
{
	const int16_t *data;
	uint32_t length;
	int8_t slot;
};

inline void
ResetPoolRefs(uint32_t *slotRef)
{
	for(int s = 0; s < kPoolSlots; s++)
		slotRef[s] = UINT32_MAX;
}

inline void
PublishMirror(Snapshot &mirror, Shared &shared, uint32_t *slotRef)
{
	mirror.serial++;
	shared.snapshot.writeBuffer() = mirror;
	shared.snapshot.publish();
	uint32_t latched = shared.latchedSerial.load(std::memory_order_acquire);
	for(int s = 0; s < kPoolSlots; s++)
		if((int32_t)(latched - slotRef[s]) > 0)
			slotRef[s] = latched - 1;
	for(int c = 0; c < kChannels; c++)
		if(mirror.voice[c].poolSlot >= 0)
			slotRef[mirror.voice[c].poolSlot] = mirror.serial;
}

inline bool
StartFromPool(const Snapshot &mirror, const Shared &shared, const uint32_t *slotRef, int16_t *pool, uint32_t slotSamples, int *cursor, const int16_t *src, uint32_t samples, PoolStart *out)
{
	int8_t held[kChannels];
	for(int c = 0; c < kChannels; c++)
		held[c] = mirror.voice[c].poolSlot;
	int s = PickPoolSlot(held, shared.readerSlot, slotRef, shared.latchedSerial.load(std::memory_order_acquire), kChannels, kPoolSlots, cursor);
	if(s < 0)
		return false;
	uint32_t n = samples < slotSamples ? samples : slotSamples;
	int16_t *dst = pool + (size_t)s * slotSamples;
	memcpy(dst, src, n * sizeof(int16_t));
	out->data = dst;
	out->length = n;
	out->slot = (int8_t)s;
	return true;
}

inline uint32_t
BankSampleBytes(uint32_t offset, uint32_t size, uint32_t blockBytes)
{
	if(offset >= blockBytes)
		return 0;
	return size < blockBytes - offset ? size : blockBytes - offset;
}

inline uint32_t
PedCommentBytes(uint32_t offset, uint32_t size, uint32_t blockBytes, uint32_t slotBytes)
{
	uint32_t n = BankSampleBytes(offset, size, blockBytes);
	return n < slotBytes ? n : slotBytes;
}

inline void
ArmVoice(VoiceSnap &v, const int16_t *data, uint32_t length, LoopRange r, bool loop, int8_t slot, uint32_t playId)
{
	v.data = data;
	v.length = length;
	v.loopStart = r.start;
	v.loopEnd = r.end;
	v.loop = loop;
	v.poolSlot = slot;
	v.playId = playId;
	v.playing = length > 0;
}

inline bool
ChannelUsed(const Snapshot &mirror, const Shared &shared, int c)
{
	const VoiceSnap &v = mirror.voice[c];
	return v.playing && shared.finishedId[c].load(std::memory_order_acquire) != v.playId;
}

}
