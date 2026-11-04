/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <NvTrace/Offline/Exporter/NvTraceZipStreamingCompressor.h>

#include <algorithm>
#include <array>
#include <cstdint>
#if __cplusplus >= 201703L
#include <filesystem>
#else
#include <experimental/filesystem>
#endif
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace nv {
namespace trace {

#if __cplusplus >= 201703L
    namespace ZipFilesystem = std::filesystem;
#else
    namespace ZipFilesystem = std::experimental::filesystem;
#endif

    // Sequential ZIP reader used by product-specific archive policies. The archive
    // remains open while directory records and selected payloads are consumed.
    class ZipArchiveReader
    {
    public:
        struct Entry : ZipStreamingCompressor::EncodedEntry
        {
            uint64_t localHeaderOffset = 0;
            uint64_t dataOffset = 0;
        };

        ZipArchiveReader() = default;

        bool Initialize(const ZipFilesystem::path& path, std::string* pError = nullptr)
        {
            Reset();
            m_path = path;
            m_input.open(m_path, std::ios::binary);
            if (!m_input)
            {
                Reset();
                return SetError(pError, "cannot open ZIP archive");
            }
            const bool opened = Open(pError);
            if (!opened)
            {
                Reset();
                return false;
            }
            m_initialized = true;
            return true;
        }

        void Reset()
        {
            m_input.close();
            m_input.clear();
            m_path.clear();
            m_fileSize = 0;
            m_directoryOffset = 0;
            m_directorySize = 0;
            m_nextDirectoryOffset = 0;
            m_entryCount = 0;
            m_entriesRead = 0;
            m_initialized = false;
        }

        bool IsInitialized() const noexcept
        {
            return m_initialized;
        }

        const ZipFilesystem::path& Path() const noexcept
        {
            return m_path;
        }

        uint64_t EntryCount() const noexcept
        {
            return m_entryCount;
        }

        bool ReadNextEntry(Entry& entry, std::string* pError = nullptr)
        {
            if (!m_initialized || m_entriesRead >= m_entryCount)
            {
                return SetError(pError, "no ZIP directory entry is available");
            }

            m_input.clear();
            m_input.seekg(static_cast<std::streamoff>(m_nextDirectoryOffset));
            std::array<uint8_t, 46> fixed{};
            const bool fixedRead = ReadExact(m_input, fixed.data(), fixed.size());
            if (!fixedRead || Get32(fixed.data()) != kCentralHeaderSignature)
            {
                return SetError(pError, "invalid central-directory entry");
            }
            if (Get16(fixed.data() + 34) != 0)
            {
                return SetError(pError, "multipart ZIP entry is unsupported");
            }

            const uint16_t nameLength = Get16(fixed.data() + 28);
            const uint16_t extraLength = Get16(fixed.data() + 30);
            const uint16_t commentLength = Get16(fixed.data() + 32);
            const uint64_t variableSize = static_cast<uint64_t>(nameLength) + extraLength + commentLength;
            const uint64_t directoryEnd = m_directoryOffset + m_directorySize;
            if (m_nextDirectoryOffset > directoryEnd || fixed.size() + variableSize > directoryEnd - m_nextDirectoryOffset)
            {
                return SetError(pError, "central-directory entry is outside the directory");
            }

            std::vector<uint8_t> variable(static_cast<size_t>(variableSize));
            const bool variableRead = ReadExact(m_input, variable.data(), variable.size());
            if (!variableRead)
            {
                return SetError(pError, "truncated central-directory entry");
            }

            entry = Entry();
            entry.flags = Get16(fixed.data() + 8);
            entry.method = static_cast<ZipStreamingCompressor::CompressionMethod>(Get16(fixed.data() + 10));
            entry.crc32 = Get32(fixed.data() + 16);
            entry.compressedSize = Get32(fixed.data() + 20);
            entry.uncompressedSize = Get32(fixed.data() + 24);
            entry.localHeaderOffset = Get32(fixed.data() + 42);
            entry.name.assign(reinterpret_cast<const char*>(variable.data()), nameLength);
            if (entry.name.empty() || entry.name.find('\0') != std::string::npos)
            {
                return SetError(pError, "invalid ZIP member name");
            }
            const bool zip64ExtraRead = ReadZip64Extra(
                variable.data() + nameLength,
                extraLength,
                entry.uncompressedSize,
                entry.compressedSize,
                entry.localHeaderOffset);
            if (!zip64ExtraRead)
            {
                return SetError(pError, "invalid ZIP64 entry metadata");
            }
            if ((entry.flags & kFlagEncrypted) != 0 || (entry.method != ZipStreamingCompressor::CompressionMethod::Store && entry.method != ZipStreamingCompressor::CompressionMethod::Deflate))
            {
                return SetError(pError, "encrypted or unsupported ZIP member");
            }

            m_nextDirectoryOffset += fixed.size() + variableSize;
            ++m_entriesRead;

            m_input.clear();
            m_input.seekg(static_cast<std::streamoff>(entry.localHeaderOffset));
            std::array<uint8_t, 30> local{};
            const bool localRead = ReadExact(m_input, local.data(), local.size());
            if (!localRead || Get32(local.data()) != kLocalHeaderSignature)
            {
                return SetError(pError, "invalid local ZIP header for " + entry.name);
            }
            entry.dataOffset = entry.localHeaderOffset + local.size() + Get16(local.data() + 26) + Get16(local.data() + 28);
            if (entry.dataOffset > m_fileSize || entry.compressedSize > m_fileSize - entry.dataOffset)
            {
                return SetError(pError, "truncated ZIP payload for " + entry.name);
            }
            return true;
        }

        bool ReadStoredEntry(const Entry& entry, std::string& output, size_t maximumSize, std::string* pError = nullptr)
        {
            if (!m_initialized || entry.method != ZipStreamingCompressor::CompressionMethod::Store || entry.compressedSize != entry.uncompressedSize || entry.uncompressedSize > maximumSize)
            {
                return SetError(pError, "entry is not a bounded stored member: " + entry.name);
            }
            output.resize(static_cast<size_t>(entry.uncompressedSize));
            m_input.clear();
            m_input.seekg(static_cast<std::streamoff>(entry.dataOffset));
            uint8_t* pOutputBytes = output.empty() ? nullptr : reinterpret_cast<uint8_t*>(&output[0]);
            const bool readSucceeded = ReadExact(m_input, pOutputBytes, output.size());
            if (!readSucceeded)
            {
                return SetError(pError, "cannot read ZIP member: " + entry.name);
            }
            const uint32_t computedCrc32 = ZipStreamingCompressor::ComputeCrc32(pOutputBytes, output.size());
            if (computedCrc32 != entry.crc32)
            {
                return SetError(pError, "CRC mismatch for ZIP member: " + entry.name);
            }
            return true;
        }

        bool PositionAtEncodedPayload(const Entry& entry, std::string* pError = nullptr)
        {
            if (!m_initialized)
            {
                return SetError(pError, "ZIP archive reader is not initialized");
            }
            m_input.clear();
            m_input.seekg(static_cast<std::streamoff>(entry.dataOffset));
            if (!m_input)
            {
                return SetError(pError, "cannot seek to ZIP member: " + entry.name);
            }
            return true;
        }

        std::istream& Stream()
        {
            return m_input;
        }

    private:
        bool Open(std::string* pError)
        {
            m_input.seekg(0, std::ios::end);
            const std::streamoff fileSizeValue = m_input.tellg();
            if (fileSizeValue < 22)
            {
                return SetError(pError, "ZIP archive is too small");
            }
            m_fileSize = static_cast<uint64_t>(fileSizeValue);
            const size_t tailSize = static_cast<size_t>(std::min<uint64_t>(m_fileSize, 22 + 0xFFFFu));
            std::vector<uint8_t> tail(tailSize);
            m_input.seekg(static_cast<std::streamoff>(m_fileSize - tailSize));
            const bool tailRead = ReadExact(m_input, tail.data(), tail.size());
            if (!tailRead)
            {
                return SetError(pError, "cannot read ZIP end records");
            }

            size_t endIndex = std::numeric_limits<size_t>::max();
            for (size_t index = tail.size() - 22;; --index)
            {
                if (Get32(tail.data() + index) == kEndOfCentralDirectorySignature && index + 22 + Get16(tail.data() + index + 20) == tail.size())
                {
                    endIndex = index;
                    break;
                }
                if (index == 0)
                {
                    break;
                }
            }
            if (endIndex == std::numeric_limits<size_t>::max())
            {
                return SetError(pError, "missing ZIP end-of-central-directory record");
            }

            const uint8_t* pEnd = tail.data() + endIndex;
            if (Get16(pEnd + 4) != 0 || Get16(pEnd + 6) != 0)
            {
                return SetError(pError, "multipart ZIP archives are unsupported");
            }
            m_entryCount = Get16(pEnd + 10);
            m_directorySize = Get32(pEnd + 12);
            m_directoryOffset = Get32(pEnd + 16);
            if (m_entryCount == kZip16Sentinel || m_directorySize == kZip32Sentinel || m_directoryOffset == kZip32Sentinel)
            {
                const uint64_t endAbsolute = m_fileSize - tailSize + endIndex;
                if (endAbsolute < 20)
                {
                    return SetError(pError, "missing ZIP64 locator");
                }
                std::array<uint8_t, 20> locator{};
                m_input.clear();
                m_input.seekg(static_cast<std::streamoff>(endAbsolute - locator.size()));
                const bool locatorRead = ReadExact(m_input, locator.data(), locator.size());
                if (!locatorRead || Get32(locator.data()) != kZip64LocatorSignature || Get32(locator.data() + 4) != 0 || Get32(locator.data() + 16) != 1)
                {
                    return SetError(pError, "invalid ZIP64 locator");
                }
                std::array<uint8_t, 56> zip64End{};
                m_input.clear();
                m_input.seekg(static_cast<std::streamoff>(Get64(locator.data() + 8)));
                const bool zip64EndRead = ReadExact(m_input, zip64End.data(), zip64End.size());
                if (!zip64EndRead || Get32(zip64End.data()) != kZip64EndSignature || Get32(zip64End.data() + 16) != 0 || Get32(zip64End.data() + 20) != 0)
                {
                    return SetError(pError, "invalid ZIP64 end record");
                }
                m_entryCount = Get64(zip64End.data() + 32);
                m_directorySize = Get64(zip64End.data() + 40);
                m_directoryOffset = Get64(zip64End.data() + 48);
            }
            if (m_directoryOffset > m_fileSize || m_directorySize > m_fileSize - m_directoryOffset || m_entryCount > m_directorySize / 46 + 1)
            {
                return SetError(pError, "central directory is outside the archive");
            }
            m_nextDirectoryOffset = m_directoryOffset;
            return true;
        }
        static constexpr uint16_t kFlagEncrypted = 1u << 0;
        static constexpr uint16_t kZip16Sentinel = 0xFFFFu;
        static constexpr uint32_t kZip32Sentinel = 0xFFFFFFFFu;
        static constexpr uint32_t kLocalHeaderSignature = 0x04034B50u;
        static constexpr uint32_t kCentralHeaderSignature = 0x02014B50u;
        static constexpr uint32_t kZip64EndSignature = 0x06064B50u;
        static constexpr uint32_t kZip64LocatorSignature = 0x07064B50u;
        static constexpr uint32_t kEndOfCentralDirectorySignature = 0x06054B50u;

        static uint16_t Get16(const uint8_t* pData)
        {
            return static_cast<uint16_t>(pData[0] | (static_cast<uint16_t>(pData[1]) << 8));
        }

        static uint32_t Get32(const uint8_t* pData)
        {
            return static_cast<uint32_t>(Get16(pData)) | (static_cast<uint32_t>(Get16(pData + 2)) << 16);
        }

        static uint64_t Get64(const uint8_t* pData)
        {
            return static_cast<uint64_t>(Get32(pData)) | (static_cast<uint64_t>(Get32(pData + 4)) << 32);
        }

        static bool ReadExact(std::istream& input, uint8_t* pData, size_t size)
        {
            if (size == 0)
            {
                return true;
            }
            input.read(reinterpret_cast<char*>(pData), static_cast<std::streamsize>(size));
            return input.gcount() == static_cast<std::streamsize>(size);
        }

        static bool ReadZip64Extra(
            const uint8_t* pData,
            size_t size,
            uint64_t& uncompressedSize,
            uint64_t& compressedSize,
            uint64_t& localHeaderOffset)
        {
            const bool needUncompressed = uncompressedSize == kZip32Sentinel;
            const bool needCompressed = compressedSize == kZip32Sentinel;
            const bool needOffset = localHeaderOffset == kZip32Sentinel;
            if (!needUncompressed && !needCompressed && !needOffset)
            {
                return true;
            }
            size_t position = 0;
            while (position + 4 <= size)
            {
                const uint16_t id = Get16(pData + position);
                const uint16_t length = Get16(pData + position + 2);
                position += 4;
                if (length > size - position)
                {
                    return false;
                }
                if (id == 0x0001)
                {
                    const uint8_t* pField = pData + position;
                    size_t remaining = length;
                    auto take = [&](uint64_t& value, bool needed) {
                        if (!needed)
                        {
                            return true;
                        }
                        if (remaining < 8)
                        {
                            return false;
                        }
                        value = Get64(pField);
                        pField += 8;
                        remaining -= 8;
                        return true;
                    };
                    return take(uncompressedSize, needUncompressed) && take(compressedSize, needCompressed) && take(localHeaderOffset, needOffset);
                }
                position += length;
            }
            return false;
        }

        static bool SetError(std::string* pError, const std::string& message)
        {
            if (pError != nullptr)
            {
                *pError = message;
            }
            return false;
        }

        ZipFilesystem::path m_path;
        std::ifstream m_input;
        uint64_t m_fileSize = 0;
        uint64_t m_directoryOffset = 0;
        uint64_t m_directorySize = 0;
        uint64_t m_nextDirectoryOffset = 0;
        uint64_t m_entryCount = 0;
        uint64_t m_entriesRead = 0;
        bool m_initialized = false;
    };

    class ZipCombiner
    {
    public:
        ZipCombiner() = default;

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

        template <typename TArchive>
        bool ImportEncodedEntry(
            TArchive& destination,
            ZipArchiveReader& source,
            const ZipArchiveReader::Entry& entry,
            const std::string& destinationName,
            std::string* pError = nullptr)
        {
            if (!m_initialized)
            {
                return SetError(pError, "ZIP combiner is not initialized");
            }
            const bool openSucceeded = source.PositionAtEncodedPayload(entry, pError);
            if (!openSucceeded)
            {
                return false;
            }
            const bool importSucceeded = destination.ImportEncodedEntry(entry, destinationName, source.Stream());
            if (!importSucceeded)
            {
                return SetError(pError, "failed to import ZIP member: " + entry.name);
            }
            return true;
        }

    private:
        static bool SetError(std::string* pError, const std::string& message)
        {
            if (pError != nullptr)
            {
                *pError = message;
            }
            return false;
        }

        bool m_initialized = false;
    };

}
} //namespace nv::trace
