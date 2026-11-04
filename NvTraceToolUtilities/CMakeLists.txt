# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

add_library(NvTraceToolUtilities INTERFACE)
target_include_directories(NvTraceToolUtilities INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(NvTraceToolUtilities INTERFACE
    nvtrace_client
)
