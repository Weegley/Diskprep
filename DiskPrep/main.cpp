#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <process.h>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <sstream>
#include <string>
#include <vector>

#include "DiskModel.h"
#include "DiskPart.h"
#include "WinSetupIntegration.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
    constexpr uint64_t KiB = 1024ull;
    constexpr uint64_t MiB = 1024ull * 1024ull;
    constexpr uint64_t GiB = 1024ull * 1024ull * 1024ull;

    constexpr int MSR_SIZE_MB = 16;
    constexpr int RECOVERY_SIZE_MB = 1024;
    constexpr int MBR_SYSTEM_SIZE_MB = 550;

    constexpr int WINSETUP_MIN_SCREEN_WIDTH = 1024;
    constexpr int WINSETUP_MIN_SCREEN_HEIGHT = 768;
    constexpr int INITIAL_WINDOW_WIDTH = 800;
    constexpr int INITIAL_WINDOW_HEIGHT = 600;

    int MaxInt(int a, int b) { return a > b ? a : b; }
    int MinInt(int a, int b) { return a < b ? a : b; }

    uint64_t EfiSizeMb(const DiskInfo& disk)
    {
        // Match current Windows Setup guidance: 200 MB for 512/512e media,
        // 300 MB for native 4K (4Kn) media. DISK_GEOMETRY_EX reports the
        // logical sector size, so 512e follows the 200 MB path and 4Kn the 300 MB path.
        return disk.logicalSectorSize >= 4096 ? 300ull : 200ull;
    }

    enum ControlId
    {
        IDC_DISK_COMBO = 1001,
        IDC_REFRESH,
        IDC_MAP,
        IDC_LIST,
        IDC_SELECTION,

        IDC_WIN_SIZE,
        IDC_WIN_ALL,
        IDC_NO_RECOVERY,
        IDC_CREATE_WINDOWS,

        IDC_DATA_SIZE,
        IDC_DATA_ALL,
        IDC_DATA_LABEL,
        IDC_DATA_FS,
        IDC_CREATE_DATA,

        IDC_DELETE,
        IDC_FORMAT_FS,
        IDC_FORMAT_LABEL,
        IDC_FORMAT,
        IDC_SET_LABEL,

        IDC_EXTEND_MODE,
        IDC_EXTEND_SIZE,
        IDC_EXTEND_ALL,
        IDC_EXTEND,
        IDC_SHRINK_MODE,
        IDC_SHRINK_SIZE,
        IDC_SHRINK,

        IDC_CONVERT_GPT,
        IDC_WIPE,
        IDC_STATUS,
        IDC_OPEN_LOG,
        IDC_INSTALL_WINDOWS
    };

    constexpr UINT WM_APP_OPERATION_DONE = WM_APP + 10;
    constexpr UINT WM_APP_DISKPART_OUTPUT = WM_APP + 11;
    constexpr UINT WM_APP_FORCE_REDRAW = WM_APP + 12;

    struct AppState
    {
        HWND hwnd = nullptr;
        HWND diskCombo = nullptr;
        HWND refreshBtn = nullptr;
        HWND mapWnd = nullptr;
        HWND list = nullptr;
        HWND selectionText = nullptr;
        HWND winSize = nullptr;
        HWND winAll = nullptr;
        HWND noRecovery = nullptr;
        HWND createWindows = nullptr;
        HWND dataSize = nullptr;
        HWND dataAll = nullptr;
        HWND dataLabel = nullptr;
        HWND dataFs = nullptr;
        HWND createData = nullptr;
        HWND deleteBtn = nullptr;
        HWND formatFs = nullptr;
        HWND formatLabel = nullptr;
        HWND formatBtn = nullptr;
        HWND setLabelBtn = nullptr;
        HWND extendMode = nullptr;
        HWND extendSize = nullptr;
        HWND extendAll = nullptr;
        HWND extendBtn = nullptr;
        HWND shrinkMode = nullptr;
        HWND shrinkSize = nullptr;
        HWND shrinkBtn = nullptr;
        HWND convertGptBtn = nullptr;
        HWND wipeBtn = nullptr;
        HWND status = nullptr;
        HWND openLogBtn = nullptr;
        HWND installWindowsBtn = nullptr;
        HFONT font = nullptr;
        HANDLE logFile = INVALID_HANDLE_VALUE;
        std::wstring logPath;

        std::vector<DiskInfo> disks;
        int selectedDiskVectorIndex = -1;
        int selectedSegment = -1;
        std::vector<RECT> mapRects;
        bool busy = false;
        bool winSetupMode = false;
        bool acceptDiskPartStyleOnce = false;

        bool pendingShrink = false;
        int pendingShrinkDisk = -1;
        int pendingShrinkPartition = -1;
        uint64_t pendingShrinkCurrentMb = 0;
        uint64_t pendingShrinkRemoveMb = 0;
        uint64_t pendingShrinkTargetMb = 0;
    } g;

    enum class OperationContinuation
    {
        None,
        CreateData,
        CreateWindows,
        ShrinkQuery
    };

    struct AsyncRequest
    {
        std::wstring script;
        std::wstring action;
        bool setLabel = false;
        int labelDisk = -1;
        uint64_t labelOffset = 0;
        std::wstring label;
        DWORD settleMs = 20;
        OperationContinuation continuation = OperationContinuation::None;
    };

    struct AsyncResult
    {
        std::wstring action;
        bool diskPartAttempted = false;
        DiskPart::Result diskPart;
        bool labelAttempted = false;
        bool labelOk = true;
        std::wstring labelError;
        OperationContinuation continuation = OperationContinuation::None;
    };

    std::wstring ToWString(int v)
    {
        return std::to_wstring(v);
    }

    std::wstring GetText(HWND h)
    {
        int len = GetWindowTextLengthW(h);
        std::wstring s(static_cast<size_t>(len) + 1u, L'\0');
        if (len > 0) GetWindowTextW(h, s.data(), len + 1);
        s.resize(static_cast<size_t>(len));
        return s;
    }

    std::wstring GetComboText(HWND h)
    {
        int index = static_cast<int>(SendMessageW(h, CB_GETCURSEL, 0, 0));
        if (index == CB_ERR) return L"";
        int len = static_cast<int>(SendMessageW(h, CB_GETLBTEXTLEN, index, 0));
        if (len < 0) return L"";
        std::wstring text(static_cast<size_t>(len) + 1u, L'\0');
        SendMessageW(h, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text.data()));
        text.resize(static_cast<size_t>(len));
        return text;
    }

    bool IsWinSetupCommandLine(PWSTR commandLine)
    {
        if (!commandLine) return false;
        std::wstring value = commandLine;
        size_t first = 0;
        while (first < value.size() && iswspace(value[first])) ++first;
        size_t last = value.size();
        while (last > first && iswspace(value[last - 1])) --last;
        value = value.substr(first, last - first);
        if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
            value = value.substr(1, value.size() - 2);
        return _wcsicmp(value.c_str(), L"/winsetup") == 0;
    }

    void SetFont(HWND h)
    {
        if (h && g.font) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    }

    void SetStatus(const std::wstring& text)
    {
        SetWindowTextW(g.status, text.c_str());
    }

    int RunTaskDialog(const std::wstring& title, const std::wstring& instruction,
        const std::wstring& content, PCWSTR icon, const TASKDIALOG_BUTTON* buttons,
        UINT buttonCount, int defaultButton, const std::wstring& expanded = L"",
        TASKDIALOG_FLAGS extraFlags = static_cast<TASKDIALOG_FLAGS>(0))
    {
        TASKDIALOGCONFIG cfg{};
        cfg.cbSize = sizeof(cfg);
        cfg.hwndParent = g.hwnd;
        cfg.dwFlags = static_cast<TASKDIALOG_FLAGS>(
            TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION | extraFlags);
        cfg.pszWindowTitle = title.c_str();
        cfg.pszMainIcon = icon;
        cfg.pszMainInstruction = instruction.c_str();
        cfg.pszContent = content.empty() ? nullptr : content.c_str();
        cfg.cButtons = buttonCount;
        cfg.pButtons = buttons;
        cfg.nDefaultButton = defaultButton;
        cfg.pszExpandedInformation = expanded.empty() ? nullptr : expanded.c_str();

        int pressed = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr)))
            return IDCANCEL;
        return pressed;
    }

    void ShowTaskMessage(const std::wstring& title, const std::wstring& instruction,
        const std::wstring& content, PCWSTR icon, const std::wstring& expanded = L"")
    {
        const TASKDIALOG_BUTTON buttons[] =
        {
            { IDOK, L"OK" }
        };
        RunTaskDialog(title, instruction, content, icon, buttons,
            static_cast<UINT>(_countof(buttons)), IDOK, expanded);
    }

    bool ConfirmTask(const std::wstring& title, const std::wstring& instruction,
        const std::wstring& content, const wchar_t* confirmText = L"Continue",
        PCWSTR icon = TD_WARNING_ICON)
    {
        const TASKDIALOG_BUTTON buttons[] =
        {
            { IDYES, confirmText },
            { IDCANCEL, L"Cancel" }
        };
        return RunTaskDialog(title, instruction, content, icon, buttons,
            static_cast<UINT>(_countof(buttons)), IDCANCEL) == IDYES;
    }

    std::wstring LogTimestamp()
    {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t buf[32]{};
        swprintf_s(buf, L"[%02u:%02u:%02u.%03u] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        return buf;
    }

    bool WriteLogUtf8(const std::wstring& text)
    {
        if (g.logFile == INVALID_HANDLE_VALUE || text.empty()) return false;

        int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
            nullptr, 0, nullptr, nullptr);
        if (needed <= 0) return false;

        std::string bytes(static_cast<size_t>(needed), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
            bytes.data(), needed, nullptr, nullptr) != needed)
            return false;

        DWORD written = 0;
        return WriteFile(g.logFile, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE &&
            written == static_cast<DWORD>(bytes.size());
    }

    std::wstring EnvironmentValue(const wchar_t* name)
    {
        const DWORD need = GetEnvironmentVariableW(name, nullptr, 0);
        if (need == 0) return L"<not set>";
        std::vector<wchar_t> value(need, L'\0');
        const DWORD got = GetEnvironmentVariableW(name, value.data(), need);
        if (got == 0 || got >= need) return L"<unavailable>";
        return value.data();
    }

    std::wstring ExecutablePath()
    {
        std::vector<wchar_t> path(32768, L'\0');
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0 || n >= path.size()) return L"<unavailable>";
        return path.data();
    }

    std::wstring CurrentDirectoryText()
    {
        const DWORD need = GetCurrentDirectoryW(0, nullptr);
        if (need == 0) return L"<unavailable>";
        std::vector<wchar_t> path(need, L'\0');
        const DWORD got = GetCurrentDirectoryW(need, path.data());
        if (got == 0 || got >= need) return L"<unavailable>";
        return path.data();
    }

    std::wstring ArchitectureText(WORD architecture)
    {
        switch (architecture)
        {
        case PROCESSOR_ARCHITECTURE_AMD64: return L"x64";
        case PROCESSOR_ARCHITECTURE_INTEL: return L"x86";
#ifdef PROCESSOR_ARCHITECTURE_ARM64
        case PROCESSOR_ARCHITECTURE_ARM64: return L"ARM64";
#endif
        case PROCESSOR_ARCHITECTURE_ARM: return L"ARM";
        default: return L"unknown (" + std::to_wstring(architecture) + L")";
        }
    }

    std::wstring FirmwareText()
    {
        FIRMWARE_TYPE firmware = FirmwareTypeUnknown;
        if (!GetFirmwareType(&firmware)) return L"unknown";
        switch (firmware)
        {
        case FirmwareTypeUefi: return L"UEFI";
        case FirmwareTypeBios: return L"Legacy BIOS";
        default: return L"unknown";
        }
    }

    void InitializeSessionLog()
    {
        wchar_t tempPath[MAX_PATH]{};
        if (!GetTempPathW(ARRAYSIZE(tempPath), tempPath)) return;

        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t name[96]{};
        swprintf_s(name, L"DiskPrep_%04u%02u%02u_%02u%02u%02u_%lu.txt",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            static_cast<unsigned long>(GetCurrentProcessId()));

        g.logPath = tempPath;
        if (!g.logPath.empty() && g.logPath.back() != L'\\') g.logPath += L'\\';
        g.logPath += name;

        g.logFile = CreateFileW(g.logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g.logFile == INVALID_HANDLE_VALUE)
        {
            g.logPath.clear();
            return;
        }

        const BYTE bom[] = { 0xEF, 0xBB, 0xBF };
        DWORD written = 0;
        WriteFile(g.logFile, bom, sizeof(bom), &written, nullptr);
        WriteLogUtf8(L"DiskPrep v1.0 session log\r\n");
        WriteLogUtf8(std::wstring(L"Mode: ") + (g.winSetupMode ? L"/winsetup" : L"normal") + L"\r\n");
        WriteLogUtf8(L"Executable: " + ExecutablePath() + L"\r\n");
        WriteLogUtf8(L"Command line: " + std::wstring(GetCommandLineW() ? GetCommandLineW() : L"") + L"\r\n");
#ifdef _WIN64
        WriteLogUtf8(L"Process architecture: x64\r\n");
#else
        WriteLogUtf8(L"Process architecture: x86\r\n");
#endif
        SYSTEM_INFO nativeInfo{};
        GetNativeSystemInfo(&nativeInfo);
        WriteLogUtf8(L"Native architecture: " + ArchitectureText(nativeInfo.wProcessorArchitecture) + L"\r\n");
        WriteLogUtf8(L"Firmware: " + FirmwareText() + L"\r\n");
        WriteLogUtf8(L"SystemRoot: " + EnvironmentValue(L"SystemRoot") + L"\r\n");
        WriteLogUtf8(L"SystemDrive: " + EnvironmentValue(L"SystemDrive") + L"\r\n");
        WriteLogUtf8(L"Current directory: " + CurrentDirectoryText() + L"\r\n");
        WriteLogUtf8(L"Session log: " + g.logPath + L"\r\n\r\n");
        FlushFileBuffers(g.logFile);
    }

    void AppendLogRaw(const std::wstring& text)
    {
        WriteLogUtf8(text);
    }

    void AppendLogLine(const std::wstring& text)
    {
        AppendLogRaw(LogTimestamp() + text + L"\r\n");
    }

    std::wstring DisplayChangeResultText(LONG code)
    {
        switch (code)
        {
        case DISP_CHANGE_SUCCESSFUL: return L"SUCCESS";
        case DISP_CHANGE_BADDUALVIEW: return L"BADDUALVIEW";
        case DISP_CHANGE_BADFLAGS: return L"BADFLAGS";
        case DISP_CHANGE_BADMODE: return L"BADMODE";
        case DISP_CHANGE_BADPARAM: return L"BADPARAM";
        case DISP_CHANGE_FAILED: return L"FAILED";
        case DISP_CHANGE_NOTUPDATED: return L"NOTUPDATED";
        case DISP_CHANGE_RESTART: return L"RESTART";
        default: return L"code " + std::to_wstring(code);
        }
    }

    void EnsureWinSetupDisplayMode()
    {
        if (!g.winSetupMode) return;

        DEVMODEW current{};
        current.dmSize = sizeof(current);
        if (!EnumDisplaySettingsExW(nullptr, ENUM_CURRENT_SETTINGS, &current, 0))
        {
            AppendLogLine(L"Display: cannot query current display mode; keeping current mode.");
            return;
        }

        AppendLogLine(L"Display: current " + std::to_wstring(current.dmPelsWidth) + L"x" +
            std::to_wstring(current.dmPelsHeight) + L" " + std::to_wstring(current.dmBitsPerPel) + L"bpp");

        if (current.dmPelsWidth >= WINSETUP_MIN_SCREEN_WIDTH &&
            current.dmPelsHeight >= WINSETUP_MIN_SCREEN_HEIGHT)
        {
            AppendLogLine(L"Display: current mode already fits DiskPrep; no change needed.");
            return;
        }

        DEVMODEW best{};
        bool found = false;
        uint64_t bestPixels = 0;

        for (DWORD index = 0;; ++index)
        {
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (!EnumDisplaySettingsExW(nullptr, index, &mode, 0)) break;

            if (mode.dmPelsWidth < WINSETUP_MIN_SCREEN_WIDTH ||
                mode.dmPelsHeight < WINSETUP_MIN_SCREEN_HEIGHT ||
                mode.dmBitsPerPel != 32)
                continue;

            const uint64_t pixels = static_cast<uint64_t>(mode.dmPelsWidth) *
                static_cast<uint64_t>(mode.dmPelsHeight);

            if (!found || pixels < bestPixels ||
                (pixels == bestPixels && mode.dmPelsWidth < best.dmPelsWidth) ||
                (pixels == bestPixels && mode.dmPelsWidth == best.dmPelsWidth &&
                    mode.dmPelsHeight < best.dmPelsHeight))
            {
                best = mode;
                bestPixels = pixels;
                found = true;
            }
        }

        if (!found)
        {
            AppendLogLine(L"Display: no supported 32bpp mode at least " +
                std::to_wstring(WINSETUP_MIN_SCREEN_WIDTH) + L"x" +
                std::to_wstring(WINSETUP_MIN_SCREEN_HEIGHT) + L"; keeping current mode.");
            return;
        }

        AppendLogLine(L"Display: selected " + std::to_wstring(best.dmPelsWidth) + L"x" +
            std::to_wstring(best.dmPelsHeight) + L" " + std::to_wstring(best.dmBitsPerPel) + L"bpp");

        DEVMODEW request = best;
        request.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;

        LONG result = ChangeDisplaySettingsExW(nullptr, &request, nullptr, CDS_TEST, nullptr);
        AppendLogLine(L"Display: CDS_TEST = " + DisplayChangeResultText(result));
        if (result != DISP_CHANGE_SUCCESSFUL) return;

        result = ChangeDisplaySettingsExW(nullptr, &request, nullptr, 0, nullptr);
        AppendLogLine(L"Display: ChangeDisplaySettingsEx = " + DisplayChangeResultText(result));
        if (result != DISP_CHANGE_SUCCESSFUL) return;

        DEVMODEW applied{};
        applied.dmSize = sizeof(applied);
        if (EnumDisplaySettingsExW(nullptr, ENUM_CURRENT_SETTINGS, &applied, 0))
        {
            AppendLogLine(L"Display: active " + std::to_wstring(applied.dmPelsWidth) + L"x" +
                std::to_wstring(applied.dmPelsHeight) + L" " + std::to_wstring(applied.dmBitsPerPel) + L"bpp");
        }
    }

    void AppendTimestampedText(const std::wstring& text)
    {
        if (text.empty()) return;

        size_t pos = 0;
        while (pos < text.size())
        {
            size_t end = pos;
            while (end < text.size() && text[end] != L'\r' && text[end] != L'\n') ++end;

            const std::wstring line = text.substr(pos, end - pos);
            if (!line.empty())
                AppendLogLine(line);
            else
                AppendLogRaw(L"\r\n");

            if (end >= text.size()) break;
            if (text[end] == L'\r' && end + 1 < text.size() && text[end + 1] == L'\n')
                end += 2;
            else
                ++end;
            pos = end;
        }
    }

    void AppendDiskPartOutput(const std::wstring& text)
    {
        AppendTimestampedText(text);
    }

    void LogScript(const std::wstring& action, const std::wstring& script)
    {
        AppendLogLine(L"=== " + action + L" ===");
        if (script.empty())
        {
            AppendLogLine(L"(DiskPart is not used)");
            return;
        }

        size_t pos = 0;
        while (pos < script.size())
        {
            size_t end = pos;
            while (end < script.size() && script[end] != L'\r' && script[end] != L'\n') ++end;
            std::wstring line = script.substr(pos, end - pos);
            if (!line.empty()) AppendLogLine(L"> " + line);

            if (end >= script.size()) break;
            if (script[end] == L'\r' && end + 1 < script.size() && script[end + 1] == L'\n')
                end += 2;
            else
                ++end;
            pos = end;
        }
    }

    void OpenSessionLog()
    {
        if (g.logPath.empty())
        {
            ShowTaskMessage(L"DiskPrep", L"Session log unavailable",
                L"The session log could not be created.", TD_ERROR_ICON);
            return;
        }

        if (g.logFile != INVALID_HANDLE_VALUE) FlushFileBuffers(g.logFile);

        // Shell32 is not guaranteed to be present in a minimal WinPE image, so
        // resolve ShellExecuteW dynamically instead of adding a hard dependency.
        HMODULE shell32 = LoadLibraryW(L"shell32.dll");
        if (shell32)
        {
            using ShellExecuteWFn = HINSTANCE(WINAPI*)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
            auto shellExecute = reinterpret_cast<ShellExecuteWFn>(GetProcAddress(shell32, "ShellExecuteW"));
            if (shellExecute)
            {
                HINSTANCE result = shellExecute(g.hwnd, L"open", g.logPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                if (reinterpret_cast<INT_PTR>(result) > 32)
                {
                    FreeLibrary(shell32);
                    return;
                }
            }
            FreeLibrary(shell32);
        }

        std::wstring command = L"notepad.exe \"" + g.logPath + L"\"";
        std::vector<wchar_t> buffer(command.begin(), command.end());
        buffer.push_back(L'\0');
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return;
        }

        std::wstring message = L"Cannot open the log automatically.\n\nLog file:\n" + g.logPath;
        ShowTaskMessage(L"DiskPrep", L"Unable to open session log", message, TD_INFORMATION_ICON);
    }

    void Enable(HWND h, bool enable)
    {
        EnableWindow(h, enable ? TRUE : FALSE);
    }

    DiskInfo* CurrentDisk()
    {
        if (g.selectedDiskVectorIndex < 0 || g.selectedDiskVectorIndex >= static_cast<int>(g.disks.size())) return nullptr;
        return &g.disks[g.selectedDiskVectorIndex];
    }

    const Segment* SelectedSegment()
    {
        auto* d = CurrentDisk();
        if (!d || g.selectedSegment < 0 || g.selectedSegment >= static_cast<int>(d->segments.size())) return nullptr;
        return &d->segments[g.selectedSegment];
    }

    uint64_t AlignUp(uint64_t value, uint64_t align)
    {
        return (value + align - 1) / align * align;
    }

    uint64_t AlignDown(uint64_t value, uint64_t align)
    {
        return value / align * align;
    }

    bool ParseGb(HWND edit, uint64_t& mb)
    {
        std::wstring text = GetText(edit);
        if (text.empty()) return false;

        // Accept both decimal separators regardless of the current WinPE locale.
        std::replace(text.begin(), text.end(), L',', L'.');

        wchar_t* end = nullptr;
        double gb = wcstod(text.c_str(), &end);
        if (end == text.c_str() || !std::isfinite(gb) || gb <= 0.0) return false;
        while (*end != L'\0' && iswspace(*end)) ++end;
        if (*end != L'\0') return false;

        double value = gb * 1024.0;
        if (value < 1.0 || value > static_cast<double>(UINT32_MAX)) return false;
        mb = static_cast<uint64_t>(std::llround(value));
        return mb > 0;
    }

    std::wstring SpaceErrorText(uint64_t requestedMb, uint64_t availableMb, const std::wstring& prefix = L"Not enough space.")
    {
        return prefix + L"\n\nRequested: " + std::to_wstring(requestedMb) +
            L" MB\nAvailable: " + std::to_wstring(availableMb) + L" MB";
    }

    std::wstring FormatOffset(uint64_t bytes)
    {
        std::wostringstream ss;
        if (bytes >= GiB)
        {
            ss.setf(std::ios::fixed);
            ss.precision(2);
            ss << static_cast<double>(bytes) / static_cast<double>(GiB) << L" GB";
        }
        else
        {
            ss.setf(std::ios::fixed);
            ss.precision(0);
            ss << static_cast<double>(bytes) / static_cast<double>(MiB) << L" MB";
        }
        return ss.str();
    }

    bool HasEfi(const DiskInfo& d)
    {
        return std::any_of(d.segments.begin(), d.segments.end(), [](const Segment& s) { return DiskModel::IsEfi(s); });
    }

    bool HasMsr(const DiskInfo& d)
    {
        return std::any_of(d.segments.begin(), d.segments.end(), [](const Segment& s) { return DiskModel::IsMsr(s); });
    }

    bool HasRecovery(const DiskInfo& d)
    {
        return std::any_of(d.segments.begin(), d.segments.end(), [](const Segment& s) { return DiskModel::IsRecovery(s); });
    }

    bool HasMbrActiveSystem(const DiskInfo& d)
    {
        return std::any_of(d.segments.begin(), d.segments.end(), [](const Segment& s)
            {
                return s.kind == SegmentKind::Partition && s.style == PARTITION_STYLE_MBR &&
                    s.mbrBootIndicator && s.length <= 2ull * GiB;
            });
    }

    bool HasAnyPartition(const DiskInfo& d)
    {
        return std::any_of(d.segments.begin(), d.segments.end(), [](const Segment& s)
            { return s.kind == SegmentKind::Partition; });
    }

    const wchar_t* StyleName(PARTITION_STYLE style)
    {
        switch (style)
        {
        case PARTITION_STYLE_GPT: return L"GPT";
        case PARTITION_STYLE_MBR: return L"MBR";
        default: return L"RAW";
        }
    }

    bool ExpectedStyleForFirmware(PARTITION_STYLE& style, std::wstring& firmwareName)
    {
        FIRMWARE_TYPE firmware = FirmwareTypeUnknown;
        if (!GetFirmwareType(&firmware)) return false;

        if (firmware == FirmwareTypeUefi)
        {
            style = PARTITION_STYLE_GPT;
            firmwareName = L"UEFI";
            return true;
        }
        if (firmware == FirmwareTypeBios)
        {
            style = PARTITION_STYLE_MBR;
            firmwareName = L"Legacy BIOS";
            return true;
        }
        return false;
    }

    enum class StyleMismatchChoice
    {
        Cancel,
        Convert,
        Keep
    };

    StyleMismatchChoice PromptStyleMismatch(PARTITION_STYLE currentStyle, PARTITION_STYLE expectedStyle,
        const std::wstring& firmwareName, const std::wstring& operationSummary)
    {
        constexpr int ID_CONVERT_AND_CONTINUE = 2001;
        constexpr int ID_DO_NOT_CONVERT = 2002;

        std::wstring instruction = L"Disk is " + std::wstring(StyleName(currentStyle)) +
            L", while Windows is running in " + firmwareName + L" mode.";
        std::wstring content = L"The recommended partition style for this firmware mode is " +
            std::wstring(StyleName(expectedStyle)) + L".\n\nPlanned operation:\n" + operationSummary;

        std::wstring convertText = L"Convert to " + std::wstring(StyleName(expectedStyle)) + L" and continue";
        TASKDIALOG_BUTTON buttons[] =
        {
            { ID_CONVERT_AND_CONTINUE, convertText.c_str() },
            { ID_DO_NOT_CONVERT, L"Do not convert" },
            { IDCANCEL, L"Cancel" }
        };

        const int pressed = RunTaskDialog(L"Partition style mismatch", instruction, content,
            TD_WARNING_ICON, buttons, static_cast<UINT>(_countof(buttons)), IDCANCEL);

        if (pressed == ID_CONVERT_AND_CONTINUE) return StyleMismatchChoice::Convert;
        if (pressed == ID_DO_NOT_CONVERT) return StyleMismatchChoice::Keep;
        return StyleMismatchChoice::Cancel;
    }

    bool ConfirmInstallTargetStyle(const DiskInfo& disk, const std::wstring& installContent,
        bool& installAlreadyConfirmed)
    {
        installAlreadyConfirmed = false;

        PARTITION_STYLE expectedStyle = PARTITION_STYLE_RAW;
        std::wstring firmwareName;
        if (!ExpectedStyleForFirmware(expectedStyle, firmwareName))
        {
            AppendLogLine(L"Install target style check: firmware mode unknown; compatibility warning skipped.");
            return true;
        }

        if (disk.style == expectedStyle)
        {
            AppendLogLine(L"Install target style check: disk=" + std::wstring(StyleName(disk.style)) +
                L", firmware=" + firmwareName + L", recommended=" +
                std::wstring(StyleName(expectedStyle)) + L" — OK.");
            return true;
        }

        AppendLogLine(L"Install target style check: disk=" + std::wstring(StyleName(disk.style)) +
            L", firmware=" + firmwareName + L", recommended=" +
            std::wstring(StyleName(expectedStyle)) + L" — mismatch.");

        std::wstring content = L"Disk is " + std::wstring(StyleName(disk.style)) +
            L", while Windows Setup is running in " + firmwareName + L" mode.\n"
            L"The recommended partition style for this firmware mode is " +
            std::wstring(StyleName(expectedStyle)) +
            L".\n\nWindows Setup may refuse to install Windows to this disk.\n"
            L"DiskPrep will not convert the disk automatically.\n\n" + installContent;

        const bool installAnyway = ConfirmTask(L"Partition style mismatch",
            L"Install Windows on this partition anyway?", content, L"Install anyway", TD_WARNING_ICON);
        AppendLogLine(installAnyway
            ? L"Install target style warning: user chose to install anyway."
            : L"Install target style warning: user cancelled.");
        installAlreadyConfirmed = installAnyway;
        return installAnyway;
    }

    enum class RawStyleChoice
    {
        Cancel,
        Gpt,
        Mbr,
        LetDiskPartDecide
    };

    RawStyleChoice PromptRawStyle(PARTITION_STYLE expectedStyle, const std::wstring& firmwareName,
        bool firmwareKnown, const std::wstring& operationSummary)
    {
        constexpr int ID_GPT = 2101;
        constexpr int ID_MBR = 2102;
        constexpr int ID_DISKPART_DECIDE = 2103;

        const bool gptRecommended = firmwareKnown && expectedStyle == PARTITION_STYLE_GPT;
        const bool mbrRecommended = firmwareKnown && expectedStyle == PARTITION_STYLE_MBR;

        std::wstring gptText = L"Initialize as GPT";
        std::wstring mbrText = L"Initialize as MBR";
        if (gptRecommended) gptText += L" — recommended";
        else if (firmwareKnown) gptText += L" — not recommended";
        if (mbrRecommended) mbrText += L" — recommended";
        else if (firmwareKnown) mbrText += L" — not recommended";

        std::wstring instruction = L"The selected disk has no partition style (RAW).";
        std::wstring content;
        if (firmwareKnown)
        {
            content = L"Windows is running in " + firmwareName + L" mode. "
                L"The recommended partition style is " + std::wstring(StyleName(expectedStyle)) + L".\n\n";
        }
        else
        {
            content = L"DiskPrep could not determine the current firmware mode.\n\n";
        }
        content += L"Planned operation:\n" + operationSummary +
            L"\n\nChoose a partition style explicitly, let DiskPart choose its default style, or cancel.";

        TASKDIALOG_BUTTON buttons[] =
        {
            { ID_GPT, gptText.c_str() },
            { ID_MBR, mbrText.c_str() },
            { ID_DISKPART_DECIDE, L"Let DiskPart decide" },
            { IDCANCEL, L"Cancel" }
        };

        const int defaultButton = gptRecommended ? ID_GPT : (mbrRecommended ? ID_MBR : ID_DISKPART_DECIDE);
        const int pressed = RunTaskDialog(L"RAW disk", instruction, content, TD_INFORMATION_ICON,
            buttons, static_cast<UINT>(_countof(buttons)), defaultButton);

        if (pressed == ID_GPT) return RawStyleChoice::Gpt;
        if (pressed == ID_MBR) return RawStyleChoice::Mbr;
        if (pressed == ID_DISKPART_DECIDE) return RawStyleChoice::LetDiskPartDecide;
        return RawStyleChoice::Cancel;
    }

    struct CreationStylePlan
    {
        PARTITION_STYLE style = PARTITION_STYLE_RAW;
        bool convert = false;
        bool letDiskPartDecide = false;
        bool confirmed = false;
    };

    bool ResolveCreationStyle(const DiskInfo& disk, const std::wstring& operationSummary, CreationStylePlan& plan)
    {
        plan.style = disk.style;
        plan.convert = false;
        plan.letDiskPartDecide = false;
        plan.confirmed = false;

        // A RAW-disk "Let DiskPart decide" probe is itself the user's final
        // confirmation. After the probe, continue with the exact style DiskPart chose
        // without asking the user a second time.
        if (g.acceptDiskPartStyleOnce && disk.style != PARTITION_STYLE_RAW)
        {
            g.acceptDiskPartStyleOnce = false;
            plan.confirmed = true;
            return true;
        }

        // Never change partition style implicitly on a disk that already has partitions.
        if (HasAnyPartition(disk)) return true;

        PARTITION_STYLE expectedStyle = PARTITION_STYLE_RAW;
        std::wstring firmwareName;
        const bool firmwareKnown = ExpectedStyleForFirmware(expectedStyle, firmwareName);

        if (disk.style == PARTITION_STYLE_RAW)
        {
            const RawStyleChoice choice = PromptRawStyle(expectedStyle, firmwareName, firmwareKnown, operationSummary);
            if (choice == RawStyleChoice::Cancel) return false;

            plan.confirmed = true;
            if (choice == RawStyleChoice::LetDiskPartDecide)
            {
                plan.letDiskPartDecide = true;
                return true;
            }

            plan.style = choice == RawStyleChoice::Gpt ? PARTITION_STYLE_GPT : PARTITION_STYLE_MBR;
            plan.convert = true;
            return true;
        }

        if (firmwareKnown && disk.style != expectedStyle)
        {
            const StyleMismatchChoice choice = PromptStyleMismatch(
                disk.style, expectedStyle, firmwareName, operationSummary);
            if (choice == StyleMismatchChoice::Cancel) return false;

            plan.confirmed = true;
            if (choice == StyleMismatchChoice::Convert)
            {
                plan.style = expectedStyle;
                plan.convert = true;
            }
            // Keep: retain the current disk style and continue exactly as requested.
        }

        return true;
    }

    void AppendConvertCommand(std::wostringstream& sc, PARTITION_STYLE style)
    {
        if (style == PARTITION_STYLE_GPT) sc << L"convert gpt\r\n";
        else if (style == PARTITION_STYLE_MBR) sc << L"convert mbr\r\n";
    }

    bool CalculateWindowsAllSizeMbForStyle(PARTITION_STYLE targetStyle, uint64_t& windowsMb)
    {
        windowsMb = 0;
        auto* d = CurrentDisk();
        const auto* gap = SelectedSegment();
        if (!d || !gap || gap->kind != SegmentKind::FreeSpace) return false;

        bool noRecovery = SendMessageW(g.noRecovery, BM_GETCHECK, 0, 0) == BST_CHECKED;
        bool createEfi = targetStyle == PARTITION_STYLE_GPT && !HasEfi(*d);
        bool createMsr = targetStyle == PARTITION_STYLE_GPT && !HasMsr(*d);
        bool createMbrSystem = targetStyle == PARTITION_STYLE_MBR && !HasMbrActiveSystem(*d);
        bool createRecovery = !noRecovery && !HasRecovery(*d);

        uint64_t cursor = AlignUp(gap->offset, MiB);
        uint64_t end = AlignDown(gap->offset + gap->length, MiB);
        uint64_t availableMb = end > cursor ? (end - cursor) / MiB : 0;

        uint64_t overheadMb = 0;
        if (createEfi) overheadMb += EfiSizeMb(*d);
        if (createMsr) overheadMb += MSR_SIZE_MB;
        if (createMbrSystem) overheadMb += MBR_SYSTEM_SIZE_MB;
        if (createRecovery) overheadMb += RECOVERY_SIZE_MB;

        if (availableMb <= overheadMb) return false;
        windowsMb = availableMb - overheadMb;
        return windowsMb > 0;
    }

    bool CalculateWindowsAllSizeMb(uint64_t& windowsMb)
    {
        auto* d = CurrentDisk();
        if (!d) return false;

        PARTITION_STYLE previewStyle = d->style;
        if (previewStyle == PARTITION_STYLE_RAW)
        {
            PARTITION_STYLE expectedStyle = PARTITION_STYLE_GPT;
            std::wstring firmwareName;
            if (ExpectedStyleForFirmware(expectedStyle, firmwareName)) previewStyle = expectedStyle;
            else previewStyle = PARTITION_STYLE_GPT;
        }
        return CalculateWindowsAllSizeMbForStyle(previewStyle, windowsMb);
    }


    struct WindowsLayoutPlan
    {
        PARTITION_STYLE style = PARTITION_STYLE_RAW;
        bool feasible = false;
        bool createEfi = false;
        bool createMsr = false;
        bool createMbrSystem = false;
        bool createRecovery = false;
        uint64_t availableMb = 0;
        uint64_t overheadMb = 0;
        uint64_t minimumRequiredMb = 0;
        uint64_t windowsMb = 0;
        uint64_t usedMb = 0;
        uint64_t freeLeftMb = 0;
    };

    WindowsLayoutPlan BuildWindowsLayoutPlan(const DiskInfo& disk, const Segment& gap,
        PARTITION_STYLE style, uint64_t requestedWindowsMb, bool all, bool noRecovery)
    {
        WindowsLayoutPlan plan;
        plan.style = style;
        plan.createEfi = style == PARTITION_STYLE_GPT && !HasEfi(disk);
        plan.createMsr = style == PARTITION_STYLE_GPT && !HasMsr(disk);
        plan.createMbrSystem = style == PARTITION_STYLE_MBR && !HasMbrActiveSystem(disk);
        plan.createRecovery = !noRecovery && !HasRecovery(disk);

        const uint64_t cursor = AlignUp(gap.offset, MiB);
        const uint64_t end = AlignDown(gap.offset + gap.length, MiB);
        plan.availableMb = end > cursor ? (end - cursor) / MiB : 0;

        if (plan.createEfi) plan.overheadMb += EfiSizeMb(disk);
        if (plan.createMsr) plan.overheadMb += MSR_SIZE_MB;
        if (plan.createMbrSystem) plan.overheadMb += MBR_SYSTEM_SIZE_MB;
        if (plan.createRecovery) plan.overheadMb += RECOVERY_SIZE_MB;

        plan.minimumRequiredMb = plan.overheadMb + (all ? 1ull : requestedWindowsMb);
        if (plan.availableMb < plan.minimumRequiredMb) return plan;

        plan.windowsMb = all ? (plan.availableMb - plan.overheadMb) : requestedWindowsMb;
        if (plan.windowsMb == 0) return plan;

        plan.usedMb = plan.windowsMb + plan.overheadMb;
        plan.freeLeftMb = plan.availableMb - plan.usedMb;
        plan.feasible = true;
        return plan;
    }

    std::wstring WindowsLayoutCompactText(const DiskInfo& disk, const WindowsLayoutPlan& plan)
    {
        std::wostringstream out;
        bool first = true;
        auto add = [&](const std::wstring& text)
            {
                if (!first) out << L" + ";
                out << text;
                first = false;
            };

        if (plan.createEfi) add(L"EFI " + std::to_wstring(EfiSizeMb(disk)) + L" MB");
        if (plan.createMsr) add(L"MSR " + std::to_wstring(MSR_SIZE_MB) + L" MB");
        if (plan.createMbrSystem) add(L"System Reserved " + std::to_wstring(MBR_SYSTEM_SIZE_MB) + L" MB");
        add(L"Windows " + std::to_wstring(plan.windowsMb) + L" MB");
        if (plan.createRecovery) add(L"Recovery " + std::to_wstring(RECOVERY_SIZE_MB) + L" MB");
        else out << L"; Recovery not created";
        if (plan.freeLeftMb != 0) out << L"; free left " << plan.freeLeftMb << L" MB";
        return out.str();
    }

    std::wstring WindowsLayoutDetailedText(const DiskInfo& disk, const WindowsLayoutPlan& plan, bool all,
        bool noRecovery)
    {
        std::wostringstream summary;
        if (plan.createEfi) summary << L"+ EFI System   " << EfiSizeMb(disk) << L" MB\n";
        else if (plan.style == PARTITION_STYLE_GPT) summary << L"= existing EFI will be kept\n";
        if (plan.createMsr) summary << L"+ MSR          " << MSR_SIZE_MB << L" MB\n";
        else if (plan.style == PARTITION_STYLE_GPT) summary << L"= existing MSR will be kept\n";
        if (plan.createMbrSystem) summary << L"+ System       " << MBR_SYSTEM_SIZE_MB << L" MB\n";
        summary << L"+ Windows      " << plan.windowsMb << L" MB";
        if (all) summary << L" (all available)";
        summary << L"\n";
        if (plan.createRecovery) summary << L"+ Recovery     " << RECOVERY_SIZE_MB << L" MB\n";
        else if (!noRecovery && HasRecovery(disk)) summary << L"= existing Recovery will be kept\n";
        else summary << L"- Recovery will not be created\n";
        summary << L"\nFree space left in this range: " << plan.freeLeftMb << L" MB";
        return summary.str();
    }

    StyleMismatchChoice PromptWindowsStyleMismatch(const DiskInfo& disk,
        PARTITION_STYLE currentStyle, PARTITION_STYLE expectedStyle, const std::wstring& firmwareName,
        const WindowsLayoutPlan& currentPlan, const WindowsLayoutPlan& expectedPlan)
    {
        constexpr int ID_CONVERT_AND_CREATE = 2401;
        constexpr int ID_KEEP_AND_CREATE = 2402;

        std::wstring instruction = L"Disk is " + std::wstring(StyleName(currentStyle)) +
            L", while Windows is running in " + firmwareName + L" mode.";

        std::wostringstream content;
        content << L"The recommended partition style for this firmware mode is "
            << StyleName(expectedStyle) << L".\n"
            << L"Selected free range: " << currentPlan.availableMb << L" MB";
        if (!expectedPlan.feasible)
            content << L"\n\n" << StyleName(expectedStyle) << L" layout cannot fit. Minimum required: "
                << expectedPlan.minimumRequiredMb << L" MB.";
        if (!currentPlan.feasible)
            content << L"\n\n" << StyleName(currentStyle) << L" layout cannot fit. Minimum required: "
                << currentPlan.minimumRequiredMb << L" MB.";

        std::wstring convertText = L"Convert to " + std::wstring(StyleName(expectedStyle)) + L" and create";
        if (expectedPlan.feasible)
            convertText += L"\n" + WindowsLayoutCompactText(disk, expectedPlan);

        std::wstring keepText = L"Keep " + std::wstring(StyleName(currentStyle)) + L" and create";
        if (currentPlan.feasible)
            keepText += L"\n" + WindowsLayoutCompactText(disk, currentPlan);

        std::vector<TASKDIALOG_BUTTON> buttons;
        if (expectedPlan.feasible) buttons.push_back({ ID_CONVERT_AND_CREATE, convertText.c_str() });
        if (currentPlan.feasible) buttons.push_back({ ID_KEEP_AND_CREATE, keepText.c_str() });
        buttons.push_back({ IDCANCEL, L"Cancel" });

        const int defaultButton = expectedPlan.feasible ? ID_CONVERT_AND_CREATE :
            (currentPlan.feasible ? ID_KEEP_AND_CREATE : IDCANCEL);
        const int pressed = RunTaskDialog(L"Partition style mismatch", instruction, content.str(),
            TD_WARNING_ICON, buttons.data(), static_cast<UINT>(buttons.size()), defaultButton, L"",
            TDF_USE_COMMAND_LINKS);

        if (pressed == ID_CONVERT_AND_CREATE) return StyleMismatchChoice::Convert;
        if (pressed == ID_KEEP_AND_CREATE) return StyleMismatchChoice::Keep;
        return StyleMismatchChoice::Cancel;
    }

    void UpdateWindowsAllPreview()
    {
        if (!g.winAll || !g.winSize) return;
        bool all = SendMessageW(g.winAll, BM_GETCHECK, 0, 0) == BST_CHECKED;
        Enable(g.winSize, !all && !g.busy);
        if (!all) return;

        uint64_t windowsMb = 0;
        if (CalculateWindowsAllSizeMb(windowsMb))
        {
            std::wostringstream ss;
            double gb = static_cast<double>(windowsMb) / 1024.0;
            ss.setf(std::ios::fixed);
            ss.precision(gb == std::floor(gb) ? 0 : 2);
            ss << gb;
            SetWindowTextW(g.winSize, ss.str().c_str());
        }
        else
        {
            SetWindowTextW(g.winSize, L"");
        }
    }

    bool IsFormattablePartition(const Segment& s)
    {
        if (s.kind != SegmentKind::Partition) return false;
        if (DiskModel::IsMsr(s)) return false;
        if (s.style == PARTITION_STYLE_MBR && (s.mbrType == 0x05 || s.mbrType == 0x0F)) return false;
        return true;
    }

    bool IsGapImmediatelyAfter(int segmentIndex, uint64_t& available)
    {
        auto* d = CurrentDisk();
        if (!d || segmentIndex < 0 || segmentIndex + 1 >= static_cast<int>(d->segments.size())) return false;
        const size_t index = static_cast<size_t>(segmentIndex);
        const auto& p = d->segments[index];
        const auto& n = d->segments[index + 1u];
        if (p.kind != SegmentKind::Partition || n.kind != SegmentKind::FreeSpace) return false;
        uint64_t end = p.offset + p.length;
        if (n.offset > end + MiB || end > n.offset + MiB) return false;
        available = n.length;
        return true;
    }

    bool IsGapImmediatelyBefore(int segmentIndex, uint64_t& available)
    {
        auto* d = CurrentDisk();
        if (!d || segmentIndex <= 0 || segmentIndex >= static_cast<int>(d->segments.size())) return false;
        const size_t index = static_cast<size_t>(segmentIndex);
        const auto& p = d->segments[index];
        const auto& prev = d->segments[index - 1u];
        if (p.kind != SegmentKind::Partition || prev.kind != SegmentKind::FreeSpace) return false;
        uint64_t prevEnd = prev.offset + prev.length;
        if (p.offset > prevEnd + MiB || prevEnd > p.offset + MiB) return false;
        available = prev.length;
        return true;
    }

    void UpdateSelectionUI()
    {
        auto* d = CurrentDisk();
        const Segment* s = SelectedSegment();

        bool isPartition = s && s->kind == SegmentKind::Partition;
        bool isGap = s && s->kind == SegmentKind::FreeSpace;
        bool basicData = s && DiskModel::IsBasicData(*s);
        bool canFormat = s && IsFormattablePartition(*s);
        const bool isNtfs = isPartition && _wcsicmp(s->fileSystem.c_str(), L"NTFS") == 0;

        std::wstring info = L"Nothing selected";
        if (d && s)
        {
            std::wostringstream ss;
            if (isGap)
            {
                ss << L"Unallocated — " << DiskModel::FormatBytes(s->length)
                   << L", offset " << FormatOffset(s->offset);
            }
            else
            {
                ss << L"Partition " << s->partitionNumber << L" — " << s->typeName
                   << L" — " << DiskModel::FormatBytes(s->length);
                if (!s->fileSystem.empty()) ss << L" — " << s->fileSystem;
                if (!s->label.empty()) ss << L" — " << s->label;
                if (!s->driveLetter.empty()) ss << L" — " << s->driveLetter;
            }
            info = ss.str();
        }
        SetWindowTextW(g.selectionText, info.c_str());

        if (g.busy)
        {
            Enable(g.createWindows, false);
            Enable(g.winAll, false);
            Enable(g.winSize, false);
            Enable(g.noRecovery, false);
            Enable(g.createData, false);
            Enable(g.deleteBtn, false);
            Enable(g.formatBtn, false);
            Enable(g.formatFs, false);
            Enable(g.formatLabel, false);
            Enable(g.setLabelBtn, false);
            Enable(g.extendMode, false);
            Enable(g.extendSize, false);
            Enable(g.extendAll, false);
            Enable(g.extendBtn, false);
            Enable(g.shrinkMode, false);
            Enable(g.shrinkSize, false);
            Enable(g.shrinkBtn, false);
            if (g.installWindowsBtn) Enable(g.installWindowsBtn, false);
            InvalidateRect(g.mapWnd, nullptr, TRUE);
            return;
        }

        Enable(g.createWindows, isGap);
        Enable(g.winAll, isGap);
        Enable(g.noRecovery, isGap);
        bool winAll = SendMessageW(g.winAll, BM_GETCHECK, 0, 0) == BST_CHECKED;
        Enable(g.winSize, isGap && !winAll);
        Enable(g.createData, isGap);
        Enable(g.deleteBtn, isPartition);
        Enable(g.formatBtn, canFormat);
        Enable(g.formatFs, canFormat);
        Enable(g.formatLabel, canFormat);
        Enable(g.setLabelBtn, isPartition && !s->fileSystem.empty());
        if (g.installWindowsBtn) Enable(g.installWindowsBtn, isPartition && basicData && isNtfs);

        uint64_t extendAvail = 0;
        bool canExtend = isPartition && basicData && isNtfs && IsGapImmediatelyAfter(g.selectedSegment, extendAvail);
        bool canShrink = isPartition && basicData && isNtfs;
        const bool extendAll = SendMessageW(g.extendAll, BM_GETCHECK, 0, 0) == BST_CHECKED;
        Enable(g.extendAll, canExtend);
        Enable(g.extendMode, canExtend && !extendAll);
        Enable(g.extendSize, canExtend && !extendAll);
        Enable(g.extendBtn, canExtend);
        Enable(g.shrinkMode, canShrink);
        Enable(g.shrinkSize, canShrink);
        Enable(g.shrinkBtn, canShrink);
        UpdateWindowsAllPreview();

        if (canExtend)
        {
            std::wstring tip = L"NTFS resize available; free space on the right: " + DiskModel::FormatBytes(extendAvail);
            SetStatus(tip);
        }
        else if (canShrink)
        {
            SetStatus(L"NTFS shrink available; extend requires contiguous free space on the right.");
        }
        else if (isPartition && basicData && !isNtfs)
        {
            SetStatus(L"Resize unavailable: DiskPrep enables Extend/Shrink only for NTFS partitions.");
        }
        else if (d)
        {
            uint64_t leftAvail = 0;
            if (isPartition && basicData && IsGapImmediatelyBefore(g.selectedSegment, leftAvail))
            {
                SetStatus(L"Free space on the left: " + DiskModel::FormatBytes(leftAvail) +
                    L". DiskPart can only extend to the right.");
            }
            else
            {
                std::wstring st = L"Disk " + std::to_wstring(d->number) + L" — " + DiskModel::PartitionStyleName(d->style) +
                    L" — " + DiskModel::FormatBytes(d->size);
                SetStatus(st);
            }
        }

        InvalidateRect(g.mapWnd, nullptr, TRUE);
    }

    void SelectSegment(int index, bool selectList)
    {
        auto* d = CurrentDisk();
        if (!d || index < 0 || index >= static_cast<int>(d->segments.size())) index = -1;
        g.selectedSegment = index;
        const Segment* selected = SelectedSegment();
        SetWindowTextW(g.formatLabel, (selected && selected->kind == SegmentKind::Partition) ? selected->label.c_str() : L"");
        if (selected && selected->kind == SegmentKind::Partition && !selected->fileSystem.empty())
        {
            int count = static_cast<int>(SendMessageW(g.formatFs, CB_GETCOUNT, 0, 0));
            for (int i = 0; i < count; ++i)
            {
                wchar_t fs[32]{};
                SendMessageW(g.formatFs, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(fs));
                if (_wcsicmp(fs, selected->fileSystem.c_str()) == 0)
                {
                    SendMessageW(g.formatFs, CB_SETCURSEL, i, 0);
                    break;
                }
            }
        }

        if (selectList)
        {
            ListView_SetItemState(g.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            if (index >= 0)
            {
                for (int row = 0; row < ListView_GetItemCount(g.list); ++row)
                {
                    LVITEMW item{};
                    item.mask = LVIF_PARAM;
                    item.iItem = row;
                    if (ListView_GetItem(g.list, &item) && static_cast<int>(item.lParam) == index)
                    {
                        ListView_SetItemState(g.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                        ListView_EnsureVisible(g.list, row, FALSE);
                        break;
                    }
                }
            }
        }
        UpdateSelectionUI();
    }

    void AddListColumn(int index, int width, const wchar_t* text)
    {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<LPWSTR>(text);
        col.cx = width;
        col.iSubItem = index;
        ListView_InsertColumn(g.list, index, &col);
    }

    void SetSubItem(int row, int col, const std::wstring& text)
    {
        ListView_SetItemText(g.list, row, col, const_cast<LPWSTR>(text.c_str()));
    }

    void PopulateList()
    {
        ListView_DeleteAllItems(g.list);
        auto* d = CurrentDisk();
        if (!d) return;

        for (int i = 0; i < static_cast<int>(d->segments.size()); ++i)
        {
            const auto& s = d->segments[i];
            std::wstring number = s.kind == SegmentKind::Partition ? std::to_wstring(s.partitionNumber) : L"—";
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = i;
            item.pszText = const_cast<LPWSTR>(number.c_str());
            item.lParam = i;
            int row = ListView_InsertItem(g.list, &item);

            SetSubItem(row, 1, s.typeName);
            SetSubItem(row, 2, s.fileSystem.empty() ? L"—" : s.fileSystem);
            SetSubItem(row, 3, s.label.empty() ? L"—" : s.label);
            SetSubItem(row, 4, s.driveLetter.empty() ? L"—" : s.driveLetter);
            SetSubItem(row, 5, DiskModel::FormatBytes(s.length));
            SetSubItem(row, 6, FormatOffset(s.offset));
        }
        g.selectedSegment = -1;
        UpdateSelectionUI();
    }

    void UpdateConvertButton()
    {
        auto* d = CurrentDisk();
        if (!d)
        {
            SetWindowTextW(g.convertGptBtn, L"Convert GPT");
            Enable(g.convertGptBtn, false);
            return;
        }

        if (d->style == PARTITION_STYLE_GPT)
            SetWindowTextW(g.convertGptBtn, L"Convert MBR");
        else
            SetWindowTextW(g.convertGptBtn, L"Convert GPT");

        Enable(g.convertGptBtn, !g.busy);
    }

    void RefreshDisks(int preserveDiskNumber = -1)
    {
        SetStatus(L"Reading partition tables...");
        g.disks = DiskModel::EnumerateDisks();
        SendMessageW(g.diskCombo, CB_RESETCONTENT, 0, 0);

        int selectCombo = -1;
        for (int i = 0; i < static_cast<int>(g.disks.size()); ++i)
        {
            const auto& d = g.disks[i];
            std::wstring text = L"Disk " + std::to_wstring(d.number) + L" — " + d.model + L" — " +
                DiskModel::FormatBytes(d.size) + L" — " + DiskModel::PartitionStyleName(d.style);
            int idx = static_cast<int>(SendMessageW(g.diskCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())));
            SendMessageW(g.diskCombo, CB_SETITEMDATA, idx, i);
            if (d.number == preserveDiskNumber) selectCombo = idx;
        }

        if (selectCombo < 0 && !g.disks.empty()) selectCombo = 0;
        if (selectCombo >= 0)
        {
            SendMessageW(g.diskCombo, CB_SETCURSEL, selectCombo, 0);
            g.selectedDiskVectorIndex = static_cast<int>(SendMessageW(g.diskCombo, CB_GETITEMDATA, selectCombo, 0));
        }
        else
        {
            g.selectedDiskVectorIndex = -1;
        }
        PopulateList();
        InvalidateRect(g.mapWnd, nullptr, TRUE);
        Enable(g.wipeBtn, CurrentDisk() != nullptr);
        UpdateConvertButton();
    }

    void LogDiskInventory(const std::wstring& heading)
    {
        AppendLogLine(L"=== " + heading + L" ===");
        if (g.disks.empty())
        {
            AppendLogLine(L"No physical disks enumerated.");
            AppendLogRaw(L"\r\n");
            return;
        }

        for (const auto& d : g.disks)
        {
            AppendLogLine(L"Disk " + std::to_wstring(d.number) + L": model=\"" + d.model +
                L"\", size=" + DiskModel::FormatBytes(d.size) +
                L", style=" + DiskModel::PartitionStyleName(d.style) +
                L", logical-sector=" + std::to_wstring(d.logicalSectorSize) +
                L", segments=" + std::to_wstring(d.segments.size()));

            for (const auto& seg : d.segments)
            {
                if (seg.kind == SegmentKind::FreeSpace)
                {
                    AppendLogLine(L"  Free: offset=" + std::to_wstring(seg.offset) +
                        L", size=" + DiskModel::FormatBytes(seg.length));
                    continue;
                }

                AppendLogLine(L"  Partition " + std::to_wstring(seg.partitionNumber) +
                    L": offset=" + std::to_wstring(seg.offset) +
                    L", size=" + DiskModel::FormatBytes(seg.length) +
                    L", type=\"" + seg.typeName + L"\", fs=\"" +
                    (seg.fileSystem.empty() ? std::wstring(L"<none>") : seg.fileSystem) +
                    L"\", label=\"" + (seg.label.empty() ? std::wstring(L"<none>") : seg.label) +
                    L"\", drive=\"" + (seg.driveLetter.empty() ? std::wstring(L"<none>") : seg.driveLetter) + L"\"");
            }
        }
        AppendLogRaw(L"\r\n");
        if (g.logFile != INVALID_HANDLE_VALUE) FlushFileBuffers(g.logFile);
    }

    void RefreshCurrentDisk()
    {
        int diskNo = -1;
        if (auto* d = CurrentDisk()) diskNo = d->number;
        AppendLogLine(L"Manual refresh requested.");
        RefreshDisks(diskNo);
        LogDiskInventory(L"Disk inventory after manual refresh");
    }

    bool Confirm(HWND, const std::wstring& text, const wchar_t* title = L"Confirmation")
    {
        return ConfirmTask(title, title, text, L"Continue", TD_WARNING_ICON);
    }

    int FindSegmentContainingOffset(const DiskInfo& disk, uint64_t offset)
    {
        for (size_t i = 0; i < disk.segments.size(); ++i)
        {
            const auto& s = disk.segments[i];
            if (s.length == 0) continue;
            const uint64_t end = s.offset + s.length;
            if (offset >= s.offset && offset < end)
                return static_cast<int>(i);
        }
        return -1;
    }

    void SetBusy(bool busy)
    {
        g.busy = busy;
        Enable(g.diskCombo, !busy);
        Enable(g.refreshBtn, !busy);
        Enable(g.wipeBtn, !busy && CurrentDisk() != nullptr);
        UpdateConvertButton();
        UpdateSelectionUI();
    }

    unsigned __stdcall OperationThreadProc(void* param)
    {
        AsyncRequest* req = static_cast<AsyncRequest*>(param);
        AsyncResult* done = new AsyncResult();
        done->action = req->action;
        done->continuation = req->continuation;

        const bool runDiskPart = !req->script.empty();
        done->diskPartAttempted = runDiskPart;
        if (runDiskPart)
        {
            done->diskPart = DiskPart::RunScript(req->script, [](const std::wstring& chunk)
            {
                auto* copy = new std::wstring(chunk);
                if (!PostMessageW(g.hwnd, WM_APP_DISKPART_OUTPUT, 0, reinterpret_cast<LPARAM>(copy)))
                    delete copy;
            });
        }

        const bool baseOk = !runDiskPart ||
            (done->diskPart.started && done->diskPart.exitCode == 0);

        if (baseOk && req->setLabel)
        {
            done->labelAttempted = true;
            done->labelOk = DiskModel::SetVolumeLabelByExtent(
                req->labelDisk, req->labelOffset, req->label, done->labelError);
        }

        if (req->settleMs) Sleep(req->settleMs);
        delete req;

        if (!PostMessageW(g.hwnd, WM_APP_OPERATION_DONE, 0, reinterpret_cast<LPARAM>(done)))
            delete done;
        return 0;
    }

    bool StartOperation(const std::wstring& script, const std::wstring& action,
        bool setLabel = false, int labelDisk = -1, uint64_t labelOffset = 0,
        const std::wstring& label = L"", DWORD settleMs = 20,
        OperationContinuation continuation = OperationContinuation::None)
    {
        if (g.busy) return false;

        AsyncRequest* req = new AsyncRequest();
        req->script = script;
        req->action = action;
        req->setLabel = setLabel;
        req->labelDisk = labelDisk;
        req->labelOffset = labelOffset;
        req->label = label;
        req->settleMs = settleMs;
        req->continuation = continuation;

        LogScript(action, script);
        SetStatus(action + L"...");
        SetBusy(true);

        uintptr_t threadValue = _beginthreadex(nullptr, 0, OperationThreadProc, req, 0, nullptr);
        if (!threadValue)
        {
            delete req;
            SetBusy(false);
            ShowTaskMessage(L"DiskPrep", L"Failed to start operation",
                L"The background worker thread could not be started.", TD_ERROR_ICON);
            return false;
        }
        CloseHandle(reinterpret_cast<HANDLE>(threadValue));
        return true;
    }


    bool ExtractLastSizeBeforeUnit(const std::wstring& text, const std::wstring& unitChars, uint64_t& value)
    {
        if (text.empty()) return false;
        for (size_t i = text.size(); i-- > 0; )
        {
            if (unitChars.find(text[i]) == std::wstring::npos) continue;

            size_t end = i;
            while (end > 0 && iswspace(text[end - 1])) --end;
            size_t begin = end;
            while (begin > 0 && iswdigit(text[begin - 1])) --begin;
            if (begin == end) continue;

            try
            {
                value = std::stoull(text.substr(begin, end - begin));
                return true;
            }
            catch (...)
            {
                return false;
            }
        }
        return false;
    }

    bool ParseShrinkQueryMaxMb(const std::wstring& output, uint64_t& maxMb)
    {
        uint64_t value = 0;

        // DiskPart commonly prints an exact MB value, sometimes in parentheses
        // after a rounded GB value. Handle both English and the common localized
        // unit initials used by Windows (for example Russian М/Г/К).
        if (ExtractLastSizeBeforeUnit(output, L"MmМм", value))
        {
            maxMb = value;
            return true;
        }
        if (ExtractLastSizeBeforeUnit(output, L"GgГг", value))
        {
            maxMb = value * 1024ull;
            return true;
        }
        if (ExtractLastSizeBeforeUnit(output, L"KkКк", value))
        {
            maxMb = (value + 1023ull) / 1024ull;
            return true;
        }
        return false;
    }

    void ClearPendingShrink()
    {
        g.pendingShrink = false;
        g.pendingShrinkDisk = -1;
        g.pendingShrinkPartition = -1;
        g.pendingShrinkCurrentMb = 0;
        g.pendingShrinkRemoveMb = 0;
        g.pendingShrinkTargetMb = 0;
    }

    void ContinueShrinkAfterQuery(const std::wstring& output)
    {
        if (!g.pendingShrink) return;

        const int diskNumber = g.pendingShrinkDisk;
        const int partitionNumber = g.pendingShrinkPartition;
        const uint64_t currentMb = g.pendingShrinkCurrentMb;
        const uint64_t removeMb = g.pendingShrinkRemoveMb;
        const uint64_t targetMb = g.pendingShrinkTargetMb;
        ClearPendingShrink();

        uint64_t maxShrinkMb = 0;
        if (!ParseShrinkQueryMaxMb(output, maxShrinkMb))
        {
            ShowTaskMessage(L"DiskPrep", L"Cannot determine shrink limit",
                L"DiskPart completed shrink querymax, but DiskPrep could not parse the reported maximum.\n\n"
                L"No shrink operation was started. See Open log for the full DiskPart output.",
                TD_ERROR_ICON);
            return;
        }

        const uint64_t minimumFinalMb = currentMb > maxShrinkMb ? currentMb - maxShrinkMb : 0;
        if (removeMb > maxShrinkMb)
        {
            std::wostringstream text;
            text << L"The requested shrink exceeds DiskPart's current limit.\n\n"
                 << L"Current size: " << currentMb << L" MB\n"
                 << L"Requested shrink: " << removeMb << L" MB\n"
                 << L"Requested final size: " << targetMb << L" MB\n"
                 << L"Maximum shrink now: " << maxShrinkMb << L" MB\n"
                 << L"Minimum final size now: " << minimumFinalMb << L" MB";
            ShowTaskMessage(L"DiskPrep", L"Operation cannot continue", text.str(), TD_ERROR_ICON);
            return;
        }

        std::wostringstream content;
        content << L"Partition " << partitionNumber << L" — NTFS\n\n"
                << L"Current size: " << currentMb << L" MB\n"
                << L"Reduce by: " << removeMb << L" MB\n"
                << L"Final size: " << targetMb << L" MB\n"
                << L"Maximum shrink now: " << maxShrinkMb << L" MB\n"
                << L"Minimum final size now: " << minimumFinalMb << L" MB\n\n"
                << L"The maximum can change if the volume is modified before DiskPart completes the operation.";
        if (!ConfirmTask(L"Resize partition", L"Shrink partition", content.str(), L"Shrink", TD_WARNING_ICON))
            return;

        std::wostringstream sc;
        sc << L"select disk " << diskNumber << L"\r\n"
           << L"select partition " << partitionNumber << L"\r\n"
           << L"shrink desired=" << removeMb << L"\r\n";
        StartOperation(sc.str(), L"Shrink partition");
    }

    void FinishOperation(AsyncResult* done)
    {
        if (!done) return;

        // Preserve the selected physical disk and the currently highlighted
        // physical location. The user may change the selection while an
        // operation is running; after refresh we select whatever now occupies
        // the same place (partition -> formatted partition, deleted partition
        // -> unallocated range, etc.).
        int diskNo = -1;
        uint64_t selectionAnchor = UINT64_MAX;
        if (auto* d = CurrentDisk())
        {
            diskNo = d->number;
            if (const Segment* selected = SelectedSegment())
                selectionAnchor = selected->offset + selected->length / 2u;
        }

        g.busy = false;
        RefreshDisks(diskNo);

        if (selectionAnchor != UINT64_MAX)
        {
            if (auto* refreshed = CurrentDisk())
            {
                const int restored = FindSegmentContainingOffset(*refreshed, selectionAnchor);
                if (restored >= 0)
                    SelectSegment(restored, true);
            }
        }
        SetBusy(false);
        if (done->diskPartAttempted)
        {
            if (done->diskPart.output.empty())
                AppendLogLine(L"(DiskPart returned no text output)");
        }

        bool diskPartOk = !done->diskPartAttempted ||
            (done->diskPart.started && done->diskPart.exitCode == 0);

        if (done->diskPartAttempted)
            AppendLogLine(L"DiskPart exit code: " + std::to_wstring(done->diskPart.exitCode));
        if (done->labelAttempted)
            AppendLogLine(done->labelOk ? L"Label: OK" : (L"Label: ERROR — " + done->labelError));
        AppendLogLine(done->action + (diskPartOk && (!done->labelAttempted || done->labelOk) ? L" — done" : L" — failed"));
        AppendLogRaw(L"\r\n");
        if (g.logFile != INVALID_HANDLE_VALUE) FlushFileBuffers(g.logFile);
        if (!diskPartOk)
        {
            std::wstring content;
            if (!done->diskPart.error.empty()) content += done->diskPart.error + L"\n\n";
            content += L"DiskPart exit code: " + std::to_wstring(done->diskPart.exitCode);
            std::wstring expanded;
            if (!done->diskPart.output.empty()) expanded = L"DiskPart output:\n" + done->diskPart.output;
            ShowTaskMessage(L"DiskPrep", done->action + L" failed", content, TD_ERROR_ICON, expanded);
            SetStatus(done->action + L" — failed");
        }
        else if (done->labelAttempted && !done->labelOk)
        {
            ShowTaskMessage(L"DiskPrep", done->action + L" completed with a warning",
                L"The partition operation completed, but the label could not be set.\n\n" + done->labelError,
                TD_WARNING_ICON);
            SetStatus(done->action + L" — done, label not set");
        }
        else
        {
            SetStatus(done->action + L" — done");
        }

        const bool operationOk = diskPartOk && (!done->labelAttempted || done->labelOk);
        const OperationContinuation continuation = done->continuation;
        const std::wstring continuationOutput = done->diskPart.output;
        delete done;

        if (continuation == OperationContinuation::ShrinkQuery)
        {
            if (operationOk)
                ContinueShrinkAfterQuery(continuationOutput);
            else
                ClearPendingShrink();
            return;
        }

        if (operationOk && continuation != OperationContinuation::None)
        {
            // The temporary 1 MB partition used for a RAW-disk probe has already
            // been deleted. Refresh above now exposes the partition style chosen by
            // DiskPart. Continue the user's original operation with that exact style.
            g.acceptDiskPartStyleOnce = true;
            if (continuation == OperationContinuation::CreateData)
                PostMessageW(g.hwnd, WM_COMMAND, MAKEWPARAM(IDC_CREATE_DATA, BN_CLICKED), reinterpret_cast<LPARAM>(g.createData));
            else if (continuation == OperationContinuation::CreateWindows)
                PostMessageW(g.hwnd, WM_COMMAND, MAKEWPARAM(IDC_CREATE_WINDOWS, BN_CLICKED), reinterpret_cast<LPARAM>(g.createWindows));
        }
    }

    std::wstring SanitizeLabel(const std::wstring& in)
    {
        // Labels are applied with SetVolumeLabelW, not embedded in a DiskPart script.
        // Validation below already enforces the filesystem-specific limits, so keep
        // the user's text intact (notably '"', which NTFS accepts).
        return in;
    }

    bool IsFatFileSystem(const std::wstring& fs)
    {
        return _wcsicmp(fs.c_str(), L"FAT") == 0 ||
            _wcsicmp(fs.c_str(), L"FAT32") == 0;
    }

    bool IsExFatFileSystem(const std::wstring& fs)
    {
        return _wcsicmp(fs.c_str(), L"exFAT") == 0;
    }

    bool ValidateVolumeLabel(const std::wstring& label, const std::wstring& fs, std::wstring& error)
    {
        error.clear();
        if (label.empty()) return true;

        const bool fat = IsFatFileSystem(fs);
        const bool exFat = IsExFatFileSystem(fs);
        const size_t maxChars = (fat || exFat) ? 11u : 32u;
        if (label.size() > maxChars)
        {
            error = L"Volume label is too long for " + fs + L".\n\nMaximum: " +
                std::to_wstring(maxChars) + L" characters\nEntered: " + std::to_wstring(label.size()) + L" characters";
            return false;
        }

        // Tested with the same SetVolumeLabelW path used by DiskPrep:
        // FAT/FAT32 reject the larger legacy set below; exFAT accepts the
        // FAT-only characters (+ , . ; [ ] =) but rejects the common set.
        // Unicode is intentionally not pre-filtered: Windows performs the
        // filesystem/code-page conversion and remains the runtime authority.
        const wchar_t* invalid = nullptr;
        if (fat)
            invalid = L"\"*+,./:;<=>?[\\]|";
        else if (exFat)
            invalid = L"\"*/:<>?\\|";

        if (invalid != nullptr)
        {
            std::vector<wchar_t> invalidChars;
            std::vector<wchar_t> invalidControls;

            for (wchar_t c : label)
            {
                if (c < 0x20)
                {
                    if (std::find(invalidControls.begin(), invalidControls.end(), c) == invalidControls.end())
                        invalidControls.push_back(c);
                }
                else if (wcschr(invalid, c) != nullptr)
                {
                    if (std::find(invalidChars.begin(), invalidChars.end(), c) == invalidChars.end())
                        invalidChars.push_back(c);
                }
            }

            const size_t invalidCount = invalidControls.size() + invalidChars.size();
            if (invalidCount != 0)
            {
                if (invalidChars.empty())
                {
                    error = invalidControls.size() == 1
                        ? L"Invalid control character in volume label: "
                        : L"Invalid control characters in volume label: ";
                }
                else
                {
                    error = invalidCount == 1
                        ? L"Invalid character in volume label: "
                        : L"Invalid characters in volume label: ";
                }

                bool first = true;
                for (wchar_t c : invalidControls)
                {
                    if (!first) error += L" ";
                    wchar_t code[16]{};
                    swprintf_s(code, L"U+%04X", static_cast<unsigned>(c));
                    error += code;
                    first = false;
                }

                for (wchar_t c : invalidChars)
                {
                    if (!first) error += L" ";
                    error.push_back(c);
                    first = false;
                }

                return false;
            }
        }

        return true;
    }

    void DeleteSelected()
    {
        auto* d = CurrentDisk();
        const auto* s = SelectedSegment();
        if (!d || !s || s->kind != SegmentKind::Partition) return;

        std::wstring msg = L"Delete Partition " + std::to_wstring(s->partitionNumber) + L" (" + s->typeName + L", " +
            DiskModel::FormatBytes(s->length) + L")?\n\nAll data on this partition will be lost.";
        if (!Confirm(g.hwnd, msg, L"Delete partition")) return;

        std::wostringstream sc;
        sc << L"select disk " << d->number << L"\r\n"
           << L"select partition " << s->partitionNumber << L"\r\n"
           << L"delete partition override\r\n";
        StartOperation(sc.str(), L"Delete partition");
    }

    void FormatSelected()
    {
        auto* d = CurrentDisk();
        const auto* s = SelectedSegment();
        if (!d || !s || !IsFormattablePartition(*s)) return;

        std::wstring fs = GetComboText(g.formatFs);
        if (fs.empty()) fs = L"NTFS";
        const std::wstring rawLabel = GetText(g.formatLabel);
        std::wstring labelError;
        if (!ValidateVolumeLabel(rawLabel, fs, labelError))
        {
            ShowTaskMessage(L"DiskPrep", L"Invalid volume label", labelError, TD_ERROR_ICON);
            return;
        }
        std::wstring label = SanitizeLabel(rawLabel);

        std::wstring msg = L"Quick-format Partition " + std::to_wstring(s->partitionNumber) + L" as " + fs + L"?\n\n" +
            DiskModel::FormatBytes(s->length) + L" — all data will be lost.";
        if (!label.empty()) msg += L"\nLabel: " + label;
        if (!Confirm(g.hwnd, msg, L"Format partition")) return;

        int diskNo = d->number;
        uint64_t offset = s->offset;
        std::wostringstream sc;
        sc << L"select disk " << diskNo << L"\r\n"
           << L"select partition " << s->partitionNumber << L"\r\n"
           << L"format quick fs=" << fs << L"\r\n";
        StartOperation(sc.str(), L"Format partition", !label.empty(), diskNo, offset, label);
    }

    void SetSelectedLabel()
    {
        auto* d = CurrentDisk();
        const auto* s = SelectedSegment();
        if (!d || !s || s->kind != SegmentKind::Partition || s->fileSystem.empty()) return;

        const std::wstring rawLabel = GetText(g.formatLabel);
        std::wstring labelError;
        if (!ValidateVolumeLabel(rawLabel, s->fileSystem, labelError))
        {
            ShowTaskMessage(L"DiskPrep", L"Invalid volume label", labelError, TD_ERROR_ICON);
            return;
        }
        std::wstring label = SanitizeLabel(rawLabel);
        std::wstring prompt = label.empty()
            ? L"Clear the label of the selected partition?"
            : L"Set the label of the selected partition:\n\n" + label;
        if (!Confirm(g.hwnd, prompt, L"Label")) return;

        StartOperation(L"", L"Change label", true, d->number, s->offset, label, 0);
    }

    void InstallWindowsHere()
    {
        if (!g.winSetupMode) return;
        auto* d = CurrentDisk();
        const auto* s = SelectedSegment();
        if (!d || !s || s->kind != SegmentKind::Partition ||
            !DiskModel::IsBasicData(*s) || _wcsicmp(s->fileSystem.c_str(), L"NTFS") != 0)
            return;

        std::wostringstream content;
        content << L"Disk " << d->number << L", Partition " << s->partitionNumber << L"\n"
            << L"Type: " << s->typeName << L"\n"
            << L"File system: " << s->fileSystem << L"\n"
            << L"Size: " << DiskModel::FormatBytes(s->length);
        if (!s->label.empty()) content << L"\nLabel: " << s->label;
        if (!s->driveLetter.empty()) content << L"\nDrive letter: " << s->driveLetter;
        content << L"\n\nDiskPrep will make this partition the Windows Setup target. "
            L"An existing unattended file referenced by Windows Setup, if present, will not be modified in place.";

        bool installAlreadyConfirmed = false;
        if (!ConfirmInstallTargetStyle(*d, content.str(), installAlreadyConfirmed)) return;

        if (!installAlreadyConfirmed &&
            !ConfirmTask(L"DiskPrep", L"Install Windows on this partition?",
                content.str(), L"Install Windows here", TD_INFORMATION_ICON))
            return;

        AppendLogLine(L"=== Prepare Windows Setup target ===");
        AppendLogLine(L"Target: disk " + std::to_wstring(d->number) +
            L", partition " + std::to_wstring(s->partitionNumber));
        AppendLogLine(L"Target details: type=\"" + s->typeName + L"\", fs=\"" + s->fileSystem +
            L"\", size=" + DiskModel::FormatBytes(s->length) +
            L", label=\"" + (s->label.empty() ? std::wstring(L"<none>") : s->label) +
            L"\", drive=\"" + (s->driveLetter.empty() ? std::wstring(L"<none>") : s->driveLetter) + L"\"");

        WinSetupIntegration::Target target{};
        target.diskNumber = d->number;
        target.partitionNumber = s->partitionNumber;
        WinSetupIntegration::Result result = WinSetupIntegration::PrepareTarget(target);
        for (const std::wstring& diagnostic : result.diagnostics)
            AppendLogLine(diagnostic);
        if (!result.ok)
        {
            AppendLogLine(L"Windows Setup target preparation failed: " + result.error);
            AppendLogRaw(L"\r\n");
            ShowTaskMessage(L"DiskPrep", L"Cannot prepare Windows Setup", result.error, TD_ERROR_ICON);
            return;
        }

        if (result.usedExistingUnattend)
            AppendLogLine(L"Source unattended file: " + result.sourcePath);
        else if (result.sourceFileWasMissing && !result.sourcePath.empty())
            AppendLogLine(L"Existing UnattendFile points to a missing file; a minimal answer file was created instead.");
        else
            AppendLogLine(L"No existing unattended file; created a minimal answer file.");
        if (result.removedDiskConfigurationEntries > 0)
            AppendLogLine(L"Removed active DiskConfiguration/Disk entries: " +
                std::to_wstring(result.removedDiskConfigurationEntries));
        if (result.removedInstallToAvailablePartition)
            AppendLogLine(L"Removed InstallToAvailablePartition because explicit InstallTo is now used.");
        AppendLogLine(L"Prepared unattended file: " + result.outputPath);
        AppendLogLine(L"HKLM\\SYSTEM\\Setup\\UnattendFile updated successfully.");
        AppendLogRaw(L"\r\n");
        if (g.logFile != INVALID_HANDLE_VALUE) FlushFileBuffers(g.logFile);

        PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
    }

    bool StartRawDiskPartProbe(int diskNumber, OperationContinuation continuation)
    {
        std::wostringstream sc;
        sc << L"select disk " << diskNumber << L"\r\n"
           << L"create partition primary size=1\r\n"
           << L"delete partition override\r\n"
           << L"rescan\r\n";

        return StartOperation(sc.str(), L"Let DiskPart choose partition style",
            false, -1, 0, L"", 50, continuation);
    }

    void CreateDataPartition()
    {
        auto* d = CurrentDisk();
        const auto* gap = SelectedSegment();
        if (!d || !gap || gap->kind != SegmentKind::FreeSpace) return;

        const bool all = SendMessageW(g.dataAll, BM_GETCHECK, 0, 0) == BST_CHECKED;
        uint64_t requestedMb = 0;
        if (!all && !ParseGb(g.dataSize, requestedMb))
        {
            ShowTaskMessage(L"DiskPrep", L"Invalid partition size",
                L"Enter a valid partition size in GB.", TD_INFORMATION_ICON);
            return;
        }

        std::wstring fs = GetComboText(g.dataFs);
        if (fs.empty()) fs = L"NTFS";

        const std::wstring rawLabel = GetText(g.dataLabel);
        std::wstring labelError;
        if (!ValidateVolumeLabel(rawLabel, fs, labelError))
        {
            ShowTaskMessage(L"DiskPrep", L"Invalid volume label", labelError, TD_ERROR_ICON);
            return;
        }
        const std::wstring label = SanitizeLabel(rawLabel);

        const uint64_t dataOffset = AlignUp(gap->offset, MiB);
        const uint64_t end = gap->offset + gap->length;
        const uint64_t available = end > dataOffset ? AlignDown(end - dataOffset, MiB) : 0;
        const uint64_t requestedBytes = all ? available : requestedMb * MiB;
        const uint64_t availableMb = available / MiB;
        const uint64_t requestedForMessageMb = requestedBytes / MiB;

        // Validate everything that is independent of partition style before asking
        // the user about GPT/MBR. An impossible request should produce one message,
        // not a style question followed by a size error.
        if (requestedBytes < MiB || requestedBytes > available)
        {
            std::wstring msg = SpaceErrorText(requestedForMessageMb, availableMb,
                L"Not enough space in the selected free range.");
            ShowTaskMessage(L"DiskPrep", L"Not enough free space", msg, TD_ERROR_ICON);
            return;
        }

        std::wostringstream requestSummary;
        requestSummary << L"Disk " << d->number << L", selected free range: "
            << (AlignDown(gap->length, MiB) / MiB) << L" MB\n"
            << L"DATA partition: " << requestedForMessageMb << L" MB\n"
            << L"File system: " << fs;
        if (!label.empty()) requestSummary << L"\nLabel: " << label;

        CreationStylePlan stylePlan;
        if (!ResolveCreationStyle(*d, requestSummary.str(), stylePlan)) return;
        if (stylePlan.letDiskPartDecide)
        {
            StartRawDiskPartProbe(d->number, OperationContinuation::CreateData);
            return;
        }

        std::wostringstream summary;
        summary << requestSummary.str();
        if (stylePlan.convert)
            summary << L"\nPartition style: " << StyleName(d->style) << L" -> " << StyleName(stylePlan.style);

        if (!stylePlan.confirmed &&
            !ConfirmTask(L"Create partition", L"Create DATA partition?", summary.str(),
                L"Create", TD_INFORMATION_ICON))
            return;

        std::wostringstream sc;
        sc << L"select disk " << d->number << L"\r\n";
        if (stylePlan.convert) AppendConvertCommand(sc, stylePlan.style);
        sc << L"create partition primary size=" << requestedBytes / MiB << L" offset=" << dataOffset / KiB << L"\r\n"
           << L"format quick fs=" << fs << L"\r\n"
           << L"assign\r\n";
        StartOperation(sc.str(), L"Create data partition", !label.empty(),
            d->number, dataOffset, label, 20);
    }

    bool FindWindowsImmediatelyBeforeSelectedGap(int& windowsSegmentIndex, uint64_t& freeBytes)
    {
        windowsSegmentIndex = -1;
        freeBytes = 0;

        auto* d = CurrentDisk();
        const auto* gap = SelectedSegment();
        if (!d || !gap || gap->kind != SegmentKind::FreeSpace || g.selectedSegment <= 0) return false;

        const size_t gapIndex = static_cast<size_t>(g.selectedSegment);
        const auto& prev = d->segments[gapIndex - 1u];
        if (prev.kind != SegmentKind::Partition || !DiskModel::IsBasicData(prev)) return false;
        if (_wcsicmp(prev.fileSystem.c_str(), L"NTFS") != 0) return false;
        if (_wcsicmp(prev.label.c_str(), L"Windows") != 0) return false;

        const uint64_t prevEnd = prev.offset + prev.length;
        if (gap->offset > prevEnd + MiB || prevEnd > gap->offset + MiB) return false;

        windowsSegmentIndex = static_cast<int>(gapIndex - 1u);
        freeBytes = gap->length;
        return true;
    }

    int PromptExistingWindowsChoice(const Segment& windows, uint64_t freeBytes)
    {
        constexpr int ID_EXTEND_EXISTING = 2201;
        constexpr int ID_CREATE_NEW = 2202;

        std::wostringstream content;
        content << L"Windows: " << DiskModel::FormatBytes(windows.length) << L"\n"
            << L"Free space immediately to the right: " << DiskModel::FormatBytes(freeBytes) << L"\n\n"
            << L"Choose whether to extend the existing Windows partition or create another one.";

        const TASKDIALOG_BUTTON buttons[] =
        {
            { ID_EXTEND_EXISTING, L"Extend existing Windows" },
            { ID_CREATE_NEW, L"Create new Windows partition" },
            { IDCANCEL, L"Cancel" }
        };

        const int pressed = RunTaskDialog(L"Existing Windows partition",
            L"An NTFS partition labeled 'Windows' is immediately to the left of the selected free space.",
            content.str(), TD_INFORMATION_ICON, buttons, static_cast<UINT>(_countof(buttons)),
            ID_EXTEND_EXISTING);

        if (pressed == ID_EXTEND_EXISTING) return IDYES;
        if (pressed == ID_CREATE_NEW) return IDNO;
        return IDCANCEL;
    }

    void CreateWindowsLayout()
    {
        auto* d = CurrentDisk();
        const auto* gap = SelectedSegment();
        if (!d || !gap || gap->kind != SegmentKind::FreeSpace) return;

        int adjacentWindowsIndex = -1;
        uint64_t adjacentFreeBytes = 0;
        if (FindWindowsImmediatelyBeforeSelectedGap(adjacentWindowsIndex, adjacentFreeBytes))
        {
            const auto& existingWindows = d->segments[static_cast<size_t>(adjacentWindowsIndex)];
            const int choice = PromptExistingWindowsChoice(existingWindows, adjacentFreeBytes);
            if (choice == IDCANCEL) return;
            if (choice == IDYES)
            {
                std::wostringstream sc;
                sc << L"select disk " << d->number << L"\r\n"
                   << L"select partition " << existingWindows.partitionNumber << L"\r\n"
                   << L"extend\r\n";
                StartOperation(sc.str(), L"Extend existing Windows");
                return;
            }
            // Create-new: continue with the normal Windows-layout flow.
        }

        const bool all = SendMessageW(g.winAll, BM_GETCHECK, 0, 0) == BST_CHECKED;
        uint64_t requestedWindowsMb = 0;
        if (!all && !ParseGb(g.winSize, requestedWindowsMb))
        {
            ShowTaskMessage(L"DiskPrep", L"Invalid Windows size",
                L"Enter a valid Windows partition size in GB.", TD_INFORMATION_ICON);
            return;
        }

        const bool noRecovery = SendMessageW(g.noRecovery, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool diskHadAnyPartitions = HasAnyPartition(*d);

        CreationStylePlan stylePlan;
        bool styleAlreadyResolved = false;

        // A preceding RAW "Let DiskPart decide" probe is already the user's final
        // style choice. Accept the style DiskPart just established without asking
        // another mismatch question on the continuation pass.
        if (g.acceptDiskPartStyleOnce && d->style != PARTITION_STYLE_RAW)
        {
            g.acceptDiskPartStyleOnce = false;
            stylePlan.style = d->style;
            stylePlan.convert = false;
            stylePlan.letDiskPartDecide = false;
            stylePlan.confirmed = true;
            styleAlreadyResolved = true;
        }

        // On an empty initialized disk with a firmware/style mismatch, the layout
        // itself changes with the user's choice. Validate both layouts first, then
        // present one command-link TaskDialog that shows exactly what each choice
        // will create. There is no second confirmation after this dialog.
        if (!styleAlreadyResolved && !diskHadAnyPartitions && d->style != PARTITION_STYLE_RAW)
        {
            PARTITION_STYLE expectedStyle = PARTITION_STYLE_RAW;
            std::wstring firmwareName;
            if (ExpectedStyleForFirmware(expectedStyle, firmwareName) && d->style != expectedStyle)
            {
                const WindowsLayoutPlan currentPlan = BuildWindowsLayoutPlan(
                    *d, *gap, d->style, requestedWindowsMb, all, noRecovery);
                const WindowsLayoutPlan expectedPlan = BuildWindowsLayoutPlan(
                    *d, *gap, expectedStyle, requestedWindowsMb, all, noRecovery);

                if (!currentPlan.feasible && !expectedPlan.feasible)
                {
                    std::wostringstream msg;
                    msg << L"The requested Windows layout does not fit in the selected free range.\n\n"
                        << L"Available: " << currentPlan.availableMb << L" MB\n"
                        << StyleName(d->style) << L" minimum required: " << currentPlan.minimumRequiredMb << L" MB\n"
                        << StyleName(expectedStyle) << L" minimum required: " << expectedPlan.minimumRequiredMb << L" MB";
                    ShowTaskMessage(L"DiskPrep", L"Not enough free space", msg.str(), TD_ERROR_ICON);
                    return;
                }

                const StyleMismatchChoice choice = PromptWindowsStyleMismatch(
                    *d, d->style, expectedStyle, firmwareName, currentPlan, expectedPlan);
                if (choice == StyleMismatchChoice::Cancel) return;

                stylePlan.style = choice == StyleMismatchChoice::Convert ? expectedStyle : d->style;
                stylePlan.convert = choice == StyleMismatchChoice::Convert;
                stylePlan.letDiskPartDecide = false;
                stylePlan.confirmed = true;
                styleAlreadyResolved = true;
            }
        }

        if (!styleAlreadyResolved)
        {
            // If there is no style choice to make, reject an impossible request
            // before displaying any confirmation dialog.
            if (d->style != PARTITION_STYLE_RAW)
            {
                const WindowsLayoutPlan preview = BuildWindowsLayoutPlan(
                    *d, *gap, d->style, requestedWindowsMb, all, noRecovery);
                if (!preview.feasible)
                {
                    std::wstring msg = SpaceErrorText(preview.minimumRequiredMb, preview.availableMb,
                        L"Not enough space in the selected Unallocated range for the requested Windows layout.");
                    ShowTaskMessage(L"DiskPrep", L"Not enough free space", msg, TD_ERROR_ICON);
                    return;
                }
            }
            else
            {
                // RAW is unusual in the tested Windows/VMware environments. If the
                // request cannot fit under either possible layout, fail once before
                // asking how to initialize the disk.
                const WindowsLayoutPlan gptPlan = BuildWindowsLayoutPlan(
                    *d, *gap, PARTITION_STYLE_GPT, requestedWindowsMb, all, noRecovery);
                const WindowsLayoutPlan mbrPlan = BuildWindowsLayoutPlan(
                    *d, *gap, PARTITION_STYLE_MBR, requestedWindowsMb, all, noRecovery);
                if (!gptPlan.feasible && !mbrPlan.feasible)
                {
                    std::wostringstream msg;
                    msg << L"The requested Windows layout does not fit in the selected free range.\n\n"
                        << L"Available: " << gptPlan.availableMb << L" MB\n"
                        << L"GPT minimum required: " << gptPlan.minimumRequiredMb << L" MB\n"
                        << L"MBR minimum required: " << mbrPlan.minimumRequiredMb << L" MB";
                    ShowTaskMessage(L"DiskPrep", L"Not enough free space", msg.str(), TD_ERROR_ICON);
                    return;
                }
            }

            std::wostringstream requestSummary;
            requestSummary << L"Disk " << d->number << L", selected free range: "
                << (AlignDown(gap->length, MiB) / MiB) << L" MB\n";
            if (all) requestSummary << L"Windows size: all available\n";
            else requestSummary << L"Windows size: " << requestedWindowsMb << L" MB\n";
            if (noRecovery) requestSummary << L"Recovery: do not create";
            else requestSummary << L"Recovery: " << RECOVERY_SIZE_MB << L" MB";
            requestSummary << L"\n\nGPT system layout: EFI " << EfiSizeMb(*d) << L" MB + MSR "
                << MSR_SIZE_MB << L" MB\nMBR system layout: System Reserved " << MBR_SYSTEM_SIZE_MB << L" MB";

            if (!ResolveCreationStyle(*d, requestSummary.str(), stylePlan)) return;
            if (stylePlan.letDiskPartDecide)
            {
                StartRawDiskPartProbe(d->number, OperationContinuation::CreateWindows);
                return;
            }
        }

        const PARTITION_STYLE targetStyle = stylePlan.style;
        const WindowsLayoutPlan plan = BuildWindowsLayoutPlan(
            *d, *gap, targetStyle, requestedWindowsMb, all, noRecovery);
        if (!plan.feasible)
        {
            std::wstring msg = SpaceErrorText(plan.minimumRequiredMb, plan.availableMb,
                L"Not enough space in the selected Unallocated range for the requested Windows layout.");
            ShowTaskMessage(L"DiskPrep", L"Not enough free space", msg, TD_ERROR_ICON);
            return;
        }

        std::wostringstream summary;
        summary << L"Disk " << d->number << L", selected free range: "
            << (AlignDown(gap->length, MiB) / MiB) << L" MB\n\n";
        if (stylePlan.convert)
            summary << StyleName(d->style) << L" -> " << StyleName(targetStyle) << L"\n";
        summary << WindowsLayoutDetailedText(*d, plan, all, noRecovery);

        if (!stylePlan.confirmed &&
            !ConfirmTask(L"Create Windows partitions", L"Create Windows partitions?",
                summary.str(), L"Create", TD_INFORMATION_ICON))
            return;

        std::wostringstream sc;
        sc << L"select disk " << d->number << L"\r\n";
        if (stylePlan.convert) AppendConvertCommand(sc, targetStyle);

        uint64_t pos = AlignUp(gap->offset, MiB);
        auto appendAt = [&](const std::wstring& command, uint64_t sizeMb)
            {
                sc << command << L" size=" << sizeMb << L" offset=" << pos / KiB << L"\r\n";
                pos = AlignUp(pos + sizeMb * MiB, MiB);
            };

        if (targetStyle == PARTITION_STYLE_GPT)
        {
            if (plan.createEfi)
            {
                appendAt(L"create partition primary", EfiSizeMb(*d));
                if (!diskHadAnyPartitions)
                {
                    sc << L"rescan\r\n"
                       << L"select disk " << d->number << L"\r\n"
                       << L"select partition 1\r\n";
                }
                sc << L"format quick fs=fat32 label=\"System\"\r\n"
                   << L"set id=\"c12a7328-f81f-11d2-ba4b-00a0c93ec93b\" override\r\n"
                   << L"gpt attributes=0x8000000000000000\r\n";
            }
            if (plan.createMsr) appendAt(L"create partition msr", MSR_SIZE_MB);
        }
        else if (targetStyle == PARTITION_STYLE_MBR && plan.createMbrSystem)
        {
            appendAt(L"create partition primary", MBR_SYSTEM_SIZE_MB);
            sc << L"format quick fs=ntfs label=\"System Reserved\"\r\nactive\r\n";
        }

        appendAt(L"create partition primary", plan.windowsMb);
        sc << L"format quick fs=ntfs label=\"Windows\"\r\n"
           << L"assign\r\n";

        if (plan.createRecovery)
        {
            appendAt(L"create partition primary", RECOVERY_SIZE_MB);
            sc << L"format quick fs=ntfs label=\"Recovery\"\r\n";
            if (targetStyle == PARTITION_STYLE_GPT)
            {
                sc << L"set id=\"de94bba4-06d1-4d40-a16a-bfd50179d6ac\" override\r\n"
                   << L"gpt attributes=0x8000000000000001\r\n";
            }
            else
            {
                sc << L"set id=27 override\r\n";
            }
        }

        StartOperation(sc.str(), L"Create Windows partitions");
    }

    bool IsResizeModeTo(HWND combo)
    {
        return static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0)) == 1;
    }

    uint64_t PartitionSizeMb(const Segment& s)
    {
        return AlignDown(s.length, MiB) / MiB;
    }

    void ExtendSelected()
    {
        auto* d = CurrentDisk();
        const auto* s = SelectedSegment();
        if (!d || !s || s->kind != SegmentKind::Partition || !DiskModel::IsBasicData(*s) ||
            _wcsicmp(s->fileSystem.c_str(), L"NTFS") != 0)
            return;

        uint64_t available = 0;
        if (!IsGapImmediatelyAfter(g.selectedSegment, available)) return;

        const uint64_t currentMb = PartitionSizeMb(*s);
        const uint64_t availableMb = AlignDown(available, MiB) / MiB;
        const bool all = SendMessageW(g.extendAll, BM_GETCHECK, 0, 0) == BST_CHECKED;

        uint64_t addMb = 0;
        uint64_t targetMb = currentMb;
        if (all)
        {
            addMb = availableMb;
            targetMb = currentMb + addMb;
        }
        else
        {
            uint64_t inputMb = 0;
            if (!ParseGb(g.extendSize, inputMb))
            {
                ShowTaskMessage(L"DiskPrep", L"Invalid extend size",
                    IsResizeModeTo(g.extendMode)
                        ? L"Enter the desired final partition size in GB."
                        : L"Enter how many GB to add to the partition.",
                    TD_INFORMATION_ICON);
                return;
            }

            const bool toMode = IsResizeModeTo(g.extendMode);
            addMb = inputMb;
            targetMb = currentMb + addMb;
            if (toMode)
            {
                targetMb = inputMb;
                if (targetMb <= currentMb)
                {
                    ShowTaskMessage(L"DiskPrep", L"Invalid target size",
                        L"Extend To must be larger than the current partition size.\n\n"
                        L"Current: " + std::to_wstring(currentMb) + L" MB",
                        TD_INFORMATION_ICON);
                    return;
                }
                addMb = targetMb - currentMb;
            }
        }

        if (addMb == 0 || addMb > availableMb)
        {
            std::wostringstream text;
            text << L"Not enough contiguous free space to extend the partition.\n\n"
                 << L"Current: " << currentMb << L" MB\n"
                 << L"Requested final size: " << targetMb << L" MB\n"
                 << L"Maximum final size: " << (currentMb + availableMb) << L" MB\n"
                 << L"Available on the right: " << availableMb << L" MB";
            ShowTaskMessage(L"DiskPrep", L"Operation cannot continue", text.str(), TD_ERROR_ICON);
            return;
        }

        std::wostringstream content;
        content << L"Partition " << s->partitionNumber << L" — NTFS\n\n"
                << L"Current size: " << currentMb << L" MB\n"
                << L"Increase by: " << addMb << L" MB\n"
                << L"Final size: " << targetMb << L" MB";
        if (all) content << L"\nMode: use all contiguous free space on the right";
        if (!ConfirmTask(L"Resize partition", L"Extend partition", content.str(), L"Extend", TD_INFORMATION_ICON))
            return;

        std::wostringstream sc;
        sc << L"select disk " << d->number << L"\r\n"
           << L"select partition " << s->partitionNumber << L"\r\n";
        if (all) sc << L"extend\r\n";
        else sc << L"extend size=" << addMb << L"\r\n";
        StartOperation(sc.str(), L"Extend partition");
    }

    void ShrinkSelected()
    {
        auto* d = CurrentDisk();
        const auto* s = SelectedSegment();
        if (!d || !s || s->kind != SegmentKind::Partition || !DiskModel::IsBasicData(*s) ||
            _wcsicmp(s->fileSystem.c_str(), L"NTFS") != 0)
            return;

        uint64_t inputMb = 0;
        if (!ParseGb(g.shrinkSize, inputMb))
        {
            ShowTaskMessage(L"DiskPrep", L"Invalid shrink size",
                IsResizeModeTo(g.shrinkMode)
                    ? L"Enter the desired final partition size in GB."
                    : L"Enter how many GB to remove from the partition.",
                TD_INFORMATION_ICON);
            return;
        }

        const uint64_t currentMb = PartitionSizeMb(*s);
        const bool toMode = IsResizeModeTo(g.shrinkMode);
        uint64_t removeMb = inputMb;
        uint64_t targetMb = currentMb > removeMb ? currentMb - removeMb : 0;

        if (toMode)
        {
            targetMb = inputMb;
            if (targetMb >= currentMb)
            {
                ShowTaskMessage(L"DiskPrep", L"Invalid target size",
                    L"Shrink To must be smaller than the current partition size.\n\n"
                    L"Current: " + std::to_wstring(currentMb) + L" MB",
                    TD_INFORMATION_ICON);
                return;
            }
            removeMb = currentMb - targetMb;
        }

        if (removeMb == 0 || removeMb >= currentMb || targetMb == 0)
        {
            ShowTaskMessage(L"DiskPrep", L"Invalid shrink size",
                L"The requested shrink would leave no usable partition space.\n\n"
                L"Current: " + std::to_wstring(currentMb) + L" MB",
                TD_INFORMATION_ICON);
            return;
        }

        // Ask DiskPart for its live shrink limit first. This does not alter the
        // partition. After the asynchronous query completes, DiskPrep validates the
        // requested By/To result and only then offers the final Shrink confirmation.
        g.pendingShrink = true;
        g.pendingShrinkDisk = d->number;
        g.pendingShrinkPartition = s->partitionNumber;
        g.pendingShrinkCurrentMb = currentMb;
        g.pendingShrinkRemoveMb = removeMb;
        g.pendingShrinkTargetMb = targetMb;

        std::wostringstream sc;
        sc << L"select disk " << d->number << L"\r\n"
           << L"select partition " << s->partitionNumber << L"\r\n"
           << L"shrink querymax\r\n";
        if (!StartOperation(sc.str(), L"Query shrink limit", false, -1, 0, L"", 0,
            OperationContinuation::ShrinkQuery))
        {
            ClearPendingShrink();
        }
    }

    void WipeDisk()
    {
        auto* d = CurrentDisk();
        if (!d) return;

        std::wstring first = L"COMPLETELY wipe Disk " + std::to_wstring(d->number) + L" (" + d->model + L")?\n\n"
            L"All partitions and data on this disk will be deleted.";
        if (!Confirm(g.hwnd, first, L"DANGEROUS OPERATION")) return;

        std::wstring finalText = L"Final confirmation. DiskPart CLEAN cannot be undone.\n\n"
            L"Wipe will clean and rescan the disk; it will not force GPT or MBR.\n"
            L"Partition style is checked when you create Windows or DATA partitions.\n\nContinue?";
        if (!Confirm(g.hwnd, finalText, L"Wipe disk")) return;

        std::wostringstream sc;
        sc << L"select disk " << d->number << L"\r\n"
           << L"clean\r\n"
           << L"rescan\r\n";
        StartOperation(sc.str(), L"Wipe disk", false, -1, 0, L"", 50);
    }

    void ConvertDiskStyle()
    {
        auto* d = CurrentDisk();
        if (!d) return;

        const bool toMbr = d->style == PARTITION_STYLE_GPT;
        const wchar_t* command = toMbr ? L"convert mbr" : L"convert gpt";
        const wchar_t* title = toMbr ? L"Convert MBR" : L"Convert GPT";

        if (HasAnyPartition(*d))
        {
            std::wstring text = L"DiskPart can " + std::wstring(command) +
                L" only on an empty disk.\n\nDelete all partitions or use Wipe, then try the conversion again.";
            ShowTaskMessage(title, L"Conversion is unavailable", text, TD_INFORMATION_ICON);
            return;
        }

        std::wstring msg = L"Convert Disk " + std::to_wstring(d->number) +
            (toMbr ? L" to MBR?" : L" to GPT?");
        if (!Confirm(g.hwnd, msg, title)) return;

        std::wostringstream sc;
        sc << L"select disk " << d->number << L"\r\n"
           << command << L"\r\n";
        StartOperation(sc.str(), title, false, -1, 0, L"", 50);
    }

    COLORREF SegmentColor(const Segment& s)
    {
        if (s.kind == SegmentKind::FreeSpace) return RGB(225, 225, 225);
        if (DiskModel::IsEfi(s)) return RGB(180, 220, 185);
        if (DiskModel::IsMsr(s)) return RGB(205, 205, 205);
        if (DiskModel::IsRecovery(s)) return RGB(230, 200, 150);
        return RGB(175, 205, 235);
    }

    void PaintMap(HWND hwnd)
    {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW) + 1));

        auto* d = CurrentDisk();
        g.mapRects.clear();
        if (!d || d->segments.empty())
        {
            SetBkMode(hdc, TRANSPARENT);
            DrawTextW(hdc, L"No partitions", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            EndPaint(hwnd, &ps);
            return;
        }

        const int margin = 8;
        RECT bar{ rc.left + margin, rc.top + 8, rc.right - margin, rc.bottom - 8 };
        int totalWidth = MaxInt(1, static_cast<int>(bar.right - bar.left));
        int n = static_cast<int>(d->segments.size());
        int minWidth = 24;
        if (n * minWidth > totalWidth) minWidth = MaxInt(4, totalWidth / n);
        int extra = MaxInt(0, totalWidth - n * minWidth);

        uint64_t totalLen = 0;
        for (const auto& s : d->segments) totalLen += s.length;
        if (!totalLen) totalLen = 1;

        int x = bar.left;
        for (int i = 0; i < n; ++i)
        {
            const auto& s = d->segments[i];
            int w = minWidth;
            if (extra > 0)
                w += static_cast<int>((static_cast<long double>(s.length) / totalLen) * extra);
            if (i == n - 1) w = bar.right - x;
            if (w < 1) w = 1;

            RECT sr{ x, bar.top, MinInt(static_cast<int>(bar.right), x + w), bar.bottom };
            g.mapRects.push_back(sr);
            HBRUSH br = CreateSolidBrush(SegmentColor(s));
            FillRect(hdc, &sr, br);
            DeleteObject(br);

            HPEN pen = CreatePen(PS_SOLID, i == g.selectedSegment ? 3 : 1,
                i == g.selectedSegment ? RGB(0, 90, 200) : RGB(90, 90, 90));
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, sr.left, sr.top, sr.right, sr.bottom);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);

            int sw = sr.right - sr.left;
            if (sw >= 42)
            {
                std::wstring label;
                if (s.kind == SegmentKind::FreeSpace) label = L"Free\n" + DiskModel::FormatBytes(s.length);
                else label = L"P" + std::to_wstring(s.partitionNumber) + L"\n" + DiskModel::FormatBytes(s.length);
                RECT tr = sr;
                InflateRect(&tr, -3, -3);
                SetBkMode(hdc, TRANSPARENT);
                DrawTextW(hdc, label.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
            }
            x = sr.right;
            if (x >= bar.right) break;
        }
        EndPaint(hwnd, &ps);
    }

    LRESULT CALLBACK MapProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_PAINT:
            PaintMap(hwnd);
            return 0;
        case WM_LBUTTONDOWN:
        {
            POINT p{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            for (int i = 0; i < static_cast<int>(g.mapRects.size()); ++i)
            {
                if (PtInRect(&g.mapRects[i], p))
                {
                    SelectSegment(i, true);
                    SetFocus(hwnd);
                    break;
                }
            }
            return 0;
        }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    HWND MakeControl(DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id)
    {
        HWND h = CreateWindowExW(ex, cls, text, style | WS_CHILD | WS_VISIBLE,
            0, 0, 10, 10, g.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SetFont(h);
        return h;
    }

    int ListHeightForRows(int rows)
    {
        if (!g.list || rows <= 0) return 145;

        int headerHeight = 0;
        if (HWND header = ListView_GetHeader(g.list))
        {
            RECT r{};
            if (GetWindowRect(header, &r)) headerHeight = r.bottom - r.top;
        }

        int rowHeight = 0;
        if (ListView_GetItemCount(g.list) > 0)
        {
            RECT r{};
            if (ListView_GetItemRect(g.list, 0, &r, LVIR_BOUNDS))
                rowHeight = r.bottom - r.top;
        }

        if (rowHeight <= 0)
        {
            HDC dc = GetDC(g.list);
            if (dc)
            {
                HFONT font = reinterpret_cast<HFONT>(SendMessageW(g.list, WM_GETFONT, 0, 0));
                HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
                TEXTMETRICW tm{};
                if (GetTextMetricsW(dc, &tm)) rowHeight = tm.tmHeight + 6;
                if (oldFont) SelectObject(dc, oldFont);
                ReleaseDC(g.list, dc);
            }
        }

        if (headerHeight <= 0) headerHeight = 24;
        if (rowHeight <= 0) rowHeight = 22;

        RECT wr{}, cr{};
        int nonClientHeight = 4;
        if (GetWindowRect(g.list, &wr) && GetClientRect(g.list, &cr))
            nonClientHeight = (wr.bottom - wr.top) - (cr.bottom - cr.top);

        return nonClientHeight + headerHeight + rows * rowHeight;
    }

    void LayoutControls(int cx, int cy)
    {
        const int m = 8;
        int y = m;

        const int wipeW = 86;
        const int gptW = 100;
        const int refreshW = 86;
        const int gap = 6;
        int wipeX = cx - m - wipeW;
        int gptX = wipeX - gap - gptW;
        int refreshX = gptX - gap - refreshW;
        MoveWindow(g.diskCombo, m, y, MaxInt(180, refreshX - m - gap), 26, TRUE);
        MoveWindow(g.refreshBtn, refreshX, y, refreshW, 26, TRUE);
        MoveWindow(g.convertGptBtn, gptX, y, gptW, 26, TRUE);
        MoveWindow(g.wipeBtn, wipeX, y, wipeW, 26, TRUE);
        y += 32;

        MoveWindow(g.mapWnd, m, y, cx - 2 * m, 80, TRUE);
        y += 86;

        // Six complete data rows keep the table useful while allowing the entire
        // fixed layout to fit on an 800x600 WinPE display.
        const int listHeight = ListHeightForRows(6);
        MoveWindow(g.list, m, y, cx - 2 * m, listHeight, TRUE);
        y += listHeight + 5;

        MoveWindow(g.selectionText, m, y, cx - 2 * m, 20, TRUE);
        y += 24;

        int col1 = m;
        int col2 = m + MaxInt(300, (cx - 3 * m) / 2);
        int colW = (cx - 3 * m) / 2;
        if (colW < 300) { col2 = m + 308; colW = cx - col2 - m; }

        // Windows layout block.
        HWND h;
        h = GetDlgItem(g.hwnd, 2001); MoveWindow(h, col1, y, colW, 82, TRUE);
        MoveWindow(GetDlgItem(g.hwnd, 2101), col1 + 10, y + 20, 132, 20, TRUE);
        MoveWindow(g.winSize, col1 + 142, y + 18, 66, 23, TRUE);
        MoveWindow(GetDlgItem(g.hwnd, 2102), col1 + 212, y + 20, 26, 20, TRUE);
        MoveWindow(g.winAll, col1 + 240, y + 18, 88, 23, TRUE);
        MoveWindow(g.noRecovery, col1 + 10, y + 48, 135, 23, TRUE);
        MoveWindow(g.createWindows, col1 + colW - 165, y + 46, 155, 26, TRUE);

        // Data block. Keep Create partition on the first row so the second row
        // can stay compact even at 800 pixels wide.
        h = GetDlgItem(g.hwnd, 2002); MoveWindow(h, col2, y, colW, 82, TRUE);
        MoveWindow(GetDlgItem(g.hwnd, 2201), col2 + 10, y + 20, 40, 20, TRUE);
        MoveWindow(g.dataSize, col2 + 50, y + 18, 62, 23, TRUE);
        MoveWindow(GetDlgItem(g.hwnd, 2202), col2 + 116, y + 20, 24, 20, TRUE);
        MoveWindow(g.dataAll, col2 + 142, y + 18, 88, 23, TRUE);
        MoveWindow(g.createData, col2 + colW - 125, y + 16, 115, 26, TRUE);
        MoveWindow(GetDlgItem(g.hwnd, 2203), col2 + 10, y + 50, 42, 20, TRUE);
        MoveWindow(g.dataLabel, col2 + 52, y + 47, MaxInt(100, colW - 218), 23, TRUE);
        MoveWindow(GetDlgItem(g.hwnd, 2204), col2 + colW - 155, y + 50, 22, 20, TRUE);
        MoveWindow(g.dataFs, col2 + colW - 131, y + 47, 121, 160, TRUE);

        y += 88;

        // Partition actions block.
        h = GetDlgItem(g.hwnd, 2003); MoveWindow(h, col1, y, colW, 90, TRUE);
        MoveWindow(g.deleteBtn, col1 + 10, y + 23, 95, 26, TRUE);
        MoveWindow(g.formatFs, col1 + 113, y + 23, 82, 160, TRUE);
        MoveWindow(g.formatBtn, col1 + 203, y + 23, 110, 26, TRUE);
        MoveWindow(GetDlgItem(g.hwnd, 2401), col1 + 10, y + 57, 42, 20, TRUE);
        MoveWindow(g.formatLabel, col1 + 52, y + 54, MaxInt(90, colW - 182), 23, TRUE);
        MoveWindow(g.setLabelBtn, col1 + colW - 122, y + 52, 112, 26, TRUE);

        // Resize block: two compact rows, each with By/To mode.
        h = GetDlgItem(g.hwnd, 2004); MoveWindow(h, col2, y, colW, 90, TRUE);
        MoveWindow(GetDlgItem(g.hwnd, 2301), col2 + 10, y + 25, 50, 20, TRUE);
        MoveWindow(g.extendMode, col2 + 60, y + 21, 55, 110, TRUE);
        MoveWindow(g.extendSize, col2 + 121, y + 21, 56, 23, TRUE);
        MoveWindow(GetDlgItem(g.hwnd, 2302), col2 + 181, y + 25, 24, 20, TRUE);
        MoveWindow(g.extendAll, col2 + 207, y + 21, 45, 23, TRUE);
        MoveWindow(g.extendBtn, col2 + colW - 92, y + 19, 82, 26, TRUE);

        MoveWindow(GetDlgItem(g.hwnd, 2303), col2 + 10, y + 57, 50, 20, TRUE);
        MoveWindow(g.shrinkMode, col2 + 60, y + 53, 55, 110, TRUE);
        MoveWindow(g.shrinkSize, col2 + 121, y + 53, 56, 23, TRUE);
        MoveWindow(GetDlgItem(g.hwnd, 2304), col2 + 181, y + 57, 24, 20, TRUE);
        MoveWindow(g.shrinkBtn, col2 + colW - 92, y + 51, 82, 26, TRUE);

        y += 96;

        const int openLogW = 82;
        const int bottomY = cy - 27;
        const int openLogX = cx - m - openLogW;
        MoveWindow(g.openLogBtn, openLogX, bottomY, openLogW, 24, TRUE);
        if (g.installWindowsBtn)
        {
            const int installW = 160;
            const int installX = openLogX - gap - installW;
            MoveWindow(g.installWindowsBtn, installX, bottomY, installW, 24, TRUE);
            MoveWindow(g.status, m, bottomY + 3, MaxInt(100, installX - m - gap), 19, TRUE);
        }
        else
        {
            MoveWindow(g.status, m, bottomY + 3, MaxInt(100, cx - 3 * m - openLogW), 19, TRUE);
        }
    }

    void CreateUi(HWND hwnd)
    {
        g.hwnd = hwnd;
        g.font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        g.diskCombo = MakeControl(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_DISK_COMBO);
        g.refreshBtn = MakeControl(0, WC_BUTTONW, L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, IDC_REFRESH);
        g.convertGptBtn = MakeControl(0, WC_BUTTONW, L"Convert GPT", BS_PUSHBUTTON | WS_TABSTOP, IDC_CONVERT_GPT);
        g.wipeBtn = MakeControl(0, WC_BUTTONW, L"Wipe...", BS_PUSHBUTTON | WS_TABSTOP, IDC_WIPE);

        g.mapWnd = MakeControl(WS_EX_CLIENTEDGE, L"DiskPrepMap", L"", WS_TABSTOP, IDC_MAP);

        g.list = MakeControl(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, IDC_LIST);
        ListView_SetExtendedListViewStyle(g.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        AddListColumn(0, 42, L"#");
        AddListColumn(1, 150, L"Type");
        AddListColumn(2, 80, L"FS");
        AddListColumn(3, 130, L"Label");
        AddListColumn(4, 65, L"Letter");
        AddListColumn(5, 105, L"Size");
        AddListColumn(6, 105, L"Offset");

        g.selectionText = MakeControl(0, WC_STATICW, L"Nothing selected", SS_LEFT, IDC_SELECTION);

        // Group boxes and their static labels use fixed IDs only for layout.
        MakeControl(0, WC_BUTTONW, L"Windows layout", BS_GROUPBOX, 2001);
        MakeControl(0, WC_STATICW, L"Windows size:", SS_LEFT, 2101);
        MakeControl(0, WC_STATICW, L"GB", SS_LEFT, 2102);
        g.winSize = MakeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"128", ES_AUTOHSCROLL | WS_TABSTOP, IDC_WIN_SIZE);
        g.winAll = MakeControl(0, WC_BUTTONW, L"All space", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_WIN_ALL);
        g.noRecovery = MakeControl(0, WC_BUTTONW, L"No Recovery", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_NO_RECOVERY);
        g.createWindows = MakeControl(0, WC_BUTTONW, L"Create WIN partitions", BS_PUSHBUTTON | WS_TABSTOP, IDC_CREATE_WINDOWS);

        MakeControl(0, WC_BUTTONW, L"Data partition", BS_GROUPBOX, 2002);
        MakeControl(0, WC_STATICW, L"Size:", SS_LEFT, 2201);
        MakeControl(0, WC_STATICW, L"GB", SS_LEFT, 2202);
        MakeControl(0, WC_STATICW, L"Label:", SS_LEFT, 2203);
        MakeControl(0, WC_STATICW, L"FS:", SS_LEFT, 2204);
        g.dataSize = MakeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"100", ES_AUTOHSCROLL | WS_TABSTOP, IDC_DATA_SIZE);
        g.dataAll = MakeControl(0, WC_BUTTONW, L"All space", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_DATA_ALL);
        g.dataLabel = MakeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"DATA", ES_AUTOHSCROLL | WS_TABSTOP, IDC_DATA_LABEL);
        g.dataFs = MakeControl(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_DATA_FS);
        g.createData = MakeControl(0, WC_BUTTONW, L"Create partition", BS_PUSHBUTTON | WS_TABSTOP, IDC_CREATE_DATA);

        MakeControl(0, WC_BUTTONW, L"Selected partition", BS_GROUPBOX, 2003);
        g.deleteBtn = MakeControl(0, WC_BUTTONW, L"Delete", BS_PUSHBUTTON | WS_TABSTOP, IDC_DELETE);
        g.formatFs = MakeControl(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_FORMAT_FS);
        g.formatBtn = MakeControl(0, WC_BUTTONW, L"Format", BS_PUSHBUTTON | WS_TABSTOP, IDC_FORMAT);
        MakeControl(0, WC_STATICW, L"Label:", SS_LEFT, 2401);
        g.formatLabel = MakeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_FORMAT_LABEL);
        g.setLabelBtn = MakeControl(0, WC_BUTTONW, L"Set Label", BS_PUSHBUTTON | WS_TABSTOP, IDC_SET_LABEL);

        MakeControl(0, WC_BUTTONW, L"Resize", BS_GROUPBOX, 2004);
        MakeControl(0, WC_STATICW, L"Extend:", SS_LEFT, 2301);
        MakeControl(0, WC_STATICW, L"GB", SS_LEFT, 2302);
        g.extendMode = MakeControl(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_EXTEND_MODE);
        g.extendSize = MakeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"10", ES_AUTOHSCROLL | WS_TABSTOP, IDC_EXTEND_SIZE);
        g.extendAll = MakeControl(0, WC_BUTTONW, L"All", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_EXTEND_ALL);
        g.extendBtn = MakeControl(0, WC_BUTTONW, L"Extend", BS_PUSHBUTTON | WS_TABSTOP, IDC_EXTEND);

        MakeControl(0, WC_STATICW, L"Shrink:", SS_LEFT, 2303);
        MakeControl(0, WC_STATICW, L"GB", SS_LEFT, 2304);
        g.shrinkMode = MakeControl(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_SHRINK_MODE);
        g.shrinkSize = MakeControl(WS_EX_CLIENTEDGE, WC_EDITW, L"10", ES_AUTOHSCROLL | WS_TABSTOP, IDC_SHRINK_SIZE);
        g.shrinkBtn = MakeControl(0, WC_BUTTONW, L"Shrink", BS_PUSHBUTTON | WS_TABSTOP, IDC_SHRINK);

        const wchar_t* resizeModes[] = { L"By", L"To" };
        for (const wchar_t* mode : resizeModes)
        {
            SendMessageW(g.extendMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(mode));
            SendMessageW(g.shrinkMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(mode));
        }
        SendMessageW(g.extendMode, CB_SETCURSEL, 0, 0);
        SendMessageW(g.shrinkMode, CB_SETCURSEL, 0, 0);

        g.status = MakeControl(0, WC_STATICW, L"", SS_LEFT, IDC_STATUS);
        g.openLogBtn = MakeControl(0, WC_BUTTONW, L"Open log", BS_PUSHBUTTON | WS_TABSTOP, IDC_OPEN_LOG);
        if (g.winSetupMode)
            g.installWindowsBtn = MakeControl(0, WC_BUTTONW, L"Install Windows here",
                BS_PUSHBUTTON | WS_TABSTOP, IDC_INSTALL_WINDOWS);

        const wchar_t* fileSystems[] = { L"NTFS", L"FAT32", L"exFAT" };
        for (const wchar_t* fs : fileSystems)
        {
            SendMessageW(g.dataFs, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(fs));
            SendMessageW(g.formatFs, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(fs));
        }
        SendMessageW(g.dataFs, CB_SETCURSEL, 0, 0);
        SendMessageW(g.formatFs, CB_SETCURSEL, 0, 0);

        Enable(g.createWindows, false);
        Enable(g.winAll, false);
        Enable(g.winSize, false);
        Enable(g.noRecovery, false);
        Enable(g.createData, false);
        Enable(g.deleteBtn, false);
        Enable(g.formatBtn, false);
        Enable(g.formatFs, false);
        Enable(g.formatLabel, false);
        Enable(g.setLabelBtn, false);
        Enable(g.extendMode, false);
        Enable(g.extendSize, false);
        Enable(g.extendAll, false);
        Enable(g.extendBtn, false);
        Enable(g.shrinkMode, false);
        Enable(g.shrinkSize, false);
        Enable(g.shrinkBtn, false);
        if (g.installWindowsBtn) Enable(g.installWindowsBtn, false);

        SendMessageW(g.winAll, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(g.noRecovery, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(g.dataAll, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(g.extendAll, BM_SETCHECK, BST_UNCHECKED, 0);
    }

    void OnDiskComboChanged()
    {
        int combo = static_cast<int>(SendMessageW(g.diskCombo, CB_GETCURSEL, 0, 0));
        if (combo == CB_ERR) return;
        LRESULT data = SendMessageW(g.diskCombo, CB_GETITEMDATA, combo, 0);
        if (data == CB_ERR) return;
        g.selectedDiskVectorIndex = static_cast<int>(data);
        PopulateList();
        InvalidateRect(g.mapWnd, nullptr, TRUE);
        Enable(g.wipeBtn, CurrentDisk() != nullptr);
        UpdateConvertButton();
    }

    LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_CREATE:
            CreateUi(hwnd);
            RefreshDisks();
            LogDiskInventory(L"Initial disk inventory");
            return 0;

        case WM_SIZE:
            LayoutControls(LOWORD(lParam), HIWORD(lParam));
            return 0;

        case WM_ACTIVATEAPP:
            if (wParam) PostMessageW(hwnd, WM_APP_FORCE_REDRAW, 0, 0);
            return 0;

        case WM_APP_FORCE_REDRAW:
            RedrawWindow(hwnd, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_COMMAND:
        {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            if (id == IDC_DISK_COMBO && code == CBN_SELCHANGE) OnDiskComboChanged();
            else if (id == IDC_REFRESH && code == BN_CLICKED) RefreshCurrentDisk();
            else if (id == IDC_DELETE && code == BN_CLICKED) DeleteSelected();
            else if (id == IDC_FORMAT && code == BN_CLICKED) FormatSelected();
            else if (id == IDC_SET_LABEL && code == BN_CLICKED) SetSelectedLabel();
            else if (id == IDC_CREATE_DATA && code == BN_CLICKED) CreateDataPartition();
            else if (id == IDC_CREATE_WINDOWS && code == BN_CLICKED) CreateWindowsLayout();
            else if (id == IDC_WIN_ALL && code == BN_CLICKED)
            {
                UpdateWindowsAllPreview();
            }
            else if (id == IDC_NO_RECOVERY && code == BN_CLICKED)
            {
                UpdateWindowsAllPreview();
            }
            else if (id == IDC_EXTEND_ALL && code == BN_CLICKED) UpdateSelectionUI();
            else if (id == IDC_EXTEND && code == BN_CLICKED) ExtendSelected();
            else if (id == IDC_SHRINK && code == BN_CLICKED) ShrinkSelected();
            else if (id == IDC_CONVERT_GPT && code == BN_CLICKED) ConvertDiskStyle();
            else if (id == IDC_WIPE && code == BN_CLICKED) WipeDisk();
            else if (id == IDC_OPEN_LOG && code == BN_CLICKED) OpenSessionLog();
            else if (id == IDC_INSTALL_WINDOWS && code == BN_CLICKED) InstallWindowsHere();
            else if (id == IDC_DATA_ALL && code == BN_CLICKED)
            {
                bool all = SendMessageW(g.dataAll, BM_GETCHECK, 0, 0) == BST_CHECKED;
                Enable(g.dataSize, !all);
            }
            return 0;
        }

        case WM_NOTIFY:
        {
            auto* hdr = reinterpret_cast<NMHDR*>(lParam);
            if (hdr->idFrom == IDC_LIST && hdr->code == LVN_ITEMCHANGED)
            {
                auto* n = reinterpret_cast<NMLISTVIEW*>(lParam);
                if ((n->uNewState & LVIS_SELECTED) && !(n->uOldState & LVIS_SELECTED))
                {
                    LVITEMW item{};
                    item.mask = LVIF_PARAM;
                    item.iItem = n->iItem;
                    if (ListView_GetItem(g.list, &item)) SelectSegment(static_cast<int>(item.lParam), false);
                }
            }
            return 0;
        }

        case WM_APP_DISKPART_OUTPUT:
        {
            auto* text = reinterpret_cast<std::wstring*>(lParam);
            if (text)
            {
                AppendDiskPartOutput(*text);
                delete text;
            }
            return 0;
        }

        case WM_APP_OPERATION_DONE:
            FinishOperation(reinterpret_cast<AsyncResult*>(lParam));
            return 0;

        case WM_GETMINMAXINFO:
        {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 760;
            mmi->ptMinTrackSize.y = 560;
            return 0;
        }

        case WM_DESTROY:
            if (g.logFile != INVALID_HANDLE_VALUE)
            {
                CloseHandle(g.logFile);
                g.logFile = INVALID_HANDLE_VALUE;
            }
            if (g.font) DeleteObject(g.font);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR commandLine, _In_ int show)
{
    SetProcessDPIAware();
    g.winSetupMode = IsWinSetupCommandLine(commandLine);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    InitializeSessionLog();
    EnsureWinSetupDisplayMode();

    HICON appIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR));
    HICON appIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    if (!appIcon) appIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!appIconSm) appIconSm = appIcon;

    WNDCLASSEXW mapClass{};
    mapClass.cbSize = sizeof(mapClass);
    mapClass.hInstance = instance;
    mapClass.lpfnWndProc = MapProc;
    mapClass.lpszClassName = L"DiskPrepMap";
    mapClass.hCursor = LoadCursorW(nullptr, IDC_HAND);
    mapClass.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW) + 1);
    RegisterClassExW(&mapClass);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = L"DiskPrepMain";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = appIcon;
    wc.hIconSm = appIconSm;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE) + 1);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);

    constexpr int initialWidth = INITIAL_WINDOW_WIDTH;
    constexpr int initialHeight = INITIAL_WINDOW_HEIGHT;
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    const int initialX = MaxInt(0, (screenWidth - initialWidth) / 2);
    const int initialY = MaxInt(0, (screenHeight - initialHeight) / 2);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName,
        L"DiskPrep v1.0 — Windows Setup partition helper",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        initialX, initialY, initialWidth, initialHeight,
        nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}
