# SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

## nvtx_cuda_driver_injection_proxy

add_library(nvtx_cuda_driver_injection_proxy SHARED
    nvtx_cuda_driver_injection_proxy.cpp
)

target_link_libraries(nvtx_cuda_driver_injection_proxy
    PRIVATE
        ${CMAKE_DL_LIBS}
        nvtrace_client
        nvtx3
)
