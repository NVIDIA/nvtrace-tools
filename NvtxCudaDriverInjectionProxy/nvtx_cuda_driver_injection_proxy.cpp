/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <NvTrace/Target/NvTraceScopeExitGuard.h>

#include <nvtx3/nvToolsExt.h>
#include <nvtx3/nvToolsExtPayload.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>

#include <dlfcn.h>
#include <unistd.h>

#define NVTX_CUDA_DRIVER_INJECTION_PROXY_EXPORT __attribute__((visibility("default")))

#define STRUCT_SIZE(structName_, fieldName_) (offsetof(structName_, fieldName_) + sizeof(static_cast<structName_*>(nullptr)->fieldName_))

namespace {

    bool s_coreInjectionSucceeded = false;
    bool s_extensionInjectionSucceeded = false;

    std::once_flag s_coreInjectionOnceFlag;
    std::once_flag s_extensionInjectionOnceFlag;

    NvtxFunctionPointer s_coreFunctionPointers[NVTX_CBID_CORE_SIZE] = {};
    NvtxFunctionPointer* s_pCoreFunctionPointers[NVTX_CBID_CORE_SIZE] = {};
    NvtxFunctionPointer s_core2FunctionPointers[NVTX_CBID_CORE2_SIZE] = {};
    NvtxFunctionPointer* s_pCore2FunctionPointers[NVTX_CBID_CORE2_SIZE] = {};

    intptr_t s_extensionFunctionSlots[NVTX_EXT_PAYLOAD_SLOT_COUNT] = {};

    bool FileExists(const std::string& path)
    {
        return !access(path.c_str(), F_OK);
    }

    bool IsNvtxCudaYamlPresent()
    {
        if (FileExists("/run/nvidia/nvtx/nvtx-cuda.yaml"))
        {
            return true;
        }

        // Root has only the global scope.
        if (geteuid() == 0)
        {
            return false;
        }
        const char* pRuntimeDir = std::getenv("XDG_RUNTIME_DIR");
        return pRuntimeDir && pRuntimeDir[0] != '\0' && FileExists(std::string(pRuntimeDir) + "/nvidia/nvtx/nvtx-cuda.yaml");
    }

    template <typename TFN_Initialize>
    bool InitializeInjection(NvtxInjectionInitializerKind kind, TFN_Initialize&& fnInitialize)
    {
        if (!IsNvtxCudaYamlPresent())
        {
            return false;
        }

        void* pCudaDriverHandle = dlopen("libcuda.so.1", RTLD_LAZY);
        if (!pCudaDriverHandle)
        {
            return false;
        }
        auto exitGuard = nv::trace::ScopeExitGuard([pCudaDriverHandle]() { dlclose(pCudaDriverHandle); });
        NvtxGetInjectionInitializerFunc_t pfnGetInjectionInitializer = reinterpret_cast<NvtxGetInjectionInitializerFunc_t>(dlsym(pCudaDriverHandle, "cuNvtxGetInjectionInitializer"));
        if (!pfnGetInjectionInitializer)
        {
            return false;
        }

        NvtxInjectionGenericFunc_t pfnInjectionInitializer = nullptr;
        if (pfnGetInjectionInitializer(kind, &pfnInjectionInitializer) || !pfnInjectionInitializer)
        {
            return false;
        }

        if (!fnInitialize(pfnInjectionInitializer))
        {
            return false;
        }

        exitGuard.Dismiss();
        return true;
    }

    int NVTX_API GetModuleFunctionTable(NvtxCallbackModule callbackModule, NvtxFunctionTable* pFunctionTable, unsigned int* pFunctionTableSize)
    {
        if (callbackModule == NVTX_CB_MODULE_CORE)
        {
            *pFunctionTable = s_pCoreFunctionPointers;
            *pFunctionTableSize = NVTX_CBID_CORE_SIZE;
            return 1;
        }
        if (callbackModule == NVTX_CB_MODULE_CORE2)
        {
            *pFunctionTable = s_pCore2FunctionPointers;
            *pFunctionTableSize = NVTX_CBID_CORE2_SIZE;
            return 1;
        }
        return 0;
    }

    constexpr NvtxExportTableVersionInfo s_exportTableVersionInfo = []() {
        NvtxExportTableVersionInfo exportTableVersionInfo = {};
        exportTableVersionInfo.struct_size = sizeof(NvtxExportTableVersionInfo);
        exportTableVersionInfo.version = 3;
        exportTableVersionInfo.SetInjectionNvtxVersion = [](uint32_t) {};
        return exportTableVersionInfo;
    }();

    constexpr NvtxExportTableCallbacks s_exportTableCallbacks = []() {
        NvtxExportTableCallbacks exportTableCallbacks = {};
        exportTableCallbacks.struct_size = sizeof(NvtxExportTableCallbacks);
        exportTableCallbacks.GetModuleFunctionTable = &GetModuleFunctionTable;
        return exportTableCallbacks;
    }();

    const void* NVTX_API GetExportTable(uint32_t exportTableId)
    {
        if (exportTableId == NVTX_ETID_VERSIONINFO)
        {
            return &s_exportTableVersionInfo;
        }
        if (exportTableId == NVTX_ETID_CALLBACKS)
        {
            return &s_exportTableCallbacks;
        }
        return nullptr;
    }

    void InitializeCoreInjection()
    {
        s_coreInjectionSucceeded = InitializeInjection(NVTX_INJECTION_INITIALIZER_CORE, [](NvtxInjectionGenericFunc_t pfnInjectionInitializer) {
            for (unsigned int index = 0; index < NVTX_CBID_CORE_SIZE; index++)
            {
                s_pCoreFunctionPointers[index] = &s_coreFunctionPointers[index];
            }
            for (unsigned int index = 0; index < NVTX_CBID_CORE2_SIZE; index++)
            {
                s_pCore2FunctionPointers[index] = &s_core2FunctionPointers[index];
            }
            return !!reinterpret_cast<NvtxInitializeInjectionNvtxFunc_t>(pfnInjectionInitializer)(&GetExportTable);
        });
    }

    bool RegisterCoreCallbacks(NvtxGetExportTableFunc_t pfnGetExportTable)
    {
        if (!pfnGetExportTable)
        {
            return false;
        }

        const auto* pExportTableVersionInfo = reinterpret_cast<const NvtxExportTableVersionInfo*>(pfnGetExportTable(NVTX_ETID_VERSIONINFO));
        if (!pExportTableVersionInfo || pExportTableVersionInfo->struct_size < STRUCT_SIZE(NvtxExportTableVersionInfo, SetInjectionNvtxVersion) || pExportTableVersionInfo->version < 2 || 3 < pExportTableVersionInfo->version || !pExportTableVersionInfo->SetInjectionNvtxVersion)
        {
            return false;
        }
        pExportTableVersionInfo->SetInjectionNvtxVersion(3);

        const auto* pExportTableCallbacks = reinterpret_cast<const NvtxExportTableCallbacks*>(pfnGetExportTable(NVTX_ETID_CALLBACKS));
        if (!pExportTableCallbacks || pExportTableCallbacks->struct_size < STRUCT_SIZE(NvtxExportTableCallbacks, GetModuleFunctionTable) || !pExportTableCallbacks->GetModuleFunctionTable)
        {
            return false;
        }

        NvtxFunctionTable pCoreFunctionTable = nullptr;
        unsigned int coreFunctionTableSize = 0;
        if (!pExportTableCallbacks->GetModuleFunctionTable(NVTX_CB_MODULE_CORE, &pCoreFunctionTable, &coreFunctionTableSize) || !pCoreFunctionTable)
        {
            return false;
        }

        NvtxFunctionTable pCore2FunctionTable = nullptr;
        unsigned int core2FunctionTableSize = 0;
        if (!pExportTableCallbacks->GetModuleFunctionTable(NVTX_CB_MODULE_CORE2, &pCore2FunctionTable, &core2FunctionTableSize) || !pCore2FunctionTable)
        {
            return false;
        }

        unsigned int functionTableSize = std::min<unsigned int>(coreFunctionTableSize, NVTX_CBID_CORE_SIZE);
        // Index 0 corresponds to NVTX_CBID_*_INVALID
        for (unsigned int index = 1; index < functionTableSize; index++)
        {
            if (s_coreFunctionPointers[index])
            {
                *pCoreFunctionTable[index] = s_coreFunctionPointers[index];
            }
        }

        functionTableSize = std::min<unsigned int>(core2FunctionTableSize, NVTX_CBID_CORE2_SIZE);
        // Index 0 corresponds to NVTX_CBID_*_INVALID
        for (unsigned int index = 1; index < functionTableSize; index++)
        {
            if (s_core2FunctionPointers[index])
            {
                *pCore2FunctionTable[index] = s_core2FunctionPointers[index];
            }
        }

        return true;
    }

    void On_nvtxMarkPayload(nvtxDomainHandle_t domain, const nvtxPayloadData_t*, size_t)
    {
        nvtxEventAttributes_t eventAttributes = {};
        eventAttributes.version = NVTX_VERSION;
        eventAttributes.size = NVTX_EVENT_ATTRIB_STRUCT_SIZE;
        eventAttributes.messageType = NVTX_MESSAGE_TYPE_REGISTERED;
        eventAttributes.message.registered = 0;
        nvtxDomainMarkEx(domain, &eventAttributes);
    }

    int On_nvtxRangePushPayload(nvtxDomainHandle_t domain, const nvtxPayloadData_t*, size_t)
    {
        nvtxEventAttributes_t eventAttributes = {};
        eventAttributes.version = NVTX_VERSION;
        eventAttributes.size = NVTX_EVENT_ATTRIB_STRUCT_SIZE;
        eventAttributes.messageType = NVTX_MESSAGE_TYPE_REGISTERED;
        eventAttributes.message.registered = 0;
        return nvtxDomainRangePushEx(domain, &eventAttributes);
    }

    int On_nvtxRangePopPayload(nvtxDomainHandle_t domain, const nvtxPayloadData_t*, size_t)
    {
        return nvtxDomainRangePop(domain);
    }

    void InitializeExtensionInjection()
    {
        s_extensionInjectionSucceeded = InitializeInjection(NVTX_INJECTION_INITIALIZER_EXTENSION, [](NvtxInjectionGenericFunc_t pfnInjectionInitializer) {
            nvtxExtModuleSegment_t extModuleSegment = {};
            extModuleSegment.segmentId = 0;
            extModuleSegment.slotCount = NVTX_EXT_PAYLOAD_SLOT_COUNT;
            extModuleSegment.functionSlots = s_extensionFunctionSlots;

            nvtxExtModuleInfo_t extModuleInfo = {};
            extModuleInfo.nvtxVer = NVTX_VERSION;
            extModuleInfo.structSize = sizeof(nvtxExtModuleInfo_t);
            extModuleInfo.moduleId = NVTX_EXT_PAYLOAD_MODULEID;
            extModuleInfo.compatId = NVTX_EXT_PAYLOAD_COMPATID;
            extModuleInfo.segmentsCount = 1;
            extModuleInfo.segments = &extModuleSegment;

            return !!reinterpret_cast<NvtxExtInitializeInjectionFunc_t>(pfnInjectionInitializer)(&extModuleInfo);
        });
        if (!s_extensionInjectionSucceeded)
        {
            s_extensionFunctionSlots[NVTX3EXT_CBID_nvtxMarkPayload] = reinterpret_cast<intptr_t>(&On_nvtxMarkPayload);
            s_extensionFunctionSlots[NVTX3EXT_CBID_nvtxRangePushPayload] = reinterpret_cast<intptr_t>(&On_nvtxRangePushPayload);
            s_extensionFunctionSlots[NVTX3EXT_CBID_nvtxRangePopPayload] = reinterpret_cast<intptr_t>(&On_nvtxRangePopPayload);

            s_extensionInjectionSucceeded = true;
        }
    }

    bool RegisterExtensionCallbacks(nvtxExtModuleInfo_t* pExtModuleInfo)
    {
        if (!pExtModuleInfo || pExtModuleInfo->moduleId != NVTX_EXT_PAYLOAD_MODULEID || pExtModuleInfo->compatId != NVTX_EXT_PAYLOAD_COMPATID || !pExtModuleInfo->segmentsCount || !pExtModuleInfo->segments)
        {
            return false;
        }

        nvtxExtModuleSegment_t& extModuleSegment = pExtModuleInfo->segments[0];
        if (!extModuleSegment.slotCount || !extModuleSegment.functionSlots)
        {
            return false;
        }

        const size_t slotCount = std::min<size_t>(extModuleSegment.slotCount, NVTX_EXT_PAYLOAD_SLOT_COUNT);
        for (size_t index = 0; index < slotCount; index++)
        {
            if (s_extensionFunctionSlots[index])
            {
                extModuleSegment.functionSlots[index] = s_extensionFunctionSlots[index];
            }
        }

        return true;
    }

} // namespace

extern "C" NVTX_CUDA_DRIVER_INJECTION_PROXY_EXPORT int NVTX_API InitializeInjectionNvtx2(NvtxGetExportTableFunc_t pfnGetExportTable)
{
    std::call_once(s_coreInjectionOnceFlag, InitializeCoreInjection);
    if (!s_coreInjectionSucceeded)
    {
        return 0;
    }

    if (!RegisterCoreCallbacks(pfnGetExportTable))
    {
        return 0;
    }

    return 1;
}

extern "C" NVTX_CUDA_DRIVER_INJECTION_PROXY_EXPORT int NVTX_API InitializeInjectionNvtxExtension(nvtxExtModuleInfo_t* pExtModuleInfo)
{
    std::call_once(s_extensionInjectionOnceFlag, InitializeExtensionInjection);
    if (!s_extensionInjectionSucceeded)
    {
        return 0;
    }

    if (!RegisterExtensionCallbacks(pExtModuleInfo))
    {
        return 0;
    }

    return 1;
}
