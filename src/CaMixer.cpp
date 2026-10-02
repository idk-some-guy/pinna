#include <string.h>
#include "CaMixer.h"

namespace ca {

Shared::Shared(void) : activeVoices(0), underruns(0)
{
	for(int c = 0; c < kChannels; c++){
		finishedId[c].store(0);
		readerSlot[c].store(-1);
	}
}

Mixer::Mixer(Shared *shared) : mShared(shared)
{
	memset(mVoice, 0, sizeof(mVoice));
}

void
Mixer::render(float *dryL, float *dryR, float *send, float *strL, float *strR, uint32_t frames, float outRate)
{
	memset(dryL, 0, frames * sizeof(float));
	memset(dryR, 0, frames * sizeof(float));
	memset(send, 0, frames * sizeof(float));
	memset(strL, 0, frames * sizeof(float));
	memset(strR, 0, frames * sizeof(float));
	const Snapshot &s = mShared->snapshot.read();
	uint32_t active = 0;
	for(int c = 0; c < kChannels; c++){
		const VoiceSnap &sn = s.voice[c];
		VoiceState &st = mVoice[c];
		if(sn.playId != st.playId){
			StartVoice(st, sn);
			mShared->readerSlot[c].store(st.active ? sn.poolSlot : -1, std::memory_order_release);
		}
		if(!st.active)
			continue;
		active++;
		if(RenderVoice(st, sn, outRate, dryL, dryR, send, frames))
			mShared->finishedId[c].store(st.playId, std::memory_order_release);
		if(!st.active)
			mShared->readerSlot[c].store(-1, std::memory_order_release);
	}
	mShared->activeVoices.store(active, std::memory_order_relaxed);
}

}
