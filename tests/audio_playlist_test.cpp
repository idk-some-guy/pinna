// mkdir -p build/tests && clang++ -std=gnu++14 -Wall -Wextra -I src tests/audio_playlist_test.cpp -o build/tests/audio_playlist_test && build/tests/audio_playlist_test
#include <cstdio>
#include "CaPlaylist.h"

using namespace pinna;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

int
main(void)
{
	PlaylistEntry e[3] = { { 1000, 0 }, { 2500, 0 }, { 400, 0 } };
	CHECK(PlaylistLayout(e, 3) == 3900);
	CHECK(e[0].streamPos == 0 && e[1].streamPos == 1000 && e[2].streamPos == 3500);
	uint32_t pos = 1200, index = 9;
	CHECK(PlaylistFind(e, 3, &pos, &index) && index == 1 && pos == 200);
	pos = 999;
	CHECK(PlaylistFind(e, 3, &pos, &index) && index == 0 && pos == 999);
	pos = 3500;
	CHECK(PlaylistFind(e, 3, &pos, &index) && index == 2 && pos == 0);
	pos = 3899;
	CHECK(PlaylistFind(e, 3, &pos, &index) && index == 2 && pos == 399);
	pos = 3900;
	index = 9;
	CHECK(!PlaylistFind(e, 3, &pos, &index) && index == 0 && pos == 0);
	pos = 5;
	index = 9;
	CHECK(!PlaylistFind(e, 0, &pos, &index) && index == 0 && pos == 0);
	CHECK(PlaylistLayout(e, 0) == 0);
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all playlist tests passed\n");
	return 0;
}
