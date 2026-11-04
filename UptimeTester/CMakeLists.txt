# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

add_executable(UptimeTester
    src/UptimeTester.cpp
)

target_link_libraries(UptimeTester
    PRIVATE
        cxxopts
        nvtrace_client
)
