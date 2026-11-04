/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <NvTrace/Offline/DataModel/NvTraceStreamDataModelBuilder.h>
#include <NvTrace/Offline/DataModel/NvTraceStreamDatabaseReader.h>
#include <NvTrace/Offline/Exporter/NvTracePerfettoTraceExporter.h>
#include <NvTraceProviderIgnoreCli.h>

#include "cxxopts.hpp"

// Configuration options for the PerfettoTraceExporter (emits .perfetto-trace.zip)
struct PerfettoTraceExporterCliOptions
{
    std::string tsfDatabaseDirPath; // Path to TSF (Trace Stream Format) database directory
    bool enableDebugEvents = false;
    bool enableStreamTimelines = false;
    nv::trace::Codec::ProviderTypeTable<bool> isProviderTypeIgnored = {};
    uint64_t startTimeUtcNsec = nv::trace::Codec::InvalidTimestamp;
    uint64_t endTimeUtcNsec = nv::trace::Codec::InvalidTimestamp;
    bool printActivityTimeSpan = false;
    bool printFullTimeSpan = false;
    bool printProcesses = false;
};

using TsfDatabaseReader = nv::trace::TraceStreamDatabaseReader<nv::trace::TsfDiskDatabaseBufferedStorageReader>;
using TsfDataModel = nv::trace::Codec::DataModel<TsfDatabaseReader>;

constexpr uintmax_t kLargeExportSizeBytes = uintmax_t{1} << 30;

struct UtcTimespan
{
    uint64_t startUtcNsec = nv::trace::Codec::InvalidTimestamp;
    uint64_t endUtcNsec = nv::trace::Codec::InvalidTimestamp;
};

struct ProcessTimespan
{
    NVTRACE_LUID processLuid = NVTRACE_NIL_LUID;
    uint64_t startUtcNsec = nv::trace::Codec::InvalidTimestamp;
    uint64_t endUtcNsec = nv::trace::Codec::InvalidTimestamp;
};

// Create common trace exporter options from command-line options
static nv::trace::PerfettoTraceExporterOptions CreatePerfettoTraceExporterOptions(const PerfettoTraceExporterCliOptions& options)
{
    nv::trace::PerfettoTraceExporterOptions traceExporterOptions = {};
    traceExporterOptions.enableDebugEvents = options.enableDebugEvents;
    traceExporterOptions.enableStreamTimelines = options.enableStreamTimelines;
    return traceExporterOptions;
}

template <typename TDataModelBuilder>
static void SetProviderIngestFilter(
    typename TDataModelBuilder::Options& dataModelBuilderOptions,
    const nv::trace::Codec::ProviderTypeTable<bool>& isProviderTypeIgnored)
{
    dataModelBuilderOptions.fnShouldIngestProvider = [isProviderTypeIgnored](const typename TDataModelBuilder::ProviderInfo& providerInfo) {
        const size_t providerTypeIndex = static_cast<size_t>(providerInfo.providerType);
        return providerTypeIndex >= isProviderTypeIgnored.size() || !isProviderTypeIgnored[providerTypeIndex];
    };
}
// Export TraceStreamDatabaseReader to Perfetto protobuf trace format
template <typename TStorageReader>
static bool ExportToPerfetto(nv::trace::TraceStreamDatabaseReader<TStorageReader>& source, const std::filesystem::path& outputPath, const PerfettoTraceExporterCliOptions& options)
{
    std::cout << "Exporting to Perfetto trace: " << outputPath.string() << '\n';

    using DatabaseReaderType = nv::trace::TraceStreamDatabaseReader<TStorageReader>;
    using DataModelBuilder = nv::trace::Codec::DataModelBuilder<DatabaseReaderType>;
    typename DataModelBuilder::Options dataModelBuilderOptions;
    dataModelBuilderOptions.observationStartUtcNsec = options.startTimeUtcNsec;
    dataModelBuilderOptions.observationEndUtcNsec = options.endTimeUtcNsec;
    SetProviderIngestFilter<DataModelBuilder>(dataModelBuilderOptions, options.isProviderTypeIgnored);

    nv::trace::Codec::DataModel<DatabaseReaderType> dataModel;
    DataModelBuilder dataModelBuilder(dataModel);
    dataModelBuilder.Build(source, dataModelBuilderOptions);

    auto traceExporterOptions = CreatePerfettoTraceExporterOptions(options);
    nv::trace::PerfettoTraceExporter<DatabaseReaderType> perfettoTraceExporter(dataModel, traceExporterOptions);
    perfettoTraceExporter.Export(outputPath);

    std::cout << "Successfully exported Perfetto trace to: " << outputPath.string() << '\n';
    return true;
}

static void PrintUtcTimespan(const char* label, uint64_t lowerUtcNsec, uint64_t upperUtcNsec)
{
    if (lowerUtcNsec == nv::trace::Codec::InvalidTimestamp || upperUtcNsec == nv::trace::Codec::InvalidTimestamp)
    {
        std::cout << label << " timespan: unavailable\n";
        return;
    }

    constexpr uint64_t NSEC_PER_MSEC = 1'000'000;

    std::cout << label << " start UTC: " << lowerUtcNsec / NSEC_PER_MSEC << " ms\n";
    std::cout << label << " end UTC:   " << upperUtcNsec / NSEC_PER_MSEC << " ms\n";
    if (upperUtcNsec >= lowerUtcNsec)
    {
        std::cout << label << " duration:  " << (upperUtcNsec - lowerUtcNsec) / NSEC_PER_MSEC << " ms\n";
    }
}

static bool GetTsfFullTimespan(TsfDatabaseReader& databaseReader, UtcTimespan& timespan)
{
    const nv::trace::TsfDataBlockRange dataBlockRange =
        databaseReader.GetHistoryDataBlockRange(databaseReader.GetDataBlockRange());
    if (dataBlockRange.Empty())
    {
        std::cerr << "Error: TSF database contains no history DataBlocks.\n";
        return false;
    }

    if (!databaseReader.GetDataBlockRangeTimeBoundsUtcNsec(
            dataBlockRange, timespan.startUtcNsec, timespan.endUtcNsec))
    {
        std::cerr << "Error: Failed to read the TSF history time bounds.\n";
        return false;
    }
    return true;
}

static TsfDataModel BuildDataBlockDataModel(
    TsfDatabaseReader& databaseReader,
    size_t beginDataBlockIndex,
    size_t endDataBlockIndex,
    uint64_t observationStartUtcNsec,
    uint64_t observationEndUtcNsec)
{
    TsfDataModel dataModel;
    nv::trace::Codec::DataModelBuilder<TsfDatabaseReader>::Options dataModelBuilderOptions;
    dataModelBuilderOptions.dataBlockRange = {beginDataBlockIndex, endDataBlockIndex};
    dataModelBuilderOptions.observationStartUtcNsec = observationStartUtcNsec;
    dataModelBuilderOptions.observationEndUtcNsec = observationEndUtcNsec;
    dataModelBuilderOptions.forceComputeObservedTimestampRange = true;

    nv::trace::Codec::DataModelBuilder<TsfDatabaseReader> dataModelBuilder(dataModel);
    dataModelBuilder.Build(databaseReader, dataModelBuilderOptions);
    return dataModel;
}

static bool GetTsfActivityTimespan(TsfDatabaseReader& databaseReader, const UtcTimespan& fullTimespan, UtcTimespan& timespan)
{
    const nv::trace::TsfDataBlockRange historyDataBlockRange =
        databaseReader.GetHistoryDataBlockRange(databaseReader.GetDataBlockRange());
    if (databaseReader.IsSnapshotDatabase())
    {
        const TsfDataModel dataModel = BuildDataBlockDataModel(
            databaseReader,
            historyDataBlockRange.beginDataBlockIndex,
            historyDataBlockRange.endDataBlockIndex,
            fullTimespan.startUtcNsec,
            fullTimespan.endUtcNsec);
        if (dataModel.m_timestampStartTsc == nv::trace::Codec::MaxTimestampTsc
            || dataModel.m_timestampEndTsc == nv::trace::Codec::MinTimestampTsc)
        {
            return true;
        }

        timespan.startUtcNsec = dataModel.ConvertTscTickToUtcNsec(dataModel.m_timestampStartTsc);
        timespan.endUtcNsec = dataModel.ConvertTscTickToUtcNsec(dataModel.m_timestampEndTsc);
        if (timespan.startUtcNsec == nv::trace::Codec::InvalidTimestamp
            || timespan.endUtcNsec == nv::trace::Codec::InvalidTimestamp)
        {
            std::cerr << "Error: Failed to convert the activity time span to UTC.\n";
            return false;
        }
        timespan.startUtcNsec = std::max(timespan.startUtcNsec, fullTimespan.startUtcNsec);
        timespan.endUtcNsec = std::min(timespan.endUtcNsec, fullTimespan.endUtcNsec);
        if (timespan.endUtcNsec < timespan.startUtcNsec)
        {
            timespan = {};
        }
        return true;
    }

    using CpuCalibrationPoint = nv::trace::TscRtcCalibrationPoint;

    uint64_t lowerTimestampTsc = nv::trace::Codec::MaxTimestampTsc;
    uint64_t upperTimestampTsc = nv::trace::Codec::MinTimestampTsc;
    bool foundLowerTimestamp = false;
    bool foundUpperTimestamp = false;
    CpuCalibrationPoint lowerCalibrationPoint = {};
    CpuCalibrationPoint upperCalibrationPoint = {};

    for (size_t dataBlockIndex = historyDataBlockRange.beginDataBlockIndex;
         dataBlockIndex < historyDataBlockRange.endDataBlockIndex;
         ++dataBlockIndex)
    {
        TsfDataModel dataModel = BuildDataBlockDataModel(databaseReader, dataBlockIndex, dataBlockIndex + 1, fullTimespan.startUtcNsec, nv::trace::Codec::InvalidTimestamp);
        const auto& calibrationPoints = dataModel.cpuClockCalibration.calibrationPoints;

        if (foundLowerTimestamp)
        {
            if (!calibrationPoints.empty())
            {
                lowerCalibrationPoint = calibrationPoints.front();
                break;
            }
            continue;
        }

        if (!calibrationPoints.empty())
        {
            lowerCalibrationPoint = calibrationPoints.back();
        }

        if (dataModel.m_timestampStartTsc != nv::trace::Codec::MaxTimestampTsc)
        {
            lowerTimestampTsc = dataModel.m_timestampStartTsc;
            foundLowerTimestamp = true;
            if (!calibrationPoints.empty())
            {
                lowerCalibrationPoint = calibrationPoints.front();
            }
            // Check if we already found a valid calibration point
            if (lowerCalibrationPoint.tscTick != nv::trace::Codec::InvalidTimestamp)
            {
                break;
            }
        }
    }

    if (!foundLowerTimestamp)
    {
        return true;
    }
    if (lowerCalibrationPoint.tscTick == nv::trace::Codec::InvalidTimestamp)
    {
        std::cerr << "Error: Failed to find a CPU clock calibration point.\n";
        return false;
    }

    for (size_t dataBlockIndex = historyDataBlockRange.endDataBlockIndex;
         dataBlockIndex > historyDataBlockRange.beginDataBlockIndex;)
    {
        --dataBlockIndex;
        TsfDataModel dataModel = BuildDataBlockDataModel(databaseReader, dataBlockIndex, dataBlockIndex + 1, nv::trace::Codec::InvalidTimestamp, fullTimespan.endUtcNsec);
        const auto& calibrationPoints = dataModel.cpuClockCalibration.calibrationPoints;

        if (foundUpperTimestamp)
        {
            if (!calibrationPoints.empty())
            {
                upperCalibrationPoint = calibrationPoints.back();
                break;
            }
            continue;
        }

        if (dataModel.m_timestampEndTsc == nv::trace::Codec::MinTimestampTsc)
        {
            if (!calibrationPoints.empty())
            {
                upperCalibrationPoint = calibrationPoints.front();
            }
            continue;
        }

        upperTimestampTsc = dataModel.m_timestampEndTsc;
        foundUpperTimestamp = true;
        if (!calibrationPoints.empty())
        {
            upperCalibrationPoint = calibrationPoints.back();
        }

        // Check the upper timestamp including the next data block in case a correlated HES event is present
        if (dataBlockIndex + 1 < historyDataBlockRange.endDataBlockIndex)
        {
            dataModel = BuildDataBlockDataModel(databaseReader, dataBlockIndex, dataBlockIndex + 2, nv::trace::Codec::InvalidTimestamp, fullTimespan.endUtcNsec);
            if (dataModel.m_timestampEndTsc != nv::trace::Codec::MinTimestampTsc)
            {
                upperTimestampTsc = std::max(upperTimestampTsc, dataModel.m_timestampEndTsc);
            }
        }

        // Check if we already found a valid calibration point
        if (upperCalibrationPoint.tscTick != nv::trace::Codec::InvalidTimestamp)
        {
            break;
        }
    }

    timespan.startUtcNsec = std::max(lowerCalibrationPoint.ToUtc(lowerTimestampTsc), fullTimespan.startUtcNsec);
    timespan.endUtcNsec = std::min(upperCalibrationPoint.ToUtc(upperTimestampTsc), fullTimespan.endUtcNsec);
    if (timespan.endUtcNsec < timespan.startUtcNsec)
    {
        timespan = {};
    }
    return true;
}

static void PrintTsfProcessTimespans(TsfDatabaseReader& databaseReader, const UtcTimespan& fullTimespan, std::vector<ProcessTimespan>& processTimespans)
{
    using DataModelBuilder = nv::trace::Codec::DataModelBuilder<TsfDatabaseReader>;

    // Build data model with only processes and CPU clock calibration points
    TsfDataModel dataModel;
    DataModelBuilder::Options dataModelBuilderOptions;
    dataModelBuilderOptions.observationStartUtcNsec = fullTimespan.startUtcNsec;
    dataModelBuilderOptions.observationEndUtcNsec = fullTimespan.endUtcNsec;
    dataModelBuilderOptions.fnShouldIngestProvider = [](const DataModelBuilder::ProviderInfo& providerInfo) {
        return providerInfo.providerType == nv::trace::Codec::ProviderType::CpuClockCalibration;
    };

    DataModelBuilder dataModelBuilder(dataModel);
    dataModelBuilder.Build(databaseReader, dataModelBuilderOptions);
    if (dataModel.processes.empty())
    {
        std::cout << "No processes were found in this TSF.\n";
        return;
    }

    // Analyze the process timespans
    constexpr uint64_t NSEC_PER_MSEC = 1'000'000;

    for (const auto& [processLuid, processData] : dataModel.processes)
    {
        const uint64_t startUtcNsec = processData.creationTimestampTsc != nv::trace::Codec::MinTimestampTsc ? dataModel.ConvertTscTickToUtcNsec(processData.creationTimestampTsc) : nv::trace::Codec::InvalidTimestamp;
        const uint64_t endUtcNsec = processData.destructionTimestampTsc != nv::trace::Codec::MaxTimestampTsc ? dataModel.ConvertTscTickToUtcNsec(processData.destructionTimestampTsc) : nv::trace::Codec::InvalidTimestamp;
        processTimespans.push_back({processLuid, startUtcNsec, endUtcNsec});

        std::cout << "Process LUID:        " << processLuid << '\n';
        std::cout << "Namespace PID:      " << processData.namespacePid << '\n';
        std::cout << "Global PID:         " << processData.globalPid << '\n';
        std::cout << "Path:               " << processData.path << '\n';

        if (startUtcNsec == nv::trace::Codec::InvalidTimestamp)
        {
            std::cout << "Process start UTC:   unavailable\n";
        }
        else
        {
            std::cout << "Process start UTC:   " << startUtcNsec / NSEC_PER_MSEC << " ms";
            if (processData.creationTimestampTsc == dataModel.m_observationStartTsc)
            {
                std::cout << " (trace start)";
            }
            std::cout << '\n';
        }

        if (endUtcNsec == nv::trace::Codec::InvalidTimestamp)
        {
            std::cout << "Process end UTC:     unavailable\n";
        }
        else
        {
            std::cout << "Process end UTC:     " << endUtcNsec / NSEC_PER_MSEC << " ms";
            if (processData.destructionTimestampTsc == dataModel.m_observationEndTsc)
            {
                std::cout << " (trace end)";
            }
            std::cout << '\n';
        }

        if (startUtcNsec != nv::trace::Codec::InvalidTimestamp && endUtcNsec >= startUtcNsec)
        {
            std::cout << "Process duration:    " << (endUtcNsec - startUtcNsec) / NSEC_PER_MSEC << " ms\n";
        }
        else
        {
            std::cout << "Process duration:    unavailable\n";
        }
        std::cout << '\n';
    }
}

static std::filesystem::path CreateTsfOutputPath(const std::filesystem::path& tsfDatabaseDirPath, const std::string& suffix = {})
{
    std::filesystem::path outputPath = tsfDatabaseDirPath.filename().empty() ? tsfDatabaseDirPath.parent_path() : tsfDatabaseDirPath;
    if (!suffix.empty())
    {
        outputPath += "." + suffix;
    }
    outputPath += ".perfetto-trace.zip";
    return outputPath;
}

static bool PromptForConfirmation(const char* prompt)
{
    std::cout << prompt << " (y/[n]): " << std::flush;
    std::string response;
    if (!std::getline(std::cin, response))
    {
        return false;
    }
    std::cout << '\n';
    return !response.empty() && (response.front() == 'y' || response.front() == 'Y');
}

static bool GetTsfSizeBytes(
    const TsfDatabaseReader& databaseReader,
    const nv::trace::TsfDataBlockRange& selectedDataBlockRange,
    uintmax_t& selectedSizeBytes,
    size_t& readDataBlockCount,
    uintmax_t& readSizeBytes)
{
    const uintmax_t dataBlockSizeBytes = databaseReader.DataBlockSize();
    if (dataBlockSizeBytes == 0)
    {
        return false;
    }

    readDataBlockCount = selectedDataBlockRange.Size();
    if (databaseReader.IsSnapshotDatabase())
    {
        const nv::trace::TsfDataBlockRange selectedHistoryDataBlockRange =
            databaseReader.GetHistoryDataBlockRange(selectedDataBlockRange);
        if (!selectedHistoryDataBlockRange.Empty())
        {
            const nv::trace::TsfDataBlockRange availableDataBlockRange =
                databaseReader.GetDataBlockRange();
            const nv::trace::TsfDataBlockRange availableHistoryDataBlockRange =
                databaseReader.GetHistoryDataBlockRange(availableDataBlockRange);
            const size_t attachDataBlockCount =
                availableHistoryDataBlockRange.beginDataBlockIndex
                - availableDataBlockRange.beginDataBlockIndex;
            readDataBlockCount =
                attachDataBlockCount
                + availableDataBlockRange.endDataBlockIndex
                - selectedHistoryDataBlockRange.beginDataBlockIndex;
        }
    }

    selectedSizeBytes = dataBlockSizeBytes * selectedDataBlockRange.Size();
    readSizeBytes = dataBlockSizeBytes * readDataBlockCount;
    return true;
}

static bool PromptAndExportTimespan(TsfDatabaseReader& databaseReader, const std::filesystem::path& tsfDatabaseDirPath, const PerfettoTraceExporterCliOptions& options, const UtcTimespan& timespan, const std::string& outputSuffix)
{
    if (timespan.startUtcNsec == nv::trace::Codec::InvalidTimestamp || timespan.endUtcNsec == nv::trace::Codec::InvalidTimestamp)
    {
        return true;
    }

    nv::trace::TsfDataBlockRange dataBlockRange;
    if (!databaseReader.FindDataBlocksInTimespan(timespan.startUtcNsec, timespan.endUtcNsec, dataBlockRange))
    {
        std::cerr << "Error: Failed to find the TSF DataBlocks for the selected range.\n";
        return false;
    }

    uintmax_t selectedSizeBytes = 0;
    size_t readDataBlockCount = 0;
    uintmax_t readSizeBytes = 0;
    if (!GetTsfSizeBytes(
            databaseReader,
            dataBlockRange,
            selectedSizeBytes,
            readDataBlockCount,
            readSizeBytes))
    {
        std::cerr << "Error: Failed to determine the size of the TSF DataBlocks to read.\n";
        return false;
    }
    std::cout << "TSF data required to be read: " << readSizeBytes / (uintmax_t{1} << 20) << " MiB across "
              << readDataBlockCount << " DataBlocks\n";
    if (readSizeBytes >= kLargeExportSizeBytes)
    {
        std::cout << "Warning: Exporting this range may use a large amount of memory.\n";
    }

    if (!PromptForConfirmation("Do you want to export this range?"))
    {
        return true;
    }

    PerfettoTraceExporterCliOptions exportOptions = options;
    exportOptions.startTimeUtcNsec = timespan.startUtcNsec;
    exportOptions.endTimeUtcNsec = timespan.endUtcNsec;
    return ExportToPerfetto(databaseReader, CreateTsfOutputPath(tsfDatabaseDirPath, outputSuffix), exportOptions);
}

static bool ParseProcessLuids(const std::string& input, std::vector<NVTRACE_LUID>& processLuids)
{
    const char* pCurrent = input.data();
    const char* const pEnd = pCurrent + input.size();
    while (pCurrent < pEnd)
    {
        // Advance to the next non-space character
        while (pCurrent < pEnd && (*pCurrent == ' ' || *pCurrent == '\t'))
        {
            ++pCurrent;
        }
        if (pCurrent == pEnd)
        {
            break;
        }
        // Parse the process LUID
        NVTRACE_LUID processLuid = NVTRACE_NIL_LUID;
        const auto result = std::from_chars(pCurrent, pEnd, processLuid);
        if (result.ec != std::errc{})
        {
            return false;
        }
        pCurrent = result.ptr;
        // Advance to the next non-space character
        while (pCurrent < pEnd && (*pCurrent == ' ' || *pCurrent == '\t'))
        {
            ++pCurrent;
        }
        // Check if the next character is a comma
        if (pCurrent < pEnd)
        {
            if (*pCurrent != ',')
            {
                return false;
            }
            ++pCurrent;
        }
        processLuids.push_back(processLuid);
    }
    if (processLuids.empty())
    {
        return false;
    }

    std::sort(processLuids.begin(), processLuids.end());
    processLuids.erase(std::unique(processLuids.begin(), processLuids.end()), processLuids.end());
    return true;
}

static bool PromptAndExportProcesses(TsfDatabaseReader& databaseReader, const std::filesystem::path& tsfDatabaseDirPath, const PerfettoTraceExporterCliOptions& options, const std::vector<ProcessTimespan>& processTimespans, const UtcTimespan& fullTimespan)
{
    if (processTimespans.empty())
    {
        return true;
    }

    while (true)
    {
        std::cout << "Enter process LUIDs to export (comma-separated, blank to finish): " << std::flush;
        std::string input;
        if (!std::getline(std::cin, input) || input.empty())
        {
            return true;
        }

        std::vector<NVTRACE_LUID> selectedProcessLuids;
        if (!ParseProcessLuids(input, selectedProcessLuids))
        {
            std::cout << "Invalid process list. Enter decimal LUIDs separated by commas.\n";
            continue;
        }

        UtcTimespan selectedTimespan = {};
        selectedTimespan.startUtcNsec = UINT64_MAX;
        selectedTimespan.endUtcNsec = 0;
        bool allProcessesFound = true;
        auto processItr = processTimespans.begin();
        for (const NVTRACE_LUID processLuid : selectedProcessLuids)
        {
            processItr = std::lower_bound(processItr, processTimespans.end(), processLuid, [](const ProcessTimespan& processTimespan, NVTRACE_LUID luid) {
                return processTimespan.processLuid < luid;
            });
            if (processItr == processTimespans.end() || processItr->processLuid != processLuid)
            {
                std::cout << "Process LUID " << processLuid << " was not found.\n";
                allProcessesFound = false;
                break;
            }

            const uint64_t processStartUtcNsec = processItr->startUtcNsec != nv::trace::Codec::InvalidTimestamp ? processItr->startUtcNsec : fullTimespan.startUtcNsec;
            const uint64_t processEndUtcNsec = processItr->endUtcNsec != nv::trace::Codec::InvalidTimestamp ? processItr->endUtcNsec : fullTimespan.endUtcNsec;
            selectedTimespan.startUtcNsec = std::min(selectedTimespan.startUtcNsec, processStartUtcNsec);
            selectedTimespan.endUtcNsec = std::max(selectedTimespan.endUtcNsec, processEndUtcNsec);
        }
        if (!allProcessesFound)
        {
            std::cout << "Not all requested processes were found. Please try again.\n";
            continue;
        }

        PrintUtcTimespan("Selected process range", selectedTimespan.startUtcNsec, selectedTimespan.endUtcNsec);
        std::string outputSuffix = "processes";
        for (const NVTRACE_LUID processLuid : selectedProcessLuids)
        {
            outputSuffix += "-" + std::to_string(processLuid);
        }
        if (!PromptAndExportTimespan(databaseReader, tsfDatabaseDirPath, options, selectedTimespan, outputSuffix))
        {
            return false;
        }
    }
}

// Parse command-line arguments and return options
static PerfettoTraceExporterCliOptions ParsePerfettoTraceExporterOptions(int argc, char** argv)
{
    PerfettoTraceExporterCliOptions opts;

    cxxopts::Options options("nvtrace_perfetto_trace_exporter", "Convert a TSF database directory to a Perfetto trace archive (.perfetto-trace.zip). ");
    options.positional_help("tsf-database-dir");
    // clang-format off
     options.add_options()
         ("tsf-database-dir", "Path to the TSF database directory to export", cxxopts::value<std::string>())
         ("enable-debug-events", "Enable debug events (calibration, provider diagnostics, etc.)", cxxopts::value<bool>()->default_value("false"))
         ("enable-stream-timelines", "Enable per-stream CUDA event timelines", cxxopts::value<bool>()->default_value("false"))
         ("ignore", "Ignore provider data. Ignorable provider types: gpu_pm, gpu_hwt, cuda_api, nvtx. Ex: --ignore gpu_pm,gpu_hwt", cxxopts::value<std::vector<std::string>>(), "provider_type")
         ("start-time-utc-ms", "Start time in UTC milliseconds", cxxopts::value<uint64_t>()->default_value("0"))
         ("end-time-utc-ms", "End time in UTC milliseconds", cxxopts::value<uint64_t>()->default_value("0"))
         ("activity-timespan", "Show the CUDA, NVTX, and kernel activity time span and optionally export it")
         ("full-timespan", "Show the full TSF recording time span and optionally export it")
         ("processes", "Show process lifetimes and optionally export a comma-separated list of process LUIDs")
         ("h,help", "Print usage");
    // clang-format on
    options.parse_positional({"tsf-database-dir"});
    auto result = options.parse(argc, argv);

    if (result.count("help"))
    {
        std::cout << options.help() << '\n';
        exit(EXIT_SUCCESS);
    }

    if (!result.count("tsf-database-dir"))
    {
        std::cerr << "Error: Must specify --tsf-database-dir.\n";
        std::cout << options.help() << '\n';
        exit(EXIT_FAILURE);
    }

    opts.printProcesses = result.count("processes");
    opts.printActivityTimeSpan = result.count("activity-timespan");
    opts.printFullTimeSpan = result.count("full-timespan");
    opts.tsfDatabaseDirPath = result["tsf-database-dir"].as<std::string>();
    opts.enableDebugEvents = result["enable-debug-events"].as<bool>();
    opts.enableStreamTimelines = result["enable-stream-timelines"].as<bool>();

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

    opts.startTimeUtcNsec = result["start-time-utc-ms"].as<uint64_t>() * 1'000'000;
    opts.endTimeUtcNsec = result["end-time-utc-ms"].as<uint64_t>() * 1'000'000;

    return opts;
}

// Process TSF database directory and export to Perfetto protobuf trace format
static int ProcessTsfDatabaseDir(const PerfettoTraceExporterCliOptions& options)
{
    std::filesystem::path tsfDatabaseDirPath(options.tsfDatabaseDirPath);

    // Validate database directory exists
    if (!std::filesystem::exists(tsfDatabaseDirPath))
    {
        std::cerr << "Error: TSF database directory does not exist: " << tsfDatabaseDirPath.string() << '\n';
        return EXIT_FAILURE;
    }

    // Initialize database reader
    std::cout << "Reading TSF database directory at: " << tsfDatabaseDirPath.string() << '\n';

    TsfDatabaseReader databaseReader;
    auto initializeStorage = [&](nv::trace::TsfDiskDatabaseBufferedStorageReader& storageReader) {
        return storageReader.Initialize(tsfDatabaseDirPath);
    };
    const bool databaseInitialized = databaseReader.Initialize(initializeStorage);
    if (!databaseInitialized)
    {
        std::cerr << "Error: Failed to initialize database reader: " << tsfDatabaseDirPath.string() << '\n';
        return EXIT_FAILURE;
    }

    UtcTimespan fullTimespan;
    if (!GetTsfFullTimespan(databaseReader, fullTimespan))
    {
        return EXIT_FAILURE;
    }

    if (options.printActivityTimeSpan || options.printFullTimeSpan || options.printProcesses)
    {
        if (options.printFullTimeSpan)
        {
            PrintUtcTimespan("Full trace", fullTimespan.startUtcNsec, fullTimespan.endUtcNsec);
            if (!PromptAndExportTimespan(databaseReader, tsfDatabaseDirPath, options, fullTimespan, "full"))
            {
                return EXIT_FAILURE;
            }
        }

        if (options.printActivityTimeSpan)
        {
            UtcTimespan activityTimespan;
            if (!GetTsfActivityTimespan(databaseReader, fullTimespan, activityTimespan))
            {
                return EXIT_FAILURE;
            }
            PrintUtcTimespan("Activity", activityTimespan.startUtcNsec, activityTimespan.endUtcNsec);
            if (!PromptAndExportTimespan(databaseReader, tsfDatabaseDirPath, options, activityTimespan, "activity"))
            {
                return EXIT_FAILURE;
            }
        }
        if (options.printProcesses)
        {
            std::vector<ProcessTimespan> processTimespans;
            PrintTsfProcessTimespans(databaseReader, fullTimespan, processTimespans);
            if (!PromptAndExportProcesses(databaseReader, tsfDatabaseDirPath, options, processTimespans, fullTimespan))
            {
                return EXIT_FAILURE;
            }
        }

        return EXIT_SUCCESS;
    }

    // Generate output path and export
    PerfettoTraceExporterCliOptions exportOptions = options;
    if (exportOptions.startTimeUtcNsec == nv::trace::Codec::InvalidTimestamp)
    {
        exportOptions.startTimeUtcNsec = fullTimespan.startUtcNsec;
    }
    if (exportOptions.endTimeUtcNsec == nv::trace::Codec::InvalidTimestamp)
    {
        exportOptions.endTimeUtcNsec = fullTimespan.endUtcNsec;
    }
    ExportToPerfetto(databaseReader, CreateTsfOutputPath(tsfDatabaseDirPath), exportOptions);

    return EXIT_SUCCESS;
}

int main(int argc, char** argv)
{
    try
    {
        auto options = ParsePerfettoTraceExporterOptions(argc, argv);

        if (options.startTimeUtcNsec == nv::trace::Codec::InvalidTimestamp && options.endTimeUtcNsec == nv::trace::Codec::InvalidTimestamp && !options.printActivityTimeSpan && !options.printFullTimeSpan && !options.printProcesses)
        {
            std::cerr << "Warning: Export time window is not specified (--start-time-utc-ms and/or --end-time-utc-ms). "
                      << "Exporting the full trace may run out of memory on large traces. "
                      << "To see the process lifetimes in the trace, use the --processes option.\n";
        }

        return ProcessTsfDatabaseDir(options);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
