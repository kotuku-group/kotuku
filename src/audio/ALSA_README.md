# Linux ALSA playback and validation

An `Audio` object requests three 256-frame periods by default.  ALSA may adjust both values.  Read `periodSize`,
`periods`, `outputRate` and `mixerLag` after activation; `mixerLag` uses the actual buffer size rather than reconstructing
it from the period count.  At 48 kHz, a 768-frame buffer represents 16 ms.  Smaller periods increase scheduling pressure.
The fallback is one retry with four 1024-frame periods.

`default` follows the desktop routing configuration.  `hw:0,0` accesses a device directly and may require exclusive access
and an exact sample format.  `plughw:0,0` adds ALSA format conversion.  `null` consumes data without real-time pacing and
is suitable for functional tests only.  ALSA output supports 8-bit unsigned, 16-bit signed and 32-bit float, mono/stereo;
24-bit output is unsupported.

Saved `user:config/audio.cfg` uses the `AUDIO` keys `PeriodFrames` and `Periods`.  Legacy `PeriodSize` bytes are converted
using saved bit depth and channels.  `StreamBufferMs` is a separate prefetch duration: default 1000, range 100–10000 ms,
limited to 8 MiB per stream and 64 MiB per Audio object.  It is based on output rate, so higher source frequencies shorten
its effective duration.  It does not add PCM output latency.  Process client messages to refill streams and receive
completion callbacks; memory-resident playback continues while the client is blocked.  Starved streams insert silence
without advancing their source position and resume when data becomes available.

## Automated tests

Build and install the Debug configuration before running:

```sh
ctest --build-config Debug --test-dir build/agents --output-on-failure -L audio
```

`AudioBufferPolicy` checks units, public clamps, ring geometry and consumption during producer delays.  `AudioWorker`
runs the production worker loop against a compiled PCM adapter with controlled write, availability, start, resume,
recovery and wait results.  It checks retained sample order and stop/join using eventfd wakeups.  The silent Flute suite
uses ALSA `null` to cover actual module integration, command sequences/overflow, callbacks, loops, stream starvation,
activation failure and lifecycle cancellation.  No fault injection is exposed through the public API.

Real-device tests are opt-in and submit silence or use zero Sound volume:

```sh
cmake -S . -B build/agents -DKOTUKU_AUDIO_HARDWARE_TESTS=ON
cmake --build build/agents --config Debug --parallel
cmake --install build/agents --config Debug
ctest --build-config Debug --test-dir build/agents --output-on-failure -L hardware
```

These tests require the configured ALSA output device.  Missing/busy hardware causes failure, rather than a false pass.
They use Flute's `@Requires(audio=true, linux_platform=true)` and check negotiated settings, immediate Sound replay,
completion bounds and a client stall after stream prefill.  The older `test_audio.tiri` remains a manually invoked audible
suite.  Restore `KOTUKU_AUDIO_HARDWARE_TESTS=OFF` for runners without a device.

## Queue diagnostics and acoustic measurements

Run Origo with `--log-info` (or `--log-api` for more detail).  Activation logs requested/effective geometry and the negotiated
buffer duration.  Deactivation logs underruns, source-starvation episodes, recovery failures and the sample count,
mean and maximum observed `snd_pcm_delay()` values.  Delay samples are taken once per worker servicing pass and while
waiting for the final drain; negative/unsupported results are excluded.  Summaries are emitted after joining the worker,
so logging never blocks PCM servicing.  The mean is sample-weighted, not time-weighted.  Neither these values nor
`mixerLag` establishes acoustic onset latency.
Some PCM plugins report delay exceeding their negotiated buffer duration; retain both measurements in reports.

For acoustic validation, record an independent trigger together with output using a loopback cable or microphone.
Measure `default` and an available direct `hw:` device at 44100/48000 Hz, requesting 128/256/512/1024 frames with 2/3/4
periods.  Include first play after idle, rapid and overlapping effects, streams, client stalls and scheduler load.
Record actual negotiated geometry, underruns and source-starvation counts with median, 95th-percentile and maximum onset.
Keep acoustic measurements separate from callback-completion timing.  Choose a smaller default only when repeated runs
show stable underrun behaviour.  Virtual PCM tests cannot establish that result.
