# pinna

An audio engine library meant for older games of the GTA III era. It mixes
voices and streams, places sounds around the listener, adds reverb that follows
the place the listener is in and echoes off nearby walls, and sends the result
to an output backend. Core Audio is the first backend.

The pinna is the outer ear. Its shape is what lets you hear where a sound
comes from.

## What it is

A C++14 library in the namespace `pinna`. The engine holds the clock, the
render drive, the lock-free handoff between the host and the audio thread, the
mixer, the voices, the streams and their ring, the resampler, filters, fades,
loop seams, panning and spatial rules, the echo and place rules, the device
rules, the decoder interface with its length caches, the settings object and the
stats. It carries no backend code, though the device rules speak Core Audio's
codes (transport types, terminal types, channel layout tags), and the clock and
stream threads set Apple quality-of-service classes under `__APPLE__`. A backend
supplies the output, the device watching and the file decoders. File names keep
the `Ca` prefix they started with.

## Layout

- `src/` the engine, headers and sources, and `CaShell.h`, the interface a
  backend implements and a host calls.
- `src/coreaudio/` the Core Audio module: the output with its device watchers,
  the Apple reverb and spatial mixer units, the AudioToolbox decoders and the
  offline shell (`CaShellOffline.h`). Objective-C++, compiled only where Core
  Audio exists.
- `tests/` the pure tests (`*_test.cpp`), `tests/audio_offline/` with
  `shell_check.mm` (the Core Audio module on fake devices and the offline shell),
  `hooks_check.mm` (the decode hooks on generated files) and `device_types.mm`,
  `tests/support/` with the helpers the tests share, the scripts below, and
  `audio_small_config.h`.

## Tests

Every test file starts with a comment line that is its own build and run
command, written for the repository root. Build output goes to `build/tests/`.

    tests/run.sh
    tests/run.sh --config tests/audio_small_config.h

`run.sh` builds every test, runs each binary under a 60 second limit, prints
`PASS <name>` or `FAIL <name>` per test and a last line `<n> of <m> passed`, and
exits non-zero when any test fails. `tests/build.sh` only builds. With
`--config` every test is built with `PINNA_CONFIG_HEADER` set to the given
header (8 channels, 1 stream, 17 pool slots in the one provided).

The ThreadSanitizer runs are not part of `run.sh`. Run these four from the
repository root and expect no `WARNING: ThreadSanitizer` line:

    mkdir -p build/tests && clang++ -std=gnu++14 -O2 -g -fsanitize=thread -Wall -Wextra -pthread -I src tests/audio_drive_test.cpp src/CaClock.cpp -o build/tests/audio_drive_test_tsan && build/tests/audio_drive_test_tsan
    mkdir -p build/tests && clang++ -std=gnu++14 -O2 -g -fsanitize=thread -Wall -Wextra -pthread -I src tests/audio_handoff_test.cpp -o build/tests/audio_handoff_test_tsan && build/tests/audio_handoff_test_tsan
    mkdir -p build/tests && clang++ -std=gnu++14 -O1 -g -fsanitize=thread -Wall -Wextra -pthread -I src tests/audio_stream_test.cpp src/CaStream.cpp src/CaResampler.cpp src/CaVoice.cpp src/CaMixer.cpp src/CaClock.cpp -o build/tests/audio_stream_test_tsan && build/tests/audio_stream_test_tsan
    mkdir -p build/tests && clang++ -std=gnu++14 -O2 -g -fsanitize=thread -Wall -Wextra -pthread -I src tests/audio_start_test.cpp src/CaResampler.cpp src/CaVoice.cpp src/CaMixer.cpp src/CaStream.cpp -o build/tests/audio_start_test_tsan && build/tests/audio_start_test_tsan

`tests/names_check.sh` greps every tracked file except this README, the licence
and the script itself for words that belong to a host program, and checks that
every quoted include resolves inside the tree. It prints `PASS <label>` or
`FAIL <label>` per check and a total, and exits non-zero on any failure.

## Configuration

Sizes are fixed at compile time because about 44 arrays are sized by them. The
host may provide a header and name it with `-DPINNA_CONFIG_HEADER='"path"'`
(a quoted include), or set the macros with `-D` directly. `src/CaConfig.h`
reads them:

- `PINNA_CHANNELS`, the number of voice channels. Default 44. At least 1.
- `PINNA_STREAMS`, the number of streams. Default 3. From 1 to 255, because
  stream indices are bytes.
- `PINNA_POOL_SLOTS`, the number of decoded-sample pool slots. Default 96. From
  1 to 127, because slots are signed bytes, and more than twice
  `PINNA_CHANNELS`, so a start cannot fail once every channel holds a current
  and a reader slot.

The static asserts in `src/CaConfig.h` reject anything outside those limits.
Build every translation unit of the library and the host with the same values.

The log prefix and the dispatch queue label are start arguments, not compile
time settings. The defaults are `pinna:` and `pinna.audio.control`. A line
logged before `ShellStart` carries the prefix last set with `SetLogPrefix` (in
`src/CaLog.h`), or the default; call it first to change that.

## Decode hooks

The host decides which files the decoders open and how. It fills a
`DecodeHooks` and installs it with `SetDecodeHooks` before anything probes a
file; the call is refused with a logged line while a decoder is open, and a null
pointer restores the defaults (`.wav` and `.mp3`, plain).

- `describe(path, ctx)` returns a `DecodeFormat` for each file: whether it is
  `accepted`, whether it is `transformed`, and `decodeAs`, the known format
  (extension without the dot) to decode it as. A `DecodeHooks` without
  `describe` is refused with a logged line and the installed hooks stay.
- `transform(bytes, n, offset, ctx)` rewrites `n` bytes read from the file at
  byte `offset`, in place, before they reach the decoder. A format described as
  transformed needs it, or its files are refused.

The host owns `ctx`.

## Using pinna in a host

Build. Compile every `src/*.cpp` and every `src/coreaudio/*.mm` with
`-std=gnu++14`, `-fobjc-arc` for the `.mm` files, `-I src -I src/coreaudio` and
`-DPINNA_COREAUDIO`, and link AudioToolbox, CoreAudio, CoreFoundation and
AppKit. The minimum is macOS 14. `PINNA_COREAUDIO` is the module's own gate:
without it `CaDecoders.mm` and `CaOutput.mm` compile to nothing and every
`Shell*` call fails to link. Define the configuration macros (see above) the
same way for the library and the host. `CA_SHELL_OFFLINE` belongs to the tests
and tools and must stay undefined in a shipping build.

Start order, from the thread that owns the engine:

    pinna::SetDecodeHooks(&hooks);                    // before anything probes a file
    // then LoadStreamLengths, LoadTrackCache, UserTrackLengthMs as needed
    pinna::InitResampler();                           // ShellStart does not call it; the sinc tables are empty without it
    pinna::Shared *shared = new pinna::Shared();
    pinna::ShellConfig names = { "host:", "host.audio.control" };
    pinna::ShellStart(shared, deviceUid, outputPref, algorithm, &names);
    pinna::StreamWorker *worker = new pinna::StreamWorker(shared->ring, shared->status, pinna::OpenDecoder, true);

Stop in reverse: delete the worker, call `ShellStop()`, then delete `Shared`.

Each frame. The host keeps a `Snapshot` mirror of its voices and a pool of
decoded samples. From one thread it calls `ResetPoolRefs` once at start, then
`StartFromPool` to copy a sample into a free pool slot, `ArmVoice` to set a
voice up from it, `ChannelUsed` to ask whether a voice still plays, and
`PublishMirror` to hand the mirror to the audio thread (all in `src/CaStart.h`).
The same thread calls `ShellService()` once per frame. The audio thread reads the
published snapshot through a triple buffer.

Settings. `AudioEnginePrefs` (in `src/CaPrefs.h`) holds the output mode, output
type, device, spatial algorithm, reverb, echo, head tracking and reverb amount.
Call `PrefsDefaults(p)`, then `PrefsLoad(p, read)` and later `PrefsSave(p, store)`.
`read` is any callable `bool read(const char *key, char *out, int size)` that
fills `out` with the stored text and returns whether the key exists; `store` is
any callable `void store(const char *key, const char *value)`. The keys are
`Mode`, `OutputType`, `OutputDevice`, `SpatialAlgorithm`, `Reverb`, `Echo`,
`HeadTracking` and `ReverbAmount`. `PrefsLoad` returns whether `OutputType` was
read. The library does no file access for settings.

Units. Volumes run from 0 to 127 and scale with `>> 14` (`ScaledVolume`,
`GainFromVolume` in `src/CaRules.h`). Loop points are byte offsets into 16-bit
mono samples, and a start of 0 or -1 means the whole sample (`OalLoopRange`).
Distances are metres: `kSpeedOfSound` is 343.3 and the place thresholds in
`src/CaEnvironment.h` are in metres. Positions use OpenAL's relative-source
axes (`OalPanGains` in `src/CaPan.h`).

## Backend

`src/coreaudio/` is the Core Audio module and the first backend. The engine in
`src/` calls it only through `src/CaShell.h` and the decoder interface in
`src/CaDecoder.h`, so a second backend implements those two headers.

## Licence

MIT. See `LICENSE`.
