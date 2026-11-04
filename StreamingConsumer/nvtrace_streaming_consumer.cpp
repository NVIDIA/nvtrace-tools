/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include <NvTrace/Offline/Exporter/NvTracePerfettoTraceExporter.h>
#include <NvTrace/Target/Consumer/NvTraceStreamingConsumer.h>
#include <NvTrace/Target/NvTraceClientLogger.h>
#include <NvTrace/Target/NvTraceCpuTimestamp.h>
#include <NvTraceProviderIgnoreCli.h>

#include "cxxopts.hpp"
#include <NvTrace/Target/Consumer/NvTraceStreamDatabaseWriter.h>

// Configuration options for the streaming consumer
struct StreamingConsumerOptions
{
    int durationMsec = 10000;
    bool stopOnEnterKey = false;
    std::string outputDir;
    nv::trace::Codec::ProviderTypeTable<bool> isProviderTypeIgnored = {};
};

// Get current local date and time as formatted string (YYYYMMDD_HHMMSS)
static std::string GetLocalDateTimeAsString()
{
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream dateTime;
    dateTime << std::put_time(std::localtime(&in_time_t), "%Y%m%d_%H%M%S");
    return dateTime.str();
}

// Generate database directory path with hostname and timestamp
static std::filesystem::path GenerateDatabasePath(const std::string& outputDir)
{
    std::filesystem::path directory = outputDir;
    std::filesystem::path databaseDirectoryName = nv::trace::GetHostName() + "." + GetLocalDateTimeAsString();
    return directory / databaseDirectoryName;
}

// Parse command-line arguments and return options
static StreamingConsumerOptions ParseStreamingConsumerOptions(int argc, char** argv)
{
    StreamingConsumerOptions opts;

    cxxopts::Options options(
        "nvtrace_streaming_consumer",
        "NVIDIA NvTrace Streaming Consumer streams profiling data directly to disk as a TSF database");

    // clang-format off
     options.add_options()
         ("duration-ms", "Duration in milliseconds", cxxopts::value<int>()->default_value("10000"))
         ("stop-on-enter-key", "Consume until enter key is pressed (overrides duration-ms)", cxxopts::value<bool>()->default_value("false"))
         ("output-dir", "Output directory for TSF database", cxxopts::value<std::string>()->default_value(""))
         ("ignore", "Ignore provider data. Ignorable provider types: gpu_pm, gpu_hwt, cuda_api, nvtx. Ex: --ignore gpu_pm,gpu_hwt", cxxopts::value<std::vector<std::string>>(), "provider_type")
         ("h,help", "Print usage");
    // clang-format on

    auto result = options.parse(argc, argv);

    if (result.count("help"))
    {
        std::cout << options.help() << '\n';
        exit(EXIT_SUCCESS);
    }

    opts.durationMsec = result["duration-ms"].as<int>();
    opts.stopOnEnterKey = result["stop-on-enter-key"].as<bool>();
    opts.outputDir = result["output-dir"].as<std::string>();

    if (result.count("ignore"))
    {
        for (const std::string& providerName : result["ignore"].as<std::vector<std::string>>())
        {
            const nv::trace::Codec::ProviderType providerType = nv::trace::ResolveIgnorableProviderTypeByName(providerName);
            if (nv::trace::Codec::ProviderType::Invalid < providerType && providerType < nv::trace::Codec::ProviderType::Count)
            {
                opts.isProviderTypeIgnored[static_cast<size_t>(providerType)] = true;
            }
        }
    }

    return opts;
}

// Initialize and configure the streaming consumer
static bool InitializeStreamingConsumer(
    nv::trace::StreamingConsumer<nv::trace::TsfDiskDatabaseBufferedStorageWriter>& consumer,
    const StreamingConsumerOptions& options)
{
    std::cout << "Initializing StreamingConsumer...\n";

    using ConsumerType = nv::trace::StreamingConsumer<nv::trace::TsfDiskDatabaseBufferedStorageWriter>;
    typename ConsumerType::Options consumerOptions;
    consumerOptions.dataBlockSize = 10 * 1024 * 1024; // 10 MiB
    consumerOptions.isProviderTypeIgnored = options.isProviderTypeIgnored;
    auto databasePath = GenerateDatabasePath(options.outputDir);
    const bool initialization_success = consumer.Initialize(consumerOptions, [databasePath](nv::trace::TsfDiskDatabaseBufferedStorageWriter& storageWriter) {
        std::cout << "Creating database at: " << databasePath.string() << '\n';
        return storageWriter.Initialize(databasePath);
    });
    if (!initialization_success)
    {
        std::cerr << "Error: StreamingConsumer initialization failed.\n";
        return false;
    }

    return true;
}

// Run the consumer for the specified duration or until user input
static bool RunConsumer(
    nv::trace::StreamingConsumer<nv::trace::TsfDiskDatabaseBufferedStorageWriter>& consumer,
    const StreamingConsumerOptions& options)
{
    if (!consumer.Start())
    {
        std::cerr << "Error: StreamingConsumer failed to start.\n";
        return false;
    }
    // User and container scripts can wait for this line to start their application, so print it only after Start() succeeded.
    std::cout << "Starting NvTrace observer...\n";

    if (options.stopOnEnterKey)
    {
        std::cout << "Press enter key to stop...\n";
        std::cin.get();
    }
    else
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(options.durationMsec));
    }

    std::cout << "Stopping consumer...\n";
    if (!consumer.StopAtTsc(nv::trace::ReadCpuTscTimestamp()))
    {
        std::cerr << "Error: StreamingConsumer failed to stop.\n";
        return false;
    }
    return true;
}

int main(int argc, char** argv)
{
    try
    {
        // The client logger has no sink by default, so warnings like a failed Router connection never appear.
        nv::trace::SetLogVolumeLevel(nv::trace::Logger::LogSeverity::Inf, 0);
        nv::trace::SetLogFileLine(false);
        nv::trace::UserLogEnableStderr(true);

        auto options = ParseStreamingConsumerOptions(argc, argv);

        // Disable stdout buffering to ensure any wrapper program receives
        // timely progress indications
        setvbuf(stdout, nullptr, _IONBF, 0);

        // Initialize and run the streaming consumer
        nv::trace::StreamingConsumer<nv::trace::TsfDiskDatabaseBufferedStorageWriter> streamingConsumer;
        if (!InitializeStreamingConsumer(streamingConsumer, options))
        {
            return EXIT_FAILURE;
        }
        if (!RunConsumer(streamingConsumer, options))
        {
            return EXIT_FAILURE;
        }

        std::cout << "Streaming consumer completed successfully\n";

        return EXIT_SUCCESS;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
