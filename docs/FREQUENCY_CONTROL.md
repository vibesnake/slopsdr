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
to adjacent digits. Enter or keypad Enter opens a complete-frequency field;
parsing and validation still occur in C++.

Right-click or Delete sets the selected digit and every less-significant digit
to zero by removing the corresponding decimal suffix. More-significant digits
are unchanged. If that exact zeroed value is not allowed, the operation is
rejected and the existing frequency is retained; clamping it would violate the
specified zeroed suffix.

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
