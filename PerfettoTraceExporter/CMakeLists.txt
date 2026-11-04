# SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

## nvtrace_perfetto_trace_exporter

add_executable(nvtrace_perfetto_trace_exporter
    nvtrace_perfetto_trace_exporter.cpp
)

target_link_libraries(nvtrace_perfetto_trace_exporter
    PRIVATE
        NvTraceToolUtilities
        cxxopts
        nvtrace_client_offline
)
