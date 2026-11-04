# NvTrace Tools

This directory contains command-line tools and supporting libraries for collecting, storing, and exporting NvTrace data:

- [`ContinuousExport`](ContinuousExport/README.md) continuously exports rolling trace snapshots.
- [`PerfettoTraceExporter`](PerfettoTraceExporter/README.md) exports Trace Stream Databases to Perfetto traces.
- [`PerfettoTraceMerger`](PerfettoTraceMerger/README.md) merges multiple Perfetto traces into one.
- [`PollUntilServiceReady`](PollUntilServiceReady/README.md) waits for the NVIDIA Nsight Perf service to become ready.
- [`SnapshotConsumer`](SnapshotConsumer/README.md) captures lookback snapshots.
- [`StreamingConsumer`](StreamingConsumer/README.md) consumes streams and writes Trace Stream Databases.
- [`NvtxCudaDriverInjectionProxy`](NvtxCudaDriverInjectionProxy/README.md) provides the NVTX CUDA driver injection proxy library.
- [`UptimeTester`](UptimeTester/README.md) provides a service liveness diagnostic.

## Common provider filtering

Consumers and exporters accept `--ignore <provider_type>` to omit selected provider data:

- `gpu_pm`: GPU performance-monitoring counters
- `gpu_hwt`: GPU hardware trace and correlated GPU events
- `cuda_api`: CUDA API trace
- `nvtx`: NVTX trace

Pass multiple providers as a comma-separated list:

```shell
./nvtrace_streaming_consumer --ignore gpu_pm,nvtx
./nvtrace_perfetto_trace_exporter \
    --tsf-database-dir /path/to/database \
    --ignore gpu_hwt,cuda_api
```

A consumer does not write ignored providers to its Trace Stream Database. The exporter omits ignored providers from its output without modifying its source database.

## Compatibility

Consumer tools inherit the bidirectional [service-consumer compatibility guarantee](https://github.com/NVIDIA/nsight-perf-sdk-for-ai/blob/main/README.md#compatibility): a consumer tool can use an older or newer NVIDIA Nsight Perf service. Features are limited to those understood by the tool and service versions.

Tool command-line interfaces and human-readable output are not guaranteed compatible across releases.

## Contributions

Contributions require signed-off commits and the Developer Certificate of Origin. See [CONTRIBUTING.md](CONTRIBUTING.md) before submitting a change.

## Security

Do not report potential security vulnerabilities through public GitHub issues. Follow the private reporting process in [SECURITY.md](SECURITY.md).

## License

NVIDIA-authored source is licensed under Apache-2.0; see [LICENSE](LICENSE). Dependencies and separately supplied proprietary runtimes retain their own terms. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
