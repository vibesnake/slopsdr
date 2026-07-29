# GUI Requirements

The interaction and layout rules in this document are nonnegotiable. Changes
to them require explicit approval.

## Frequency terminology

The receiver has two separate frequency concepts:

* **Center frequency** is the hardware tuning frequency and the center of the
  nominal FFT capture passband. At a device RF limit, the visible intersection
  may place it away from the display midpoint.
* **Listening frequency** is the frequency currently selected for
  demodulation.

Both values must be visible and visually distinct. The center marker uses a
cyan treatment and remains at the horizontal midpoint. The listening marker
uses an amber treatment and shows its position within the current passband.

## Waterfall tuning

The waterfall header keeps its **Waterfall** title and places a compact
two-handle **WF dB** range control beside it. Its lower and upper handles set
the independent waterfall color minimum and maximum in integer dBFS steps; the
control remains in the waterfall header rather than the receiver panel, is
keyboard and touch operable, and must not crowd or crop at supported window
sizes.

The spectrum and waterfall follow distinct interaction rules:

* Mouse-wheel movement over the waterfall changes a shared spectrum/waterfall
  display zoom without changing receiver state. Zoom is exponential, is
  anchored to the listening frequency rather than the pointer, and preserves
  that frequency's horizontal position unless capture-edge clamping prevents
  it. Fractional wheel input is accumulated and rapid events are coalesced.
* Mouse-wheel movement over the spectrum changes the hardware center frequency.
  Scrolling upward increases it and scrolling downward decreases it. The
  default step is 10 kHz per wheel action. High-resolution deltas accumulate,
  rapid requests preview the newest validated center immediately, and hardware
  tuning settles asynchronously on the final coalesced absolute frequency.
  During that settling interval, the retained spectrum moves with the previewed
  viewport and newer overlapping frames from applied intermediate centers
  continue updating at their captured RF coordinates.
* Control-wheel over either panel changes the existing mode-specific
  demodulation filter width. Up widens and down narrows; AM/NFM use 500 Hz
  increments, WFM uses 10 kHz, and USB/LSB use 100 Hz. The operation clamps
  without repeated updates, preserves receiver and viewport frequencies,
  updates the running DSP filters in place without restarting reception, and
  retains the USB upper-sideband and LSB lower-sideband orientation.
* Shift-wheel over either panel changes only listening frequency by the
  configured spectrum tuning step. Up tunes higher and down tunes lower while
  hardware center and filter width remain unchanged. At full-span zoom the
  viewport does not move. When zoomed, the shared viewport keeps its span and
  recenters on the new listening frequency, clamped inside capture coverage.
* Control takes priority when Control and Shift are both held. Shift applies
  only without Control, Alt does not change the selected action, and a modifier
  action never falls back to normal wheel behavior.
* Every confirmed center-frequency change preserves the current display zoom
  factor, recenters the shared viewport around the new center, and recenters the
  listening frequency according to the receiver rule.
* A primary click or touchscreen tap selects the listening frequency represented
  by that horizontal point in the zoomed waterfall.
* The Spectrum and Waterfall header bars each provide an independent compact
  pause/resume toggle immediately before the large display title. Each toggle
  freezes only its renderer, drops frames received while paused, and resumes
  with the next live frame.
* Spectrum and waterfall pointer handlers consume wheel input. They must not
  cause ordinary scroll-view movement or vertical page scrolling.
* Frequency-to-pixel conversion and tuning calculations belong in C++. QML may
  forward pointer coordinates, dimensions, and wheel deltas to the application
  model and render normalized, display-ready positions returned by C++.

The shared C++ viewport keeps hardware center/capture span separate from visible
center/span and supplies one absolute-frequency mapping to the spectrum,
waterfall, labels, filter gate markers, click tuning, and retained
history reprojection. A read-only zoom indicator reports the rounded whole
percentage of the full effective capture span relative to the current visible
span, with a minimum of 100%.
At a device RF limit, that effective span is the intersection of nominal FFT
coverage and the device-advertised RF range; clipped FFT bins retain their
original RF coordinates and are not redistributed across the visible width.
The minimum span retains at least 32 FFT bins and, when practical, the complete
active filter.

### Passband-edge behavior

Every successful center-frequency change, including spectrum wheel movement,
digit editing, complete entry, and programmatic requests, sets the listening
frequency to that same confirmed center atomically. A waterfall click remains
listening-only; the next center-frequency change recenters it.

The shell currently validates listening/display frequencies from 0 through
9,999,999,999 Hz. To keep the complete 2 MHz passband visible, it normally
validates the center frequency from 1,000,000 through 9,998,999,999 Hz.
The verified RTL-SDR Blog V4 low-RF path is the narrow exception: its confirmed
500 kHz practical RF edge may be selected even though the nominal lower FFT
edge is negative. The visible and selectable display begins at the advertised
500 kHz device minimum while retaining the original FFT-bin spacing.
Device-specific limits further restrict these provisional UI limits when a
real backend is connected.

## Decimal-digit frequency entry

The center-frequency entry must provide a decimal digit control:

* Every displayed decimal digit is independently focusable and controllable.
* Scrolling over a digit changes the center frequency by that digit's place
  value.
* Scrolling upward increments and scrolling downward decrements.
* Right-clicking a digit sets that digit and every less-significant digit to
  zero.
* Carry and borrow operate across adjacent digits.
* Values are validated and clamped to the available center-frequency limits.
* Keyboard focus is visible. Up and Down perform the same increment/decrement
  operation, Left and Right move between digits, Enter opens complete-frequency
  entry, and Delete performs the same zeroing operation as right-click.
* Touching the upper or lower half of a digit increments or decrements it without
  changing mouse-wheel or right-click behavior.
* Each digit exposes an accessible name, role, and operation description.

Tuning arithmetic, carry, borrow, zeroing, and limit enforcement must be
implemented in C++, not QML JavaScript.

Receiver center-frequency limits are intersected with selected-device
capability ranges when those ranges are available. Increment, decrement, and
complete entry clamp to the nearest available boundary and report that result.
If an exact right-click/Delete zeroing result would be outside those limits, it
is rejected with a clear response so more-significant digits remain unchanged
and the requested suffix is never silently made nonzero.

## Always-visible receiver controls

During normal receiver operation, without opening a settings dialog, the GUI
must keep all of these visible:

* Start reception
* Stop reception
* Filter width
* Demodulation mode
* Squelch level
* Automatic squelch
* Disable or remove squelch
* PPM correction
* Current center frequency
* Current listening frequency
* Device state
* Reception state
* Audio output device
* Audio volume
* Audio mute
* Capture bandwidth, including requested and effective SDR sample rates
* Spectrum and waterfall FFT resolution
* Waterfall aggregation
* Visible waterfall history

**Capture bandwidth** is the complete spectrum width received from the SDR at
once. It is controlled by the RX sample rate and is separate from the
demodulation filter width. The always-visible control lists common values
reported by the selected SDR and accepts a custom numeric value only when that
device reports a range. It displays requested and driver-confirmed effective
rates in MS/s and is disabled during discovery and receiver transitions.

Changing capture bandwidth while receiving performs a controlled restart. The
user does not need to Stop and Start manually; conflicting controls remain
disabled until the result is confirmed. A failed change leaves reception
stopped and shows the driver or DSP error. A stopped receiver remains stopped.

The primary capture list contains 0.250, 1.000, 1.200, 2.000, 2.250, and
2.400 MS/s only when reported by the selected SDR, plus **Custom…** for ranged
devices. Filter-width controls similarly use mode-specific presets plus an
explicit **Custom…** dialog; each mode remembers its last valid width. The
receiver panel must reflow controls rather than crop them at desktop widths.

The FFT-resolution control selects 1,024, 2,048, 4,096, 8,192, 16,384,
32,768, 65,536, 131,072, or 262,144 bins for both spectrum and waterfall. It
shows approximate Hz/bin, preserves the complete effective capture span, and
remains changeable while receiving. A runtime change reconfigures the spectrum
branch without restarting audio or the source. The selected request remains
persisted; if allocation or backend limits require a smaller plan, the control
shows both requested and effective sizes instead of misrepresenting either.

The visible-history control offers 5, 10, 15, 30, and 60 seconds plus a custom
value; 10 seconds is the default. It is the only user-facing waterfall speed
control and persists. Its duration fills the physical waterfall height, so
apparent speed is physical height divided by visible seconds. Changing it is a
waterfall-only time-mapping operation and must not reconfigure the shared FFT
producer or interrupt live spectrum frames and Max-hold accumulation. FFT-row
cadence is internal and independent of the selected duration. Every rebuild
must initialize exactly one output row per physical viewport pixel. Between
FFT arrivals, pixels newer than the latest row hold that row; during startup,
pixels older than collected history hold the oldest row until real history
replaces them. Visible History changes must not discard older retained rows,
and session retention must cover the longest requested duration within the
memory budget.
History storage must reduce retained horizontal resolution before shortening
retained time and must never report a duration it cannot retain. When one FFT
window is longer than the internal row interval, generated FFT frames use the
lower non-overlapping cadence rather than presenting overlapping windows as
independent time rows.

The persisted waterfall aggregation control offers **Original** and
**Average**, with Original as the default. Original uses the existing
peak-preserving horizontal and temporal reduction without smoothing, gating,
denoising, or blurring. Average combines contributing FFT bins and FFT frames
in linear power and converts to dB only after aggregation, before applying the
existing waterfall range and Slop Spectrum palette. Both modes retain the
compact full-span history plus a separately bounded recent history projected directly from each
original FFT to the current physical viewport. Changing the selection
re-renders those retained statistics without clearing history or changing
timestamps, frequency metadata, zoom, viewport, spectrum, audio, or receiver
processing. Zoomed enlargement remains nearest-bin/flat-hold.

Viewport-resolution rows must exactly match the current viewport generation,
visible lower and upper frequencies, physical width, device-pixel ratio, and
capture geometry. Zoom, pan, center-axis, width, device-pixel-ratio, and
capture-geometry changes invalidate mismatched rows immediately. Each output
row then uses either complete matching viewport-resolution data or compact
history projected across the complete current width; partial combinations,
stretching, cropping, padding, black side fill, and repeated edge pixels are
not permitted. A viewport change builds a fully initialized replacement
raster off-screen while the previous complete texture remains displayed, then
swaps the replacement atomically.

The user-selectable demodulation modes are AM, NFM, WFM, USB, LSB, and DMR/P25.
If the interface uses the generic label SSB, it must provide a clear way to
select USB or LSB.

Controls display confirmed receiver and device state. Availability follows the
current selection and runtime state: Start requires a valid discovered device,
hardware-specific controls require the reported capability, and DMR/P25 reports
its configured decoder status.

Audio controls display confirmed state from the platform audio service. QML
must not enumerate or open sound devices. Audio-device selection, volume, and
mute remain visible during normal reception; audio failures must not disable
receiver tuning or spectrum display.

## Device selection and runtime confirmation

Hardware discovery runs once at startup and remains available through the
refresh control. It shows a clear searching state while it runs, then selects
the first usable listed device when no valid stopped selection exists. Discovery
and selection only retain device metadata; hardware opening and backend creation
require Start. Reception must never start automatically. Clearing selection
returns to a safe no-device state.

Discovery, opening, receiver lifecycle, and hardware operations run on the
receiver worker thread. Start remains disabled until a valid discovered
selection is confirmed. User controls display confirmed snapshots returned by that worker, and
runtime failures are presented in the application status rather than applied
optimistically. Refresh preserves a stopped selection while it remains listed,
does not switch an active receiver, and replaces a disappeared stopped
selection with the first remaining device.

## Current boundaries

The GUI offers conventional single-frequency **DMR/P25** decoding through the
configured DSD-FME executable and reports its bounded runtime state near normal
audio status. DSD-FME process management remains outside QML. Bookmark
scanner-inclusion checkboxes are persisted metadata and are not used by the
current-passband scanner.

## Layout and responsibility boundaries

The normal receiver workspace must avoid page scrolling and keep the required
controls visible at reasonable desktop resolutions. Touch targets should
remain usable on touch-capable systems, while mouse wheel, right-click,
keyboard, and accessibility operation remain available on desktop.

Checkable **Bookmarks**, **Scan**, **Settings**, and **Console** buttons immediately to
the right of Listening Frequency select one persisted, horizontally resizable
panel on the left of the radio display. The four panels are mutually exclusive
and any may be closed. Each retains its own saved width; opening one reduces
display width rather than overlaying the spectrum or waterfall. The Settings
panel contains the optional DSD-FME executable-path field, native file chooser,
clear action, and local file-validation message. It also contains an **SDR
calibration** section showing the effective signed PPM correction, one **Auto
PPM** button that becomes **Cancel**, a progress indicator, and the preparing,
settling, measuring 1/3 through 3/3, applying, completed, failed, and cancelled
states. Auto PPM is disabled unless the selected physical device exposes both
RTL-SDR test mode and frequency correction. Decoder runtime status remains near
the normal audio status rather than in the Settings panel. The Console is a
bounded, read-only session log with severity filtering, copy and clear actions,
and conditional auto-scrolling; it never exposes command entry or DSD-FME's
binary stdout. The Bookmarks panel renders
nested groups with independent expansion state and scanner-inclusion checkboxes.
Group inclusion is derived as a tri-state value and updates all descendant
bookmarks; that metadata is not used by the current-passband scanner.

The Scan panel runs a current-passband scanner owned by the application model.
It validates lower and upper bounds against the usable captured passband, steps
only through the existing in-passband listening-frequency path, and never
retunes the SDR center frequency. It wraps at the upper bound, holds on live
squelch activity, resumes after the configured delay, and stops safely if a
center-frequency, sample-rate, or device-limit change invalidates its range.
Each step changes only the active channel translation on the receiver worker;
it does not mark the complete runtime busy, flush audio or decoder output,
persist receiver settings, or publish a full receiver snapshot. Rapid requests
are bounded to one operation in flight plus the newest pending frequency, and
the focused confirmation updates listening-frequency presentation without
renotifying unchanged Receiver Control properties. Per-step current-frequency
updates use a dedicated notification; scanner state and status are not
republished when they have not changed.
Its lower and upper bounds, step size, dwell time, and resume delay are
persisted as integer-Hz scanner configuration. Running, paused, holding, and
current-frequency state are transient and always reset to stopped on startup.
Saved bounds remain visible and invalid rather than being silently replaced if
the current usable passband no longer contains them. Bookmark scans and
hardware-retuning scans are not provided.

The panel provides Add Group, Add Bookmark, Update Bookmark, Edit, Remove, and
Tune actions. Update Bookmark writes the current receiver-owned fields to the
selected bookmark, or to the last bookmark loaded by stable UUID, while
preserving its name, group, and other metadata. Its enabled state never relies
on frequency matching.
Actions follow selection validity, non-empty group removal requires
confirmation, and a single click selects without tuning. Add Bookmark captures
the current receiver fields when its modal name prompt opens, selects the
suggested name for immediate replacement, and creates nothing on cancellation
or an empty name. Bookmark rows can be reordered or moved by UUID into unfiled,
top-level, and nested groups; insertion markers distinguish before/after drops
from highlighted group drops, while drag-hover expands collapsed groups. A
bookmark-only edge grip appears on hover, selection, or active drag, has a
larger pointer target than its visible stacked-line mark, and is the only place
that starts dragging. The dragged row follows the pointer, edge proximity
auto-scrolls the list, and Escape cancels without changing the model.
Double-click and Tune apply available bookmarks through asynchronous receiver
controls without
starting a stopped receiver, invoking one-shot Automatic Squelch, or clearing
waterfall history. Unknown demodulator bookmarks remain editable but cannot be
tuned. Bookmark files may omit the squelch fields for legacy data; such
bookmarks remain loadable and leave the current receiver squelch unchanged.

The spectrum and waterfall occupy a shared full-width display column separated
by a subtle, hover-highlighted horizontal splitter. Dragging upward gives the
waterfall more height; dragging downward gives the spectrum more height. The
split is stored as a validated ratio, not an absolute pixel height, so it
survives maximizing, restoring, and repeated window resizing. Both panels keep
sensible minimum heights. Splitter movement is presentation-only: it must not
stop reception, clear retained waterfall history, recreate DSP resources, or
introduce black bars, seams, or flicker in the pixel-native waterfall.

QML owns responsive layout, visual formatting, and forwarding input events.
C++ owns tuning calculations, validation, receiver state, and application
logic. Radio, device, DSP, decoder process management, and bookmark persistence
remain outside QML and behind their appropriate interfaces.
