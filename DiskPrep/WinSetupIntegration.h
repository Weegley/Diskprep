#pragma once

#include <string>
#include <vector>

namespace WinSetupIntegration
{
    struct Target
    {
        int diskNumber = -1;
        int partitionNumber = -1;
    };

    struct Result
    {
        bool ok = false;
        bool usedExistingUnattend = false;
        bool sourcePointerWasMissing = false;
        bool sourceFileWasMissing = false;
        int removedDiskConfigurationEntries = 0;
        bool removedInstallToAvailablePartition = false;
        std::wstring sourcePath;
        std::wstring outputPath;
        std::wstring error;
        std::vector<std::wstring> diagnostics;
    };

    Result PrepareTarget(const Target& target);
}
