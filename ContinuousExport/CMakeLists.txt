# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

## nvtrace_continuous_export

add_executable(nvtrace_continuous_export
    nvtrace_continuous_export.cpp
)

target_link_libraries(nvtrace_continuous_export
    PRIVATE
        NvTraceToolUtilities
        cxxopts
        nvtrace_client_offline
        NvTraceConsumerDuckDb
        Threads::Threads
)
