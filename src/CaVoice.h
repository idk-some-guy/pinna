#pragma once

#include <stdint.h>
#include "CaConfig.h"
#include "CaResampler.h"

namespace ca {

struct VoiceSnap
{
	const int16_t *data;
	uint32_t length;
	uint32_t loopStart, loopEnd;
	uint32_t playId;
	float freq;
	float gainL, gainR, send;
	uint8_t loop, playing;
	int8_t poolSlot;
	float sendHF;
	uint32_t baseRate;
};

struct VoiceState
{
	uint32_t playId;
	bool active;
	bool wrapped;
	double pos;
	double step;
	float gainL, gainR, send;
	float targetL, targetR, targetSend;
	float incL, incR, incSend;
	uint32_t gainLeft;
	float fade[3][kOalFadeFrames];
	uint32_t fadeLeft;
};

void StartVoice(VoiceState &st, const VoiceSnap &sn);
void StopVoice(VoiceState &st, const VoiceSnap &sn);
bool RenderVoice(VoiceState &st, const VoiceSnap &sn, float outRate, float *accL, float *accR, float *accSend, uint32_t frames);

}
