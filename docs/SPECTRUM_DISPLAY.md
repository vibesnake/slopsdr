<!-- SPDX-License-Identifier: AGPL-3.0-only -->

# Spectrum and Waterfall Data Path

Spectrum rendering consumes display-ready magnitude frames. Raw IQ samples do
not cross the backend boundary and are never exposed to QML.

## Backend frame production

The GNU Radio flowgraph generates only the FFT windows needed for display. The
user may select 1,024, 2,048, 4,096, 8,192, 16,384, 32,768, 65,536, 131,072,
or 262,144 bins; 4,096 is used when no saved preference exists. The same
effective size controls both spectrum and waterfall while every frame covers
the complete effective capture span. The validated request remains persisted;
if a plan cannot be allocated, the next usable lower size becomes effective
and both values are reported.

Window length equals the effective FFT size. The nominal hop is `effective
sample rate / internal source cadence`; integer hops preserve the division's
fractional remainder so window start times do not drift. The shared FFT source
targets 60 frames per second and does not change with Waterfall Visible
History. The hop is never shorter than the FFT window: when an interval would
overlap, effective FFT cadence is capped at `sample rate / FFT size`. The
generator discards unneeded samples when the requested hop is longer. It
therefore executes no more than one non-overlapping FFT per requested source
frame rather than one FFT per input block. Startup window fill or a source that
cannot deliver samples at its effective rate can lower the observed rate;
diagnostics report the achievable and actual rates honestly.

Each selected vector uses a periodic Hann window. `FftFrameProcessor` divides
FFT magnitude by the FFT size and the window's measured coherent gain before
converting to dBFS. The normal display range is -120 through 0 dBFS, normalized
into the inclusive 0-to-1 renderer range. Changing among supported FFT sizes
therefore changes frequency resolution without materially shifting signal
level.

Every selected frame carries acquisition-time center frequency, effective
sample rate, explicit capture span, FFT size, monotonic sequence, monotonic
acquisition timestamp, and tuning generation. The hardware
source stamps this metadata when samples enter the flowgraph and propagates it
with the selected FFT vector. A vector that crosses a tuning-generation
boundary is not published. Buffered rows are therefore never interpreted from
the receiver's current center frequency alone.

The backend output queue holds at most 64 selected frames. It preserves
selection order and drops the oldest frame only when that bound is exceeded. The
GNU Radio window generator declares its variable relative rate and forecast;
scheduler batch size does not alter the sample-time selection average.

The runtime drains each backend burst once. The live spectrum receives its
newest compatible frame immediately. The waterfall presents real FFT rows at
60 Hz when available, retaining FIFO row order during normal operation so
short bursts remain distinct. If the FFT window permits fewer than 60
independent frames per second, the timer follows that lower achievable rate.
If timer scheduling falls fractionally behind a normal source cadence, it
briefly drains one additional real FIFO row to prevent a small steady backlog
from turning into a periodic bulk drop.
After a bounded post-stall backlog it collapses to the newest compatible row,
counting superseded rows as coalesced, rather than replaying stale work. It
never fabricates duplicate rows. Timestamp rendering separates sample-time FFT
selection from wall-clock waterfall scrolling.

The pending waterfall buffer remains bounded at 64 frames, while the
cross-thread spectrum and waterfall handoffs each retain at most one latest
frame behind at most one queued GUI dispatch. A slow GUI therefore coalesces
display work instead of accumulating latency. Stop, restart, effective-rate
changes, and FFT-size reconfiguration clear pending presentation state.
Hardware tuning-generation, center-frequency mapping, or capture-span changes
also discard pending older-generation waterfall work. Visible History changes
only waterfall time mapping and retained-history render state; they do not
reset delivery or reconfigure the shared FFT producer. The application model
accepts waterfall rows only from the confirmed tuning generation.

The requested center remains a viewport/control preview until the worker
applies a coalesced hardware retune. Each frame continues to carry the actual
applied center under which its samples were captured; it is never relabelled
with the preview. While another retune is pending, a newer overlapping frame
from an intermediate applied center replaces the front frame and is projected
by absolute RF frequency. Every requested-center or viewport change also
reprojects the retained frame immediately. Uncovered frequencies use the empty
black level, but no empty intermediate spectrum is published. Once the final
applied-center frame arrives, the same monotonic replacement path installs it
normally.

Automatic PPM calibration is the narrow exception that pauses live frame
publication without clearing the front spectrum or retained waterfall. Pending
backend and presentation frames are discarded before the test stream starts,
and RTL test-counter bytes use a separate device path. Cancellation or failure
resumes over the retained display. After a successfully changed correction,
the frequency-aligned spectrum, waterfall histories, and Max-hold envelope are
cleared before frames from the restored valid stream are accepted.

When a SoapySDR device advertises a `bufflen` stream argument, the adapter asks
for a capability-derived buffer near 30 ms and observes advertised options,
ranges, steps, or alignment text. Newest-frame waterfall selection remains the
primary burst protection; no throttle is inserted after a hardware source.

Runtime metrics count received IQ samples and windows, executed and published
FFTs, backend or presentation-buffer overflow drops, coalesced and stale-
generation rows, presentation underruns,
spectrum frames displayed, and waterfall rows consumed. With `--verbose`, per-second deltas
also report requested/effective FFT bins, effective sample rate, approximate
Hz/bin, hop size, internal/effective/measured FFT-frame rates and waterfall
row rates, produced/displayed row intervals, sequence gaps, duplicate or
non-monotonic rows, dropped rows, source-frame age, processing time, and pending
buffer depth. The
renderer separately reports rendered frames, merged updates,
waterfall-history memory use, stored bins, requested duration, capacity
duration, actual retained duration, vertical raster phase and dimensions,
raster-rebuild count and duration, scroll-phase resets, render-frame interval,
and whether the request fits. Queue overflow produces a visible
performance warning. Only an allocation or backend limit can select a reported
effective FFT-size fallback below the persisted request.

Changing FFT size while receiving locks and reconnects only the GNU Radio
spectrum branch. The backend attempts to prepare all nine supported Hann FFT
plans, windows, and magnitude buffers before reception starts; the available
resources are reused by runtime FFT-size changes. Reconfiguration installs a
fresh window/hop generator and sink, clears pending frames, then
unlocks the graph. The audio branch and device stream
remain running.

The mock backend produces equivalent normalized synthetic display frames so the
renderer and interactions can be exercised without SDR hardware. It does not
expose synthetic IQ to the application model.

The receiver runtime polls backend batches every 33 ms on its worker thread.
The spectrum catches up to the newest frame, while the waterfall retains real
rows at its independent 60 Hz presentation cadence when available. Neither path
blocks the producer.

Spectrum delivery is independent of the 5 ms audio-service timer. Display load
therefore cannot reduce audio-service cadence.

Audio-output device enumeration is deferred while reception is running. The
periodic idle refresh resumes after Stop, so a slow platform sound-server or
driver query cannot periodically stall the receiver worker and collapse queued
waterfall rows.

## Rendering

`SpectrumWaterfallView` is a custom Qt Quick scene-graph item. Spectrum mode
uses a line-strip geometry node; waterfall mode uses a Qt scene-graph texture
built from bounded history. It has no GNU Radio or SoapySDR dependency and does
not compute FFTs. Qt Charts is not used.

Before rendering, each FFT is mapped to the item's device-pixel column count.
The source-bin mapper uses the signed nominal lower frequency
`center - capture span / 2`, so a passband extending below 0 Hz keeps every
original FFT bin at its actual nominal RF coordinate. The visible unsigned RF
viewport is the intersection of that nominal coverage with the selected
device's advertised RF range. Bins outside the intersection are clipped, not
clamped or compressed across the remaining width.
Reduction takes the strongest bin in each covered source range so a narrow
carrier is not discarded. Waterfall rows are separately reprojected from their
retained 16-bit intensity bins: near native resolution uses direct bins and
deeper zoom holds the nearest bin, so no rendered or colorized image is ever
enlarged. Newly received rows also take a second bounded path directly from the
original full FFT to the current physical viewport: Original stores the peak
for every output column, while Average stores its mean linear power. Horizontal
projection keeps nearest-bin/flat-hold detail and the texture uses nearest
filtering. The
completed waterfall is a pixel-native raster with one texture row per physical
output row. Its source and destination rectangles remain 1:1 in physical
pixels, so the scene graph never stretches it vertically or applies a second
translation.
Each refresh maps retained acquisition timestamps directly from the persistent
monotonic render timeline into the raster; installing a new row or texture
therefore does not reset scroll phase. Sparse history uses only adjacent-row
display interpolation, while dense intervals use the selected temporal
aggregation. Stored values are never rewritten. The raster geometry is aligned
to final scene physical pixels on high-DPI screens. The
waterfall uses the project-authored deterministic 256-color **Slop Spectrum**
palette. Independently selected color control points are smoothly interpolated
in linear-light RGB, progressing from black and deep blue through cyan, green,
yellow, orange, red, and warm white. Values are converted to dBFS and mapped
linearly through the configured waterfall minimum and maximum; values outside
that range clamp to the first or final palette color. The compact **WF dB**
header control sets the persisted
integer minimum and maximum independently, with a 5 dB minimum separation.
Changing it recolors the existing normalized history on a rendered frame
without clearing rows or changing receiver processing.

Waterfall history has two bounded paths. The compact frequency-aligned path
performs one horizontal reduction per incoming row and retains the existing
16-bit peak value for Original plus, when multiple FFT bins contribute to a
retained bin, a floating-point linear-power sum for Average. The
contributing-bin count is derived from the original FFT size and retained
width, so no redundant count array or full 128K/256K frame is stored.
One-to-one retained bins need no extra sum because their power is recoverable
from the stored value. Its planner accounts for peak and Average statistics,
source FFT size, physical history width, source cadence, requested duration,
and the 16 MiB compact-history budget. It first reduces retained horizontal
resolution, including below physical width when necessary, so supported
visible durations remain honest.

Before that compact reduction discards detail, the current-viewport path
projects the original full FFT directly to the physical waterfall width.
Original uses peak reduction so a narrow carrier survives; Average computes
each column in linear power. It stores at most 512 recent rows under its
separate named 8 MiB viewport-history budget and evicts the oldest rows first.
It never retains complete source FFT vectors. The live spectrum and current
FFT retain their full horizontal resolution independently of both histories.

The persisted aggregation selector defaults to Original. Original reads only
the retained peak statistic and keeps the prior peak-preserving horizontal and
temporal rendering exactly, including short peaks and grain. Average divides
linear-power sums by their derived bin counts, then time-weights contributing
FFT rows in linear power according to their monotonic timestamps. This keeps
smoothing stable under irregular FFT arrival intervals instead of making it
frame-count dependent. Only after all horizontal and temporal aggregation is
complete is the result converted to dB and passed through the existing
waterfall dB range and Slop Spectrum palette. Neither mode averages dB values,
palette
indices, or colorized pixels. Mode changes invalidate only projected-row and
texture caches, so the same retained history is immediately re-rendered and
switching back restores Original.

The independent visible-duration selector uses the same timestamp mapping for
1, 2.5, 5, 10, 15, 30, and 60 seconds. The exact fractional duration is saved
as a floating-point settings value and changes only raster time mapping; it
does not restart reception or alter FFT delivery.

The spectrum and waterfall are vertically split by a presentation-only
draggable handle. Its persisted value is a validated spectrum-to-display
height ratio rather than a pixel coordinate, so the same proportion is restored
after maximizing, restoring, or repeated window resizing. The splitter changes
only QML item geometry; reception, display queues, retained rows, DSP
resources, and the shared full-width frequency axis remain unchanged. Each
renderer coalesces rapid geometry changes to a display-frame boundary and the
waterfall swaps a fully initialized pixel-native raster atomically, preserving
seam-free output without black bars or flicker.

Spectrum mode renders an opaque black backdrop, then fills every column below
the trace with a vertical gradient whose colors come from the shared Slop
Spectrum palette. The gradient uses the spectrum's displayed dBFS minimum
and maximum, independently of the waterfall WF dB range. The original thin
spectrum trace is a separate scene-graph layer above the fill, so it remains
sharp while the area above it stays black.

The optional persisted **Max** control exposes a per-bin spectrum hold envelope.
Accepted live FFT captures update the envelope in dBFS without
allocating new buffers at a compatible FFT geometry; retained or reprojected
display frames never update them. Center, gain, effective sample-rate, FFT-size,
device, and receiver-lifecycle changes discard the envelope immediately.
Zoom, listening/filter changes, palette and display-range changes, and
visibility toggles retain them. Enabled holds render last as one-device-pixel
white lines with a subtle dark contrast stroke and no fill.

The independent persisted **AVG** slider affects only the live spectrum trace.
Position 0 bypasses averaging and clears residual state. Positions 1 through
100 use an exponential 8 ms through 1.8 s time-constant mapping, which provides
fine useful adjustment near the low end and strong smoothing at the maximum.
For each compatible frame, the display processor calculates
`alpha = 1 - exp(-elapsed / time constant)` from acquisition timestamps. It
updates one bounded EMA accumulator matching the FFT bin count directly from
the normalized display values. These values are linear on the displayed dBFS
axis, giving symmetric-looking rise and fall without a physical-power
conversion. The first enabled frame initializes the accumulator directly,
avoiding a fade from zero. Compatible frames reuse the established buffers and
no historical frame queue is introduced.

Hardware-center or tuning-generation changes, sample-rate or FFT-bin-count
changes, receiver restart, and device change reset the spectrum average.
Listening-frequency, filter, demodulation-mode, zoom, and pan changes do not.
While Spectrum is paused, current coalesced frames may continue updating the
accumulator, but the displayed trace and Max envelope stay frozen; Resume uses
the next live frame without replay. MAX processes each raw accepted capture
before AVG transforms the live trace, preserving its established peak envelope
independently. The raw frame also continues to drive the separate noise-floor
estimate. Waterfall delivery, aggregation, history, and pause state never pass
through spectrum averaging.

Each compact row retains its original timestamp, center, capture span, FFT
size, sequence, and tuning generation. A viewport-resolution row additionally
stores its viewport generation, exact visible lower and upper frequencies,
physical width, device-pixel ratio, and capture geometry. Direct rendering
requires every field to match exactly and requires the source FFT to have
covered the complete viewport when the row was created. A zoom, pan,
center-axis, viewport-width, device-pixel-ratio, or capture-geometry change
advances the viewport generation and immediately rejects every mismatch.
Rows are never stretched, cropped, padded, or partially combined with empty
columns.

For each historical raster row, every temporally contributing source row must
have matching viewport-resolution data or that complete raster row uses
compact history projected across the full viewport. This produces only a
moving boundary between newer sharp rows and older softer rows. Existing
compact rows are never resampled when a resize changes the storage plan, so
mixed stored widths and source FFT sizes coexist. On a visible-axis change or
FFT-size change, the item marks one pending reprojection and rebuilds the image
once in the next rendered frame. Resize and device-pixel-ratio changes are
coalesced to a display-frame boundary. Every viewport transition builds a
fully initialized replacement image off-screen, keeps the previous complete
scene-graph texture visible during that work, and swaps the completed texture
atomically without clearing retained rows. Viewport zoom requests use one
16 ms coalescing window. Runtime center
and listening-frequency tuning share action-aware coalescing, while discrete
filter-width requests use their own bounded coalescing. Rapid wheel
events therefore do not starve the independent
waterfall timer, repeated display operations do not accumulate resampling blur,
and rows captured at different centers align by absolute frequency. Pixels
outside a row's acquisition span use the configured minimum waterfall palette
color. Newly delivered rows and historical rows use the selected aggregation,
the same C++ lookup table, and the same dBFS mapping; selecting aggregation does
not change row ordering or presentation cadence.

A paused waterfall normally preserves its image. While current-passband,
wide-range, or bookmark scanning owns tuning, the renderer instead clears both
bounded history stores and the visible raster to black. Frames remain dropped
while paused, scanner Stop cannot revive the cleared rows, and Resume begins
with the next current-frequency row rather than replaying missed data.

The configured visible duration maps to the complete physical-pixel waterfall
height using one authoritative monotonic render timestamp rather than row
arrival time or an assumed fixed cadence. Every output row represents a
deterministic timestamp interval. Multiple source rows in one interval use peak
reduction in Original and time-weighted linear-power averaging in Average; an
interval below source cadence blends only its adjacent timestamped rows.
Temporal reduction and interpolation stop at a tuning-generation or capture-
mapping boundary, so old and new RF mappings never form one displayed row. A
16 ms refresh timer
advances that single timestamp mapping and updates the pixel-native raster; no
node translation or second timestamp reprojection moves the same data again.
The replacement texture contains exactly the physical viewport row count and
every row is initialized before the atomic swap. Its complete source rectangle
maps directly to the item, avoiding a second overscan offset or clipped texture
edge. The render timestamp cannot lead the newest row by more than one observed
source-row interval. Pixels newer than that row use a zero-order hold, while
pixels older than all collected startup history hold the oldest row without
interpolating into nonexistent data. Uncovered-frequency pixels still use the
configured minimum palette entry.

Compact history is bounded by the longest Visible History requested during the
current receiver session (plus a small staging interval), calculated row
capacity, and the configurable 16 MiB compact budget. The independent recent
viewport history is bounded by 512 rows and its 8 MiB budget. Reducing Visible
History changes
only the displayed time span and does not discard older stored rows; stopping,
disconnecting, or an incompatible capture reset starts a new retention session.
Supported 1, 2.5, 5, 10, 15, 30, and 60 second settings retain the complete requested
duration by combining the bounded internal cadence with reduced horizontal
history storage while the live spectrum keeps all selected FFT bins. Changing
Visible History rebases the newest-row render anchor, preserves fractional
scroll phase, and invalidates only the waterfall raster and time mapping;
spectrum geometry, its retained front frame, and the Max envelope remain live
and intact.

The spectrum draws a fixed dBFS amplitude scale over its left side without
reserving plot width. Its labels use subtle translucent backing and do not
handle pointer input, so the trace continues underneath and the overlay cannot
change tuning behavior. The normal spectrum view maps normalized frame values to
-120 through -20 dBFS, with labelled 20 dB ticks and unlabelled 10 dB ticks.
Values outside that range clip at the corresponding edge. The scale follows
the renderer's spectrum minimum/maximum dBFS properties when they are
configured; it is independent of the waterfall **WF dB** color range.

For an active receiver with a sufficiently complete FFT frame, the renderer
also estimates the wideband noise floor from the 20th percentile, which limits
the influence of narrow strong signals. It smooths successive estimates and
draws a subtle dashed dBFS reference line. This is display-only information and
does not affect squelch or gain control.

The tuning overlay has a device-pixel-aligned amber listening-frequency line
and exact one-device-pixel warm-yellow lower/upper filter edges. Each filter
edge has compact downward and upward yellow triangle markers at the top and
bottom plotting boundaries; their black one-pixel outlines remain legible over
spectrum and waterfall colors. The edge treatment adapts to the physical
on-screen gate width: at 12 pixels or wider it uses subtle full-height lines,
from 5 through 11 pixels it uses sparse lower-opacity dashes, and below 5
pixels it draws only short stubs beside the markers. This leaves the center of
a very narrow passband readable without moving its exact edge coordinates.
During Ctrl-wheel width adjustment, low-opacity full-height lines and the
exact width briefly reappear. The custom scene-graph renderer draws the gate
over both views without adding an input item or recreating waterfall textures.
It does not tint the passband, apply a contrast mode, or alter waterfall
reprojection, cadence, FFT processing, or pointer input. Filter edges derive
directly from listening frequency and filter width, including narrow filters
where the markers may overlap.

## Frequency mapping and tuning

Horizontal mapping is inclusive and linear. Pixel zero maps to the visible
lower frequency, the display midpoint maps to the visible center, and a pixel
equal to the display width maps to the visible upper frequency. Invalid widths,
out-of-display pixels, and frequencies outside the visible span are rejected.

One C++ frequency viewport owns the valid unsigned visible range and supplies a
`FrequencyAxisMapper` for frequency labels, listening and filter-edge markers,
gate markers, and pointer-position conversion. Spectrum, retained spectrum,
Original and Average waterfall, and the Max hold independently maps its
source bins from the signed nominal FFT range into that same viewport. Spectrum
and waterfall both use the complete item width; the dBFS overlay never changes
the plot rectangle. Logical size and device-pixel ratio determine projection
resolution, while frequency coordinates remain stable through resize and
high-DPI screen changes.

A waterfall click maps its horizontal position in C++ and changes only the
listening frequency. A waterfall wheel action changes only the visible span,
using exponential steps and preserving the listening frequency's old normalized
position. Capture-edge clamping moves that anchor only as far as required.
Fractional wheel deltas accumulate before a viewport update. The toolbar's
read-only zoom indicator reports the rounded whole percentage of the effective
capture span relative to the current visible span, with a minimum of 100%. The
event is consumed by the waterfall and cannot scroll a surrounding view.
The **Pause spectrum** control in the Spectrum header and **Pause waterfall**
control in the Waterfall header stop rendering new frames for only their
selected display. They do not stop receiver DSP, tuning, audio, recording, or
frame production; frames arriving while paused are dropped, and resuming
renders the next live frame without replaying them.

Normal wheel movement over the spectrum retains hardware-center tuning. Its
default step is 10 kHz and is configurable in the waterfall header. Upward
wheel movement increments and downward movement decrements. High-resolution
wheel and touchpad deltas accumulate until a complete step is available. The
application model previews the newest validated center and listening frequency
immediately and reprojects the retained spectrum from its captured RF
coordinates. A trailing absolute-frequency request replaces intermediate
relative shifts, and the receiver worker collapses queued requests to the
newest value before touching hardware. Newer compatible spectrum frames from
the currently applied or intermediate tuner centers remain eligible while a
later request is pending; waterfall rows retain their acquisition metadata and
history.

Control-wheel over either panel adjusts the validated receiver filter width
without changing center/listening frequency or the viewport. The accumulated
discrete increments are 500 Hz for AM/NFM, 10 kHz for WFM, and 100 Hz for
USB/LSB. Widths clamp to the existing mode/sample-rate limits; non-preset
results display as Custom, and USB/LSB filter geometry remains respectively
above/below listening frequency. The GNU Radio backend keeps a stable
mode-specific channel rate and changes the running channel/sideband/audio FIR
taps in place, so filter resizing does not rebuild the source, demodulator,
audio, FFT, or waterfall paths.

Shift-wheel over either panel changes listening frequency by the configured
spectrum tuning step: up tunes higher and down tunes lower. At full capture
span the viewport remains fixed. When zoomed, the current visible span is
centered on the new listening frequency and clamped at capture edges. Hardware
center, filter width, FFT data, zoom factor, and waterfall history remain
unchanged. Control has priority over Shift, Alt is neutral, and all handled
panel wheel events are consumed.

Every successful center-frequency change recenters listening frequency.
It also preserves the display zoom factor and recenters the viewport around the
new hardware/listening frequency. Waterfall clicks remain listening-only until
the next center change.

Ordinary center changes from the spectrum wheel, digit control, complete entry, or
application commands preserve and reproject waterfall history. Capture
bandwidth and FFT-size changes also preserve compatible stored rows, recalculate
viewport limits, and reproject each row from its own width and original
acquisition metadata.
