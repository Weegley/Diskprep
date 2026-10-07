#include "DiskModel.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <iomanip>
#include <vector>

#pragma comment(lib, "ole32.lib")

namespace
{
    struct VolumeInfo
    {
        DWORD diskNumber = MAXDWORD;
        uint64_t offset = 0;
        uint64_t length = 0;
        std::wstring fileSystem;
        std::wstring label;
        std::wstring driveLetter;
        std::wstring volumeName;
    };

    constexpr uint64_t MiB = 1024ull * 1024ull;

    std::wstring Win32Error(DWORD code)
    {
        wchar_t* msg = nullptr;
        DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
        FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
        std::wstring out = msg ? msg : L"Unknown error";
        if (msg) LocalFree(msg);
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n')) out.pop_back();
        return out;
    }

    bool GuidEq(const GUID& a, const GUID& b)
    {
        return !!IsEqualGUID(a, b);
    }

    std::wstring TrimAsciiField(const char* p)
    {
        if (!p) return L"";
        int needed = MultiByteToWideChar(CP_ACP, 0, p, -1, nullptr, 0);
        if (needed <= 1) return L"";
        std::wstring out(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_ACP, 0, p, -1, out.data(), needed);
        while (!out.empty() && (out.back() == L' ' || out.back() == L'\0')) out.pop_back();
        size_t first = 0;
        while (first < out.size() && out[first] == L' ') ++first;
        if (first) out.erase(0, first);
        return out;
    }

    std::wstring GetDiskModel(HANDLE h)
    {
        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;

        BYTE buffer[4096]{};
        DWORD returned = 0;
        if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
            &query, sizeof(query), buffer, sizeof(buffer), &returned, nullptr))
        {
            return L"Physical disk";
        }

        auto* d = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buffer);
        std::wstring vendor;
        std::wstring product;

        if (d->VendorIdOffset && d->VendorIdOffset < returned)
            vendor = TrimAsciiField(reinterpret_cast<char*>(buffer + d->VendorIdOffset));
        if (d->ProductIdOffset && d->ProductIdOffset < returned)
            product = TrimAsciiField(reinterpret_cast<char*>(buffer + d->ProductIdOffset));

        std::wstring model = vendor;
        if (!product.empty())
        {
            if (!model.empty()) model += L" ";
            model += product;
        }
        return model.empty() ? L"Physical disk" : model;
    }

    bool QueryGeometry(HANDLE h, uint64_t& size, DWORD& logicalSectorSize)
    {
        std::vector<BYTE> buffer(4096);
        DWORD returned = 0;
        if (!DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
            nullptr, 0, buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr))
        {
            return false;
        }

        auto* g = reinterpret_cast<DISK_GEOMETRY_EX*>(buffer.data());
        size = static_cast<uint64_t>(g->DiskSize.QuadPart);
        logicalSectorSize = g->Geometry.BytesPerSector ? g->Geometry.BytesPerSector : 512;
        return true;
    }

    bool QueryLayout(HANDLE h, std::vector<BYTE>& buffer, DRIVE_LAYOUT_INFORMATION_EX*& layout)
    {
        DWORD returned = 0;
        buffer.resize(64 * 1024);
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                nullptr, 0, buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr))
            {
                layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(buffer.data());
                return true;
            }
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
            buffer.resize(buffer.size() * 2);
        }
        layout = nullptr;
        return false;
    }

    std::vector<VolumeInfo> EnumerateVolumes()
    {
        std::vector<VolumeInfo> result;
        wchar_t volumeName[MAX_PATH]{};
        HANDLE find = FindFirstVolumeW(volumeName, ARRAYSIZE(volumeName));
        if (find == INVALID_HANDLE_VALUE) return result;

        do
        {
            std::wstring openPath = volumeName;
            if (!openPath.empty() && openPath.back() == L'\\') openPath.pop_back();

            HANDLE hVol = CreateFileW(openPath.c_str(), 0,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (hVol != INVALID_HANDLE_VALUE)
            {
                BYTE extBuffer[sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT) * 8]{};
                DWORD returned = 0;
                if (DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                    nullptr, 0, extBuffer, sizeof(extBuffer), &returned, nullptr))
                {
                    auto* ex = reinterpret_cast<VOLUME_DISK_EXTENTS*>(extBuffer);
                    if (ex->NumberOfDiskExtents == 1)
                    {
                        VolumeInfo vi;
                        vi.volumeName = volumeName;
                        vi.diskNumber = ex->Extents[0].DiskNumber;
                        vi.offset = static_cast<uint64_t>(ex->Extents[0].StartingOffset.QuadPart);
                        vi.length = static_cast<uint64_t>(ex->Extents[0].ExtentLength.QuadPart);

                        wchar_t fs[64]{};
                        wchar_t label[128]{};
                        if (GetVolumeInformationW(volumeName, label, ARRAYSIZE(label),
                            nullptr, nullptr, nullptr, fs, ARRAYSIZE(fs)))
                        {
                            vi.fileSystem = fs;
                            vi.label = label;
                        }

                        DWORD chars = 0;
                        GetVolumePathNamesForVolumeNameW(volumeName, nullptr, 0, &chars);
                        if (chars > 1)
                        {
                            std::vector<wchar_t> paths(chars + 1);
                            if (GetVolumePathNamesForVolumeNameW(volumeName, paths.data(),
                                static_cast<DWORD>(paths.size()), &chars))
                            {
                                for (const wchar_t* p = paths.data(); *p; p += wcslen(p) + 1)
                                {
                                    if (wcslen(p) >= 3 && p[1] == L':' && p[2] == L'\\')
                                    {
                                        vi.driveLetter.assign(p, p + 2);
                                        break;
                                    }
                                }
                            }
                        }
                        result.push_back(std::move(vi));
                    }
                }
                CloseHandle(hVol);
            }
        } while (FindNextVolumeW(find, volumeName, ARRAYSIZE(volumeName)));

        FindVolumeClose(find);
        return result;
    }

    std::wstring GptTypeName(const GUID& type)
    {
        if (GuidEq(type, DiskModel::GPT_EFI_SYSTEM)) return L"EFI System";
        if (GuidEq(type, DiskModel::GPT_MICROSOFT_RESERVED)) return L"MSR";
        if (GuidEq(type, DiskModel::GPT_WINDOWS_RECOVERY)) return L"Recovery";
        if (GuidEq(type, DiskModel::GPT_BASIC_DATA)) return L"Basic data";
        return L"GPT partition";
    }

    std::wstring MbrTypeName(BYTE type, bool boot)
    {
        switch (type)
        {
        case 0x07: return boot ? L"System" : L"Primary";
        case 0x0B:
        case 0x0C: return L"FAT32";
        case 0x05:
        case 0x0F: return L"Extended";
        case 0x27: return L"Recovery";
        case 0x00: return L"Unused";
        default: return boot ? L"Active partition" : L"MBR partition";
        }
    }

    uint64_t AlignUp(uint64_t value, uint64_t align)
    {
        return (value + align - 1) / align * align;
    }

    uint64_t AlignDown(uint64_t value, uint64_t align)
    {
        return value / align * align;
    }
}

namespace DiskModel
{
    const GUID GPT_EFI_SYSTEM =
        { 0xc12a7328, 0xf81f, 0x11d2,{0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b} };
    const GUID GPT_MICROSOFT_RESERVED =
        { 0xe3c9e316, 0x0b5c, 0x4db8,{0x81,0x7d,0xf9,0x2d,0xf0,0x02,0x15,0xae} };
    const GUID GPT_BASIC_DATA =
        { 0xebd0a0a2, 0xb9e5, 0x4433,{0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7} };
    const GUID GPT_WINDOWS_RECOVERY =
        { 0xde94bba4, 0x06d1, 0x4d40,{0xa1,0x6a,0xbf,0xd5,0x01,0x79,0xd6,0xac} };

    bool IsEfi(const Segment& s)
    {
        return s.kind == SegmentKind::Partition && s.style == PARTITION_STYLE_GPT && GuidEq(s.gptType, GPT_EFI_SYSTEM);
    }

    bool IsMsr(const Segment& s)
    {
        return s.kind == SegmentKind::Partition && s.style == PARTITION_STYLE_GPT && GuidEq(s.gptType, GPT_MICROSOFT_RESERVED);
    }

    bool IsRecovery(const Segment& s)
    {
        if (s.kind != SegmentKind::Partition) return false;
        if (s.style == PARTITION_STYLE_GPT) return GuidEq(s.gptType, GPT_WINDOWS_RECOVERY);
        return s.style == PARTITION_STYLE_MBR && s.mbrType == 0x27;
    }

    bool IsBasicData(const Segment& s)
    {
        if (s.kind != SegmentKind::Partition) return false;
        if (s.style == PARTITION_STYLE_GPT) return GuidEq(s.gptType, GPT_BASIC_DATA);
        if (s.style == PARTITION_STYLE_MBR) return s.mbrType == 0x07 || s.mbrType == 0x0B || s.mbrType == 0x0C;
        return false;
    }

    std::wstring PartitionStyleName(PARTITION_STYLE style)
    {
        switch (style)
        {
        case PARTITION_STYLE_GPT: return L"GPT";
        case PARTITION_STYLE_MBR: return L"MBR";
        default: return L"RAW";
        }
    }

    std::wstring FormatBytes(uint64_t bytes)
    {
        const double Ki = 1024.0;
        const double Mi = Ki * 1024.0;
        const double Gi = Mi * 1024.0;
        const double Ti = Gi * 1024.0;
        std::wostringstream ss;
        ss << std::fixed;
        if (bytes >= static_cast<uint64_t>(Ti)) ss << std::setprecision(2) << bytes / Ti << L" TB";
        else if (bytes >= static_cast<uint64_t>(Gi)) ss << std::setprecision(2) << bytes / Gi << L" GB";
        else if (bytes >= static_cast<uint64_t>(Mi)) ss << std::setprecision(0) << bytes / Mi << L" MB";
        else ss << std::setprecision(0) << bytes / Ki << L" KB";
        return ss.str();
    }

    std::vector<DiskInfo> EnumerateDisks()
    {
        auto volumes = EnumerateVolumes();
        std::vector<DiskInfo> disks;

        for (int diskNo = 0; diskNo < 32; ++diskNo)
        {
            wchar_t path[64]{};
            swprintf_s(path, L"\\\\.\\PhysicalDrive%d", diskNo);
            HANDLE h = CreateFileW(path, GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;

            DiskInfo disk;
            disk.number = diskNo;
            disk.model = GetDiskModel(h);
            if (!QueryGeometry(h, disk.size, disk.logicalSectorSize))
            {
                CloseHandle(h);
                continue;
            }

            std::vector<BYTE> layoutBuffer;
            DRIVE_LAYOUT_INFORMATION_EX* layout = nullptr;
            if (!QueryLayout(h, layoutBuffer, layout))
            {
                CloseHandle(h);
                continue;
            }

            disk.style = static_cast<PARTITION_STYLE>(layout->PartitionStyle);

            struct PartTemp
            {
                uint64_t offset;
                uint64_t length;
                int number;
                PARTITION_STYLE style;
                GUID gptType;
                BYTE mbrType;
                bool boot;
            };
            std::vector<PartTemp> parts;

            for (DWORD i = 0; i < layout->PartitionCount; ++i)
            {
                const auto& p = layout->PartitionEntry[i];
                if (p.PartitionLength.QuadPart <= 0 || p.PartitionNumber == 0) continue;

                PartTemp pt{};
                pt.offset = static_cast<uint64_t>(p.StartingOffset.QuadPart);
                pt.length = static_cast<uint64_t>(p.PartitionLength.QuadPart);
                pt.number = static_cast<int>(p.PartitionNumber);
                pt.style = static_cast<PARTITION_STYLE>(p.PartitionStyle);
                if (p.PartitionStyle == PARTITION_STYLE_GPT)
                    pt.gptType = p.Gpt.PartitionType;
                else if (p.PartitionStyle == PARTITION_STYLE_MBR)
                {
                    pt.mbrType = p.Mbr.PartitionType;
                    pt.boot = !!p.Mbr.BootIndicator;
                }
                parts.push_back(pt);
            }

            std::sort(parts.begin(), parts.end(), [](const PartTemp& a, const PartTemp& b)
                { return a.offset < b.offset; });

            uint64_t usableStart = MiB;
            uint64_t usableEnd = disk.size > MiB ? disk.size - MiB : disk.size;
            if (layout->PartitionStyle == PARTITION_STYLE_GPT)
            {
                if (layout->Gpt.StartingUsableOffset.QuadPart > 0)
                    usableStart = static_cast<uint64_t>(layout->Gpt.StartingUsableOffset.QuadPart);
                if (layout->Gpt.UsableLength.QuadPart > 0)
                    usableEnd = usableStart + static_cast<uint64_t>(layout->Gpt.UsableLength.QuadPart);
            }
            usableStart = AlignUp(usableStart, MiB);
            usableEnd = AlignDown(usableEnd, MiB);

            uint64_t cursor = usableStart;
            for (const auto& p : parts)
            {
                if (p.offset > cursor)
                {
                    uint64_t gapStart = AlignUp(cursor, MiB);
                    uint64_t gapEnd = AlignDown(p.offset, MiB);
                    if (gapEnd > gapStart && gapEnd - gapStart >= MiB)
                    {
                        Segment gap;
                        gap.kind = SegmentKind::FreeSpace;
                        gap.offset = gapStart;
                        gap.length = gapEnd - gapStart;
                        gap.style = disk.style;
                        gap.typeName = L"Unallocated";
                        disk.segments.push_back(std::move(gap));
                    }
                }

                Segment s;
                s.kind = SegmentKind::Partition;
                s.partitionNumber = p.number;
                s.offset = p.offset;
                s.length = p.length;
                s.style = p.style;
                s.gptType = p.gptType;
                s.mbrType = p.mbrType;
                s.mbrBootIndicator = p.boot;
                s.typeName = p.style == PARTITION_STYLE_GPT ? GptTypeName(p.gptType) : MbrTypeName(p.mbrType, p.boot);

                for (const auto& v : volumes)
                {
                    if (v.diskNumber == static_cast<DWORD>(diskNo) && v.offset == s.offset)
                    {
                        s.fileSystem = v.fileSystem;
                        s.label = v.label;
                        s.driveLetter = v.driveLetter;
                        break;
                    }
                }
                disk.segments.push_back(std::move(s));

                uint64_t end = p.offset + p.length;
                if (end > cursor) cursor = end;
            }

            if (usableEnd > cursor)
            {
                uint64_t gapStart = AlignUp(cursor, MiB);
                uint64_t gapEnd = AlignDown(usableEnd, MiB);
                if (gapEnd > gapStart && gapEnd - gapStart >= MiB)
                {
                    Segment gap;
                    gap.kind = SegmentKind::FreeSpace;
                    gap.offset = gapStart;
                    gap.length = gapEnd - gapStart;
                    gap.style = disk.style;
                    gap.typeName = L"Unallocated";
                    disk.segments.push_back(std::move(gap));
                }
            }

            std::sort(disk.segments.begin(), disk.segments.end(), [](const Segment& a, const Segment& b)
                { return a.offset < b.offset; });

            disks.push_back(std::move(disk));
            CloseHandle(h);
        }

        return disks;
    }

    bool SetVolumeLabelByExtent(int diskNumber, uint64_t partitionOffset,
        const std::wstring& label, std::wstring& error)
    {
        // A newly formatted volume can take a moment to appear in the volume
        // namespace, especially on removable media. Retry briefly.
        for (int attempt = 0; attempt < 12; ++attempt)
        {
            auto volumes = EnumerateVolumes();
            for (const auto& v : volumes)
            {
                uint64_t delta = v.offset > partitionOffset ? v.offset - partitionOffset : partitionOffset - v.offset;
                if (v.diskNumber == static_cast<DWORD>(diskNumber) && delta <= MiB)
                {
                    if (SetVolumeLabelW(v.volumeName.c_str(), label.c_str()))
                        return true;

                    DWORD err = GetLastError();
                    error = L"Failed to set the label: " + Win32Error(err) +
                        L" (Win32 " + std::to_wstring(err) + L")";
                    return false;
                }
            }
            Sleep(150);
        }

        error = L"Could not find a volume for the selected partition after the operation.";
        return false;
    }

}
