# SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

## nvtrace_poll_until_service_ready

add_executable(nvtrace_poll_until_service_ready
    nvtrace_poll_until_service_ready.cpp
)

target_link_libraries(nvtrace_poll_until_service_ready
    PRIVATE
        NvTraceToolUtilities
        CUDA::cuda_driver
        CUDA::toolkit
        cxxopts
        nvtrace_client_offline
)
