# Troubleshooting

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
