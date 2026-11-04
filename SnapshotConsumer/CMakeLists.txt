# SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

## nvtrace_snapshot_consumer (TSF)

add_executable(nvtrace_snapshot_consumer
    nvtrace_snapshot_consumer.cpp
)

target_link_libraries(nvtrace_snapshot_consumer
    PRIVATE
        NvTraceToolUtilities
        cxxopts
        nvtrace_client_offline
)
