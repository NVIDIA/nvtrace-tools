# NvTrace Perfetto Trace Exporter

`nvtrace_perfetto_trace_exporter` converts a Trace Stream Database into a compressed Perfetto trace archive (`.perfetto-trace.zip`) for visualization in [Perfetto](https://ui.perfetto.dev).

## Usage

```shell
./nvtrace_perfetto_trace_exporter \
    --tsf-database-dir /path/to/hostname.YYYYMMDD_HHMMSS
```

The database path can also be supplied as the positional argument.

Export a bounded UTC interval, expressed in milliseconds since the Unix epoch:

```shell
./nvtrace_perfetto_trace_exporter \
    --tsf-database-dir /path/to/database \
    --start-time-utc-ms <start> \
    --end-time-utc-ms <end>
```

An omitted or zero start uses the recording start; an omitted or zero end uses the recording end. Exporting an unbounded large recording can exhaust memory.

## Options

- `--tsf-database-dir <path>`: required Trace Stream Database directory.
- `--enable-debug-events`: include calibration, provider diagnostics, and other debug events.
- `--enable-stream-timelines`: include per-stream CUDA event timelines.
- `--ignore <provider_type>`: omit one or more comma-separated providers. See the [common provider-filtering documentation](../README.md#common-provider-filtering).
- `--start-time-utc-ms <ms>` and `--end-time-utc-ms <ms>`: select an analysis window.
- `--activity-timespan`: print the CUDA, NVTX, and kernel activity interval and offer to export it.
- `--full-timespan`: print the complete recording interval and offer to export it.
- `--processes`: print process lifetimes and offer to export the union of selected process LUIDs.
- `--help`: print usage.

The three informational options can be combined:

```shell
./nvtrace_perfetto_trace_exporter \
    --tsf-database-dir /path/to/database \
    --activity-timespan \
    --full-timespan \
    --processes
```

Press Enter at an export prompt to skip that export. Interactive outputs add `.full`, `.activity`, or `.processes-<LUID>-...` to the filename.

## Partial-timespan limitations

- An event that begins before and ends after the analysis window may be absent.
- A GPU context-switch interval enclosing the window may be attributed to an inferred or unknown process.
- A CUDA API call whose entry precedes the window may be absent even if its exit is inside the window.
- An NVTX push-pop range that begins before the window may use the placeholder name `?? (NVTX Push-Pop Range)`.

Move the start earlier, or omit it, when the exporter needs state established before the desired interval. Loading more history consumes more memory.

## Perfetto-view limitations

- The final GPU PM-counter event is synthetic and stabilizes the counter-track height.
- Track ordering may differ from the intended order because Perfetto does not support the required JSON track sorting.

## Compatibility

The exporter supports only Trace Stream Format versions implemented by its matching client. Cross-version database compatibility is not guaranteed.
