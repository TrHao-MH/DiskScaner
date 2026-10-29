#include <iostream>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <future>
#include <list>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <random>
#include <sstream>
#include <iomanip>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <malloc.h> // _aligned_malloc, _aligned_free

#include "httplib.h"
#include "json.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;
using json = nlohmann::json;

// =====================================================================
// 1. RAII WRAPPERS CHO WINDOWS HANDLES & BUFFER CĂN CHỈNH 4096B
// =====================================================================
class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE h = INVALID_HANDLE_VALUE) noexcept : h_(h) {}
    ~ScopedHandle() noexcept { Close(); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& other) noexcept : h_(other.h_) { other.h_ = INVALID_HANDLE_VALUE; }
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) { Close(); h_ = other.h_; other.h_ = INVALID_HANDLE_VALUE; }
        return *this;
    }
    void Close() noexcept {
        if (h_ != INVALID_HANDLE_VALUE && h_ != NULL) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }
    HANDLE get() const noexcept { return h_; }
    bool valid() const noexcept { return h_ != INVALID_HANDLE_VALUE && h_ != NULL; }
private:
    HANDLE h_;
};

class ScopedFindHandle {
public:
    explicit ScopedFindHandle(HANDLE h = INVALID_HANDLE_VALUE) noexcept : h_(h) {}
    ~ScopedFindHandle() noexcept {
        if (h_ != INVALID_HANDLE_VALUE && h_ != NULL) FindClose(h_);
    }
    ScopedFindHandle(const ScopedFindHandle&) = delete;
    ScopedFindHandle& operator=(const ScopedFindHandle&) = delete;
    HANDLE get() const noexcept { return h_; }
    bool valid() const noexcept { return h_ != INVALID_HANDLE_VALUE && h_ != NULL; }
private:
    HANDLE h_;
};

class AlignedBuffer {
public:
    AlignedBuffer() noexcept = default;
    explicit AlignedBuffer(size_t size, size_t alignment = 4096) {
        Allocate(size, alignment);
    }
    ~AlignedBuffer() noexcept { Free(); }
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    AlignedBuffer(AlignedBuffer&& other) noexcept : ptr_(other.ptr_), size_(other.size_) {
        other.ptr_ = nullptr;
        other.size_ = 0;
    }
    AlignedBuffer& operator=(AlignedBuffer&& other) noexcept {
        if (this != &other) {
            Free();
            ptr_ = other.ptr_;
            size_ = other.size_;
            other.ptr_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }
    void Allocate(size_t size, size_t alignment = 4096) {
        Free();
        if (size > 0) {
            ptr_ = static_cast<uint8_t*>(_aligned_malloc(size, alignment));
            if (!ptr_) throw std::bad_alloc();
            size_ = size;
        }
    }
    void Free() noexcept {
        if (ptr_) {
            _aligned_free(ptr_);
            ptr_ = nullptr;
            size_ = 0;
        }
    }
    uint8_t* data() noexcept { return ptr_; }
    const uint8_t* data() const noexcept { return ptr_; }
    size_t size() const noexcept { return size_; }
private:
    uint8_t* ptr_ = nullptr;
    size_t size_ = 0;
};

// =====================================================================
// 2. HÀM TIỆN ÍCH CHUỖI & STRING ARENA TỐI ƯU HOT-PATH
// =====================================================================
std::string WStringToUTF8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int sz = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), NULL, 0, NULL, NULL);
    if (sz <= 0) return "";
    std::string res(sz, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), &res[0], sz, NULL, NULL);
    return res;
}

std::wstring UTF8ToWString(const std::string& str) {
    if (str.empty()) return L"";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), NULL, 0);
    if (wlen <= 0) return L"";
    std::wstring res(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), &res[0], wlen);
    return res;
}

std::string FormatSize(uint64_t bytes) {
    const char* u[] = { "B", "KB", "MB", "GB", "TB" };
    int i = 0;
    double s = static_cast<double>(bytes);
    while (s >= 1024.0 && i < 4) { s /= 1024.0; i++; }
    char b[32];
    if (i == 0) {
        snprintf(b, sizeof(b), "%llu B", static_cast<unsigned long long>(bytes));
    }
    else {
        snprintf(b, sizeof(b), "%.2f %s", s, u[i]);
    }
    return std::string(b);
}

std::string FormatSignedSize(int64_t bytes) {
    if (bytes == 0) return "0 B";
    bool neg = (bytes < 0);
    uint64_t absVal = neg ? static_cast<uint64_t>(-bytes) : static_cast<uint64_t>(bytes);
    return (neg ? "-" : "+") + FormatSize(absVal);
}

class StringArena {
public:
    void Clear() {
        buffer_.clear();
        buffer_.shrink_to_fit();
    }
    void Reserve(size_t cap) {
        buffer_.reserve(cap);
    }
    std::pair<uint32_t, uint16_t> AppendWString(const wchar_t* w, size_t n) {
        if (!w || n == 0 || n > 340) return { 0, 0 };
        if (buffer_.size() + n * 3 >= 0xFFFFFFFFULL) {
            throw std::runtime_error("StringArena vượt quá giới hạn địa chỉ 32-bit (4GB)!");
        }

        uint32_t offset = static_cast<uint32_t>(buffer_.size());
        size_t i = 0;
        while (i < n && w[i] < 0x80) ++i;

        if (i == n) {
            buffer_.resize(offset + n);
            char* dst = buffer_.data() + offset;
            for (size_t k = 0; k < n; ++k) {
                dst[k] = static_cast<char>(w[k]);
            }
            return { offset, static_cast<uint16_t>(n) };
        }

        char tmp[1024];
        int len = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(n), tmp, sizeof(tmp), NULL, NULL);
        if (len <= 0) return { 0, 0 };
        buffer_.insert(buffer_.end(), tmp, tmp + len);
        return { offset, static_cast<uint16_t>(len) };
    }

    std::string Get(uint32_t offset, uint16_t len) const {
        if (len == 0 || static_cast<size_t>(offset) + len > buffer_.size()) return "";
        return std::string(buffer_.data() + offset, len);
    }
    std::string_view GetView(uint32_t offset, uint16_t len) const noexcept {
        if (len == 0 || static_cast<size_t>(offset) + len > buffer_.size()) return {};
        return std::string_view(buffer_.data() + offset, len);
    }
    size_t SizeBytes() const noexcept { return buffer_.size(); }
    size_t CapacityBytes() const noexcept { return buffer_.capacity(); }
private:
    std::vector<char> buffer_;
};

// =====================================================================
// 3. CẤU TRÚC NHỊ PHÂN NTFS MFT & BẢN GHI TRONG RAM
// =====================================================================
#pragma pack(push, 1)
struct NtfsFileRecordHeader {
    uint32_t Magic;                 // 'FILE' (0x454C4946)
    uint16_t UpdateSequenceOffset;
    uint16_t UpdateSequenceCount;
    uint64_t LogFileSequenceNumber;
    uint16_t SequenceNumber;
    uint16_t HardLinkCount;
    uint16_t FirstAttributeOffset;
    uint16_t Flags;                 // 0x01 = InUse, 0x02 = Directory
    uint32_t UsedSize;
    uint32_t AllocatedSize;
    uint64_t BaseFileRecordSegment;
    uint16_t NextAttributeId;
    uint16_t Padding;
    uint32_t MftRecordNumber;
};

struct NtfsAttributeHeader {
    uint32_t Type;
    uint32_t Length;
    uint8_t  IsNonResident;
    uint8_t  NameLength;
    uint16_t NameOffset;
    uint16_t Flags;
    uint16_t AttributeId;
    union {
        struct {
            uint32_t ValueLength;
            uint16_t ValueOffset;
            uint8_t  IndexedFlag;
            uint8_t  Padding;
        } Resident;
        struct {
            uint64_t LowestVCN;
            uint64_t HighestVCN;
            uint16_t MappingPairsOffset;
            uint8_t  CompressionUnit;
            uint8_t  Padding[5];
            uint64_t AllocatedSize;
            uint64_t DataSize;
            uint64_t InitializedSize;
        } NonResident;
    };
};

static constexpr uint32_t MIN_RESIDENT_ATTR_SIZE = 24;
static constexpr uint32_t MIN_NONRESIDENT_ATTR_SIZE = 64;

struct NtfsFileNameAttribute {
    uint64_t ParentDirectory;
    uint64_t CreationTime;
    uint64_t ModificationTime;
    uint64_t MftModificationTime;
    uint64_t AccessTime;
    uint64_t AllocatedSize;
    uint64_t DataSize;
    uint32_t FileAttributes;
    uint32_t EaOrReparse;
    uint8_t  FileNameLength;
    uint8_t  Namespace;
    wchar_t  FileName[1];
};

struct NtfsAttributeListEntry {
    uint32_t Type;
    uint16_t RecordLength;
    uint8_t  NameLength;
    uint8_t  NameOffset;
    uint64_t LowestVCN;
    uint64_t FileReferenceNumber;
    uint16_t AttributeId;
};
#pragma pack(pop)

enum RecordFlags : uint8_t {
    FLAG_IN_USE = 1 << 0,
    FLAG_IS_DIR = 1 << 1,
    FLAG_IS_REPARSE = 1 << 2,
    FLAG_HAS_DATA = 1 << 3,
    FLAG_HARDLINK_SECONDARY = 1 << 4,
    FLAG_SYSTEM_META = 1 << 5
};

struct MftRecordInfo {
    uint64_t logicalSize = 0;           // 8B
    uint64_t allocatedSize = 0;         // 8B
    uint32_t parentIdx = 0xFFFFFFFF;    // 4B
    int32_t  firstChild = -1;           // 4B
    int32_t  nextSibling = -1;          // 4B
    uint32_t nameOffset = 0;            // 4B
    uint32_t subtreeFiles = 0;          // 4B
    uint16_t nameLen = 0;               // 2B
    uint8_t  nameNamespace = 255;       // 1B
    uint8_t  flags = 0;                 // 1B

    bool IsInUse() const noexcept { return (flags & FLAG_IN_USE) != 0; }
    bool IsDir() const noexcept { return (flags & FLAG_IS_DIR) != 0; }
    bool IsReparse() const noexcept { return (flags & FLAG_IS_REPARSE) != 0; }
    bool HasData() const noexcept { return (flags & FLAG_HAS_DATA) != 0; }
    bool IsHardLinkSecondary() const noexcept { return (flags & FLAG_HARDLINK_SECONDARY) != 0; }
    bool IsSystemMeta() const noexcept { return (flags & FLAG_SYSTEM_META) != 0; }
};

struct ExtraHardLink {
    uint32_t baseRecordIdx;
    uint32_t parentIdx;
    uint32_t nameOffset;
    uint16_t nameLen;
    uint8_t  nameNamespace;
};

struct DeferredExtensionRecord {
    uint32_t extRecordIdx;
    uint32_t baseRecordIdx;
    uint16_t baseSeqNum;
    uint32_t poolOffset;
};

struct MftExtent {
    uint64_t vcnOffset;
    uint64_t diskOffset;
    uint64_t byteLength;
    bool     isSparse;
};

struct ChunkJob {
    uint64_t diskOffset;
    uint32_t bytes;
    uint32_t firstRecord;
};

struct ScanTelemetry {
    std::string engineUsed = "None";
    std::string fallbackReason = "";
    bool isApproximateFallback = false;
    uint32_t chunkSizeMb = 8;
    uint64_t totalMftRecords = 0;
    uint64_t processedMftRecords = 0;
    uint64_t validFiles = 0;
    uint64_t validDirectories = 0;
    uint64_t extraHardLinksCount = 0;
    uint64_t reparsePointsCount = 0;
    uint64_t skippedSystemRecords = 0;
    uint64_t corruptedRecords = 0;
    uint64_t unsupportedAttributes = 0;
    uint64_t accessDeniedDirs = 0;

    uint64_t systemMetaAllocated = 0;
    uint64_t volumeUsedBytes = 0;
    int64_t  unexplainedBytes = 0;
    bool     isFullVolumeScan = false;

    size_t sizeofRecordStruct = sizeof(MftRecordInfo);
    double recordsVectorMb = 0.0;
    double stringArenaMb = 0.0;

    long long readMftMs = 0;
    long long parseRecordsMs = 0;
    long long linkTreeMs = 0;
    long long sumSizesMs = 0;
    long long sortMs = 0;
    long long totalMs = 0;
};

// =====================================================================
// 4. LÕI QUÉT MFT RAW, DOUBLE-BUFFER PIPELINE & BỘ KIỂM THỬ
// =====================================================================
class DiskScanner {
public:
    static bool ValidateRecordHeaderOnly(const NtfsFileRecordHeader* hdr, uint32_t recordSize, uint32_t bytesPerSector) {
        if (!hdr || recordSize < sizeof(NtfsFileRecordHeader) || bytesPerSector == 0) return false;
        if ((recordSize % bytesPerSector) != 0) return false;
        if (hdr->Magic != 0x454C4946) return false;

        if (hdr->UsedSize < sizeof(NtfsFileRecordHeader) ||
            hdr->UsedSize > recordSize ||
            hdr->AllocatedSize != recordSize) {
            return false;
        }

        if (hdr->FirstAttributeOffset < sizeof(NtfsFileRecordHeader) ||
            hdr->FirstAttributeOffset >= hdr->UsedSize ||
            (hdr->FirstAttributeOffset % 8) != 0) {
            return false;
        }

        uint32_t expectedSectors = recordSize / bytesPerSector;
        if (hdr->UpdateSequenceCount != expectedSectors + 1) {
            return false;
        }

        if (hdr->UpdateSequenceOffset < 42 || (hdr->UpdateSequenceOffset % 2) != 0) {
            return false;
        }
        uint32_t usaByteEnd = static_cast<uint32_t>(hdr->UpdateSequenceOffset) +
            static_cast<uint32_t>(hdr->UpdateSequenceCount) * sizeof(uint16_t);
        if (usaByteEnd > hdr->FirstAttributeOffset || usaByteEnd > recordSize) {
            return false;
        }

        return true;
    }

    static bool ApplyUsaFixupAndValidate(uint8_t* recordBuf, uint32_t recordSize, uint32_t bytesPerSector) {
        auto* hdr = reinterpret_cast<NtfsFileRecordHeader*>(recordBuf);
        if (!ValidateRecordHeaderOnly(hdr, recordSize, bytesPerSector)) {
            return false;
        }

        uint16_t* usa = reinterpret_cast<uint16_t*>(recordBuf + hdr->UpdateSequenceOffset);
        uint16_t usn = usa[0];
        uint32_t expectedSectors = recordSize / bytesPerSector;

        for (uint32_t i = 1; i <= expectedSectors; ++i) {
            uint32_t sectorOffset = i * bytesPerSector - sizeof(uint16_t);
            uint16_t* sectorEnd = reinterpret_cast<uint16_t*>(recordBuf + sectorOffset);
            if (*sectorEnd != usn) {
                return false;
            }
        }

        for (uint32_t i = 1; i <= expectedSectors; ++i) {
            uint32_t sectorOffset = i * bytesPerSector - sizeof(uint16_t);
            uint16_t* sectorEnd = reinterpret_cast<uint16_t*>(recordBuf + sectorOffset);
            *sectorEnd = usa[i];
        }
        return true;
    }

    static void AddOrMergeFileNameLink(
        uint32_t targetIdx,
        uint32_t parentFrn,
        uint8_t ns,
        const wchar_t* fileName,
        uint8_t fileNameLen,
        MftRecordInfo& info,
        StringArena& arena,
        std::vector<ExtraHardLink>& extraHardLinks,
        bool isExtensionPhase = false
    ) {
        if (info.nameLen == 0) {
            auto [nOff, nLen] = arena.AppendWString(fileName, fileNameLen);
            info.parentIdx = parentFrn;
            info.nameOffset = nOff;
            info.nameLen = nLen;
            info.nameNamespace = ns;
            return;
        }

        if (info.parentIdx == parentFrn) {
            if (info.nameNamespace == 2 && ns == 1) {
                auto [nOff, nLen] = arena.AppendWString(fileName, fileNameLen);
                info.nameOffset = nOff;
                info.nameLen = nLen;
                info.nameNamespace = 3;
                return;
            }
            if (info.nameNamespace == 1 && ns == 2) {
                info.nameNamespace = 3;
                return;
            }
        }

        for (size_t k = extraHardLinks.size(); k-- > 0;) {
            auto& extLink = extraHardLinks[k];
            if (extLink.baseRecordIdx != targetIdx) {
                if (!isExtensionPhase) break;
                continue;
            }
            if (extLink.parentIdx != parentFrn) continue;

            if (extLink.nameNamespace == 2 && ns == 1) {
                auto [nOff, nLen] = arena.AppendWString(fileName, fileNameLen);
                extLink.nameOffset = nOff;
                extLink.nameLen = nLen;
                extLink.nameNamespace = 3;
                return;
            }
            if (extLink.nameNamespace == 1 && ns == 2) {
                extLink.nameNamespace = 3;
                return;
            }
        }

        auto [nOff, nLen] = arena.AppendWString(fileName, fileNameLen);
        extraHardLinks.push_back({
            targetIdx,
            parentFrn,
            nOff,
            nLen,
            ns
            });
    }

    static bool ExtractAttributesFromValidatedRecord(
        uint32_t targetIdx,
        const uint8_t* recordBuf,
        uint32_t recordSize,
        std::vector<MftRecordInfo>& records,
        StringArena& arena,
        std::vector<ExtraHardLink>& extraHardLinks,
        ScanTelemetry& telemetry,
        bool isExtensionPhase = false
    ) {
        const auto* hdr = reinterpret_cast<const NtfsFileRecordHeader*>(recordBuf);
        uint32_t offset = hdr->FirstAttributeOffset;
        uint32_t limit = hdr->UsedSize;

        MftRecordInfo backupInfo = records[targetIdx];
        size_t backupExtraLinksSize = extraHardLinks.size();
        bool reachedEndMarker = false;

        while (offset + sizeof(uint32_t) <= limit) {
            uint32_t attrType = *reinterpret_cast<const uint32_t*>(recordBuf + offset);
            if (attrType == 0xFFFFFFFF) {
                reachedEndMarker = true;
                break;
            }

            if (offset + MIN_RESIDENT_ATTR_SIZE > limit) break;
            const auto* attr = reinterpret_cast<const NtfsAttributeHeader*>(recordBuf + offset);

            if (attr->Length < MIN_RESIDENT_ATTR_SIZE || (attr->Length % 8) != 0 || offset + attr->Length > limit) {
                break;
            }

            if (attr->NameLength > 0) {
                uint32_t nameEnd = static_cast<uint32_t>(attr->NameOffset) + static_cast<uint32_t>(attr->NameLength) * sizeof(wchar_t);
                if (attr->NameOffset < MIN_RESIDENT_ATTR_SIZE || nameEnd > attr->Length) {
                    break;
                }
            }

            MftRecordInfo& info = records[targetIdx];

            if (attr->IsNonResident == 0) {
                uint32_t valOff = attr->Resident.ValueOffset;
                uint32_t valLen = attr->Resident.ValueLength;
                if (valOff < MIN_RESIDENT_ATTR_SIZE || static_cast<uint64_t>(valOff) + valLen > attr->Length) {
                    break;
                }

                const uint8_t* valPtr = recordBuf + offset + valOff;

                if (attr->Type == 0x10 && valLen >= 36) {
                    uint32_t fileAttrs = *reinterpret_cast<const uint32_t*>(valPtr + 32);
                    if (fileAttrs & FILE_ATTRIBUTE_REPARSE_POINT) {
                        info.flags |= FLAG_IS_REPARSE;
                    }
                }
                else if (attr->Type == 0x30) {
                    if (valLen < offsetof(NtfsFileNameAttribute, FileName)) break;
                    const auto* fn = reinterpret_cast<const NtfsFileNameAttribute*>(valPtr);
                    uint32_t nameBytes = static_cast<uint32_t>(fn->FileNameLength) * sizeof(wchar_t);
                    if (fn->FileNameLength == 0 || offsetof(NtfsFileNameAttribute, FileName) + nameBytes > valLen || fn->Namespace > 3) {
                        break;
                    }
                    if (fn->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                        info.flags |= FLAG_IS_REPARSE;
                    }
                    uint32_t parentFrn = static_cast<uint32_t>(fn->ParentDirectory & 0x0000FFFFFFFFFFFFULL);
                    AddOrMergeFileNameLink(targetIdx, parentFrn, fn->Namespace, fn->FileName, fn->FileNameLength, info, arena, extraHardLinks, isExtensionPhase);
                }
                else if (attr->Type == 0x80 && attr->NameLength == 0) {
                    if (!info.HasData()) {
                        info.logicalSize = valLen;
                        info.allocatedSize = 0;
                        info.flags |= FLAG_HAS_DATA;
                    }
                }
            }
            else {
                if (attr->Length < MIN_NONRESIDENT_ATTR_SIZE ||
                    attr->NonResident.MappingPairsOffset < 64 ||
                    attr->NonResident.MappingPairsOffset > attr->Length) {
                    break;
                }

                if (attr->Type == 0x20) {
                    telemetry.unsupportedAttributes++;
                }
                else if (attr->Type == 0x80 && attr->NameLength == 0 && attr->NonResident.LowestVCN == 0) {
                    uint64_t alloc = attr->NonResident.AllocatedSize;
                    if (attr->NonResident.CompressionUnit != 0 &&
                        attr->Length >= 72 &&
                        attr->NonResident.MappingPairsOffset >= 72) {
                        std::memcpy(&alloc, reinterpret_cast<const uint8_t*>(attr) + 0x40, sizeof(alloc));
                    }
                    info.logicalSize = attr->NonResident.DataSize;
                    info.allocatedSize = alloc;
                    info.flags |= FLAG_HAS_DATA;
                }
            }

            offset += attr->Length;
        }

        if (!reachedEndMarker) {
            records[targetIdx] = backupInfo;
            extraHardLinks.resize(backupExtraLinksSize);
            telemetry.corruptedRecords++;
            return false;
        }

        return true;
    }

    static void ProcessRawMftSlot(
        uint32_t currentRecordIdx,
        uint8_t* recordBuf,
        uint32_t recordSize,
        uint32_t sectorSize,
        std::vector<MftRecordInfo>& records,
        std::vector<uint16_t>& baseSeqNums,
        StringArena& arena,
        std::vector<ExtraHardLink>& extraHardLinks,
        std::vector<DeferredExtensionRecord>& deferredExtensions,
        std::vector<uint8_t>& extensionRawPool,
        ScanTelemetry& telemetry
    ) {
        const auto* rawHdr = reinterpret_cast<const NtfsFileRecordHeader*>(recordBuf);
        if (rawHdr->Magic == 0x00000000) return;
        if (rawHdr->Magic != 0x454C4946) {
            telemetry.corruptedRecords++;
            return;
        }
        if ((rawHdr->Flags & 0x0001) == 0) return;

        if (!ApplyUsaFixupAndValidate(recordBuf, recordSize, sectorSize)) {
            telemetry.corruptedRecords++;
            return;
        }

        const auto* hdr = reinterpret_cast<const NtfsFileRecordHeader*>(recordBuf);
        if (hdr->BaseFileRecordSegment == 0) {
            MftRecordInfo& info = records[currentRecordIdx];
            info.flags |= FLAG_IN_USE;
            if (hdr->Flags & 0x0002) info.flags |= FLAG_IS_DIR;
            if (currentRecordIdx < 24 && currentRecordIdx != 5) {
                info.flags |= FLAG_SYSTEM_META;
            }
            info.subtreeFiles = currentRecordIdx;

            if (ExtractAttributesFromValidatedRecord(currentRecordIdx, recordBuf, recordSize, records, arena, extraHardLinks, telemetry, false)) {
                baseSeqNums[currentRecordIdx] = hdr->SequenceNumber;
            }
            else {
                info = MftRecordInfo{};
            }
        }
        else {
            uint64_t baseRef = hdr->BaseFileRecordSegment;
            uint32_t baseIdx = static_cast<uint32_t>(baseRef & 0x0000FFFFFFFFFFFFULL);
            uint16_t baseSeq = static_cast<uint16_t>((baseRef >> 48) & 0xFFFF);

            if (baseIdx >= records.size() || baseIdx == currentRecordIdx) {
                telemetry.corruptedRecords++;
                return;
            }

            uint32_t poolOff = static_cast<uint32_t>(extensionRawPool.size());
            extensionRawPool.insert(extensionRawPool.end(), recordBuf, recordBuf + recordSize);
            deferredExtensions.push_back({ currentRecordIdx, baseIdx, baseSeq, poolOff });
        }
    }

    static void ApplyDeferredExtensionRecords(
        uint32_t recordSize,
        const std::vector<DeferredExtensionRecord>& deferredExtensions,
        const std::vector<uint8_t>& extensionRawPool,
        std::vector<MftRecordInfo>& records,
        const std::vector<uint16_t>& baseSeqNums,
        StringArena& arena,
        std::vector<ExtraHardLink>& extraHardLinks,
        ScanTelemetry& telemetry
    ) {
        for (const auto& def : deferredExtensions) {
            if (def.baseRecordIdx >= records.size()) {
                telemetry.corruptedRecords++;
                continue;
            }
            const MftRecordInfo& baseInfo = records[def.baseRecordIdx];
            if (!baseInfo.IsInUse()) {
                telemetry.corruptedRecords++;
                continue;
            }
            if (def.baseSeqNum != 0 && baseSeqNums[def.baseRecordIdx] != 0 && def.baseSeqNum != baseSeqNums[def.baseRecordIdx]) {
                telemetry.corruptedRecords++;
                continue;
            }

            ExtractAttributesFromValidatedRecord(
                def.baseRecordIdx,
                extensionRawPool.data() + def.poolOffset,
                recordSize,
                records,
                arena,
                extraHardLinks,
                telemetry,
                true
            );
        }
    }

    static bool DecodeDataRunsWithVcn(
        const uint8_t* runPtr,
        const uint8_t* attrEnd,
        uint64_t lowestVcn,
        uint64_t highestVcn,
        uint64_t bytesPerCluster,
        std::vector<MftExtent>& extents
    ) {
        if (runPtr >= attrEnd || bytesPerCluster == 0 || highestVcn < lowestVcn) {
            return false;
        }

        uint64_t expectedClusters = highestVcn - lowestVcn + 1;
        uint64_t decodedClusters = 0;
        int64_t currentLcn = 0;
        uint64_t currentVcn = lowestVcn;

        while (runPtr < attrEnd && *runPtr != 0x00) {
            uint8_t header = *runPtr++;
            uint8_t lenBytes = header & 0x0F;
            uint8_t offBytes = (header >> 4) & 0x0F;
            if (lenBytes == 0 || lenBytes > 8 || offBytes > 8 || runPtr + lenBytes + offBytes > attrEnd) {
                return false;
            }

            uint64_t clusterCount = 0;
            for (uint8_t i = 0; i < lenBytes; ++i) {
                clusterCount |= (static_cast<uint64_t>(*runPtr++) << (i * 8));
            }
            if (clusterCount == 0 || decodedClusters + clusterCount > expectedClusters) {
                return false;
            }

            if (offBytes == 0) {
                extents.push_back({ currentVcn, 0, clusterCount * bytesPerCluster, true });
            }
            else {
                int64_t lcnDelta = 0;
                for (uint8_t i = 0; i < offBytes; ++i) {
                    lcnDelta |= (static_cast<uint64_t>(*runPtr++) << (i * 8));
                }
                if (runPtr[-1] & 0x80) {
                    for (uint8_t i = offBytes; i < 8; ++i) {
                        lcnDelta |= (static_cast<uint64_t>(0xFF) << (i * 8));
                    }
                }
                currentLcn += lcnDelta;
                if (currentLcn < 0) return false;

                extents.push_back({
                    currentVcn,
                    static_cast<uint64_t>(currentLcn) * bytesPerCluster,
                    clusterCount * bytesPerCluster,
                    false
                    });
            }

            currentVcn += clusterCount;
            decodedClusters += clusterCount;
        }

        if (runPtr >= attrEnd || *runPtr != 0x00 || decodedClusters != expectedClusters) {
            return false;
        }
        return true;
    }

    static bool ReadNonResidentAttributeData(
        HANDLE hVol,
        const NtfsAttributeHeader* attr,
        const uint8_t* attrRawPtr,
        uint64_t clusterSize,
        std::vector<uint8_t>& outData
    ) {
        if (!attr || attr->IsNonResident != 1 || attr->Length < MIN_NONRESIDENT_ATTR_SIZE) return false;
        if (attr->NonResident.MappingPairsOffset < 64 || attr->NonResident.MappingPairsOffset > attr->Length) return false;

        uint64_t dataSize = attr->NonResident.DataSize;
        if (dataSize == 0 || dataSize > 64 * 1024 * 1024) return false;

        std::vector<MftExtent> attrExtents;
        const uint8_t* runPtr = attrRawPtr + attr->NonResident.MappingPairsOffset;
        const uint8_t* attrEnd = attrRawPtr + attr->Length;

        if (!DecodeDataRunsWithVcn(runPtr, attrEnd, attr->NonResident.LowestVCN, attr->NonResident.HighestVCN, clusterSize, attrExtents)) {
            return false;
        }

        uint64_t totalAllocated = 0;
        for (const auto& e : attrExtents) totalAllocated += e.byteLength;
        if (totalAllocated < dataSize) return false;

        outData.resize(static_cast<size_t>(totalAllocated), 0);
        uint64_t writeOffset = 0;

        for (const auto& ext : attrExtents) {
            if (!ext.isSparse) {
                LARGE_INTEGER pos;
                pos.QuadPart = static_cast<LONGLONG>(ext.diskOffset);
                DWORD toRead = static_cast<DWORD>(ext.byteLength);
                DWORD bytesRead = 0;
                if (!SetFilePointerEx(hVol, pos, NULL, FILE_BEGIN) ||
                    !ReadFile(hVol, outData.data() + writeOffset, toRead, &bytesRead, NULL) ||
                    bytesRead != toRead) {
                    return false;
                }
            }
            writeOffset += ext.byteLength;
        }

        outData.resize(static_cast<size_t>(dataSize));
        return true;
    }

    static bool ParseAttributeListBytes(const uint8_t* listPtr, size_t listLen, std::vector<uint64_t>& outExtRecords) {
        size_t listOff = 0;
        while (listOff < listLen) {
            if (listOff + sizeof(NtfsAttributeListEntry) > listLen) return false;
            const auto* entry = reinterpret_cast<const NtfsAttributeListEntry*>(listPtr + listOff);
            if (entry->RecordLength < sizeof(NtfsAttributeListEntry) ||
                (entry->RecordLength % 8) != 0 ||
                listOff + entry->RecordLength > listLen) {
                return false;
            }
            if (entry->Type == 0x80 && entry->NameLength == 0) {
                uint64_t extRecIdx = entry->FileReferenceNumber & 0x0000FFFFFFFFFFFFULL;
                if (extRecIdx != 0 && std::find(outExtRecords.begin(), outExtRecords.end(), extRecIdx) == outExtRecords.end()) {
                    outExtRecords.push_back(extRecIdx);
                }
            }
            listOff += entry->RecordLength;
        }
        return true;
    }

    static bool FetchSingleMftRecordValidated(HANDLE hVol, uint64_t recordIndex, uint32_t recordSize, uint32_t sectorSize, std::vector<uint8_t>& outRecord) {
        NTFS_FILE_RECORD_INPUT_BUFFER inBuf{};
        inBuf.FileReferenceNumber.QuadPart = static_cast<LONGLONG>(recordIndex);

        DWORD outBufSize = static_cast<DWORD>(offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer) + recordSize);
        std::vector<uint8_t> rawOut(outBufSize);
        DWORD bytesRet = 0;

        if (!DeviceIoControl(hVol, FSCTL_GET_NTFS_FILE_RECORD, &inBuf, sizeof(inBuf), rawOut.data(), outBufSize, &bytesRet, NULL)) {
            return false;
        }
        const auto* outBuf = reinterpret_cast<const NTFS_FILE_RECORD_OUTPUT_BUFFER*>(rawOut.data());
        if ((static_cast<uint64_t>(outBuf->FileReferenceNumber.QuadPart) & 0x0000FFFFFFFFFFFFULL) != recordIndex) {
            return false;
        }
        if (outBuf->FileRecordLength < recordSize) return false;

        outRecord.assign(outBuf->FileRecordBuffer, outBuf->FileRecordBuffer + recordSize);
        const auto* hdr = reinterpret_cast<const NtfsFileRecordHeader*>(outRecord.data());
        if (!ValidateRecordHeaderOnly(hdr, recordSize, sectorSize) || (hdr->Flags & 0x0001) == 0) {
            return false;
        }
        return true;
    }

    static bool GetMftExtentsOrdered(
        HANDLE hVol,
        const NTFS_VOLUME_DATA_BUFFER& volData,
        std::vector<MftExtent>& extents,
        std::string& outErr
    ) {
        uint32_t recordSize = volData.BytesPerFileRecordSegment;
        uint32_t sectorSize = volData.BytesPerSector;
        uint64_t clusterSize = volData.BytesPerCluster;
        uint64_t totalMftBytes = static_cast<uint64_t>(volData.MftValidDataLength.QuadPart);

        std::vector<uint8_t> rec0(recordSize);
        if (!FetchSingleMftRecordValidated(hVol, 0, recordSize, sectorSize, rec0)) {
            LARGE_INTEGER pos;
            pos.QuadPart = volData.MftStartLcn.QuadPart * static_cast<LONGLONG>(clusterSize);
            DWORD bytesRead = 0;
            if (!SetFilePointerEx(hVol, pos, NULL, FILE_BEGIN) ||
                !ReadFile(hVol, rec0.data(), recordSize, &bytesRead, NULL) ||
                bytesRead != recordSize) {
                outErr = "MFT_REC0_IO: Không đọc được Record 0 của $MFT.";
                return false;
            }
            if (!ApplyUsaFixupAndValidate(rec0.data(), recordSize, sectorSize)) {
                outErr = "MFT_REC0_USA: Record 0 của $MFT không vượt qua kiểm tra USA Fixup.";
                return false;
            }
        }

        const auto* hdr = reinterpret_cast<const NtfsFileRecordHeader*>(rec0.data());
        uint32_t offset = hdr->FirstAttributeOffset;
        std::vector<uint64_t> extraMftRecords;
        bool rec0EndMarker = false;

        while (offset + sizeof(uint32_t) <= hdr->UsedSize) {
            uint32_t type = *reinterpret_cast<const uint32_t*>(rec0.data() + offset);
            if (type == 0xFFFFFFFF) {
                rec0EndMarker = true;
                break;
            }
            if (offset + MIN_RESIDENT_ATTR_SIZE > hdr->UsedSize) break;
            const auto* attr = reinterpret_cast<const NtfsAttributeHeader*>(rec0.data() + offset);
            if (attr->Length < MIN_RESIDENT_ATTR_SIZE || (attr->Length % 8) != 0 || offset + attr->Length > hdr->UsedSize) {
                break;
            }

            if (attr->Type == 0x80 && attr->NameLength == 0) {
                if (attr->IsNonResident != 1 || attr->Length < MIN_NONRESIDENT_ATTR_SIZE || attr->NonResident.MappingPairsOffset > attr->Length) {
                    outErr = "MFT_DATA_ATTR_ERR: Thuộc tính $DATA của $MFT bị lỗi định dạng.";
                    return false;
                }
                const uint8_t* runPtr = rec0.data() + offset + attr->NonResident.MappingPairsOffset;
                const uint8_t* attrEnd = rec0.data() + offset + attr->Length;
                if (!DecodeDataRunsWithVcn(runPtr, attrEnd, attr->NonResident.LowestVCN, attr->NonResident.HighestVCN, clusterSize, extents)) {
                    outErr = "MFT_DATARUN_ERR: Giải mã Data Runs trong Record 0 của $MFT thất bại.";
                    return false;
                }
            }
            else if (attr->Type == 0x20) {
                if (attr->IsNonResident == 0) {
                    uint32_t listOff = attr->Resident.ValueOffset;
                    uint32_t listLen = attr->Resident.ValueLength;
                    if (listOff < MIN_RESIDENT_ATTR_SIZE || static_cast<uint64_t>(listOff) + listLen > attr->Length) {
                        outErr = "MFT_ATTRLIST_BOUNDS: $ATTRIBUTE_LIST Resident của $MFT vượt quá giới hạn.";
                        return false;
                    }
                    if (!ParseAttributeListBytes(rec0.data() + offset + listOff, listLen, extraMftRecords)) {
                        outErr = "MFT_ATTRLIST_CORRUPT: Nội dung $ATTRIBUTE_LIST của $MFT bị hỏng.";
                        return false;
                    }
                }
                else {
                    std::vector<uint8_t> nonResList;
                    if (!ReadNonResidentAttributeData(hVol, attr, rec0.data() + offset, clusterSize, nonResList) ||
                        !ParseAttributeListBytes(nonResList.data(), nonResList.size(), extraMftRecords)) {
                        outErr = "MFT_NONRES_ATTRLIST_ERR: Không thể đọc/giải mã $ATTRIBUTE_LIST Non-Resident của $MFT.";
                        return false;
                    }
                }
            }
            offset += attr->Length;
        }

        if (!rec0EndMarker) {
            outErr = "MFT_REC0_TRUNCATED: Danh sách thuộc tính trong Record 0 không kết thúc bằng 0xFFFFFFFF.";
            return false;
        }

        for (uint64_t extIdx : extraMftRecords) {
            std::vector<uint8_t> extRec;
            if (!FetchSingleMftRecordValidated(hVol, extIdx, recordSize, sectorSize, extRec)) {
                outErr = "MFT_EXTREC_FETCH_ERR: Không đọc được Extension Record " + std::to_string(extIdx) + " của $MFT.";
                return false;
            }
            const auto* extHdr = reinterpret_cast<const NtfsFileRecordHeader*>(extRec.data());
            if ((extHdr->BaseFileRecordSegment & 0x0000FFFFFFFFFFFFULL) != 0) {
                outErr = "MFT_EXTREC_BASE_MISMATCH: Extension Record " + std::to_string(extIdx) + " không thuộc về $MFT (Record 0).";
                return false;
            }

            uint32_t off = extHdr->FirstAttributeOffset;
            bool extEndMarker = false;
            while (off + sizeof(uint32_t) <= extHdr->UsedSize) {
                uint32_t type = *reinterpret_cast<const uint32_t*>(extRec.data() + off);
                if (type == 0xFFFFFFFF) {
                    extEndMarker = true;
                    break;
                }
                if (off + MIN_RESIDENT_ATTR_SIZE > extHdr->UsedSize) break;
                const auto* attr = reinterpret_cast<const NtfsAttributeHeader*>(extRec.data() + off);
                if (attr->Length < MIN_RESIDENT_ATTR_SIZE || (attr->Length % 8) != 0 || off + attr->Length > extHdr->UsedSize) {
                    break;
                }

                if (attr->Type == 0x80 && attr->NameLength == 0) {
                    if (attr->IsNonResident != 1 || attr->Length < MIN_NONRESIDENT_ATTR_SIZE || attr->NonResident.MappingPairsOffset > attr->Length) {
                        outErr = "MFT_EXTREC_DATA_ERR: Thuộc tính $DATA trong Extension Record " + std::to_string(extIdx) + " không hợp lệ.";
                        return false;
                    }
                    if (!DecodeDataRunsWithVcn(
                        extRec.data() + off + attr->NonResident.MappingPairsOffset,
                        extRec.data() + off + attr->Length,
                        attr->NonResident.LowestVCN,
                        attr->NonResident.HighestVCN,
                        clusterSize,
                        extents)) {
                        outErr = "MFT_EXTREC_DATARUN_ERR: Giải mã Data Runs thất bại tại Extension Record " + std::to_string(extIdx) + ".";
                        return false;
                    }
                }
                off += attr->Length;
            }

            if (!extEndMarker) {
                outErr = "MFT_EXTREC_TRUNCATED: Extension Record " + std::to_string(extIdx) + " của $MFT bị hỏng.";
                return false;
            }
        }

        if (extents.empty()) {
            outErr = "MFT_NO_EXTENTS: Không tìm thấy mảnh dữ liệu nào của $MFT.";
            return false;
        }

        std::sort(extents.begin(), extents.end(), [](const MftExtent& a, const MftExtent& b) {
            return a.vcnOffset < b.vcnOffset;
            });

        uint64_t expectedVcn = 0;
        uint64_t totalExtentBytes = 0;
        for (const auto& ext : extents) {
            if (ext.vcnOffset != expectedVcn || ext.byteLength == 0 || (ext.byteLength % recordSize) != 0) {
                outErr = "MFT_VCN_GAP: Các mảnh VCN của $MFT bị đứt quãng hoặc chồng lấn tại VCN " + std::to_string(ext.vcnOffset);
                return false;
            }
            expectedVcn += (ext.byteLength / clusterSize);
            totalExtentBytes += ext.byteLength;
        }

        if (totalExtentBytes < totalMftBytes) {
            outErr = "MFT_INCOMPLETE_EXTENTS: Tổng dung lượng các Extent (" + std::to_string(totalExtentBytes) +
                " B) nhỏ hơn MftValidDataLength (" + std::to_string(totalMftBytes) + " B).";
            return false;
        }

        return true;
    }

    static void BuildLinksAndComputeSizesBottomUp(
        uint32_t rootRecordIdx,
        uint32_t baseRecordCount,
        std::vector<MftRecordInfo>& records,
        const std::vector<ExtraHardLink>& extraHardLinks,
        std::atomic<bool>& cancelFlag,
        ScanTelemetry& telemetry,
        bool sortByAllocated = false
    ) {
        auto tLink0 = std::chrono::steady_clock::now();

        records.reserve(records.size() + extraHardLinks.size());

        for (const auto& hl : extraHardLinks) {
            if (hl.baseRecordIdx >= baseRecordCount) continue;
            const auto& baseRec = records[hl.baseRecordIdx];
            if (!baseRec.IsInUse() || baseRec.IsDir() || baseRec.IsSystemMeta()) continue;

            MftRecordInfo hlNode{};
            hlNode.logicalSize = baseRec.logicalSize;
            hlNode.allocatedSize = baseRec.allocatedSize;
            hlNode.parentIdx = hl.parentIdx;
            hlNode.nameOffset = hl.nameOffset;
            hlNode.nameLen = hl.nameLen;
            hlNode.nameNamespace = hl.nameNamespace;
            hlNode.subtreeFiles = hl.baseRecordIdx;
            hlNode.flags = FLAG_IN_USE | FLAG_HARDLINK_SECONDARY;
            records.push_back(hlNode);
            telemetry.extraHardLinksCount++;
        }

        uint32_t totalNodes = static_cast<uint32_t>(records.size());
        for (uint32_t i = 0; i < totalNodes; ++i) {
            records[i].firstChild = -1;
            records[i].nextSibling = -1;
        }

        for (uint32_t i = totalNodes; i-- > 0;) {
            if (i == 5) continue;
            const auto& rec = records[i];

            if (rec.IsSystemMeta() || i == 11 || rec.parentIdx == 11) {
                if (i < baseRecordCount && rec.IsInUse()) {
                    telemetry.skippedSystemRecords++;
                    telemetry.systemMetaAllocated += rec.allocatedSize;
                }
                continue;
            }

            if (!rec.IsInUse() || rec.nameLen == 0) continue;

            uint32_t p = rec.parentIdx;
            if (p < baseRecordCount && p != i && records[p].IsInUse() && records[p].IsDir()) {
                records[i].nextSibling = records[p].firstChild;
                records[p].firstChild = static_cast<int32_t>(i);
            }
        }

        auto tLink1 = std::chrono::steady_clock::now();
        telemetry.linkTreeMs = std::chrono::duration_cast<std::chrono::milliseconds>(tLink1 - tLink0).count();

        if (rootRecordIdx >= records.size()) return;

        auto tSum0 = std::chrono::steady_clock::now();
        std::vector<uint32_t> dirOrder;
        dirOrder.reserve(65536);
        std::vector<uint8_t> visitedDir(records.size(), 0);
        std::vector<uint8_t> fileCountedInSubtree(baseRecordCount, 0);

        dirOrder.push_back(rootRecordIdx);
        visitedDir[rootRecordIdx] = 1;

        for (size_t idx = 0; idx < dirOrder.size(); ++idx) {
            if (cancelFlag.load(std::memory_order_relaxed)) return;
            uint32_t u = dirOrder[idx];
            telemetry.validDirectories++;

            int32_t child = records[u].firstChild;
            while (child != -1) {
                uint32_t cIdx = static_cast<uint32_t>(child);
                MftRecordInfo& cRec = records[cIdx];

                if (cRec.IsReparse()) {
                    telemetry.reparsePointsCount++;
                }

                if (cRec.IsDir()) {
                    if (!cRec.IsReparse() && !visitedDir[cIdx]) {
                        visitedDir[cIdx] = 1;
                        dirOrder.push_back(cIdx);
                    }
                }
                else {
                    uint32_t baseIdx = (cIdx < baseRecordCount) ? cIdx : cRec.subtreeFiles;
                    if (baseIdx < baseRecordCount && !fileCountedInSubtree[baseIdx]) {
                        fileCountedInSubtree[baseIdx] = 1;
                        cRec.flags &= ~FLAG_HARDLINK_SECONDARY;
                        telemetry.validFiles++;
                    }
                    else {
                        cRec.flags |= FLAG_HARDLINK_SECONDARY;
                    }
                }
                child = cRec.nextSibling;
            }
        }

        for (auto it = dirOrder.rbegin(); it != dirOrder.rend(); ++it) {
            if (cancelFlag.load(std::memory_order_relaxed)) return;
            uint32_t u = *it;

            uint64_t sumLogical = 0;
            uint64_t sumAllocated = 0;
            uint32_t sumFiles = 0;

            int32_t child = records[u].firstChild;
            while (child != -1) {
                const MftRecordInfo& cRec = records[child];
                if (cRec.IsDir()) {
                    if (!cRec.IsReparse()) {
                        sumLogical += cRec.logicalSize;
                        sumAllocated += cRec.allocatedSize;
                        sumFiles += cRec.subtreeFiles;
                    }
                }
                else {
                    if (!cRec.IsHardLinkSecondary()) {
                        sumLogical += cRec.logicalSize;
                        sumAllocated += cRec.allocatedSize;
                        sumFiles += 1;
                    }
                }
                child = cRec.nextSibling;
            }

            records[u].logicalSize = sumLogical;
            records[u].allocatedSize = sumAllocated;
            records[u].subtreeFiles = sumFiles;
        }

        if (rootRecordIdx == 5 && telemetry.volumeUsedBytes > 0) {
            telemetry.isFullVolumeScan = true;
            uint64_t accountedBytes = records[rootRecordIdx].allocatedSize + telemetry.systemMetaAllocated;
            telemetry.unexplainedBytes = static_cast<int64_t>(telemetry.volumeUsedBytes) - static_cast<int64_t>(accountedBytes);
        }

        auto tSum1 = std::chrono::steady_clock::now();
        telemetry.sumSizesMs = std::chrono::duration_cast<std::chrono::milliseconds>(tSum1 - tSum0).count();

        SortDirectoryChildren(dirOrder, records, cancelFlag, telemetry, sortByAllocated);
    }

    static void SortDirectoryChildren(
        const std::vector<uint32_t>& dirOrder,
        std::vector<MftRecordInfo>& records,
        std::atomic<bool>& cancelFlag,
        ScanTelemetry& telemetry,
        bool sortByAllocated
    ) {
        auto tSort0 = std::chrono::steady_clock::now();
        std::vector<int32_t> childBuf;
        childBuf.reserve(256);

        for (uint32_t u : dirOrder) {
            if (cancelFlag.load(std::memory_order_relaxed)) return;
            int32_t child = records[u].firstChild;
            if (child == -1 || records[child].nextSibling == -1) continue;

            childBuf.clear();
            while (child != -1) {
                childBuf.push_back(child);
                child = records[child].nextSibling;
            }

            std::sort(childBuf.begin(), childBuf.end(), [&records, sortByAllocated](int32_t a, int32_t b) {
                const auto& ra = records[a];
                const auto& rb = records[b];
                uint64_t sa = ra.IsHardLinkSecondary() ? 0 : (sortByAllocated ? ra.allocatedSize : ra.logicalSize);
                uint64_t sb = rb.IsHardLinkSecondary() ? 0 : (sortByAllocated ? rb.allocatedSize : rb.logicalSize);
                if (sa != sb) return sa > sb;
                return a < b;
                });

            records[u].firstChild = childBuf[0];
            for (size_t i = 0; i + 1 < childBuf.size(); ++i) {
                records[childBuf[i]].nextSibling = childBuf[i + 1];
            }
            records[childBuf.back()].nextSibling = -1;
        }

        auto tSort1 = std::chrono::steady_clock::now();
        telemetry.sortMs = std::chrono::duration_cast<std::chrono::milliseconds>(tSort1 - tSort0).count();
    }

    void ExecuteScan(
        const std::string& targetPathUTF8,
        uint32_t chunkSizeMb,
        bool allowFallback,
        std::atomic<bool>& cancelFlag,
        std::atomic<uint64_t>& scannedCount,
        std::atomic<uint64_t>& totalRecordsExpected,
        std::vector<MftRecordInfo>& records,
        StringArena& arena,
        uint32_t& outRootRecordIdx,
        std::wstring& outRootCanonicalPathW,
        ScanTelemetry& telemetry,
        const std::function<void(const std::string&, const std::string&)>& onProgress
    ) {
        auto tStartAll = std::chrono::steady_clock::now();

        std::wstring wTarget = UTF8ToWString(targetPathUTF8);
        std::error_code ec;
        fs::path rootPath = fs::absolute(fs::path(wTarget), ec);
        if (ec || !fs::exists(rootPath, ec) || !fs::is_directory(rootPath, ec)) {
            throw std::runtime_error("Đường dẫn không tồn tại hoặc không phải là thư mục hợp lệ!");
        }

        std::wstring canonicalPath = rootPath.wstring();
        while (canonicalPath.size() > 3 && (canonicalPath.back() == L'\\' || canonicalPath.back() == L'/')) {
            canonicalPath.pop_back();
        }
        outRootCanonicalPathW = canonicalPath;
        telemetry.chunkSizeMb = chunkSizeMb;

        std::string mftFailReason;
        bool mftSuccess = TryScanMftRaw(
            canonicalPath,
            chunkSizeMb,
            false,
            cancelFlag,
            scannedCount,
            totalRecordsExpected,
            records,
            arena,
            outRootRecordIdx,
            telemetry,
            mftFailReason,
            [&](const std::string& msg) {
                if (onProgress) onProgress(msg, "NTFS MFT Raw (Async Pipeline)");
            }
        );

        if (cancelFlag.load(std::memory_order_relaxed)) {
            throw std::runtime_error("Đã hủy quét bởi người dùng.");
        }

        if (!mftSuccess) {
            if (mftFailReason.rfind("IO_ERROR:", 0) == 0 ||
                mftFailReason.rfind("INCOMPLETE_MFT_SCAN:", 0) == 0 ||
                mftFailReason.rfind("MFT_", 0) == 0) {
                throw std::runtime_error("Lỗi toàn vẹn bảng MFT (" + mftFailReason + "). Đã dừng quét để đảm bảo không trả về kết quả thiếu!");
            }
            if (!allowFallback) {
                throw std::runtime_error("Không thể sử dụng MFT Raw Engine: " + mftFailReason + " (Chế độ tự động Fallback đang tắt).");
            }

            telemetry.fallbackReason = mftFailReason;
            telemetry.isApproximateFallback = true;
            telemetry.engineUsed = "Win32 LargeFetch API (Ước lượng Cluster)";
            scannedCount.store(0, std::memory_order_relaxed);
            totalRecordsExpected.store(0, std::memory_order_relaxed);
            records.clear();
            arena.Clear();

            RunWin32FallbackToFlatTree(
                canonicalPath,
                cancelFlag,
                scannedCount,
                records,
                arena,
                outRootRecordIdx,
                telemetry,
                [&](const std::string& msg) {
                    if (onProgress) onProgress(msg, "Win32 LargeFetch API (Fallback)");
                }
            );
        }
        else {
            telemetry.engineUsed = "NTFS MFT Raw (Async Pipeline)";
        }

        if (cancelFlag.load(std::memory_order_relaxed)) {
            throw std::runtime_error("Đã hủy quét bởi người dùng.");
        }

        telemetry.recordsVectorMb = static_cast<double>(records.capacity() * sizeof(MftRecordInfo)) / (1024.0 * 1024.0);
        telemetry.stringArenaMb = static_cast<double>(arena.CapacityBytes()) / (1024.0 * 1024.0);

        auto tEndAll = std::chrono::steady_clock::now();
        telemetry.totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(tEndAll - tStartAll).count();
    }

    static json RunAutomatedSelfTests() {
        json report;
        report["suite"] = "MFT Engine V9.0 Verification Suite (7 Tests: USA, DOS 8.3, Hardlinks, Compressed/Sparse & Volume Reconciliation)";
        report["tests"] = json::array();
        bool allPassed = true;

        auto recordTest = [&](const std::string& name, bool passed, const std::string& details) {
            if (!passed) allPassed = false;
            report["tests"].push_back({
                {"name", name},
                {"passed", passed},
                {"details", details}
                });
            };

        const uint32_t REC_SIZE = 1024;
        const uint32_t SEC_SIZE = 512;

        auto finalizeMockRecord = [&](std::vector<uint8_t>& buf, uint16_t flags, uint16_t seqNum, uint64_t baseRef, uint16_t usaCountOverride = 3) -> uint32_t {
            buf.assign(REC_SIZE, 0);
            auto* hdr = reinterpret_cast<NtfsFileRecordHeader*>(buf.data());
            hdr->Magic = 0x454C4946;
            hdr->UpdateSequenceOffset = 48;
            hdr->UpdateSequenceCount = usaCountOverride;
            hdr->SequenceNumber = seqNum;
            hdr->FirstAttributeOffset = 56;
            hdr->Flags = flags;
            hdr->UsedSize = 56 + 8;
            hdr->AllocatedSize = REC_SIZE;
            hdr->BaseFileRecordSegment = baseRef;
            *reinterpret_cast<uint32_t*>(buf.data() + 56) = 0xFFFFFFFF;
            return 56;
            };

        auto appendFileNameAttr = [&](std::vector<uint8_t>& buf, uint32_t& curOff, uint64_t parentFrn, uint8_t ns, const std::wstring& name, uint32_t fileAttrs = 0) {
            uint32_t valLen = static_cast<uint32_t>(offsetof(NtfsFileNameAttribute, FileName) + name.size() * sizeof(wchar_t));
            uint32_t totalLen = (MIN_RESIDENT_ATTR_SIZE + valLen + 7) & ~7u;
            auto* attr = reinterpret_cast<NtfsAttributeHeader*>(buf.data() + curOff);
            std::memset(attr, 0, totalLen);
            attr->Type = 0x30;
            attr->Length = totalLen;
            attr->IsNonResident = 0;
            attr->Resident.ValueOffset = MIN_RESIDENT_ATTR_SIZE;
            attr->Resident.ValueLength = valLen;

            auto* fn = reinterpret_cast<NtfsFileNameAttribute*>(buf.data() + curOff + MIN_RESIDENT_ATTR_SIZE);
            fn->ParentDirectory = parentFrn;
            fn->FileAttributes = fileAttrs;
            fn->FileNameLength = static_cast<uint8_t>(name.size());
            fn->Namespace = ns;
            std::memcpy(fn->FileName, name.data(), name.size() * sizeof(wchar_t));

            curOff += totalLen;
            *reinterpret_cast<uint32_t*>(buf.data() + curOff) = 0xFFFFFFFF;
            reinterpret_cast<NtfsFileRecordHeader*>(buf.data())->UsedSize = curOff + 8;
            };

        auto appendNonResidentDataAttr = [&](std::vector<uint8_t>& buf, uint32_t& curOff, uint64_t logicalSz, uint64_t allocatedSz, uint8_t compressionUnit = 0, uint64_t compressedSz = 0) {
            uint32_t totalLen = (compressionUnit != 0) ? 80 : 72;
            auto* attr = reinterpret_cast<NtfsAttributeHeader*>(buf.data() + curOff);
            std::memset(attr, 0, totalLen);
            attr->Type = 0x80;
            attr->Length = totalLen;
            attr->IsNonResident = 1;
            attr->NameLength = 0;
            attr->Flags = (compressionUnit != 0) ? 0x0001 : 0;
            attr->NonResident.LowestVCN = 0;
            attr->NonResident.HighestVCN = 1;
            attr->NonResident.MappingPairsOffset = (compressionUnit != 0) ? 72 : 64;
            attr->NonResident.CompressionUnit = compressionUnit;
            attr->NonResident.DataSize = logicalSz;
            attr->NonResident.AllocatedSize = allocatedSz;
            attr->NonResident.InitializedSize = logicalSz;

            if (compressionUnit != 0) {
                std::memcpy(buf.data() + curOff + 0x40, &compressedSz, sizeof(compressedSz));
            }

            curOff += totalLen;
            *reinterpret_cast<uint32_t*>(buf.data() + curOff) = 0xFFFFFFFF;
            reinterpret_cast<NtfsFileRecordHeader*>(buf.data())->UsedSize = curOff + 8;
            };

        auto applyUsaEncodingToRawDiskRecord = [&](std::vector<uint8_t>& buf, uint16_t usnValue = 0xABCD) {
            uint16_t* usa = reinterpret_cast<uint16_t*>(buf.data() + 48);
            usa[0] = usnValue;
            usa[1] = *reinterpret_cast<uint16_t*>(buf.data() + 510);
            usa[2] = *reinterpret_cast<uint16_t*>(buf.data() + 1022);
            *reinterpret_cast<uint16_t*>(buf.data() + 510) = usnValue;
            *reinterpret_cast<uint16_t*>(buf.data() + 1022) = usnValue;
            };

        // TEST 1
        {
            std::vector<uint8_t> recValid, recBadUsaCount, recTornSector;
            uint32_t off1 = finalizeMockRecord(recValid, 0x01, 1, 0, 3);
            appendFileNameAttr(recValid, off1, 5, 1, L"ValidFile.txt");
            *reinterpret_cast<uint16_t*>(recValid.data() + 510) = 0x1122;
            *reinterpret_cast<uint16_t*>(recValid.data() + 1022) = 0x3344;
            applyUsaEncodingToRawDiskRecord(recValid, 0x7F7F);

            recBadUsaCount = recValid;
            reinterpret_cast<NtfsFileRecordHeader*>(recBadUsaCount.data())->UpdateSequenceCount = 2;

            recTornSector = recValid;
            *reinterpret_cast<uint16_t*>(recTornSector.data() + 1022) = 0x9999;

            bool okValid = ApplyUsaFixupAndValidate(recValid.data(), REC_SIZE, SEC_SIZE) &&
                (*reinterpret_cast<uint16_t*>(recValid.data() + 510) == 0x1122) &&
                (*reinterpret_cast<uint16_t*>(recValid.data() + 1022) == 0x3344);
            bool rejectBadCount = !ApplyUsaFixupAndValidate(recBadUsaCount.data(), REC_SIZE, SEC_SIZE);
            bool rejectTorn = !ApplyUsaFixupAndValidate(recTornSector.data(), REC_SIZE, SEC_SIZE);

            recordTest(
                "1. Xác minh giải mã USA (UpdateSequenceCount == sectors + 1 & Torn Write)",
                okValid && rejectBadCount && rejectTorn,
                "Khôi phục đúng byte 510/1022, từ chối UpdateSequenceCount=2 trên bản ghi 1024B và từ chối sector rách."
            );
        }

        // TEST 2
        {
            std::vector<MftRecordInfo> recs(40);
            std::vector<uint16_t> seqs(40, 0);
            StringArena arena;
            std::vector<ExtraHardLink> extraLinks;
            std::vector<DeferredExtensionRecord> defExts;
            std::vector<uint8_t> extPool;
            ScanTelemetry tel;

            std::vector<uint8_t> buf;
            uint32_t off = finalizeMockRecord(buf, 0x01, 1, 0);
            appendFileNameAttr(buf, off, 5, 2, L"PROGRA~1.TXT");
            appendFileNameAttr(buf, off, 5, 1, L"Program_Long_Name.txt");
            appendNonResidentDataAttr(buf, off, 4096, 4096);
            applyUsaEncodingToRawDiskRecord(buf);

            ProcessRawMftSlot(25, buf.data(), REC_SIZE, SEC_SIZE, recs, seqs, arena, extraLinks, defExts, extPool, tel);
            std::string finalName = arena.Get(recs[25].nameOffset, recs[25].nameLen);

            bool passed = (finalName == "Program_Long_Name.txt") &&
                (recs[25].nameNamespace == 3) &&
                extraLinks.empty() &&
                (tel.corruptedRecords == 0);
            recordTest(
                "2. Ghép cặp DOS 8.3 -> Win32 trong cùng thư mục cha (ASCII Fast-path B3)",
                passed,
                "Tên hiển thị: '" + finalName + "', số hard link thừa: " + std::to_string(extraLinks.size())
            );
        }

        // TEST 3
        {
            std::vector<MftRecordInfo> recs(40);
            std::vector<uint16_t> seqs(40, 0);
            StringArena arena;
            std::vector<ExtraHardLink> extraLinks;
            std::vector<DeferredExtensionRecord> defExts;
            std::vector<uint8_t> extPool;
            ScanTelemetry tel;

            std::vector<uint8_t> rootBuf;
            uint32_t rOff = finalizeMockRecord(rootBuf, 0x03, 5, 0);
            appendFileNameAttr(rootBuf, rOff, 5, 3, L".");
            applyUsaEncodingToRawDiskRecord(rootBuf);
            ProcessRawMftSlot(5, rootBuf.data(), REC_SIZE, SEC_SIZE, recs, seqs, arena, extraLinks, defExts, extPool, tel);

            std::vector<uint8_t> fileBuf;
            uint32_t fOff = finalizeMockRecord(fileBuf, 0x01, 1, 0);
            appendFileNameAttr(fileBuf, fOff, 5, 2, L"LINKAL~1.TXT");
            appendFileNameAttr(fileBuf, fOff, 5, 2, L"LINKBE~1.TXT");
            appendFileNameAttr(fileBuf, fOff, 5, 1, L"LinkAlpha_Long.txt");
            appendFileNameAttr(fileBuf, fOff, 5, 1, L"LinkBeta_Long.txt");
            appendNonResidentDataAttr(fileBuf, fOff, 5000000, 8000000);
            applyUsaEncodingToRawDiskRecord(fileBuf);
            ProcessRawMftSlot(26, fileBuf.data(), REC_SIZE, SEC_SIZE, recs, seqs, arena, extraLinks, defExts, extPool, tel);

            std::atomic<bool> cancel{ false };
            BuildLinksAndComputeSizesBottomUp(5, 40, recs, extraLinks, cancel, tel, false);

            std::string name1 = arena.Get(recs[26].nameOffset, recs[26].nameLen);
            std::string name2 = (recs.size() > 40) ? arena.Get(recs[40].nameOffset, recs[40].nameLen) : "";

            bool passed = (extraLinks.size() == 1) &&
                (name1 == "LinkAlpha_Long.txt") &&
                (name2 == "LinkBeta_Long.txt") &&
                (recs[5].logicalSize == 5000000) &&
                (recs[5].allocatedSize == 8000000) &&
                (recs[5].subtreeFiles == 1);

            recordTest(
                "3. Hai Hard Link cùng thư mục cha (Duyệt ngược B1 & xen kẽ DOS 8.3)",
                passed,
                "Hiển thị đủ 2 tên dài ('" + name1 + "', '" + name2 + "'), tổng thư mục cha tính đúng 1 lần: Logical=" +
                std::to_string(recs[5].logicalSize) + " B, Allocated=" + std::to_string(recs[5].allocatedSize) + " B."
            );
        }

        // TEST 4 & 5
        {
            std::vector<MftRecordInfo> recs(50);
            std::vector<uint16_t> seqs(50, 0);
            StringArena arena;
            std::vector<ExtraHardLink> extraLinks;
            std::vector<DeferredExtensionRecord> defExts;
            std::vector<uint8_t> extPool;
            ScanTelemetry tel;

            for (auto [idx, dname] : std::vector<std::pair<uint32_t, std::wstring>>{
                     {5, L"."}, {25, L"DirA"}, {26, L"DirB"}, {27, L"DirC"} }) {
                std::vector<uint8_t> dBuf;
                uint32_t dOff = finalizeMockRecord(dBuf, 0x03, 1, 0);
                appendFileNameAttr(dBuf, dOff, 5, 3, dname);
                applyUsaEncodingToRawDiskRecord(dBuf);
                ProcessRawMftSlot(idx, dBuf.data(), REC_SIZE, SEC_SIZE, recs, seqs, arena, extraLinks, defExts, extPool, tel);
            }

            {
                std::vector<uint8_t> extBuf;
                uint64_t baseRef = (static_cast<uint64_t>(9) << 48) | 30ULL;
                uint32_t eOff = finalizeMockRecord(extBuf, 0x01, 1, baseRef);
                appendFileNameAttr(extBuf, eOff, 26, 2, L"BACKUP~1.BIN");
                appendFileNameAttr(extBuf, eOff, 26, 1, L"Backup_In_DirB.bin");
                applyUsaEncodingToRawDiskRecord(extBuf);
                ProcessRawMftSlot(28, extBuf.data(), REC_SIZE, SEC_SIZE, recs, seqs, arena, extraLinks, defExts, extPool, tel);
            }

            {
                std::vector<uint8_t> extBuf2;
                uint64_t baseRef = (static_cast<uint64_t>(9) << 48) | 30ULL;
                uint32_t eOff2 = finalizeMockRecord(extBuf2, 0x01, 1, baseRef);
                appendFileNameAttr(extBuf2, eOff2, 27, 3, L"RAW83.BIN");
                applyUsaEncodingToRawDiskRecord(extBuf2);
                ProcessRawMftSlot(29, extBuf2.data(), REC_SIZE, SEC_SIZE, recs, seqs, arena, extraLinks, defExts, extPool, tel);
            }

            {
                std::vector<uint8_t> badExtBuf;
                uint64_t badBaseRef = (static_cast<uint64_t>(4) << 48) | 30ULL;
                uint32_t bOff = finalizeMockRecord(badExtBuf, 0x01, 1, badBaseRef);
                appendFileNameAttr(badExtBuf, bOff, 27, 1, L"Ghost_Stale_Link.bin");
                applyUsaEncodingToRawDiskRecord(badExtBuf);
                ProcessRawMftSlot(31, badExtBuf.data(), REC_SIZE, SEC_SIZE, recs, seqs, arena, extraLinks, defExts, extPool, tel);
            }

            {
                std::vector<uint8_t> baseBuf;
                uint32_t bOff = finalizeMockRecord(baseBuf, 0x01, 9, 0);
                appendFileNameAttr(baseBuf, bOff, 25, 1, L"Primary_In_DirA.bin");
                appendFileNameAttr(baseBuf, bOff, 25, 2, L"PRIMAR~1.BIN");
                appendNonResidentDataAttr(baseBuf, bOff, 20000000ULL, 24000000ULL);
                applyUsaEncodingToRawDiskRecord(baseBuf);
                ProcessRawMftSlot(30, baseBuf.data(), REC_SIZE, SEC_SIZE, recs, seqs, arena, extraLinks, defExts, extPool, tel);
            }

            ApplyDeferredExtensionRecords(REC_SIZE, defExts, extPool, recs, seqs, arena, extraLinks, tel);

            std::atomic<bool> cancel{ false };
            BuildLinksAndComputeSizesBottomUp(5, 50, recs, extraLinks, cancel, tel, false);

            bool passedMultiDir = (extraLinks.size() == 2) &&
                (recs[5].logicalSize == 20000000ULL) &&
                (recs[5].allocatedSize == 24000000ULL) &&
                (recs[5].subtreeFiles == 1) &&
                (recs[25].firstChild != -1) &&
                (recs[26].firstChild != -1) &&
                (recs[27].firstChild != -1) &&
                (tel.corruptedRecords == 1);

            recordTest(
                "4. Hard Link đa thư mục cha (DirA, DirB, DirC) + Extension Pool (B6)",
                passedMultiDir,
                "Cả 3 thư mục đều có link hợp lệ, từ chối 1 Extension Record lệch SequenceNumber, tổng Root = 20 MB Logical / 24 MB Allocated."
            );

            ScanTelemetry subTel;
            std::vector<ExtraHardLink> emptyExtra;
            BuildLinksAndComputeSizesBottomUp(26, 50, recs, emptyExtra, cancel, subTel, false);
            bool passedSubfolderScope = (recs[26].logicalSize == 20000000ULL) &&
                (recs[26].allocatedSize == 24000000ULL) &&
                (recs[26].subtreeFiles == 1);
            recordTest(
                "5. Quy tắc phạm vi thư mục con (Subfolder Scope Hard Link)",
                passedSubfolderScope,
                "Khi chỉ quét riêng nhánh DirB, liên kết trong DirB trở thành đại diện duy nhất và nhận đúng 20 MB Logical / 24 MB Allocated."
            );
        }

        // TEST 6 (A1)
        {
            std::vector<MftRecordInfo> recs(30);
            std::vector<uint16_t> seqs(30, 0);
            StringArena arena;
            std::vector<ExtraHardLink> extraLinks;
            std::vector<DeferredExtensionRecord> defExts;
            std::vector<uint8_t> extPool;
            ScanTelemetry tel;

            std::vector<uint8_t> sparseBuf;
            uint32_t sOff = finalizeMockRecord(sparseBuf, 0x01, 1, 0);
            appendFileNameAttr(sparseBuf, sOff, 5, 1, L"SparseVirtualDisk.vhdx");
            appendNonResidentDataAttr(sparseBuf, sOff, 100000000ULL, 100000000ULL, 4, 4194304ULL);
            applyUsaEncodingToRawDiskRecord(sparseBuf);

            ProcessRawMftSlot(25, sparseBuf.data(), REC_SIZE, SEC_SIZE, recs, seqs, arena, extraLinks, defExts, extPool, tel);

            bool passedSparse = (recs[25].logicalSize == 100000000ULL) &&
                (recs[25].allocatedSize == 4194304ULL);
            recordTest(
                "6. [A1] Đọc CompressedSize (offset 0x40) cho file Nén / Sparse",
                passedSparse,
                "File Sparse (Logical=100 MB, VCN Allocated=100 MB, CompressedSize=4 MB) -> Ghi nhận chuẩn xác allocatedSize = " +
                FormatSize(recs[25].allocatedSize) + " thay vì 100 MB."
            );
        }

        // TEST 7 (A3)
        {
            std::vector<MftRecordInfo> recs(30);
            std::vector<uint16_t> seqs(30, 0);
            StringArena arena;
            std::vector<ExtraHardLink> extraLinks;
            std::vector<DeferredExtensionRecord> defExts;
            std::vector<uint8_t> extPool;
            ScanTelemetry tel;
            tel.volumeUsedBytes = 550000000ULL;

            {
                std::vector<uint8_t> mftBuf;
                uint32_t mOff = finalizeMockRecord(mftBuf, 0x01, 1, 0);
                appendFileNameAttr(mftBuf, mOff, 5, 3, L"$MFT");
                appendNonResidentDataAttr(mftBuf, mOff, 100000000ULL, 100000000ULL);
                applyUsaEncodingToRawDiskRecord(mftBuf);
                ProcessRawMftSlot(0, mftBuf.data(), REC_SIZE, SEC_SIZE, recs, seqs, arena, extraLinks, defExts, extPool, tel);
            }
            {
                std::vector<uint8_t> rootBuf;
                uint32_t rOff = finalizeMockRecord(rootBuf, 0x03, 5, 0);
                appendFileNameAttr(rootBuf, rOff, 5, 3, L".");
                applyUsaEncodingToRawDiskRecord(rootBuf);
                ProcessRawMftSlot(5, rootBuf.data(), REC_SIZE, SEC_SIZE, recs, seqs, arena, extraLinks, defExts, extPool, tel);
            }
            {
                std::vector<uint8_t> userBuf;
                uint32_t uOff = finalizeMockRecord(userBuf, 0x01, 1, 0);
                appendFileNameAttr(userBuf, uOff, 5, 1, L"UserDatabase.db");
                appendNonResidentDataAttr(userBuf, uOff, 440000000ULL, 440000000ULL);
                applyUsaEncodingToRawDiskRecord(userBuf);
                ProcessRawMftSlot(25, userBuf.data(), REC_SIZE, SEC_SIZE, recs, seqs, arena, extraLinks, defExts, extPool, tel);
            }

            std::atomic<bool> cancel{ false };
            BuildLinksAndComputeSizesBottomUp(5, 30, recs, extraLinks, cancel, tel, false);

            bool passedReconcile = (tel.systemMetaAllocated == 100000000ULL) &&
                (recs[5].allocatedSize == 440000000ULL) &&
                (tel.unexplainedBytes == 10000000LL);
            recordTest(
                "7. [A3] Đối soát volumeUsedBytes - (root.allocated + systemMetaAllocated)",
                passedReconcile,
                "VolumeUsed=550 MB, RootAllocated=440 MB, SystemMeta($MFT)=100 MB -> Độ lệch chưa giải thích (Index/ADS) = " +
                FormatSignedSize(tel.unexplainedBytes)
            );
        }

        report["allPassed"] = allPassed;
        return report;
    }

    static json RunBenchmark(const std::string& drivePathUTF8) {
        std::wstring wPath = UTF8ToWString(drivePathUTF8);
        if (wPath.size() < 2 || wPath[1] != L':') {
            throw std::runtime_error("Đường dẫn ổ đĩa không hợp lệ để chạy Benchmark.");
        }
        std::wstring volPath = L"\\\\.\\" + wPath.substr(0, 2);

        ScopedHandle hVol(CreateFileW(volPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL));
        if (!hVol.valid()) {
            throw std::runtime_error("Cần quyền Administrator để chạy Benchmark trực tiếp trên Volume " + WStringToUTF8(volPath));
        }

        json result;
        result["drive"] = WStringToUTF8(wPath.substr(0, 2));
        result["tests"] = json::array();

        {
            auto t0 = std::chrono::steady_clock::now();
            MFT_ENUM_DATA_V0 med{};
            med.StartFileReferenceNumber = 0;
            med.LowUsn = 0;
            med.HighUsn = MAXLONGLONG;

            const DWORD BUF_SZ = 64 * 1024;
            std::vector<uint8_t> buf(BUF_SZ);
            DWORD bytesRet = 0;
            uint64_t usnRecordCount = 0;

            while (DeviceIoControl(hVol.get(), FSCTL_ENUM_USN_DATA, &med, sizeof(med), buf.data(), BUF_SZ, &bytesRet, NULL)) {
                if (bytesRet <= sizeof(USN)) break;
                DWORD offset = sizeof(USN);
                while (offset + sizeof(USN_RECORD_V2) <= bytesRet) {
                    auto* rec = reinterpret_cast<USN_RECORD_V2*>(buf.data() + offset);
                    if (rec->RecordLength == 0) break;
                    usnRecordCount++;
                    offset += rec->RecordLength;
                }
                med.StartFileReferenceNumber = *reinterpret_cast<DWORDLONG*>(buf.data());
            }
            auto t1 = std::chrono::steady_clock::now();
            long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

            result["tests"].push_back({
                {"method", "FSCTL_ENUM_USN_DATA (USN_RECORD_V2)"},
                {"elapsedMs", ms},
                {"recordsProcessed", usnRecordCount},
                {"hasFileSize", false},
                {"note", "Khác phạm vi: Chỉ liệt kê danh mục FRN/Tên, KHÔNG có dữ liệu kích thước file (DataSize/CompressedSize)."}
                });
        }
        hVol.Close();

        struct BenchConfig {
            uint32_t mb;
            bool unbuffered;
            std::string label;
        };
        std::vector<BenchConfig> configs = {
            { 4,  false, "NTFS Raw MFT Async (Batch 4 MB Buffered)" },
            { 8,  false, "NTFS Raw MFT Async (Batch 8 MB Buffered)" },
            { 16, false, "NTFS Raw MFT Async (Batch 16 MB Buffered)" },
            { 8,  true,  "NTFS Raw MFT Async (Batch 8 MB Unbuffered 4K-Aligned [B6])" }
        };

        for (const auto& cfg : configs) {
            DiskScanner scanner;
            std::atomic<bool> cancel{ false };
            std::atomic<uint64_t> count{ 0 };
            std::atomic<uint64_t> totalExp{ 0 };
            std::vector<MftRecordInfo> recs;
            StringArena arena;
            uint32_t rootIdx = 5;
            ScanTelemetry tel;
            std::string failReason;

            auto t0 = std::chrono::steady_clock::now();
            bool ok = scanner.TryScanMftRaw(wPath.substr(0, 2) + L"\\", cfg.mb, cfg.unbuffered, cancel, count, totalExp, recs, arena, rootIdx, tel, failReason, nullptr);
            auto t1 = std::chrono::steady_clock::now();
            long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

            result["tests"].push_back({
                {"method", cfg.label},
                {"elapsedMs", ok ? ms : -1},
                {"ioReadMs", tel.readMftMs},
                {"parseMs", tel.parseRecordsMs},
                {"recordsProcessed", tel.processedMftRecords},
                {"hasFileSize", true},
                {"note", ok ? ("Đầy đủ Logical/Allocated/Hardlink | Tổng: " + std::to_string(ms) + "ms (Đợi I/O: " + std::to_string(tel.readMftMs) + "ms, CPU Parse: " + std::to_string(tel.parseRecordsMs) + "ms)") : failReason}
                });
        }

        return result;
    }

private:
    static uint64_t GetClusterSize(const std::wstring& path) {
        wchar_t root[MAX_PATH] = { 0 };
        DWORD spc = 0, bps = 0, fc = 0, tc = 0;
        if (!GetVolumePathNameW(path.c_str(), root, MAX_PATH) ||
            !GetDiskFreeSpaceW(root, &spc, &bps, &fc, &tc)) {
            return 4096;
        }
        uint64_t cluster = static_cast<uint64_t>(spc) * bps;
        return cluster > 0 ? cluster : 4096;
    }

    static bool GetTargetRecordIndex(const std::wstring& folderPath, uint64_t& outRecordIndex) {
        if (folderPath.size() <= 3 && folderPath[1] == L':') {
            outRecordIndex = 5;
            return true;
        }
        ScopedHandle hDir(CreateFileW(
            folderPath.c_str(),
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            NULL
        ));
        if (!hDir.valid()) return false;

        BY_HANDLE_FILE_INFORMATION fileInfo{};
        if (!GetFileInformationByHandle(hDir.get(), &fileInfo)) return false;

        uint64_t frn = (static_cast<uint64_t>(fileInfo.nFileIndexHigh) << 32) | fileInfo.nFileIndexLow;
        outRecordIndex = frn & 0x0000FFFFFFFFFFFFULL;
        return true;
    }

    bool TryScanMftRaw(
        const std::wstring& canonicalPath,
        uint32_t chunkSizeMb,
        bool useUnbufferedIo,
        std::atomic<bool>& cancelFlag,
        std::atomic<uint64_t>& scannedCount,
        std::atomic<uint64_t>& totalRecordsExpected,
        std::vector<MftRecordInfo>& records,
        StringArena& arena,
        uint32_t& outRootRecordIdx,
        ScanTelemetry& telemetry,
        std::string& outFailReason,
        const std::function<void(const std::string&)>& onProgress
    ) {
        if (canonicalPath.size() < 2 || canonicalPath[1] != L':') {
            outFailReason = "NOT_LOCAL_DRIVE: Đường dẫn không phải ổ đĩa cục bộ.";
            return false;
        }

        std::wstring rootDriveW = canonicalPath.substr(0, 3);
        wchar_t fsName[MAX_PATH] = { 0 };
        if (GetVolumeInformationW(rootDriveW.c_str(), NULL, 0, NULL, NULL, NULL, fsName, MAX_PATH)) {
            if (_wcsicmp(fsName, L"NTFS") != 0) {
                outFailReason = "NOT_NTFS: Hệ thống tệp là " + WStringToUTF8(fsName) + " (yêu cầu NTFS).";
                return false;
            }
        }

        uint64_t targetRecordIdx = 5;
        if (!GetTargetRecordIndex(canonicalPath, targetRecordIdx)) {
            outFailReason = "TARGET_FRN_ERR: Không lấy được MFT Record Index của thư mục đích.";
            return false;
        }

        std::wstring volumePath = L"\\\\.\\" + canonicalPath.substr(0, 2);
        DWORD openFlags = FILE_FLAG_SEQUENTIAL_SCAN;
        if (useUnbufferedIo) {
            openFlags |= FILE_FLAG_NO_BUFFERING;
        }

        ScopedHandle hVol(CreateFileW(
            volumePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            openFlags,
            NULL
        ));
        if (!hVol.valid()) {
            DWORD err = GetLastError();
            if (err == ERROR_ACCESS_DENIED) {
                outFailReason = "ACCESS_DENIED: Chưa có quyền Administrator để đọc Raw Volume.";
            }
            else {
                outFailReason = "OPEN_VOL_ERR: Mã lỗi Win32 " + std::to_string(err);
            }
            return false;
        }

        NTFS_VOLUME_DATA_BUFFER volData{};
        DWORD bytesRet = 0;
        if (!DeviceIoControl(hVol.get(), FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &volData, sizeof(volData), &bytesRet, NULL)) {
            outFailReason = "NOT_NTFS_VOL: FSCTL_GET_NTFS_VOLUME_DATA thất bại.";
            return false;
        }

        uint32_t recordSize = volData.BytesPerFileRecordSegment;
        uint32_t sectorSize = volData.BytesPerSector;
        uint64_t clusterSize = volData.BytesPerCluster;
        if (recordSize == 0 || sectorSize == 0 || clusterSize == 0 || (recordSize % sectorSize) != 0) {
            outFailReason = "MFT_INVALID_GEOMETRY: Thông số sector/record của NTFS không hợp lệ.";
            return false;
        }

        if (volData.TotalClusters.QuadPart >= volData.FreeClusters.QuadPart) {
            telemetry.volumeUsedBytes = static_cast<uint64_t>(volData.TotalClusters.QuadPart - volData.FreeClusters.QuadPart) * clusterSize;
        }

        std::vector<MftExtent> extents;
        if (!GetMftExtentsOrdered(hVol.get(), volData, extents, outFailReason)) {
            return false;
        }

        uint64_t totalMftBytes = static_cast<uint64_t>(volData.MftValidDataLength.QuadPart);
        if ((totalMftBytes % recordSize) != 0) {
            outFailReason = "MFT_INVALID_VALID_LENGTH: MftValidDataLength không phải bội số của recordSize.";
            return false;
        }

        uint64_t totalRecords = totalMftBytes / recordSize;
        if (totalRecords == 0 || targetRecordIdx >= totalRecords || totalRecords > 500000000ULL) {
            outFailReason = "MFT_INVALID_RECORD_COUNT: Số lượng bản ghi MFT không hợp lệ.";
            return false;
        }

        const uint32_t clampedMb = std::clamp(chunkSizeMb, 1u, 64u);
        const uint32_t CHUNK_SIZE = (clampedMb * 1024 * 1024 / recordSize) * recordSize;

        std::vector<ChunkJob> jobs;
        jobs.reserve(static_cast<size_t>(totalMftBytes / CHUNK_SIZE) + extents.size());
        uint64_t expectedNextRecordIdx = 0;
        uint64_t sparseRecordsCount = 0;

        for (const auto& ext : extents) {
            uint64_t extStartLogicalBytes = ext.vcnOffset * clusterSize;
            if (extStartLogicalBytes >= totalMftBytes) break;

            uint64_t validBytesInThisExtent = (std::min)(ext.byteLength, totalMftBytes - extStartLogicalBytes);
            if ((validBytesInThisExtent % recordSize) != 0) {
                outFailReason = "MFT_EXTENT_ALIGN_ERR: Độ dài dữ liệu hợp lệ trong Extent không khớp bội số recordSize.";
                return false;
            }

            uint64_t extOffsetBytes = 0;
            while (extOffsetBytes < validBytesInThisExtent) {
                uint64_t logicalBytePos = extStartLogicalBytes + extOffsetBytes;
                uint64_t startRecordIdx = logicalBytePos / recordSize;

                if (startRecordIdx != expectedNextRecordIdx) {
                    outFailReason = "INCOMPLETE_MFT_SCAN: Phát hiện độ lệch chỉ số bản ghi MFT tại " +
                        std::to_string(startRecordIdx) + " (kỳ vọng " + std::to_string(expectedNextRecordIdx) + ").";
                    return false;
                }

                uint64_t remainingInExt = validBytesInThisExtent - extOffsetBytes;
                uint32_t toRead = static_cast<uint32_t>((std::min)(static_cast<uint64_t>(CHUNK_SIZE), remainingInExt));
                toRead = (toRead / recordSize) * recordSize;
                if (toRead == 0) {
                    outFailReason = "INCOMPLETE_MFT_SCAN: Kích thước khối đọc bằng 0 khi chưa quét hết Extent.";
                    return false;
                }

                uint32_t recordsInChunk = toRead / recordSize;
                if (ext.isSparse) {
                    sparseRecordsCount += recordsInChunk;
                }
                else {
                    jobs.push_back({
                        ext.diskOffset + extOffsetBytes,
                        toRead,
                        static_cast<uint32_t>(startRecordIdx)
                        });
                }

                expectedNextRecordIdx += recordsInChunk;
                extOffsetBytes += toRead;
            }
        }

        if (expectedNextRecordIdx != totalRecords) {
            outFailReason = "INCOMPLETE_MFT_SCAN: Tổng số bản ghi trong danh sách Job (" + std::to_string(expectedNextRecordIdx) +
                ") không khớp tổng số bản ghi MFT cần quét (" + std::to_string(totalRecords) + ").";
            return false;
        }

        telemetry.totalMftRecords = totalRecords;
        totalRecordsExpected.store(totalRecords, std::memory_order_relaxed);
        outRootRecordIdx = static_cast<uint32_t>(targetRecordIdx);

        records.assign(static_cast<size_t>(totalRecords), MftRecordInfo{});
        std::vector<uint16_t> baseSeqNums(static_cast<size_t>(totalRecords), 0);
        arena.Clear();
        arena.Reserve(static_cast<size_t>(totalRecords) * 20);
        std::vector<ExtraHardLink> extraHardLinks;
        std::vector<DeferredExtensionRecord> deferredExtensions;
        std::vector<uint8_t> extensionRawPool;
        extensionRawPool.reserve(256 * 1024);

        AlignedBuffer buf[2] = { AlignedBuffer(CHUNK_SIZE, 4096), AlignedBuffer(CHUNK_SIZE, 4096) };

        auto readJob = [&hVol](const ChunkJob& j, uint8_t* dst) -> DWORD {
            LARGE_INTEGER p;
            p.QuadPart = static_cast<LONGLONG>(j.diskOffset);
            DWORD got = 0;
            if (!SetFilePointerEx(hVol.get(), p, NULL, FILE_BEGIN) ||
                !ReadFile(hVol.get(), dst, j.bytes, &got, NULL) ||
                got != j.bytes) {
                DWORD err = GetLastError();
                return err ? err : ERROR_READ_FAULT;
            }
            return 0;
            };

        uint64_t totalRecordsProcessed = sparseRecordsCount;
        scannedCount.store(totalRecordsProcessed, std::memory_order_relaxed);
        auto lastProgressTime = std::chrono::steady_clock::now();

        if (!jobs.empty()) {
            auto fut = std::async(std::launch::async, readJob, std::cref(jobs[0]), buf[0].data());

            for (size_t k = 0; k < jobs.size(); ++k) {
                auto tWait0 = std::chrono::steady_clock::now();
                DWORD ioErr = fut.get();
                auto tWait1 = std::chrono::steady_clock::now();
                telemetry.readMftMs += std::chrono::duration_cast<std::chrono::milliseconds>(tWait1 - tWait0).count();

                if (ioErr != 0) {
                    outFailReason = "IO_ERROR: Đọc đĩa thất bại tại offset " + std::to_string(jobs[k].diskOffset) +
                        " (Win32 Error " + std::to_string(ioErr) + ").";
                    return false;
                }

                if (cancelFlag.load(std::memory_order_relaxed)) {
                    return false;
                }

                if (k + 1 < jobs.size()) {
                    fut = std::async(std::launch::async, readJob, std::cref(jobs[k + 1]), buf[(k + 1) & 1].data());
                }

                auto tParse0 = std::chrono::steady_clock::now();
                const ChunkJob& curJob = jobs[k];
                uint8_t* curBuf = buf[k & 1].data();
                uint32_t recordsInChunk = curJob.bytes / recordSize;

                for (uint32_t r = 0; r < recordsInChunk; ++r) {
                    ProcessRawMftSlot(
                        curJob.firstRecord + r,
                        curBuf + static_cast<size_t>(r) * recordSize,
                        recordSize,
                        sectorSize,
                        records,
                        baseSeqNums,
                        arena,
                        extraHardLinks,
                        deferredExtensions,
                        extensionRawPool,
                        telemetry
                    );
                }
                auto tParse1 = std::chrono::steady_clock::now();
                telemetry.parseRecordsMs += std::chrono::duration_cast<std::chrono::milliseconds>(tParse1 - tParse0).count();

                totalRecordsProcessed += recordsInChunk;
                scannedCount.store(totalRecordsProcessed, std::memory_order_relaxed);

                auto now = std::chrono::steady_clock::now();
                if (onProgress && std::chrono::duration_cast<std::chrono::milliseconds>(now - lastProgressTime).count() >= 150) {
                    lastProgressTime = now;
                    int pct = static_cast<int>((totalRecordsProcessed * 100) / totalRecords);
                    onProgress("Đang đọc & phân tích MFT (" + std::to_string(clampedMb) + "MB batch): " +
                        std::to_string(pct) + "% (" + std::to_string(totalRecordsProcessed) + " / " + std::to_string(totalRecords) + ")");
                }

                if (cancelFlag.load(std::memory_order_relaxed)) {
                    if (fut.valid()) fut.wait();
                    return false;
                }
            }
        }

        telemetry.processedMftRecords = totalRecordsProcessed;
        if (totalRecordsProcessed != totalRecords) {
            outFailReason = "INCOMPLETE_MFT_SCAN: Số bản ghi xử lý (" + std::to_string(totalRecordsProcessed) +
                ") không khớp tổng số bản ghi MFT cần quét (" + std::to_string(totalRecords) + ").";
            return false;
        }

        auto tParseExt0 = std::chrono::steady_clock::now();
        ApplyDeferredExtensionRecords(recordSize, deferredExtensions, extensionRawPool, records, baseSeqNums, arena, extraHardLinks, telemetry);
        auto tParseExt1 = std::chrono::steady_clock::now();
        telemetry.parseRecordsMs += std::chrono::duration_cast<std::chrono::milliseconds>(tParseExt1 - tParseExt0).count();

        deferredExtensions.clear();
        deferredExtensions.shrink_to_fit();
        extensionRawPool.clear();
        extensionRawPool.shrink_to_fit();
        baseSeqNums.clear();
        baseSeqNums.shrink_to_fit();

        if (onProgress) onProgress("Đang liên kết cây thư mục, xử lý Hard Link và cộng dồn dung lượng Bottom-Up...");
        BuildLinksAndComputeSizesBottomUp(
            outRootRecordIdx,
            static_cast<uint32_t>(totalRecords),
            records,
            extraHardLinks,
            cancelFlag,
            telemetry,
            false
        );

        return !cancelFlag.load(std::memory_order_relaxed);
    }

    static void RunWin32FallbackToFlatTree(
        const std::wstring& rootDirPath,
        std::atomic<bool>& cancelFlag,
        std::atomic<uint64_t>& scannedCount,
        std::vector<MftRecordInfo>& records,
        StringArena& arena,
        uint32_t& outRootRecordIdx,
        ScanTelemetry& telemetry,
        const std::function<void(const std::string&)>& onProgress
    ) {
        auto tIo0 = std::chrono::steady_clock::now();
        const uint64_t cluster = GetClusterSize(rootDirPath);

        records.reserve(131072);
        arena.Reserve(131072 * 20);

        fs::path rp(rootDirPath);
        std::wstring rootNameW = rp.filename().wstring();
        if (rootNameW.empty()) rootNameW = rootDirPath;

        MftRecordInfo rootRec{};
        auto [rOff, rLen] = arena.AppendWString(rootNameW.c_str(), rootNameW.size());
        rootRec.nameOffset = rOff;
        rootRec.nameLen = rLen;
        rootRec.flags = FLAG_IN_USE | FLAG_IS_DIR;
        records.push_back(rootRec);
        outRootRecordIdx = 0;

        std::vector<std::pair<uint32_t, std::wstring>> queue;
        queue.push_back({ 0, rootDirPath });

        auto lastProgressTime = std::chrono::steady_clock::now();

        for (size_t qIdx = 0; qIdx < queue.size(); ++qIdx) {
            if (cancelFlag.load(std::memory_order_relaxed)) break;

            uint32_t parentNodeIdx = queue[qIdx].first;
            std::wstring currentDirW = std::move(queue[qIdx].second);

            std::wstring searchPath = currentDirW;
            if (!searchPath.empty() && searchPath.back() != L'\\') searchPath += L'\\';
            searchPath += L"*";

            std::wstring longSearchPath = (searchPath.rfind(L"\\\\?\\", 0) == 0) ? searchPath : (L"\\\\?\\" + searchPath);

            WIN32_FIND_DATAW findData{};
            ScopedFindHandle hFind(FindFirstFileExW(
                longSearchPath.c_str(),
                FindExInfoBasic,
                &findData,
                FindExSearchNameMatch,
                NULL,
                FIND_FIRST_EX_LARGE_FETCH
            ));

            if (!hFind.valid()) {
                if (GetLastError() == ERROR_ACCESS_DENIED) {
                    telemetry.accessDeniedDirs++;
                }
                continue;
            }

            uint64_t localCount = 0;
            do {
                if (cancelFlag.load(std::memory_order_relaxed)) break;
                if (findData.cFileName[0] == L'.' &&
                    (findData.cFileName[1] == L'\0' || (findData.cFileName[1] == L'.' && findData.cFileName[2] == L'\0'))) {
                    continue;
                }

                localCount++;
                bool isDir = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                bool isReparse = (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

                size_t nameWLen = wcslen(findData.cFileName);
                auto [nOff, nLen] = arena.AppendWString(findData.cFileName, nameWLen);

                uint32_t newChildIdx = static_cast<uint32_t>(records.size());

                MftRecordInfo childRec{};
                childRec.parentIdx = parentNodeIdx;
                childRec.nameOffset = nOff;
                childRec.nameLen = nLen;
                childRec.subtreeFiles = newChildIdx;
                childRec.flags = FLAG_IN_USE;
                if (isDir) childRec.flags |= FLAG_IS_DIR;
                if (isReparse) childRec.flags |= FLAG_IS_REPARSE;

                if (!isDir) {
                    uint64_t fsize = (static_cast<uint64_t>(findData.nFileSizeHigh) << 32) | findData.nFileSizeLow;
                    childRec.logicalSize = fsize;
                    childRec.allocatedSize = (fsize == 0) ? 0 : ((fsize + cluster - 1) / cluster) * cluster;
                    childRec.flags |= FLAG_HAS_DATA;
                }

                records.push_back(childRec);

                if (isDir && !isReparse) {
                    std::wstring childPathW = currentDirW;
                    if (!childPathW.empty() && childPathW.back() != L'\\') childPathW += L'\\';
                    childPathW += findData.cFileName;
                    queue.push_back({ newChildIdx, std::move(childPathW) });
                }
            } while (FindNextFileW(hFind.get(), &findData) != 0);

            uint64_t totalNow = scannedCount.fetch_add(localCount, std::memory_order_relaxed) + localCount;
            auto now = std::chrono::steady_clock::now();
            if (onProgress && std::chrono::duration_cast<std::chrono::milliseconds>(now - lastProgressTime).count() >= 150) {
                lastProgressTime = now;
                onProgress("Đang quét Win32 Fallback (" + std::to_string(totalNow) + " mục): " + WStringToUTF8(currentDirW));
            }
        }

        telemetry.totalMftRecords = records.size();
        telemetry.processedMftRecords = records.size();
        auto tIo1 = std::chrono::steady_clock::now();
        telemetry.readMftMs = std::chrono::duration_cast<std::chrono::milliseconds>(tIo1 - tIo0).count();

        std::vector<ExtraHardLink> noExtraLinks;
        BuildLinksAndComputeSizesBottomUp(
            outRootRecordIdx,
            static_cast<uint32_t>(records.size()),
            records,
            noExtraLinks,
            cancelFlag,
            telemetry,
            false
        );
    }
};

// =====================================================================
// 5. QUẢN LÝ TRẠNG THÁI VỚI SHARED_MUTEX & LRU CHILD CACHE
// =====================================================================
class ScanManager {
public:
    std::atomic<bool> isScanning{ false };
    std::atomic<bool> cancelFlag{ false };
    std::atomic<uint64_t> scannedCount{ 0 };
    std::atomic<uint64_t> totalRecordsExpected{ 0 };

    mutable std::shared_mutex stateMutex;
    std::string statusState{ "idle" };
    std::string currentProgressText;
    std::string activeEngineText{ "—" };
    std::string errorMsg;
    std::chrono::steady_clock::time_point scanStartTime;

    std::vector<MftRecordInfo> records;
    StringArena arena;
    uint32_t rootRecordIdx = 0;
    std::wstring rootCanonicalPathW;
    ScanTelemetry telemetry;

    ~ScanManager() {
        cancelFlag.store(true, std::memory_order_relaxed);
        if (scanThread_.joinable()) scanThread_.join();
    }

    bool StartAsync(const std::string& path, uint32_t chunkSizeMb, bool allowFallback) {
        bool expected = false;
        if (!isScanning.compare_exchange_strong(expected, true)) {
            return false;
        }
        if (scanThread_.joinable()) scanThread_.join();

        cancelFlag.store(false, std::memory_order_relaxed);
        scannedCount.store(0, std::memory_order_relaxed);
        totalRecordsExpected.store(0, std::memory_order_relaxed);

        {
            std::unique_lock<std::shared_mutex> lock(stateMutex);
            statusState = "scanning";
            currentProgressText = "Đang khởi tạo trình quét...";
            activeEngineText = (IsUserAnAdmin() != FALSE) ? "NTFS MFT Raw (Đang khởi tạo)" : "Win32 LargeFetch API";
            errorMsg.clear();
            scanStartTime = std::chrono::steady_clock::now();
            telemetry = ScanTelemetry{};
            ClearOrderCache();
        }

        scanThread_ = std::thread([this, path, chunkSizeMb, allowFallback]() {
            DiskScanner scanner;
            std::vector<MftRecordInfo> localRecords;
            StringArena localArena;
            uint32_t localRootIdx = 0;
            std::wstring localRootPathW;
            ScanTelemetry localTel;

            try {
                auto progressCb = [this](const std::string& msg, const std::string& eng) {
                    std::unique_lock<std::shared_mutex> lock(this->stateMutex);
                    this->currentProgressText = msg;
                    this->activeEngineText = eng;
                    };

                scanner.ExecuteScan(
                    path,
                    chunkSizeMb,
                    allowFallback,
                    cancelFlag,
                    scannedCount,
                    totalRecordsExpected,
                    localRecords,
                    localArena,
                    localRootIdx,
                    localRootPathW,
                    localTel,
                    progressCb
                );

                std::unique_lock<std::shared_mutex> lock(stateMutex);
                if (cancelFlag.load(std::memory_order_relaxed)) {
                    statusState = "cancelled";
                }
                else {
                    records = std::move(localRecords);
                    arena = std::move(localArena);
                    rootRecordIdx = localRootIdx;
                    rootCanonicalPathW = std::move(localRootPathW);
                    telemetry = localTel;
                    activeEngineText = localTel.engineUsed;
                    ClearOrderCache();
                    statusState = "done";
                }
            }
            catch (const std::exception& e) {
                std::unique_lock<std::shared_mutex> lock(stateMutex);
                if (cancelFlag.load(std::memory_order_relaxed)) {
                    statusState = "cancelled";
                }
                else {
                    errorMsg = e.what();
                    statusState = "error";
                }
            }
            isScanning.store(false, std::memory_order_relaxed);
            });
        return true;
    }

    long long GetRunningElapsedMs() const {
        if (statusState == "scanning") {
            auto now = std::chrono::steady_clock::now();
            return std::chrono::duration_cast<std::chrono::milliseconds>(now - scanStartTime).count();
        }
        return telemetry.totalMs;
    }

    std::wstring ReconstructFullPathW(uint32_t nodeId) const {
        if (nodeId >= records.size()) return L"";
        if (nodeId == rootRecordIdx) return rootCanonicalPathW;

        std::vector<std::string> parts;
        uint32_t curr = nodeId;
        int guard = 0;
        while (curr != rootRecordIdx && curr < records.size() && guard < 1024) {
            const auto& r = records[curr];
            parts.push_back(arena.Get(r.nameOffset, r.nameLen));
            if (r.parentIdx == curr || r.parentIdx >= records.size()) break;
            curr = r.parentIdx;
            guard++;
        }
        if (curr != rootRecordIdx) return L"";

        std::wstring fullW = rootCanonicalPathW;
        for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
            if (!fullW.empty() && fullW.back() != L'\\') fullW += L'\\';
            fullW += UTF8ToWString(*it);
        }
        return fullW;
    }

    json BuildNodeSummaryJson(uint32_t nodeId, bool isRoot = false) const {
        const auto& r = records[nodeId];
        json j;
        j["id"] = nodeId;
        j["parentId"] = (nodeId == rootRecordIdx) ? nodeId : r.parentIdx;
        j["rootId"] = rootRecordIdx;

        if (isRoot) {
            fs::path p(rootCanonicalPathW);
            std::wstring fn = p.filename().wstring();
            j["name"] = WStringToUTF8(fn.empty() ? rootCanonicalPathW : fn);
        }
        else {
            j["name"] = arena.Get(r.nameOffset, r.nameLen);
        }

        bool isSecondaryHl = r.IsHardLinkSecondary();
        uint64_t countedLogical = isSecondaryHl ? 0ULL : r.logicalSize;
        uint64_t countedAllocated = isSecondaryHl ? 0ULL : r.allocatedSize;

        j["logicalSize"] = countedLogical;
        j["allocatedSize"] = countedAllocated;
        j["logicalText"] = FormatSize(countedLogical);
        j["allocatedText"] = FormatSize(countedAllocated);

        j["refLogicalSize"] = r.logicalSize;
        j["refAllocatedSize"] = r.allocatedSize;
        j["refLogicalText"] = FormatSize(r.logicalSize);
        j["refAllocatedText"] = FormatSize(r.allocatedSize);

        j["is_dir"] = r.IsDir();
        j["is_reparse"] = r.IsReparse();
        j["is_hardlink"] = isSecondaryHl;
        j["has_data"] = r.HasData();
        j["subtreeFiles"] = r.IsDir() ? r.subtreeFiles : (isSecondaryHl ? 0u : 1u);
        j["hasChildren"] = (r.firstChild != -1);
        return j;
    }

    // sortMode: 0 = logical desc, 1 = allocated desc, 2 = name asc
    json GetChildrenJson(uint32_t dirId, size_t offset, size_t limit, int sortMode) const {
        json res;
        if (dirId >= records.size() || !records[dirId].IsDir()) {
            res["items"] = json::array();
            res["totalChildren"] = 0;
            res["hasMore"] = false;
            return res;
        }

        auto order = GetOrder(dirId, sortMode);
        size_t totalChildren = order->size();
        json items = json::array();

        for (size_t i = offset; i < totalChildren && items.size() < limit; ++i) {
            items.push_back(BuildNodeSummaryJson((*order)[i], false));
        }

        res["dir"] = BuildNodeSummaryJson(dirId, dirId == rootRecordIdx);
        res["parentId"] = dirId;
        res["parentLogicalSize"] = records[dirId].logicalSize;
        res["parentAllocatedSize"] = records[dirId].allocatedSize;
        res["offset"] = offset;
        res["limit"] = limit;
        res["totalChildren"] = totalChildren;
        res["hasMore"] = (offset + items.size() < totalChildren);
        res["items"] = std::move(items);
        return res;
    }

    json GetTelemetryJson() const {
        json t;
        t["engineUsed"] = telemetry.engineUsed;
        t["fallbackReason"] = telemetry.fallbackReason;
        t["isApproximateFallback"] = telemetry.isApproximateFallback;
        t["chunkSizeMb"] = telemetry.chunkSizeMb;
        t["totalMftRecords"] = telemetry.totalMftRecords;
        t["processedMftRecords"] = telemetry.processedMftRecords;
        t["validFiles"] = telemetry.validFiles;
        t["validDirectories"] = telemetry.validDirectories;
        t["extraHardLinksCount"] = telemetry.extraHardLinksCount;
        t["reparsePointsCount"] = telemetry.reparsePointsCount;
        t["skippedSystemRecords"] = telemetry.skippedSystemRecords;
        t["corruptedRecords"] = telemetry.corruptedRecords;
        t["unsupportedAttributes"] = telemetry.unsupportedAttributes;
        t["accessDeniedDirs"] = telemetry.accessDeniedDirs;

        t["systemMetaAllocated"] = telemetry.systemMetaAllocated;
        t["systemMetaAllocatedText"] = FormatSize(telemetry.systemMetaAllocated);
        t["volumeUsedBytes"] = telemetry.volumeUsedBytes;
        t["volumeUsedText"] = FormatSize(telemetry.volumeUsedBytes);
        t["unexplainedBytes"] = telemetry.unexplainedBytes;
        t["unexplainedText"] = FormatSignedSize(telemetry.unexplainedBytes);
        t["isFullVolumeScan"] = telemetry.isFullVolumeScan;

        t["sizeofRecordStruct"] = telemetry.sizeofRecordStruct;
        t["recordsVectorMb"] = telemetry.recordsVectorMb;
        t["stringArenaMb"] = telemetry.stringArenaMb;
        t["readMftMs"] = telemetry.readMftMs;
        t["parseRecordsMs"] = telemetry.parseRecordsMs;
        t["linkTreeMs"] = telemetry.linkTreeMs;
        t["sumSizesMs"] = telemetry.sumSizesMs;
        t["sortMs"] = telemetry.sortMs;
        t["totalMs"] = telemetry.totalMs;
        return t;
    }

private:
    std::thread scanThread_;

    mutable std::mutex cacheMx_;
    mutable std::list<std::pair<uint64_t, std::shared_ptr<std::vector<uint32_t>>>> cache_;

    void ClearOrderCache() const {
        std::lock_guard<std::mutex> g(cacheMx_);
        cache_.clear();
    }

    std::shared_ptr<std::vector<uint32_t>> GetOrder(uint32_t dir, int sortMode) const {
        uint64_t key = (static_cast<uint64_t>(dir) << 2) | static_cast<uint64_t>(sortMode & 3);
        {
            std::lock_guard<std::mutex> g(cacheMx_);
            for (auto it = cache_.begin(); it != cache_.end(); ++it) {
                if (it->first == key) {
                    cache_.splice(cache_.begin(), cache_, it);
                    return it->second;
                }
            }
        }

        auto v = std::make_shared<std::vector<uint32_t>>();
        for (int32_t c = records[dir].firstChild; c != -1; c = records[c].nextSibling) {
            v->push_back(static_cast<uint32_t>(c));
        }

        if (sortMode == 1) {
            std::sort(v->begin(), v->end(), [this](uint32_t a, uint32_t b) {
                const auto& ra = records[a];
                const auto& rb = records[b];
                uint64_t sa = ra.IsHardLinkSecondary() ? 0 : ra.allocatedSize;
                uint64_t sb = rb.IsHardLinkSecondary() ? 0 : rb.allocatedSize;
                if (sa != sb) return sa > sb;
                return a < b;
                });
        }
        else if (sortMode == 2) {
            std::sort(v->begin(), v->end(), [this](uint32_t a, uint32_t b) {
                const auto& ra = records[a];
                const auto& rb = records[b];
                if (ra.IsDir() != rb.IsDir()) return ra.IsDir() > rb.IsDir();
                std::string_view na = arena.GetView(ra.nameOffset, ra.nameLen);
                std::string_view nb = arena.GetView(rb.nameOffset, rb.nameLen);
                return na < nb;
                });
        }

        std::lock_guard<std::mutex> g(cacheMx_);
        cache_.emplace_front(key, v);
        if (cache_.size() > 24) {
            cache_.pop_back();
        }
        return v;
    }
};

// =====================================================================
// 6. GIAO DIỆN WEB CHUYÊN NGHIỆP (CHỐNG XSS 100%, TREE-TABLE, INSPECTOR)
// =====================================================================
std::string BuildWebGuiHtml(const std::string& sessionToken) {
    std::string html = R"=====(
<!DOCTYPE html>
<html lang="vi">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>NTFS Disk Space Analyzer</title>
    <style>
        :root {
            --bg-app: #141619;
            --bg-panel: #1c1f24;
            --bg-elevated: #252930;
            --bg-hover: #2c313a;
            --bg-selected: #1e3a5f;
            --border-color: #31363f;
            --text-main: #e6edf3;
            --text-muted: #8b949e;
            --accent: #2f81f7;
            --accent-hover: #388bfd;
            --success: #3fb950;
            --warning: #d29922;
            --danger: #f85149;
            --purple: #a371f7;
        }
        * { box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', -apple-system, BlinkMacSystemFont, Roboto, sans-serif;
            background: var(--bg-app);
            color: var(--text-main);
            margin: 0;
            padding: 12px 16px;
            font-size: 13.5px;
            line-height: 1.45;
        }
        .app-shell {
            max-width: 1480px;
            margin: 0 auto;
            display: flex;
            flex-direction: column;
            gap: 12px;
        }

        /* A. Top Toolbar */
        .toolbar-panel {
            background: var(--bg-panel);
            border: 1px solid var(--border-color);
            border-radius: 6px;
            padding: 10px 14px;
            display: flex;
            flex-direction: column;
            gap: 10px;
        }
        .toolbar-top {
            display: flex;
            justify-content: space-between;
            align-items: center;
            flex-wrap: wrap;
            gap: 10px;
        }
        .brand-title {
            display: flex;
            align-items: center;
            gap: 10px;
            font-size: 16px;
            font-weight: 700;
            color: #fff;
        }
        .status-badges {
            display: flex;
            align-items: center;
            gap: 8px;
            flex-wrap: wrap;
        }
        .pill-badge {
            padding: 4px 10px;
            border-radius: 4px;
            font-size: 12px;
            font-weight: 600;
            border: 1px solid transparent;
        }
        .pill-idle { background: var(--bg-elevated); color: var(--text-muted); border-color: var(--border-color); }
        .pill-scanning { background: rgba(47, 129, 247, 0.16); color: #58a6ff; border-color: var(--accent); }
        .pill-success { background: rgba(63, 185, 80, 0.16); color: var(--success); border-color: var(--success); }
        .pill-warn { background: rgba(210, 153, 34, 0.16); color: var(--warning); border-color: var(--warning); }
        .pill-danger { background: rgba(248, 81, 73, 0.16); color: var(--danger); border-color: var(--danger); }

        .toolbar-controls {
            display: flex;
            gap: 8px;
            flex-wrap: wrap;
            align-items: center;
        }
        .drive-quick-btns {
            display: flex;
            gap: 4px;
        }
        .btn-quick-drive {
            background: var(--bg-elevated);
            color: var(--text-main);
            border: 1px solid var(--border-color);
            padding: 6px 10px;
            font-size: 12.5px;
            border-radius: 4px;
            cursor: pointer;
        }
        .btn-quick-drive:hover:not(:disabled) { background: var(--bg-hover); border-color: #555; }
        input[type="text"], select {
            padding: 7px 10px;
            background: var(--bg-elevated);
            border: 1px solid var(--border-color);
            color: var(--text-main);
            border-radius: 4px;
            font-size: 13px;
            outline: none;
        }
        input[type="text"]:focus, select:focus { border-color: var(--accent); }
        #pathInput { flex: 1; min-width: 220px; }
        label.chk-inline {
            display: flex;
            align-items: center;
            gap: 5px;
            font-size: 12.5px;
            color: var(--text-muted);
            cursor: pointer;
            user-select: none;
            padding: 0 4px;
        }
        button.btn-primary, button.btn-secondary, button.btn-danger, button.btn-elevate {
            padding: 7px 14px;
            border-radius: 4px;
            font-weight: 600;
            font-size: 13px;
            cursor: pointer;
            border: 1px solid transparent;
            transition: background 0.12s;
        }
        button:disabled { opacity: 0.45; cursor: not-allowed; }
        .btn-primary { background: var(--accent); color: #fff; }
        .btn-primary:hover:not(:disabled) { background: var(--accent-hover); }
        .btn-secondary { background: var(--bg-elevated); color: var(--text-main); border-color: var(--border-color); }
        .btn-secondary:hover:not(:disabled) { background: var(--bg-hover); }
        .btn-danger { background: #da3633; color: #fff; display: none; }
        .btn-danger:hover:not(:disabled) { background: var(--danger); }
        .btn-elevate { background: #9e6a03; color: #fff; font-size: 12px; padding: 4px 10px; }
        .btn-elevate:hover:not(:disabled) { background: #bb8009; }

        /* B. Scan Status Area */
        .status-panel {
            background: var(--bg-panel);
            border: 1px solid var(--border-color);
            border-radius: 6px;
            padding: 10px 14px;
            display: flex;
            flex-direction: column;
            gap: 6px;
        }
        .status-header-row {
            display: flex;
            justify-content: space-between;
            align-items: center;
            gap: 12px;
            flex-wrap: wrap;
        }
        .status-headline { font-weight: 600; font-size: 13.5px; }
        .status-meta-row {
            display: flex;
            gap: 16px;
            font-size: 12.5px;
            color: var(--text-muted);
            flex-wrap: wrap;
        }
        .progress-track {
            height: 6px;
            background: var(--bg-elevated);
            border-radius: 3px;
            overflow: hidden;
            position: relative;
        }
        .progress-fill {
            height: 100%;
            width: 0%;
            background: var(--accent);
            transition: width 0.2s ease;
        }
        .progress-indeterminate {
            width: 35% !important;
            animation: indeterm 1.2s infinite ease-in-out;
        }
        @keyframes indeterm {
            0% { transform: translateX(-100%); }
            100% { transform: translateX(300%); }
        }
        .status-subtext {
            font-size: 12px;
            color: var(--text-muted);
            white-space: nowrap;
            overflow: hidden;
            text-overflow: ellipsis;
        }
        .banner-list { display: flex; flex-direction: column; gap: 6px; margin-top: 2px; }
        .notice-banner {
            padding: 7px 10px;
            border-radius: 4px;
            font-size: 12.5px;
            border: 1px solid transparent;
        }
        .notice-warn { background: rgba(210, 153, 34, 0.12); color: #e3b341; border-color: rgba(210, 153, 34, 0.4); }
        .notice-danger { background: rgba(248, 81, 73, 0.12); color: #ff7b72; border-color: rgba(248, 81, 73, 0.4); }
        .notice-info { background: rgba(47, 129, 247, 0.12); color: #79c0ff; border-color: rgba(47, 129, 247, 0.4); }

        /* C. Overview Cards */
        .overview-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(230px, 1fr));
            gap: 10px;
        }
        .metric-card {
            background: var(--bg-panel);
            border: 1px solid var(--border-color);
            border-radius: 6px;
            padding: 10px 14px;
        }
        .metric-label { font-size: 12px; color: var(--text-muted); }
        .metric-value { font-size: 18px; font-weight: 700; margin: 4px 0 2px 0; color: #fff; }
        .metric-note { font-size: 11.5px; color: var(--text-muted); }

        /* D. Top Largest Sub-items Bar Strip */
        .top-items-panel {
            background: var(--bg-panel);
            border: 1px solid var(--border-color);
            border-radius: 6px;
            padding: 10px 14px;
        }
        .section-title {
            font-size: 12.5px;
            font-weight: 600;
            color: var(--text-muted);
            text-transform: uppercase;
            letter-spacing: 0.4px;
            margin-bottom: 8px;
            display: flex;
            justify-content: space-between;
            align-items: center;
        }
        .top-items-grid {
            display: grid;
            grid-template-columns: repeat(auto-fill, minmax(210px, 1fr));
            gap: 8px;
        }
        .top-item-card {
            background: var(--bg-elevated);
            border: 1px solid var(--border-color);
            border-radius: 4px;
            padding: 7px 10px;
            cursor: pointer;
            position: relative;
            overflow: hidden;
        }
        .top-item-card:hover { border-color: var(--accent); }
        .top-item-bar {
            position: absolute;
            left: 0; top: 0; bottom: 0;
            background: rgba(47, 129, 247, 0.16);
            pointer-events: none;
        }
        .top-item-head {
            position: relative;
            display: flex;
            justify-content: space-between;
            gap: 6px;
            font-weight: 600;
            font-size: 12.5px;
        }
        .top-item-name { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
        .top-item-sub {
            position: relative;
            font-size: 11.5px;
            color: var(--text-muted);
            display: flex;
            justify-content: space-between;
            margin-top: 2px;
        }

        /* E & F. Main Workspace: Tree-Table + Inspector Panel */
        .workspace-split {
            display: grid;
            grid-template-columns: 1fr 330px;
            gap: 12px;
            align-items: start;
        }
        @media (max-width: 1024px) {
            .workspace-split { grid-template-columns: 1fr; }
        }
        .explorer-panel, .inspector-panel {
            background: var(--bg-panel);
            border: 1px solid var(--border-color);
            border-radius: 6px;
            overflow: hidden;
        }
        .explorer-bar {
            padding: 8px 12px;
            border-bottom: 1px solid var(--border-color);
            display: flex;
            justify-content: space-between;
            align-items: center;
            gap: 8px;
            flex-wrap: wrap;
            background: var(--bg-elevated);
        }
        .breadcrumb-bar {
            display: flex;
            align-items: center;
            gap: 4px;
            flex-wrap: wrap;
            font-size: 12.5px;
            flex: 1;
            min-width: 200px;
        }
        .crumb-btn {
            background: transparent;
            color: #58a6ff;
            border: none;
            padding: 3px 6px;
            border-radius: 3px;
            cursor: pointer;
            font-weight: 600;
            font-size: 12.5px;
        }
        .crumb-btn:hover { background: var(--bg-hover); }
        .crumb-sep { color: var(--text-muted); }

        .filter-bar {
            padding: 8px 12px;
            border-bottom: 1px solid var(--border-color);
            display: flex;
            gap: 8px;
            flex-wrap: wrap;
            align-items: center;
        }
        #searchInput { flex: 1; min-width: 180px; }

        /* Tree Table */
        .table-scroll {
            max-height: 620px;
            overflow: auto;
        }
        table.tree-table {
            width: 100%;
            border-collapse: collapse;
            table-layout: fixed;
        }
        table.tree-table th {
            position: sticky;
            top: 0;
            background: var(--bg-elevated);
            color: var(--text-muted);
            font-size: 12px;
            font-weight: 600;
            text-align: left;
            padding: 7px 10px;
            border-bottom: 1px solid var(--border-color);
            z-index: 2;
            user-select: none;
        }
        table.tree-table th.col-num, table.tree-table td.col-num { text-align: right; }
        table.tree-table tr.tree-row {
            border-bottom: 1px solid rgba(49, 54, 63, 0.45);
            cursor: pointer;
        }
        table.tree-table tr.tree-row:hover { background: var(--bg-hover); }
        table.tree-table tr.tree-row.selected { background: var(--bg-selected); }
        table.tree-table td {
            padding: 5px 10px;
            white-space: nowrap;
            overflow: hidden;
            text-overflow: ellipsis;
            vertical-align: middle;
        }
        .name-cell-wrap {
            display: flex;
            align-items: center;
            gap: 5px;
            overflow: hidden;
        }
        .twisty-btn {
            width: 18px;
            height: 18px;
            border: none;
            background: transparent;
            color: var(--text-muted);
            cursor: pointer;
            padding: 0;
            font-size: 11px;
            display: inline-flex;
            align-items: center;
            justify-content: center;
            flex-shrink: 0;
        }
        .twisty-btn:hover { color: #fff; }
        .item-label { overflow: hidden; text-overflow: ellipsis; }
        .mini-tag {
            font-size: 11px;
            padding: 1px 5px;
            border-radius: 3px;
            flex-shrink: 0;
        }
        .tag-hl { background: rgba(210, 153, 34, 0.2); color: #f2cc60; border: 1px solid var(--warning); }
        .tag-rp { background: rgba(163, 113, 247, 0.2); color: #d2a8ff; border: 1px solid var(--purple); }
        .tag-comp { background: rgba(47, 129, 247, 0.18); color: #79c0ff; border: 1px solid var(--accent); }

        .pct-cell {
            display: flex;
            align-items: center;
            gap: 6px;
            justify-content: flex-end;
        }
        .pct-bar-bg {
            width: 55px;
            height: 6px;
            background: var(--bg-elevated);
            border-radius: 3px;
            overflow: hidden;
            flex-shrink: 0;
        }
        .pct-bar-fill {
            height: 100%;
            background: var(--accent);
        }
        .pct-num { width: 42px; text-align: right; font-size: 12px; color: var(--text-muted); }

        /* F. Inspector Panel */
        .inspector-body { padding: 12px 14px; display: flex; flex-direction: column; gap: 10px; }
        .insp-row { display: flex; flex-direction: column; gap: 2px; border-bottom: 1px solid rgba(49,54,63,0.5); padding-bottom: 7px; }
        .insp-key { font-size: 11.5px; color: var(--text-muted); }
        .insp-val { font-size: 13px; word-break: break-all; font-weight: 500; }
        .insp-actions { display: flex; flex-direction: column; gap: 6px; margin-top: 4px; }

        /* Section 5: Diagnostics & Benchmark Collapsible Panel */
        details.diag-panel {
            background: var(--bg-panel);
            border: 1px solid var(--border-color);
            border-radius: 6px;
            padding: 10px 14px;
        }
        details.diag-panel > summary {
            cursor: pointer;
            font-weight: 600;
            color: var(--text-main);
            outline: none;
            display: flex;
            justify-content: space-between;
            align-items: center;
        }
        .diag-content { margin-top: 12px; display: flex; flex-direction: column; gap: 12px; }
        .diag-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(210px, 1fr));
            gap: 8px;
        }
        .diag-card {
            background: var(--bg-elevated);
            border: 1px solid var(--border-color);
            border-radius: 4px;
            padding: 8px 10px;
        }
        table.diag-table {
            width: 100%;
            border-collapse: collapse;
            font-size: 12.5px;
        }
        table.diag-table th, table.diag-table td {
            border: 1px solid var(--border-color);
            padding: 6px 9px;
            text-align: left;
        }
        table.diag-table th { background: var(--bg-elevated); color: var(--text-muted); }

        /* Custom Context Menu */
        .ctx-menu {
            position: fixed;
            background: var(--bg-elevated);
            border: 1px solid var(--border-color);
            border-radius: 5px;
            padding: 4px 0;
            min-width: 210px;
            box-shadow: 0 8px 24px rgba(0,0,0,0.6);
            z-index: 9999;
            display: none;
        }
        .ctx-item {
            padding: 6px 12px;
            font-size: 13px;
            color: var(--text-main);
            cursor: pointer;
            display: flex;
            align-items: center;
            gap: 8px;
        }
        .ctx-item:hover { background: var(--accent); color: #fff; }
        .ctx-sep { height: 1px; background: var(--border-color); margin: 4px 0; }
    </style>
</head>
<body>
    <div class="app-shell">
        <!-- A. THANH CÔNG CỤ TRÊN CÙNG -->
        <header class="toolbar-panel">
            <div class="toolbar-top">
                <div class="brand-title">
                    <span>🖴 Phân Tích Dung Lượng Ổ Đĩa NTFS</span>
                </div>
                <div class="status-badges">
                    <span id="scanStateBadge" class="pill-badge pill-idle">Chưa quét</span>
                    <span id="adminBadge" class="pill-badge pill-idle">Đang kiểm tra quyền...</span>
                    <button id="btnElevate" class="btn-elevate" style="display:none;" onclick="elevateAdmin()">🛡️ Cấp quyền Admin (Bật MFT)</button>
                </div>
            </div>
            <div class="toolbar-controls">
                <div class="drive-quick-btns" title="Chọn nhanh ổ đĩa">
                    <button type="button" class="btn-quick-drive" onclick="selectQuickDrive('C:\\')">C:\</button>
                    <button type="button" class="btn-quick-drive" onclick="selectQuickDrive('D:\\')">D:\</button>
                    <button type="button" class="btn-quick-drive" onclick="selectQuickDrive('E:\\')">E:\</button>
                </div>
                <input type="text" id="pathInput" value="C:\" placeholder="Nhập đường dẫn ổ đĩa hoặc thư mục (VD: C:\ hoặc D:\Project)">
                <select id="chunkSelect" title="Kích thước đọc theo lô cho MFT Raw">
                    <option value="4">Lô đọc: 4 MB</option>
                    <option value="8" selected>Lô đọc: 8 MB</option>
                    <option value="16">Lô đọc: 16 MB</option>
                </select>
                <label class="chk-inline" title="Tự động chuyển sang Win32 API nếu phân vùng không phải NTFS hoặc chưa có quyền Admin">
                    <input type="checkbox" id="fallbackChk" checked> Cho phép Win32 Fallback
                </label>
                <button type="button" id="btnScan" class="btn-primary" onclick="startScan()">▶ Bắt đầu quét</button>
                <button type="button" id="btnRescan" class="btn-secondary" onclick="startScan()" disabled title="Quét lại đường dẫn hiện tại">🔄 Quét lại</button>
                <button type="button" id="btnCancel" class="btn-danger" onclick="cancelScan()">⏹ Hủy quét</button>
            </div>
        </header>

        <!-- B. KHU VỰC TRẠNG THÁI QUÉT -->
        <section class="status-panel">
            <div class="status-header-row">
                <div id="statusHeadline" class="status-headline">Sẵn sàng. Hãy chọn ổ đĩa hoặc nhập đường dẫn rồi nhấn "Bắt đầu quét".</div>
                <div class="status-meta-row">
                    <span>Phương pháp: <b id="metaEngine">—</b></span>
                    <span>Đã xử lý: <b id="metaProcessed">—</b></span>
                    <span>Thời gian: <b id="metaElapsed">—</b></span>
                </div>
            </div>
            <div class="progress-track">
                <div id="progressBarFill" class="progress-fill"></div>
            </div>
            <div id="statusSubtext" class="status-subtext">Chưa có tác vụ nào đang chạy.</div>
            <div id="statusBanners" class="banner-list"></div>
        </section>

        <!-- C. CÁC THẺ TỔNG QUAN -->
        <section class="overview-grid">
            <div class="metric-card">
                <div class="metric-label">Dung lượng Logic đã phân tích</div>
                <div id="cardLogicalVal" class="metric-value">—</div>
                <div id="cardLogicalNote" class="metric-note">Tổng kích thước dữ liệu (DataSize)</div>
            </div>
            <div class="metric-card">
                <div class="metric-label">Dung lượng Cấp phát trên đĩa</div>
                <div id="cardAllocatedVal" class="metric-value">—</div>
                <div id="cardAllocatedNote" class="metric-note">Thực chiếm (Allocated / CompressedSize)</div>
            </div>
            <div class="metric-card">
                <div class="metric-label">Tổng số Tệp & Thư mục</div>
                <div id="cardCountsVal" class="metric-value">—</div>
                <div id="cardCountsNote" class="metric-note">Trong phạm vi đường dẫn đã quét</div>
            </div>
            <div class="metric-card">
                <div class="metric-label">Đối soát Volume & Thời gian</div>
                <div id="cardVolumeVal" class="metric-value">—</div>
                <div id="cardVolumeNote" class="metric-note">So sánh với tổng dung lượng ổ đĩa đã dùng</div>
            </div>
        </section>

        <!-- D. KHU VỰC PHÂN TÍCH CÁC MỤC LỚN NHẤT TRONG THƯ MỤC ĐANG XEM -->
        <section class="top-items-panel">
            <div class="section-title">
                <span id="topItemsTitle">📊 Các mục chiếm dung lượng lớn nhất tại vị trí đang xem</span>
                <span id="topItemsHint" style="font-size:11.5px;text-transform:none;font-weight:normal;">Nhấn vào thẻ để chọn hoặc đi sâu vào thư mục</span>
            </div>
            <div id="topItemsContainer" class="top-items-grid">
                <div style="color:var(--text-muted);font-size:12.5px;">— Chưa có dữ liệu quét —</div>
            </div>
        </section>

        <!-- E & F. CÂY THƯ MỤC (TREE-TABLE) VÀ PANEL CHI TIẾT MỤC ĐƯỢC CHỌN -->
        <section class="workspace-split">
            <!-- E. Cây thư mục & Bảng dữ liệu -->
            <div class="explorer-panel">
                <div class="explorer-bar">
                    <button type="button" id="btnUpDir" class="btn-secondary" style="padding:4px 10px;font-size:12px;" onclick="navigateUpDirectory()" disabled>⬆️ Lên cấp cha</button>
                    <div id="breadcrumbBar" class="breadcrumb-bar">
                        <span style="color:var(--text-muted);">Vị trí: —</span>
                    </div>
                </div>
                <div class="filter-bar">
                    <input type="text" id="searchInput" placeholder="🔍 Lọc theo tên tệp / thư mục đang hiển thị..." oninput="onSearchInput()">
                    <select id="typeFilterSelect" onchange="onTypeFilterChange()" title="Lọc theo loại mục">
                        <option value="all">Loại: Tất cả</option>
                        <option value="dir">Chỉ thư mục</option>
                        <option value="file">Chỉ tệp tin</option>
                        <option value="compressed">Tệp Nén / Sparse (Thực chiếm &lt; Logic)</option>
                        <option value="hardlink">Chỉ Hardlink phụ</option>
                        <option value="reparse">Chỉ Reparse Point</option>
                        <option value="special">Hardlink phụ &amp; Reparse Point</option>
                    </select>
                    <label class="chk-inline" title="Ẩn toàn bộ các mục Reparse Point (Junction / Symlink) khỏi bảng cây thư mục và thẻ tổng hợp">
                        <input type="checkbox" id="hideReparseChk" onchange="onHideReparseChange()"> Ẩn Reparse Point
                    </label>
                    <select id="sortSelect" onchange="onSortOrMetricChange()" title="Tiêu chí sắp xếp và tính tỷ lệ %">
                        <option value="logical" selected>Sắp xếp: Dung lượng Logic ↓</option>
                        <option value="allocated">Sắp xếp: Dung lượng Cấp phát ↓</option>
                        <option value="name">Sắp xếp: Tên (A → Z)</option>
                    </select>
                </div>
                <div class="table-scroll">
                    <table class="tree-table">
                        <thead>
                            <tr>
                                <th style="width:38%;">Tên mục</th>
                                <th style="width:12%;">Loại</th>
                                <th class="col-num" style="width:14%;">Dung lượng Logic</th>
                                <th class="col-num" style="width:14%;">Dung lượng Cấp phát</th>
                                <th class="col-num" style="width:14%;">Tỷ lệ cấp cha</th>
                                <th class="col-num" style="width:8%;">Tệp con</th>
                            </tr>
                        </thead>
                        <tbody id="treeTableBody">
                            <tr>
                                <td colspan="6" style="text-align:center;padding:28px;color:var(--text-muted);">
                                    Chưa có dữ liệu. Hãy nhấn "Bắt đầu quét" để phân tích ổ đĩa.
                                </td>
                            </tr>
                        </tbody>
                    </table>
                </div>
            </div>

            <!-- F. Khu vực chi tiết mục được chọn -->
            <aside class="inspector-panel">
                <div class="explorer-bar">
                    <span style="font-weight:600;font-size:13px;">ℹ️ Chi tiết mục được chọn</span>
                </div>
                <div id="inspectorBody" class="inspector-body">
                    <div style="color:var(--text-muted);font-size:12.5px;">
                        Hãy chọn một tệp hoặc thư mục trong bảng bên trái để xem thông tin chi tiết.
                    </div>
                </div>
            </aside>
        </section>

        <!-- 5. KHU VỰC NÂNG CAO: CHẨN ĐOÁN, TELEMETRY & BENCHMARK -->
        <details id="diagAccordion" class="diag-panel">
            <summary>
                <span>🛠️ Khu vực Chẩn đoán kỹ thuật, Telemetry & Benchmark</span>
                <span style="font-size:12px;color:var(--text-muted);font-weight:normal;">Nhấn để mở rộng / thu gọn</span>
            </summary>
            <div class="diag-content">
                <div style="display:flex;gap:8px;flex-wrap:wrap;align-items:center;">
                    <button type="button" id="btnRunSelfTest" class="btn-secondary" onclick="runSelfTests()">🧪 Chạy Tự Kiểm Thử (7 Tests)</button>
                    <button type="button" id="btnRunBench" class="btn-secondary" onclick="runBenchmark()">⏱️ Chạy Benchmark MFT vs USN</button>
                    <button type="button" id="btnCopyDiag" class="btn-secondary" onclick="copyDiagnosticsJson()">📋 Sao chép JSON Chẩn đoán</button>
                    <span id="diagActionStatus" style="font-size:12px;color:var(--text-muted);"></span>
                </div>
                <div id="telemetryGrid" class="diag-grid">
                    <div class="diag-card" style="color:var(--text-muted);">Chưa có dữ liệu Telemetry.</div>
                </div>
                <div id="diagResultBox"></div>
            </div>
        </details>
    </div>

    <!-- MENU CHUỘT PHẢI (CONTEXT MENU) -->
    <div id="contextMenu" class="ctx-menu">
        <div class="ctx-item" onclick="ctxOpenExplorer()">📂 Mở vị trí trong Explorer</div>
        <div class="ctx-item" onclick="ctxCopyPath()">📋 Sao chép đường dẫn đầy đủ</div>
        <div id="ctxDrillDownItem" class="ctx-item" onclick="ctxDrillDown()">🔍 Đặt thư mục này làm gốc hiển thị</div>
        <div class="ctx-sep"></div>
        <div id="ctxRefreshItem" class="ctx-item" onclick="ctxRefreshBranch()">🔄 Làm mới nhánh này</div>
    </div>

    <script>
        const CSRF_TOKEN = "__SESSION_TOKEN__";
        const PAGE_LIMIT = 150;

        // Quản lý trạng thái toàn cục & chống Race Condition
        let scanGeneration = 0;
        let pollTimer = null;
        let isBusyTask = false;
        let rootNodeData = null;
        let currentFocusNode = null;
        let breadcrumbStack = [];
        let lastTelemetryData = null;
        let lastDiagExportObj = {};
        let currentFocusItems = []; // Lưu danh sách mục con của thư mục gốc đang xem

        // Lưu trữ phẳng các node đã nạp trên UI: Map<nodeId, NodeModel>
        const nodeMap = new Map();
        // Khóa chống gửi trùng request khi người dùng nhấn liên tục
        const inFlightRequests = new Set();

        let selectedNodeId = null;
        let contextMenuNodeId = null;
        let detailRequestId = 0;
        let searchDebounceTimer = null;

        async function apiFetch(url, options = {}) {
            options.headers = options.headers || {};
            options.headers['X-Scanner-Token'] = CSRF_TOKEN;
            return fetch(url, options);
        }

        function setBusyState(busy) {
            isBusyTask = busy;
            document.getElementById('btnScan').disabled = busy;
            document.getElementById('btnRescan').disabled = busy || !rootNodeData;
            document.getElementById('btnRunSelfTest').disabled = busy;
            document.getElementById('btnRunBench').disabled = busy;
            document.getElementById('btnCancel').style.display = busy ? 'inline-block' : 'none';
            document.querySelectorAll('.btn-quick-drive').forEach(b => b.disabled = busy);
        }

        function selectQuickDrive(drivePath) {
            if (isBusyTask) return;
            document.getElementById('pathInput').value = drivePath;
            startScan();
        }

        function getSortMode() {
            return document.getElementById('sortSelect').value || 'logical';
        }

        function isAllocatedMetricMode() {
            return getSortMode() === 'allocated';
        }

        function getFileExtensionType(node) {
            if (node.is_dir) {
                return node.is_reparse ? 'Thư mục (Reparse)' : 'Thư mục';
            }
            const name = node.name || '';
            const dotIdx = name.lastIndexOf('.');
            if (dotIdx > 0 && dotIdx < name.length - 1) {
                return 'Tệp ' + name.substring(dotIdx).toUpperCase();
            }
            return 'Tệp tin';
        }

        // Kiểm tra quyền Administrator
        async function checkAdminStatus() {
            try {
                const res = await apiFetch('/api/info');
                if (!res.ok) throw new Error('HTTP ' + res.status);
                const data = await res.json();
                const badge = document.getElementById('adminBadge');
                const btnElev = document.getElementById('btnElevate');
                if (data.isAdmin) {
                    badge.className = 'pill-badge pill-success';
                    badge.textContent = '⚡ Quyền Admin (MFT Raw sẵn sàng)';
                    btnElev.style.display = 'none';
                } else {
                    badge.className = 'pill-badge pill-warn';
                    badge.textContent = '⚠️ Quyền chuẩn (Win32 Fallback)';
                    btnElev.style.display = 'inline-block';
                }
            } catch (err) {
                const badge = document.getElementById('adminBadge');
                badge.className = 'pill-badge pill-danger';
                badge.textContent = 'Không kiểm tra được quyền (' + err.message + ')';
            }
        }
        checkAdminStatus();
        try {
            if (localStorage.getItem('ntfs_hide_reparse') === '1') {
                document.getElementById('hideReparseChk').checked = true;
            }
        } catch (e) {}
        async function elevateAdmin() {
            if (isBusyTask) return;
            setStatusDisplay('warn', 'Đang yêu cầu quyền Administrator...', 'Hãy xác nhận hộp thoại UAC của Windows.');
            try {
                const res = await apiFetch('/api/elevate', { method: 'POST' });
                if (!res.ok) throw new Error('Không thể khởi tạo tiến trình nâng quyền');
                setTimeout(() => {
                    const retry = setInterval(async () => {
                        try {
                            const r = await fetch('/api/info');
                            if (r.ok) {
                                clearInterval(retry);
                                location.reload();
                            }
                        } catch (e) {
                            // Đợi tiến trình mới khởi động xong
                        }
                    }, 600);
                }, 800);
            } catch (err) {
                setStatusDisplay('danger', 'Nâng quyền Admin thất bại', err.message);
            }
        }

        // Hiển thị khu vực trạng thái trung thực, dùng textContent chống XSS
        function setStatusDisplay(level, headline, subtext) {
            const badge = document.getElementById('scanStateBadge');
            const headEl = document.getElementById('statusHeadline');
            const subEl = document.getElementById('statusSubtext');
            headEl.textContent = headline;
            subEl.textContent = subtext || '';

            if (level === 'scanning') {
                badge.className = 'pill-badge pill-scanning';
                badge.textContent = '⏳ Đang quét';
            } else if (level === 'success') {
                badge.className = 'pill-badge pill-success';
                badge.textContent = '✔️ Hoàn tất đầy đủ';
            } else if (level === 'warn') {
                badge.className = 'pill-badge pill-warn';
                badge.textContent = '⚠️ Hoàn tất có giới hạn';
            } else if (level === 'cancelled') {
                badge.className = 'pill-badge pill-warn';
                badge.textContent = '⏹ Đã hủy quét';
            } else if (level === 'danger') {
                badge.className = 'pill-badge pill-danger';
                badge.textContent = '❌ Thất bại';
            } else {
                badge.className = 'pill-badge pill-idle';
                badge.textContent = 'Chưa quét';
            }
        }

        function clearBanners() {
            const box = document.getElementById('statusBanners');
            box.replaceChildren();
        }

        function addNoticeBanner(type, text) {
            const box = document.getElementById('statusBanners');
            const div = document.createElement('div');
            div.className = 'notice-banner notice-' + type;
            div.textContent = text;
            box.appendChild(div);
        }

        function setProgressBar(percentOrNull, isIndeterminate) {
            const fill = document.getElementById('progressBarFill');
            if (isIndeterminate) {
                fill.classList.add('progress-indeterminate');
                fill.style.width = '35%';
            } else {
                fill.classList.remove('progress-indeterminate');
                const p = (percentOrNull !== null && !isNaN(percentOrNull)) ? Math.max(0, Math.min(100, percentOrNull)) : 0;
                fill.style.width = p.toFixed(1) + '%';
            }
        }

        // Bắt đầu quét mới
        async function startScan() {
            if (isBusyTask) return;
            const path = document.getElementById('pathInput').value.trim() || 'C:\\';
            const chunkMb = document.getElementById('chunkSelect').value || '8';
            const allowFallback = document.getElementById('fallbackChk').checked ? 1 : 0;

            scanGeneration++;
            const myGen = scanGeneration;
            if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }

            setBusyState(true);
            clearBanners();
            nodeMap.clear();
            inFlightRequests.clear();
            selectedNodeId = null;
            rootNodeData = null;
            currentFocusNode = null;
            breadcrumbStack = [];

            resetOverviewCards();
            renderEmptyTreeTable('⏳ Đang phân tích ổ đĩa, vui lòng đợi...');
            renderTopLargestItems([]);
            renderInspector(null);

            document.getElementById('metaEngine').textContent = 'Đang xác định...';
            document.getElementById('metaProcessed').textContent = '0';
            document.getElementById('metaElapsed').textContent = '0.00 s';
            setStatusDisplay('scanning', 'Đang khởi tạo tiến trình quét cho: ' + path, 'Đang gửi yêu cầu tới Backend C++...');
            setProgressBar(0, true);

            try {
                const res = await apiFetch(`/api/start?path=${encodeURIComponent(path)}&chunkMb=${chunkMb}&fallback=${allowFallback}`, { method: 'POST' });
                if (myGen !== scanGeneration) return;
                if (!res.ok) {
                    let errMsg = 'HTTP ' + res.status;
                    try { const errJson = await res.json(); if (errJson.error) errMsg = errJson.error; } catch (e) {}
                    throw new Error(errMsg);
                }
                pollTimer = setInterval(() => pollScanStatus(myGen), 200);
            } catch (err) {
                if (myGen !== scanGeneration) return;
                setBusyState(false);
                setProgressBar(0, false);
                setStatusDisplay('danger', 'Không thể bắt đầu quét', err.message);
                addNoticeBanner('danger', 'Lỗi khởi tạo: ' + err.message);
                renderEmptyTreeTable('Quét thất bại: ' + err.message);
            }
        }

        async function cancelScan() {
            const btnCancel = document.getElementById('btnCancel');
            btnCancel.disabled = true;
            document.getElementById('statusHeadline').textContent = 'Đang gửi tín hiệu dừng quét an toàn...';
            try {
                await apiFetch('/api/cancel', { method: 'POST' });
            } catch (err) {
                addNoticeBanner('danger', 'Lỗi khi gửi lệnh hủy: ' + err.message);
            } finally {
                btnCancel.disabled = false;
            }
        }

        async function pollScanStatus(myGen) {
            if (myGen !== scanGeneration) return;
            try {
                const res = await apiFetch('/api/status');
                if (myGen !== scanGeneration) return;
                if (!res.ok) throw new Error('HTTP ' + res.status);
                const data = await res.json();

                document.getElementById('metaEngine').textContent = data.engine || '—';
                const scanned = Number(data.scannedFiles || 0);
                const totalExp = Number(data.totalRecords || 0);
                const elapsedSec = (Number(data.elapsedMs || 0) / 1000).toFixed(2) + ' s';
                document.getElementById('metaElapsed').textContent = elapsedSec;

                if (data.state === 'scanning') {
                    if (totalExp > 0) {
                        const pct = Math.min(100, (scanned * 100) / totalExp);
                        document.getElementById('metaProcessed').textContent = `${scanned.toLocaleString()} / ${totalExp.toLocaleString()} bản ghi (${pct.toFixed(1)}%)`;
                        setProgressBar(pct, false);
                    } else {
                        document.getElementById('metaProcessed').textContent = `${scanned.toLocaleString()} mục`;
                        setProgressBar(null, true);
                    }
                    setStatusDisplay('scanning', 'Đang phân tích dữ liệu ổ đĩa...', data.currentDir || 'Đang xử lý...');
                }
                else if (data.state === 'done') {
                    clearInterval(pollTimer);
                    pollTimer = null;
                    await loadScanFinalResult(myGen);
                }
                else if (data.state === 'cancelled') {
                    clearInterval(pollTimer);
                    pollTimer = null;
                    setBusyState(false);
                    setProgressBar(0, false);
                    document.getElementById('metaProcessed').textContent = `${scanned.toLocaleString()} mục (Đã dừng)`;
                    setStatusDisplay('cancelled', 'Quá trình quét đã được hủy bởi người dùng.', `Dừng tại mức ${scanned.toLocaleString()} bản ghi.`);
                    addNoticeBanner('warn', 'Quá trình quét đã bị hủy trước khi hoàn tất. Kết quả không được hiển thị để tránh sai lệch số liệu.');
                    renderEmptyTreeTable('Đã hủy quét. Nhấn "Bắt đầu quét" để thực hiện lại.');
                }
                else if (data.state === 'error') {
                    clearInterval(pollTimer);
                    pollTimer = null;
                    setBusyState(false);
                    setProgressBar(0, false);
                    setStatusDisplay('danger', 'Quá trình quét thất bại', data.error || 'Lỗi không xác định');
                    addNoticeBanner('danger', 'Chi tiết lỗi: ' + (data.error || 'Lỗi không xác định'));
                    renderEmptyTreeTable('Quét thất bại: ' + (data.error || 'Lỗi không xác định'));
                }
            } catch (err) {
                if (myGen !== scanGeneration) return;
                clearInterval(pollTimer);
                pollTimer = null;
                setBusyState(false);
                setProgressBar(0, false);
                setStatusDisplay('danger', 'Mất kết nối tới máy chủ Backend C++', err.message);
                addNoticeBanner('danger', 'Không thể lấy trạng thái quét: ' + err.message);
            }
        }

        async function loadScanFinalResult(myGen) {
            try {
                const res = await apiFetch('/api/result');
                if (myGen !== scanGeneration) return;
                if (!res.ok) throw new Error('HTTP ' + res.status);
                const payload = await res.json();

                rootNodeData = payload.root;
                lastTelemetryData = payload.telemetry;
                lastDiagExportObj = { root: payload.root, telemetry: payload.telemetry };

                setBusyState(false);
                setProgressBar(100, false);

                const t = payload.telemetry;
                const timeSec = (Number(t.totalMs || 0) / 1000).toFixed(2);
                document.getElementById('metaEngine').textContent = t.engineUsed || '—';
                document.getElementById('metaProcessed').textContent = `${Number(t.processedMftRecords || 0).toLocaleString()} bản ghi`;
                document.getElementById('metaElapsed').textContent = `${timeSec} s`;

                // Phân biệt rõ: Hoàn tất đầy đủ vs Hoàn tất nhưng có giới hạn/lỗi đọc
                const hasLimitations = (t.corruptedRecords > 0 || t.accessDeniedDirs > 0 || t.unsupportedAttributes > 0 || t.isApproximateFallback);
                clearBanners();

                if (!hasLimitations) {
                    setStatusDisplay('success', `Hoàn tất đầy đủ trong ${timeSec} giây (${t.engineUsed}).`, `Đã xác nhận xử lý 100% phạm vi yêu cầu (${Number(t.processedMftRecords).toLocaleString()} bản ghi).`);
                } else {
                    setStatusDisplay('warn', `Hoàn tất có giới hạn / cảnh báo trong ${timeSec} giây (${t.engineUsed}).`, 'Một số mục bị từ chối truy cập, đang ghi dở hoặc dùng chế độ ước lượng.');
                }

                if (t.isApproximateFallback) {
                    addNoticeBanner('warn', `Chế độ Win32 Fallback (Số liệu ước lượng): ${t.fallbackReason} — Dung lượng cấp phát được làm tròn theo Cluster; không phân tách Hard Link hoặc đọc trực tiếp CompressedSize của MFT.`);
                }
                if (t.corruptedRecords > 0) {
                    addNoticeBanner('warn', `Phát hiện ${t.corruptedRecords.toLocaleString()} bản ghi MFT đang ghi dở (torn sector) hoặc thay đổi trong lúc ổ đĩa đang hoạt động.`);
                }
                if (t.accessDeniedDirs > 0) {
                    addNoticeBanner('warn', `Có ${t.accessDeniedDirs.toLocaleString()} thư mục bị từ chối quyền truy cập (Access Denied) nên không thể tính vào tổng dung lượng.`);
                }
                if (t.isFullVolumeScan) {
                    addNoticeBanner('info', `Đối soát toàn phân vùng: Volume đã dùng ${t.volumeUsedText} | Đã giải thích (Root + Siêu dữ liệu NTFS): ${payload.root.allocatedText} + ${t.systemMetaAllocatedText} | Phần chênh lệch (Index $I30, ADS, $Secure...): ${t.unexplainedText}.`);
                }

                updateOverviewCards(payload.root, t);
                renderTelemetryDiagnostics(t, payload.root);

                // Khởi tạo hiển thị tại nút gốc
                breadcrumbStack = [{ id: payload.root.id, name: payload.root.name }];
                await focusDirectoryNode(payload.root.id, myGen, true);
            } catch (err) {
                if (myGen !== scanGeneration) return;
                setBusyState(false);
                setStatusDisplay('danger', 'Lỗi khi tải kết quả tổng hợp', err.message);
                addNoticeBanner('danger', err.message);
            }
        }

        function resetOverviewCards() {
            document.getElementById('cardLogicalVal').textContent = '—';
            document.getElementById('cardLogicalNote').textContent = 'Tổng kích thước dữ liệu (DataSize)';
            document.getElementById('cardAllocatedVal').textContent = '—';
            document.getElementById('cardAllocatedNote').textContent = 'Thực chiếm (Allocated / CompressedSize)';
            document.getElementById('cardCountsVal').textContent = '—';
            document.getElementById('cardCountsNote').textContent = 'Trong phạm vi đường dẫn đã quét';
            document.getElementById('cardVolumeVal').textContent = '—';
            document.getElementById('cardVolumeNote').textContent = 'So sánh với tổng dung lượng ổ đĩa đã dùng';
        }

        function updateOverviewCards(rootNode, t) {
            document.getElementById('cardLogicalVal').textContent = rootNode.logicalText || '0 B';
            document.getElementById('cardLogicalNote').textContent = `${Number(rootNode.logicalSize || 0).toLocaleString()} bytes (Logic DataSize)`;

            document.getElementById('cardAllocatedVal').textContent = rootNode.allocatedText || '0 B';
            document.getElementById('cardAllocatedNote').textContent = t.isApproximateFallback
                ? 'Ước lượng làm tròn theo Cluster (Fallback)'
                : `${Number(rootNode.allocatedSize || 0).toLocaleString()} bytes (Thực chiếm đĩa)`;

            document.getElementById('cardCountsVal').textContent = `${Number(t.validFiles || 0).toLocaleString()} tệp`;
            document.getElementById('cardCountsNote').textContent = `${Number(t.validDirectories || 0).toLocaleString()} thư mục | ${Number(t.extraHardLinksCount || 0).toLocaleString()} hardlink phụ | ${Number(t.reparsePointsCount || 0).toLocaleString()} reparse`;

            if (t.isFullVolumeScan && t.volumeUsedBytes > 0) {
                document.getElementById('cardVolumeVal').textContent = t.volumeUsedText;
                document.getElementById('cardVolumeNote').textContent = `Siêu dữ liệu NTFS: ${t.systemMetaAllocatedText} | Chênh lệch: ${t.unexplainedText}`;
            } else {
                document.getElementById('cardVolumeVal').textContent = `${(Number(t.totalMs || 0) / 1000).toFixed(2)} giây`;
                document.getElementById('cardVolumeNote').textContent = `I/O: ${t.readMftMs}ms | Parse: ${t.parseRecordsMs}ms | Sum: ${t.sumSizesMs}ms`;
            }
        }

        // Điều hướng Focus vào một thư mục làm gốc bảng hiện tại
        async function focusDirectoryNode(dirId, myGen = scanGeneration, isInitialRoot = false) {
            const reqKey = `focus_${dirId}_${getSortMode()}`;
            if (inFlightRequests.has(reqKey)) return;
            inFlightRequests.add(reqKey);

            try {
                const sortParam = getSortMode();
                const res = await apiFetch(`/api/children?id=${dirId}&offset=0&limit=${PAGE_LIMIT}&sort=${sortParam}`);
                if (myGen !== scanGeneration) return;
                if (!res.ok) throw new Error('HTTP ' + res.status);
                const data = await res.json();

                const dirInfo = data.dir || rootNodeData;
                currentFocusNode = dirInfo;
                currentFocusItems = data.items || [];
                // Lưu vào nodeMap
                const dirModel = {
                    ...dirInfo,
                    depth: 0,
                    expanded: true,
                    loadedChildrenIds: [],
                    nextOffset: data.items.length,
                    totalChildren: data.totalChildren,
                    hasMore: data.hasMore,
                    loadError: null
                };

                data.items.forEach(item => {
                    const childModel = {
                        ...item,
                        parentLogicalSize: dirInfo.logicalSize,
                        parentAllocatedSize: dirInfo.allocatedSize,
                        depth: 1,
                        expanded: false,
                        loadedChildrenIds: [],
                        nextOffset: 0,
                        totalChildren: 0,
                        hasMore: false,
                        loadError: null
                    };
                    nodeMap.set(item.id, childModel);
                    dirModel.loadedChildrenIds.push(item.id);
                });
                nodeMap.set(dirId, dirModel);

                // Cập nhật Breadcrumb
                updateBreadcrumbForFocus(dirInfo);
                renderTopLargestItems(data.items, dirInfo);
                rebuildTreeTableDOM();
                selectNode(dirId);
            } catch (err) {
                if (myGen !== scanGeneration) return;
                renderEmptyTreeTable('Lỗi khi tải thư mục (ID ' + dirId + '): ' + err.message);
            } finally {
                inFlightRequests.delete(reqKey);
            }
        }

        function updateBreadcrumbForFocus(dirInfo) {
            const btnUp = document.getElementById('btnUpDir');
            btnUp.disabled = (!dirInfo || dirInfo.id === rootNodeData.id);

            const existIdx = breadcrumbStack.findIndex(b => b.id === dirInfo.id);
            if (existIdx >= 0) {
                breadcrumbStack = breadcrumbStack.slice(0, existIdx + 1);
            } else {
                breadcrumbStack.push({ id: dirInfo.id, name: dirInfo.name });
            }

            const bar = document.getElementById('breadcrumbBar');
            bar.replaceChildren();
            const label = document.createElement('span');
            label.style.color = 'var(--text-muted)';
            label.textContent = 'Vị trí: ';
            bar.appendChild(label);

            breadcrumbStack.forEach((crumb, idx) => {
                if (idx > 0) {
                    const sep = document.createElement('span');
                    sep.className = 'crumb-sep';
                    sep.textContent = '›';
                    bar.appendChild(sep);
                }
                const btn = document.createElement('button');
                btn.type = 'button';
                btn.className = 'crumb-btn';
                btn.textContent = crumb.name;
                btn.onclick = () => focusDirectoryNode(crumb.id);
                bar.appendChild(btn);
            });
        }

        function navigateUpDirectory() {
            if (!currentFocusNode || !rootNodeData || currentFocusNode.id === rootNodeData.id) return;
            if (breadcrumbStack.length >= 2) {
                const parentCrumb = breadcrumbStack[breadcrumbStack.length - 2];
                focusDirectoryNode(parentCrumb.id);
            } else if (currentFocusNode.parentId !== undefined) {
                focusDirectoryNode(currentFocusNode.parentId);
            }
        }

        // D. Hiển thị các mục lớn nhất tại thư mục đang xem (có áp dụng tùy chọn Ẩn Reparse Point)
        function renderTopLargestItems(items, parentDir) {
            const container = document.getElementById('topItemsContainer');
            container.replaceChildren();

            const titleEl = document.getElementById('topItemsTitle');
            titleEl.textContent = parentDir
                ? `📊 Các mục chiếm dung lượng lớn nhất trong "${parentDir.name}"`
                : '📊 Các mục chiếm dung lượng lớn nhất tại vị trí đang xem';

            const hideReparse = document.getElementById('hideReparseChk').checked;
            const filteredItems = (items || []).filter(it => !(hideReparse && it.is_reparse));

            if (filteredItems.length === 0) {
                const empty = document.createElement('div');
                empty.style.color = 'var(--text-muted)';
                empty.style.fontSize = '12.5px';
                empty.textContent = '— Không có mục con nào phù hợp —';
                container.appendChild(empty);
                return;
            }

            const allocMode = isAllocatedMetricMode();
            const parentTotal = parentDir ? (allocMode ? parentDir.allocatedSize : parentDir.logicalSize) : 0;

            const topList = [...filteredItems]
                .sort((a, b) => {
                    const sa = allocMode ? a.allocatedSize : a.logicalSize;
                    const sb = allocMode ? b.allocatedSize : b.logicalSize;
                    return sb - sa;
                })
                .slice(0, 6);

            topList.forEach(item => {
                const sz = allocMode ? item.allocatedSize : item.logicalSize;
                const szText = allocMode ? item.allocatedText : item.logicalText;
                const pct = parentTotal > 0 ? Math.min(100, (sz * 100) / parentTotal) : 0;

                const card = document.createElement('div');
                card.className = 'top-item-card';
                card.onclick = () => {
                    selectNode(item.id);
                    const rowEl = document.getElementById('row_node_' + item.id);
                    if (rowEl) rowEl.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
                };
                card.ondblclick = () => {
                    if (item.is_dir && !item.is_reparse) focusDirectoryNode(item.id);
                };

                const bar = document.createElement('div');
                bar.className = 'top-item-bar';
                bar.style.width = pct.toFixed(1) + '%';
                card.appendChild(bar);

                const head = document.createElement('div');
                head.className = 'top-item-head';
                const nameSpan = document.createElement('span');
                nameSpan.className = 'top-item-name';
                nameSpan.textContent = (item.is_dir ? (item.is_reparse ? '↪️ ' : '📁 ') : '📄 ') + item.name;
                const sizeSpan = document.createElement('span');
                sizeSpan.style.color = '#b5cea8';
                sizeSpan.textContent = szText;
                head.appendChild(nameSpan);
                head.appendChild(sizeSpan);
                card.appendChild(head);

                const sub = document.createElement('div');
                sub.className = 'top-item-sub';
                const typeSpan = document.createElement('span');
                typeSpan.textContent = item.is_dir ? `${Number(item.subtreeFiles || 0).toLocaleString()} tệp` : getFileExtensionType(item);
                const pctSpan = document.createElement('span');
                pctSpan.textContent = parentTotal > 0 ? `${pct.toFixed(1)}%` : '—';
                sub.appendChild(typeSpan);
                sub.appendChild(pctSpan);
                card.appendChild(sub);

                container.appendChild(card);
            });
        }

        

        // E. Xây dựng và điều khiển Bảng Cây Thư Mục (Tree-Table)
        function renderEmptyTreeTable(message) {
            const tbody = document.getElementById('treeTableBody');
            tbody.replaceChildren();
            const tr = document.createElement('tr');
            const td = document.createElement('td');
            td.colSpan = 6;
            td.style.textAlign = 'center';
            td.style.padding = '28px';
            td.style.color = 'var(--text-muted)';
            td.textContent = message;
            tr.appendChild(td);
            tbody.appendChild(tr);
        }

        function matchesFilter(node) {
            const hideReparse = document.getElementById('hideReparseChk').checked;
            if (hideReparse && node.is_reparse) {
                return false;
            }

            const query = (document.getElementById('searchInput').value || '').trim().toLowerCase();
            const typeFilter = document.getElementById('typeFilterSelect').value || 'all';

            if (query && !(node.name || '').toLowerCase().includes(query)) {
                return false;
            }
            if (typeFilter === 'dir' && !node.is_dir) return false;
            if (typeFilter === 'file' && node.is_dir) return false;
            if (typeFilter === 'compressed') {
                if (node.is_dir || node.is_hardlink) return false;
                if (!(node.allocatedSize < node.logicalSize && node.logicalSize > 0)) return false;
            }
            if (typeFilter === 'hardlink' && !node.is_hardlink) return false;
            if (typeFilter === 'reparse' && !node.is_reparse) return false;
            if (typeFilter === 'special' && !(node.is_hardlink || node.is_reparse)) return false;
            return true;
        }

        function rebuildTreeTableDOM() {
            if (!currentFocusNode) return;
            const tbody = document.getElementById('treeTableBody');
            tbody.replaceChildren();

            const rootModel = nodeMap.get(currentFocusNode.id);
            if (!rootModel) return;

            const frag = document.createDocumentFragment();
            let visibleCount = 0;

            function appendNodeAndVisibleChildren(nodeModel, isFocusRoot) {
                if (!isFocusRoot && !matchesFilter(nodeModel)) {
                    // Nếu thư mục đang mở rộng, vẫn kiểm tra các con bên trong
                    if (nodeModel.is_dir && nodeModel.expanded) {
                        nodeModel.loadedChildrenIds.forEach(cid => {
                            const cModel = nodeMap.get(cid);
                            if (cModel) appendNodeAndVisibleChildren(cModel, false);
                        });
                    }
                    return;
                }

                frag.appendChild(createTreeTableRow(nodeModel, isFocusRoot));
                visibleCount++;

                if (nodeModel.is_dir && nodeModel.expanded) {
                    nodeModel.loadedChildrenIds.forEach(cid => {
                        const cModel = nodeMap.get(cid);
                        if (cModel) appendNodeAndVisibleChildren(cModel, false);
                    });

                    // Dòng trạng thái đang tải hoặc lỗi của nhánh này
                    if (nodeModel.isLoading) {
                        frag.appendChild(createBranchStatusRow(nodeModel, '⏳ Đang tải danh sách mục con...', false));
                    } else if (nodeModel.loadError) {
                        frag.appendChild(createBranchStatusRow(nodeModel, '❌ Lỗi tải nhánh: ' + nodeModel.loadError, true));
                    } else if (nodeModel.hasMore) {
                        frag.appendChild(createLoadMoreRow(nodeModel));
                    } else if (nodeModel.loadedChildrenIds.length === 0) {
                        const emptyMsg = nodeModel.is_reparse
                            ? '↪️ Mục Reparse Point / Junction (Không duyệt đệ quy để tránh lặp và cộng trùng)'
                            : 'Thư mục trống (0 mục con)';
                        frag.appendChild(createBranchStatusRow(nodeModel, emptyMsg, false));
                    }
                }
            }

            appendNodeAndVisibleChildren(rootModel, true);

            if (visibleCount === 0) {
                renderEmptyTreeTable('Không có mục nào khớp với bộ lọc hiện tại.');
            } else {
                tbody.appendChild(frag);
            }
        }

        function createTreeTableRow(node, isFocusRoot) {
            const tr = document.createElement('tr');
            tr.id = 'row_node_' + node.id;
            tr.className = 'tree-row' + (selectedNodeId === node.id ? ' selected' : '');

            tr.onclick = () => selectNode(node.id);
            tr.ondblclick = () => {
                if (node.is_dir) {
                    if (!isFocusRoot) toggleExpandBranch(node.id);
                }
            };
            tr.oncontextmenu = (e) => showContextMenu(e, node.id);

            // Cột 1: Tên + Twisty + Badges
            const tdName = document.createElement('td');
            const wrap = document.createElement('div');
            wrap.className = 'name-cell-wrap';
            wrap.style.paddingLeft = (node.depth * 18) + 'px';

            const twisty = document.createElement('button');
            twisty.type = 'button';
            twisty.className = 'twisty-btn';
            if (node.is_dir && (node.hasChildren || isFocusRoot)) {
                twisty.textContent = node.expanded ? '▾' : '▸';
                twisty.onclick = (e) => {
                    e.stopPropagation();
                    toggleExpandBranch(node.id);
                };
            } else {
                twisty.textContent = '';
                twisty.disabled = true;
            }
            wrap.appendChild(twisty);

            const iconSpan = document.createElement('span');
            iconSpan.textContent = node.is_dir ? (node.is_reparse ? '↪️' : '📁') : '📄';
            wrap.appendChild(iconSpan);

            const nameSpan = document.createElement('span');
            nameSpan.className = 'item-label';
            nameSpan.style.fontWeight = node.is_dir ? '600' : '400';
            nameSpan.textContent = node.name;
            wrap.appendChild(nameSpan);

            if (node.is_hardlink) {
                const tag = document.createElement('span');
                tag.className = 'mini-tag tag-hl';
                tag.title = `Hardlink phụ: File gốc nặng ${node.refLogicalText}, đóng góp +0 B vào tổng để tránh tính trùng.`;
                tag.textContent = '🔗 Hardlink (+0 B)';
                wrap.appendChild(tag);
            }
            if (node.is_reparse) {
                const tag = document.createElement('span');
                tag.className = 'mini-tag tag-rp';
                tag.textContent = 'Reparse';
                wrap.appendChild(tag);
            }
            if (!node.is_dir && !node.is_hardlink && node.allocatedSize > 0 && node.allocatedSize < node.logicalSize) {
                const tag = document.createElement('span');
                tag.className = 'mini-tag tag-comp';
                tag.title = 'Tệp Nén NTFS hoặc Sparse: Dung lượng thực chiếm nhỏ hơn dung lượng logic.';
                tag.textContent = 'Nén/Sparse';
                wrap.appendChild(tag);
            }

            tdName.appendChild(wrap);
            tr.appendChild(tdName);

            // Cột 2: Loại
            const tdType = document.createElement('td');
            tdType.style.color = 'var(--text-muted)';
            tdType.textContent = getFileExtensionType(node);
            tr.appendChild(tdType);

            // Cột 3: Dung lượng Logic
            const tdLogical = document.createElement('td');
            tdLogical.className = 'col-num';
            tdLogical.style.fontWeight = '600';
            tdLogical.style.color = node.is_hardlink ? 'var(--warning)' : '#b5cea8';
            tdLogical.textContent = node.is_hardlink ? `0 B (Đích: ${node.refLogicalText})` : node.logicalText;
            tr.appendChild(tdLogical);

            // Cột 4: Dung lượng Cấp phát
            const tdAlloc = document.createElement('td');
            tdAlloc.className = 'col-num';
            tdAlloc.style.color = 'var(--text-muted)';
            if (node.is_hardlink) {
                tdAlloc.textContent = '0 B';
            } else if (!node.is_dir && node.logicalSize > 0 && node.allocatedSize === 0) {
                tdAlloc.textContent = '0 B (Resident)';
                tdAlloc.title = 'Tệp nhỏ nằm gọn trực tiếp trong bản ghi MFT 1KB, không tốn thêm Cluster rời.';
            } else {
                tdAlloc.textContent = node.allocatedText;
            }
            tr.appendChild(tdAlloc);

            // Cột 5: Tỷ lệ % trong thư mục cha
            const tdPct = document.createElement('td');
            tdPct.className = 'col-num';
            const pctWrap = document.createElement('div');
            pctWrap.className = 'pct-cell';

            const allocMode = isAllocatedMetricMode();
            const myVal = allocMode ? node.allocatedSize : node.logicalSize;
            const parentVal = allocMode ? node.parentAllocatedSize : node.parentLogicalSize;

            if (isFocusRoot) {
                const numSpan = document.createElement('span');
                numSpan.className = 'pct-num';
                numSpan.textContent = '100.0%';
                pctWrap.appendChild(numSpan);
            } else if (parentVal && parentVal > 0) {
                const pct = Math.min(100, (myVal * 100) / parentVal);
                const barBg = document.createElement('div');
                barBg.className = 'pct-bar-bg';
                const barFill = document.createElement('div');
                barFill.className = 'pct-bar-fill';
                barFill.style.width = pct.toFixed(1) + '%';
                barBg.appendChild(barFill);

                const numSpan = document.createElement('span');
                numSpan.className = 'pct-num';
                numSpan.textContent = pct.toFixed(1) + '%';

                pctWrap.appendChild(barBg);
                pctWrap.appendChild(numSpan);
            } else {
                const numSpan = document.createElement('span');
                numSpan.className = 'pct-num';
                numSpan.textContent = '0.0%';
                pctWrap.appendChild(numSpan);
            }
            tdPct.appendChild(pctWrap);
            tr.appendChild(tdPct);

            // Cột 6: Số tệp con
            const tdFiles = document.createElement('td');
            tdFiles.className = 'col-num';
            tdFiles.style.color = 'var(--text-muted)';
            tdFiles.textContent = node.is_dir ? Number(node.subtreeFiles || 0).toLocaleString() : '—';
            tr.appendChild(tdFiles);

            return tr;
        }

        function createBranchStatusRow(parentNode, text, isError) {
            const tr = document.createElement('tr');
            const td = document.createElement('td');
            td.colSpan = 6;
            td.style.paddingLeft = ((parentNode.depth + 1) * 18 + 24) + 'px';
            td.style.color = isError ? 'var(--danger)' : 'var(--text-muted)';
            td.style.fontSize = '12.5px';

            const span = document.createElement('span');
            span.textContent = text + ' ';
            td.appendChild(span);

            if (isError) {
                const btnRetry = document.createElement('button');
                btnRetry.type = 'button';
                btnRetry.className = 'btn-secondary';
                btnRetry.style.padding = '2px 8px';
                btnRetry.style.fontSize = '11.5px';
                btnRetry.textContent = '🔄 Thử lại';
                btnRetry.onclick = () => loadBranchChildrenPage(parentNode.id, true);
                td.appendChild(btnRetry);
            }

            tr.appendChild(td);
            return tr;
        }

        function createLoadMoreRow(parentNode) {
            const tr = document.createElement('tr');
            const td = document.createElement('td');
            td.colSpan = 6;
            td.style.paddingLeft = ((parentNode.depth + 1) * 18 + 24) + 'px';

            const remain = Math.max(0, parentNode.totalChildren - parentNode.nextOffset);
            const btn = document.createElement('button');
            btn.type = 'button';
            btn.className = 'btn-secondary';
            btn.style.padding = '3px 12px';
            btn.style.fontSize = '12px';
            btn.style.color = '#58a6ff';
            btn.textContent = `➕ Tải thêm ${remain.toLocaleString()} mục còn lại trong "${parentNode.name}"...`;
            btn.onclick = () => loadBranchChildrenPage(parentNode.id, false);

            td.appendChild(btn);
            tr.appendChild(td);
            return tr;
        }

        async function toggleExpandBranch(dirId) {
            const model = nodeMap.get(dirId);
            if (!model || !model.is_dir) return;

            if (model.expanded) {
                model.expanded = false;
                rebuildTreeTableDOM();
                return;
            }

            model.expanded = true;
            if (model.loadedChildrenIds.length === 0 && model.nextOffset === 0) {
                await loadBranchChildrenPage(dirId, true);
            } else {
                rebuildTreeTableDOM();
            }
        }

        async function loadBranchChildrenPage(dirId, resetBranch = false) {
            const model = nodeMap.get(dirId);
            if (!model || !model.is_dir) return;

            const reqKey = `children_${dirId}_${resetBranch ? 0 : model.nextOffset}`;
            if (inFlightRequests.has(reqKey)) return;
            inFlightRequests.add(reqKey);

            const myGen = scanGeneration;
            if (resetBranch) {
                model.loadedChildrenIds = [];
                model.nextOffset = 0;
            }
            model.isLoading = true;
            model.loadError = null;
            rebuildTreeTableDOM();

            try {
                const sortParam = getSortMode();
                const res = await apiFetch(`/api/children?id=${dirId}&offset=${model.nextOffset}&limit=${PAGE_LIMIT}&sort=${sortParam}`);
                if (myGen !== scanGeneration) return;
                if (!res.ok) throw new Error('HTTP ' + res.status);
                const data = await res.json();

                data.items.forEach(item => {
                    const existing = nodeMap.get(item.id);
                    const childModel = {
                        ...item,
                        parentLogicalSize: model.logicalSize,
                        parentAllocatedSize: model.allocatedSize,
                        depth: model.depth + 1,
                        expanded: existing ? existing.expanded : false,
                        loadedChildrenIds: existing ? existing.loadedChildrenIds : [],
                        nextOffset: existing ? existing.nextOffset : 0,
                        totalChildren: existing ? existing.totalChildren : 0,
                        hasMore: existing ? existing.hasMore : false,
                        loadError: null
                    };
                    nodeMap.set(item.id, childModel);
                    if (!model.loadedChildrenIds.includes(item.id)) {
                        model.loadedChildrenIds.push(item.id);
                    }
                });

                model.nextOffset += data.items.length;
                model.totalChildren = data.totalChildren;
                model.hasMore = data.hasMore;
                model.isLoading = false;
                rebuildTreeTableDOM();
            } catch (err) {
                if (myGen !== scanGeneration) return;
                model.isLoading = false;
                model.loadError = err.message;
                rebuildTreeTableDOM();
            } finally {
                inFlightRequests.delete(reqKey);
            }
        }

        function onHideReparseChange() {
            const hideChk = document.getElementById('hideReparseChk');
            const typeSel = document.getElementById('typeFilterSelect');
            // Tránh xung đột nếu người dùng đang chọn "Chỉ Reparse Point" mà lại bật "Ẩn Reparse Point"
            if (hideChk.checked && typeSel.value === 'reparse') {
                typeSel.value = 'all';
            }
            try {
                localStorage.setItem('ntfs_hide_reparse', hideChk.checked ? '1' : '0');
            } catch (e) {}
            applyClientFilters();
        }

        function onTypeFilterChange() {
            const hideChk = document.getElementById('hideReparseChk');
            const typeSel = document.getElementById('typeFilterSelect');
            // Tự động bỏ tích "Ẩn Reparse Point" nếu người dùng chủ động chọn lọc "Chỉ Reparse Point"
            if (typeSel.value === 'reparse' && hideChk.checked) {
                hideChk.checked = false;
                try { localStorage.setItem('ntfs_hide_reparse', '0'); } catch (e) {}
            }
            applyClientFilters();
        }

        function onSearchInput() {
            if (searchDebounceTimer) clearTimeout(searchDebounceTimer);
            searchDebounceTimer = setTimeout(() => {
                applyClientFilters();
            }, 200);
        }

        function applyClientFilters() {
            if (currentFocusNode) {
                renderTopLargestItems(currentFocusItems, currentFocusNode);
            }
            rebuildTreeTableDOM();
        }

        function onSortOrMetricChange() {
            if (!currentFocusNode) return;
            focusDirectoryNode(currentFocusNode.id);
        }

        // F. Khu vực Chi tiết mục được chọn (Inspector)
        async function selectNode(nodeId) {
            selectedNodeId = nodeId;
            document.querySelectorAll('tr.tree-row.selected').forEach(el => el.classList.remove('selected'));
            const rowEl = document.getElementById('row_node_' + nodeId);
            if (rowEl) rowEl.classList.add('selected');

            const node = nodeMap.get(nodeId);
            renderInspector(node);
        }

        async function renderInspector(node) {
            const box = document.getElementById('inspectorBody');
            box.replaceChildren();

            if (!node) {
                const msg = document.createElement('div');
                msg.style.color = 'var(--text-muted)';
                msg.textContent = 'Hãy chọn một tệp hoặc thư mục trong bảng bên trái để xem thông tin chi tiết.';
                box.appendChild(msg);
                return;
            }

            const myReqId = ++detailRequestId;

            function addRow(label, valText, valColor = null) {
                const row = document.createElement('div');
                row.className = 'insp-row';
                const k = document.createElement('div');
                k.className = 'insp-key';
                k.textContent = label;
                const v = document.createElement('div');
                v.className = 'insp-val';
                if (valColor) v.style.color = valColor;
                v.textContent = valText;
                row.appendChild(k);
                row.appendChild(v);
                box.appendChild(row);
                return v;
            }

            addRow('Tên mục', node.name);
            const pathValEl = addRow('Đường dẫn đầy đủ', '⏳ Đang truy vấn đường dẫn...');
            addRow('Loại mục & MFT Record ID', `${getFileExtensionType(node)} (#${node.id})`);

            if (node.is_hardlink) {
                addRow('Dung lượng đóng góp vào thư mục', '0 B (Hardlink phụ - chống cộng trùng)', 'var(--warning)');
                addRow('Dung lượng tệp gốc tham chiếu', `${node.refLogicalText} (${Number(node.refLogicalSize).toLocaleString()} B)`);
            } else {
                addRow('Dung lượng Logic (DataSize)', `${node.logicalText} (${Number(node.logicalSize).toLocaleString()} bytes)`, '#b5cea8');
                const allocDesc = (!node.is_dir && node.logicalSize > 0 && node.allocatedSize === 0)
                    ? '0 B (Lưu trực tiếp Resident trong MFT Record)'
                    : `${node.allocatedText} (${Number(node.allocatedSize).toLocaleString()} bytes)`;
                addRow('Dung lượng Cấp phát (Thực chiếm)', allocDesc);
            }

            if (node.is_dir) {
                addRow('Tổng số tệp con trong nhánh', `${Number(node.subtreeFiles || 0).toLocaleString()} tệp`);
            }
            addRow('Thời gian sửa đổi', '— (Tối ưu RAM 40B/record)');

            // Các nút hành động
            const actWrap = document.createElement('div');
            actWrap.className = 'insp-actions';

            const btnOpen = document.createElement('button');
            btnOpen.type = 'button';
            btnOpen.className = 'btn-primary';
            btnOpen.textContent = '📂 Mở vị trí trong Windows Explorer';
            btnOpen.onclick = () => openExplorerById(node.id);
            actWrap.appendChild(btnOpen);

            const btnCopy = document.createElement('button');
            btnCopy.type = 'button';
            btnCopy.className = 'btn-secondary';
            btnCopy.textContent = '📋 Sao chép đường dẫn đầy đủ';
            btnCopy.onclick = () => copyPathToClipboard(node.id, btnCopy);
            actWrap.appendChild(btnCopy);

            if (node.is_dir) {
                if (!node.is_reparse && currentFocusNode && node.id !== currentFocusNode.id) {
                    const btnDrill = document.createElement('button');
                    btnDrill.type = 'button';
                    btnDrill.className = 'btn-secondary';
                    btnDrill.textContent = '🔍 Đặt thư mục này làm gốc hiển thị';
                    btnDrill.onclick = () => focusDirectoryNode(node.id);
                    actWrap.appendChild(btnDrill);
                }
                const btnRefresh = document.createElement('button');
                btnRefresh.type = 'button';
                btnRefresh.className = 'btn-secondary';
                btnRefresh.textContent = '🔄 Làm mới danh sách con của nhánh này';
                btnRefresh.onclick = () => {
                    if (currentFocusNode && node.id === currentFocusNode.id) {
                        focusDirectoryNode(node.id);
                    } else {
                        node.expanded = true;
                        loadBranchChildrenPage(node.id, true);
                    }
                };
                actWrap.appendChild(btnRefresh);
            }

            box.appendChild(actWrap);

            // Tải đường dẫn đầy đủ từ API /api/path
            try {
                const res = await apiFetch(`/api/path?id=${node.id}`);
                if (myReqId !== detailRequestId) return;
                if (!res.ok) throw new Error('HTTP ' + res.status);
                const data = await res.json();
                pathValEl.textContent = data.path || '—';
            } catch (err) {
                if (myReqId !== detailRequestId) return;
                pathValEl.style.color = 'var(--danger)';
                pathValEl.textContent = 'Lỗi lấy đường dẫn: ' + err.message;
            }
        }

        async function openExplorerById(nodeId) {
            try {
                const res = await apiFetch(`/api/open?id=${nodeId}`, { method: 'POST' });
                if (!res.ok) {
                    const errData = await res.json();
                    throw new Error(errData.error || ('HTTP ' + res.status));
                }
            } catch (err) {
                addNoticeBanner('danger', 'Không thể mở Explorer: ' + err.message);
            }
        }

        async function copyPathToClipboard(nodeId, btnEl = null) {
            try {
                const res = await apiFetch(`/api/path?id=${nodeId}`);
                if (!res.ok) throw new Error('HTTP ' + res.status);
                const data = await res.json();
                if (data.path) {
                    await navigator.clipboard.writeText(data.path);
                    if (btnEl) {
                        const orig = btnEl.textContent;
                        btnEl.textContent = '✔️ Đã sao chép vào Clipboard!';
                        setTimeout(() => { btnEl.textContent = orig; }, 1400);
                    }
                }
            } catch (err) {
                addNoticeBanner('danger', 'Lỗi sao chép đường dẫn: ' + err.message);
            }
        }

        // Context Menu chuột phải
        function showContextMenu(e, nodeId) {
            e.preventDefault();
            e.stopPropagation();
            selectNode(nodeId);
            contextMenuNodeId = nodeId;

            const node = nodeMap.get(nodeId);
            const menu = document.getElementById('contextMenu');
            document.getElementById('ctxDrillDownItem').style.display = (node && node.is_dir && !node.is_reparse) ? 'flex' : 'none';
            document.getElementById('ctxRefreshItem').style.display = (node && node.is_dir) ? 'flex' : 'none';

            menu.style.display = 'block';
            const x = Math.min(e.clientX, window.innerWidth - 225);
            const y = Math.min(e.clientY, window.innerHeight - 160);
            menu.style.left = x + 'px';
            menu.style.top = y + 'px';
        }

        window.addEventListener('click', () => {
            document.getElementById('contextMenu').style.display = 'none';
        });
        window.addEventListener('keydown', (e) => {
            if (e.key === 'Escape') {
                document.getElementById('contextMenu').style.display = 'none';
            } else if (e.key === 'Backspace' && document.activeElement.tagName !== 'INPUT') {
                navigateUpDirectory();
            }
        });

        function ctxOpenExplorer() {
            if (contextMenuNodeId !== null) openExplorerById(contextMenuNodeId);
        }
        function ctxCopyPath() {
            if (contextMenuNodeId !== null) copyPathToClipboard(contextMenuNodeId);
        }
        function ctxDrillDown() {
            if (contextMenuNodeId !== null) focusDirectoryNode(contextMenuNodeId);
        }
        function ctxRefreshBranch() {
            if (contextMenuNodeId === null) return;
            const node = nodeMap.get(contextMenuNodeId);
            if (!node || !node.is_dir) return;
            if (currentFocusNode && node.id === currentFocusNode.id) {
                focusDirectoryNode(node.id);
            } else {
                node.expanded = true;
                loadBranchChildrenPage(node.id, true);
            }
        }

        // Section 5: Khu vực Chẩn đoán, Telemetry & Benchmark (Dựng bằng DOM an toàn)
        function renderTelemetryDiagnostics(t, rootNode) {
            const grid = document.getElementById('telemetryGrid');
            grid.replaceChildren();

            const items = [
                { k: 'Phương pháp quét thực tế', v: `${t.engineUsed} (Batch ${t.chunkSizeMb} MB)` },
                { k: 'Độ phủ bản ghi MFT', v: `${Number(t.processedMftRecords).toLocaleString()} / ${Number(t.totalMftRecords).toLocaleString()} records` },
                { k: 'Thời gian từng pha (ms)', v: `Đợi I/O: ${t.readMftMs}ms | Parse: ${t.parseRecordsMs}ms | Link: ${t.linkTreeMs}ms | Sum: ${t.sumSizesMs}ms | Sort: ${t.sortMs}ms` },
                { k: 'Bộ nhớ cấu trúc RAM', v: `sizeof=${t.sizeofRecordStruct}B | Vector: ${Number(t.recordsVectorMb).toFixed(1)} MB | Arena: ${Number(t.stringArenaMb).toFixed(1)} MB` },
                { k: 'Siêu dữ liệu NTFS đã tách', v: `${t.skippedSystemRecords} bản ghi (${t.systemMetaAllocatedText})` },
                { k: 'Bản ghi lỗi / Từ chối truy cập', v: `Hỏng USA/Attr: ${t.corruptedRecords} | Không hỗ trợ: ${t.unsupportedAttributes} | Access Denied: ${t.accessDeniedDirs}` }
            ];

            items.forEach(it => {
                const card = document.createElement('div');
                card.className = 'diag-card';
                const lbl = document.createElement('div');
                lbl.className = 'metric-label';
                lbl.textContent = it.k;
                const val = document.createElement('div');
                val.style.fontWeight = '600';
                val.style.marginTop = '3px';
                val.textContent = it.v;
                card.appendChild(lbl);
                card.appendChild(val);
                grid.appendChild(card);
            });
        }

        async function runSelfTests() {
            if (isBusyTask) return;
            setBusyState(true);
            const statusEl = document.getElementById('diagActionStatus');
            statusEl.textContent = '🧪 Đang chạy 7 bài kiểm thử tự động...';
            document.getElementById('diagAccordion').open = true;

            try {
                const res = await apiFetch('/api/selftest', { method: 'POST' });
                if (!res.ok) throw new Error('HTTP ' + res.status);
                const data = await res.json();
                lastDiagExportObj.selfTest = data;

                const box = document.getElementById('diagResultBox');
                box.replaceChildren();

                const title = document.createElement('div');
                title.style.fontWeight = '700';
                title.style.marginBottom = '6px';
                title.style.color = data.allPassed ? 'var(--success)' : 'var(--danger)';
                title.textContent = `${data.suite} — ${data.allPassed ? 'ĐẠT 7/7 BÀI KIỂM THỬ' : 'CÓ BÀI KIỂM THỬ THẤT BẠI'}`;
                box.appendChild(title);

                const tbl = document.createElement('table');
                tbl.className = 'diag-table';
                const thead = document.createElement('thead');
                const trH = document.createElement('tr');
                ['Bài kiểm thử', 'Trạng thái', 'Chi tiết xác minh'].forEach(h => {
                    const th = document.createElement('th');
                    th.textContent = h;
                    trH.appendChild(th);
                });
                thead.appendChild(trH);
                tbl.appendChild(thead);

                const tbody = document.createElement('tbody');
                (data.tests || []).forEach(t => {
                    const tr = document.createElement('tr');
                    const tdName = document.createElement('td');
                    tdName.style.fontWeight = '600';
                    tdName.textContent = t.name;

                    const tdPass = document.createElement('td');
                    tdPass.style.fontWeight = '700';
                    tdPass.style.color = t.passed ? 'var(--success)' : 'var(--danger)';
                    tdPass.textContent = t.passed ? '✔️ PASS' : '❌ FAIL';

                    const tdDet = document.createElement('td');
                    tdDet.textContent = t.details;

                    tr.appendChild(tdName);
                    tr.appendChild(tdPass);
                    tr.appendChild(tdDet);
                    tbody.appendChild(tr);
                });
                tbl.appendChild(tbody);
                box.appendChild(tbl);

                statusEl.textContent = 'Hoàn thành Tự Kiểm Thử.';
            } catch (err) {
                statusEl.textContent = 'Lỗi kiểm thử: ' + err.message;
            } finally {
                setBusyState(false);
            }
        }

        async function runBenchmark() {
            if (isBusyTask) return;
            setBusyState(true);
            const path = document.getElementById('pathInput').value.trim() || 'C:\\';
            const statusEl = document.getElementById('diagActionStatus');
            statusEl.textContent = '⏱️ Đang chạy Benchmark thực nghiệm (USN_DATA vs Raw MFT 4/8/16MB)...';
            document.getElementById('diagAccordion').open = true;

            try {
                const res = await apiFetch(`/api/benchmark?path=${encodeURIComponent(path)}`, { method: 'POST' });
                const data = await res.json();
                if (!res.ok) throw new Error(data.error || ('HTTP ' + res.status));
                lastDiagExportObj.benchmark = data;

                const box = document.getElementById('diagResultBox');
                box.replaceChildren();

                const title = document.createElement('div');
                title.style.fontWeight = '700';
                title.style.marginBottom = '6px';
                title.style.color = '#58a6ff';
                title.textContent = `📊 Kết quả Benchmark trên phân vùng ${data.drive} (Lưu ý: USN_DATA không có dữ liệu kích thước tệp nên không tương đương phạm vi công việc với Raw MFT)`;
                box.appendChild(title);

                const tbl = document.createElement('table');
                tbl.className = 'diag-table';
                const thead = document.createElement('thead');
                const trH = document.createElement('tr');
                ['Phương pháp', 'Tổng thời gian', 'Đợi I/O', 'CPU Parse', 'Bản ghi xử lý', 'Đủ File Size?', 'Ghi chú phạm vi'].forEach(h => {
                    const th = document.createElement('th');
                    th.textContent = h;
                    trH.appendChild(th);
                });
                thead.appendChild(trH);
                tbl.appendChild(thead);

                const tbody = document.createElement('tbody');
                (data.tests || []).forEach(t => {
                    const tr = document.createElement('tr');
                    const vals = [
                        t.method,
                        t.elapsedMs >= 0 ? (t.elapsedMs + ' ms') : 'Lỗi',
                        t.ioReadMs !== undefined ? (t.ioReadMs + ' ms') : '—',
                        t.parseMs !== undefined ? (t.parseMs + ' ms') : '—',
                        Number(t.recordsProcessed || 0).toLocaleString(),
                        t.hasFileSize ? '✔️ Đầy đủ' : '❌ Không có',
                        t.note || ''
                    ];
                    vals.forEach((v, idx) => {
                        const td = document.createElement('td');
                        if (idx === 0) td.style.fontWeight = '600';
                        td.textContent = v;
                        tr.appendChild(td);
                    });
                    tbody.appendChild(tr);
                });
                tbl.appendChild(tbody);
                box.appendChild(tbl);

                statusEl.textContent = 'Hoàn thành Benchmark.';
            } catch (err) {
                statusEl.textContent = 'Benchmark thất bại: ' + err.message;
            } finally {
                setBusyState(false);
            }
        }

        async function copyDiagnosticsJson() {
            const statusEl = document.getElementById('diagActionStatus');
            try {
                const jsonStr = JSON.stringify(lastDiagExportObj, null, 2);
                await navigator.clipboard.writeText(jsonStr);
                statusEl.textContent = '✔️ Đã sao chép JSON Chẩn đoán vào Clipboard!';
            } catch (err) {
                statusEl.textContent = 'Lỗi sao chép: ' + err.message;
            }
        }
    </script>
</body>
</html>
)=====";

    size_t pos = html.find("__SESSION_TOKEN__");
    if (pos != std::string::npos) {
        html.replace(pos, 17, sessionToken);
    }
    return html;
}

// =====================================================================
// 7. MÁY CHỦ API HTTP & ĐIỂM VÀO CHƯƠNG TRÌNH
// =====================================================================
std::string GenerateSessionToken() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << dis(gen) << std::setw(16) << dis(gen);
    return oss.str();
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--test") {
            json testRep = DiskScanner::RunAutomatedSelfTests();
            std::cout << testRep.dump(2) << "\n";
            return testRep["allPassed"].get<bool>() ? 0 : 1;
        }
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    httplib::Server svr;
    ScanManager manager;
    const std::string sessionToken = GenerateSessionToken();
    const std::string webGuiHtml = BuildWebGuiHtml(sessionToken);

    auto verifyToken = [&](const httplib::Request& req, httplib::Response& res) -> bool {
        if (!req.has_header("X-Scanner-Token") || req.get_header_value("X-Scanner-Token") != sessionToken) {
            res.status = 403;
            res.set_content(R"({"error": "Forbidden: Invalid session token"})", "application/json");
            return false;
        }
        return true;
        };

    svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(webGuiHtml, "text/html; charset=utf-8");
        });

    svr.Get("/api/info", [&](const httplib::Request&, httplib::Response& res) {
        json j;
        j["isAdmin"] = (IsUserAnAdmin() != FALSE);
        res.set_content(j.dump(), "application/json");
        });

    svr.Post("/api/elevate", [&](const httplib::Request& req, httplib::Response& res) {
        if (!verifyToken(req, res)) return;
        wchar_t exePath[MAX_PATH];
        if (GetModuleFileNameW(NULL, exePath, MAX_PATH)) {
            HINSTANCE hInst = ShellExecuteW(NULL, L"runas", exePath, L"--no-browser", NULL, SW_SHOW);
            if (reinterpret_cast<INT_PTR>(hInst) > 32) {
                res.set_content(R"({"status": "elevating"})", "application/json");
                std::thread([&svr]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    svr.stop();
                    }).detach();
                return;
            }
        }
        res.status = 400;
        res.set_content(R"({"status": "failed"})", "application/json");
        });

    svr.Post("/api/open", [&](const httplib::Request& req, httplib::Response& res) {
        if (!verifyToken(req, res)) return;
        if (!req.has_param("id")) {
            res.status = 400;
            res.set_content(R"({"error": "Missing node id"})", "application/json");
            return;
        }

        uint32_t nodeId = 0;
        try { nodeId = static_cast<uint32_t>(std::stoul(req.get_param_value("id"))); }
        catch (...) {
            res.status = 400;
            res.set_content(R"({"error": "Invalid node id"})", "application/json");
            return;
        }

        std::wstring targetPathW;
        {
            std::shared_lock<std::shared_mutex> lock(manager.stateMutex);
            if (manager.statusState != "done" || nodeId >= manager.records.size()) {
                res.status = 404;
                res.set_content(R"({"error": "Node not found"})", "application/json");
                return;
            }
            targetPathW = manager.ReconstructFullPathW(nodeId);
        }

        // Chặn đường dẫn rỗng hoặc chứa ký tự ngoặc kép bất thường
        if (targetPathW.empty() || targetPathW.find(L'\"') != std::wstring::npos) {
            res.status = 403;
            res.set_content(R"({"error": "Invalid or out-of-scope path"})", "application/json");
            return;
        }

        // Kiểm tra đường dẫn có tồn tại trên đĩa hay không
        DWORD attrs = GetFileAttributesW(targetPathW.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            res.status = 404;
            res.set_content(R"({"error": "Path does not exist on disk"})", "application/json");
            return;
        }

        // Trường hợp 1: Nếu là thư mục gốc của ổ đĩa (VD: "C:\"), mở trực tiếp ổ đĩa đó
        if (targetPathW.size() <= 3 && targetPathW.size() >= 2 && targetPathW[1] == L':') {
            std::wstring driveRoot = targetPathW.substr(0, 2) + L"\\";
            ShellExecuteW(NULL, L"open", driveRoot.c_str(), NULL, NULL, SW_SHOWNORMAL);
            res.set_content(R"({"status": "ok"})", "application/json");
            return;
        }

        // Trường hợp 2: Thử dùng COM Shell API trên luồng Worker hiện tại
        HRESULT hrCom = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        bool openedViaShellApi = false;

        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(targetPathW.c_str());
        if (pidl) {
            HRESULT hrOpen = SHOpenFolderAndSelectItems(pidl, 0, NULL, 0);
            if (SUCCEEDED(hrOpen)) {
                openedViaShellApi = true;
            }
            ILFree(pidl);
        }

        if (SUCCEEDED(hrCom)) {
            CoUninitialize();
        }

        // Trường hợp 3: Nếu SHOpenFolderAndSelectItems bị chặn do lệch quyền Admin/User (UIPI),
        // chuyển sang gọi explorer.exe /select,"<path>" (hoạt động với mọi mức quyền)
        if (!openedViaShellApi) {
            std::wstring params = L"/select,\"" + targetPathW + L"\"";
            HINSTANCE hInst = ShellExecuteW(NULL, L"open", L"explorer.exe", params.c_str(), NULL, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(hInst) <= 32) {
                res.status = 500;
                res.set_content(R"({"error": "Failed to launch Windows Explorer"})", "application/json");
                return;
            }
        }

        res.set_content(R"({"status": "ok"})", "application/json");
        });

    svr.Get("/api/path", [&](const httplib::Request& req, httplib::Response& res) {
        if (!verifyToken(req, res)) return;
        if (!req.has_param("id")) {
            res.status = 400;
            return;
        }
        uint32_t nodeId = 0;
        try { nodeId = static_cast<uint32_t>(std::stoul(req.get_param_value("id"))); }
        catch (...) { res.status = 400; return; }

        std::shared_lock<std::shared_mutex> lock(manager.stateMutex);
        std::wstring pathW = manager.ReconstructFullPathW(nodeId);
        json j;
        j["path"] = WStringToUTF8(pathW);
        res.set_content(j.dump(), "application/json");
        });

    svr.Post("/api/start", [&](const httplib::Request& req, httplib::Response& res) {
        if (!verifyToken(req, res)) return;
        if (manager.isScanning.load(std::memory_order_relaxed)) {
            res.status = 409;
            res.set_content(R"({"error": "Đang có tiến trình quét chạy!"})", "application/json");
            return;
        }

        std::string path = req.has_param("path") ? req.get_param_value("path") : "C:\\";
        if (path.size() > 32768) {
            res.status = 400;
            res.set_content(R"({"error": "Đường dẫn vượt quá độ dài cho phép!"})", "application/json");
            return;
        }

        path.erase(std::remove(path.begin(), path.end(), '\"'), path.end());
        path.erase(std::remove(path.begin(), path.end(), '\''), path.end());
        while (!path.empty() && std::isspace(static_cast<unsigned char>(path.back()))) path.pop_back();
        while (!path.empty() && std::isspace(static_cast<unsigned char>(path.front()))) path.erase(path.begin());
        if (path.empty()) path = "C:\\";

        uint32_t chunkMb = 8;
        if (req.has_param("chunkMb")) {
            try { chunkMb = static_cast<uint32_t>(std::stoul(req.get_param_value("chunkMb"))); }
            catch (...) { chunkMb = 8; }
        }
        if (chunkMb != 4 && chunkMb != 8 && chunkMb != 16) chunkMb = 8;

        bool allowFallback = true;
        if (req.has_param("fallback")) {
            allowFallback = (req.get_param_value("fallback") != "0");
        }

        if (!manager.StartAsync(path, chunkMb, allowFallback)) {
            res.status = 409;
            res.set_content(R"({"error": "Không thể khởi tạo luồng quét."})", "application/json");
            return;
        }
        res.set_content(R"({"status": "started"})", "application/json");
        });

    svr.Get("/api/status", [&](const httplib::Request& req, httplib::Response& res) {
        if (!verifyToken(req, res)) return;
        std::shared_lock<std::shared_mutex> lock(manager.stateMutex);
        json j;
        j["state"] = manager.statusState;
        j["scannedFiles"] = manager.scannedCount.load(std::memory_order_relaxed);
        j["totalRecords"] = manager.totalRecordsExpected.load(std::memory_order_relaxed);
        j["currentDir"] = manager.currentProgressText;
        j["engine"] = manager.activeEngineText;
        j["elapsedMs"] = manager.GetRunningElapsedMs();
        if (manager.statusState == "error") j["error"] = manager.errorMsg;
        res.set_content(j.dump(), "application/json");
        });

    svr.Get("/api/result", [&](const httplib::Request& req, httplib::Response& res) {
        if (!verifyToken(req, res)) return;
        std::shared_lock<std::shared_mutex> lock(manager.stateMutex);
        if (manager.statusState != "done" || manager.rootRecordIdx >= manager.records.size()) {
            res.status = 404;
            res.set_content(R"({"error": "No result available"})", "application/json");
            return;
        }
        json j;
        j["root"] = manager.BuildNodeSummaryJson(manager.rootRecordIdx, true);
        j["telemetry"] = manager.GetTelemetryJson();
        res.set_content(j.dump(), "application/json");
        });

    svr.Get("/api/children", [&](const httplib::Request& req, httplib::Response& res) {
        if (!verifyToken(req, res)) return;
        uint32_t dirId = 0;
        size_t offset = 0;
        size_t limit = 150;
        int sortMode = 0; // 0 = logical, 1 = allocated, 2 = name
        try {
            if (req.has_param("id")) dirId = static_cast<uint32_t>(std::stoul(req.get_param_value("id")));
            if (req.has_param("offset")) offset = std::stoull(req.get_param_value("offset"));
            if (req.has_param("limit")) limit = std::clamp<size_t>(std::stoull(req.get_param_value("limit")), 10, 1000);
            if (req.has_param("sort")) {
                std::string s = req.get_param_value("sort");
                if (s == "allocated") sortMode = 1;
                else if (s == "name") sortMode = 2;
            }
        }
        catch (...) {
            res.status = 400;
            res.set_content(R"({"error": "Invalid pagination parameters"})", "application/json");
            return;
        }

        std::shared_lock<std::shared_mutex> lock(manager.stateMutex);
        if (manager.statusState != "done") {
            res.status = 400;
            res.set_content(R"({"error": "Scan not finished"})", "application/json");
            return;
        }
        json j = manager.GetChildrenJson(dirId, offset, limit, sortMode);
        res.set_content(j.dump(), "application/json");
        });

    svr.Post("/api/selftest", [&](const httplib::Request& req, httplib::Response& res) {
        if (!verifyToken(req, res)) return;
        json testRep = DiskScanner::RunAutomatedSelfTests();
        res.set_content(testRep.dump(), "application/json");
        });

    svr.Post("/api/benchmark", [&](const httplib::Request& req, httplib::Response& res) {
        if (!verifyToken(req, res)) return;
        if (manager.isScanning.load(std::memory_order_relaxed)) {
            res.status = 409;
            res.set_content(R"({"error": "Đang quét, không thể chạy Benchmark!"})", "application/json");
            return;
        }
        std::string path = req.has_param("path") ? req.get_param_value("path") : "C:\\";
        try {
            json benchRes = DiskScanner::RunBenchmark(path);
            res.set_content(benchRes.dump(), "application/json");
        }
        catch (const std::exception& e) {
            res.status = 400;
            json err;
            err["error"] = e.what();
            res.set_content(err.dump(), "application/json");
        }
        });

    svr.Post("/api/cancel", [&](const httplib::Request& req, httplib::Response& res) {
        if (!verifyToken(req, res)) return;
        manager.cancelFlag.store(true, std::memory_order_relaxed);
        res.set_content(R"({"status": "cancelling"})", "application/json");
        });

    std::cout << "=================================================================\n";
    std::cout << "   DISK SCANNER V9.1 - ENTERPRISE UI/UX & ASYNC MFT ENGINE       \n";
    std::cout << "=================================================================\n";
    std::cout << "[i] sizeof(MftRecordInfo) = " << sizeof(MftRecordInfo) << " bytes\n" << std::flush;

    json bootTest = DiskScanner::RunAutomatedSelfTests();
    std::cout << "[+] Self-Test (7/7 Suite: USA, Hardlink, Compressed & Reconcile): "
        << (bootTest["allPassed"].get<bool>() ? "7/7 PASSED" : "FAILED") << "\n";
    std::cout << "=> Truy cap Web GUI tai: http://127.0.0.1:8080\n" << std::flush;

    bool openBrowser = true;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--no-browser") openBrowser = false;
    }
    if (openBrowser) {
        ShellExecuteA(0, 0, "http://127.0.0.1:8080", 0, 0, SW_SHOW);
    }

    if (!svr.listen("127.0.0.1", 8080)) {
        std::cerr << "[!] LOI: Khong the mo cong 8080 (Co the cong dang bi chiem boi tien trinh cu)!\n";
        std::cerr << "    Hay tat cac tien trinh DiskScanner cu trong Task Manager roi thu lai.\n";
        system("pause");
    }

    CoUninitialize();
    return 0;
}