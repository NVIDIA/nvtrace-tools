# NvTrace Perfetto Trace Merger

`nvtrace_merge_perfetto_traces` combines one or more `.perfetto.zip` archives.

## Usage

```shell
./nvtrace_merge_perfetto_traces \
    --output combined.perfetto.zip \
    first.perfetto.zip second.perfetto.zip
```

Use `-o` as the short form of `--output`.

## Response files

Input arguments can also come from a response file:

```shell
./nvtrace_merge_perfetto_traces \
    --output combined.perfetto.zip \
    @input-archives.txt
```

Response files contain whitespace-separated paths, support single- or double-quoted paths, and may be nested up to eight levels.
