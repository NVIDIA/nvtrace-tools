/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cxxopts.hpp"
#include <NvTrace/Target/NvTraceGpuPartitionInfo_generated.h>

#include <NvTrace/Target/Consumer/NvTraceConsumerBase.h>
#include <NvTraceProviderIgnoreCli.h>
#include <cuda.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>
#include <vector>

#define CUDA_RETURN_ON_ERR(call, errorDetail)                                                            \
    do                                                                                                   \
    {                                                                                                    \
        CUresult result = (call);                                                                        \
        if (result != CUDA_SUCCESS)                                                                      \
        {                                                                                                \
            NVTRACE_LOG_ERR(20, "CUDA error: %s failed with error %d - %s", #call, result, errorDetail); \
            return false;                                                                                \
        }                                                                                                \
    } while (0)

namespace {
    // ================================
    // Options
    // ================================
    struct PollUntilServiceReadyOptions
    {
        uint32_t timeoutSec = 60;
        nv::trace::Codec::ProviderTypeTable<bool> isProviderTypeIgnored = {};
    };

    PollUntilServiceReadyOptions g_pollUntilServiceReadyOptions;

    void ParsePollUntilServiceReadyOptions(int argc, char** argv)
    {
        cxxopts::Options options("Options");
        options.add_options()("h,help", "Show help message");
        options.add_options()("timeout-sec", "The timeout to wait for the nvidia-nsight-perf service to be ready (seconds)", cxxopts::value<uint32_t>()->default_value("60"));
        options.add_options()(
            "ignore",
            "Ignore a provider when checking service readiness. Ignorable provider types: gpu_pm, gpu_hwt. Ex: --ignore gpu_pm,gpu_hwt",
            cxxopts::value<std::vector<std::string>>(),
            "provider_type");

        auto result = options.parse(argc, argv);
        if (result.count("help"))
        {
            fprintf(stdout, "%s\n", options.help().c_str());
            exit(0);
        }

        g_pollUntilServiceReadyOptions.timeoutSec = result["timeout-sec"].as<uint32_t>();

        if (result.count("ignore"))
        {
            for (const std::string& providerName : result["ignore"].as<std::vector<std::string>>())
            {
                const nv::trace::Codec::ProviderType providerType = nv::trace::ResolveIgnorableProviderTypeByName(providerName);
                if (nv::trace::Codec::ProviderType::Invalid < providerType && providerType < nv::trace::Codec::ProviderType::Count)
                {
                    g_pollUntilServiceReadyOptions.isProviderTypeIgnored[static_cast<size_t>(providerType)] = true;
                }
            }
        }
    }

    bool IsConfiguredProviderTypeIgnored(nv::trace::Codec::ProviderType providerType)
    {
        const size_t providerTypeIndex = static_cast<size_t>(providerType);
        return providerTypeIndex < g_pollUntilServiceReadyOptions.isProviderTypeIgnored.size()
            && g_pollUntilServiceReadyOptions.isProviderTypeIgnored[providerTypeIndex];
    }

    bool HasTimedOut(const std::chrono::steady_clock::time_point& startTime, uint32_t timeoutSec)
    {
        return std::chrono::steady_clock::now() - startTime > std::chrono::seconds(timeoutSec);
    }
} //namespace

namespace nv::trace {
    // ================================
    // PollUntilServiceReadyConsumer
    // ================================
    struct PollUntilServiceReadyConsumerObjectModel : ConsumerObjectModelBase
    {
        using CpuClockCalibrationProviderState = TypedProviderState<CpuClockCalibrationObserver, ProviderState>;
        using GpuClockCalibrationProviderState = TypedProviderState<GpuClockCalibrationObserver, ProviderState>;
        using GpuPmCountersProviderState = TypedProviderState<GpuPmCounterObserver, ProviderState>;
        using GpuHwTraceProviderState = TypedProviderState<GpuHwTraceObserver, ProviderState>;
        using GpuGrCtxTraceProviderState = TypedProviderState<GpuGrCtxTraceObserver, ProviderState>;
        using CudaApiTraceProviderState = NullProviderState;
        using NvtxTraceProviderState = NullProviderState;
        using ThreadEventProviderState = NullProviderState;
    };

    class PollUntilServiceReadyConsumer : protected ConsumerBase<PollUntilServiceReadyConsumer, PollUntilServiceReadyConsumerObjectModel>
    {
        using Super = ConsumerBase<PollUntilServiceReadyConsumer, PollUntilServiceReadyConsumerObjectModel>;
        friend Super;

    public:
        struct Options : Super::Options
        {
        };

    public:
        ~PollUntilServiceReadyConsumer()
        {
            TerminateConsumeThread();
        }

        PollUntilServiceReadyConsumer() = default;
        PollUntilServiceReadyConsumer(const PollUntilServiceReadyConsumer&) = delete;
        PollUntilServiceReadyConsumer& operator=(const PollUntilServiceReadyConsumer&) = delete;

        void Reset() noexcept
        {
            TerminateConsumeThread();
            m_waitMultiplexer.Reset();
            m_consumeThreadCommandPendingEvent.Reset();
            m_consumeThreadCommand = ConsumeThreadCommand::Idle;
            m_visibleCudaDeviceInfos.clear();
            m_initialized = false;
            Super::Reset();
        }

        bool Initialize(const Options& options)
        {
            if (m_initialized)
            {
                return false;
            }

            if (!Super::Initialize(options))
            {
                NVTRACE_LOG_WRN(20, "Failed to connect to NvTrace router server. Retrying...");
                return false;
            }

            auto errorGuard = ScopeExitGuard([this]() { Reset(); });

            bool succeeded = m_consumeThreadCommandPendingEvent.Initialize();
            NVTRACE_LOG_ERR_RETURN_IF(!succeeded, false, 50, "AutoResetEvent::Initialize failed");

            constexpr uint32_t eventCount = 2;
            succeeded = m_waitMultiplexer.Initialize(eventCount);
            NVTRACE_LOG_ERR_RETURN_IF(!succeeded, false, 50, "WaitMultiplexer::Initialize failed");
            succeeded = m_waitMultiplexer.AddObject(EventIdentifier::Command, m_consumeThreadCommandPendingEvent.Desc());
            NVTRACE_LOG_ERR_RETURN_IF(!succeeded, false, 50, "WaitMultiplexer::AddObject failed");
            succeeded = m_waitMultiplexer.AddObject(EventIdentifier::RouterNotification, m_pRouterObserverClient->GetWaitForPendingNotificationHandle());
            NVTRACE_LOG_ERR_RETURN_IF(!succeeded, false, 50, "WaitMultiplexer::AddObject failed");

            if (!FindAllVisibleCudaDevices())
            {
                NVTRACE_LOG_WRN(20, "Failed to find CUDA devices. Retrying...");
                return false;
            }

            // Early out: if service is already ready
            if (AllCudaDevicesNotified() && AllGpuProvidersInitialized())
            {
                m_areAllGpuProvidersMeasuring = true;
                errorGuard.Dismiss();
                m_initialized = true;
                return true;
            }

            try
            {
                m_consumeThread = std::thread(&PollUntilServiceReadyConsumer::ConsumeLoop, this);
            }
            catch (const std::system_error&)
            {
                NVTRACE_LOG_ERR(20, "Failed to create consume thread.");
                return false;
            }

            errorGuard.Dismiss();
            m_initialized = true;
            return true;
        }

        bool AreAllGpuProvidersMeasuring() const
        {
            std::atomic_thread_fence(std::memory_order_acquire);

            if (!m_areAllGpuProvidersMeasuring)
            {
                NVTRACE_LOG_INF(20, "Waiting for all GPU partitions to be notified and all required GPU providers to be ready...");
                return false;
            }

            return true;
        }

        void OutputGpuProvidersFailedStates() const
        {
            for (const auto& [gpuUuid, visibleCudaDeviceInfo] : m_visibleCudaDeviceInfos)
            {
                if (!visibleCudaDeviceInfo.notified)
                {
                    std::string gpuUuidStr(36, '\0');
                    ConvertUuidToString(gpuUuid, gpuUuidStr.data());
                    fprintf(stdout, "CUDA device %s is not notified!\n", gpuUuidStr.c_str());
                }
            }

            for (const auto& [_, gpuPartitionState] : m_systemState.gpuPartitions)
            {
                std::string gpuUuidStr(36, '\0');
                ConvertUuidToString(gpuPartitionState.gpuUuid, gpuUuidStr.data());

                ForEachUninitializedGpuProvider(gpuPartitionState, [&gpuUuidStr](Codec::ProviderType providerType) {
                    fprintf(stdout, "CUDA device %s %s provider is not initialized!\n", gpuUuidStr.c_str(), GetProviderTypeName(providerType));
                    return true;
                });
            }
        }

        void TerminateConsumeThread() noexcept
        {
            if (m_consumeThread.joinable())
            {
                m_consumeThreadCommand = ConsumeThreadCommand::Terminate;
                std::atomic_thread_fence(std::memory_order_release);

                bool succeeded = m_consumeThreadCommandPendingEvent.Signal();
                NVTRACE_LOG_ERR_RETURN_IF(!succeeded, /*void*/, 50, "AutoResetEvent::Signal failed");

                m_consumeThread.join();
            }
        }

    protected:
        bool FindAllVisibleCudaDevices()
        {
            CUDA_RETURN_ON_ERR(cuInit(0), "cuInit failed");

            int deviceCount = 0;
            CUDA_RETURN_ON_ERR(cuDeviceGetCount(&deviceCount), "cuDeviceGetCount failed");

            if (deviceCount == 0)
            {
                NVTRACE_LOG_WRN(50, "No CUDA devices found!");
                return false;
            }

            for (int deviceIdx = 0; deviceIdx < deviceCount; deviceIdx++)
            {
                CUdevice device;
                CUDA_RETURN_ON_ERR(cuDeviceGet(&device, deviceIdx), "cuDeviceGet failed");

                CUuuid uuid;
                CUDA_RETURN_ON_ERR(cuDeviceGetUuid(&uuid, device), "cuDeviceGetUuid failed");

                NVTRACE_UUID nvtraceUuid;
                memcpy(nvtraceUuid.bytes, uuid.bytes, sizeof(nvtraceUuid.bytes));

                CudaDeviceInfo cudaDeviceInfo;
                m_visibleCudaDeviceInfos.insert({nvtraceUuid, cudaDeviceInfo});
            }

            // Handle gpu partition states if the users run this program after the nvidia-nsight-perf service is created and initialized
            for (const auto& [_, gpuPartitionState] : m_systemState.gpuPartitions)
            {
                auto itr = m_visibleCudaDeviceInfos.find(gpuPartitionState.gpuUuid);
                if (itr != m_visibleCudaDeviceInfos.end())
                {
                    itr->second.notified = true;
                }
            }

            return true;
        }

        bool AllCudaDevicesNotified() const
        {
            for (const auto& [_, visibleCudaDeviceInfo] : m_visibleCudaDeviceInfos)
            {
                if (!visibleCudaDeviceInfo.notified)
                {
                    std::string gpuUuidStr(36, '\0');
                    ConvertUuidToString(_, gpuUuidStr.data());
                    NVTRACE_LOG_INF(99, "CUDA device %s is not notified. Waiting for the router notification...", gpuUuidStr.c_str());
                    return false;
                }
            }

            return true;
        }

        template <typename TFN_OnUninitializedProvider>
        bool ForEachUninitializedGpuProvider(const GpuPartitionState& gpuPartitionState, TFN_OnUninitializedProvider&& fnOnUninitializedProvider) const
        {
            const auto* gpuPartitionInfo = gpuPartitionState.gpuPartitionInfo.GetRoot();
            for (const auto& supportedProvider : *gpuPartitionInfo->supportedProviders())
            {
                const NVTRACE_UUID& providerUuid = *reinterpret_cast<const NVTRACE_UUID*>(supportedProvider);
                const Codec::ProviderType providerType = GetProviderType(providerUuid);
                if (IsConfiguredProviderTypeIgnored(providerType))
                {
                    continue;
                }

                bool isInitialized = true;
                switch (providerType)
                {
                    case Codec::ProviderType::GpuClockCalibration:
                        isInitialized = gpuPartitionState.gpuClockCalibrationProvider.IsInitialized();
                        break;
                    case Codec::ProviderType::GpuGrCtxTrace:
                        isInitialized = gpuPartitionState.gpuGrCtxTraceProvider.IsInitialized();
                        break;
                    case Codec::ProviderType::GpuPmCounters:
                        isInitialized = gpuPartitionState.gpuPmCountersProvider.IsInitialized();
                        break;
                    case Codec::ProviderType::GpuHwTrace:
                        isInitialized = gpuPartitionState.gpuHwTraceProvider.IsInitialized();
                        break;
                    case Codec::ProviderType::Invalid:
                    case Codec::ProviderType::Unknown:
                    case Codec::ProviderType::CpuClockCalibration:
                    case Codec::ProviderType::CudaApiTrace:
                    case Codec::ProviderType::NvtxTrace:
                    case Codec::ProviderType::ThreadEvent:
                    case Codec::ProviderType::Count:
                        continue;
                }

                if (!isInitialized && !fnOnUninitializedProvider(providerType))
                {
                    return false;
                }
            }

            return true;
        }

        bool AllGpuProvidersInitialized() const
        {
            for (const auto& [_, gpuPartitionState] : m_systemState.gpuPartitions)
            {
                std::string gpuUuidStr(36, '\0');
                ConvertUuidToString(gpuPartitionState.gpuUuid, gpuUuidStr.data());

                if (!ForEachUninitializedGpuProvider(gpuPartitionState, [&gpuUuidStr](Codec::ProviderType providerType) {
                        NVTRACE_LOG_INF(99, "CUDA device %s %s provider is not initialized yet...", gpuUuidStr.c_str(), GetProviderTypeName(providerType));
                        return false;
                    }))
                {
                    return false;
                }
            }

            return true;
        }

        void ConsumeLoop()
        {
            while (true)
            {
                EventIdentifier signaledIdentifier;
                if (!m_waitMultiplexer.Wait(signaledIdentifier))
                {
                    NVTRACE_LOG_ERR(50, "Failed to wait for events");
                    return;
                }

                switch (signaledIdentifier)
                {
                    case EventIdentifier::Command:
                    {
                        std::atomic_thread_fence(std::memory_order_acquire);
                        switch (m_consumeThreadCommand)
                        {
                            case ConsumeThreadCommand::Idle:
                            {
                                break;
                            }

                            case ConsumeThreadCommand::Terminate:
                            {
                                return;
                            }
                        }

                        break;
                    }

                    case EventIdentifier::RouterNotification:
                    {
                        bool succeeded = m_pRouterObserverClient->ReceiveNotificationIfPending([this](NvTrace_RouterObserverClient_NotificationType notificationType, const void* pNotification) {
                            bool handledResult = HandleRouterNotification(notificationType, pNotification);

                            if (handledResult && notificationType == NvTrace_RouterObserverClient_NotificationType_GpuPartitionCreated)
                            {
                                const NvTrace_RouterObserverClient_GpuPartitionCreatedNotification& notification = *reinterpret_cast<const NvTrace_RouterObserverClient_GpuPartitionCreatedNotification*>(pNotification);
                                const NVTRACE_UUID& gpuUuid = notification.gpuUuid;

                                if (m_visibleCudaDeviceInfos.find(gpuUuid) != m_visibleCudaDeviceInfos.end())
                                {
                                    m_visibleCudaDeviceInfos[gpuUuid].notified = true;
                                }
                            }

                            return handledResult;
                        });
                        NVTRACE_LOG_ERR_RETURN_IF(!succeeded, /*void*/, 50, "Failed to receive notification from the router observer client");

                        if (AllCudaDevicesNotified() && AllGpuProvidersInitialized())
                        {
                            m_areAllGpuProvidersMeasuring = true;
                            std::atomic_thread_fence(std::memory_order_release);
                        }

                        break;
                    }
                }
            }
        }

    protected:
        bool m_initialized = false;

        struct CudaDeviceInfo
        {
            bool notified = false;
        };
        std::map<NVTRACE_UUID, CudaDeviceInfo> m_visibleCudaDeviceInfos;

        enum class ConsumeThreadCommand
        {
            Idle,
            Terminate
        };
        ConsumeThreadCommand m_consumeThreadCommand = ConsumeThreadCommand::Idle;
        AutoResetEvent m_consumeThreadCommandPendingEvent;

        enum class EventIdentifier
        {
            Command,
            RouterNotification
        };
        WaitMultiplexer m_waitMultiplexer;

        std::thread m_consumeThread;
        bool m_areAllGpuProvidersMeasuring = false;
    };

} //namespace nv::trace

int main(int argc, char* argv[])
{
    ParsePollUntilServiceReadyOptions(argc, argv);

    nv::trace::PollUntilServiceReadyConsumer pollUntilServiceReadyConsumer;

    nv::trace::PollUntilServiceReadyConsumer::Options pollUntilServiceReadyConsumerOptions;
    pollUntilServiceReadyConsumerOptions.isProviderTypeIgnored = g_pollUntilServiceReadyOptions.isProviderTypeIgnored;

    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    constexpr uint32_t retryIntervalMsec = 1000;

    fprintf(
        stdout,
        "Waiting to connect to nvidia-nsight-perf service (timeout %u seconds)...\n",
        g_pollUntilServiceReadyOptions.timeoutSec);

    while (!pollUntilServiceReadyConsumer.Initialize(pollUntilServiceReadyConsumerOptions))
    {
        if (HasTimedOut(startTime, g_pollUntilServiceReadyOptions.timeoutSec))
        {
            fprintf(stderr, "Timeout: Failed to connect to nvidia-nsight-perf service within %u seconds!\n", g_pollUntilServiceReadyOptions.timeoutSec);
            return EXIT_FAILURE;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(retryIntervalMsec));
    }

    fprintf(stdout, "Connected to nvidia-nsight-perf service. Waiting for all required GPU providers to be ready...\n");

    while (!pollUntilServiceReadyConsumer.AreAllGpuProvidersMeasuring())
    {
        if (HasTimedOut(startTime, g_pollUntilServiceReadyOptions.timeoutSec))
        {
            pollUntilServiceReadyConsumer.TerminateConsumeThread();
            pollUntilServiceReadyConsumer.OutputGpuProvidersFailedStates();
            fprintf(stderr, "Timeout: Failed to wait for all required GPU providers to be ready within %u seconds!\n", g_pollUntilServiceReadyOptions.timeoutSec);
            return EXIT_FAILURE;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(retryIntervalMsec));
    }

    fprintf(stdout, "nvidia-nsight-perf service is ready! All required GPU providers are measuring! Please continue...\n");

    return EXIT_SUCCESS;
}
