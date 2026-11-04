# NVTX CUDA Driver Injection Proxy

Applications built against NVTX 3.7 or newer perform the CUDA driver discovery needed for NVIDIA Nsight Perf tracing. For an application built against an older NVTX version, the `nvtx_cuda_driver_injection_proxy` library performs that discovery on the application's behalf.

## Usage

Set `NVTX_INJECTION64_PATH` to the proxy library when launching the application:

```shell
NVTX_INJECTION64_PATH=/path/to/libnvtx_cuda_driver_injection_proxy.so \
    /path/to/application
```


The proxy activates only when the NVIDIA Nsight Perf NVTX configuration is present in the platform runtime location. It then asks the CUDA driver for the NVTX injection initializer and forwards initialization to it.

Frameworks that launch workers with restricted environments must explicitly forward `NVTX_INJECTION64_PATH`. For example, vLLM's Ray executor can forward it with:

```shell
VLLM_RAY_EXTRA_ENV_VARS_TO_COPY=NVTX_INJECTION64_PATH
```