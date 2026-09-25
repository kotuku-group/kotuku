# Windows playback and validation

Windows 10 and later use one event-driven shared-mode WASAPI stream per active `Audio` object.  Ordinary `Sound`
playback always traverses the same mixer, channel-set effects and global effects as low-level channels.  Sounds on
the same Audio retain their shared channel set; open separate sets when independent application effects are needed.
The per-Sound DirectSound implementation has been removed.

## Format and scheduling

The transport follows the endpoint mix rate.  The mixer and effects process normalised 32-bit float, mono or stereo.
Mono/stereo float endpoints receive the mixer's final clipped copy directly.  Other supported endpoint formats use
a prepared intermediate buffer and a final layout/PCM conversion.  Wider layouts must identify front left/right in
their channel mask; those speakers receive the stereo mix and all remaining speakers receive silence.  PCM supports
8/16/24/32-bit containers, including left-aligned valid bits.  Unsupported masks/formats fail activation explicitly.

`IAudioClient3` selects the supported default period.  Setting `PeriodSize` below that default before activation
requests a shorter period, rounded up to the device's fundamental period and minimum.  This is an advanced latency
request that must be validated against the application's workload.  No universal minimum-period recommendation is
made.  Without `IAudioClient3`, or if its initialisation fails, a fresh `IAudioClient` uses shared event-driven output
with the default device period.  There is no DirectSound fallback.

COM initialisation, endpoint/client/service acquisition, buffer acquisition/release and interface destruction all
belong to the transport thread.  An activation handshake lets the client prepare mixer and effect storage before
rendering starts.  Device, command and shutdown waits run outside the mixer lock, with MMCSS `Audio` registration
scoped to rendering.  There is no process-wide timer-resolution change.  The render callback uses a nonblocking
lock attempt; client contention produces a silent packet and is counted separately from source starvation.

Callbacks and file decoding remain on the Core client thread.  Windows shares ALSA's source rings, bounded command
queues, generation checks and deferred completion delivery.  Source prefetch remains independent of endpoint
capacity.  Client callback retries use a timer; endpoint rendering is driven by WASAPI events.

## Lifecycle

Playback primes the endpoint before starting and drains submitted frames before stopping an idle stream.  Effect
chains receive at most two seconds of additional silent input after sources stop.  This provisional bounded tail
policy covers existing equalisers but is not a general tail/latency contract for future reverbs or indefinite-tail
processors.  Source completion callbacks account for submitted endpoint audio, not the entire effect tail.

`Sound.Deactivate` and seek affect future mixing; already submitted mixed audio cannot be withdrawn for one Sound.
`Audio.Deactivate` stops the whole endpoint and discards its queue.  `MixerLag` estimates remaining submitted frames
from the last padding observation and elapsed time, excluding downstream driver/wireless/acoustic latency.
Windows Sound position subtracts that estimate from the render cursor.

Device invalidation and default-console-endpoint changes notify the client, which closes/joins the failed transport,
renegotiates the endpoint and resets DSP before restarting.  Source cursors, rings and pending commands survive this
reopen; audio already submitted to the lost endpoint can be lost.  Reopening retries at 250 ms intervals, up to 20
attempts, then reports failure and stops.  Endpoint IDs or unique friendly names can be assigned through `Device`;
`default` follows the
default console endpoint.  Physical unplug, default-device switching and suspend/resume still require manual
hardware qualification.

## Tests and diagnostics

With `BUILD_TESTS=ON`, the Windows module additionally supports `Device='null'` and
`Device='capture:<native file path>'`.  These are test-only simulated endpoints.  Capture reserves thirty seconds
of PCM before publication and writes interleaved native float data after joining, never from the render callback.
Packets vary in length, and capture stops storing after its bounded capacity.  The simulated clock is not a latency
reference.  These endpoints are absent when tests are disabled.

`test_windows_effects.tiri` checks captured Sound PCM through application/global effects and bypass, real source
streaming, live attachment/rapid edits, target destruction, scope isolation, shared Sound sets, and
seek/pause/replay/completion, producer stalls, bounded drain and destruction during an EQ transition.
`AudioWasapiTransport` checks variable packets, command wakes, shutdown, simulated
invalidation, layout/PCM conversion and no C++ heap allocations/deallocations during its warmed simulated render loop.
That allocation assertion does not instrument Core/Tiri publication or the Windows audio service.

The opt-in `test_windows_hardware.tiri` runs real Sound completion/reactivation and a quiet 1/16/64 voice by
0/1/8/64 EQ-band workload matrix with rapid edits.  Enable `KOTUKU_AUDIO_HARDWARE_TESTS` to register it with CTest, or
run it directly using the installed Origo and `--log-info`.  It is separate from deterministic tests.

After joining, `--log-info` reports render mean/p50/p95/p99/max, wakeups, empty queues, source starvation, mixer
contention, queued frames and process CPU.  Percentiles are upper bounds in 100 microsecond buckets; the last bucket
is overflow.  Empty queues are an underrun indicator, not proof of an audible glitch.  Process CPU includes client
work and activation.  Measurements and outstanding acceptance gates are recorded in
[`1_windows_audio_effects.md`](../../docs/plans/audio/1_windows_audio_effects.md).
