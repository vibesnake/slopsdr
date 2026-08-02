# Building slopSDR

slopSDR targets Debian-family desktop Linux and is developed and hardware-tested
on Devuan. The Debian and Ubuntu package names below are installation references;
they do not claim testing on every Debian-family distribution.

Build and linked runtime dependencies are supplied through the distribution
package manager, normally APT on these systems. slopSDR does not download,
build, vendor, install, or update them.

## APT packages

Install the compiler, build system, Qt development files, QML runtime modules,
GNU Radio, and SoapySDR development files. `qt6-base-dev` supplies Qt Widgets,
which slopSDR uses for the shared non-native `QFileDialog` behind recording,
recordings-folder, and DSD-FME Browse actions:

```sh
sudo apt update
sudo apt install build-essential cmake ninja-build \
    qt6-base-dev qt6-declarative-dev qt6-multimedia-dev \
    qml6-module-qtqml-workerscript qml6-module-qtquick \
    qml6-module-qtquick-controls qml6-module-qtquick-dialogs \
    qml6-module-qtquick-layouts qml6-module-qtquick-templates \
    qml6-module-qtquick-window \
    gnuradio-dev libsoapysdr-dev soapysdr-tools
```

Install the APT-provided Soapy module and driver for the intended device. For
RTL-SDR hardware, Debian uses `soapysdr0.8-module-rtlsdr` and `rtl-sdr` on
currently supported releases. Ubuntu may provide the Soapy module through the
`soapysdr-module-rtlsdr` virtual package instead:

```sh
sudo apt install soapysdr0.8-module-rtlsdr rtl-sdr
```

## Optional DSD-FME decoder

DSD-FME is not required for analog reception. It is a separately installed
external executable and may be installed outside APT, including under
`/usr/local/bin`. Configure its executable path in the application's Settings
panel before selecting **DMR/P25**. slopSDR does not install or update DSD-FME.

## Debug hardware application

The `desktop-app-debug` preset requires both GNU Radio and SoapySDR and fails
configuration if their development packages are unavailable.

```sh
cmake --preset desktop-app-debug
cmake --build --preset app
./build/desktop-app-debug/slopsdr
```

The `app` build preset builds only the real `slopsdr` executable target.
To build the debug tests as well:

```sh
cmake --build build/desktop-app-debug
ctest --preset desktop-app-debug
```

## Release hardware application

```sh
cmake --preset desktop-app-release
cmake --build build/desktop-app-release -j2
./build/desktop-app-release/slopsdr --verbose
```

`desktop-app-release` is the normal user build. It uses the release hardware
configuration, builds slopSDR and its required application targets, and does
not compile the test suite.

`--verbose` prints opened-device overall, RF, CORR, and selected practical RF
capability ranges with the RTL-SDR Blog V4 HF decision, plus once-per-second
audio scheduling, spectrum, and active-mode DSP stage RMS/peak metrics while
receiving.
Use `--spectrum-fft-size 4096` to set the initial size when no preference has
been saved. Supported values are 1024, 2048, 4096, 8192, 16384, 32768, 65536,
131072, and 262144; the normal default is 4096. The in-application dropdown
persists later changes. `--verbose` reports requested and effective sizes,
effective sample rate, Hz/bin, hop, internal/effective FFT-frame and waterfall
row rates, dropped rows, processing time, and bounded
waterfall-history stored bins, requested/capacity/retained durations, memory,
and budget-fit status.

## Mock development build

The dependency-disabled presets are for tests and mock development; their
executables are not hardware applications. Run one deliberately with `--mock`:

```sh
cmake --preset desktop-debug
cmake --build --preset desktop-debug
ctest --preset desktop-debug
./build/desktop-debug/slopsdr --mock
```

The full desktop application can also be run without hardware in deliberate
mock mode:

```sh
./build/desktop-app-debug/slopsdr --mock
```

## Development and testing build

Use the separate release-oriented test build for maintainer validation:

```sh
cmake --preset desktop-tests
cmake --build build/desktop-tests -j2
ctest --preset desktop-tests
./tools/test-gui-headless.sh
```

The headless GUI script uses `build/desktop-tests` by default and accepts an
explicit build-directory override.

## CI and sanitizer validation

CTest assigns every automated test to one of four tiers:

* `unit` covers isolated components.
* `runtime` covers deterministic threaded application and receiver behavior.
* `gui-smoke` covers lightweight Qt Widgets and Qt Quick integration under
  Xvfb.
* `extended` covers timing-sensitive playback, display, and dialog integration.

On pushes to `master` and pull requests, GitHub Actions builds the
`ci-desktop-tests` preset (with warnings as errors for slopSDR targets), runs
the deterministic `unit` and `runtime` labels, and runs the `gui-smoke` label
under Xvfb. A separate `desktop-tests-sanitized` job runs the same deterministic
tiers with target-scoped AddressSanitizer and UndefinedBehaviorSanitizer
instrumentation. The `extended` tier runs only from manual dispatch and the
weekly scheduled workflow; both its clean and sanitized jobs run the complete
suite.

Changes limited to `README.md` or `docs/` (including documentation images) do
not build C++. Their visible documentation-check job runs `git diff --check`
and checks local Markdown targets instead. Workflow, source, build, and test
changes always take the C++ path. Each CI job remains visible and reports
whether it ran or was skipped.

Run an individual local tier with the same CTest labels:

```sh
ctest --test-dir build/desktop-tests --output-on-failure -L 'unit|runtime'
./tools/test-gui-headless.sh build/desktop-tests -L gui-smoke
./tools/test-gui-headless.sh build/desktop-tests -L extended
```

For release validation, retain the complete clean and sanitizer runs rather
than filtering labels:

```sh
./tools/test-gui-headless.sh build/desktop-tests
./tools/test-gui-headless.sh build/desktop-tests-sanitized
```

Run the sanitizer configuration locally with the same headless test harness:

```sh
cmake --preset desktop-tests-sanitized
cmake --build build/desktop-tests-sanitized -j2
./tools/test-gui-headless.sh build/desktop-tests-sanitized
```

CI does not require SDR or audio hardware. It supplements, rather than
replaces, the local release-oriented validation described above and physical
hardware checks when they are relevant.

## Other tests

Run the dependency-disabled and complete desktop suites with:

```sh
ctest --preset desktop-debug
ctest --preset desktop-app-debug
```

Physical-hardware tests remain opt-in and require an exact
`SDR_TEST_DEVICE_ID`; normal test presets never open hardware.

The GNU Radio-enabled suite includes a synthetic smoke executable that builds
the complete spectrum and demodulation graph, exercises the five built-in analog
modes (AM, NFM, WFM, USB, and LSB), and repeats start, stop, and wait:

```sh
cmake --build build/desktop-app-debug --target gnuradio_smoke_test
./build/desktop-app-debug/tests/gnuradio_smoke_test
```

Use the hardware build's read-only diagnostic mode to report dependency,
kernel, and circular-buffer preference information:

```sh
./build/desktop-app-debug/slopsdr --diagnose-gnuradio
```

See `docs/TROUBLESHOOTING.md` for safe buffer-factory experiments and GDB
backtrace collection.

The default tests also use a fake audio sink and never require or open a sound
device. To build the separate audible Qt audio smoke test, configure a distinct
build with `ENABLE_AUDIO_HARDWARE_TESTING=ON`, then run only its labelled test:

```sh
cmake -S . -B build/audio-hardware -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
    -DENABLE_AUDIO_HARDWARE_TESTING=ON
cmake --build build/audio-hardware --target audio_output_smoke_test
ctest --test-dir build/audio-hardware -L audio-hardware --output-on-failure
```

The smoke test skips when no output exists. When an output is available it
opens the system default at 48 kHz stereo and plays a quiet 440 Hz tone for about
half a second.

## Install

Configure and build the release application before installing it under the
current user's local prefix:

```sh
cmake --preset desktop-app-release
cmake --build build/desktop-app-release -j2
cmake --install build/desktop-app-release --prefix ~/.local
```

This installs `slopsdr` under `~/.local/bin`.

## Cleaning stale build directories

Generated build directories are ignored by Git. Remove only the specific stale
configuration that needs to be regenerated:

```sh
rm -rf build/desktop-app-debug
rm -rf build/desktop-app-release
```

Then rerun the corresponding configure preset. Do not commit build output,
`CMakeFiles/`, or generated CMake metadata.
