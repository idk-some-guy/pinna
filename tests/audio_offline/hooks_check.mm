// mkdir -p build/tests && clang++ -std=gnu++14 -O2 -g -fsanitize=address -fobjc-arc -Wall -Wextra -mmacosx-version-min=14.0 -DPINNA_COREAUDIO -I src -I src/coreaudio tests/audio_offline/hooks_check.mm src/coreaudio/CaDecoders.mm -framework AudioToolbox -framework CoreAudio -framework CoreFoundation -o build/tests/hooks_check && build/tests/hooks_check
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>
#include "CaShell.h"

static int failures = 0;

static void
Report(bool ok, const char *name)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", name);
	if(!ok)
		failures++;
}

static const uint32_t kRate = 44100, kFrames = 22050;

static int16_t
Sample(uint32_t i, int c)
{
	double t = (double)i / kRate;
	double v = c == 0 ? sin(2.0 * M_PI * 440.0 * t) : 0.6 * sin(2.0 * M_PI * 660.0 * t);
	return (int16_t)lrint(0.3 * v * 32767.0);
}

static bool
WriteWav(const char *path)
{
	std::vector<uint8_t> f;
	auto put = [&f](uint32_t v, int bytes) { for(int i = 0; i < bytes; i++) f.push_back((uint8_t)(v >> (8 * i))); };
	uint32_t data = kFrames * 4;
	f.insert(f.end(), { 'R', 'I', 'F', 'F' });
	put(4 + 8 + 16 + 8 + data, 4);
	f.insert(f.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
	put(16, 4); put(1, 2); put(2, 2); put(kRate, 4); put(kRate * 4, 4); put(4, 2); put(16, 2);
	f.insert(f.end(), { 'd', 'a', 't', 'a' });
	put(data, 4);
	for(uint32_t i = 0; i < kFrames; i++)
		for(int c = 0; c < 2; c++)
			put((uint16_t)Sample(i, c), 2);
	FILE *out = fopen(path, "wb");
	bool ok = out && fwrite(f.data(), 1, f.size(), out) == f.size();
	if(out)
		ok = fclose(out) == 0 && ok;
	return ok;
}

static void
Scramble(void *bytes, size_t n, uint64_t offset, void *)
{
	uint8_t *b = (uint8_t *)bytes;
	for(size_t i = 0; i < n; i++)
		b[i] ^= (uint8_t)((offset + i) * 7 + 3);
}

static pinna::DecodeFormat
DescribeScrambled(const char *path, void *)
{
	pinna::DecodeFormat f = { false, false, NULL };
	if(pinna::HasExtension(path, ".scr")){
		f.accepted = f.transformed = true;
		f.decodeAs = "wav";
	}
	else if(pinna::HasExtension(path, ".unk")){
		f.accepted = f.transformed = true;
		f.decodeAs = "ogg";
	}
	else if(pinna::HasExtension(path, ".scm")){
		f.accepted = f.transformed = true;
		f.decodeAs = "mp3";
	}
	return f;
}

static const char *gDescribedAs;

static pinna::DecodeFormat
DescribePlainSnd(const char *path, void *)
{
	pinna::DecodeFormat f = { pinna::HasExtension(path, ".snd"), false, gDescribedAs };
	return f;
}

static void
Install(const pinna::DecodeHooks *hooks)
{
	if(!pinna::SetDecodeHooks(hooks)){
		printf("FAIL the check could not install its decode hooks\n");
		failures++;
	}
}

static std::string
CaptureStdout(const std::function<void(void)> &run)
{
	fflush(stdout);
	int saved = dup(1);
	FILE *tmp = tmpfile();
	dup2(fileno(tmp), 1);
	run();
	fflush(stdout);
	dup2(saved, 1);
	close(saved);
	std::string text;
	rewind(tmp);
	char buf[512];
	size_t n;
	while((n = fread(buf, 1, sizeof(buf), tmp)) > 0)
		text.append(buf, n);
	fclose(tmp);
	return text;
}

static std::vector<float>
ReadFrames(pinna::Decoder *d, uint32_t *got)
{
	const uint32_t frames = kFrames + 4096;
	std::vector<float> out(frames * 2, 0.0f);
	uint32_t done = 0;
	while(done < frames){
		uint32_t want = std::min<uint32_t>(frames - done, 4000);
		uint32_t n = d->read(out.data() + done * 2, want);
		done += n;
		if(n < want)
			break;
	}
	*got = done;
	out.resize(done * 2);
	return out;
}

static std::vector<uint8_t>
ReadFile(const char *path)
{
	std::vector<uint8_t> bytes;
	if(FILE *f = fopen(path, "rb")){
		uint8_t buf[4096];
		size_t n;
		while((n = fread(buf, 1, sizeof(buf), f)) > 0)
			bytes.insert(bytes.end(), buf, buf + n);
		fclose(f);
	}
	return bytes;
}

static bool
WriteScrambledCopy(const char *from, const std::string &to)
{
	std::vector<uint8_t> bytes = ReadFile(from);
	Scramble(bytes.data(), bytes.size(), 0, NULL);
	FILE *f = fopen(to.c_str(), "wb");
	bool ok = f && !bytes.empty() && fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
	if(f)
		fclose(f);
	return ok;
}

static std::string gDir;

static void
CheckHooksFrozenWhileOpen(void)
{
	std::string plain = gDir + "/frozen.wav", scrambled = gDir + "/frozen.scr";
	bool wrote = WriteWav(plain.c_str()) && WriteScrambledCopy(plain.c_str(), scrambled);
	pinna::DecodeHooks test = { DescribeScrambled, Scramble, NULL }, other = { DescribePlainSnd, NULL, NULL };
	pinna::Decoder *ref = wrote ? pinna::OpenDecoder(plain.c_str()) : NULL;
	uint32_t refGot = 0, decGot = 0;
	std::vector<float> a;
	if(ref)
		a = ReadFrames(ref, &refGot);
	delete ref;
	Install(&test);
	pinna::Decoder *dec = wrote ? pinna::OpenDecoder(scrambled.c_str()) : NULL;
	bool refused = false;
	std::string log = CaptureStdout([&]() { refused = !pinna::SetDecodeHooks(&other); });
	std::vector<float> b;
	if(dec)
		b = ReadFrames(dec, &decGot);
	delete dec;
	bool installed = pinna::SetDecodeHooks(&other);
	bool kept = refGot == kFrames && decGot == refGot && a == b;
	Report(kept && refused && installed && log.find("decode hooks cannot change while a decoder is open") != std::string::npos,
	       "decode hooks installed while a decoder is open are refused with a logged line, the open file keeps its transform, and the install works once it is closed");
	Install(NULL);
	remove(plain.c_str());
	remove(scrambled.c_str());
}

static void
CheckDecodeAsPlain(void)
{
	std::string wav = gDir + "/plain.wav", snd = gDir + "/plain.snd";
	bool wrote = WriteWav(wav.c_str()) && WriteWav(snd.c_str());
	pinna::Decoder *ref = wrote ? pinna::OpenDecoder(wav.c_str()) : NULL;
	uint32_t refGot = 0, gotWav = 0;
	std::vector<float> a;
	if(ref)
		a = ReadFrames(ref, &refGot);
	delete ref;
	pinna::DecodeHooks plain = { DescribePlainSnd, NULL, NULL };
	Install(&plain);
	gDescribedAs = "wav";
	pinna::Decoder *asWav = wrote ? pinna::OpenDecoder(snd.c_str()) : NULL;
	std::vector<float> b;
	if(asWav)
		b = ReadFrames(asWav, &gotWav);
	delete asWav;
	gDescribedAs = "mp3";
	pinna::Decoder *asMp3 = wrote ? pinna::OpenDecoder(snd.c_str()) : NULL;
	uint32_t mp3Length = wrote ? pinna::ProbeLengthMs(snd.c_str()) : 1;
	delete asMp3;
	gDescribedAs = "ogg";
	pinna::Decoder *asOgg = NULL;
	std::string log = CaptureStdout([&]() { asOgg = wrote ? pinna::OpenDecoder(snd.c_str()) : NULL; });
	uint32_t oggLength = wrote ? pinna::ProbeLengthMs(snd.c_str()) : 1;
	delete asOgg;
	gDescribedAs = NULL;
	pinna::Decoder *own = wrote ? pinna::OpenDecoder(snd.c_str()) : NULL;
	bool ownOk = own != NULL && own->lengthFrames() == kFrames;
	delete own;
	Install(NULL);
	bool same = refGot == kFrames && gotWav == refGot && a == b;
	Report(same && asMp3 == NULL && mp3Length == 0 && asOgg == NULL && oggLength == 0 && ownOk &&
	       log.find("plain file") != std::string::npos && log.find("does not know") != std::string::npos,
	       "decodeAs means the same for a plain file as for a transformed one: a plain wav described as wav decodes, as mp3 or as an unknown type it is refused, the unknown type is logged, and with none the file's own type decides");
	remove(wav.c_str());
	remove(snd.c_str());
}

static void
CheckDecodeAsTransformed(void)
{
	std::string plain = gDir + "/as.wav", asWav = gDir + "/as.scr", asMp3 = gDir + "/as.scm";
	bool wrote = WriteWav(plain.c_str()) && WriteScrambledCopy(plain.c_str(), asWav) && WriteScrambledCopy(plain.c_str(), asMp3);
	pinna::DecodeHooks test = { DescribeScrambled, Scramble, NULL };
	Install(&test);
	pinna::Decoder *right = wrote ? pinna::OpenDecoder(asWav.c_str()) : NULL;
	pinna::Decoder *wrong = wrote ? pinna::OpenDecoder(asMp3.c_str()) : NULL;
	uint32_t wrongLength = wrote ? pinna::ProbeLengthMs(asMp3.c_str()) : 1;
	bool rightOk = right != NULL && right->lengthFrames() == kFrames;
	delete right;
	delete wrong;
	Install(NULL);
	Report(rightOk && wrong == NULL && wrongLength == 0, "a transformed wav described as wav decodes and described as mp3 is refused, as a plain file is");
	remove(plain.c_str());
	remove(asWav.c_str());
	remove(asMp3.c_str());
}

static void
CheckPositionScramble(void)
{
	std::string plain = gDir + "/scramble.wav", scrambled = gDir + "/scramble.scr", unknown = gDir + "/scramble.unk";
	bool wrote = WriteWav(plain.c_str()) && WriteScrambledCopy(plain.c_str(), scrambled) && WriteScrambledCopy(plain.c_str(), unknown);
	pinna::Decoder *ref = wrote ? pinna::OpenDecoder(plain.c_str()) : NULL;
	pinna::DecodeHooks test = { DescribeScrambled, Scramble, NULL }, noTransform = { DescribeScrambled, NULL, NULL };
	uint32_t refGot = 0, decGot = 0;
	std::vector<float> a;
	if(ref)
		a = ReadFrames(ref, &refGot);
	delete ref;
	Install(&test);
	pinna::Decoder *dec = wrote ? pinna::OpenDecoder(scrambled.c_str()) : NULL;
	std::vector<float> b;
	if(dec)
		b = ReadFrames(dec, &decGot);
	bool decoded = dec != NULL;
	delete dec;
	pinna::Decoder *bad = wrote ? pinna::OpenDecoder(unknown.c_str()) : NULL;
	Install(&noTransform);
	pinna::Decoder *none = wrote ? pinna::OpenDecoder(scrambled.c_str()) : NULL;
	pinna::DecodeHooks noDescribe = { NULL, Scramble, NULL };
	bool refused = false;
	std::string log = CaptureStdout([&]() { refused = !pinna::SetDecodeHooks(&noDescribe); });
	pinna::Decoder *bare = wrote ? pinna::OpenDecoder(plain.c_str()) : NULL;
	pinna::Decoder *bareScr = wrote ? pinna::OpenDecoder(scrambled.c_str()) : NULL;
	bool kept = bare == NULL && bareScr == NULL;
	delete bare;
	delete bareScr;
	Install(NULL);
	bool same = decoded && refGot == kFrames && decGot == refGot && a == b;
	Report(same && bad == NULL && none == NULL && refused && kept && log.find("no describe") != std::string::npos,
	       "a file the hooks describe as transformed and decoded as wav, scrambled by position, decodes to the original's samples; an unknown decodeAs and a missing transform are refused cleanly, and hooks with no describe are refused with a logged line while the installed hooks stay");
	delete bad;
	delete none;
	remove(plain.c_str());
	remove(scrambled.c_str());
	remove(unknown.c_str());
}

int
main(void)
{
	char dir[] = "build/tests/hooks_check.XXXXXX";
	if(mkdtemp(dir) == NULL){
		printf("FAIL no scratch directory\n");
		return 1;
	}
	gDir = dir;
	CheckHooksFrozenWhileOpen();
	CheckDecodeAsPlain();
	CheckDecodeAsTransformed();
	CheckPositionScramble();
	rmdir(dir);
	printf("%s\n", failures ? "FAILED" : "all checks passed");
	return failures ? 1 : 0;
}
