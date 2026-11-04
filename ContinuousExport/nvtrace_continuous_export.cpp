/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#endif

#include <NvTrace/Offline/DataModel/NvTraceStreamDataModelBuilder.h>
#include <NvTrace/Offline/DataModel/NvTraceStreamDatabaseReader.h>
#include <NvTrace/Offline/Exporter/NvTraceDuckDbTableSink.h>
#include <NvTrace/Offline/Exporter/NvTracePerfettoTraceExporter.h>
#include <NvTrace/Offline/Exporter/NvTraceTableExporter.h>
#include <NvTrace/Target/Consumer/NvTraceContinuousSnapshotExporter.h>
#include <NvTrace/Target/NvTraceScopeExitGuard.h>
#include <NvTraceProviderIgnoreCli.h>

#include "cxxopts.hpp"

namespace {

    constexpr uint64_t bytesPerMiB = 1024ULL * 1024ULL;
    constexpr uint64_t nsecPerMsec = 1'000'000ULL;
    constexpr uint64_t unlimitedDurationMsec = static_cast<uint64_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max());
    constexpr uint64_t recommendedMinimumSnapshotMsec = 5000;

    struct Options
    {
        uint64_t snapshotMsec = 5000;
        uint64_t dataRateMiBPerSec = 12;
        uint32_t dataBlockSizeMiB = 10;
        uint32_t latencyBufferDataBlockCount = 8;
        uint64_t maxAttachSizeMiB = 100;
        uint64_t maxRetainedShmMiB = 2048;
        size_t dbFlushThresholdBytes = nv::trace::DuckDbTableSink::DefaultUpsertFlushThresholdBytes;
        size_t dbMemoryLimitBytes = 0;
        uint64_t durationMsec = unlimitedDurationMsec;
        bool stopOnEnterKey = true;
        bool keepWorkFiles = false;
        bool exportPerfettoTrace = false;
        bool enableDebugEvents = false;
        bool showDataRate = false;
        bool showSchemaOnly = false;
        std::filesystem::path outputDirectory;
        std::filesystem::path databasePath;
        nv::trace::Codec::ProviderTypeTable<bool> isProviderTypeIgnored = {};
    };

    class ManifestStorageReader
    {
    public:
        class DataBlockStorageReader
        {
        public:
            void Reset() noexcept
            {
                m_file.close();
            }

            bool Initialize(const std::filesystem::path& filePath)
            {
                m_file.open(filePath, std::ios::binary);
                return !m_file.fail();
            }

            bool ReadSection(void* pBuffer, size_t sectionBeginOffset, size_t sectionEndOffset)
            {
                m_file.seekg(static_cast<std::streamoff>(sectionBeginOffset), std::ios::beg);
                m_file.read(static_cast<char*>(pBuffer) + sectionBeginOffset, static_cast<std::streamsize>(sectionEndOffset - sectionBeginOffset));
                return !m_file.fail();
            }

        private:
            std::ifstream m_file;
        };

        void Reset() noexcept
        {
            m_dataBlockFiles.clear();
        }

        bool Initialize(const nv::trace::ContinuousSnapshotManifest& manifest)
        {
            Reset();
            m_dataBlockFiles = manifest.dataBlockFiles;
            std::sort(m_dataBlockFiles.begin(), m_dataBlockFiles.end(), [](const nv::trace::ContinuousSnapshotDataBlockFile& lhs, const nv::trace::ContinuousSnapshotDataBlockFile& rhs) {
                return lhs.dataBlockSequenceNumber < rhs.dataBlockSequenceNumber;
            });
            return !m_dataBlockFiles.empty();
        }

        size_t DataBlockCount() const
        {
            return m_dataBlockFiles.size();
        }

        uint64_t GetDataBlockSequenceNumber(size_t dataBlockIndex) const
        {
            return m_dataBlockFiles[dataBlockIndex].dataBlockSequenceNumber;
        }

        size_t FindDataBlockIndexBySequenceNumber(uint64_t dataBlockSequenceNumber) const
        {
            const auto itr = std::lower_bound(
                m_dataBlockFiles.begin(),
                m_dataBlockFiles.end(),
                dataBlockSequenceNumber,
                [](const nv::trace::ContinuousSnapshotDataBlockFile& lhs, uint64_t rhs) {
                    return lhs.dataBlockSequenceNumber < rhs;
                });
            return itr != m_dataBlockFiles.end() && itr->dataBlockSequenceNumber == dataBlockSequenceNumber
                     ? static_cast<size_t>(itr - m_dataBlockFiles.begin())
                     : SIZE_MAX;
        }

        bool OpenDataBlock(size_t dataBlockIndex, DataBlockStorageReader& dataBlockStorageReader)
        {
            return dataBlockStorageReader.Initialize(m_dataBlockFiles[dataBlockIndex].filePath);
        }

    private:
        std::vector<nv::trace::ContinuousSnapshotDataBlockFile> m_dataBlockFiles;
    };

    using SnapshotDatabaseReader = nv::trace::TraceStreamDatabaseReader<ManifestStorageReader>;
    using SnapshotDataModel = nv::trace::Codec::DataModel<SnapshotDatabaseReader>;

    struct EagerGraphStatistics
    {
        uint64_t eagerCount = 0;
        uint64_t graphCount = 0;
        uint64_t eagerBytes = 0;
        uint64_t graphBytes = 0;

        uint64_t Count() const
        {
            return eagerCount + graphCount;
        }

        uint64_t Bytes() const
        {
            return eagerBytes + graphBytes;
        }

        void Accumulate(const EagerGraphStatistics& rhs)
        {
            eagerCount += rhs.eagerCount;
            graphCount += rhs.graphCount;
            eagerBytes += rhs.eagerBytes;
            graphBytes += rhs.graphBytes;
        }
    };

    struct WorkloadStatistics
    {
        EagerGraphStatistics kernelLaunches;
        EagerGraphStatistics memcpyCalls;
        EagerGraphStatistics memsetCalls;
        EagerGraphStatistics hostCallbacks;
        uint64_t graphLaunches = 0;
        uint64_t graphNodesCreated = 0;
        uint64_t modulesLoaded = 0;
        uint64_t functionsCreated = 0;
        uint64_t nvtxPushCalls = 0;

        void Accumulate(const WorkloadStatistics& rhs)
        {
            kernelLaunches.Accumulate(rhs.kernelLaunches);
            memcpyCalls.Accumulate(rhs.memcpyCalls);
            memsetCalls.Accumulate(rhs.memsetCalls);
            hostCallbacks.Accumulate(rhs.hostCallbacks);
            graphLaunches += rhs.graphLaunches;
            graphNodesCreated += rhs.graphNodesCreated;
            modulesLoaded += rhs.modulesLoaded;
            functionsCreated += rhs.functionsCreated;
            nvtxPushCalls += rhs.nvtxPushCalls;
        }
    };

    struct ApplicationState
    {
        struct TraceVolumes
        {
            nv::trace::Codec::ProviderTypeTable<uint64_t> historyBytesByProviderType = {};
            uint64_t totalHistoryBytes = 0;
            uint64_t gpuPmSampleBytes = 0;
            uint64_t attachBytes = 0;
        };

        struct SnapshotStatistics
        {
            TraceVolumes traceVolumes;
            WorkloadStatistics workloads;
            uint64_t observationDurationNsec = 0;
            uint64_t snapshotNumber = 0;
            bool valid = false;
        };

        struct DashboardStatistics
        {
            TraceVolumes traceVolumes;
            WorkloadStatistics workloads;
            uint64_t observationDurationNsec = 0;
            uint64_t completedSnapshotCount = 0;
            uint64_t truncatedSnapshotCount = 0;
            uint64_t backpressuredRecords = 0;
            uint64_t latestConsumeDataDurationNsec = 0;
            uint64_t maximumConsumeDataDurationNsec = 0;
            uint64_t attachDurationNsec = 0;
            uint64_t exportDurationNsec = 0;
            uint64_t perfettoExportDurationNsec = 0;
            uint64_t databaseExportDurationNsec = 0;
            uint64_t latestTotalWorkerDurationNsec = 0;
            uint64_t maximumTotalWorkerDurationNsec = 0;
        };

        std::filesystem::path outputDirectory;
        std::atomic<uint64_t> completedSnapshotCount = 0;
        std::atomic<bool> exportFailed = false;
        // The export thread exclusively owns pendingSnapshotStatistics. It
        // publishes a completed snapshot to dashboardStatistics under one
        // short lock so the UI never observes a partially accumulated result.
        SnapshotStatistics pendingSnapshotStatistics;
        // Owned by the export thread and published with pendingSnapshotStatistics.
        uint64_t pendingPerfettoExportDurationNsec = 0;
        uint64_t pendingDatabaseExportDurationNsec = 0;
        // Owned by the export thread. Keys the run's traces row, so it is fixed by the first exported snapshot.
        uint64_t captureStartUtcNsec = nv::trace::Codec::InvalidTimestamp;
        std::mutex dashboardStatisticsMutex;
        DashboardStatistics dashboardStatistics;
        bool enableDebugEvents = false;
        bool exportPerfettoTrace = true;
        bool showDataRate = false;
        // A snapshot holding at least this many DataBlocks ended because its history bank was nearly full.
        size_t fullHistoryBankDataBlockCount = std::numeric_limits<size_t>::max();
        std::filesystem::path databasePath;
        nv::trace::DuckDbTableSink sink;
    };

    bool ConvertMiBToBytes(uint64_t sizeMiB, size_t& out_sizeBytes)
    {
        if (std::numeric_limits<size_t>::max() / bytesPerMiB < sizeMiB)
        {
            return false;
        }
        out_sizeBytes = static_cast<size_t>(sizeMiB * bytesPerMiB);
        return true;
    }

    bool ParseOptions(int argc, char** argv, Options& out_options)
    {
        cxxopts::Options options(
            "nvtrace_continuous_export",
            "Continuously collects trace snapshots and ingests each one into a working DuckDB database. Exports final .parquet file per table.");

        // clang-format off
        options.add_options()
            ("snapshot-ms", "Snapshot interval in milliseconds. Each snapshot must finish exporting within it, so values below 5000 risk losing data under heavy workloads. This x data-rate sizes each history bank on disk.", cxxopts::value<uint64_t>()->default_value("30000"))
            ("data-rate", "Expected average trace data rate in MiB-per-second", cxxopts::value<uint64_t>()->default_value("12"))
            ("data-block-size", "TSF DataBlock size in MiB", cxxopts::value<uint32_t>()->default_value("10"))
            ("latency-buffer", "Number of in-memory DataBlocks used to absorb storage latency (minimum 4)", cxxopts::value<uint32_t>()->default_value("8"))
            ("max-attach-size", "Maximum attach size in MiB, rounded up to whole DataBlocks", cxxopts::value<uint64_t>()->default_value("100"))
            ("max-retained-shm", "Maximum MiB of provider SHM retained beyond process exit time", cxxopts::value<uint64_t>()->default_value("2048"))
            ("duration-ms", "Maximum recording runtime in milliseconds before automatic stop; defaults to unlimited", cxxopts::value<uint64_t>()->default_value(std::to_string(unlimitedDurationMsec)))
            ("stop-on-enter-key", "Stop when Enter is pressed; compatible with --duration-ms", cxxopts::value<bool>()->default_value("true"))
            ("output-dir", "Parent directory for the per-run directory holding temporary TSF files", cxxopts::value<std::string>()->default_value(""))
            ("db", "DuckDB database file to create or update; defaults to snapshots.duckdb in the per-run directory", cxxopts::value<std::string>()->default_value(""))
            ("db-flush-threshold", "MiB of buffered rows per table before they are upserted into the database; 0 flushes only every 204800 rows", cxxopts::value<uint64_t>()->default_value(std::to_string(nv::trace::DuckDbTableSink::DefaultUpsertFlushThresholdBytes / bytesPerMiB)))
            ("db-memory-limit", "MiB of memory DuckDB may use, including its cache of database pages; 0 keeps DuckDB's default of 80% of physical memory", cxxopts::value<uint64_t>()->default_value("0"))
            ("keep-work-files", "Keep the preallocated temporary TSF files after shutdown", cxxopts::value<bool>()->default_value("false"))
            ("show-data-rate", "Show a live ANSI data-rate and volume meter", cxxopts::value<bool>()->default_value("false"))
            ("perfetto", "Export each completed snapshot as an individual Perfetto trace for debugging.", cxxopts::value<bool>()->default_value("false"))
            ("enable-debug-events", "Include debug events in the placeholder Perfetto export", cxxopts::value<bool>()->default_value("false"))
            ("show-schema-only", "Print the SQL statements that create all tables to stdout and exit", cxxopts::value<bool>()->default_value("false"))
            ("ignore", "Ignore provider data. Ignorable provider types: gpu_pm, gpu_hwt, cuda_api, nvtx. Ex: --ignore gpu_pm,gpu_hwt", cxxopts::value<std::vector<std::string>>(), "provider_type")
            ("h,help", "Print usage");
        // clang-format on

        const auto result = options.parse(argc, argv);
        if (result.count("help"))
        {
            std::cout << options.help() << '\n';
            std::exit(EXIT_SUCCESS);
        }

        out_options.snapshotMsec = result["snapshot-ms"].as<uint64_t>();
        out_options.dataRateMiBPerSec = result["data-rate"].as<uint64_t>();
        out_options.dataBlockSizeMiB = result["data-block-size"].as<uint32_t>();
        out_options.latencyBufferDataBlockCount = result["latency-buffer"].as<uint32_t>();
        out_options.maxAttachSizeMiB = result["max-attach-size"].as<uint64_t>();
        out_options.maxRetainedShmMiB = result["max-retained-shm"].as<uint64_t>();
        out_options.durationMsec = result["duration-ms"].as<uint64_t>();
        out_options.stopOnEnterKey = result["stop-on-enter-key"].as<bool>();
        out_options.outputDirectory = result["output-dir"].as<std::string>();
        out_options.databasePath = result["db"].as<std::string>();
        if (!ConvertMiBToBytes(result["db-flush-threshold"].as<uint64_t>(), out_options.dbFlushThresholdBytes))
        {
            std::cerr << "Error: --db-flush-threshold is too large.\n";
            return false;
        }
        if (!ConvertMiBToBytes(result["db-memory-limit"].as<uint64_t>(), out_options.dbMemoryLimitBytes))
        {
            std::cerr << "Error: --db-memory-limit is too large.\n";
            return false;
        }
        out_options.keepWorkFiles = result["keep-work-files"].as<bool>();
        out_options.exportPerfettoTrace = result["perfetto"].as<bool>();
        out_options.enableDebugEvents = result["enable-debug-events"].as<bool>();
        out_options.showDataRate = result["show-data-rate"].as<bool>();
        out_options.showSchemaOnly = result["show-schema-only"].as<bool>();

        if (result.count("ignore"))
        {
            for (const std::string& providerName : result["ignore"].as<std::vector<std::string>>())
            {
                const nv::trace::Codec::ProviderType providerType = nv::trace::ResolveIgnorableProviderTypeByName(providerName);
                if (nv::trace::Codec::ProviderType::Invalid < providerType && providerType < nv::trace::Codec::ProviderType::Count)
                {
                    out_options.isProviderTypeIgnored[static_cast<size_t>(providerType)] = true;
                }
            }
        }

        if (out_options.snapshotMsec == 0 || std::numeric_limits<uint64_t>::max() / nsecPerMsec < out_options.snapshotMsec)
        {
            std::cerr << "Error: --snapshot-ms must convert to a non-zero nanosecond duration.\n";
            return false;
        }
        if (out_options.dataRateMiBPerSec == 0 || std::numeric_limits<uint64_t>::max() / bytesPerMiB < out_options.dataRateMiBPerSec)
        {
            std::cerr << "Error: --data-rate must convert to a non-zero byte rate.\n";
            return false;
        }
        if (UINT8_MAX < out_options.latencyBufferDataBlockCount)
        {
            std::cerr << "Error: --latency-buffer must not exceed " << static_cast<unsigned int>(UINT8_MAX) << " DataBlocks.\n";
            return false;
        }
        if (static_cast<uint64_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max()) < out_options.durationMsec)
        {
            std::cerr << "Error: --duration-ms is too large.\n";
            return false;
        }
        return true;
    }

    std::filesystem::path CreateRunOutputDirectory(const std::filesystem::path& parentDirectory)
    {
        std::stringstream directoryName;
        directoryName << nv::trace::GetHostName() << ".continuous-snapshots-" << std::hex << nv::trace::ReadCpuTscTimestamp();
        return parentDirectory / directoryName.str();
    }

    std::filesystem::path CreatePerfettoOutputPath(const ApplicationState& state, uint64_t snapshotNumber)
    {
        std::stringstream fileName;
        fileName << "snapshot-" << std::setfill('0') << std::setw(6) << snapshotNumber << ".perfetto-trace.zip";
        return state.outputDirectory / fileName.str();
    }

    // Places each top-level column definition on its own line.
    std::string FormatCreateTableSql(const std::string& sql)
    {
        std::string formatted;
        int depth = 0;
        for (size_t charIndex = 0; charIndex < sql.size(); ++charIndex)
        {
            const char c = sql[charIndex];
            if (c == '(' && depth++ == 0)
            {
                formatted += "(\n    ";
            }
            else if (c == ')' && --depth == 0)
            {
                formatted += "\n)";
            }
            else if (c == ',' && depth == 1 && charIndex + 1 < sql.size() && sql[charIndex + 1] == ' ')
            {
                formatted += ",\n    ";
                ++charIndex;
            }
            else
            {
                formatted += c;
            }
        }
        return formatted;
    }

    bool ExportDuckDbToParquet(
        const std::filesystem::path& databasePath,
        const std::filesystem::path& outputDirectory)
    {
        try
        {
            duckdb::DuckDB database(databasePath.string());
            duckdb::Connection connection(database);
            const std::unique_ptr<duckdb::MaterializedQueryResult> pLoadResult =
                connection.Query("LOAD parquet");
            if (pLoadResult->HasError())
            {
                std::cerr << "Error: failed to load the DuckDB Parquet extension: "
                          << pLoadResult->GetError() << '\n';
                return false;
            }

            const std::string sql = duckdb::StringUtil::Format(
                "EXPORT DATABASE %s (FORMAT PARQUET)",
                duckdb::SQLString(outputDirectory.string()));
            const std::unique_ptr<duckdb::MaterializedQueryResult> pResult = connection.Query(sql);
            if (pResult->HasError())
            {
                std::cerr << "Error: failed to export DuckDB to Parquet: " << pResult->GetError() << '\n';
                return false;
            }
            return true;
        }
        catch (const std::exception& exception)
        {
            std::cerr << "Error: failed to export DuckDB to Parquet: " << exception.what() << '\n';
            return false;
        }
    }

    bool IsTimestampInObservationWindow(uint64_t timestampTsc, const SnapshotDataModel& dataModel)
    {
        return dataModel.m_observationStartTsc <= timestampTsc && timestampTsc < dataModel.m_observationEndTsc;
    }

    template <typename TObjectMap>
    uint64_t CountObjectsCreatedInObservationWindow(const TObjectMap& objects, const SnapshotDataModel& dataModel)
    {
        uint64_t count = 0;
        for (const auto& objectEntry : objects)
        {
            if (IsTimestampInObservationWindow(objectEntry.second.creationTimestampTsc, dataModel))
            {
                count += 1;
            }
        }
        return count;
    }

    bool IsGraphLaunchInObservationWindow(
        const nv::trace::Codec::CudaApiTraceData& cudaApiTraceData,
        const nv::trace::Codec::CudaApiTraceData::GraphLaunch& graphLaunch,
        const SnapshotDataModel& dataModel)
    {
        using CudaApiTraceData = nv::trace::Codec::CudaApiTraceData;
        if (graphLaunch.source == CudaApiTraceData::GraphLaunchSource::Host)
        {
            const CudaApiTraceData::DriverApiCallRef& apiCallRef = graphLaunch.payload.host.driverApiCallRef;
            const auto threadItr = cudaApiTraceData.threads.find(apiCallRef.uniqueThreadId);
            if (threadItr == cudaApiTraceData.threads.end() || threadItr->second.driverApiCalls.size() <= apiCallRef.driverApiCallIdx)
            {
                return false;
            }
            return IsTimestampInObservationWindow(threadItr->second.driverApiCalls[apiCallRef.driverApiCallIdx].startTimestampTsc, dataModel);
        }

        if (graphLaunch.source == CudaApiTraceData::GraphLaunchSource::Device)
        {
            const uint32_t cudaDeviceOrdinal = graphLaunch.payload.device.cudaDeviceOrdinal;
            if (cudaApiTraceData.devices.size() <= cudaDeviceOrdinal)
            {
                return false;
            }
            const nv::trace::Codec::GpuPartitionData* const pGpuPartitionData = dataModel.FindGpuPartitionByGpuUuid(cudaApiTraceData.devices[cudaDeviceOrdinal].gpuUuid);
            if (!pGpuPartitionData)
            {
                return false;
            }
            const uint64_t timestampTsc = SnapshotDataModel::ConvertGpuNsecToTscTick(*pGpuPartitionData, graphLaunch.payload.device.timestampGpuNsec);
            return IsTimestampInObservationWindow(timestampTsc, dataModel);
        }

        return false;
    }

    WorkloadStatistics SummarizeWorkloads(const SnapshotDataModel& dataModel)
    {
        using CudaApiTraceData = nv::trace::Codec::CudaApiTraceData;
        WorkloadStatistics statistics;
        for (const auto& processEntry : dataModel.processes)
        {
            const nv::trace::Codec::ProcessData& processData = processEntry.second;
            const CudaApiTraceData& cudaApiTraceData = processData.cudaApiTrace;

            for (const CudaApiTraceData::CuDeviceData& deviceData : cudaApiTraceData.devices)
            {
                statistics.kernelLaunches.eagerCount += deviceData.immediateKernelLaunches.size();
                statistics.kernelLaunches.graphCount += deviceData.graphNodeLaunches.size();

                for (const CudaApiTraceData::GpuCommand& gpuCommand : deviceData.gpuCommands)
                {
                    switch (gpuCommand.commandType)
                    {
                        case CudaApiTraceData::GpuCommandType::EagerMemcpy:
                        {
                            statistics.memcpyCalls.eagerCount += 1;
                            const auto* const pPayload = dataModel.GetMemcpyBeginPayload(gpuCommand.recordLocation);
                            statistics.memcpyCalls.eagerBytes += pPayload ? pPayload->byteCount : 0;
                            break;
                        }
                        case CudaApiTraceData::GpuCommandType::GraphMemcpy:
                        {
                            statistics.memcpyCalls.graphCount += 1;
                            const CudaApiTraceData::CuGraphNodeData* const pGraphNodeData = dataModel.FindCuGraphNode(cudaApiTraceData, gpuCommand.graphNodeCorrelationId);
                            if (pGraphNodeData)
                            {
                                const NvTrace_CUDA_ApiTrace_GraphNodeType graphNodeType = dataModel.GetCudaApiTraceGraphNodeType(pGraphNodeData->recordLocation, cudaApiTraceData);
                                const nv::trace::CudaApiTraceGraphNodeParams* const pGraphNodeParams = dataModel.GetCudaApiTraceGraphNodeParams(pGraphNodeData->recordLocation, cudaApiTraceData, graphNodeType);
                                if (pGraphNodeParams && graphNodeType == NvTrace_CUDA_ApiTrace_GraphNodeType_Memcpy)
                                {
                                    statistics.memcpyCalls.graphBytes += pGraphNodeParams->memcpy.byteCount;
                                }
                                else if (pGraphNodeParams && graphNodeType == NvTrace_CUDA_ApiTrace_GraphNodeType_MemcpyBatch)
                                {
                                    statistics.memcpyCalls.graphBytes += pGraphNodeParams->memcpyBatch.totalByteCount;
                                }
                            }
                            break;
                        }
                        case CudaApiTraceData::GpuCommandType::EagerMemset:
                        {
                            statistics.memsetCalls.eagerCount += 1;
                            const auto* const pPayload = dataModel.GetMemsetBeginPayload(gpuCommand.recordLocation);
                            statistics.memsetCalls.eagerBytes += pPayload ? pPayload->byteCount : 0;
                            break;
                        }
                        case CudaApiTraceData::GpuCommandType::GraphMemset:
                        {
                            statistics.memsetCalls.graphCount += 1;
                            const CudaApiTraceData::CuGraphNodeData* const pGraphNodeData = dataModel.FindCuGraphNode(cudaApiTraceData, gpuCommand.graphNodeCorrelationId);
                            if (pGraphNodeData)
                            {
                                const NvTrace_CUDA_ApiTrace_GraphNodeType graphNodeType = dataModel.GetCudaApiTraceGraphNodeType(pGraphNodeData->recordLocation, cudaApiTraceData);
                                const nv::trace::CudaApiTraceGraphNodeParams* const pGraphNodeParams = dataModel.GetCudaApiTraceGraphNodeParams(pGraphNodeData->recordLocation, cudaApiTraceData, graphNodeType);
                                if (pGraphNodeParams && graphNodeType == NvTrace_CUDA_ApiTrace_GraphNodeType_Memset)
                                {
                                    statistics.memsetCalls.graphBytes += pGraphNodeParams->memset.byteCount;
                                }
                            }
                            break;
                        }
                        default:
                            break;
                    }
                }
            }

            for (const CudaApiTraceData::HostCallbackInstance& hostCallback : cudaApiTraceData.hostCallbackInstances)
            {
                if (hostCallback.kind == CudaApiTraceData::HostCallbackKind::Eager)
                {
                    statistics.hostCallbacks.eagerCount += 1;
                }
                else
                {
                    statistics.hostCallbacks.graphCount += 1;
                }
            }

            for (const CudaApiTraceData::GraphLaunch& graphLaunch : cudaApiTraceData.graphLaunches)
            {
                if (IsGraphLaunchInObservationWindow(cudaApiTraceData, graphLaunch, dataModel))
                {
                    statistics.graphLaunches += 1;
                }
            }

            statistics.graphNodesCreated += CountObjectsCreatedInObservationWindow(cudaApiTraceData.graphNodes, dataModel);
            statistics.modulesLoaded += CountObjectsCreatedInObservationWindow(cudaApiTraceData.modules, dataModel);
            statistics.functionsCreated += CountObjectsCreatedInObservationWindow(cudaApiTraceData.functions, dataModel);

            for (const nv::trace::Codec::NvtxTraceData::DomainCliqueData& domainCliqueData : processData.nvtxTrace.domainCliques)
            {
                for (const auto& threadEntry : domainCliqueData.threads)
                {
                    for (const nv::trace::Codec::NvtxTraceData::PushPopRangeData& pushPopRange : threadEntry.second.pushPopRanges)
                    {
                        const bool hasApproximateStart = (pushPopRange.approximationMask & nv::trace::Codec::NvtxTraceData::FieldMask_StartTimestamp) != 0;
                        if (!pushPopRange.isSynthetic && !hasApproximateStart && IsTimestampInObservationWindow(pushPopRange.startTimestampTsc, dataModel))
                        {
                            statistics.nvtxPushCalls += 1;
                        }
                    }
                }
            }
        }
        return statistics;
    }

    bool ExportSnapshotToPerfetto(const nv::trace::ContinuousSnapshotManifest& manifest, ApplicationState& state)
    {
        state.pendingSnapshotStatistics = {};
        SnapshotDatabaseReader databaseReader;
        const bool initialized = databaseReader.Initialize([&manifest](ManifestStorageReader& storageReader) {
            return storageReader.Initialize(manifest);
        });
        if (!initialized)
        {
            std::cerr << "Error: failed to initialize snapshot " << manifest.snapshotNumber << " for Perfetto export.\n";
            return false;
        }

        using DataModelBuilder = nv::trace::Codec::DataModelBuilder<SnapshotDatabaseReader>;
        SnapshotDataModel dataModel;
        DataModelBuilder dataModelBuilder(dataModel);
        typename DataModelBuilder::Options builderOptions;
        builderOptions.observationStartUtcNsec = manifest.actualBeginTimestampUtcNsec;
        builderOptions.observationEndUtcNsec = manifest.cutoffTimestampUtcNsec;
        if (!dataModelBuilder.Build(databaseReader, builderOptions))
        {
            std::cerr << "Error: failed to read snapshot " << manifest.snapshotNumber << " for Perfetto export.\n";
            return false;
        }

        const auto& traceVolumeStatistics = dataModelBuilder.GetTraceVolumeStatistics();
        ApplicationState::SnapshotStatistics& snapshotStatistics = state.pendingSnapshotStatistics;
        for (size_t providerTypeIndex = 0; providerTypeIndex < traceVolumeStatistics.historyByProviderType.size(); ++providerTypeIndex)
        {
            snapshotStatistics.traceVolumes.historyBytesByProviderType[providerTypeIndex] = traceVolumeStatistics.historyByProviderType[providerTypeIndex].byteCount;
        }
        snapshotStatistics.traceVolumes.totalHistoryBytes = traceVolumeStatistics.totalHistory.byteCount;
        snapshotStatistics.traceVolumes.gpuPmSampleBytes = traceVolumeStatistics.gpuPmSamples.byteCount;
        snapshotStatistics.traceVolumes.attachBytes = traceVolumeStatistics.attach.byteCount;
        snapshotStatistics.workloads = SummarizeWorkloads(dataModel);
        if (dataModel.m_observationStartUtcNsec <= dataModel.m_observationEndUtcNsec)
        {
            snapshotStatistics.observationDurationNsec = dataModel.m_observationEndUtcNsec - dataModel.m_observationStartUtcNsec;
        }
        snapshotStatistics.snapshotNumber = manifest.snapshotNumber;
        snapshotStatistics.valid = true;

        nv::trace::PerfettoTraceExporterOptions exporterOptions;
        exporterOptions.enableDebugEvents = state.enableDebugEvents;
        nv::trace::PerfettoTraceExporter<SnapshotDatabaseReader> exporter(dataModel, exporterOptions);
        const std::filesystem::path outputPath = CreatePerfettoOutputPath(state, manifest.snapshotNumber);
        if (!state.showDataRate)
        {
            std::cout << "Exporting snapshot " << manifest.snapshotNumber << " to " << outputPath.string() << ".\n";
        }
        exporter.Export(outputPath);
        return true;
    }

    bool ExportSnapshotToDatabase(const nv::trace::ContinuousSnapshotManifest& manifest, ApplicationState& state)
    {
        state.pendingSnapshotStatistics = {};
        SnapshotDatabaseReader databaseReader;
        const bool initialized = databaseReader.Initialize([&manifest](ManifestStorageReader& storageReader) {
            return storageReader.Initialize(manifest);
        });
        if (!initialized)
        {
            std::cerr << "Error: failed to initialize snapshot " << manifest.snapshotNumber << " for database export.\n";
            return false;
        }

        using DataModelBuilder = nv::trace::Codec::DataModelBuilder<SnapshotDatabaseReader>;
        SnapshotDataModel dataModel;
        DataModelBuilder dataModelBuilder(dataModel);
        typename DataModelBuilder::Options builderOptions;
        builderOptions.observationStartUtcNsec = manifest.actualBeginTimestampUtcNsec;
        builderOptions.observationEndUtcNsec = manifest.cutoffTimestampUtcNsec;
        builderOptions.preserveOutOfWindowTimestamps = true;
        if (!dataModelBuilder.Build(databaseReader, builderOptions))
        {
            std::cerr << "Error: failed to read snapshot " << manifest.snapshotNumber << " for database export.\n";
            return false;
        }

        const auto& traceVolumeStatistics = dataModelBuilder.GetTraceVolumeStatistics();
        ApplicationState::SnapshotStatistics& snapshotStatistics = state.pendingSnapshotStatistics;
        for (size_t providerTypeIndex = 0; providerTypeIndex < traceVolumeStatistics.historyByProviderType.size(); ++providerTypeIndex)
        {
            snapshotStatistics.traceVolumes.historyBytesByProviderType[providerTypeIndex] = traceVolumeStatistics.historyByProviderType[providerTypeIndex].byteCount;
        }
        snapshotStatistics.traceVolumes.totalHistoryBytes = traceVolumeStatistics.totalHistory.byteCount;
        snapshotStatistics.traceVolumes.gpuPmSampleBytes = traceVolumeStatistics.gpuPmSamples.byteCount;
        snapshotStatistics.traceVolumes.attachBytes = traceVolumeStatistics.attach.byteCount;
        snapshotStatistics.workloads = SummarizeWorkloads(dataModel);
        if (dataModel.m_observationStartUtcNsec <= dataModel.m_observationEndUtcNsec)
        {
            snapshotStatistics.observationDurationNsec = dataModel.m_observationEndUtcNsec - dataModel.m_observationStartUtcNsec;
        }
        snapshotStatistics.snapshotNumber = manifest.snapshotNumber;
        snapshotStatistics.valid = true;

        if (!state.showDataRate)
        {
            std::cout << "Ingesting snapshot " << manifest.snapshotNumber << " into " << state.databasePath.string() << ".\n";
        }

        nv::trace::DuckDbTableSink& sink = state.sink;
        if (!sink.BeginSnapshotIngestionTransaction())
        {
            std::cerr << "Error: failed to begin ingesting snapshot " << manifest.snapshotNumber << ".\n";
            return false;
        }
        auto abortGuard = nv::trace::ScopeExitGuard([&sink]() { sink.AbortSnapshotIngestionTransaction(); });

        if (state.captureStartUtcNsec == nv::trace::Codec::InvalidTimestamp)
        {
            state.captureStartUtcNsec = manifest.actualBeginTimestampUtcNsec;
        }
        const std::string traceDirectory = state.outputDirectory.string();
        nv::trace::TraceExportInfo traceExportInfo;
        traceExportInfo.traceDir = traceDirectory;
        traceExportInfo.captureStartUtcNsec = state.captureStartUtcNsec;
        traceExportInfo.captureEndUtcNsec = manifest.cutoffTimestampUtcNsec;

        nv::trace::TableExporter<SnapshotDatabaseReader, nv::trace::DuckDbTableSink> tableExporter(dataModel);

        const bool tableExportSucceeded = tableExporter.Export(sink, manifest.snapshotNumber, traceExportInfo);
        if (!tableExportSucceeded)
        {
            std::cerr << "Error: failed to ingest snapshot " << manifest.snapshotNumber << ".\n";
            return false;
        }

        const bool commitSucceeded = sink.CommitSnapshotIngestionTransaction();
        if (!commitSucceeded)
        {
            std::cerr << "Error: failed to ingest snapshot " << manifest.snapshotNumber << ".\n";
            return false;
        }
        return true;
    }

    void OnExportCompleted(
        const nv::trace::ContinuousSnapshotManifest& manifest,
        const nv::trace::ContinuousSnapshotExportResult& result,
        ApplicationState& state)
    {
        const bool succeeded = result.storageSucceeded && result.attachComplete && result.exportSucceeded;
        state.completedSnapshotCount.fetch_add(1, std::memory_order_relaxed);
        if (!succeeded)
        {
            state.exportFailed.store(true, std::memory_order_relaxed);
        }

        {
            std::lock_guard<std::mutex> lock(state.dashboardStatisticsMutex);
            ApplicationState::DashboardStatistics& dashboardStatistics = state.dashboardStatistics;
            const ApplicationState::SnapshotStatistics& snapshotStatistics = state.pendingSnapshotStatistics;
            if (snapshotStatistics.valid && snapshotStatistics.snapshotNumber == manifest.snapshotNumber)
            {
                for (size_t providerTypeIndex = 0; providerTypeIndex < dashboardStatistics.traceVolumes.historyBytesByProviderType.size(); ++providerTypeIndex)
                {
                    dashboardStatistics.traceVolumes.historyBytesByProviderType[providerTypeIndex] += snapshotStatistics.traceVolumes.historyBytesByProviderType[providerTypeIndex];
                }
                dashboardStatistics.traceVolumes.totalHistoryBytes += snapshotStatistics.traceVolumes.totalHistoryBytes;
                dashboardStatistics.traceVolumes.gpuPmSampleBytes += snapshotStatistics.traceVolumes.gpuPmSampleBytes;
                dashboardStatistics.traceVolumes.attachBytes += snapshotStatistics.traceVolumes.attachBytes;
                dashboardStatistics.workloads.Accumulate(snapshotStatistics.workloads);
                dashboardStatistics.observationDurationNsec += snapshotStatistics.observationDurationNsec;
            }
            dashboardStatistics.completedSnapshotCount += 1;
            dashboardStatistics.truncatedSnapshotCount += manifest.historyTruncated ? 1 : 0;
            dashboardStatistics.backpressuredRecords += manifest.backpressuredRecords;
            dashboardStatistics.latestConsumeDataDurationNsec = result.maxConsumeDataDurationNsec;
            dashboardStatistics.maximumConsumeDataDurationNsec = std::max(dashboardStatistics.maximumConsumeDataDurationNsec, result.maxConsumeDataDurationNsec);
            dashboardStatistics.attachDurationNsec = result.attachDurationNsec;
            dashboardStatistics.exportDurationNsec = result.exportDurationNsec;
            dashboardStatistics.perfettoExportDurationNsec = state.pendingPerfettoExportDurationNsec;
            dashboardStatistics.databaseExportDurationNsec = state.pendingDatabaseExportDurationNsec;
            dashboardStatistics.latestTotalWorkerDurationNsec = result.totalWorkerDurationNsec;
            dashboardStatistics.maximumTotalWorkerDurationNsec = std::max(dashboardStatistics.maximumTotalWorkerDurationNsec, result.totalWorkerDurationNsec);
        }

        const uint64_t retainedDurationNsec = manifest.actualBeginTimestampUtcNsec <= manifest.actualEndTimestampUtcNsec
                                                ? manifest.actualEndTimestampUtcNsec - manifest.actualBeginTimestampUtcNsec
                                                : 0;

        if (!state.showDataRate)
        {
            std::cout << "Snapshot " << manifest.snapshotNumber
                      << " completed: " << (succeeded ? "success" : "failure")
                      << ", retained " << retainedDurationNsec / nsecPerMsec << " ms"
                      << ", backpressured records " << manifest.backpressuredRecords
                      << ", attach " << result.attachDurationNsec / nsecPerMsec << " ms"
                      << ", export " << result.exportDurationNsec / nsecPerMsec << " ms"
                      << (result.exceededSnapshotCadence ? ", exceeded snapshot cadence" : "")
                      << '\n';

            if (result.exceededSnapshotCadence)
            {
                std::cerr << "Warning: snapshot " << manifest.snapshotNumber << " took longer than the snapshot interval to export, so trace data may be lost. Consider increasing --snapshot-ms.\n";
            }
            if (state.fullHistoryBankDataBlockCount <= manifest.dataBlockFiles.size())
            {
                std::cerr << "Warning: snapshot " << manifest.snapshotNumber << " filled its history bank before the snapshot interval ended, so trace data may be lost. Consider increasing --data-rate.\n";
            }
        }
    }

    uint64_t CalculateRate(uint64_t byteCount, uint64_t durationNsec)
    {
        if (durationNsec == 0)
        {
            return 0;
        }
        return byteCount > UINT64_MAX / 1'000'000'000ULL
                 ? UINT64_MAX
                 : byteCount * 1'000'000'000ULL / durationNsec;
    }

    std::string FormatCount(uint64_t count)
    {
        static constexpr const char* pSuffixes[] = {"", " K", " M", " B", " T"};
        double value = static_cast<double>(count);
        size_t suffixIndex = 0;
        while (1000.0 <= value && suffixIndex + 1 < sizeof(pSuffixes) / sizeof(pSuffixes[0]))
        {
            value /= 1000.0;
            suffixIndex += 1;
        }

        std::ostringstream text;
        if (suffixIndex == 0)
        {
            text << count;
        }
        else
        {
            text << std::fixed << std::setprecision(1) << value << pSuffixes[suffixIndex];
        }
        return text.str();
    }

    std::string FormatCountRate(uint64_t count, uint64_t durationNsec)
    {
        if (count == 0 || durationNsec == 0)
        {
            return "0.0 K/s";
        }
        const double countPerSec = static_cast<double>(count) * 1'000'000'000.0 / static_cast<double>(durationNsec);
        if (countPerSec < 100.0)
        {
            return "<0.1 K/s";
        }

        std::ostringstream text;
        text << std::fixed << std::setprecision(1) << countPerSec / 1000.0 << " K/s";
        return text.str();
    }

    std::string FormatGigabytes(uint64_t byteCount)
    {
        if (byteCount != 0 && byteCount < 10'000'000)
        {
            return "<0.01 GB";
        }
        std::ostringstream text;
        text << std::fixed << std::setprecision(2) << static_cast<double>(byteCount) / 1'000'000'000.0 << " GB";
        return text.str();
    }

    std::string FormatGigabytesPerSec(uint64_t byteCount, uint64_t durationNsec)
    {
        if (byteCount == 0 || durationNsec == 0)
        {
            return "0.00 GB/s";
        }
        const double gigabytesPerSec = static_cast<double>(byteCount) / static_cast<double>(durationNsec);
        if (gigabytesPerSec < 0.01)
        {
            return "<0.01 GB/s";
        }
        std::ostringstream text;
        text << std::fixed << std::setprecision(2) << gigabytesPerSec << " GB/s";
        return text.str();
    }

    std::string BuildMeterLine(
        const char* pLabel,
        uint64_t byteRate,
        uint64_t totalBytes,
        uint64_t scaleBytesPerSec,
        const char* pColor)
    {
        constexpr size_t barWidth = 36;
        constexpr const char* pAnsiReset = "\x1b[0m";
        const double fraction = scaleBytesPerSec == 0
                                  ? 0.0
                                  : std::clamp(static_cast<double>(byteRate) / scaleBytesPerSec, 0.0, 1.0);
        const size_t filledWidth = static_cast<size_t>(fraction * barWidth + 0.5);

        std::ostringstream line;
        line << "\x1b[2K\r" << std::left << std::setw(15) << pLabel << ' '
             << pColor << std::string(filledWidth, '#') << "\x1b[2m" << std::string(barWidth - filledWidth, '-')
             << pAnsiReset << ' '
             << std::right << std::fixed << std::setprecision(1)
             << std::setw(12) << static_cast<double>(byteRate) / 1024.0 << " KiB/s  "
             << std::setw(12) << static_cast<double>(totalBytes) / bytesPerMiB << " MiB";
        return line.str();
    }

    uint64_t RoundNsecToMsec(uint64_t durationNsec)
    {
        return durationNsec / nsecPerMsec + (nsecPerMsec / 2 <= durationNsec % nsecPerMsec ? 1 : 0);
    }

    std::string BuildTimeBudgetBar(uint64_t durationNsec, uint64_t maximumDurationNsec, uint64_t budgetNsec)
    {
        constexpr size_t barWidth = 36;
        constexpr size_t midpointIndex = barWidth / 2;
        constexpr const char* pAnsiReset = "\x1b[0m";
        const double utilization = budgetNsec == 0 ? 0.0 : static_cast<double>(durationNsec) / static_cast<double>(budgetNsec);
        const double maximumUtilization = budgetNsec == 0 ? 0.0 : static_cast<double>(maximumDurationNsec) / static_cast<double>(budgetNsec);
        const size_t filledWidth = static_cast<size_t>(std::clamp(utilization / 2.0, 0.0, 1.0) * barWidth + 0.5);
        const size_t maximumIndex = std::min(barWidth - 1, static_cast<size_t>(std::clamp(maximumUtilization / 2.0, 0.0, 1.0) * barWidth));

        std::ostringstream bar;
        for (size_t cellIndex = 0; cellIndex < barWidth; ++cellIndex)
        {
            if (maximumDurationNsec != 0 && cellIndex == maximumIndex)
            {
                bar << "\x1b[1;97m*" << pAnsiReset;
                continue;
            }
            if (cellIndex == midpointIndex)
            {
                bar << "\x1b[97m|" << pAnsiReset;
                continue;
            }
            if (filledWidth <= cellIndex)
            {
                bar << "\x1b[2m-" << pAnsiReset;
                continue;
            }

            const double cellUtilization = 2.0 * static_cast<double>(cellIndex + 1) / static_cast<double>(barWidth);
            if (cellUtilization < 0.5)
            {
                bar << "\x1b[92m#";
            }
            else if (cellUtilization < 0.8)
            {
                bar << "\x1b[93m#";
            }
            else if (cellUtilization < 0.95)
            {
                bar << "\x1b[38;5;208m#";
            }
            else
            {
                bar << "\x1b[91m#";
            }
            bar << pAnsiReset;
        }
        return bar.str();
    }

    std::string BuildTimeBudgetLine(const char* pLabel, uint64_t durationNsec, uint64_t maximumDurationNsec, uint64_t budgetNsec)
    {
        const double utilizationPercent = budgetNsec == 0 ? 0.0 : 100.0 * static_cast<double>(durationNsec) / static_cast<double>(budgetNsec);

        std::ostringstream line;
        line << "\x1b[2K\r" << std::left << std::setw(15) << pLabel << ' '
             << BuildTimeBudgetBar(durationNsec, maximumDurationNsec, budgetNsec) << ' '
             << "\x1b[0m" << std::right
             << std::setw(7) << RoundNsecToMsec(durationNsec) << " / " << std::setw(7) << RoundNsecToMsec(budgetNsec) << " ms  "
             << std::fixed << std::setprecision(1) << std::setw(6) << utilizationPercent << "%  max "
             << std::setw(7) << RoundNsecToMsec(maximumDurationNsec) << " ms";
        return line.str();
    }

    std::string BuildWorkloadLine(
        const char* pLabel,
        const EagerGraphStatistics& statistics,
        uint64_t durationNsec,
        bool showBytes)
    {
        std::ostringstream line;
        line << "\x1b[2K\r" << std::left << std::setw(20) << pLabel
             << std::right << std::setw(10) << FormatCount(statistics.Count())
             << std::setw(12) << FormatCountRate(statistics.Count(), durationNsec)
             << std::setw(11) << FormatCount(statistics.eagerCount)
             << std::setw(11) << FormatCount(statistics.graphCount);
        if (showBytes)
        {
            line << std::setw(13) << FormatGigabytes(statistics.Bytes())
                 << std::setw(15) << FormatGigabytesPerSec(statistics.Bytes(), durationNsec);
        }
        return line.str();
    }

    std::string BuildScalarWorkloadLine(const char* pLabel, uint64_t count, uint64_t durationNsec)
    {
        std::ostringstream line;
        line << "\x1b[2K\r" << std::left << std::setw(20) << pLabel
             << std::right << std::setw(10) << FormatCount(count)
             << std::setw(12) << FormatCountRate(count, durationNsec);
        return line.str();
    }

    void DrawDataRateMeter(
        nv::trace::ContinuousSnapshotExporter& exporter,
        ApplicationState& state,
        uint64_t scaleBytesPerSec,
        bool redraw)
    {
        nv::trace::ContinuousSnapshotExporter::LiveStatistics liveStatistics;
        exporter.TryGetLiveStatistics(liveStatistics);

        ApplicationState::DashboardStatistics dashboardStatistics;
        {
            std::lock_guard<std::mutex> lock(state.dashboardStatisticsMutex);
            dashboardStatistics = state.dashboardStatistics;
        }

        const ApplicationState::TraceVolumes& traceVolumes = dashboardStatistics.traceVolumes;
        const WorkloadStatistics& workloads = dashboardStatistics.workloads;

        const auto providerBytes = [&](nv::trace::Codec::ProviderType providerType) {
            return traceVolumes.historyBytesByProviderType[static_cast<size_t>(providerType)];
        };
        const uint64_t cudaBytes = providerBytes(nv::trace::Codec::ProviderType::CudaApiTrace);
        const uint64_t nvtxBytes = providerBytes(nv::trace::Codec::ProviderType::NvtxTrace);
        const uint64_t gpuPmSampleBytes = traceVolumes.gpuPmSampleBytes;
        const uint64_t gpuHwTraceBytes = providerBytes(nv::trace::Codec::ProviderType::GpuHwTrace);
        const uint64_t categorizedBytes = cudaBytes + nvtxBytes + gpuPmSampleBytes + gpuHwTraceBytes;
        const uint64_t otherBytes = categorizedBytes <= traceVolumes.totalHistoryBytes
                                      ? traceVolumes.totalHistoryBytes - categorizedBytes
                                      : 0;

        std::ostringstream exportBreakdown;
        if (state.exportPerfettoTrace)
        {
            exportBreakdown << ": perfetto " << RoundNsecToMsec(dashboardStatistics.perfettoExportDurationNsec)
                            << " + database " << RoundNsecToMsec(dashboardStatistics.databaseExportDurationNsec) << " ms";
        }

        std::ostringstream dashboard;
        constexpr size_t dashboardLineCount = 24;
        if (redraw)
        {
            dashboard << "\x1b[" << dashboardLineCount << 'A';
        }
        dashboard << "\x1b[2K\rGlobal TSF throughput (bar scale: 2x provisioned rate)\n"
                  << BuildMeterLine("Total", liveStatistics.dataRateBytesPerSec, liveStatistics.totalCommittedBytes, scaleBytesPerSec, "\x1b[92m") << '\n'
                  << BuildMeterLine("CUDA", CalculateRate(cudaBytes, dashboardStatistics.observationDurationNsec), cudaBytes, scaleBytesPerSec, "\x1b[96m") << '\n'
                  << BuildMeterLine("NVTX", CalculateRate(nvtxBytes, dashboardStatistics.observationDurationNsec), nvtxBytes, scaleBytesPerSec, "\x1b[95m") << '\n'
                  << BuildMeterLine("GPU PM Samples", CalculateRate(gpuPmSampleBytes, dashboardStatistics.observationDurationNsec), gpuPmSampleBytes, scaleBytesPerSec, "\x1b[93m") << '\n'
                  << BuildMeterLine("GPU HW Trace", CalculateRate(gpuHwTraceBytes, dashboardStatistics.observationDurationNsec), gpuHwTraceBytes, scaleBytesPerSec, "\x1b[94m") << '\n'
                  << BuildMeterLine("Other", CalculateRate(otherBytes, dashboardStatistics.observationDurationNsec), otherBytes, scaleBytesPerSec, "\x1b[91m") << '\n'
                  << BuildMeterLine("Attach", CalculateRate(traceVolumes.attachBytes, dashboardStatistics.observationDurationNsec), traceVolumes.attachBytes, scaleBytesPerSec, "\x1b[90m") << '\n'
                  << '\n'
                  << "\x1b[2K\rQoS wall time (bar scale: 2x budget, | = deadline, * = run max)\n"
                  << BuildTimeBudgetLine(
                         "ConsumeData",
                         dashboardStatistics.latestConsumeDataDurationNsec,
                         dashboardStatistics.maximumConsumeDataDurationNsec,
                         exporter.GetConfiguration().consumeIntervalNsec)
                  << '\n'
                  << BuildTimeBudgetLine(
                         "Export worker",
                         dashboardStatistics.latestTotalWorkerDurationNsec,
                         dashboardStatistics.maximumTotalWorkerDurationNsec,
                         exporter.GetConfiguration().snapshotCadenceNsec)
                  << "  (attach " << RoundNsecToMsec(dashboardStatistics.attachDurationNsec)
                  << " + hook " << RoundNsecToMsec(dashboardStatistics.exportDurationNsec) << " ms" << exportBreakdown.str() << ")\n"
                  << "\x1b[2K\r\n"
                  << "\x1b[2K\rCompleted snapshots: " << dashboardStatistics.completedSnapshotCount
                  << "   total-duration: " << std::fixed << std::setprecision(8) << static_cast<double>(dashboardStatistics.observationDurationNsec) / 1'000'000'000.0 << " s"
                  << "   truncated-snapshots: " << dashboardStatistics.truncatedSnapshotCount
                  << "   backpressured-records: " << dashboardStatistics.backpressuredRecords << '\n'
                  << "\x1b[2K\r" << std::left << std::setw(20) << "Workload"
                  << std::right << std::setw(10) << "Total" << std::setw(12) << "Rate"
                  << std::setw(11) << "Eager" << std::setw(11) << "Graph"
                  << std::setw(13) << "Data" << std::setw(15) << "Data rate" << '\n'
                  << BuildWorkloadLine("CUDA kernels", workloads.kernelLaunches, dashboardStatistics.observationDurationNsec, false) << '\n'
                  << BuildWorkloadLine("Memcpy calls", workloads.memcpyCalls, dashboardStatistics.observationDurationNsec, true) << '\n'
                  << BuildWorkloadLine("Memset calls", workloads.memsetCalls, dashboardStatistics.observationDurationNsec, true) << '\n'
                  << BuildWorkloadLine("Host callbacks", workloads.hostCallbacks, dashboardStatistics.observationDurationNsec, false) << '\n'
                  << BuildScalarWorkloadLine("Graph launches", workloads.graphLaunches, dashboardStatistics.observationDurationNsec) << '\n'
                  << BuildScalarWorkloadLine("Graph nodes created", workloads.graphNodesCreated, dashboardStatistics.observationDurationNsec) << '\n'
                  << BuildScalarWorkloadLine("CUmodules loaded", workloads.modulesLoaded, dashboardStatistics.observationDurationNsec) << '\n'
                  << BuildScalarWorkloadLine("CUfunctions created", workloads.functionsCreated, dashboardStatistics.observationDurationNsec) << '\n'
                  << BuildScalarWorkloadLine("NVTX pushes", workloads.nvtxPushCalls, dashboardStatistics.observationDurationNsec) << '\n';
        std::cout << dashboard.str() << std::flush;
    }

    volatile sig_atomic_t interruptRequested = 0;

    void ResetTerminalColorAndRequestInterrupt(int)
    {
        interruptRequested = 1;
        constexpr char resetSequence[] = "\x1b[0m\r\n";
        const ssize_t result = ::write(STDOUT_FILENO, resetSequence, sizeof(resetSequence) - 1);
        static_cast<void>(result);
    }

    bool InstallConsoleControlHandler(struct sigaction& out_previousAction)
    {
        struct sigaction action = {};
        action.sa_handler = &ResetTerminalColorAndRequestInterrupt;
        sigemptyset(&action.sa_mask);
        return sigaction(SIGINT, &action, &out_previousAction) == 0;
    }

    bool ConsumeEnterKeyIfPressed()
    {
        pollfd descriptor = {STDIN_FILENO, POLLIN, 0};
        int pollResult = 0;
        do
        {
            pollResult = ::poll(&descriptor, 1, 0);
        } while (pollResult < 0 && errno == EINTR);

        if (pollResult <= 0 || !(descriptor.revents & POLLIN))
        {
            return false;
        }

        char input[64];
        ssize_t inputSize = 0;
        do
        {
            inputSize = ::read(STDIN_FILENO, input, sizeof(input));
        } while (inputSize < 0 && errno == EINTR);

        for (ssize_t inputIndex = 0; inputIndex < inputSize; ++inputIndex)
        {
            if (input[inputIndex] == '\n')
            {
                return true;
            }
        }
        return false;
    }

    bool HasFiniteDuration(const Options& options)
    {
        return options.durationMsec != unlimitedDurationMsec;
    }

    bool HasDurationElapsed(const Options& options, const std::chrono::steady_clock::time_point& startTime)
    {
        return HasFiniteDuration(options)
            && std::chrono::milliseconds(options.durationMsec) <= std::chrono::steady_clock::now() - startTime;
    }

    void RunCapturePolling(
        nv::trace::ContinuousSnapshotExporter& exporter,
        ApplicationState& state,
        const Options& options)
    {
#if defined(__linux__)
        interruptRequested = 0;
        struct sigaction previousAction = {};
        const bool consoleControlHandlerInstalled = InstallConsoleControlHandler(previousAction);
#endif
        const auto startTime = std::chrono::steady_clock::now();
        const uint64_t provisionedDataRateBytesPerSec = options.dataRateMiBPerSec * bytesPerMiB;
        const uint64_t meterScaleBytesPerSec = provisionedDataRateBytesPerSec <= UINT64_MAX / 2
                                                 ? 2 * provisionedDataRateBytesPerSec
                                                 : UINT64_MAX;
        bool redraw = false;
        while (true)
        {
            if (options.showDataRate)
            {
                DrawDataRateMeter(exporter, state, meterScaleBytesPerSec, redraw);
                redraw = true;
            }

            if (options.stopOnEnterKey && ConsumeEnterKeyIfPressed())
            {
                break;
            }
#if defined(__linux__)
            if (interruptRequested)
            {
                break;
            }
#endif
            if (HasDurationElapsed(options, startTime))
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (options.showDataRate)
        {
            std::cout << "\x1b[0m\n";
        }
#if defined(__linux__)
        if (consoleControlHandlerInstalled)
        {
            sigaction(SIGINT, &previousAction, nullptr);
        }
#endif
    }

    bool InitializeExporter(
        nv::trace::ContinuousSnapshotExporter& exporter,
        const Options& options,
        ApplicationState& state)
    {
        size_t maxAttachSize = 0;
        size_t maxRetainedShmBytes = 0;
        if (!ConvertMiBToBytes(options.maxAttachSizeMiB, maxAttachSize)
            || !ConvertMiBToBytes(options.maxRetainedShmMiB, maxRetainedShmBytes))
        {
            std::cerr << "Error: a configured MiB size does not fit in size_t.\n";
            return false;
        }

        nv::trace::ContinuousSnapshotExporter::Options exporterOptions;
        exporterOptions.historyDurationNsec = options.snapshotMsec * nsecPerMsec;
        exporterOptions.provisionedDataRateBytesPerSec = options.dataRateMiBPerSec * bytesPerMiB;
        exporterOptions.dataBlockSizeMiB = options.dataBlockSizeMiB;
        exporterOptions.latencyBufferDataBlockCount = static_cast<uint8_t>(options.latencyBufferDataBlockCount);
        exporterOptions.maxAttachSize = maxAttachSize;
        exporterOptions.maxZombieObjectBytes = maxRetainedShmBytes;
        exporterOptions.outputDirectory = state.outputDirectory;
        exporterOptions.deleteFilesOnReset = !options.keepWorkFiles;
        exporterOptions.isProviderTypeIgnored = options.isProviderTypeIgnored;
        exporterOptions.fnExportSnapshot = [&state](const nv::trace::ContinuousSnapshotManifest& manifest) {
            state.pendingPerfettoExportDurationNsec = 0;
            state.pendingDatabaseExportDurationNsec = 0;
            if (!manifest.attachComplete)
            {
                std::cerr << "Error: snapshot " << manifest.snapshotNumber << " has incomplete attachment data ("
                          << manifest.droppedAttachRecordCount << " records, " << manifest.droppedAttachBytes << " bytes omitted).\n";
                if (manifest.minimumRequiredAttachSizeBytes)
                {
                    const uint64_t minimumAttachSizeMiB = manifest.minimumRequiredAttachSizeBytes / bytesPerMiB
                                                        + (manifest.minimumRequiredAttachSizeBytes % bytesPerMiB != 0 ? 1 : 0);
                    std::cerr << "       Increase --max-attach-size to at least " << minimumAttachSizeMiB << " MiB.\n";
                }
                if (manifest.minimumRequiredDataBlockSizeBytes)
                {
                    const uint64_t minimumDataBlockSizeMiB = manifest.minimumRequiredDataBlockSizeBytes / bytesPerMiB
                                                           + (manifest.minimumRequiredDataBlockSizeBytes % bytesPerMiB != 0 ? 1 : 0);
                    std::cerr << "       Increase --data-block-size to at least " << minimumDataBlockSizeMiB << " MiB.\n";
                }
                return false;
            }

            const auto elapsedNsecSince = [](std::chrono::steady_clock::time_point startTime) {
                return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - startTime).count());
            };
            bool perfettoExportSucceeded = true;
            if (state.exportPerfettoTrace)
            {
                const auto perfettoStartTime = std::chrono::steady_clock::now();
                perfettoExportSucceeded = ExportSnapshotToPerfetto(manifest, state);
                state.pendingPerfettoExportDurationNsec = elapsedNsecSince(perfettoStartTime);
            }
            const auto databaseStartTime = std::chrono::steady_clock::now();
            const bool databaseExportSucceeded = ExportSnapshotToDatabase(manifest, state);
            state.pendingDatabaseExportDurationNsec = elapsedNsecSince(databaseStartTime);
            return perfettoExportSucceeded && databaseExportSucceeded;
        };
        exporterOptions.fnOnExportCompleted = [&state](const nv::trace::ContinuousSnapshotManifest& manifest, const nv::trace::ContinuousSnapshotExportResult& result) {
            OnExportCompleted(manifest, result, state);
        };
        return exporter.Initialize(exporterOptions);
    }

} // namespace

int main(int argc, char** argv)
{
    try
    {
        Options options;
        if (!ParseOptions(argc, argv, options))
        {
            return EXIT_FAILURE;
        }

        if (options.showSchemaOnly)
        {
            for (const nv::trace::TableDesc& tableDesc : nv::trace::GetTableDescs())
            {
                std::cout << FormatCreateTableSql(nv::trace::DuckDbTableSink::CreateTableSql(tableDesc)) << ";\n\n";
            }
            return EXIT_SUCCESS;
        }

        setvbuf(stdout, nullptr, _IONBF, 0);

        if (options.snapshotMsec < recommendedMinimumSnapshotMsec)
        {
            std::cerr << "Warning: --snapshot-ms " << options.snapshotMsec << " is below the recommended minimum of "
                      << recommendedMinimumSnapshotMsec << " ms, so snapshots may not finish exporting within the interval.\n";
        }

        ApplicationState state;
        state.outputDirectory = CreateRunOutputDirectory(options.outputDirectory);
        state.enableDebugEvents = options.enableDebugEvents;
        state.exportPerfettoTrace = options.exportPerfettoTrace;
        state.showDataRate = options.showDataRate;
        state.databasePath = options.databasePath.empty() ? state.outputDirectory / "snapshots.duckdb" : options.databasePath;
        std::error_code errorCode;
        std::filesystem::create_directories(state.outputDirectory, errorCode);
        if (errorCode != std::error_code{})
        {
            std::cerr << "Error: failed to create output directory " << state.outputDirectory.string() << ": " << errorCode.message() << '\n';
            return EXIT_FAILURE;
        }

        if (!state.sink.Open(state.databasePath, nv::trace::GetTableDescs(), nv::trace::TableSchemaVersion, options.dbFlushThresholdBytes, options.dbMemoryLimitBytes))
        {
            std::cerr << "Error: failed to prepare database " << state.databasePath.string() << ".\n";
            return EXIT_FAILURE;
        }

        nv::trace::ContinuousSnapshotExporter exporter;
        std::cout << "Initializing continuous snapshot exporter in " << state.outputDirectory.string() << ".\n";
        if (!InitializeExporter(exporter, options, state))
        {
            std::cerr << "Error: ContinuousSnapshotExporter initialization failed. The nvidia-nsight-perf service may not be running.\n";
            return EXIT_FAILURE;
        }

        const auto& configuration = exporter.GetConfiguration();
        std::cout << "Configured " << configuration.historyBufferDataBlockCount << " history DataBlocks per bank ("
                  << configuration.historyBufferSizeBytesPerBank / bytesPerMiB << " MiB, including "
                  << configuration.overflowPreventionReserveSizeBytesPerBank / bytesPerMiB << " MiB overflow-prevention reserve), plus "
                  << static_cast<unsigned int>(configuration.latencyBufferDataBlockCount) << " latency DataBlocks ("
                  << configuration.latencyBufferSizeBytes / bytesPerMiB << " MiB).\n";
        const size_t reserveDataBlockCount = static_cast<size_t>(configuration.overflowPreventionReserveSizeBytesPerBank / configuration.dataBlockSizeBytes);
        state.fullHistoryBankDataBlockCount = configuration.historyBufferDataBlockCount > reserveDataBlockCount + 1
                                                ? configuration.historyBufferDataBlockCount - reserveDataBlockCount - 1
                                                : 1;

        if (!exporter.Start())
        {
            std::cerr << "Error: failed to start continuous snapshot capture.\n";
            return EXIT_FAILURE;
        }

        if (options.stopOnEnterKey && HasFiniteDuration(options))
        {
            std::cout << "Capturing until Enter is pressed or " << options.durationMsec << " ms elapse.\n";
        }
        else if (options.stopOnEnterKey)
        {
            std::cout << "Capturing. Press Enter to stop and export the final partial snapshot.\n";
        }
        else if (HasFiniteDuration(options))
        {
            std::cout << "Capturing for at most " << options.durationMsec << " ms.\n";
        }
        else
        {
            std::cout << "Capturing until interrupted.\n";
        }

        if (options.showDataRate || HasFiniteDuration(options) || !options.stopOnEnterKey)
        {
            RunCapturePolling(exporter, state, options);
        }
        else if (options.stopOnEnterKey)
        {
            std::cin.get();
        }
        std::cout << "Stopping capture and draining exports.\n";
        if (!exporter.StopAtTsc(0))
        {
            std::cerr << "Error: failed to stop and drain continuous snapshot capture.\n";
            return EXIT_FAILURE;
        }
        state.sink.Close();

        if (state.exportFailed.load(std::memory_order_relaxed))
        {
            std::cerr << "Error: one or more snapshot exports failed.\n";
            return EXIT_FAILURE;
        }

        const std::filesystem::path parquetOutputDirectory = state.outputDirectory / "parquet_export";
        std::cout << "Exporting finalized database to Parquet in " << parquetOutputDirectory.string() << ".\n";
        if (!ExportDuckDbToParquet(state.databasePath, parquetOutputDirectory))
        {
            return EXIT_FAILURE;
        }

        std::cout << "Completed " << state.completedSnapshotCount.load(std::memory_order_relaxed)
                  << " snapshot exports into " << state.databasePath.string()
                  << " and " << parquetOutputDirectory.string() << ".\n";
        if (state.exportPerfettoTrace)
        {
            std::cout << "Perfetto traces in " << state.outputDirectory.string() << ".\n";
        }
        return EXIT_SUCCESS;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Fatal error: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
