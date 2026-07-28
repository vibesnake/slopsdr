# Radio Domain API

The radio-domain API is defined by `radio/ReceiverBackend.hpp`. It uses only
standard C++ types and does not expose Qt, QML, GNU Radio, or SoapySDR types.
`MockReceiverBackend` provides deterministic state transitions without physical
hardware.

`ReceiverStateModel` is the shared Qt-free validation and transition engine
used by receiver backends. Keeping it separate ensures that the mock and DSP
implementations preserve the same center/listening, passband, squelch, and mode
behavior.

## Initial state and limits

The mock starts stopped at a 100 MHz center and listening frequency, with a
2 MHz sample rate, 12.5 kHz filter, 0 dB gain, 0 PPM correction, AM mode, and
a -80 dB manual squelch threshold. Its provisional validation limits are:

* Frequency: 0 through 9,999,999,999 Hz
* Sample rate: 200 kHz through 10 MHz
* Gain: -10 through 100 dB
* PPM correction: -200 through 200
* Squelch: -160 through 0 dB
* PPM support: advertised per backend; the mock supports it, while the
  synthetic GNU Radio backend reports it unsupported because no device is open

Filter width is constrained by both the selected mode and current sample rate:

| Mode | Minimum | Maximum | Incompatible-mode default |
| --- | ---: | ---: | ---: |
| AM | 3 kHz | 15 kHz | 10 kHz |
| NFM | 5 kHz | 25 kHz | 12.5 kHz |
| WFM | 100 kHz | 250 kHz | 180 kHz |
| USB | 1.8 kHz | 4 kHz | 2.4 kHz |
| LSB | 1.8 kHz | 4 kHz | 2.4 kHz |

The effective maximum is the smaller of the mode maximum and sample rate.
Invalid direct requests are rejected. Selecting a mode whose range does not
contain the current width changes the filter to its documented default and
reports the adjustment; it is not a silent clamp.

## State-transition behavior

Start and stop are idempotent. Repeated operations succeed without changing
state.

The available passband is derived from the center frequency and sample rate.
Direct listening-frequency requests outside that passband are rejected without
changing state. Selecting a normalized spectrum position from 0 through 1 maps
that position into the current passband; positions outside that range are
rejected.

Every successful center-frequency change sets listening frequency to the same
confirmed center atomically. This includes direct requests and requested
center shifts; a limit-clamped shift reports `adjusted`. Waterfall clicks remain
listening-only until the next center-frequency change recenters the channel.

A capture-bandwidth (RX sample-rate) change updates the available passband. It preserves listening
frequency when possible and otherwise moves it to the nearest edge with an
`adjusted` result. The operation is rejected without state changes if the new
rate is outside the mock limits, cannot provide a complete passband at the
current center, or is narrower than the current filter width.

The requested capture bandwidth is stored in samples per second. Device
adapters validate it against their advertised discrete rates or ranges; radio
domain limits remain provisional for mock and synthetic backends. Hardware uses
the driver-confirmed effective rate for passband, translation, FFT, filtering,
demodulator gains, and audio resampling.

Gain capabilities include minimum, maximum, and driver-reported step. The UI
shows requested and driver-confirmed effective gain, applies slider changes on
release, and retains the last valid persisted requested value. Center and
listening frequencies, demodulation mode, manual squelch threshold, and the
disabled-squelch state are likewise restored before the GUI initializes and
applied when reception starts. Invalid or outdated saved values safely fall
back to the defaults. When no saved gain exists it requests 20 dB; an
unsupported 20 dB uses the nearest supported value without overwriting a saved
preference.

Invalid gain, PPM, filter, and squelch requests are rejected without state
changes. PPM requests are also rejected when a backend does not advertise
frequency-correction support. Setting a manual squelch level selects manual
squelch and saves that threshold. Automatic and disabled squelch are explicit,
mutually exclusive states; returning from either one to manual restores the
saved manual threshold.

### Automatic PPM calibration

Automatic PPM calibration is a bounded radio/device operation exposed only by
a hardware backend that confirms RTL-SDR test mode and frequency-correction
support. The application runtime drives one bounded raw-counter read per worker
event, so the GUI thread is never blocked and cancellation is processed between
reads. The calibration estimator is independent of Qt, GNU Radio, and real
time; tests provide a fake backend and controllable monotonic clock.

If reception was running, the backend stops and joins the normal flowgraph and
deactivates its stream without changing the confirmed receiver state or
closing the device. Audio and external-decoder service stop, display-frame
publication pauses, and the GUI retains its last complete spectrum and
waterfall. After test mode is disabled and a correction is read back, a fresh
normal flowgraph and valid stream are started before audio is restored. If
reception was stopped, the selected device is opened only for calibration and
is closed afterward. Failure, cancellation, and shutdown restore the previous
correction and state where the device remains available.

Successful effective corrections are stored by stable physical device identity
or serial. A saved correction is applied immediately after open and before the
first normal center tune. No global correction is shared by dongles, and an
unsuccessful calibration never overwrites the previous saved value.

### Automatic squelch

The automatic-squelch estimator is deterministic. A backend submits a bounded
set of recent channel-power measurements in dB. Non-finite samples are removed,
the remaining samples are sorted, and the sample at
`floor((count - 1) * 0.20)` is the noise-floor estimate. The opening threshold
is that estimate plus 6 dB, constrained to the receiver's -160 through 0 dB
squelch range. New estimates update the active automatic threshold but never
overwrite the saved manual value.

The mock supplies a fixed deterministic measurement set, producing a -95 dB
threshold. The GNU Radio backend currently uses its configured conservative
-100 dB noise-floor seed, producing a -94 dB threshold; it does not claim
continuous live hardware noise estimation. No estimation is done in QML.
Disabled squelch applies the backend's fully-open -160 dB threshold and the UI
labels this state `Disabled (open)`.

The GNU Radio receiver's five built-in analog demodulation modes are AM, NFM,
WFM, USB, and LSB. The user-selectable **DMR/P25** mode is separate: it supplies
bounded discriminator audio to the configured DSD-FME process service without
changing the wideband display branch.

`DemodulatorRegistry` maps those runtime modes to durable `am`, `nfm`, `wfm`,
`usb`, `lsb`, and `digital-auto` IDs. Receiver preferences and bookmarks
persist only those IDs, never enum ordinals, labels, executable paths, or
control indexes. Legacy numeric receiver
preferences are migrated once to the corresponding stable ID. Bookmark filter
low/high edges are signed Hz offsets from listening frequency, preserving
asymmetric USB and LSB passbands. Unknown bookmark demodulator IDs and their
versioned settings remain stored but are not resolved to a runtime mode.

## GNU Radio backend

When configured with `ENABLE_GNURADIO=ON`, `GnuRadioReceiverBackend` implements
the same domain interface. Its public header uses only standard and radio-domain
types; all GNU Radio types are private to the DSP implementation.

The receive flowgraph is:

```text
synthetic source + throttle OR explicitly selected device source
  |-> translating mode-specific FIR channel filter + safe decimation
  |     -> power squelch
  |     -> active-mode selector -> selected level control/demodulator
  |          -> 48 kHz resampler -> selector
  |          `-> bounded analog receiver-audio queue
  |     -> DMR/P25 selector (bypasses squelch)
  |          -> bounded discriminator -> DC blocker -> 48 kHz resampler
  |          -> bounded DSD-FME process-input queue
  `-> configured sample window/hop generator -> selected Hann FFT -> calibrated magnitude
       -> bounded display-frame sink
```

The translating filter uses `listening frequency - center frequency` as its
channel offset. Every center-frequency operation resets that offset to zero;
sample-rate changes retain the documented passband-edge behavior.

Construction builds but does not start the flowgraph. Synthetic flowgraph start
is nonblocking. Hardware configuration and stream activation complete before
flowgraph start and must be invoked away from the GUI thread. Stop requests
flowgraph shutdown, waits for its worker threads, and closes the device stream
before marking the receiver stopped; destruction performs the same cleanup if
needed. Repeated start and stop operations are idempotent. A capture-bandwidth
change stops and joins a running graph, stops the SDR stream, configures and
reads back the device rate, clears stale display/audio data, rebuilds
rate-dependent blocks, and restarts. If any step fails, reception remains
stopped and no alternative rate is substituted.

For hardware reception, the source is a narrow adapter over the standard-C++
device-session stream API. Device configuration completes before the flowgraph
is marked running. The same translating filter, squelch, demodulation selector,
bounded spectrum sink, and output routing are used for synthetic and device IQ.
Only the selected demodulation branch receives channel samples, so inactive
branches cannot fill their scheduler buffers and backpressure the source or
spectrum path. Transient hardware read timeouts are retried until samples,
stop, or a terminal stream result. The hardware source never receives raw IQ
through QML or the application model.

The channel decimator derives an integer-exact intermediate rate from the
effective hardware rate, selected mode, filter width, and FIR transition room.
Mode and filter changes replace the running channel flowgraph after a clean
stop, flush old audio, and restart it; frequency-only changes continue to
update the translating filter in place. The wideband FFT branch is never
channel-decimated.

AM uses bounded complex AGC, magnitude demodulation, and DC removal. NFM and WFM
use quadrature discriminators with 5 kHz and 75 kHz nominal deviations followed
by 300 and 75 microsecond deemphasis respectively. USB and LSB decimate a
symmetric narrow channel, apply bounded AGC, then use distinct positive- and
negative-sideband FIR responses at the reduced channel rate before
real-component extraction and 48 kHz audio band-pass filtering. Each
analog path is rationally resampled to 48 kHz mono before entering the bounded
audio queue. DMR/P25 uses a dedicated bounded quadrature discriminator, DC
removal, and 48 kHz rational resampler without analog squelch or deemphasis.
Only the selected demodulator input consumes channel samples in each rebuilt
graph.

The spectrum sink corrects magnitude for FFT size and Hann coherent gain,
normalizes calibrated dBFS data, and publishes bounded, display-ready frames at
an internal cadence independent of the selected waterfall visible-history
duration. The persisted FFT selection is 1,024 through 262,144 power-of-two
bins with a 4,096 default, without changing the full visible span. Requested
and backend-effective sizes remain distinct if plan allocation requires
fallback. Runtime changes reconnect only this branch and leave the audio path
and source active. Long windows cap FFT and waterfall cadence to
non-overlapping frames. Raw IQ remains inside the flowgraph. Audio uses a
bounded 48 kHz transport consumed
by the platform Qt Multimedia service; `docs/AUDIO.md` defines its processing
and buffering policy. The receiver worker owns the DSD-FME process service and
moves bounded discriminator and decoded-audio chunks without allowing process
I/O to block GNU Radio or wideband display delivery. The desktop composition
owns the selected GNU Radio backend on a dedicated receiver thread; `--mock`
selects the mock backend deliberately.
