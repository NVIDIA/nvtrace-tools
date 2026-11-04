# Uptime Tester

Uptime Tester is a liveness probe that runs continuously, verifying that the NVIDIA Nsight Perf service is running. It is intended to be used as a diagnostic tool. Use it to detect errors and intermittent event dropouts and to provide additional information when filing bug reports to NVIDIA.

## How to use Uptime Tester

***With another process running NVIDIA Nsight Perf service***, open another terminal:

```shell
./UptimeTester
```

Additionally you can specify the optional parameter `--gpu-hw-event-rate`, which is the minimum expected rate of GPU HW trace events, expressed in nanoseconds. (at least one event every N nanoseconds).
The intended usage is to receive an error message when the GPU HW trace provider is silent for longer than N nanoseconds. The parameter is application dependent, and requires continuous GPU workloads in the background to be meaningful.

- If NVIDIA Nsight Perf is working **successfully**: Uptime Test will remain *silent*.

- If one of NVIDIA Nsight Perf’s components **fails**: Uptime Test will *report an error*.

- If the NVIDIA Nsight Perf shuts down while Uptime Tester is running, it will *report an error* and quit.