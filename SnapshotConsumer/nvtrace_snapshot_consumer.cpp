/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <NvTrace/Offline/Exporter/NvTraceExporterBase.h>
#include <NvTrace/Target/Consumer/NvTraceStreamDatabaseWriter.h>
#include <NvTrace/Target/Consumer/NvTraceTsfSnapshotConsumer.h>
#include <NvTrace/Target/NvTraceClientLogger.h>
#include <NvTraceProviderIgnoreCli.h>

#include "cxxopts.hpp"

struct TsfSnapshotConsumerOptions
{
    uint32_t historyDurationMsec = 1000;
    uint32_t futureDurationMsec = 0;
    uint32_t triggerIntervalMsec = 1000;
    bool stopOnEnterKey = false;
    bool keepPrimaryDatabase = false;
    std::string outputDir;
    size_t dataBlockSizeMiB = 10;
    uint32_t historyDataBlockCount = 8;
    size_t maxZombieObjectBytes = 2ULL << 30;
    nv::trace::Codec::ProviderTypeTable<bool> isProviderTypeIgnored = {};
};

struct TsfSnapshotConsumerState
{
    std::mutex mutex;
    std::vector<uint32_t> snapshotNumbers;
};

static std::string GetLocalDateTimeAsString(std::chrono::system_clock::time_point timePoint)
{
    const auto time = std::chrono::system_clock::to_time_t(timePoint);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(timePoint.time_since_epoch()) % std::chrono::seconds(1);
    std::stringstream dateTime;
    dateTime << std::put_time(std::localtime(&time), "%Y%m%d_%H%M%S") << '_' << std::setfill('0') << std::setw(3) << milliseconds.count();
    return dateTime.str();
}

static std::string GetLocalDateTimeAsString()
{
    return GetLocalDateTimeAsString(std::chrono::system_clock::now());
}

static std::filesystem::path GenerateDatabasePath(const std::filesystem::path& outputDir)
{
    return outputDir / (nv::trace::GetHostName() + "." + GetLocalDateTimeAsString());
}

class TemporaryDatabaseDirectory
{
public:
    TemporaryDatabaseDirectory(std::filesystem::path databaseDirectoryPath, bool keep)
        : m_databaseDirectoryPath(std::move(databaseDirectoryPath))
        , m_keep(keep)
    {
    }

    ~TemporaryDatabaseDirectory()
    {
        if (m_keep)
        {
            return;
        }

        std::error_code errorCode;
        std::filesystem::remove_all(m_databaseDirectoryPath, errorCode);
        if (errorCode)
        {
            std::cerr << "Warning: failed to remove temporary TSF database directory " << m_databaseDirectoryPath << ": " << errorCode.message() << '\n';
        }
    }

    const std::filesystem::path& GetPath() const noexcept
    {
        return m_databaseDirectoryPath;
    }

private:
    std::filesystem::path m_databaseDirectoryPath;
    bool m_keep = false;
};

static TsfSnapshotConsumerOptions ParseOptions(int argc, char** argv)
{
    TsfSnapshotConsumerOptions opts;
    cxxopts::Options options(
        "nvtrace_snapshot_consumer",
        "NVIDIA NvTrace Snapshot Consumer retains a rolling TSF history and writes one snapshot database");

    // clang-format off
    options.add_options()
        ("duration-ms", "History duration in milliseconds for each snapshot", cxxopts::value<uint32_t>()->default_value("1000"))
        ("future-duration-ms", "Future duration in milliseconds to capture after each snapshot trigger", cxxopts::value<uint32_t>()->default_value("0"))
        ("trigger-interval-ms", "Debug only: interval in milliseconds between automatically triggered snapshots", cxxopts::value<uint32_t>()->default_value("2000"))
        ("stop-on-enter-key", "Wait for Enter before each snapshot (overrides --trigger-interval-ms); use --stop-on-enter-key=false for automatic snapshots", cxxopts::value<bool>()->default_value("true"))
        ("keep-primary-database", "Debug only: keep the temporary primary TSF database after exit", cxxopts::value<bool>()->default_value("false"))
        ("output-dir", "Output directory for the TSF snapshot database", cxxopts::value<std::string>()->default_value(""))
        ("data-block-size", "DataBlock size in MiB", cxxopts::value<size_t>()->default_value("10"))
        ("history-data-block-count", "Number of rolling TSF DataBlocks", cxxopts::value<uint32_t>()->default_value("8"))
        ("max-zombie-object-bytes", "Maximum bytes retained by destroyed objects", cxxopts::value<size_t>()->default_value("2147483648"))
        ("ignore", "Ignore provider data. Ignorable provider types: gpu_pm, gpu_hwt, cuda_api, nvtx. Ex: --ignore gpu_pm,gpu_hwt", cxxopts::value<std::vector<std::string>>(), "provider_type")
        ("h,help", "Print usage");
    // clang-format on

    const auto result = options.parse(argc, argv);
    if (result.count("help"))
    {
        std::cout << options.help() << '\n';
        std::exit(EXIT_SUCCESS);
    }

    opts.historyDurationMsec = result["duration-ms"].as<uint32_t>();
    opts.futureDurationMsec = result["future-duration-ms"].as<uint32_t>();
    opts.triggerIntervalMsec = result["trigger-interval-ms"].as<uint32_t>();
    opts.stopOnEnterKey = result["stop-on-enter-key"].as<bool>();
    opts.keepPrimaryDatabase = result["keep-primary-database"].as<bool>();
    opts.outputDir = result["output-dir"].as<std::string>();
    opts.dataBlockSizeMiB = result["data-block-size"].as<size_t>();
    opts.historyDataBlockCount = result["history-data-block-count"].as<uint32_t>();
    opts.maxZombieObjectBytes = result["max-zombie-object-bytes"].as<size_t>();

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

using Consumer = nv::trace::TsfSnapshotConsumer<nv::trace::TsfDiskDatabaseBufferedStorageWriter>;

static void OnSnapshotTaken(void* pUserData, uint32_t snapshotNumber)
{
    TsfSnapshotConsumerState* pState = static_cast<TsfSnapshotConsumerState*>(pUserData);
    std::lock_guard<std::mutex> lock(pState->mutex);
    pState->snapshotNumbers.push_back(snapshotNumber);
}

static void OnSnapshotCompleted(void* pUserData, uint32_t snapshotNumber, bool succeeded)
{
    TsfSnapshotConsumerState* pState = static_cast<TsfSnapshotConsumerState*>(pUserData);
    std::lock_guard<std::mutex> lock(pState->mutex);
    auto it = std::find(pState->snapshotNumbers.begin(), pState->snapshotNumbers.end(), snapshotNumber);
    if (it == pState->snapshotNumbers.end())
    {
        std::cout << "TSF snapshot " << snapshotNumber << " not found. This should not happen.\n";
        return;
    }
    pState->snapshotNumbers.erase(it);
    std::cout << "TSF snapshot " << snapshotNumber << " completed: " << (succeeded ? "success" : "failure") << '\n';
}

static bool InitializeConsumer(
    Consumer& consumer,
    const TsfSnapshotConsumerOptions& options,
    TsfSnapshotConsumerState& state,
    const std::filesystem::path& primaryDatabaseDirectoryPath)
{
    Consumer::Options consumerOptions;
    consumerOptions.dataBlockSize = options.dataBlockSizeMiB * 1024 * 1024;
    consumerOptions.numDataBlocks = options.historyDataBlockCount;
    consumerOptions.maxZombieObjectBytes = options.maxZombieObjectBytes;
    consumerOptions.isProviderTypeIgnored = options.isProviderTypeIgnored;
    consumerOptions.fnOnSnapshotTaken = OnSnapshotTaken;
    consumerOptions.pUserDataOnSnapshotTaken = &state;
    consumerOptions.fnOnSnapshotCompleted = OnSnapshotCompleted;
    consumerOptions.pUserDataOnSnapshotCompleted = &state;
    return consumer.Initialize(consumerOptions, [&primaryDatabaseDirectoryPath](nv::trace::TsfDiskDatabaseBufferedStorageWriter& storageWriter) {
        return storageWriter.Initialize(primaryDatabaseDirectoryPath);
    });
}

static bool TakeSnapshot(Consumer& consumer, const TsfSnapshotConsumerOptions& options)
{
    const std::filesystem::path snapshotDatabasePath = GenerateDatabasePath(std::filesystem::path(options.outputDir));
    std::cout << "Triggering TSF snapshot at " << snapshotDatabasePath << ".\n";
    Consumer::SnapshotParam snapshotParam;
    snapshotParam.historyDurationNsec = static_cast<uint64_t>(options.historyDurationMsec) * 1000ULL * 1000ULL;
    snapshotParam.futureDurationNsec = static_cast<uint64_t>(options.futureDurationMsec) * 1000ULL * 1000ULL;
    const bool succeeded = consumer.Snapshot(
        [&snapshotDatabasePath](nv::trace::TsfDiskDatabaseBufferedStorageWriter& snapshotStorageWriter) {
            return snapshotStorageWriter.Initialize(snapshotDatabasePath);
        },
        snapshotParam);
    if (!succeeded)
    {
        std::cerr << "Error: failed to take TSF snapshot at " << snapshotDatabasePath << ".\n";
    }
    return succeeded;
}

int main(int argc, char** argv)
{
    try
    {
        // The client logger has no sink by default, so warnings like a failed Router connection never appear.
        nv::trace::SetLogVolumeLevel(nv::trace::Logger::LogSeverity::Inf, 0);
        nv::trace::SetLogFileLine(false);
        nv::trace::UserLogEnableStderr(true);

        const auto options = ParseOptions(argc, argv);
        setvbuf(stdout, nullptr, _IONBF, 0);

        // OnSnapshotTaken() will add the snapshot number.
        // OnSnapshotCompleted() will remove the snapshot number.
        TsfSnapshotConsumerState state;

        // Declared before the consumer so it is removed only after the
        // consumer has stopped and released its primary storage writer.
        TemporaryDatabaseDirectory primaryDatabaseDirectory(
            GenerateDatabasePath(std::filesystem::path(options.outputDir) / ".nvtrace_snapshot_consumer_primary_database"),
            options.keepPrimaryDatabase);
        Consumer consumer;
        std::cout << "Initializing TSF snapshot consumer.\n";
        if (!InitializeConsumer(consumer, options, state, primaryDatabaseDirectory.GetPath()))
        {
            std::cerr << "Error: TSF SnapshotConsumer initialization failed.\n";
            return EXIT_FAILURE;
        }
        std::cout << "TSF snapshot consumer initialized.\n";

        if (!options.stopOnEnterKey)
        {
            std::cout << "Capturing. Triggering a snapshot every " << options.triggerIntervalMsec << " ms.\n";
        }

        while (true)
        {
            if (options.stopOnEnterKey)
            {
                std::cout << "Capturing. Press Enter to trigger the snapshot...\n";
                if (std::cin.get() == std::char_traits<char>::eof())
                {
                    if (!std::cin.eof())
                    {
                        std::cerr << "Error: failed to read snapshot trigger from stdin.\n";
                        return EXIT_FAILURE;
                    }
                    std::cout << "Stdin reached EOF. Stopping snapshot consumer.\n";
                    break;
                }
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(options.triggerIntervalMsec));
            }

            // Snapshot() returns after the snapshot command is accepted.
            // futureDurationNsec is captured by TsfSnapshotConsumer via a later checkpoint.
            std::cout << "Snapshot triggered (history " << options.historyDurationMsec
                      << " ms, future " << options.futureDurationMsec << " ms).\n";
            TakeSnapshot(consumer, options);
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
