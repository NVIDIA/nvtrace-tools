# SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

## nvtrace_streaming_consumer

add_executable(nvtrace_streaming_consumer
    nvtrace_streaming_consumer.cpp
)

target_link_libraries(nvtrace_streaming_consumer
    PRIVATE
        NvTraceToolUtilities
        cxxopts
        nvtrace_client_offline
        Threads::Threads
)
