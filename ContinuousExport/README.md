# NvTrace Continuous Export

`nvtrace_continuous_export` retains rolling trace data, periodically completes snapshots, and invokes an export hook. 
By default, the tool merges completed snapshots into `snapshots.duckdb`. The database grows throughout capture. Sending rows to a backend server requires modifying the export hook.
Enable `--perfetto` to export each completed snapshot as a separate Perfetto trace archive for debugging. The number of archives grows throughout capture.

## Usage

```shell
./nvtrace_continuous_export \
    --output-dir /path/to/output \
    --snapshot-ms 30000
```

Each run creates a `hostname.continuous-snapshots-*` directory. 
Each snapshot is ingested into the DuckDB database `snapshots.duckdb` (see `--db`). 

The export process must be able to export each snapshot to the DB within the time period of one snapshot.
If export falls behind, then data can be dropped and will be missing from the DB.
Ensure that the DB export process has sufficient resources.
To ensure speed of the DB export process, write to a file on local storage instead of over a network.

At shutdown, the database is exported to `parquet_export/` with one `.parquet` file per table. 
Perfetto output files are named `snapshot-<number>.perfetto-trace.zip`.

## Options

- `--snapshot-ms <ms>`: snapshot interval; default `30000`.
- `--data-rate <MiB/s>`: expected average trace-data rate used for storage sizing; default `12`.
- `--data-block-size <MiB>`: TSF data-block size; default `10`.
- `--latency-buffer <count>`: in-memory blocks used to absorb storage latency; default `8`.
- `--max-attach-size <MiB>`: maximum snapshot attachment size, rounded to whole blocks; default `100`.
- `--max-retained-shm <MiB>`: provider shared memory retained after process exit; default `2048`.
- `--duration-ms <ms>`: maximum runtime; default is unlimited.
- `--stop-on-enter-key <bool>`: stop when Enter is pressed; default `true`.
- `--output-dir <path>`: parent directory for the per-run directory.
- `--db <path>`: DuckDB database to create or update; default `snapshots.duckdb` in the per-run directory.
- `--db-flush-threshold <MiB>`: memory of buffered rows per table before they are upserted into the database; `0` disables the memory limit and flushes every 204800 rows instead; default `16`.
- `--db-memory-limit <MiB>`: memory DuckDB may use, including its cache of database pages; `0` keeps DuckDB's default of 80% of physical memory; default `0`.
- `--keep-work-files <bool>`: retain temporary TSF files; default `false`.
- `--perfetto <bool>`: also export each completed snapshot to Perfetto; default `false`.
- `--enable-debug-events <bool>`: include debug events in Perfetto output; default `false`.
- `--show-data-rate <bool>`: display a live data-rate and volume meter; default `false`.
- `--show-schema-only <bool>`: print the SQL that creates all tables, then exit; default `false`.
- `--ignore <provider_type>`: omit one or more comma-separated providers. See the [common provider-filtering documentation](../README.md#common-provider-filtering).
- `--help`: print usage.

## Database

For each snapshot, the export hook builds a data model and writes it through `NvTraceTableExporter.h` into one working DuckDB database.

`NvTraceDuckDbTableSink.h` implements the schema in Duck DB. It could be replaced with another sink for a different database.

## Schema

`NvTraceTableSchema.h` defines the database schema in a database agnostic way. 
To see the SQL version of the schema, use the `--show-schema-only` flag.
The schema is designed to support multiple nodes and have all of the relevant captured information.

Exported Schema
├── Capture and system
│   ├── traces
│   └── snapshot_metadata
│
├── Process data
│   └── processes
│       ├── Threads
│       │   └── thread_event_threads
│       │
│       ├── CUDA
│       │   ├── Resources
│       │   │   ├── cuda_devices
│       │   │   ├── cuda_contexts
│       │   │   ├── cuda_streams
│       │   │   ├── cuda_modules
│       │   │   └── cuda_functions
│       │   │
│       │   ├── CPU API activity
│       │   │   ├── cuda_runtime_api_calls
│       │   │   ├── cuda_driver_api_calls
│       │   │   └── cuda_stream_api_call_refs
│       │   │
│       │   ├── CUDA graphs
│       │   │   ├── cuda_graph_execs
│       │   │   ├── cuda_graph_nodes
│       │   │   └── cuda_graph_launches
│       │   │
│       │   ├── GPU workloads
│       │   │   ├── cuda_immediate_kernel_launches
│       │   │   ├── cuda_graph_node_launches
│       │   │   ├── cuda_gpu_commands
│       │   │   │   ├── cuda_gpu_memcpy_details
│       │   │   │   ├── cuda_gpu_memset_details
│       │   │   │   └── cuda_gpu_mem_ops
│       │   │   └── cuda_host_callback_instances
│       │   │
│       │   └── Unified view
│       │       └── cuda_gpu_operations (view)
│       │
│       └── NVTX
│           ├── nvtx_domains
│           ├── nvtx_marks
│           └── nvtx_push_pop_ranges
│
├── GPU partition data
│   └── gpu_partitions
│       ├── Metadata
│       │   ├── gpu_partition_supported_providers
│       │   └── gpu_partition_cuda_attributes
│       ├── GR context trace
│       │   └── gpu_gr_ctx_resident_contexts
│       └── Performance metrics
│           ├── gpu_pm_samples
│           └── gpu_metrics
│
└── Cross-domain projection data
    ├── nvtx_gpu_projection_segments
    └── nvtx_gpu_projection_spans (view)
