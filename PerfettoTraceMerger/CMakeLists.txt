# SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

add_library(NvTracePerfettoTraceMerger INTERFACE)
target_include_directories(NvTracePerfettoTraceMerger INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(NvTracePerfettoTraceMerger INTERFACE
    nvtrace_client_offline
)

add_executable(nvtrace_merge_perfetto_traces
    nvtrace_merge_perfetto_traces.cpp
)

target_link_libraries(nvtrace_merge_perfetto_traces
    PRIVATE
        NvTracePerfettoTraceMerger
)
