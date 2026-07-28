# Troubleshooting

## DMR/P25 decoding problems

DMR/P25 support is experimental and requires a compatible, separately installed
DSD-FME executable. Confirm that its configured path in **Settings** points to
an executable file and inspect the DSD-FME status and stderr messages in the
**Console**. Decoding reliability may vary, and FEC errors or audio underruns
may occur. Encrypted traffic is not decoded.

Start slopSDR with `--verbose` and select **All** in the Console severity filter
to show one `App-measured decoder diagnostics` summary per second while the
DMR/P25 decoder is active. The fields are cumulative since the current decoder
start unless described as a current value:

* `input-rms` and `input-peak` are normalized levels for the bounded samples
  queued by slopSDR; `1.0` is full scale. `clipped` counts finite input samples
  outside the accepted `-1.0` to `1.0` range, and `non-finite` counts values
  replaced with silence.
* `discontinuities` counts observed input-drop incidents, while
  `dropped-samples` counts samples lost from the DSP input buffer or discarded
  to keep the decoder-input queue bounded.
* `queued-input-bytes` is the current total waiting in slopSDR and Qt's process
  pipe; `queued-input-peak` is its high-water mark. A sustained high value with
  increasing drops indicates that DSD-FME is not consuming input fast enough.
* `partial-writes` counts accepted prefixes that must be retried later;
  `failed-writes` counts pipe errors that stop the decoder process.
* `stdout-backlog-bytes` is the current unread decoded-audio backlog, and
  `stdout-backlog-peak` is its high-water mark.
* `audio-underruns` and `platform-audio-underruns` count starvation observed by
  slopSDR's playback buffer and by the platform audio sink while decoder mode
  is active.

These values are measured by slopSDR. DSD-FME stderr is separate, opaque
decoder-reported diagnostic text: slopSDR retains and displays it with bounded
memory and processing, but does not infer frame or FEC counters from it.

## GNU Radio circular-buffer and `shmat` messages

GNU Radio uses a double-mapped circular buffer between streaming blocks. Its
`gr::vmcircbuf` startup check may print messages such as:

```text
gr::vmcircbuf :error: shmat (2): Invalid argument
```

The message alone does not establish the cause of a later failure. GNU Radio
can reject one allocation and continue with another buffer arrangement. Capture
a backtrace and identify the faulting thread before attributing a crash to the
buffer factory. Do not change kernel shared-memory limits without evidence that
those limits caused the failed allocation.

The hardware build reports the relevant versions, kernel, GNU Radio user
preference directory, preference path, and saved factory without changing any
configuration:

```sh
./build/desktop-app-debug/slopsdr --diagnose-gnuradio
```

The GNU Radio command-line tool can also print the preference directory:

```sh
gnuradio-config-info --userprefsdir
```

The selected factory is stored in the plain-text file
`prefs/vmcircbuf_default_factory` below that directory. The following commands
back it up and remove the saved choice so GNU Radio can test available factories
again:

```sh
prefs_dir="$(gnuradio-config-info --userprefsdir)"
factory_file="$prefs_dir/prefs/vmcircbuf_default_factory"
cp "$factory_file" "$factory_file.backup"
rm "$factory_file"
```

To test the temporary-file mmap factory in the user's development environment,
write its exact GNU Radio name before launching the application or smoke test:

```sh
printf '%s\n' 'gr::vmcircbuf_mmap_tmpfile_factory' > "$factory_file"
./build/desktop-app-debug/slopsdr
```

GNU Radio may replace the file if its startup check rejects that factory. This
procedure changes a user-level GNU Radio preference; it does not change project
source code. Restore the original saved choice after the experiment:

```sh
mv "$factory_file.backup" "$factory_file"
```

## Collect a complete GDB backtrace

Build the hardware application with debug symbols, then start it in GDB:

```sh
cmake --preset desktop-app-debug
cmake --build --preset app
gdb --args ./build/desktop-app-debug/slopsdr
```

At the GDB prompt, enter `run`, reproduce the failure, and capture every thread:

```text
set pagination off
thread apply all bt full
```

Do not commit the backtrace, a core dump, or machine-specific paths. Include the
faulting thread's relevant top frames in the task or bug report together with
the output of `--diagnose-gnuradio`.
