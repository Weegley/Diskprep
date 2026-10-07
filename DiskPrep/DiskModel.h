#pragma once

#include <windows.h>
#include <winioctl.h>
#include <string>
#include <vector>
#include <cstdint>

enum class SegmentKind
{
    Partition,
    FreeSpace
};

struct Segment
{
    SegmentKind kind = SegmentKind::FreeSpace;
    int partitionNumber = 0;
    uint64_t offset = 0;
    uint64_t length = 0;

    PARTITION_STYLE style = PARTITION_STYLE_RAW;
    GUID gptType{};
    BYTE mbrType = 0;
    bool mbrBootIndicator = false;

    std::wstring typeName;
    std::wstring fileSystem;
    std::wstring label;
    std::wstring driveLetter;
};

struct DiskInfo
{
    int number = -1;
    uint64_t size = 0;
    DWORD logicalSectorSize = 512;
    PARTITION_STYLE style = PARTITION_STYLE_RAW;
    std::wstring model;
    std::vector<Segment> segments;
};

namespace DiskModel
{
    extern const GUID GPT_EFI_SYSTEM;
    extern const GUID GPT_MICROSOFT_RESERVED;
    extern const GUID GPT_BASIC_DATA;
    extern const GUID GPT_WINDOWS_RECOVERY;

    std::vector<DiskInfo> EnumerateDisks();
    std::wstring PartitionStyleName(PARTITION_STYLE style);
    std::wstring FormatBytes(uint64_t bytes);

    bool IsEfi(const Segment& s);
    bool IsMsr(const Segment& s);
    bool IsRecovery(const Segment& s);
    bool IsBasicData(const Segment& s);

    // Unicode-safe volume label update. The volume is located by physical disk
    // number and partition starting offset, so a drive letter is not required.
    bool SetVolumeLabelByExtent(int diskNumber, uint64_t partitionOffset,
        const std::wstring& label, std::wstring& error);
}
