// mkdir -p build/tests && clang++ -std=gnu++14 -O2 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -pthread -I src tests/audio_start_test.cpp src/CaResampler.cpp src/CaVoice.cpp src/CaMixer.cpp src/CaStream.cpp -o build/tests/audio_start_test && build/tests/audio_start_test && clang++ -std=gnu++14 -O2 -g -fsanitize=thread -Wall -Wextra -pthread -I src tests/audio_start_test.cpp src/CaResampler.cpp src/CaVoice.cpp src/CaMixer.cpp src/CaStream.cpp -o build/tests/audio_start_test_tsan && build/tests/audio_start_test_tsan
#include <cstdio>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include "CaStart.h"

using namespace pinna;

#ifdef PINNA_CONFIG_HEADER
static_assert(kChannels == PINNA_CHANNELS && kStreams == PINNA_STREAMS && kPoolSlots == PINNA_POOL_SLOTS, "the configuration header sets the counts");
#endif

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static const uint32_t kSlot = 1024;

struct Starter
{
	Shared shared;
	Snapshot mirror;
	std::vector<int16_t> pool;
	uint32_t slotRef[kPoolSlots];
	int cursor;
	uint32_t counter;
	Starter(void) : pool((size_t)kPoolSlots * kSlot, 0), cursor(0), counter(0)
	{
		memset(&mirror, 0, sizeof(mirror));
		for(int c = 0; c < kChannels; c++)
			mirror.voice[c].poolSlot = -1;
		ResetPoolRefs(slotRef);
	}
	void arm(int c, const int16_t *data, uint32_t length, int8_t slot)
	{
		LoopRange whole = { 0, length };
		VoiceSnap &v = mirror.voice[c];
		ArmVoice(v, data, length, whole, false, slot, ++counter);
		v.freq = 22050.0f;
		v.gainL = v.gainR = 1.0f;
	}
	bool start(int c, const std::vector<int16_t> &src)
	{
		PoolStart ps;
		if(!StartFromPool(mirror, shared, slotRef, pool.data(), kSlot, &cursor, src.data(), (uint32_t)src.size(), &ps))
			return false;
		arm(c, ps.data, ps.length, ps.slot);
		return true;
	}
	void publish(void)
	{
		PublishMirror(mirror, shared, slotRef);
	}
};

static std::vector<int16_t>
Fill(int16_t value, uint32_t n)
{
	return std::vector<int16_t>(n, value);
}

static void
Render(Mixer &m, uint32_t frames)
{
	static float a[4096], b[4096], c[4096], d[4096], e[4096];
	m.render(a, b, c, d, e, frames, 48000.0f);
}

static void
TestStoppedChannelKeepsSlot(void)
{
	static Starter g;
	CHECK(g.start(6, Fill(111, 40)));
	g.publish();
	int8_t s = g.mirror.voice[6].poolSlot;
	g.mirror.voice[6].playing = 0;
	g.cursor = s;
	CHECK(g.start(7, Fill(222, 40)));
	CHECK(g.mirror.voice[7].poolSlot != s);
	CHECK(g.mirror.voice[6].poolSlot == s);
	CHECK(g.pool[(size_t)s * kSlot] == 111);
}

static void
TestRestartReleasesOldSlot(void)
{
	static Starter g;
	CHECK(g.start(6, Fill(111, 40)));
	int8_t s = g.mirror.voice[6].poolSlot;
	g.cursor = s;
	CHECK(g.start(6, Fill(112, 40)));
	CHECK(g.mirror.voice[6].poolSlot != s);
	g.cursor = s;
	CHECK(g.start(4, Fill(113, 40)));
	CHECK(g.mirror.voice[4].poolSlot == s);
}

static void
TestReaderSlotHeld(void)
{
	static Starter g;
	Mixer m(&g.shared);
	CHECK(g.start(6, Fill(111, 1000)));
	int8_t s = g.mirror.voice[6].poolSlot;
	g.publish();
	Render(m, 256);
	CHECK(g.shared.readerSlot[6].load() == s);
	g.cursor = s;
	CHECK(g.start(6, Fill(112, 1000)));
	g.cursor = s;
	CHECK(g.start(5, Fill(113, 40)));
	CHECK(g.mirror.voice[5].poolSlot != s);
}

static void
TestRestartLeavesNoReaderOnOldSlot(void)
{
	static Starter g;
	Mixer m(&g.shared);
	CHECK(g.start(6, Fill(111, 4000)));
	int8_t s = g.mirror.voice[6].poolSlot;
	g.publish();
	Render(m, 256);
	CHECK(g.start(6, Fill(112, 4000)));
	g.publish();
	Render(m, 16);
	CHECK(g.shared.readerSlot[6].load() == g.mirror.voice[6].poolSlot);
	g.publish();
	g.cursor = s;
	CHECK(g.start(5, Fill(113, 40)));
	CHECK(g.mirror.voice[5].poolSlot == s);
}

static void
TestLengthClampedToSlot(void)
{
	static Starter g;
	for(size_t i = 0; i < g.pool.size(); i++)
		g.pool[i] = -7;
	g.cursor = kPoolSlots - 1;
	CHECK(g.start(6, Fill(300, kSlot + 50)));
	CHECK(g.mirror.voice[6].poolSlot == kPoolSlots - 1);
	CHECK(g.mirror.voice[6].length == kSlot);
	CHECK(g.mirror.voice[6].data + g.mirror.voice[6].length == g.pool.data() + g.pool.size());
	CHECK(g.start(7, Fill(301, kSlot + 50)));
	CHECK(g.mirror.voice[7].poolSlot == 0 && g.mirror.voice[7].length == kSlot);
	CHECK(g.pool[kSlot] == -7);
	CHECK(g.start(4, Fill(302, 10)));
	CHECK(g.mirror.voice[4].length == 10 && g.pool[(size_t)g.mirror.voice[4].poolSlot * kSlot + 10] == -7);
}

static void
TestSourceLengthClampedToBlock(void)
{
	const uint32_t blockBytes = 4096, offset = 3000;
	int16_t *block = new int16_t[blockBytes / 2];
	for(uint32_t i = 0; i < blockBytes / 2; i++)
		block[i] = (int16_t)(1000 + i);
	const int16_t *data = block + offset / 2;
	uint32_t length = (blockBytes - offset) / 2;
	CHECK(data + length == block + blockBytes / 2);
	LoopRange past = OalLoopRange(200, 2000, length);
	CHECK(past.start == 0 && past.end == length);
	LoopRange inside = OalLoopRange(200, 1000, length);
	CHECK(inside.start == 100 && inside.end == 500);

	static Starter g;
	Mixer m(&g.shared);
	VoiceSnap &v = g.mirror.voice[5];
	ArmVoice(v, data, length, past, false, -1, ++g.counter);
	v.freq = 22050.0f;
	v.gainL = v.gainR = 1.0f;
	g.publish();
	for(int k = 0; k < 8; k++)
		Render(m, 512);
	CHECK(g.shared.finishedId[5].load() == v.playId);
	CHECK(!ChannelUsed(g.mirror, g.shared, 5));
	delete[] block;
}

static void
TestRecordedBytesClampedToSlot(void)
{
	const uint32_t slotBytes = 600, sourceBytes = 8000, offset = 1000;
	std::vector<int16_t> source(sourceBytes / 2);
	for(uint32_t i = 0; i < source.size(); i++)
		source[i] = (int16_t)(500 + i);
	int16_t *slot = new int16_t[slotBytes / 2];
	uint32_t recorded = slotBytes;
	memcpy(slot, (const char *)source.data() + offset, recorded);
	CHECK(slot[slotBytes / 2 - 1] == source[(offset + slotBytes) / 2 - 1]);

	static Starter g;
	Mixer m(&g.shared);
	for(size_t i = 0; i < g.pool.size(); i++)
		g.pool[i] = -7;
	PoolStart ps;
	CHECK(StartFromPool(g.mirror, g.shared, g.slotRef, g.pool.data(), kSlot, &g.cursor, slot, recorded / 2, &ps));
	CHECK(ps.length == slotBytes / 2);
	CHECK(memcmp(ps.data, slot, slotBytes) == 0);
	CHECK(ps.data[slotBytes / 2] == -7);
	g.arm(6, ps.data, ps.length, ps.slot);
	g.publish();
	CHECK(g.mirror.voice[6].length == slotBytes / 2);
	Render(m, 2048);
	CHECK(g.shared.finishedId[6].load() == g.mirror.voice[6].playId);
	delete[] slot;
}

static void
TestPoolOutlastsEveryHolder(void)
{
	static Starter g;
	for(int c = 0; c < kChannels; c++){
		g.mirror.voice[c].poolSlot = (int8_t)c;
		g.shared.readerSlot[c].store(kChannels + c);
	}
	g.cursor = 0;
	CHECK(g.start(3, Fill(1, 10)));
	CHECK(g.mirror.voice[3].poolSlot == 2 * kChannels);
}

static void
TestUsedFlag(void)
{
	static Starter g;
	Mixer m(&g.shared);
	CHECK(!ChannelUsed(g.mirror, g.shared, 6));
	CHECK(g.start(6, Fill(1000, 300)));
	g.publish();
	CHECK(ChannelUsed(g.mirror, g.shared, 6));
	Render(m, 256);
	CHECK(ChannelUsed(g.mirror, g.shared, 6));
	Render(m, 512);
	CHECK(!ChannelUsed(g.mirror, g.shared, 6));
	CHECK(g.start(6, Fill(1000, 300)));
	CHECK(ChannelUsed(g.mirror, g.shared, 6));
	g.mirror.voice[6].playing = 0;
	CHECK(!ChannelUsed(g.mirror, g.shared, 6));
}

static void
TestEmptyStartIsNotUsed(void)
{
	static Starter g;
	Mixer m(&g.shared);
	std::vector<int16_t> none;
	CHECK(g.start(kChannels - 1, none));
	CHECK(g.mirror.voice[kChannels - 1].length == 0);
	CHECK(!ChannelUsed(g.mirror, g.shared, kChannels - 1));
	g.publish();
	Render(m, 64);
	CHECK(!ChannelUsed(g.mirror, g.shared, kChannels - 1));
}

static const int16_t kSamples[400] = { 0 };

static void
TestUnreadSnapshotHoldsSlot(void)
{
	static Starter g;
	Mixer m(&g.shared);
	CHECK(g.start(6, Fill(111, 1000)));
	int8_t s = g.mirror.voice[6].poolSlot;
	g.publish();
	g.arm(6, kSamples, 400, -1);
	g.publish();
	g.cursor = s;
	CHECK(g.start(7, Fill(222, 1000)));
	CHECK(g.mirror.voice[7].poolSlot != s);
	CHECK(g.pool[(size_t)s * kSlot] == 111);
	g.publish();
	Render(m, 64);
	g.cursor = s;
	CHECK(g.start(4, Fill(333, 40)));
	CHECK(g.mirror.voice[4].poolSlot == s);
}

static void
TestFreshEngineStartsSpeech(void)
{
	static Starter g;
	CHECK(g.start(6, Fill(111, 40)));
	CHECK(g.mirror.voice[6].poolSlot == 0);
	g.publish();
	CHECK(g.start(7, Fill(222, 40)));
	CHECK(g.mirror.voice[7].poolSlot == 1);
}

static void
TestSerialWrap(void)
{
	static Starter g;
	Mixer m(&g.shared);
	for(uint32_t k = 1; k <= 3; k++){
		g.mirror.serial = k * 0x40000000u - 1;
		g.publish();
		Render(m, 64);
	}
	g.mirror.serial = UINT32_MAX - 1;
	g.publish();
	Render(m, 64);
	CHECK(g.shared.latchedSerial.load() == UINT32_MAX);
	CHECK(g.start(6, Fill(111, 1000)));
	int8_t s = g.mirror.voice[6].poolSlot;
	g.publish();
	CHECK(g.mirror.serial == 0);
	g.arm(6, kSamples, 400, -1);
	g.cursor = s;
	CHECK(g.start(7, Fill(222, 40)));
	CHECK(g.mirror.voice[7].poolSlot != s);
	g.publish();
	Render(m, 64);
	g.cursor = s;
	CHECK(g.start(4, Fill(333, 40)));
	CHECK(g.mirror.voice[4].poolSlot == s);
}

static void
TestIdleSlotStaysFree(void)
{
	static Starter g;
	for(int k = 1; k <= 6; k++){
		g.mirror.serial = (uint32_t)k * 0x40000000u - 1;
		g.publish();
		g.shared.latchedSerial.store(g.mirror.serial);
	}
	g.cursor = 5;
	CHECK(g.start(6, Fill(111, 40)));
	CHECK(g.mirror.voice[6].poolSlot == 5);
}

static void
TestPoolStressThreads(void)
{
	static Starter g;
	static Mixer m(&g.shared);
	std::atomic<bool> done(false);
	std::atomic<uint32_t> latches(0), wrong(0);
	std::thread audio([&] {
		while(!done.load(std::memory_order_acquire)){
			const Snapshot &s = g.shared.snapshot.read();
			for(int c = 0; c < kChannels; c++){
				const VoiceSnap &v = s.voice[c];
				if(v.poolSlot >= 0 && v.length && (v.data[0] != (int16_t)(v.playId & 0x7fff) || v.data[v.length - 1] != (int16_t)(v.playId & 0x7fff)))
					wrong++;
			}
			latches++;
			Render(m, 256);
			std::this_thread::sleep_for(std::chrono::microseconds(700));
		}
	});
	static const uint32_t lengths[4] = { 900, 700, 1000, 500 };
	uint32_t starts = 0;
	for(int frame = 0; frame < 600; frame++){
		g.publish();
		for(int k = 0; k < 100; k++)
			if(g.start(3 + k % 3, Fill((int16_t)((g.counter + 1) & 0x7fff), lengths[(frame + k) & 3])))
				starts++;
		std::this_thread::sleep_for(std::chrono::microseconds(1500));
	}
	done.store(true, std::memory_order_release);
	audio.join();
	CHECK(starts > 600u * 80);
	CHECK(latches.load() > 300);
	CHECK(wrong.load() == 0);
}

int
main(void)
{
	InitResampler();
	TestStoppedChannelKeepsSlot();
	TestRestartReleasesOldSlot();
	TestReaderSlotHeld();
	TestRestartLeavesNoReaderOnOldSlot();
	TestLengthClampedToSlot();
	TestSourceLengthClampedToBlock();
	TestRecordedBytesClampedToSlot();
	TestPoolOutlastsEveryHolder();
	TestUsedFlag();
	TestEmptyStartIsNotUsed();
	TestUnreadSnapshotHoldsSlot();
	TestFreshEngineStartsSpeech();
	TestSerialWrap();
	TestIdleSlotStaysFree();
	TestPoolStressThreads();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all start tests passed\n");
	return 0;
}
