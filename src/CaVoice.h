#pragma once

#include <stdint.h>
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
};

struct VoiceState
{
	uint32_t playId;
	bool active;
	bool wrapped;
	double pos;
	double step, stepTarget, stepInc;
	uint32_t stepLeft;
	float gainL, gainR, send;
	float targetL, targetR, targetSend;
	float incL, incR, incSend;
	uint32_t gainLeft;
};

uint32_t RampFrames(float outRate);
void StartVoice(VoiceState &st, const VoiceSnap &sn);
bool RenderVoice(VoiceState &st, const VoiceSnap &sn, float outRate, float *accL, float *accR, float *accSend, uint32_t frames);

}
