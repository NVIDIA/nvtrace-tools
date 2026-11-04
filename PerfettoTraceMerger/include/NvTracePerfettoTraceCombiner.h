/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "NvTraceZipCombiner.h"
#include <NvTrace/Offline/NvTraceYamlCodec.h>
#include <NvTrace/Target/NvTraceClientLogger.h>

#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace nv {
namespace trace {


    class PerfettoTraceCombiner
    {
    public:
        PerfettoTraceCombiner() = default;

        bool Initialize()
        {
            Reset();
            m_initialized = true;
            return true;
        }

        void Reset()
        {
            m_initialized = false;
        }

        bool IsInitialized() const noexcept
        {
            return m_initialized;
        }

        static const char* ManifestName()
        {
            return "perfetto_manifest.json";
        }

        static size_t MaximumManifestSize()
        {
            return 1024 * 1024;
        }

        // Combines input archives and replaces outputArchive if it already exists.
        bool Combine(
            const std::vector<ZipFilesystem::path>& inputArchives,
            const ZipFilesystem::path& outputArchive,
            std::string* pError = nullptr,
            std::string* pWarnings = nullptr) const
        {
            if (pError != nullptr)
            {
                pError->clear();
            }
            if (pWarnings != nullptr)
            {
                pWarnings->clear();
            }
            if (!m_initialized)
            {
                return SetError(pError, "Perfetto trace combiner is not initialized");
            }
            if (inputArchives.empty())
            {
                return SetError(pError, "at least one input archive is required");
            }
            const ZipFilesystem::path outputDirectory = outputArchive.parent_path();
            const ZipFilesystem::path temporary = outputArchive.string() + ".tmp";
            std::error_code ignored;
            ZipFilesystem::remove(temporary, ignored);
            if (!outputDirectory.empty())
            {
                ZipFilesystem::create_directories(outputDirectory, ignored);
                if (ignored)
                {
                    return SetError(pError, "cannot create output directory: " + ignored.message());
                }
            }

            bool succeeded = false;
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                if (!output)
                {
                    return SetError(pError, "cannot create temporary output: " + temporary.string());
                }
                std::vector<uint8_t> stagingBuffer(64 * 1024, uint8_t{0});
                ZipStreamingCompressor compressor;
                const bool compressorInitialized = compressor.Initialize(inputArchives.size() + 1);
                if (!compressorInitialized)
                {
                    return SetError(pError, "cannot initialize ZIP compressor");
                }
                ZipCombiner combiner;
                const bool combinerInitialized = combiner.Initialize();
                if (!combinerInitialized)
                {
                    return SetError(pError, "cannot initialize ZIP combiner");
                }

                auto acquireBuffer = [&]() {
                    ZipStreamingCompressor::OutputBuffer outputBuffer;
                    outputBuffer.pData = stagingBuffer.data();
                    outputBuffer.capacity = stagingBuffer.size();
                    outputBuffer.slot = 0;
                    return outputBuffer;
                };
                auto submitBuffer = [&](const ZipStreamingCompressor::OutputBuffer& outputBuffer, size_t size) {
                    if (size != 0)
                    {
                        output.write(reinterpret_cast<const char*>(outputBuffer.pData), static_cast<std::streamsize>(size));
                    }
                    return output.good();
                };
                auto writeArchive = [&](auto& archive) {
                    std::map<std::string, ZipFilesystem::path> outputEntryOwners;
                    std::vector<Plan> plans;
                    for (const ZipFilesystem::path& inputPath : inputArchives)
                    {
                        ZipArchiveReader reader;
                        std::string localError;
                        const bool readerInitialized = reader.Initialize(inputPath, &localError);
                        if (!readerInitialized)
                        {
                            SetError(pError, inputPath.string() + ": " + localError);
                            return false;
                        }

                        std::string manifestText;
                        bool manifestFound = false;
                        std::set<std::string> encounteredEntryPaths;
                        std::set<std::string> retainedEntryPaths;
                        for (uint64_t entryIndex = 0; entryIndex < reader.EntryCount(); ++entryIndex)
                        {
                            ZipArchiveReader::Entry entry;
                            const bool entryRead = reader.ReadNextEntry(entry, &localError);
                            if (!entryRead)
                            {
                                SetError(pError, inputPath.string() + ": " + localError);
                                return false;
                            }

                            const bool isManifest = entry.name == ManifestName() || entry.name == "perfetto_manifest";
                            if (isManifest)
                            {
                                if (manifestFound)
                                {
                                    SetError(pError, inputPath.string() + ": multiple perfetto manifests");
                                    return false;
                                }
                                manifestFound = true;
                                const bool manifestRead = reader.ReadStoredEntry(
                                    entry,
                                    manifestText,
                                    MaximumManifestSize(),
                                    &localError);
                                if (!manifestRead)
                                {
                                    SetError(pError, inputPath.string() + ": manifest: " + localError);
                                    return false;
                                }
                                continue;
                            }

                            encounteredEntryPaths.insert(entry.name);
                            const auto itrExistingOwner = outputEntryOwners.find(entry.name);
                            if (itrExistingOwner != outputEntryOwners.end())
                            {
                                const std::string warning =
                                    "duplicate file detected: " + entry.name + " retained from " + itrExistingOwner->second.string() + ", discarded from " + inputPath.string();
                                AppendWarning(pWarnings, warning);
                                NVTRACE_LOG_WRN(50, "%s", warning.c_str());
                                continue;
                            }

                            const bool importSucceeded = combiner.ImportEncodedEntry(
                                archive,
                                reader,
                                entry,
                                entry.name,
                                &localError);
                            if (!importSucceeded)
                            {
                                SetError(pError, inputPath.string() + ": " + localError);
                                return false;
                            }
                            outputEntryOwners.emplace(entry.name, inputPath);
                            retainedEntryPaths.insert(entry.name);
                        }

                        if (!manifestFound)
                        {
                            SetError(pError, inputPath.string() + ": missing perfetto_manifest.json");
                            return false;
                        }

                        Plan plan;
                        const bool manifestParsed = ReadPartialManifest(
                            manifestText,
                            encounteredEntryPaths,
                            retainedEntryPaths,
                            plan,
                            localError);
                        if (!manifestParsed)
                        {
                            SetError(pError, inputPath.string() + ": " + localError);
                            return false;
                        }
                        if (plan.retained)
                        {
                            plans.push_back(std::move(plan));
                        }
                    }

                    if (plans.empty())
                    {
                        SetError(pError, "no input trace remained after duplicate-file filtering");
                        return false;
                    }
                    std::string manifest;
                    std::string manifestError;
                    const bool manifestBuilt = BuildManifest(plans, manifest, manifestError);
                    if (!manifestBuilt)
                    {
                        SetError(pError, manifestError);
                        return false;
                    }
                    const bool manifestSucceeded = archive.WriteStoredEntry(
                        ManifestName(),
                        reinterpret_cast<const uint8_t*>(manifest.data()),
                        manifest.size());
                    if (!manifestSucceeded)
                    {
                        return false;
                    }
                    return true;
                };
                const bool writeSucceeded = compressor.WriteArchive(acquireBuffer, submitBuffer, writeArchive);
                output.flush();
                succeeded = writeSucceeded && output.good();
                if (!succeeded && (pError == nullptr || pError->empty()))
                {
                    SetError(pError, "failed while writing combined ZIP archive");
                }
            }
            if (!succeeded)
            {
                ZipFilesystem::remove(temporary, ignored);
                return false;
            }
            ZipFilesystem::remove(outputArchive, ignored);
            if (ignored)
            {
                const std::string replaceError = ignored.message();
                ZipFilesystem::remove(temporary, ignored);
                return SetError(pError, "cannot replace output archive: " + replaceError);
            }
            ZipFilesystem::rename(temporary, outputArchive, ignored);
            if (ignored)
            {
                ZipFilesystem::remove(temporary, ignored);
                return SetError(pError, "cannot publish output archive: " + ignored.message());
            }
            return true;
        }

    private:
        struct Plan
        {
            std::string machineName;
            std::string destinationName;
            bool retained = false;
        };

        static bool ReadPartialManifest(
            const std::string& text,
            const std::set<std::string>& encounteredEntryPaths,
            const std::set<std::string>& retainedEntryPaths,
            Plan& plan,
            std::string& error)
        {
            YamlCodec::DataNode root;
            YamlCodec::ReadOptions readOptions;
            readOptions.maxMemory = MaximumManifestSize();
            YamlCodec::Error codecError;
            const bool manifestParsed = YamlCodec::ReadJson(text, &root, readOptions, &codecError);
            if (!manifestParsed)
            {
                error = "invalid manifest JSON at line " + std::to_string(codecError.line) + ", column " + std::to_string(codecError.column) + ": " + codecError.message;
                return false;
            }

            const YamlCodec::NodeValue& rootValue = root.GetValue();
            const YamlCodec::SequenceEntry* pManifestEntry = root.MapFind("perfetto_manifest");
            const YamlCodec::DataNode* pManifest = pManifestEntry != nullptr ? &pManifestEntry->value : nullptr;
            const YamlCodec::SequenceEntry* pVersionEntry = pManifest != nullptr ? pManifest->MapFind("version") : nullptr;
            const YamlCodec::SequenceEntry* pFilesEntry = pManifest != nullptr ? pManifest->MapFind("files") : nullptr;
            const YamlCodec::DataNode* pVersion = pVersionEntry != nullptr ? &pVersionEntry->value : nullptr;
            const YamlCodec::DataNode* pFiles = pFilesEntry != nullptr ? &pFilesEntry->value : nullptr;
            const bool rootIsValid = rootValue.nodeType == YamlCodec::NodeType::Mapping && rootValue.entries.Size() == 1;
            const bool manifestIsValid = pManifest != nullptr && pManifest->GetValue().nodeType == YamlCodec::NodeType::Mapping;
            const bool versionIsValid = pVersion != nullptr && pVersion->GetValue().nodeType == YamlCodec::NodeType::SignedInteger && pVersion->GetValue().s64 == 1;
            const bool filesAreValid = pFiles != nullptr && pFiles->GetValue().nodeType == YamlCodec::NodeType::Sequence && pFiles->GetValue().entries.Size() == 1;
            if (!rootIsValid || !manifestIsValid || !versionIsValid || !filesAreValid)
            {
                error = "manifest must be version 1 with exactly one file";
                return false;
            }

            const YamlCodec::DataNode& file = pFiles->GetValue().entries[0].value;
            const YamlCodec::SequenceEntry* pPathEntry = file.MapFind("path");
            const YamlCodec::SequenceEntry* pMachineEntry = file.MapFind("machine");
            const YamlCodec::DataNode* pPath = pPathEntry != nullptr ? &pPathEntry->value : nullptr;
            const YamlCodec::DataNode* pMachine = pMachineEntry != nullptr ? &pMachineEntry->value : nullptr;
            const YamlCodec::SequenceEntry* pMachineNameEntry = pMachine != nullptr ? pMachine->MapFind("name") : nullptr;
            const YamlCodec::DataNode* pMachineName = pMachineNameEntry != nullptr ? &pMachineNameEntry->value : nullptr;
            const bool pathIsValid = pPath != nullptr && pPath->GetValue().nodeType == YamlCodec::NodeType::String;
            const bool machineNameIsValid = pMachineName != nullptr && pMachineName->GetValue().nodeType == YamlCodec::NodeType::String && !pMachineName->GetValue().str.empty();
            if (!pathIsValid || !machineNameIsValid)
            {
                error = "manifest file requires path and non-empty machine.name";
                return false;
            }
            const std::string& tracePath = pPath->GetValue().str;
            const bool archiveMemberEncountered = encounteredEntryPaths.count(tracePath) != 0;
            if (!archiveMemberEncountered)
            {
                error = "manifest path does not name an archive member";
                return false;
            }

            plan.machineName = pMachineName->GetValue().str;
            plan.destinationName = tracePath;
            plan.retained = retainedEntryPaths.count(tracePath) != 0;
            return true;
        }

        static bool InsertMappingNode(
            YamlCodec::DataNode& mapping,
            const char* pKey,
            YamlCodec::DataNode&& value,
            std::string& error)
        {
            const bool inserted = mapping.MapInsert(std::string(pKey), std::move(value));
            if (!inserted)
            {
                error = std::string("duplicate manifest field: ") + pKey;
            }
            return inserted;
        }

        static bool BuildManifest(
            const std::vector<Plan>& plans,
            std::string& output,
            std::string& error)
        {
            output.clear();
            error.clear();
            YamlCodec::DataNode manifest;

            YamlCodec::DataNode version;
            version.SetUInt64(1);
            const bool versionInserted = InsertMappingNode(manifest, "version", std::move(version), error);
            if (!versionInserted)
            {
                return false;
            }

            YamlCodec::DataNode traceTime;
            YamlCodec::DataNode clock;
            clock.SetString("REALTIME");
            const bool clockInserted = InsertMappingNode(traceTime, "clock", std::move(clock), error);
            if (!clockInserted)
            {
                return false;
            }
            YamlCodec::DataNode file;
            file.SetString(plans.front().destinationName);
            const bool fileInserted = InsertMappingNode(traceTime, "file", std::move(file), error);
            if (!fileInserted)
            {
                return false;
            }
            const bool traceTimeInserted = InsertMappingNode(manifest, "trace_time", std::move(traceTime), error);
            if (!traceTimeInserted)
            {
                return false;
            }

            YamlCodec::DataNode files;
            files.UpdateSequence([&](YamlCodec::Vector<YamlCodec::SequenceEntry>& entries) {
                for (size_t planIndex = 0; planIndex < plans.size(); ++planIndex)
                {
                    const Plan& plan = plans[planIndex];
                    YamlCodec::DataNode file;

                    YamlCodec::DataNode path;
                    path.SetString(plan.destinationName);
                    const bool pathInserted = InsertMappingNode(file, "path", std::move(path), error);
                    if (!pathInserted)
                    {
                        return;
                    }

                    YamlCodec::DataNode machine;
                    YamlCodec::DataNode machineName;
                    machineName.SetString(plan.machineName);
                    const bool machineNameInserted = InsertMappingNode(machine, "name", std::move(machineName), error);
                    if (!machineNameInserted)
                    {
                        return;
                    }
                    const bool machineInserted = InsertMappingNode(file, "machine", std::move(machine), error);
                    if (!machineInserted)
                    {
                        return;
                    }

                    entries.PushBackDefault().value = std::move(file);
                }
            });
            if (!error.empty())
            {
                return false;
            }
            const bool filesInserted = InsertMappingNode(manifest, "files", std::move(files), error);
            if (!filesInserted)
            {
                return false;
            }

            YamlCodec::DataNode attributes;
            YamlCodec::DataNode inputTraceCount;
            inputTraceCount.SetUInt64(plans.size());
            const bool inputTraceCountInserted = InsertMappingNode(attributes, "com.nvidia.nvtrace.input_trace_count", std::move(inputTraceCount), error);
            if (!inputTraceCountInserted)
            {
                return false;
            }
            const bool attributesInserted = InsertMappingNode(manifest, "attributes", std::move(attributes), error);
            if (!attributesInserted)
            {
                return false;
            }

            YamlCodec::DataNode root;
            const bool manifestInserted = InsertMappingNode(root, "perfetto_manifest", std::move(manifest), error);
            if (!manifestInserted)
            {
                return false;
            }

            YamlCodec::WriteOptions writeOptions;
            writeOptions.maxOutputLength = MaximumManifestSize() - 1;
            writeOptions.prettyJson = true;
            writeOptions.prettyJsonFirstRootKeyInline = true;
            YamlCodec::Error codecError;
            auto sink = [&](const uint8_t* pData, size_t size) {
                const char* pChars = reinterpret_cast<const char*>(pData);
                output.insert(output.end(), pChars, pChars + size);
                return true;
            };
            const bool manifestWritten = YamlCodec::WriteJson(root, writeOptions, sink, &codecError);
            if (!manifestWritten)
            {
                error = "cannot render combined manifest: " + codecError.message;
                return false;
            }
            output += '\n';
            return true;
        }

        static bool SetError(std::string* pError, const std::string& message)
        {
            NVTRACE_LOG_ERR(50, "%s", message.c_str());
            if (pError != nullptr)
            {
                *pError = message;
            }
            return false;
        }

        static void AppendWarning(std::string* pWarnings, const std::string& warning)
        {
            if (pWarnings == nullptr)
            {
                return;
            }
            *pWarnings += warning;
            *pWarnings += '\n';
        }

        bool m_initialized = false;
    };

}
} //namespace nv::trace
