// mkdir -p build/tests && clang++ -std=gnu++14 -Wall -Wextra -I src tests/audio_device_test.cpp -o build/tests/audio_device_test && build/tests/audio_device_test
#include <cstdio>
#include <cstring>
#include "CaDevice.h"

using namespace pinna;

static int failures = 0;

#define CHECK(expr) \
	do { if(!(expr)){ printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while(0)

static void
TestChooseAndRebuild(void)
{
	DeviceEntry list[3];
	memset(list, 0, sizeof(list));
	list[0].id = 10; strcpy(list[0].uid, "BuiltInSpeakerDevice"); list[0].alive = true; list[0].outputChannels = 2;
	list[1].id = 20; strcpy(list[1].uid, "A50-USB"); list[1].alive = true; list[1].outputChannels = 2;
	list[2].id = 30; strcpy(list[2].uid, "DualSense"); list[2].alive = false; list[2].outputChannels = 4;

	DeviceChoice c = ChooseDevice("default", list, 3, 10);
	CHECK(c.id == 10 && !c.fellBack);
	c = ChooseDevice("A50-USB", list, 3, 10);
	CHECK(c.id == 20 && !c.fellBack);
	c = ChooseDevice("DualSense", list, 3, 10);
	CHECK(c.id == 10 && c.fellBack);
	c = ChooseDevice("Gone", list, 3, 20);
	CHECK(c.id == 20 && c.fellBack);
	c = ChooseDevice("", list, 3, 10);
	CHECK(c.id == 10 && !c.fellBack);

	CHECK(NeedsRebuild(EVENT_DEFAULT_CHANGED, true, 10, 20, false, false));
	CHECK(!NeedsRebuild(EVENT_DEFAULT_CHANGED, false, 20, 10, true, true));
	CHECK(NeedsRebuild(EVENT_RATE_CHANGED, true, 10, 10, false, false));
	CHECK(!NeedsRebuild(EVENT_RATE_CHANGED, true, 10, 20, false, false));
	CHECK(NeedsRebuild(EVENT_DEVICE_GONE, false, 20, 20, true, true));
	CHECK(!NeedsRebuild(EVENT_DEVICE_GONE, true, 10, 20, false, false));
	CHECK(NeedsRebuild(EVENT_DEVICES_CHANGED, false, 10, 0, true, false));
	CHECK(!NeedsRebuild(EVENT_DEVICES_CHANGED, false, 20, 0, true, true));
	CHECK(!NeedsRebuild(EVENT_DEVICES_CHANGED, true, 10, 0, false, false));
	CHECK(NeedsRebuild(EVENT_WAKE, true, 10, 0, false, false));
	CHECK(NeedsRebuild(EVENT_PIN, true, 10, 0, true, false));
}

static void
TestPinState(void)
{
	DeviceEntry list[2];
	memset(list, 0, sizeof(list));
	list[0].id = 10; strcpy(list[0].uid, "Speakers"); list[0].alive = true; list[0].outputChannels = 2;
	list[1].id = 20; strcpy(list[1].uid, "Headset"); list[1].alive = true; list[1].outputChannels = 2;

	PinState p = FindPin("Headset", list, 2, 20);
	CHECK(p.present && p.isCurrent);
	p = FindPin("Headset", list, 2, 10);
	CHECK(p.present && !p.isCurrent);
	list[1].alive = false;
	p = FindPin("Headset", list, 2, 20);
	CHECK(!p.present && !p.isCurrent);
	list[1].alive = true;
	list[1].outputChannels = 0;
	p = FindPin("Headset", list, 2, 20);
	CHECK(!p.present);
	p = FindPin("default", list, 2, 10);
	CHECK(!p.present && !p.isCurrent);
}

static void
TestDecide(void)
{
	DeviceEntry list[2];
	memset(list, 0, sizeof(list));
	list[0].id = 10; strcpy(list[0].uid, "Speakers"); list[0].alive = true; list[0].outputChannels = 2;
	list[1].id = 20; strcpy(list[1].uid, "Headset"); list[1].alive = true; list[1].outputChannels = 2;

	CHECK(DecideRebuild(EVENT_START, "default", list, 2, 0, 0));
	CHECK(DecideRebuild(EVENT_DEFAULT_CHANGED, "default", list, 2, 0, 10));
	CHECK(!DecideRebuild(EVENT_DEFAULT_CHANGED, "Headset", list, 2, 20, 10));
	CHECK(DecideRebuild(EVENT_DEVICES_CHANGED, "Headset", list, 2, 10, 0));
	CHECK(!DecideRebuild(EVENT_DEVICES_CHANGED, "Headset", list, 2, 20, 0));
	list[1].alive = false;
	CHECK(!DecideRebuild(EVENT_DEFAULT_CHANGED, "Headset", list, 2, 10, 10));
	CHECK(DecideRebuild(EVENT_DEFAULT_CHANGED, "Headset", list, 2, 10, 30));
	CHECK(!DecideRebuild(EVENT_DEVICES_CHANGED, "Headset", list, 2, 10, 0));
	CHECK(DecideRebuild(EVENT_RATE_CHANGED, "default", list, 2, 0, 10));
	CHECK(!DecideRebuild(EVENT_RATE_CHANGED, "default", list, 2, 20, 10));
	CHECK(DecideRebuild(EVENT_RATE_CHANGED, "default", list, 2, 10, 10));
}

static void
TestUnknownCurrent(void)
{
	for(int event = EVENT_DEFAULT_CHANGED; event <= EVENT_RETRY; event++){
		CHECK(NeedsRebuild(event, true, 0, 0, false, false));
		CHECK(NeedsRebuild(event, true, 0, 10, false, false));
		CHECK(NeedsRebuild(event, false, 0, 10, true, false));
		CHECK(NeedsRebuild(event, false, 0, 10, false, false));
	}
	CHECK(!NeedsRebuild(EVENT_RETRY, true, 10, 0, false, false));
	CHECK(!NeedsRebuild(EVENT_RETRY, false, 20, 0, true, true));
}

static void
TestDefaultChangedSameDevice(void)
{
	CHECK(!NeedsRebuild(EVENT_DEFAULT_CHANGED, true, 10, 10, false, false));
	CHECK(NeedsRebuild(EVENT_DEFAULT_CHANGED, true, 10, 20, false, false));
	CHECK(!NeedsRebuild(EVENT_DEFAULT_CHANGED, false, 10, 10, false, false));
	CHECK(NeedsRebuild(EVENT_DEFAULT_CHANGED, false, 10, 20, false, false));
	CHECK(!NeedsRebuild(EVENT_DEFAULT_CHANGED, false, 20, 10, true, true));
}

static void
TestDeviceBatch(void)
{
	CHECK(DeviceBatchEvent(false, false, 48000.0, 2, 48000.0, 2) == EVENT_NONE);
	CHECK(DeviceBatchEvent(false, true, 48000.0, 2, 48000.0, 2) == EVENT_NONE);
	CHECK(DeviceBatchEvent(false, true, 48000.0, 2, 44100.0, 2) == EVENT_RATE_CHANGED);
	CHECK(DeviceBatchEvent(false, true, 48000.0, 2, 48000.0, 6) == EVENT_RATE_CHANGED);
	CHECK(DeviceBatchEvent(true, true, 48000.0, 2, 44100.0, 2) == EVENT_DEVICE_GONE);
	CHECK(DeviceBatchEvent(true, false, 48000.0, 2, 48000.0, 2) == EVENT_DEVICE_GONE);
	CHECK(DeviceBatchEvent(false, true, 48000.0, 6, 48000.0, 6, 100, 200) == EVENT_RATE_CHANGED);
	CHECK(DeviceBatchEvent(false, true, 48000.0, 6, 48000.0, 6, 100, 100) == EVENT_NONE);
	CHECK(DeviceBatchEvent(false, false, 48000.0, 6, 48000.0, 6, 100, 200) == EVENT_NONE);
}

static DeviceEntry
LayoutDevice(uint32_t channels, uint32_t tag)
{
	DeviceEntry d;
	memset(&d, 0, sizeof(d));
	d.outputChannels = channels;
	d.layoutTag = tag;
	return d;
}

static void
TestMultichannelLayout(void)
{
	CHECK(MultichannelLayoutTag(LayoutDevice(6, 0)) == kLayoutMpeg51A);
	CHECK(MultichannelLayoutTag(LayoutDevice(4, 0)) == kLayoutQuadraphonic);
	CHECK(MultichannelLayoutTag(LayoutDevice(5, 0)) == kLayoutMpeg50A);
	CHECK(MultichannelLayoutTag(LayoutDevice(7, 0)) == kLayoutMpeg61A);
	CHECK(MultichannelLayoutTag(LayoutDevice(8, 0)) == kLayoutMpeg71C);
	CHECK(MultichannelLayoutTag(LayoutDevice(3, 0)) == 0);
	CHECK(MultichannelLayoutTag(LayoutDevice(2, 0)) == 0);
	CHECK(MultichannelLayoutTag(LayoutDevice(12, 0)) == kLayoutMpeg71C);
	CHECK(MultichannelLayoutTag(LayoutDevice(6, kLayoutStereo)) == 0);
	CHECK(MultichannelLayoutTag(LayoutDevice(6, kLayoutMono)) == 0);
	CHECK(MultichannelLayoutTag(LayoutDevice(6, kLayoutMpeg51D)) == kLayoutMpeg51D);
	CHECK(MultichannelLayoutTag(LayoutDevice(8, kLayoutMpeg51D)) == kLayoutMpeg51D);
	CHECK(MultichannelLayoutTag(LayoutDevice(6, kLayoutMpeg71C)) == kLayoutMpeg51A);
	CHECK(MultichannelLayoutTag(LayoutDevice(8, kLayoutMpeg71C)) == kLayoutMpeg71C);
	CHECK(MultichannelLayoutTag(LayoutDevice(6, (147u << 16) | 6)) == kLayoutMpeg51A);
	CHECK(MultichannelLayoutTag(LayoutDevice(6, 0xFFFF0000u | 6)) == kLayoutMpeg51A);
	CHECK(MultichannelLayoutTag(LayoutDevice(6, 1u << 16)) == kLayoutMpeg51A);
	CHECK(MultichannelLayoutTag(LayoutDevice(10, (0x80u << 16) | 10)) == kLayoutMpeg71C);
}

static DeviceEntry
Typed(uint32_t transport, uint32_t terminal, uint32_t dataSource, uint32_t channels)
{
	DeviceEntry d;
	memset(&d, 0, sizeof(d));
	d.transport = transport;
	d.terminal = terminal;
	d.dataSource = dataSource;
	d.outputChannels = channels;
	d.alive = true;
	return d;
}

static void
TestTerminals(void)
{
	uint32_t a50[1] = { FourCC('h', 'd', 'p', 'h') }, second[2] = { 0, FourCC('h', 'd', 'p', 'h') }, mac[1] = { 0x301 };
	uint32_t spkr[2] = { FourCC('s', 'p', 'k', 'r'), FourCC('l', 'i', 'n', 'e') };
	CHECK(SummariseTerminals(a50, 1) == kTerminalHeadphones);
	CHECK(SummariseTerminals(second, 2) == kTerminalHeadphones);
	CHECK(SummariseTerminals(mac, 1) == 0x301);
	CHECK(SummariseTerminals(spkr, 2) == kTerminalSpeaker);
	CHECK(SummariseTerminals(spkr, 0) == 0);
	CHECK(FourCC('u', 's', 'b', ' ') == 0x75736220u && kTransportUSB == 0x75736220u && kTransportBuiltIn == 0x626c746eu);
}

static void
TestOutputType(void)
{
	DeviceEntry a50Main = Typed(kTransportUSB, kTerminalHeadphones, 0, 2), a50Voice = a50Main;
	DeviceEntry display = Typed(kTransportDisplayPort, 0, 0, 2);
	DeviceEntry macSpeakers = Typed(kTransportBuiltIn, 0x301, kDataSourceInternalSpeaker, 2);
	DeviceEntry teams = Typed(kTransportVirtual, kTerminalSpeaker, 0, 1);
	CHECK(DetectOutputType(a50Main) == OUTPUT_HEADPHONES && DetectOutputType(a50Voice) == OUTPUT_HEADPHONES);
	CHECK(DetectOutputType(display) == OUTPUT_EXTERNAL);
	CHECK(DetectOutputType(macSpeakers) == OUTPUT_BUILTIN);
	CHECK(DetectOutputType(teams) == OUTPUT_EXTERNAL);

	CHECK(DetectOutputType(Typed(kTransportBuiltIn, 0x301, kDataSourceHeadphones, 2)) == OUTPUT_HEADPHONES);
	CHECK(DetectOutputType(Typed(kTransportBuiltIn, 0, 0, 2)) == OUTPUT_EXTERNAL);
	CHECK(DetectOutputType(Typed(kTransportUSB, kTerminalHeadphones, 0, 6)) == OUTPUT_HEADPHONES);
	CHECK(DetectOutputType(Typed(kTransportBuiltIn, 0, kDataSourceHeadphones, 6)) == OUTPUT_HEADPHONES);
	CHECK(DetectOutputType(Typed(kTransportUSB, 0, 0, 6)) == OUTPUT_HEADPHONES);
	CHECK(DetectOutputType(Typed(kTransportHDMI, 0, 0, 8)) == OUTPUT_EXTERNAL);
	CHECK(DetectOutputType(Typed(kTransportBuiltIn, 0, kDataSourceInternalSpeaker, 4)) == OUTPUT_BUILTIN);
	CHECK(DetectOutputType(Typed(kTransportUSB, 0, 0, 2)) == OUTPUT_HEADPHONES);
	CHECK(DetectOutputType(Typed(kTransportBluetooth, 0, 0, 2)) == OUTPUT_HEADPHONES);
	CHECK(DetectOutputType(Typed(kTransportBluetoothLE, 0, 0, 2)) == OUTPUT_HEADPHONES);
	CHECK(DetectOutputType(Typed(kTransportBluetooth, kTerminalHeadphones, 0, 2)) == OUTPUT_HEADPHONES);
	CHECK(DetectOutputType(Typed(kTransportUSB, kTerminalSpeaker, 0, 2)) == OUTPUT_EXTERNAL);
	CHECK(DetectOutputType(Typed(kTransportUSB, kTerminalLine, 0, 2)) == OUTPUT_EXTERNAL);
	CHECK(DetectOutputType(Typed(kTransportBluetooth, kTerminalSpeaker, 0, 2)) == OUTPUT_EXTERNAL);
	CHECK(DetectOutputType(Typed(kTransportHDMI, 0, 0, 2)) == OUTPUT_EXTERNAL);
	CHECK(DetectOutputType(Typed(kTransportVirtual, 0, 0, 2)) == OUTPUT_EXTERNAL);
	CHECK(DetectOutputType(Typed(kTransportVirtual, kTerminalHeadphones, 0, 2)) == OUTPUT_HEADPHONES);

	CHECK(EffectiveOutputType(OUTPUT_AUTO, a50Main) == OUTPUT_HEADPHONES);
	CHECK(EffectiveOutputType(OUTPUT_EXTERNAL, a50Main) == OUTPUT_EXTERNAL);
	CHECK(EffectiveOutputType(OUTPUT_HEADPHONES, display) == OUTPUT_HEADPHONES);
	CHECK(EffectiveOutputType(OUTPUT_BUILTIN, a50Main) == OUTPUT_BUILTIN);
	CHECK(EffectiveOutputType(OUTPUT_MULTICHANNEL, macSpeakers) == OUTPUT_BUILTIN);
	CHECK(EffectiveOutputType(OUTPUT_AUTO, display) == OUTPUT_EXTERNAL);
	CHECK(EffectiveOutputType(-4, display) == OUTPUT_EXTERNAL && EffectiveOutputType(NUM_OUTPUTS, a50Main) == OUTPUT_HEADPHONES);
}

static DeviceEntry
Coded(const char *transport, uint32_t terminal, const char *dataSource, uint32_t channels)
{
	return Typed(FourCC(transport[0], transport[1], transport[2], transport[3]), terminal,
		dataSource ? FourCC(dataSource[0], dataSource[1], dataSource[2], dataSource[3]) : 0, channels);
}

static void
TestLiteralCodes(void)
{
	uint32_t hdph = FourCC('h', 'd', 'p', 'h');
	CHECK(hdph == 0x68647068u);
	CHECK(DetectOutputType(Coded("bltn", 0x301, "hdpn", 2)) == OUTPUT_HEADPHONES);
	CHECK(DetectOutputType(Coded("bltn", 0x301, "ispk", 2)) == OUTPUT_BUILTIN);
	CHECK(DetectOutputType(Coded("usb ", 0, 0, 2)) == OUTPUT_HEADPHONES);
	CHECK(DetectOutputType(Coded("dprt", 0, 0, 2)) == OUTPUT_EXTERNAL);
	CHECK(DetectOutputType(Coded("hdmi", 0, 0, 2)) == OUTPUT_EXTERNAL);
	CHECK(DetectOutputType(Coded("blue", hdph, 0, 2)) == OUTPUT_HEADPHONES);
	DeviceEntry usb6 = Coded("usb ", 0, 0, 6);
	CHECK(EffectiveOutputType(OUTPUT_MULTICHANNEL, usb6) == OUTPUT_MULTICHANNEL);
	CHECK(EffectiveOutputType(OUTPUT_AUTO, usb6) == OUTPUT_HEADPHONES);
	CHECK(EffectiveOutputType(OUTPUT_AUTO, Coded("usb ", FourCC('s', 'p', 'k', 'r'), 0, 6)) == OUTPUT_EXTERNAL);
	DeviceEntry hdmi8 = Coded("hdmi", 0, 0, 8);
	CHECK(EffectiveOutputType(OUTPUT_AUTO, hdmi8) == OUTPUT_EXTERNAL);
	CHECK(EffectiveOutputType(OUTPUT_MULTICHANNEL, hdmi8) == OUTPUT_MULTICHANNEL);
	CHECK(EffectiveOutputType(OUTPUT_MULTICHANNEL, Coded("hdmi", 0, 0, 2)) == OUTPUT_EXTERNAL);
	CHECK(EffectiveOutputType(OUTPUT_EXTERNAL, Coded("usb ", 0, 0, 2)) == OUTPUT_EXTERNAL);
	CHECK(EffectiveOutputType(OUTPUT_AUTO, Coded("usb ", 0, 0, 2)) == OUTPUT_HEADPHONES);
	CHECK(EffectiveOutputType(OUTPUT_EXTERNAL, Coded("bltn", 0x301, "hdpn", 2)) == OUTPUT_EXTERNAL);
}

static void
TestOwnerDevices(void)
{
	DeviceEntry a50Main = Coded("usb ", 0x68647068, 0, 2);
	DeviceEntry a50Voice = Coded("usb ", 0x68647068, 0, 2);
	DeviceEntry display = Coded("dprt", 0, 0, 2);
	DeviceEntry mac = Coded("bltn", 0x301, "ispk", 2);
	CHECK(DetectOutputType(a50Main) == OUTPUT_HEADPHONES && DetectOutputType(a50Voice) == OUTPUT_HEADPHONES);
	CHECK(DetectOutputType(display) == OUTPUT_EXTERNAL);
	CHECK(DetectOutputType(mac) == OUTPUT_BUILTIN);
}

static void
TestDeviceChoiceChanges(void)
{
	CHECK(!DeviceChoiceChanges("default", "default") && !DeviceChoiceChanges("", "default"));
	CHECK(!DeviceChoiceChanges("A50-USB", "A50-USB"));
	CHECK(DeviceChoiceChanges("A50-USB", "default") && DeviceChoiceChanges("default", "A50-USB"));
	CHECK(DeviceChoiceChanges("A50-USB", "DualSense"));
}

int
main(void)
{
	TestLiteralCodes();
	TestOwnerDevices();
	TestTerminals();
	TestOutputType();
	TestChooseAndRebuild();
	TestPinState();
	TestDecide();
	TestUnknownCurrent();
	TestDefaultChangedSameDevice();
	TestDeviceBatch();
	TestMultichannelLayout();
	TestDeviceChoiceChanges();
	if(failures){
		printf("%d failures\n", failures);
		return 1;
	}
	printf("all device tests passed\n");
	return 0;
}
