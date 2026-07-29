# slopSDR user guide

## Run the application

After a debug hardware build, start the receiver with:

```sh
./build/desktop-app-debug/slopsdr
```

The application starts stopped and shows **Searching for SDR devices…** while a
worker-thread discovery runs. It may select the first discovered SDR, but does
not open it or start reception automatically.

For deliberate development without hardware, use:

```sh
./build/desktop-app-debug/slopsdr --mock
```

Use `--help` to list command-line options and `--version` to print the
application version.

slopSDR stores scalar settings at
`${XDG_CONFIG_HOME:-$HOME/.config}/slopSDR/slopSDR.conf` and bookmarks at
`${XDG_CONFIG_HOME:-$HOME/.config}/slopSDR/bookmarks.json`.

For read-only GNU Radio, SoapySDR, kernel, and circular-buffer diagnostics, run
the hardware build with `--diagnose-gnuradio`. See `docs/TROUBLESHOOTING.md` for
interpreting `gr::vmcircbuf` messages and collecting a GDB backtrace.

## Refresh devices

Select **Refresh** in the receiver controls. Discovery runs on the receiver
worker thread and updates the device list when it completes. A refresh does not
open a device; it preserves the selected stopped device when present and selects
the first remaining device if the selection disappeared.

An empty result is shown as a safe no-device state. Discovery or driver errors
are reported in the status bar without closing the application.

## Select a device

Choose the intended SDR from the device list. That action changes the selected
identifier but does not open hardware. The
capability line reports available frequency and sample-rate ranges, gain
limits, PPM support, and verified RTL-SDR Blog V4 HF support where applicable.

Use **Clear** to release a stopped receiver and return to **No device
selected**. Stop reception before selecting another device or clearing the
selection.

## Start and stop reception

**Start reception** becomes available when a usable SDR is selected. Starting
revalidates and opens that selected device, then configures the confirmed center
frequency, sample rate, gain, and supported PPM correction before activating
the receive stream.
The application never starts reception automatically.

Select **Stop reception** before changing devices. Closing the window or using
the standard **Ctrl+Q** quit shortcut while receiving requests stop, waits for
GNU Radio, closes the device stream, and joins the receiver worker thread before
exit. Device, flowgraph, and disconnection failures are shown in the status bar;
controls continue to display confirmed backend state.

## Capture bandwidth

**Capture bandwidth** is the spectrum width received from the SDR at once, not
the demodulation filter width. The always-visible control selects an advertised
common rate or, when a device reports a rate range, accepts a custom numeric
value in MS/s. It shows both requested and effective rates; the spectrum and
waterfall axis span the effective rate around the center frequency.

The selected value is persisted in samples per second. A saved value that a
new SDR does not support is replaced with the conservative 2 MS/s default when
that device supports it, otherwise its lowest advertised rate, and reported in
the status line. Rejected values are never rounded or silently
substituted. Changing the setting while receiving automatically stops GNU
Radio and the SDR stream, applies and reads back the rate, rebuilds DSP, clears
stale audio/spectrum/waterfall data, and resumes with valid existing settings.
On failure reception remains stopped with the actual error. Higher rates can
increase USB and CPU load, so verify stability with the selected hardware.

## FFT resolution

**FFT resolution** selects 1,024, 2,048, 4,096, 8,192, 16,384, 32,768,
65,536, 131,072, or 262,144 bins for both spectrum and waterfall. New users
start at 4,096; the requested selection is validated and persisted. The
adjacent readout estimates `Hz/bin = effective sample rate / effective FFT
size`. Increasing the size makes narrow signals more distinguishable without
reducing the visible capture bandwidth, but costs CPU. Waterfall history uses
its separate bounded storage resolution.

The Hann window, coherent-gain correction, and dBFS mapping are identical at
every size, so a signal should not jump in level when resolution changes.
Changing resolution during reception replaces only the spectrum branch: audio
and the SDR stream continue, pending old-size display frames are discarded,
and new frames are not shown until their size matches the effective plan. If
allocation or backend limits prevent the requested plan, the interface retains
the request and shows the smaller effective size. If processing cannot keep
up, the status line warns about dropped rows.

**Waterfall aggregation** defaults to **Original**. Original is the existing
grainy peak-preserving view: it retains narrow peaks and brief signals without
smoothing, denoising, gating, or blur. Choose **Average** for a calmer view that
averages contributing frequency bins and FFT rows in linear power, then applies
the same dB range and Slop Spectrum palette. Switching either way re-renders
the history
already on screen without clearing it. This is a display-only setting and does
not affect the spectrum, FFT normalization, audio, squelch, or demodulation.

**Visible history** selects 5, 10, 15, 30, 60, or a custom number of seconds;
the default is 10 seconds. It is the only waterfall speed control and persists.
That duration maps to the full physical waterfall height, so apparent scrolling
speed is panel height divided by visible seconds. Changing it remaps only the
waterfall timeline; the live spectrum, Max-hold accumulation, FFT cadence, and
horizontal FFT resolution continue unchanged.

Drag the subtle horizontal handle between **Spectrum** and **Waterfall** to
resize the two panels. Drag upward for more waterfall space or downward for
more spectrum space. The split ratio is saved and restored across starts and
continues to scale correctly when the window is maximized, restored, or resized.
Both panels retain minimum usable heights. Dragging changes only presentation
geometry; reception, waterfall history, and DSP resources continue unchanged.

If the selected FFT window spans more sample time than the internal source-row
interval, FFT frames run at the lower non-overlapping rate. This avoids
presenting overlapping windows as independent time rows; `--verbose` reports
the internal and effective rates.

Historical rows keep their own timestamp, acquisition frequency, capture span,
and source FFT size, so a resolution or aggregation change does not clear them.
Before a row enters history, its width is reduced independently of the live
FFT while retaining both the Original peak and the Average linear-power sum.
Storage keeps at least twice the physical waterfall width and at least 2,048
bins when the source contains that much detail, normally using no more than
8,192 or 16,384 bins. Resize affects only later rows; stored rows are not
repeatedly resampled.

New rows also keep a separately bounded viewport-resolution copy made directly
from the original full FFT. This makes narrow carriers sharp immediately while
zoomed. A zoom, pan, center-axis, resize, DPI, or capture-geometry change
rejects those viewport-specific copies rather than stretching or padding them.
The complete waterfall is then rebuilt from compact full-span history while
the previous complete texture remains visible, and the replacement appears in
one atomic swap. After zooming out, the full width is therefore represented
immediately by softer compact history; newly arriving sharp wide-view rows
gradually replace it from the top without black side gaps.

The compact 16 MiB budget is calculated from stored peak and linear-power
statistics, row rate, and requested seconds. One-to-one retained bins use only
their two-byte peak value; horizontally reduced bins also use a four-byte power
sum. Horizontal compact resolution is reduced toward the display requirement
before duration is shortened. The recent viewport-resolution cache has its own
8 MiB budget and 512-row limit and evicts oldest rows first. If even the compact
minimum cannot retain the request, a warning below the history control shows
requested and actual capacity. Verbose diagnostics report both cache memory
uses and budgets together with duration, stored bins, rows, and budget-fit
status.

Use the compact **WF dB** range slider beside the Waterfall title to set its
minimum and maximum Slop Spectrum color levels. The lower and upper handles use
integer dBFS
steps from -140 to 0 with a 5 dB minimum separation; existing history is
recolored immediately and remains available. The setting persists across
restarts and does not change the spectrum scale, squelch, or demodulation.

The tuning overlay uses an amber listening-frequency line and paired
warm-yellow filter-edge gate markers. The filter edges are exact
one-physical-pixel lines when the passband is wide, sparse dashes at medium
on-screen widths, and short marker-adjacent stubs when the passband is narrower than
five physical pixels. This preserves narrow signals at full-span zoom while
the marker tips remain at the true filter edges. Ctrl-wheel width changes
briefly show low-opacity full-height lines and the selected width. The overlay
does not tint the passband or hide waterfall data.

## Bookmark manager

Select the checkable **Bookmarks**, **Scan**, **Settings**, or **Console** button
immediately to the right of Listening Frequency to open that left-side pane;
selecting the active button closes it. Only one pane is open at a time. Opening
one reduces the spectrum and waterfall width. Drag its right edge to resize it;
each pane keeps its own width, and the active pane is restored at startup.

The **Scan** pane runs a **Current passband** scan without retuning the SDR's
hardware center frequency. Its default lower and upper bounds are the current
usable captured passband, not the displayed zoom range. Set the bounds, step,
dwell time, and resume delay, then select **Start**. The scanner uses the
receiver's active mode, filter, and live squelch setting; it holds when
squelch opens and resumes after the configured delay once it closes. **Pause**
stops scanner movement while reception continues, **Skip** advances once, and
**Stop** leaves the receiver on its current frequency. A center-frequency,
capture-bandwidth, or device-limit change that places either bound outside the
usable passband stops the scanner. Scan settings and scanner state are not
saved, and bookmark or hardware-retuning scans are not available. Scanning does
not disable or reset the Receiver Control settings, and the spectrum and
waterfall continue at their normal live display cadence.

**Add Group** creates a nested group under the selected group, or at the root
when no group is selected. **Add Bookmark** snapshots the current listening
frequency, requested gain, demodulator, filter edges, squelch state, and
supported mode-specific settings, then opens a name prompt with the automatic
name selected. Edit or keep that suggestion and press Enter to add; Cancel or
Escape adds nothing. Drag bookmark rows to reorder them, drop on a group to
move them inside it, or use the unfiled drop area to return them to the root.
Begin dragging from the stacked-line grip at the row's right edge; it appears
when the row is hovered or selected. The row preview follows the pointer,
dragging near an edge scrolls the list, hovering over a collapsed group expands
it after a short pause, and Escape cancels the move.
**Edit** renames groups or changes every bookmark receiver field without
changing its UUID. **Remove** deletes the selected item and confirms before
deleting a group with descendants.

**Update Bookmark** overwrites the selected bookmark with the current
frequency, requested gain, demodulator, filter, and squelch settings while
preserving its name, group, and other saved metadata. After tuning a bookmark,
the action keeps that bookmark's stable identity even if its frequency is
shared with another bookmark. Add and update outcomes are shown in the
application status line.

Single-click an item to select it. Double-click a bookmark or select it and
press **Tune** to restore its frequency, requested gain, mode, oriented filter,
and saved manual/disabled squelch settings. Older bookmarks without saved
squelch fields keep the receiver's current squelch settings. Unavailable modes cannot be tuned. Tuning
does not start a stopped receiver, run Automatic Squelch, or clear waterfall
history; a running receiver uses its live update paths.

Groups expand independently and remember their state.
The checkboxes store scanner-inclusion metadata: bookmark values are stored directly,
while group checkboxes show unchecked, checked, or partial state and update all
descendant bookmarks. They are not used by the current-passband scanner.

Bookmarks use stable demodulator IDs. A bookmark for a mode unavailable in the
current build remains intact and is marked **Unavailable**; it is not changed to
AM or another available mode. Editing such a bookmark retains the unknown mode
and its mode-specific settings unless the user explicitly selects an available
mode.

## Settings

The **SDR calibration** section shows the correction currently applied to the
selected device, including its sign. **Auto PPM** is available only for a
stable RTL-SDR that exposes both its internal test mode and frequency
correction. It becomes **Cancel** while running and reports preparing, settling,
up to three measuring windows, applying, and the final completed, failed, or
cancelled state.

Calibration uses the RTL2832 internal incrementing test counter rather than a
broadcast station or an RF peak. Allow about 25 to 35 seconds. The first five
seconds settle, followed by two ten-second windows; a third is collected when
the first two do not round to the same integer PPM. Any counter gap, driver
overflow, unstable window spread, invalid timing, disconnection, or correction
outside the conservative automatic range rejects the result.

When reception is active, audio is muted, DSD-FME input and live display-frame
publication stop, and the last complete spectrum and waterfall remain visible.
Normal SDR streaming is restored before audio. A receiver that was stopped
stays stopped. Cancel or failure restores the previous correction and receiver
state. A successful driver-read-back correction is saved for that dongle's
stable identity/serial and is applied after every reopen before the first tune;
calibrating one dongle never changes another dongle's saved value.

The **DSD-FME binary** setting stores an optional path to an executable. Type a
path or use **Browse** to choose one, and use **Clear** to remove the explicit
path. The status reports whether the path is valid, missing, not a regular file,
or not executable; `~` is expanded for validation and launch. The path is
saved even when it is currently invalid. The application does not install,
discover, or update DSD-FME.

## Console

The read-only **Console** keeps a bounded recent history throughout the current
application session, including receiver and device failures, audio underruns,
bookmark save failures, and DSD-FME lifecycle and stderr messages. Decoder
stdout is decoded PCM and is never displayed. Use the severity selector to show
all entries, Info and above, warnings and above, or errors only. **Copy
Selected**, **Copy All**, and **Clear** affect only the displayed in-memory
history. Auto-scroll follows new entries only while the view remains at its
bottom; scrolling upward leaves the current position undisturbed.

## Receiver presets

Capture bandwidth and filter width use preset lists for their primary controls;
choose **Custom…** to enter an exact validated value. Filter width remembers a
separate last valid value for the five built-in analog modes (AM, NFM, WFM, USB,
and LSB) and for the separate DMR/P25 decoder mode. Gain defaults to 20 dB when
no saved value exists; if a device cannot use 20 dB, the nearest advertised
value is shown without replacing a user-saved gain. The Gain slider uses the
selected SDR's reported range and step, displays requested and effective values,
and applies a drag on release so it does not flood the SDR.

Changing center frequency with digits, complete entry, or the spectrum wheel
also recenters listening frequency while preserving display zoom. Clicking the
waterfall still changes only the listening frequency; the next center change
recenters it. Wheel movement over the waterfall instead zooms the shared
spectrum/waterfall viewport around the listening marker. The read-only toolbar
indicator shows the rounded whole percentage of the full effective capture
bandwidth relative to the current visible span, with a minimum of 100%.
Use the compact **Pause spectrum** toggle in the Spectrum header and the
**Pause waterfall** toggle in the Waterfall header to freeze either display
independently. Pausing drops frames for that display while reception, DSP,
tuning, audio, and recording continue; resuming accepts the next live frame
without replaying paused data.

Hold **Ctrl** while scrolling either spectrum or waterfall to widen or narrow
the active demodulation filter within its mode limits. Non-preset widths appear
as Custom. Hold **Shift** while scrolling either panel to move listening
frequency higher or lower without retuning the hardware center. At full span
the viewport stays fixed; when zoomed, both displays recenter on the new
listening frequency without changing zoom. Ctrl takes priority if both
modifiers are held.

Center-frequency changes preserve waterfall history. Existing carrier traces
move horizontally to the same absolute frequency on the shared visible axis;
parts of old rows outside their original capture span use the minimum
waterfall color. Repeated retunes always rebuild from the stored original FFT
rows, so they do not progressively blur. The live spectrum waits for a frame
from the confirmed tuning generation and then places its peak directly above
the corresponding new waterfall trace. Capture-bandwidth and FFT-size changes
preserve compatible metadata-backed mixed-resolution history and clamp the
visible zoom only when required.

## Audio output

The application initially selects the desktop's default audio output, but it
does not open it until reception starts. Use the always-visible **Audio device**
control to choose another output. **Volume** applies software gain from 0 to
100 percent, and **Mute** outputs silence without changing the saved volume.

Receiver output is 48 kHz stereo. AM, NFM, WFM, USB, and LSB use mode-specific
mono filtering and correction and are duplicated into both output channels;
WFM output is mono and does not decode stereo broadcasts or RDS. A mode change
discards queued audio from the previous mode.

**DMR/P25 support is experimental.** Choose **DMR/P25** to start the configured,
separately installed DSD-FME executable while reception is running. This
conventional single-frequency decoder mode uses a 12.5 kHz default filter and
sends flat, unsquelched discriminator audio to DSD-FME for its default DMR and
P25 detection. Decoded 8 kHz interleaved stereo native IEEE-754 float32 output
is resampled to 48 kHz stereo while preserving its two timeslot channels.
Retuning keeps the decoder process running but discards stale decoded audio.
The status below the normal audio status reports not configured, starting,
running, stopped, failure, or bounded input/output overflow. Decoding
reliability may vary, and FEC errors or audio underruns may occur. Encrypted
traffic is not decoded. Decoder failure does not stop the receiver, spectrum,
or waterfall. Configure the executable path in **Settings**; slopSDR does not
install or update DSD-FME.

The audio status line reports the active format or a no-device, unsupported
format, open, disappearance, underrun, overflow, or write failure. Reception
and spectrum display remain available when audio cannot be opened. Overflow
drops the oldest queued audio to retain bounded live latency. Underrun inserts
silence rather than replaying old samples.
