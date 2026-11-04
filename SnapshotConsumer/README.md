# NvTrace Snapshot Consumer

`nvtrace_snapshot_consumer` retains a rolling Trace Streaming Format history and writes a snapshot database when triggered. Use it as a flight recorder for lookback or anomaly-triggered capture.

## Usage

```shell
./nvtrace_snapshot_consumer \
    --output-dir /path/to/output \
    --duration-ms 1000
```

By default, press Enter to trigger a snapshot. Each successful trigger writes a separate Trace Stream Database. Export the resulting database with [`nvtrace_perfetto_trace_exporter`](../PerfettoTraceExporter/README.md).

## Options

- `--duration-ms <ms>`: lookback history duration; default `1000`.
- `--future-duration-ms <ms>`: additional interval captured after the trigger; default `0`.
- `--stop-on-enter-key`: wait for Enter before each snapshot; default `true`.
- `--trigger-interval-ms <ms>`: debug-only automatic trigger interval; default `2000`.
- `--output-dir <path>`: output directory for snapshot databases; default is the current directory.
- `--data-block-size <MiB>`: TSF data-block size; default `10`.
- `--history-data-block-count <count>`: number of blocks in the rolling history; default `8`.
- `--max-zombie-object-bytes <bytes>`: retention budget for destroyed process, provider, and GPU-partition buffers; default `2147483648` (2 GiB).
- `--keep-primary-database`: debug-only retention of the temporary rolling database.
- `--ignore <provider_type>`: omit one or more comma-separated providers. See the [common provider-filtering documentation](../README.md#common-provider-filtering).
- `--help`: print usage.

## Known limitations

The consumer retains buffers belonging to destroyed processes, providers, and GPU partitions so their events can appear in later snapshots. When `--max-zombie-object-bytes` is exceeded, it releases older retained buffers. A later snapshot can therefore omit CUDA API or NVTX events from exited processes. Increase the budget when preserving more exited-process history is more important than memory use.

