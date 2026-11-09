// mkdir -p build/tests && clang++ -std=gnu++14 -O2 -g -fsanitize=thread -Wall -Wextra -pthread -I src tests/audio_handoff_test.cpp -o build/tests/audio_handoff_test && build/tests/audio_handoff_test
#include <cstdio>
#include <thread>
#include <atomic>
#include <type_traits>
#include "CaHandoff.h"

using namespace pinna;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

struct Payload
{
	uint32_t serial;
	uint32_t fill[63];
};

static_assert(std::is_trivially_destructible<TripleBuffer<Payload>>::value, "the handoff owns no heap memory");

static void
Publish(TripleBuffer<Payload> &tb, uint32_t serial)
{
	Payload &p = tb.writeBuffer();
	p.serial = serial;
	for(int i = 0; i < 63; i++)
		p.fill[i] = serial;
	tb.publish();
}

static void
TestSequence(void)
{
	TripleBuffer<Payload> tb;
	CHECK(tb.read().serial == 0);
	Publish(tb, 1);
	CHECK(tb.read().serial == 1);
	CHECK(tb.read().serial == 1);
	Publish(tb, 2);
	Publish(tb, 3);
	CHECK(tb.read().serial == 3);
	Publish(tb, 4);
	CHECK(tb.read().serial == 4);
	CHECK(tb.read().serial == 4);
}

static void
TestReaderStall(void)
{
	TripleBuffer<Payload> tb;
	Publish(tb, 7);
	const Payload &held = tb.read();
	for(uint32_t s = 8; s <= 1000; s++)
		Publish(tb, s);
	bool intact = held.serial == 7;
	for(int i = 0; i < 63; i++)
		intact = intact && held.fill[i] == 7;
	CHECK(intact);
	CHECK(tb.read().serial == 1000);
}

static void
TestThreads(void)
{
	static TripleBuffer<Payload> tb;
	std::atomic<bool> done(false);
	std::atomic<int> torn(0), backwards(0);
	std::thread reader([&] {
		uint32_t last = 0;
		while(!done.load()){
			const Payload &p = tb.read();
			for(int i = 0; i < 63; i++)
				if(p.fill[i] != p.serial){
					torn++;
					break;
				}
			if(p.serial < last)
				backwards++;
			last = p.serial;
		}
	});
	for(uint32_t s = 1; s <= 2000000; s++)
		Publish(tb, s);
	done = true;
	reader.join();
	CHECK(torn.load() == 0);
	CHECK(backwards.load() == 0);
	CHECK(tb.read().serial == 2000000);
}

static void
TestPool(void)
{
	int8_t mirror[4] = { 0, -1, 2, -1 };
	std::atomic<int32_t> reader[4];
	reader[0] = 1;
	reader[1] = -1;
	reader[2] = 2;
	reader[3] = 3;
	uint32_t ref[6] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
	int cursor = 0;
	CHECK(PickPoolSlot(mirror, reader, ref, 0, 4, 6, &cursor) == 4);
	CHECK(cursor == 5);
	CHECK(PickPoolSlot(mirror, reader, ref, 0, 4, 6, &cursor) == 5);
	CHECK(PickPoolSlot(mirror, reader, ref, 0, 4, 6, &cursor) == 4);
	int8_t full[4] = { 0, 1, 2, 3 };
	CHECK(PickPoolSlot(full, reader, ref, 0, 4, 4, &cursor) == -1);
	ref[4] = 9;
	cursor = 4;
	CHECK(PickPoolSlot(mirror, reader, ref, 9, 4, 6, &cursor) == 5);
	CHECK(PickPoolSlot(mirror, reader, ref, 10, 4, 6, &cursor) == 4);
	ref[4] = 0;
	ref[5] = UINT32_MAX;
	CHECK(PickPoolSlot(mirror, reader, ref, UINT32_MAX, 4, 6, &cursor) == -1);
	CHECK(PickPoolSlot(mirror, reader, ref, 0, 4, 6, &cursor) == 5);
}

int
main(void)
{
	TestSequence();
	TestReaderStall();
	TestThreads();
	TestPool();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all handoff tests passed\n");
	return 0;
}
