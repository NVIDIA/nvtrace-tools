/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <NvTrace/Target/Consumer/NvTraceUptimeValidationConsumer.h>
#include <NvTrace/Target/NvTraceCpuTimestamp.h>

#include "cxxopts.hpp"

#include <cstdio>
#include <iostream>

struct GlobalTestOptions
{
    uint64_t gpuHwEventRate = 0;
};
GlobalTestOptions g_TestOptions;

void ParseGlobalTestOptions(int argc, char** argv)
{
    cxxopts::Options options("Options");
    options.add_options()("gpu-hw-event-rate", "the minimum expected rate of GPU HW trace events, expressed in nanoseconds. (at least one event every N nanoseconds).", cxxopts::value<uint64_t>()->default_value("0"));
    options.add_options()("h,help", "Print usage");

    auto result = options.parse(argc, argv);

    if (result.count("help") > 0)
    {
        std::cout << options.help() << std::endl;
        exit(0);
    }

    g_TestOptions.gpuHwEventRate = result["gpu-hw-event-rate"].as<uint64_t>();
}

int main(int argc, char** argv)
{
    using namespace nv::trace;

    ParseGlobalTestOptions(argc, argv);

    // Enable stderr logging to see the test output
    UserLogEnableStderr(true);

    UptimeValidationConsumer::Options uptimeValidationConsumerOptions;
    uptimeValidationConsumerOptions.consumeIntervalMsec = 100;
    uptimeValidationConsumerOptions.gpuHwTraceUpdateIntervalNsec = g_TestOptions.gpuHwEventRate;
    uptimeValidationConsumerOptions.cudaApiTraceUpdateIntervalNsec = 0; // currently buggy, so disabled

    UptimeValidationConsumer uptimeValidationConsumer;

    if (!uptimeValidationConsumer.Initialize(uptimeValidationConsumerOptions))
    {
        return 1;
    }

    printf("\n");

    uint64_t lastTotalErrorCount = 0;
    while (uptimeValidationConsumer.Update())
    {
        const auto& counters = uptimeValidationConsumer.GetCounters();
        const uint64_t totalErrorCount = counters.outOfOrderSampleCount + counters.tooLongIntervalCount + counters.providerHaltedCount;

        // If the error count hasn't changed, no error messages have been printed, so override the previous line instead of appending a new one
        if (totalErrorCount == lastTotalErrorCount)
        {
            printf("\033[A\r");
        }

        printf(
            "Out-of-order sample count: %llu, too long interval count: %llu, provider halted count: %llu                             \n",
            (unsigned long long)counters.outOfOrderSampleCount,
            (unsigned long long)counters.tooLongIntervalCount,
            (unsigned long long)counters.providerHaltedCount);

        lastTotalErrorCount = totalErrorCount;
    }

    // The loop should never exit, so if we get here, something went wrong
    return 1;
}
