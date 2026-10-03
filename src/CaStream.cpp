#include <math.h>
#include <string.h>
#include <chrono>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include "CaStream.h"

namespace ca {

static_assert((kWindowFrames & (kWindowFrames - 1)) == 0, "the window is indexed with a mask");
static_assert(kWindowFrames >= kReaderSlice * kOalMaxPitch + 2 * kSincMaxTaps + kChunkFrames, "a slice at the highest step fits the window");

StreamReader::StreamReader(void) : mFadeLeft(0)
{
	reset(0);
}

void
StreamReader::reset(uint32_t gen)
{
	mGen = gen;
	mWinEnd = 0;
	mPos = 0.0;
	mStep = 0.0;
	mRate = 0;
	mLength = 0;
	mStarted = mEof = mDrained = mLoop = mAudible = false;
	mFresh = true;
	mGainLeft = 0;
	mNumSeg = 0;
	mNumHist = 0;
	mOut = 0;
}

float
StreamReader::fetch(int ch, int64_t idx) const
{
	if(idx < 0)
		return 0.0f;
	if((uint64_t)idx >= mWinEnd){
		if(!mEof || mWinEnd == 0)
			return 0.0f;
		idx = (int64_t)mWinEnd - 1;
	}
	if((uint64_t)idx + kWindowFrames < mWinEnd)
		return 0.0f;
	return mWin[ch][idx & (kWindowFrames - 1)];
}

void
StreamReader::fill(ChunkRing &ring, uint64_t need)
{
	while(mWinEnd < need && !mEof){
		const Chunk *c = ring.peek();
		if(c == 0)
			return;
		if(c->gen != mGen)
			return;
		int64_t keep = (int64_t)mPos - kSincMaxTaps;
		if(mWinEnd + c->frames > (uint64_t)(keep > 0 ? keep : 0) + kWindowFrames)
			return;
		if(!mStarted){
			mStarted = true;
			mRate = c->rate;
			mLoop = c->loop != 0;
			mLength = c->length;
		}
		if(c->frames){
			Segment &sg = mSeg[mNumSeg % kSegments];
			sg.at = mWinEnd;
			sg.src = c->srcFrame;
			mNumSeg++;
			for(uint32_t i = 0; i < c->frames; i++){
				uint32_t w = (uint32_t)((mWinEnd + i) & (kWindowFrames - 1));
				mWin[0][w] = c->pcm[i * 2];
				mWin[1][w] = c->pcm[i * 2 + 1];
			}
			mWinEnd += c->frames;
		}
		if(c->eof)
			mEof = true;
		ring.pop();
	}
}

void
StreamReader::fade(ChunkRing *paused)
{
	if(!mAudible)
		return;
	mAudible = false;
	if(mStep <= 0.0)
		return;
	if(paused){
		fadePaused(*paused);
		return;
	}
	int64_t i = (int64_t)mPos, heldAt[2];
	float held[2];
	for(int ch = 0; ch < 2; ch++){
		held[ch] = fetch(ch, i);
		heldAt[ch] = i;
		for(int k = 1; k < kOalResamplerEdge; k++){
			float v = fetch(ch, i + k);
			if(fabsf(v) < fabsf(held[ch])){
				held[ch] = v;
				heldAt[ch] = i + k;
			}
		}
	}
	float next[2][kOalFadeFrames], w[kSincMaxTaps];
	uint32_t at = kOalFadeFrames - mFadeLeft;
	double p = mPos;
	bool flat[2] = { false, false };
	for(uint32_t k = 0; k < kOalFadeFrames; k++, p += mStep){
		int64_t j = (int64_t)p;
		int first;
		int taps = SincWeights(p - (double)j, mStep, w, &first);
		float r[2];
		for(int ch = 0; ch < 2; ch++){
			r[ch] = held[ch];
			if(flat[ch])
				continue;
			if(j + first > heldAt[ch]){
				flat[ch] = true;
				continue;
			}
			r[ch] = 0.0f;
			for(int t = 0; t < taps; t++)
				r[ch] += (j + first + t > heldAt[ch] ? held[ch] : fetch(ch, j + first + t)) * w[t];
		}
		float g = (float)(kOalFadeFrames - k) / kOalFadeFrames;
		bool old = k < mFadeLeft;
		next[0][k] = (r[0] * mGain[0] + r[1] * mGain[2]) * g + (old ? mFade[0][at + k] : 0.0f);
		next[1][k] = (r[0] * mGain[1] + r[1] * mGain[3]) * g + (old ? mFade[1][at + k] : 0.0f);
	}
	memcpy(mFade, next, sizeof(next));
	mFadeLeft = kOalFadeFrames;
}

void
StreamReader::fadePaused(ChunkRing &ring)
{
	if(!mEof)
		fill(ring, (uint64_t)(mPos + kOalFadeFrames * mStep) + SincHalfFor(mStep) + 2);
	float next[2][kOalFadeFrames], w[kSincMaxTaps];
	uint32_t at = kOalFadeFrames - mFadeLeft;
	double p = mPos;
	for(uint32_t k = 0; k < kOalFadeFrames; k++, p += mStep){
		int64_t j = (int64_t)p;
		int first;
		int taps = SincWeights(p - (double)j, mStep, w, &first);
		float r[2] = { 0.0f, 0.0f };
		for(int t = 0; t < taps; t++){
			r[0] += fetch(0, j + first + t) * w[t];
			r[1] += fetch(1, j + first + t) * w[t];
		}
		float g = (float)(kOalFadeFrames - k) / kOalFadeFrames;
		bool old = k < mFadeLeft;
		next[0][k] = (r[0] * mGain[0] + r[1] * mGain[2]) * g + (old ? mFade[0][at + k] : 0.0f);
		next[1][k] = (r[0] * mGain[1] + r[1] * mGain[3]) * g + (old ? mFade[1][at + k] : 0.0f);
	}
	memcpy(mFade, next, sizeof(next));
	mFadeLeft = kOalFadeFrames;
}

uint64_t
StreamReader::sourceAt(uint64_t at) const
{
	uint32_t n = mNumSeg < (uint32_t)kSegments ? mNumSeg : (uint32_t)kSegments;
	const Segment *s = &mSeg[(mNumSeg - 1) % kSegments];
	for(uint32_t k = 1; k < n && s->at > at; k++)
		s = &mSeg[(mNumSeg - 1 - k) % kSegments];
	uint64_t src = s->src + (at > s->at ? at - s->at : 0);
	if(mLoop)
		return mLength ? src % mLength : src;
	return mLength && src > mLength ? mLength : src;
}

double
StreamReader::audibleAt(uint64_t out) const
{
	uint32_t n = mNumHist < (uint32_t)kHistory ? mNumHist : (uint32_t)kHistory;
	if(n == 0)
		return mPos;
	const Block *b = &mHist[(mNumHist - 1) % kHistory];
	for(uint32_t k = 1; k < n && b->outStart > out; k++)
		b = &mHist[(mNumHist - 1 - k) % kHistory];
	if(out <= b->outStart)
		return b->posStart;
	uint64_t d = out - b->outStart;
	return b->posStart + (double)(d < b->advanced ? d : b->advanced) * b->step;
}

void
StreamReader::render(const StreamSnap &p, ChunkRing &ring, StreamStatus &st, std::atomic<uint32_t> &underruns, float outRate, uint32_t latency,
	float *accL, float *accR, uint32_t frames)
{
	uint32_t posted = st.postedGen.load(std::memory_order_acquire);
	if(p.gen != mGen){
		fade();
		reset(p.gen);
	}
	bool superseded = posted && posted != mGen && (mGen == 0 || GenBefore(mGen, posted));
	uint32_t cut = superseded ? posted : mGen;
	for(const Chunk *c = ring.peek(); c && cut && GenBefore(c->gen, cut); c = ring.peek())
		ring.pop();
	bool want = p.playing && !p.paused && !mDrained && !superseded;
	if(!want)
		fade(p.playing && !mDrained && !superseded ? &ring : 0);
	if(want && !mStarted)
		fill(ring, 1);
	want = want && mStarted;
	if(want){
		mStep = OalStreamStep(mRate, outRate);
		float target[4] = { p.ll, p.lr, p.rl, p.rr };
		if(mFresh){
			for(int k = 0; k < 4; k++)
				mGain[k] = mTarget[k] = target[k];
			mGainLeft = 0;
			mFresh = false;
		}else if(!mAudible){
			for(int k = 0; k < 4; k++){
				mTarget[k] = target[k];
				mGain[k] = 0.0f;
				mInc[k] = target[k] / kOalFadeFrames;
			}
			mGainLeft = kOalFadeFrames;
		}else if(memcmp(target, mTarget, sizeof(target)) != 0){
			for(int k = 0; k < 4; k++){
				mTarget[k] = target[k];
				mInc[k] = (target[k] - mGain[k]) / kOalFadeFrames;
			}
			mGainLeft = kOalFadeFrames;
		}
	}
	double posStart = mPos;
	uint32_t advanced = 0;
	float w[kSincMaxTaps];
	for(uint32_t done = 0; done < frames;){
		uint32_t n = frames - done < (uint32_t)kReaderSlice ? frames - done : (uint32_t)kReaderSlice;
		if(want && !mEof){
			uint64_t need = (uint64_t)(mPos + n * mStep) + SincHalfFor(mStep) + 2;
			fill(ring, need);
			if(mWinEnd < need && !mEof){
				underruns.fetch_add(1, std::memory_order_relaxed);
				fade();
				want = false;
			}
		}
		for(uint32_t k = 0; k < n; k++){
			uint32_t o = done + k;
			if(mFadeLeft){
				uint32_t f = kOalFadeFrames - mFadeLeft--;
				accL[o] += mFade[0][f];
				accR[o] += mFade[1][f];
			}
			if(!want)
				continue;
			if(mEof && mPos >= (double)mWinEnd){
				mDrained = true;
				st.drainedGen.store(mGen, std::memory_order_release);
				fade();
				want = false;
				continue;
			}
			int64_t i = (int64_t)mPos;
			int first;
			int taps = SincWeights(mPos - (double)i, mStep, w, &first);
			int64_t base = i + first;
			float s0 = 0.0f, s1 = 0.0f;
			if(base >= 0 && (uint64_t)(base + taps) <= mWinEnd)
				for(int t = 0; t < taps; t++){
					uint32_t x = (uint32_t)((base + t) & (kWindowFrames - 1));
					s0 += mWin[0][x] * w[t];
					s1 += mWin[1][x] * w[t];
				}
			else
				for(int t = 0; t < taps; t++){
					s0 += fetch(0, base + t) * w[t];
					s1 += fetch(1, base + t) * w[t];
				}
			float g0 = mGain[0], g1 = mGain[1], g2 = mGain[2], g3 = mGain[3];
			if(mGainLeft){
				if(--mGainLeft)
					for(int c = 0; c < 4; c++)
						mGain[c] += mInc[c];
				else
					memcpy(mGain, mTarget, sizeof(mGain));
			}
			accL[o] += s0 * g0 + s1 * g2;
			accR[o] += s0 * g1 + s1 * g3;
			mPos += mStep;
			advanced++;
			mAudible = true;
		}
		done += n;
	}
	Block &b = mHist[mNumHist % kHistory];
	b.outStart = mOut;
	b.posStart = posStart;
	b.step = mStep;
	b.advanced = advanced;
	mNumHist++;
	mOut += frames;
	if(mStarted && mNumSeg){
		uint64_t frame = sourceAt((uint64_t)audibleAt(mOut > latency ? mOut - latency : 0));
		st.format.store((uint64_t)mGen << 32 | mRate, std::memory_order_relaxed);
		st.pos.store((uint64_t)mGen << 32 | (uint32_t)frame, std::memory_order_release);
	}
}

StreamWorker::StreamWorker(ChunkRing *rings, StreamStatus *status, DecoderFactory factory, bool threaded)
	: mRings(rings), mStatus(status), mFactory(factory), mPending(false), mQuit(false), mWakes(0), mStarved(false)
{
	memset(mSlot, 0, sizeof(mSlot));
	if(threaded)
		mThread = std::thread(&StreamWorker::run, this);
}

StreamWorker::~StreamWorker(void)
{
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mQuit.store(true, std::memory_order_release);
	}
	mCv.notify_all();
	if(mThread.joinable())
		mThread.join();
	for(int s = 0; s < kStreams; s++)
		delete mSlot[s].dec;
}

void
StreamWorker::post(const StreamCommand &c)
{
	mStatus[c.stream].postedGen.store(c.gen, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mQueue.push_back(c);
		mPending.store(true, std::memory_order_release);
	}
	mCv.notify_one();
}

bool
StreamWorker::waitPrefilled(uint8_t stream, uint32_t gen, int timeoutMs)
{
	std::unique_lock<std::mutex> lock(mMutex);
	return mDoneCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
		return mStatus[stream].prefilledGen.load(std::memory_order_acquire) == gen || mStatus[stream].failedGen.load(std::memory_order_acquire) == gen;
	});
}

void
StreamWorker::position(Slot &s, uint32_t ms)
{
	uint64_t f = MsToFrames(ms, s.rate);
	if(s.loop){
		f %= s.length;
		s.next = (f + s.length - s.lead % s.length) % s.length;
		s.dec->seekFrame(s.next, false);
	}else if(f >= s.length){
		s.next = s.length;
		s.ending = true;
	}else{
		s.next = f;
		s.dec->seekFrame(f, true);
	}
}

void
StreamWorker::apply(const StreamCommand &c)
{
	Slot &s = mSlot[c.stream];
	StreamStatus &st = mStatus[c.stream];
	s.gen = c.gen;
	s.mark = mRings[c.stream].mark();
	s.done = s.ending = s.prefilled = false;
	s.pass = s.written = 0;
	if(c.op == CMD_CLOSE){
		delete s.dec;
		s.dec = 0;
		s.done = true;
		return;
	}
	if(c.op == CMD_OPEN){
		delete s.dec;
		s.dec = mFactory(c.path);
		if(s.dec){
			s.rate = s.dec->rate();
			s.length = s.dec->lengthFrames();
			s.lead = s.dec->leadFrames();
		}
	}
	if(s.dec == 0 || s.rate == 0 || s.length == 0){
		s.loop = false;
		s.next = 0;
		s.ending = true;
		st.failedGen.store(c.gen, std::memory_order_release);
		std::lock_guard<std::mutex> lock(mMutex);
		mDoneCv.notify_all();
		return;
	}
	s.loop = c.loop != 0;
	position(s, c.startMs);
}

bool
StreamWorker::fill(int index)
{
	Slot &s = mSlot[index];
	ChunkRing &ring = mRings[index];
	if(s.done || (s.dec == 0 && !s.ending))
		return false;
	bool starved = false;
	uint32_t target = (s.rate * 3 / 2 + kChunkFrames - 1) / kChunkFrames;
	if(target > (uint32_t)kRingChunks)
		target = kRingChunks;
	while(!s.done && !mPending.load(std::memory_order_acquire) && !mQuit.load(std::memory_order_acquire)){
		if(!s.ending && ring.filledSince(s.mark) >= target)
			break;
		Chunk *c = ring.beginWrite();
		if(c == 0){
			starved = true;
			break;
		}
		c->gen = s.gen;
		c->rate = s.rate;
		c->loop = s.loop;
		c->length = s.length;
		c->eof = 0;
		c->srcFrame = s.loop ? (s.next + s.lead) % s.length : s.next;
		uint32_t got = s.ending ? 0 : s.dec->read(c->pcm, kChunkFrames);
		s.next += got;
		s.pass += got;
		s.written += got;
		if(got < (uint32_t)kChunkFrames){
			if(s.loop && !s.ending && s.pass > 0){
				s.dec->seekFrame(0, false);
				s.next = s.pass = 0;
				if(got == 0)
					continue;
			}else{
				c->eof = 1;
				s.done = true;
			}
		}
		c->frames = got;
		ring.endWrite();
	}
	if(!s.prefilled && (s.done || s.written >= s.rate)){
		s.prefilled = true;
		mStatus[index].prefilledGen.store(s.gen, std::memory_order_release);
		std::lock_guard<std::mutex> lock(mMutex);
		mDoneCv.notify_all();
	}
	return starved && (s.ending || ring.filledSince(s.mark) < target);
}

bool
StreamWorker::active(void) const
{
	for(int s = 0; s < kStreams; s++)
		if(!mSlot[s].done && (mSlot[s].dec || mSlot[s].ending))
			return true;
	return false;
}

void
StreamWorker::step(void)
{
	std::deque<StreamCommand> todo;
	{
		std::lock_guard<std::mutex> lock(mMutex);
		todo.swap(mQueue);
		mPending.store(false, std::memory_order_relaxed);
	}
	for(size_t i = 0; i < todo.size(); i++){
		StreamCommand &c = todo[i];
		size_t j = i + 1;
		while(c.op != CMD_CLOSE && j < todo.size() && todo[j].stream != c.stream)
			j++;
		if(c.op == CMD_CLOSE || j == todo.size()){
			apply(c);
			continue;
		}
		if(c.op == CMD_OPEN && todo[j].op == CMD_SEEK){
			todo[j].op = CMD_OPEN;
			memcpy(todo[j].path, c.path, sizeof(c.path));
		}
	}
	bool starved = false;
	for(int s = 0; s < kStreams; s++)
		starved = fill(s) || starved;
	mStarved = starved;
}

void
StreamWorker::run(void)
{
#ifdef __APPLE__
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
	for(;;){
		{
			std::unique_lock<std::mutex> lock(mMutex);
			if(active())
				mCv.wait_for(lock, std::chrono::milliseconds(mStarved ? kWorkerRetryMs : kWorkerWakeMs), [this] { return mQuit || !mQueue.empty(); });
			else
				mCv.wait(lock, [this] { return mQuit || !mQueue.empty(); });
			if(mQuit)
				return;
		}
		mWakes.fetch_add(1, std::memory_order_relaxed);
		step();
	}
}

}
