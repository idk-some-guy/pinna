// mkdir -p build/tests && clang++ -std=gnu++14 -O2 -g -fobjc-arc -Wall -Wextra -mmacosx-version-min=14.0 tests/audio_offline/device_types.mm -framework CoreAudio -framework CoreFoundation -o build/tests/device_types && build/tests/device_types
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

static const AudioObjectPropertyScope kScope = kAudioObjectPropertyScopeOutput;

static std::string
FourCC(UInt32 v)
{
	char c[4] = { (char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v };
	bool printable = true;
	for(int i = 0; i < 4; i++)
		if(!isprint((unsigned char)c[i]))
			printable = false;
	char buf[32];
	if(printable)
		snprintf(buf, sizeof(buf), "'%c%c%c%c'", c[0], c[1], c[2], c[3]);
	else
		snprintf(buf, sizeof(buf), "0x%08x", (unsigned)v);
	return buf;
}

static std::string
Status(OSStatus s)
{
	return "error " + FourCC((UInt32)s) + " (" + std::to_string((int)s) + ")";
}

static bool
Has(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope)
{
	AudioObjectPropertyAddress a = { sel, scope, kAudioObjectPropertyElementMain };
	return AudioObjectHasProperty(obj, &a);
}

static OSStatus
GetBytes(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope, std::vector<UInt8> &out)
{
	AudioObjectPropertyAddress a = { sel, scope, kAudioObjectPropertyElementMain };
	UInt32 size = 0;
	OSStatus s = AudioObjectGetPropertyDataSize(obj, &a, 0, nullptr, &size);
	if(s != noErr)
		return s;
	out.assign(size, 0);
	if(size == 0)
		return noErr;
	s = AudioObjectGetPropertyData(obj, &a, 0, nullptr, &size, out.data());
	if(s == noErr)
		out.resize(size);
	return s;
}

static std::string
UInt32Prop(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope, UInt32 *value = nullptr)
{
	if(!Has(obj, sel, scope))
		return "absent";
	AudioObjectPropertyAddress a = { sel, scope, kAudioObjectPropertyElementMain };
	UInt32 v = 0, size = sizeof(v);
	OSStatus s = AudioObjectGetPropertyData(obj, &a, 0, nullptr, &size, &v);
	if(s != noErr)
		return Status(s);
	if(value)
		*value = v;
	return FourCC(v);
}

static std::string
CFToString(CFStringRef str)
{
	if(!str)
		return "(null)";
	char buf[512];
	if(!CFStringGetCString(str, buf, sizeof(buf), kCFStringEncodingUTF8))
		return "(unconvertible)";
	return buf;
}

static std::string
StringProp(AudioObjectID obj, AudioObjectPropertySelector sel)
{
	if(!Has(obj, sel, kAudioObjectPropertyScopeGlobal))
		return "absent";
	AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
	CFStringRef str = nullptr;
	UInt32 size = sizeof(str);
	OSStatus s = AudioObjectGetPropertyData(obj, &a, 0, nullptr, &size, &str);
	if(s != noErr)
		return Status(s);
	std::string r = CFToString(str);
	if(str)
		CFRelease(str);
	return r;
}

static std::string
DataSourceName(AudioObjectID dev, UInt32 source)
{
	if(!Has(dev, kAudioDevicePropertyDataSourceNameForIDCFString, kScope))
		return "absent";
	AudioObjectPropertyAddress a = { kAudioDevicePropertyDataSourceNameForIDCFString, kScope, kAudioObjectPropertyElementMain };
	CFStringRef str = nullptr;
	AudioValueTranslation t = { &source, sizeof(source), &str, sizeof(str) };
	UInt32 size = sizeof(t);
	OSStatus s = AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &t);
	if(s != noErr)
		return Status(s);
	std::string r = "\"" + CFToString(str) + "\"";
	if(str)
		CFRelease(str);
	return r;
}

static std::vector<AudioStreamID>
Streams(AudioObjectID dev, std::string *error)
{
	std::vector<AudioStreamID> ids;
	std::vector<UInt8> bytes;
	OSStatus s = GetBytes(dev, kAudioDevicePropertyStreams, kScope, bytes);
	if(s != noErr){
		*error = Status(s);
		return ids;
	}
	size_t n = bytes.size() / sizeof(AudioStreamID);
	ids.resize(n);
	for(size_t i = 0; i < n; i++)
		memcpy(&ids[i], bytes.data() + i * sizeof(AudioStreamID), sizeof(AudioStreamID));
	return ids;
}

static std::string
ChannelCount(AudioObjectID dev)
{
	if(!Has(dev, kAudioDevicePropertyStreamConfiguration, kScope))
		return "absent";
	std::vector<UInt8> bytes;
	OSStatus s = GetBytes(dev, kAudioDevicePropertyStreamConfiguration, kScope, bytes);
	if(s != noErr)
		return Status(s);
	if(bytes.size() < offsetof(AudioBufferList, mBuffers))
		return "0";
	const AudioBufferList *list = (const AudioBufferList *)bytes.data();
	size_t room = (bytes.size() - offsetof(AudioBufferList, mBuffers)) / sizeof(AudioBuffer);
	UInt32 n = list->mNumberBuffers < room ? list->mNumberBuffers : (UInt32)room;
	UInt32 channels = 0;
	for(UInt32 i = 0; i < n; i++)
		channels += list->mBuffers[i].mNumberChannels;
	return std::to_string(channels) + " in " + std::to_string(n) + " buffer(s)";
}

static std::string
Layout(AudioObjectID dev)
{
	if(!Has(dev, kAudioDevicePropertyPreferredChannelLayout, kScope))
		return "absent";
	std::vector<UInt8> bytes;
	OSStatus s = GetBytes(dev, kAudioDevicePropertyPreferredChannelLayout, kScope, bytes);
	if(s != noErr)
		return Status(s);
	if(bytes.size() < offsetof(AudioChannelLayout, mChannelDescriptions))
		return "short (" + std::to_string(bytes.size()) + " bytes)";
	const AudioChannelLayout *l = (const AudioChannelLayout *)bytes.data();
	char buf[64];
	snprintf(buf, sizeof(buf), "tag 0x%08x", (unsigned)l->mChannelLayoutTag);
	std::string r = buf;
	if(l->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelBitmap){
		snprintf(buf, sizeof(buf), " bitmap 0x%08x", (unsigned)l->mChannelBitmap);
		r += buf;
	}else if(l->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions){
		size_t room = (bytes.size() - offsetof(AudioChannelLayout, mChannelDescriptions)) / sizeof(AudioChannelDescription);
		UInt32 n = l->mNumberChannelDescriptions < room ? l->mNumberChannelDescriptions : (UInt32)room;
		r += " labels";
		for(UInt32 i = 0; i < n; i++)
			r += " " + std::to_string(l->mChannelDescriptions[i].mChannelLabel);
	}
	return r;
}

static void
DataSources(AudioObjectID dev)
{
	UInt32 current = 0;
	std::string cur = UInt32Prop(dev, kAudioDevicePropertyDataSource, kScope, &current);
	if(cur == "absent" || cur.compare(0, 5, "error") == 0){
		printf("  data source: %s\n", cur.c_str());
	}else{
		printf("  data source: %s %s\n", cur.c_str(), DataSourceName(dev, current).c_str());
	}
	if(!Has(dev, kAudioDevicePropertyDataSources, kScope)){
		printf("  data sources available: absent\n");
		return;
	}
	std::vector<UInt8> bytes;
	OSStatus s = GetBytes(dev, kAudioDevicePropertyDataSources, kScope, bytes);
	if(s != noErr){
		printf("  data sources available: %s\n", Status(s).c_str());
		return;
	}
	size_t n = bytes.size() / sizeof(UInt32);
	printf("  data sources available: %zu\n", n);
	for(size_t i = 0; i < n; i++){
		UInt32 id;
		memcpy(&id, bytes.data() + i * sizeof(UInt32), sizeof(UInt32));
		printf("    %s %s\n", FourCC(id).c_str(), DataSourceName(dev, id).c_str());
	}
}

int
main()
{
	UInt32 defaultOut = kAudioObjectUnknown;
	std::string def = UInt32Prop(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, &defaultOut);
	printf("default output device: %u (%s)\n", (unsigned)defaultOut, def.c_str());

	std::vector<UInt8> bytes;
	OSStatus s = GetBytes(kAudioObjectSystemObject, kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, bytes);
	if(s != noErr){
		printf("FAIL devices: %s\n", Status(s).c_str());
		return 1;
	}
	size_t count = bytes.size() / sizeof(AudioObjectID);
	printf("devices: %zu\n", count);

	bool defaultSeen = false;
	size_t defaultStreams = 0;
	for(size_t d = 0; d < count; d++){
		AudioObjectID dev;
		memcpy(&dev, bytes.data() + d * sizeof(AudioObjectID), sizeof(AudioObjectID));
		std::string streamError;
		std::vector<AudioStreamID> streams = Streams(dev, &streamError);
		bool isDefault = dev == defaultOut;
		if(streams.empty() && streamError.empty() && !isDefault)
			continue;
		if(isDefault){
			defaultSeen = true;
			defaultStreams = streams.size();
		}
		printf("\ndevice %u%s\n", (unsigned)dev, isDefault ? " (default output)" : "");
		printf("  name: %s\n", StringProp(dev, kAudioObjectPropertyName).c_str());
		printf("  uid: %s\n", StringProp(dev, kAudioDevicePropertyDeviceUID).c_str());
		printf("  transport: %s\n", UInt32Prop(dev, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal).c_str());
		if(!streamError.empty())
			printf("  output streams: %s\n", streamError.c_str());
		else
			printf("  output streams: %zu\n", streams.size());
		for(size_t i = 0; i < streams.size(); i++){
			UInt32 term = 0;
			std::string t = UInt32Prop(streams[i], kAudioStreamPropertyTerminalType, kAudioObjectPropertyScopeGlobal, &term);
			if(t == "absent" || t.compare(0, 5, "error") == 0)
				printf("    stream %u terminal: %s\n", (unsigned)streams[i], t.c_str());
			else
				printf("    stream %u terminal: %s (0x%x)\n", (unsigned)streams[i], t.c_str(), (unsigned)term);
		}
		DataSources(dev);
		printf("  output channels: %s\n", ChannelCount(dev).c_str());
		printf("  preferred layout: %s\n", Layout(dev).c_str());
	}
	printf("\n");
	if(!defaultSeen || defaultStreams == 0){
		printf("FAIL the default output device lists an output stream\n");
		return 1;
	}
	printf("PASS the default output device lists an output stream\n");
	return 0;
}
