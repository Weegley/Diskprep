#include "DiskPart.h"

#include <vector>

namespace
{
    std::wstring Win32Error(DWORD code)
    {
        wchar_t* msg = nullptr;
        DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
        FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
        std::wstring out = msg ? msg : L"Unknown error";
        if (msg) LocalFree(msg);
        return out;
    }

    bool WideToCodePage(const std::wstring& text, UINT codePage, std::string& out, bool failOnLoss)
    {
        BOOL usedDefault = FALSE;
        DWORD flags = 0;
        if (failOnLoss && codePage != CP_UTF8)
            flags = WC_NO_BEST_FIT_CHARS;

        int needed = WideCharToMultiByte(codePage, flags, text.data(), static_cast<int>(text.size()),
            nullptr, 0, nullptr, failOnLoss ? &usedDefault : nullptr);
        if (needed <= 0 || (failOnLoss && usedDefault)) return false;

        out.resize(static_cast<size_t>(needed));
        usedDefault = FALSE;
        int written = WideCharToMultiByte(codePage, flags, text.data(), static_cast<int>(text.size()),
            out.data(), needed, nullptr, failOnLoss ? &usedDefault : nullptr);
        return written == needed && !(failOnLoss && usedDefault);
    }

    std::wstring MultiByteToWide(const std::string& text, UINT codePage)
    {
        if (text.empty()) return L"";
        int needed = MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (needed <= 0) return L"";
        std::wstring out(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
        return out;
    }

    bool WriteAnsiScript(const wchar_t* path, const std::wstring& script, std::wstring& error)
    {
        // DiskPart /s is happiest with a legacy text file without BOM.
        // DiskPrep deliberately keeps DiskPart scripts ASCII-only; user-entered
        // volume labels are applied separately through the Unicode Win32 API.
        std::string bytes;
        if (!WideToCodePage(script, CP_ACP, bytes, true))
        {
            error = L"DiskPart script contains characters that cannot be represented in the current ANSI code page. "
                    L"Use an ASCII-compatible volume label.";
            return false;
        }

        HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            error = L"Cannot create temporary DiskPart script: " + Win32Error(GetLastError());
            return false;
        }

        DWORD written = 0;
        bool ok = true;
        DWORD err = ERROR_SUCCESS;
        if (!bytes.empty())
        {
            if (bytes.size() > MAXDWORD)
            {
                ok = false;
                err = ERROR_FILE_TOO_LARGE;
            }
            else
            {
                ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE &&
                     written == static_cast<DWORD>(bytes.size());
                if (!ok) err = GetLastError();
            }
        }
        CloseHandle(h);
        if (!ok)
        {
            error = L"Cannot write temporary DiskPart script: " + Win32Error(err);
            return false;
        }
        return true;
    }
}

namespace DiskPart
{
    std::wstring QuoteLabel(const std::wstring& label)
    {
        std::wstring clean;
        clean.reserve(label.size());
        for (wchar_t c : label)
        {
            if (c == L'\r' || c == L'\n' || c == L'\"') continue;
            clean.push_back(c);
            if (clean.size() >= 32) break;
        }
        if (clean.empty()) clean = L"Data";
        return L"\"" + clean + L"\"";
    }

    Result RunScript(const std::wstring& script, const OutputCallback& onOutput)
    {
        Result result;
        wchar_t tempPath[MAX_PATH]{};
        wchar_t tempFile[MAX_PATH]{};
        if (!GetTempPathW(ARRAYSIZE(tempPath), tempPath) ||
            !GetTempFileNameW(tempPath, L"dpp", 0, tempFile))
        {
            result.error = L"Cannot create temporary DiskPart script.";
            return result;
        }

        if (!WriteAnsiScript(tempFile, script, result.error))
        {
            DeleteFileW(tempFile);
            return result;
        }

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE readPipe = nullptr;
        HANDLE writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
        {
            DWORD err = GetLastError();
            DeleteFileW(tempFile);
            result.error = L"Cannot create DiskPart output pipe: " + Win32Error(err);
            return result;
        }
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

        std::wstring command = L"diskpart.exe /s \"" + std::wstring(tempFile) + L"\"";
        std::vector<wchar_t> cmd(command.begin(), command.end());
        cmd.push_back(L'\0');

        HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = writePipe;
        si.hStdError = writePipe;
        si.hStdInput = nullInput != INVALID_HANDLE_VALUE ? nullInput : GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi{};

        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        {
            DWORD err = GetLastError();
            if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
            CloseHandle(readPipe);
            CloseHandle(writePipe);
            DeleteFileW(tempFile);
            result.error = L"Cannot start diskpart.exe: " + Win32Error(err);
            return result;
        }

        result.started = true;
        if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
        CloseHandle(writePipe);
        writePipe = nullptr;

        std::string captured;
        std::string pendingLine;
        char buffer[4096];
        DWORD got = 0;

        auto emitCompleteLines = [&]()
        {
            size_t start = 0;
            for (size_t i = 0; i < pendingLine.size(); ++i)
            {
                const char c = pendingLine[i];
                if (c != '\r' && c != '\n') continue;

                std::string line = pendingLine.substr(start, i - start);
                if (onOutput)
                {
                    std::wstring wide = MultiByteToWide(line, CP_OEMCP);
                    wide += L"\r\n";
                    onOutput(wide);
                }

                if (c == '\r' && i + 1 < pendingLine.size() && pendingLine[i + 1] == '\n')
                    ++i;
                start = i + 1;
            }

            if (start != 0)
                pendingLine.erase(0, start);
        };

        while (ReadFile(readPipe, buffer, sizeof(buffer), &got, nullptr) && got != 0)
        {
            captured.append(buffer, buffer + got);
            pendingLine.append(buffer, buffer + got);
            emitCompleteLines();
        }

        if (!pendingLine.empty() && onOutput)
            onOutput(MultiByteToWide(pendingLine, CP_OEMCP));

        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &result.exitCode);

        result.output = MultiByteToWide(captured, CP_OEMCP);

        CloseHandle(readPipe);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        DeleteFileW(tempFile);
        return result;
    }
}
