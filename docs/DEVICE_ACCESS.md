# Device Access

The device-access API in `devices/DeviceAccess.hpp` uses only standard C++ and
radio-domain types. SoapySDR types and construction arguments remain private to
`SoapyDeviceProvider.cpp`; they are not exposed to QML, the application model,
or public radio-domain interfaces.

## Discovery and selection

Discovery returns human-readable descriptors and does not create a retained
device session or tune hardware. Startup discovery may select the first usable
descriptor when no valid stopped selection exists, but that metadata selection
does not open hardware. A persistent device handle is created only when Start
opens the currently selected identifier.
Individual SoapySDR modules may briefly probe hardware while producing their
enumeration metadata; that driver behavior is not treated as selection.

When SoapySDR supplies both driver and serial values, the identifier is
`driver:serial=serial` and is marked stable. Devices without both values receive
a discovery-local identifier marked unstable. Duplicate identifiers are also
marked unstable rather than silently referring to the wrong device.

Selecting a device queries its receive channels and frequency ranges. Discovery,
opening, capability queries, tuning, and cleanup convert C++ driver exceptions
into device-level results. The desktop composition invokes the provider only on
its dedicated receiver thread and returns standard application snapshots to the
GUI thread.

An opened session also reports receive sample-rate ranges, adjustable gain
minimum, maximum, and step, complex-float streaming support, and PPM support. `DeviceController`
validates those capabilities before forwarding configuration. Its streaming
interface uses only standard C++ complex samples, spans, durations, and result
types; no SoapySDR or GNU Radio type crosses the device boundary.

## Explicit hardware receive path

The GNU Radio backend has separate synthetic and hardware constructors. The
synthetic constructor remains the default. The hardware constructor accepts a
`DeviceController` only after Start has verified and opened the selected
identifier. It rejects a controller with no selection and never chooses a
descriptor or starts reception by itself.

After opening a device, the runtime applies its saved PPM correction before the
first normal center-frequency tune. On reception start, the hardware backend
applies the validated requested sample rate, reads back and retains the
driver-confirmed effective sample rate (rounded to the nearest hertz), confirms
the PPM correction again, and only then applies center frequency and gain
before activating a single-channel `CF32` receive stream. A private GNU Radio
source block performs bounded, 50 ms reads from that standard-C++ stream
interface. Transient timeouts are retried inside the source without returning a
zero-item source result that can park the scheduler; scheduler interruption and
block stop terminate that bounded retry promptly. Driver stream errors and
disconnection end the source, are retained as an actionable runtime error, and
stop confirmed receiver state when the owner collects that error.

Sample-rate capabilities are device-advertised ranges. A range whose endpoints
match is a discrete supported rate; a wider range permits a custom requested
value. The application only lists common values that intersect those
capabilities and does not put RTL-SDR-specific limits in the radio domain.
Sample-rate changes stop and join the active graph, stop the receive stream,
reconfigure the device, and construct a replacement graph using the confirmed
effective rate. Unsupported frequency, sample
rate, gain, PPM, stream-format, and stream-start requests leave the confirmed
radio-domain value unchanged. Flowgraph and device exceptions are contained and
converted to receiver results.

## PPM correction

An opened SoapySDR session queries `hasFrequencyCorrection()` for receive
channel zero. The device capability reports that result without exposing a
SoapySDR type. For a capable, explicitly selected device, PPM changes use
`setFrequencyCorrection()` and are recorded by the controller only after the
driver confirms success. Unsupported correction, missing selection, driver
errors, and exceptions return explicit errors and leave the confirmed value
unchanged. The radio domain separately validates the provisional -200 through
200 PPM range before a hardware adapter applies it.
The adapter reads `getFrequencyCorrection()` after every successful write so
the application displays and persists the driver's effective value, including
the RTL-SDR backend's integer-PPM resolution.

## Automatic RTL-SDR PPM calibration

An opened RTL-SDR is calibration-capable only when its Soapy settings expose
`testmode`, its receive formats expose complex signed 8-bit samples, and
frequency correction is supported. RTL-SDR driver-key matching accepts both
the lowercase discovery key and the canonical uppercase key reported by an
opened SoapyRTLSDR session. Calibration writes the existing device's `testmode`
setting; it never launches `rtl_test` and never estimates clock error from
received RF peaks.

The normal CF32 stream is first deactivated without closing the device. A fresh
CS8 stream is then created and activated, which resets the driver stream/buffer
before measurement. The driver correction is temporarily set to zero because
librtlsdr applies frequency correction to the sample clock as well as tuning;
this ensures a repeat calibration measures the dongle rather than only the
residual after its saved correction. Its raw bytes retain the RTL2832
incrementing test counter. The calibration stream uses 16 KiB transfers and a
two-buffer Soapy ring to keep callback completion close to production time
without sacrificing overflow headroom. Live audio, device-refresh, spectrum,
and waterfall service timers are suspended until calibration restoration.
The first settling buffer permits the device's possible one-time startup
counter reset. Every later byte is checked modulo 256, including the rest of
the five-second settling interval and all measurement windows, and Soapy
stream-overflow status is reported as data loss. Test bytes have a separate
read API and cannot enter the GNU Radio source.

Measurement ignores the first five seconds, then collects up to three
ten-second monotonic-time windows. Each window uses complex-sample count and
the requested sample rate. A least-squares slope over cumulative samples at
each monotonic buffer timestamp suppresses USB callback and worker-scheduling
jitter without using driver sample timestamps:

```text
measuredPpm = 1e6 * (measuredSampleRate / requestedSampleRate - 1)
```

The full three-window spread may not exceed the named 20 PPM host-timing
tolerance. This permits bounded USB completion-time jitter while the median
rejects one high or low window; counter discontinuities and driver-reported
drops are always fatal.

Two loss-free windows finish early only when they round to the same integer
PPM. Otherwise the median of three is used. Automatic results are
conservatively limited to -100 through +100 PPM even though manual domain
validation remains -200 through +200. Counter discontinuity, stream loss,
invalid time, instability, disconnection, or an out-of-range value rejects the
complete result.

Test mode and its stream are always disabled before correction or normal-stream
restoration. Session cleanup makes a best effort to disable test mode after an
exception or shutdown.

## RTL-SDR Blog V4 HF handling

A device is identified as an RTL-SDR Blog V4 only when all of these values are
available and agree:

* SoapySDR driver key: `rtlsdr`
* USB manufacturer: `RTLSDRBlog`
* USB product: `Blog V4`
* Hardware or tuner identity containing `R828D`

An R828D tuner by itself is not sufficient. A V4 identity discovered without
opening the device is only a candidate; its HF capability is verified after
explicit selection.

After opening a device, the adapter queries its named SoapySDR `RF` tuning
element. It treats the unnamed overall range and the optional `CORR` component
range as diagnostics only; neither substitutes for the RF range. A confirmed
V4 whose RF range reaches below 500 kHz enables the driver-managed HF path.
The application then uses 500 kHz as its practical minimum and retains the RF
range's reported upper limit. If that RF capability is absent, V4 HF tuning is
rejected with a clear limitation instead of sending an invented setting or USB
command.

For a confirmed and capable V4, crossing below 27 MHz selects the domain-level
`DriverManagedRtlSdrBlogV4` tuning mode. The Soapy adapter still calls only the
normal receive-channel `setFrequency()` API. The supported RTL-SDR driver owns
the V4's 28.8 MHz upconverter and RF-input switching. Tuning back to 27 MHz or
above calls the same API in normal mode, allowing the driver to restore normal
tuning. Direct-sampling settings are not forced because they are not the V4
upconverter mechanism.

Non-V4 devices always receive ordinary tuning requests below 27 MHz and may
accept or reject them according to their own advertised range and driver.

The real GNU Radio path uses this same `DeviceController` tuning operation for
initial configuration and every center-frequency change. It enters
driver-managed V4 HF mode for a confirmed capable V4 below 27 MHz and uses
normal tuning again at or above that transition. The adapter passes each
logical RF frequency directly to SoapySDR: it adds no 28.8 MHz offset and never
enables direct sampling. A fresh successful open replaces prior capability
state and warnings. Under `--verbose`, the adapter logs the overall, RF, CORR,
and selected practical RF ranges plus its V4 detection decision.

## Hardware test isolation

Default tests use synthetic signals and mock device sessions. The optional
`ENABLE_HARDWARE_TESTING=ON` build adds a separately labelled
`hardware_receiver_smoke_test`. It skips unless `SDR_TEST_DEVICE_ID` names the
exact discovered identifier to open; it never selects the first result. This
test must be run only when the named device is available and safe to use.

## Current boundaries

The layer streams a GUI-selected device into the GNU Radio backend. Startup may
select the first discovered device, but the selected device opens only after an
explicit Start action and reception never starts automatically.

SoapySDR can contain third-party modules running in the application process.
C++ exceptions are contained, but a process-fatal defect inside a native module
cannot be recovered by a C++ boundary. Startup discovery therefore runs once on
the receiver worker and reports failures without opening a device or starting
reception.
