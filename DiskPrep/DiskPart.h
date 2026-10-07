#pragma once

#include <windows.h>
#include <string>
#include <functional>

namespace DiskPart
{
    struct Result
    {
        bool started = false;
        DWORD exitCode = MAXDWORD;
        std::wstring error;
        std::wstring output;
    };

    using OutputCallback = std::function<void(const std::wstring&)>;

    Result RunScript(const std::wstring& script, const OutputCallback& onOutput = {});
    std::wstring QuoteLabel(const std::wstring& label);
}
