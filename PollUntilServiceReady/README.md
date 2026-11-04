# NvTrace Poll Until Service Ready

`nvtrace_poll_until_service_ready` waits until the NVIDIA Nsight Perf service and required GPU providers are ready. Run it before a workload that must not start ahead of profiling initialization.

## Usage

```shell
./nvidia-nsight-perf &
./nvtrace_poll_until_service_ready
```

The command exits successfully when initialization completes and fails if readiness is not reached before the timeout.

## Options

- `--timeout-sec <seconds>`: maximum wait; default `60`.
- `--ignore <provider_type>`: exclude `gpu_pm`, `gpu_hwt`, or both from the readiness requirement.
- `--help`: print usage.

When a service provider is disabled, ignore the same provider here:

```shell
NSIGHT_PERF_GPU_PM_ENABLE=0 ./nvidia-nsight-perf &
./nvtrace_poll_until_service_ready --ignore gpu_pm
```