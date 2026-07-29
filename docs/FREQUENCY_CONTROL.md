# Digit-based Frequency Control

The center-frequency control displays ten decimal digits. From left to right,
their place values are 1 GHz, 100 MHz, 10 MHz, 1 MHz, 100 kHz, 10 kHz, 1 kHz,
100 Hz, 10 Hz, and 1 Hz. The listening frequency remains a separate amber
display and is never selected implicitly by the digit control.

## Interaction

Wheel up, keyboard Up, or a touchscreen tap in the upper half of a digit adds
that digit's place value. Wheel down, keyboard Down, or a tap in the lower half
subtracts it. Arithmetic operates on the complete frequency, so carry and
borrow naturally cross adjacent digits.

Each digit participates in keyboard focus traversal. Left and Right move focus
to adjacent digits. Outside a sequential edit, Enter or keypad Enter opens a
complete-frequency field; parsing and validation still occur in C++.

Left-clicking a digit starts a temporary sequential edit at that position. The
digits to its left remain unchanged. Each numeric key replaces the active digit
and advances to the next digit, without tuning the receiver. The control marks
the active digit and remaining editable suffix. After the final digit, Enter
validates and applies the complete integer-Hz value; Escape restores the exact
original displayed frequency. Clicking any digit in the editable suffix moves
the active position without discarding pending replacements, so an earlier
digit can be corrected before continuing. Unsupported completed values remain
pending for correction and never alter receiver tuning. Enter, Escape, wheel,
touch, and other cancellation paths clear the digit focus and edit highlight.

When the pointer directly hovers a digit, pressing `0` through `9` instead
replaces only that digit and immediately applies the exact resulting frequency.
The same hover routing applies to Up and Down for increment/decrement. It is
disabled while a text editor has keyboard focus, during sequential editing, and
while the scanner owns tuning. Up and Down do nothing during a sequential edit.

Right-click or Delete sets the selected digit and every less-significant digit
to zero by removing the corresponding decimal suffix. A right-click does not
focus, select, or start editing that digit. More-significant digits are
unchanged. If that exact zeroed value is not allowed, the operation is rejected
and the existing frequency is retained; clamping it would violate the specified
zeroed suffix.

## Validation

The control first derives the receiver's valid center-frequency range, including
the half-passband margin required to keep the complete displayed range visible.
When selected-device capability ranges are supplied, each is intersected with
the receiver range. No SoapySDR or QML type enters this calculation.

Increment, decrement, and complete-frequency requests outside the resulting
ranges use the nearest allowed boundary and report the adjustment. If ranges
are disjoint, the nearest boundary is selected; an equal-distance tie selects
the lower frequency. An empty intersection rejects tuning with a clear status.

When a physical device is explicitly selected, the receiver worker publishes
its standard-C++ capability ranges to the application model. Digit validation
then uses those ranges without exposing a SoapySDR type or applying an
optimistic frequency change. The deliberate `--mock` path retains the
provisional receiver limits for development and automated tests.

For a confirmed RTL-SDR Blog V4 with the opened device's verified low RF range,
the receiver permits the 500 kHz practical RF edge even when a full capture
passband would extend below it. Other devices retain complete-passband center
validation.
