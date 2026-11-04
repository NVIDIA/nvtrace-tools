/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <NvTrace/Target/Consumer/NvTraceProviderTypes.h>
#include <NvTrace/Target/NvTraceClientLogger.h>

#include <string_view>

namespace nv::trace {

    inline Codec::ProviderType ResolveIgnorableProviderTypeByName(std::string_view providerName)
    {
        struct IgnorableProviderType
        {
            std::string_view name;
            Codec::ProviderType providerType;
        };

        constexpr IgnorableProviderType ignorableProviderTypes[] = {
            {"cuda_api",  Codec::ProviderType::CudaApiTrace},
            { "gpu_hwt",    Codec::ProviderType::GpuHwTrace},
            {  "gpu_pm", Codec::ProviderType::GpuPmCounters},
            {    "nvtx",     Codec::ProviderType::NvtxTrace},
        };

        for (const auto& [name, providerType] : ignorableProviderTypes)
        {
            if (name == providerName)
            {
                return providerType;
            }
        }

        NVTRACE_LOG_WRN(
            50,
            "Invalid provider type input '%.*s'. Ignorable provider types: gpu_pm, gpu_hwt, cuda_api, nvtx.",
            static_cast<int>(providerName.size()),
            providerName.data());

        return Codec::ProviderType::Invalid;
    }

} // namespace nv::trace
