# Receiver audio

## Format and ownership

Analog demodulators produce one channel of 48,000 Hz floating-point audio
inside the DSP layer. The platform audio service duplicates that mono signal
into a 48 kHz stereo sink. DSD-FME decoded output is 8 kHz interleaved stereo
native IEEE-754 float32; the process service preserves its two channels and
resamples them once to 48 kHz stereo. The service converts both sources to
interleaved signed 16-bit PCM for Qt
Multimedia's `QAudioSink`. The fixed rate keeps the
application/DSP boundary stable across receiver sample rates and demodulation
modes.

## Requested and effective receiver rates

The receiver state retains the **requested** hardware rate, which is the value
validated against device capabilities and shown by configuration controls. Once
the selected SoapySDR device accepts that request, the device layer reads back
its **effective** receive rate. The backend rounds that driver value to the
nearest hertz, which is the integer-rate contract used by GNU Radio's rational
resampler and display-frame metadata.

The effective rate, not the request, is used to construct channel-filter taps,
frequency translation timing, FFT frame mapping, FM discriminator gain, and
the rational conversion to 48 kHz. The GUI maps spectrum, waterfall, listening
position, and filter markers from that same effective rate while retaining the
requested rate separately for configuration state.

The translating channel filter chooses the largest integer-exact decimation
whose output rate preserves the selected passband and its FIR transition on
both sides. USB and LSB use the highest one-sided passband edge in that
calculation. A four-times-audio-rate floor leaves practical resampler
transition room and prevents narrow-mode scheduler batches from spanning the
bounded audio transport; wider filters, including maximum-width WFM, raise the
intermediate rate automatically. The resulting intermediate rate is used for
FM discriminator gain and rational conversion to 48 kHz. Mode and filter
changes rebuild this channel chain and flush old audio. The spectrum branch
continues to consume the original effective-rate wideband input.

GNU Radio blocks, deemphasis, filtering, and resampling remain in the DSP
backend. Audio-device enumeration, selection, format validation, volume, mute,
and `QAudioSink` lifecycle remain in `platform/AudioOutputService`. The service
is constructed and used on the receiver runtime thread. QML only displays
confirmed application-model state and sends control requests.

Recorded WAV playback is intentionally separate from that RF chain. Its
standard-C++ source reader decodes supported RIFF/WAVE PCM or float frames at
the file rate, preserves stereo pairs through a bounded 48 kHz output handoff,
and lets `AudioOutputService` apply its existing volume/mute policy.
The visualization downmix is `(left + right) / 2`; it does
not alter speaker or recording channels. WAV playback cannot be treated as IQ,
demodulated, squelched, scanned, or captured again as IQ.

## Recording path

The receiver runtime sends analog or decoded 48 kHz stereo samples to the WAV
recording service before sending them to `AudioOutputService`. Consequently,
recordings contain received audio before speaker volume and mute; changing
either playback control does not alter an active recording. The WAV service
converts samples to 16-bit stereo PCM and writes them on its own bounded,
background writer thread. It remains active across manual and scanner retunes.

Manual **Skip quiet parts** uses the receiver squelch gate to keep a bounded
pre-roll, open the file when squelch opens, and retain a bounded tail after it
closes. Longer quiet intervals are omitted. Scanner activity uses the same
squelch-gated service to produce separate filtered-audio clips and JSON
sidecars; it can run alongside manual recording.

Full-bandwidth IQ is a separate path: the GNU Radio backend publishes complex
samples to a bounded buffer only while IQ capture is enabled, the receiver
runtime drains that handoff, and `IqRecordingService` writes interleaved
little-endian float32 I/Q (`cf32_le`) to `.raw` with a JSON sidecar on its own
writer thread. A hardware-center or capture-rate change finalizes the current
segment and starts another; listening-only tuning does not. IQ capture is
therefore not confined to an unwritable flowgraph-local buffer.

## DSP chains

Analog paths begin after frequency translation, channel decimation, the
selected channel filter, and squelch. A rational polyphase resampler uses the
reduced integer ratio between the intermediate channel rate and 48 kHz.

* AM uses a restrained complex AGC, magnitude demodulation, rational
  resampling, and a DC blocker. The AGC reference is 0.35 with gain capped at
  50, so weak signals and receiver noise remain audible without unbounded
  amplification.
* NFM uses a 5 kHz-deviation quadrature discriminator, rational resampling, and
  300 microsecond single-pole deemphasis.
* WFM uses a 75 kHz-deviation quadrature discriminator, rational resampling,
  and 75 microsecond single-pole deemphasis. WFM audio is mono; stereo pilot
  and RDS decoding are not implemented.
* USB and LSB first decimate a symmetric narrow channel, apply the same bounded
  AGC, then select the positive or negative sideband with a second FIR at the
  reduced channel rate. Real-component detection, rational resampling, and a
  300 Hz to selected sideband-width audio FIR filter capped at 4 kHz follow.
* Experimental DMR/P25 support uses its own 12.5 kHz-default
  flat quadrature-discriminator branch directly after the channel filter,
  bypassing analog squelch and deemphasis. It requires a separately installed
  DSD-FME executable. Bounded, DC-removed discriminator samples are rationally
  resampled to 48 kHz mono and queued as signed 16-bit little-endian PCM for
  DSD-FME. DSD-FME stdout is reconstructed as fragmented eight-byte 8 kHz
  interleaved stereo native IEEE-754 float32 frames. Non-finite values become
  silence, finite values are clamped to unity, and the distinct left/right
  timeslot channels are resampled once to 48 kHz stereo. Decoding reliability
  may vary, and FEC errors or audio underruns may occur. Encrypted traffic is
  not decoded.

A mode change updates the selector and filters, then flushes both the DSP queue
and platform playback queue. Samples produced by the previous mode are never
deliberately replayed under the new mode.

## Bounded buffering

The analog DSP-to-runtime queue holds at most 2,880 samples, or 60 ms at 48 kHz, to
absorb bounded GNU Radio scheduler batches after channel decimation. The
platform playback queue, including a partially written PCM block, holds at
most 2,400 samples (50 ms), and the Qt sink is configured for 40 ms with a
30 ms service target. The combined bounded audio path is therefore no more
than about 150 ms. Capacity-aware transfers move at most 1,920 frames per
service call and leave excess samples in the DSP queue instead of overflowing
the platform queue. GNU Radio never waits for the sound device.
If a producer would exceed a queue's capacity, the oldest samples are discarded
and the newest live samples are retained. The platform service counts overflow
events and dropped samples.
Overflow and underrun status diagnostics are rate-limited to one report per
second, so a slow sound device cannot flood the receiver status path.

Spectrum/status polling remains a 33 ms coarse timer. Audio transfer and sink
service use a separate 5 ms precise timer. Playback does not start until the
service has collected 1,440 fresh samples (30 ms), then fills the sink's
30 ms target. Each pass transfers at most 1,920 frames when the platform queue
has capacity. This maintains 48,000 frames/second even when the display poll
is late, without interpreting the sink's full free capacity as an immediate
audio demand. Partial `QIODevice` byte writes remain pending and are retried on
later passes, including a partial PCM sample; no write path waits indefinitely.

The service holds about 30 ms in the Qt sink after startup. If source audio is briefly short,
it supplies silence only for the missing portion of that bounded target; an
empty software buffer counts as one underrun per contiguous starvation period.
The Qt sink separately reports platform underruns. Device enumeration is not on
the real-time path: it occurs at startup, explicit selection, and a separate
five-second refresh timer.

With `--verbose`, the receiver logs once per second: the actual DSP producer
rate, precise-service timer rate, sink write rate, upstream/output/sink queue
levels, dropped samples, software and platform underruns, and overflows. These
counters make sustained playback regressions visible without placing
diagnostics on the audio path.

While DMR/P25 mode is active, the same option adds one bounded decoder-input
summary per second. It covers application-measured discriminator level and
clipping, input drops, queued stdin bytes, partial or failed writes, decoded
stdout backlog, and decoder-mode audio underruns. DSD-FME stderr remains opaque
decoder-reported text and is not converted into inferred frame or FEC counts.

The GNU Radio hardware backend also reports rate-limited RMS and peak levels
after the channel filter, active demodulator, resampler, audio filter, and
bounded audio-sink route. USB and LSB include their reduced-rate sideband FIR
stage. These verbose-only measurements help distinguish a quiet input from a
silent processing stage without adding permanent GUI controls.

The audio-output regression uses a deterministic clocked fake sink with
byte-granular partial writes and repeating 3–12 ms service jitter over several
seconds. It verifies startup prefill, stop, mode-boundary flushing, bounded
queues, and that the required average frame rate is delivered without
continuous underruns.

Stop first waits for receiver/DSP shutdown, then stops `QAudioSink` and discards
queued audio. Audio is not drained after reception stops because bounded live
latency is preferred over playing obsolete receiver output.

## Devices and failures

The system default audio output is selected on initial audio discovery, but it
is opened only when reception starts. This does not change the separate rule
that SDR hardware is never selected or opened automatically. The user can
select another audio output from the always-visible control.

The required device format is 48 kHz, stereo, signed 16-bit PCM. No-device,
unsupported-format, open, write, and runtime failures leave reception and the
spectrum usable while audio status explains the limitation. If the selected
audio device disappears, playback stops, the selection is cleared, queued
samples are discarded, and no different output is silently substituted.

DSD-FME is launched directly with the configured executable and arguments
`-i - -o -`; no shell is involved. Process input, decoded output, and recent
stderr diagnostics are independently bounded. Each service pass writes pending
discriminator input before reading fixed-size stdout and stderr chunks, and
batches completed stderr lines into bounded Console updates. A slow decoder
drops stale discriminator input instead of blocking GNU Radio, the runtime
worker, or the wideband spectrum/waterfall path. A failed decoder mutes decoded
output and stays failed until the mode or configured path is changed, while
reception and wideband display processing continue.

DSD-FME is a separately installed executable required for the experimental
DMR/P25 mode, not a linked runtime dependency. Its configured path may be
outside APT, including under `/usr/local/bin`; slopSDR does not install or
update it.

## Test coverage

The automated GNU Radio regression injects deterministic 1 kHz AM, NFM, WFM,
USB, and LSB signals from a controlled mock device. It covers 1.0, 2.0, and
2.4 MS/s effective rates, multiple mode-appropriate filter widths, a 100 kHz
listening-frequency offset, and live mode changes. For every mode it verifies
the recovered 1 kHz audio frequency at 48 kHz, finite unclipped samples, and a
bounded audible RMS level. Additional weak-signal AM/SSB cases use 1% IQ
amplitude, deterministic receiver-noise cases verify that AM/SSB do not become
silent, and opposite-sideband cases verify USB/LSB rejection. These tests catch
incorrect filter, sideband, level-control, discriminator, translation, and
resampler behavior.

DSD-FME process tests cover bounded input and output, fragmented native-float
stereo output, 8 kHz-to-48 kHz resampling, channel preservation, and process
failure states. They do not establish successful over-air digital decoding.

Historical machine-specific observations and profiling snapshots are collected
in [validation observations](VALIDATION.md). They are not performance or
hardware guarantees.
