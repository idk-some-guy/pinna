#include <string.h>
#include "CaMixer.h"

namespace ca {

Shared::Shared(void) : latchedSerial(0), activeVoices(0), underruns(0), streamLatency(0), renders(0)
{
	for(int c = 0; c < kChannels; c++){
		finishedId[c].store(0);
		readerSlot[c].store(-1);
	}
}

Mixer::Mixer(Shared *shared) : mShared(shared), mShelfRate(0.0f)
{
	memset(mVoice, 0, sizeof(mVoice));
	memset(mLast, 0, sizeof(mLast));
	memset(mShelf, 0, sizeof(mShelf));
	for(int c = 0; c < kChannels; c++)
		mShelfHF[c] = -1.0f;
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
	if(outRate != mShelfRate)
		for(int c = 0; c < kChannels; c++)
			mShelfHF[c] = -1.0f;
	uint32_t active = 0;
	for(int c = 0; c < kChannels; c++){
		const VoiceSnap &sn = s.voice[c];
		VoiceState &st = mVoice[c];
		if(sn.playId != st.playId){
			StopVoice(st, mLast[c]);
			StartVoice(st, sn);
			if(!st.fadeLeft)
				mShelf[c].z1 = mShelf[c].z2 = 0.0f;
			mShared->readerSlot[c].store(st.active ? sn.poolSlot : -1, std::memory_order_release);
			if(!st.active)
				mShared->finishedId[c].store(st.playId, std::memory_order_release);
		}
		mLast[c] = sn;
		if(!sn.playing)
			StopVoice(st, sn);
		if(!st.active && !st.fadeLeft)
			continue;
		if(st.active)
			active++;
		Biquad &shelf = mShelf[c];
		if(sn.sendHF != mShelfHF[c]){
			HighShelf(shelf, 5000.0f, outRate, sn.sendHF);
			mShelfHF[c] = sn.sendHF;
		}
		for(uint32_t done = 0; done < frames && (st.active || st.fadeLeft);){
			uint32_t n = frames - done < (uint32_t)kMaxBlock ? frames - done : (uint32_t)kMaxBlock;
			bool sending = st.send != 0.0f || sn.send != 0.0f || st.fadeLeft != 0 || shelf.z1 != 0.0f || shelf.z2 != 0.0f;
			if(sending)
				memset(mVoiceSend, 0, n * sizeof(float));
			bool ended = RenderVoice(st, sn, outRate, dryL + done, dryR + done, mVoiceSend, n);
			if(sending){
				RunBiquad(shelf, mVoiceSend, n);
				if(isfinite(shelf.z1) && isfinite(shelf.z2))
					for(uint32_t i = 0; i < n; i++)
						send[done + i] += mVoiceSend[i];
				else
					shelf.z1 = shelf.z2 = 0.0f;
			}
			if(ended)
				mShared->finishedId[c].store(st.playId, std::memory_order_release);
			done += n;
		}
		if(!st.active)
			mShared->readerSlot[c].store(-1, std::memory_order_release);
	}
	uint32_t latency = mShared->streamLatency.load(std::memory_order_relaxed);
	for(int st = 0; st < kStreams; st++)
		mReader[st].render(s.stream[st], mShared->ring[st], mShared->status[st], mShared->underruns, outRate, latency, strL, strR, frames);
	mShelfRate = outRate;
	mShared->latchedSerial.store(s.serial, std::memory_order_release);
	mShared->activeVoices.store(active, std::memory_order_relaxed);
	mShared->renders.fetch_add(1, std::memory_order_release);
}

}
