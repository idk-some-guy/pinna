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
stats. It carries no platform code. A backend supplies the output, the device
watching and the file decoders. File names keep the `Ca` prefix they started
with.

## Layout

- `src/` the engine, headers and sources.
- `src/coreaudio/` the Core Audio module: the output with its device watchers,
  the Apple reverb and spatial mixer units, the AudioToolbox decoders and the
  offline shell. Objective-C++, compiled only where Core Audio exists.
- `tests/` the pure tests (`*_test.cpp`), `tests/audio_offline/` with
  `shell_check.mm` (the Core Audio module on fake devices and the offline shell)
  and `device_types.mm`, `tests/support/` with the helpers the tests share, the
  scripts below, and `audio_small_config.h`.

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
time settings. The defaults are `pinna:` and `pinna.audio.control`.

## Decode hooks

The host decides which files the decoders open and how. It fills a
`DecodeHooks` and installs it with `SetDecodeHooks` before anything probes a
file; the call is refused with a logged line while a decoder is open, and a null
pointer restores the defaults (`.wav` and `.mp3`, plain).

- `describe(path, ctx)` returns a `DecodeFormat` for each file: whether it is
  `accepted`, whether it is `transformed`, and `decodeAs`, the known format
  (extension without the dot) to decode it as.
- `transform(bytes, n, offset, ctx)` rewrites `n` bytes read from the file at
  byte `offset`, in place, before they reach the decoder. A format described as
  transformed needs it, or its files are refused.

The host owns `ctx`.

## Backend

`src/coreaudio/` is the Core Audio module. The engine in `src/` has no platform
code.

## Licence

MIT. See `LICENSE`.
