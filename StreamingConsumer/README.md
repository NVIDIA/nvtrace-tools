# NvTrace Streaming Consumer

`nvtrace_streaming_consumer` collects profiling data continuously and writes it incrementally to a Trace Stream Database. It is suitable for fixed-duration captures and longer sessions that should not retain all data in memory.

## Usage

Start the matching NVIDIA Nsight Perf service, then run:

```shell
# Collect for the default 10 seconds.
./nvtrace_streaming_consumer --output-dir /path/to/output

# Collect for 60 seconds.
./nvtrace_streaming_consumer \
    --output-dir /path/to/output \
    --duration-ms 60000

# Collect until Enter is pressed.
./nvtrace_streaming_consumer \
    --output-dir /path/to/output \
    --stop-on-enter-key
```

The tool creates a timestamped directory:

```text
/path/to/output/hostname.YYYYMMDD_HHMMSS/
├── 1.nvtsf
├── 2.nvtsf
└── ...
```

Export that directory with [`nvtrace_perfetto_trace_exporter`](../PerfettoTraceExporter/README.md).

## Why use streaming collection

- Completed Trace Stream Format blocks are persisted incrementally instead of retaining the complete capture in memory.
- A dedicated storage-writer thread performs disk writes separately from the consumer's collection loop.
- Fixed-duration and interactive-stop modes support both bounded captures and operator-controlled sessions.

## Options

- `--duration-ms <ms>`: collection duration; default `10000`.
- `--stop-on-enter-key`: collect until Enter is pressed instead of using the duration.
- `--output-dir <path>`: parent directory for the Trace Stream Database; default is the current directory.
- `--ignore <provider_type>`: omit one or more comma-separated providers. See the [common provider-filtering documentation](../README.md#common-provider-filtering).
- `--help`: print usage.