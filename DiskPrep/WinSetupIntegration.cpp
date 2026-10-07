#include "WinSetupIntegration.h"

#include <windows.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

namespace
{
    enum class TextEncoding
    {
        Utf8,
        Utf8Bom,
        Utf16Le,
        Utf16Be
    };

    struct TextFile
    {
        std::wstring text;
        TextEncoding encoding = TextEncoding::Utf8;
    };

    const wchar_t* TextEncodingName(TextEncoding encoding)
    {
        switch (encoding)
        {
        case TextEncoding::Utf8: return L"UTF-8";
        case TextEncoding::Utf8Bom: return L"UTF-8 BOM";
        case TextEncoding::Utf16Le: return L"UTF-16 LE";
        case TextEncoding::Utf16Be: return L"UTF-16 BE";
        default: return L"unknown";
        }
    }

    struct XmlTag
    {
        size_t start = 0;
        size_t end = 0; // one past '>'
        bool closing = false;
        bool selfClosing = false;
        std::wstring localName;
        std::wstring raw;
    };

    struct XmlElement
    {
        size_t openStart = 0;
        size_t openEnd = 0;
        size_t closeStart = 0;
        size_t closeEnd = 0;
        bool selfClosing = false;
    };

    std::wstring Win32ErrorText(DWORD error)
    {
        wchar_t* buffer = nullptr;
        DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
            reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
        std::wstring result;
        if (n && buffer)
        {
            result.assign(buffer, n);
            while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' '))
                result.pop_back();
        }
        if (buffer) LocalFree(buffer);
        if (result.empty()) result = L"Win32 error " + std::to_wstring(error);
        return result;
    }

    bool ReadAllBytes(const std::wstring& path, std::vector<BYTE>& bytes, std::wstring& error)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            error = L"Cannot open unattended file:\n" + path + L"\n\n" + Win32ErrorText(GetLastError());
            return false;
        }

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > 64ll * 1024ll * 1024ll)
        {
            DWORD e = GetLastError();
            CloseHandle(h);
            error = L"Cannot read unattended file size.\n\n" + Win32ErrorText(e ? e : ERROR_FILE_TOO_LARGE);
            return false;
        }

        bytes.resize(static_cast<size_t>(size.QuadPart));
        size_t done = 0;
        while (done < bytes.size())
        {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - done, 1024u * 1024u));
            DWORD got = 0;
            if (!ReadFile(h, bytes.data() + done, chunk, &got, nullptr))
            {
                DWORD e = GetLastError();
                CloseHandle(h);
                error = L"Cannot read unattended file.\n\n" + Win32ErrorText(e);
                return false;
            }
            if (got == 0) break;
            done += got;
        }
        CloseHandle(h);
        bytes.resize(done);
        return true;
    }

    bool DecodeUtf8(const BYTE* data, size_t size, std::wstring& out)
    {
        if (size == 0)
        {
            out.clear();
            return true;
        }
        if (size > static_cast<size_t>(INT_MAX)) return false;
        int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            reinterpret_cast<LPCCH>(data), static_cast<int>(size), nullptr, 0);
        if (needed <= 0) return false;
        out.resize(static_cast<size_t>(needed));
        return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            reinterpret_cast<LPCCH>(data), static_cast<int>(size), out.data(), needed) == needed;
    }

    bool ReadTextFile(const std::wstring& path, TextFile& file, std::wstring& error)
    {
        std::vector<BYTE> bytes;
        if (!ReadAllBytes(path, bytes, error)) return false;

        if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        {
            file.encoding = TextEncoding::Utf8Bom;
            if (!DecodeUtf8(bytes.data() + 3, bytes.size() - 3, file.text))
            {
                error = L"The unattended file is not valid UTF-8.";
                return false;
            }
            return true;
        }

        if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE)
        {
            file.encoding = TextEncoding::Utf16Le;
            const size_t count = (bytes.size() - 2) / 2;
            file.text.resize(count);
            for (size_t i = 0; i < count; ++i)
            {
                uint16_t v = static_cast<uint16_t>(bytes[2 + i * 2]) |
                    (static_cast<uint16_t>(bytes[3 + i * 2]) << 8);
                file.text[i] = static_cast<wchar_t>(v);
            }
            return true;
        }

        if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF)
        {
            file.encoding = TextEncoding::Utf16Be;
            const size_t count = (bytes.size() - 2) / 2;
            file.text.resize(count);
            for (size_t i = 0; i < count; ++i)
            {
                uint16_t v = (static_cast<uint16_t>(bytes[2 + i * 2]) << 8) |
                    static_cast<uint16_t>(bytes[3 + i * 2]);
                file.text[i] = static_cast<wchar_t>(v);
            }
            return true;
        }

        file.encoding = TextEncoding::Utf8;
        if (!DecodeUtf8(bytes.data(), bytes.size(), file.text))
        {
            error = L"Unsupported unattended-file encoding. DiskPrep supports UTF-8 and UTF-16 XML files.";
            return false;
        }
        return true;
    }

    bool EncodeText(const TextFile& file, std::vector<BYTE>& bytes, std::wstring& error)
    {
        bytes.clear();
        if (file.encoding == TextEncoding::Utf16Le || file.encoding == TextEncoding::Utf16Be)
        {
            bytes.reserve(2 + file.text.size() * 2);
            if (file.encoding == TextEncoding::Utf16Le)
            {
                bytes.push_back(0xFF);
                bytes.push_back(0xFE);
            }
            else
            {
                bytes.push_back(0xFE);
                bytes.push_back(0xFF);
            }

            for (wchar_t wc : file.text)
            {
                uint16_t v = static_cast<uint16_t>(wc);
                if (file.encoding == TextEncoding::Utf16Le)
                {
                    bytes.push_back(static_cast<BYTE>(v & 0xFF));
                    bytes.push_back(static_cast<BYTE>((v >> 8) & 0xFF));
                }
                else
                {
                    bytes.push_back(static_cast<BYTE>((v >> 8) & 0xFF));
                    bytes.push_back(static_cast<BYTE>(v & 0xFF));
                }
            }
            return true;
        }

        if (file.text.size() > static_cast<size_t>(INT_MAX))
        {
            error = L"Unattended file is too large.";
            return false;
        }

        int needed = file.text.empty() ? 0 : WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            file.text.data(), static_cast<int>(file.text.size()), nullptr, 0, nullptr, nullptr);
        if (!file.text.empty() && needed <= 0)
        {
            error = L"Cannot encode unattended file as UTF-8.\n\n" + Win32ErrorText(GetLastError());
            return false;
        }

        if (file.encoding == TextEncoding::Utf8Bom)
        {
            bytes.push_back(0xEF);
            bytes.push_back(0xBB);
            bytes.push_back(0xBF);
        }
        const size_t base = bytes.size();
        bytes.resize(base + static_cast<size_t>(needed));
        if (needed > 0 && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            file.text.data(), static_cast<int>(file.text.size()),
            reinterpret_cast<LPSTR>(bytes.data() + base), needed, nullptr, nullptr) != needed)
        {
            error = L"Cannot encode unattended file as UTF-8.\n\n" + Win32ErrorText(GetLastError());
            return false;
        }
        return true;
    }

    bool WriteAllBytesAtomic(const std::wstring& path, const std::vector<BYTE>& bytes, std::wstring& error)
    {
        const std::wstring temp = path + L".tmp";
        DeleteFileW(temp.c_str());

        HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            error = L"Cannot create temporary unattended file:\n" + temp + L"\n\n" + Win32ErrorText(GetLastError());
            return false;
        }

        size_t done = 0;
        while (done < bytes.size())
        {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - done, 1024u * 1024u));
            DWORD written = 0;
            if (!WriteFile(h, bytes.data() + done, chunk, &written, nullptr) || written != chunk)
            {
                DWORD e = GetLastError();
                CloseHandle(h);
                DeleteFileW(temp.c_str());
                error = L"Cannot write temporary unattended file.\n\n" + Win32ErrorText(e);
                return false;
            }
            done += written;
        }
        FlushFileBuffers(h);
        CloseHandle(h);

        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DWORD e = GetLastError();
            DeleteFileW(temp.c_str());
            error = L"Cannot replace unattended file:\n" + path + L"\n\n" + Win32ErrorText(e);
            return false;
        }
        return true;
    }

    bool WriteTextFileAtomic(const std::wstring& path, const TextFile& file, std::wstring& error)
    {
        std::vector<BYTE> bytes;
        if (!EncodeText(file, bytes, error)) return false;
        return WriteAllBytesAtomic(path, bytes, error);
    }

    std::wstring LocalName(const std::wstring& name)
    {
        size_t colon = name.find_last_of(L':');
        return colon == std::wstring::npos ? name : name.substr(colon + 1);
    }

    bool NextTag(const std::wstring& text, size_t& cursor, size_t limit, XmlTag& out)
    {
        limit = std::min(limit, text.size());
        while (cursor < limit)
        {
            size_t p = text.find(L'<', cursor);
            if (p == std::wstring::npos || p >= limit) return false;

            if (text.compare(p, 4, L"<!--") == 0)
            {
                size_t e = text.find(L"-->", p + 4);
                if (e == std::wstring::npos || e + 3 > limit) return false;
                cursor = e + 3;
                continue;
            }
            if (text.compare(p, 9, L"<![CDATA[") == 0)
            {
                size_t e = text.find(L"]]>", p + 9);
                if (e == std::wstring::npos || e + 3 > limit) return false;
                cursor = e + 3;
                continue;
            }
            if (text.compare(p, 2, L"<?") == 0)
            {
                size_t e = text.find(L"?>", p + 2);
                if (e == std::wstring::npos || e + 2 > limit) return false;
                cursor = e + 2;
                continue;
            }
            if (text.compare(p, 2, L"<!") == 0)
            {
                size_t e = text.find(L'>', p + 2);
                if (e == std::wstring::npos || e + 1 > limit) return false;
                cursor = e + 1;
                continue;
            }

            bool inQuote = false;
            wchar_t quote = 0;
            size_t e = p + 1;
            for (; e < limit; ++e)
            {
                wchar_t c = text[e];
                if (inQuote)
                {
                    if (c == quote) inQuote = false;
                }
                else if (c == L'\'' || c == L'"')
                {
                    inQuote = true;
                    quote = c;
                }
                else if (c == L'>')
                {
                    break;
                }
            }
            if (e >= limit) return false;

            size_t n = p + 1;
            while (n < e && iswspace(text[n])) ++n;
            bool closing = false;
            if (n < e && text[n] == L'/')
            {
                closing = true;
                ++n;
                while (n < e && iswspace(text[n])) ++n;
            }
            size_t ns = n;
            while (n < e && !iswspace(text[n]) && text[n] != L'/' && text[n] != L'>') ++n;
            if (n == ns)
            {
                cursor = e + 1;
                continue;
            }

            size_t back = e;
            while (back > p && iswspace(text[back - 1])) --back;
            bool selfClosing = back > p && text[back - 1] == L'/';

            out.start = p;
            out.end = e + 1;
            out.closing = closing;
            out.selfClosing = selfClosing;
            out.localName = LocalName(text.substr(ns, n - ns));
            out.raw = text.substr(p, e + 1 - p);
            cursor = e + 1;
            return true;
        }
        return false;
    }

    bool AttributeEquals(const std::wstring& rawTag, const std::wstring& attrName, const std::wstring& expected)
    {
        size_t p = 0;
        while (p < rawTag.size())
        {
            p = rawTag.find(attrName, p);
            if (p == std::wstring::npos) return false;

            const bool leftOk = p == 0 || iswspace(rawTag[p - 1]) || rawTag[p - 1] == L'<';
            const size_t afterName = p + attrName.size();
            const bool rightOk = afterName >= rawTag.size() || iswspace(rawTag[afterName]) || rawTag[afterName] == L'=';
            if (!leftOk || !rightOk)
            {
                p = afterName;
                continue;
            }

            size_t q = afterName;
            while (q < rawTag.size() && iswspace(rawTag[q])) ++q;
            if (q >= rawTag.size() || rawTag[q] != L'=')
            {
                p = afterName;
                continue;
            }
            ++q;
            while (q < rawTag.size() && iswspace(rawTag[q])) ++q;
            if (q >= rawTag.size() || (rawTag[q] != L'\'' && rawTag[q] != L'"')) return false;
            wchar_t quote = rawTag[q++];
            size_t valueEnd = rawTag.find(quote, q);
            if (valueEnd == std::wstring::npos) return false;
            return rawTag.substr(q, valueEnd - q) == expected;
        }
        return false;
    }

    bool FindElement(const std::wstring& text, const std::wstring& localName,
        size_t begin, size_t end, XmlElement& element,
        const std::wstring& attrName = L"", const std::wstring& attrValue = L"")
    {
        size_t cursor = begin;
        XmlTag tag{};
        while (NextTag(text, cursor, end, tag))
        {
            if (tag.closing || tag.localName != localName) continue;
            if (!attrName.empty() && !AttributeEquals(tag.raw, attrName, attrValue)) continue;

            element.openStart = tag.start;
            element.openEnd = tag.end;
            element.selfClosing = tag.selfClosing;
            if (tag.selfClosing)
            {
                element.closeStart = tag.start;
                element.closeEnd = tag.end;
                return true;
            }

            int depth = 1;
            size_t nestedCursor = tag.end;
            XmlTag nested{};
            while (NextTag(text, nestedCursor, end, nested))
            {
                if (nested.localName != localName) continue;
                if (nested.closing)
                {
                    --depth;
                    if (depth == 0)
                    {
                        element.closeStart = nested.start;
                        element.closeEnd = nested.end;
                        return true;
                    }
                }
                else if (!nested.selfClosing)
                {
                    ++depth;
                }
            }
            return false;
        }
        return false;
    }

    std::wstring NewLineFor(const std::wstring& text)
    {
        return text.find(L"\r\n") != std::wstring::npos ? L"\r\n" : L"\n";
    }

    std::wstring LineIndent(const std::wstring& text, size_t pos)
    {
        if (pos > text.size()) pos = text.size();
        size_t line = text.rfind(L'\n', pos == 0 ? 0 : pos - 1);
        line = line == std::wstring::npos ? 0 : line + 1;
        size_t p = line;
        while (p < pos && (text[p] == L' ' || text[p] == L'\t')) ++p;
        return text.substr(line, p - line);
    }

    size_t IndentedLineStart(const std::wstring& text, size_t pos)
    {
        if (pos > text.size()) pos = text.size();
        size_t line = text.rfind(L'\n', pos == 0 ? 0 : pos - 1);
        line = line == std::wstring::npos ? 0 : line + 1;
        for (size_t p = line; p < pos; ++p)
        {
            if (text[p] != L' ' && text[p] != L'\t') return pos;
        }
        return line;
    }

    std::wstring WithoutFirstIndent(const std::wstring& block, const std::wstring& indent)
    {
        if (!indent.empty() && block.compare(0, indent.size(), indent) == 0)
            return block.substr(indent.size());
        return block;
    }

    void ReplaceRange(std::wstring& text, size_t start, size_t end, const std::wstring& replacement)
    {
        text.replace(start, end - start, replacement);
    }

    std::wstring SetupComponentOpenTag()
    {
#ifdef _WIN64
        const wchar_t* arch = L"amd64";
#else
        const wchar_t* arch = L"x86";
#endif
        return std::wstring(L"<component name=\"Microsoft-Windows-Setup\" processorArchitecture=\"") + arch +
            L"\" publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\">";
    }

    std::wstring MakeInstallTo(const std::wstring& indent, const std::wstring& nl, int disk, int partition)
    {
        const std::wstring child = indent + L"\t";
        return indent + L"<InstallTo>" + nl +
            child + L"<DiskID>" + std::to_wstring(disk) + L"</DiskID>" + nl +
            child + L"<PartitionID>" + std::to_wstring(partition) + L"</PartitionID>" + nl +
            indent + L"</InstallTo>";
    }

    std::wstring MakeImageInstall(const std::wstring& indent, const std::wstring& nl, int disk, int partition)
    {
        const std::wstring i1 = indent + L"\t";
        const std::wstring i2 = i1 + L"\t";
        const std::wstring i3 = i2 + L"\t";
        const std::wstring i4 = i3 + L"\t";
        return indent + L"<ImageInstall>" + nl +
            i1 + L"<OSImage>" + nl +
            i2 + L"<InstallTo>" + nl +
            i3 + L"<DiskID>" + std::to_wstring(disk) + L"</DiskID>" + nl +
            i3 + L"<PartitionID>" + std::to_wstring(partition) + L"</PartitionID>" + nl +
            i2 + L"</InstallTo>" + nl +
            i2 + L"<WillShowUI>OnError</WillShowUI>" + nl +
            i1 + L"</OSImage>" + nl +
            indent + L"</ImageInstall>";
    }

    bool LocateWindowsPeSetup(const std::wstring& text, XmlElement& settings, XmlElement& component)
    {
        XmlElement root{};
        if (!FindElement(text, L"unattend", 0, text.size(), root)) return false;
        if (!FindElement(text, L"settings", root.openEnd, root.closeStart, settings, L"pass", L"windowsPE")) return false;
        if (!FindElement(text, L"component", settings.openEnd, settings.closeStart, component,
            L"name", L"Microsoft-Windows-Setup")) return false;
        return true;
    }

    bool EnsureWindowsPeSetup(std::wstring& text, int disk, int partition, std::wstring& error)
    {
        XmlElement root{};
        if (!FindElement(text, L"unattend", 0, text.size(), root))
        {
            error = L"The unattended XML does not contain a valid <unattend> root element.";
            return false;
        }

        const std::wstring nl = NewLineFor(text);
        XmlElement settings{};
        if (!FindElement(text, L"settings", root.openEnd, root.closeStart, settings, L"pass", L"windowsPE"))
        {
            const std::wstring rootIndent = LineIndent(text, root.openStart);
            const std::wstring i1 = rootIndent + L"\t";
            const std::wstring i2 = i1 + L"\t";
            const std::wstring block = i1 + L"<settings pass=\"windowsPE\">" + nl +
                i2 + SetupComponentOpenTag() + nl +
                MakeImageInstall(i2 + L"\t", nl, disk, partition) + nl +
                i2 + L"</component>" + nl +
                i1 + L"</settings>" + nl;
            text.insert(IndentedLineStart(text, root.closeStart), block);
            return true;
        }

        XmlElement component{};
        if (!FindElement(text, L"component", settings.openEnd, settings.closeStart, component,
            L"name", L"Microsoft-Windows-Setup"))
        {
            const std::wstring settingsIndent = LineIndent(text, settings.openStart);
            const std::wstring i1 = settingsIndent + L"\t";
            const std::wstring i2 = i1 + L"\t";
            const std::wstring block = i1 + SetupComponentOpenTag() + nl +
                MakeImageInstall(i2, nl, disk, partition) + nl +
                i1 + L"</component>" + nl;
            text.insert(IndentedLineStart(text, settings.closeStart), block);
        }
        return true;
    }

    bool PatchImageInstall(std::wstring& text, int disk, int partition,
        bool& removedAvailable, std::wstring& error)
    {
        XmlElement settings{}, component{};
        if (!LocateWindowsPeSetup(text, settings, component))
        {
            error = L"Cannot locate Microsoft-Windows-Setup in the windowsPE pass.";
            return false;
        }
        const std::wstring nl = NewLineFor(text);

        XmlElement image{};
        if (!FindElement(text, L"ImageInstall", component.openEnd, component.closeStart, image))
        {
            const std::wstring indent = LineIndent(text, component.openStart) + L"\t";
            size_t insertPos = IndentedLineStart(text, component.closeStart);
            XmlElement userData{};
            if (FindElement(text, L"UserData", component.openEnd, component.closeStart, userData))
                insertPos = IndentedLineStart(text, userData.openStart);
            text.insert(insertPos, MakeImageInstall(indent, nl, disk, partition) + nl);
            return true;
        }
        if (image.selfClosing)
        {
            const std::wstring indent = LineIndent(text, image.openStart);
            ReplaceRange(text, image.openStart, image.openEnd, WithoutFirstIndent(MakeImageInstall(indent, nl, disk, partition), indent));
            return true;
        }

        XmlElement os{};
        if (!FindElement(text, L"OSImage", image.openEnd, image.closeStart, os))
        {
            const std::wstring indent = LineIndent(text, image.openStart) + L"\t";
            const std::wstring i1 = indent + L"\t";
            std::wstring block = indent + L"<OSImage>" + nl +
                MakeInstallTo(i1, nl, disk, partition) + nl +
                i1 + L"<WillShowUI>OnError</WillShowUI>" + nl +
                indent + L"</OSImage>" + nl;
            text.insert(IndentedLineStart(text, image.closeStart), block);
            return true;
        }
        if (os.selfClosing)
        {
            const std::wstring indent = LineIndent(text, os.openStart);
            const std::wstring i1 = indent + L"\t";
            std::wstring block = indent + L"<OSImage>" + nl +
                MakeInstallTo(i1, nl, disk, partition) + nl +
                i1 + L"<WillShowUI>OnError</WillShowUI>" + nl +
                indent + L"</OSImage>";
            ReplaceRange(text, os.openStart, os.openEnd, WithoutFirstIndent(block, indent));
            return true;
        }

        // Remove every active InstallToAvailablePartition from OSImage. It conflicts
        // with an explicit InstallTo and would make Windows Setup fail.
        while (true)
        {
            if (!LocateWindowsPeSetup(text, settings, component)) return false;
            if (!FindElement(text, L"ImageInstall", component.openEnd, component.closeStart, image)) return false;
            if (!FindElement(text, L"OSImage", image.openEnd, image.closeStart, os)) return false;
            XmlElement available{};
            if (!FindElement(text, L"InstallToAvailablePartition", os.openEnd, os.closeStart, available)) break;
            ReplaceRange(text, available.openStart, available.selfClosing ? available.openEnd : available.closeEnd, L"");
            removedAvailable = true;
        }

        if (!LocateWindowsPeSetup(text, settings, component)) return false;
        if (!FindElement(text, L"ImageInstall", component.openEnd, component.closeStart, image)) return false;
        if (!FindElement(text, L"OSImage", image.openEnd, image.closeStart, os)) return false;

        XmlElement installTo{};
        if (FindElement(text, L"InstallTo", os.openEnd, os.closeStart, installTo))
        {
            const std::wstring indent = LineIndent(text, installTo.openStart);
            ReplaceRange(text, installTo.openStart,
                installTo.selfClosing ? installTo.openEnd : installTo.closeEnd,
                WithoutFirstIndent(MakeInstallTo(indent, nl, disk, partition), indent));
        }
        else
        {
            const std::wstring indent = LineIndent(text, os.openStart) + L"\t";
            text.insert(IndentedLineStart(text, os.closeStart), MakeInstallTo(indent, nl, disk, partition) + nl);
        }

        if (!LocateWindowsPeSetup(text, settings, component)) return false;
        if (!FindElement(text, L"ImageInstall", component.openEnd, component.closeStart, image)) return false;
        if (!FindElement(text, L"OSImage", image.openEnd, image.closeStart, os)) return false;
        XmlElement show{};
        if (FindElement(text, L"WillShowUI", os.openEnd, os.closeStart, show))
        {
            const std::wstring indent = LineIndent(text, show.openStart);
            ReplaceRange(text, show.openStart, show.selfClosing ? show.openEnd : show.closeEnd,
                L"<WillShowUI>OnError</WillShowUI>");
        }
        else
        {
            const std::wstring indent = LineIndent(text, os.openStart) + L"\t";
            text.insert(IndentedLineStart(text, os.closeStart), indent + L"<WillShowUI>OnError</WillShowUI>" + nl);
        }
        return true;
    }

    bool PatchDiskConfiguration(std::wstring& text, int& removedDiskEntries, std::wstring& error)
    {
        XmlElement settings{}, component{};
        if (!LocateWindowsPeSetup(text, settings, component))
        {
            error = L"Cannot locate Microsoft-Windows-Setup in the windowsPE pass.";
            return false;
        }

        XmlElement diskConfig{};
        if (!FindElement(text, L"DiskConfiguration", component.openEnd, component.closeStart, diskConfig))
            return true;
        if (diskConfig.selfClosing) return true;

        // A pre-existing unattended Disk entry can wipe/repartition the disk before
        // ImageInstall is processed. In /winsetup mode DiskPrep is the disk-layout
        // authority, so active Disk entries are removed from the copied answer file.
        while (true)
        {
            if (!LocateWindowsPeSetup(text, settings, component)) return false;
            if (!FindElement(text, L"DiskConfiguration", component.openEnd, component.closeStart, diskConfig)) break;
            XmlElement disk{};
            if (!FindElement(text, L"Disk", diskConfig.openEnd, diskConfig.closeStart, disk)) break;
            ReplaceRange(text, disk.openStart, disk.selfClosing ? disk.openEnd : disk.closeEnd, L"");
            ++removedDiskEntries;
        }

        if (!LocateWindowsPeSetup(text, settings, component)) return false;
        if (!FindElement(text, L"DiskConfiguration", component.openEnd, component.closeStart, diskConfig)) return true;
        const std::wstring nl = NewLineFor(text);
        XmlElement show{};
        if (FindElement(text, L"WillShowUI", diskConfig.openEnd, diskConfig.closeStart, show))
        {
            const std::wstring indent = LineIndent(text, show.openStart);
            ReplaceRange(text, show.openStart, show.selfClosing ? show.openEnd : show.closeEnd,
                L"<WillShowUI>OnError</WillShowUI>");
        }
        else
        {
            const std::wstring indent = LineIndent(text, diskConfig.openStart) + L"\t";
            text.insert(IndentedLineStart(text, diskConfig.closeStart), indent + L"<WillShowUI>OnError</WillShowUI>" + nl);
        }
        return true;
    }

    bool PatchUnattend(std::wstring& text, int disk, int partition,
        int& removedDiskEntries, bool& removedAvailable, std::wstring& error)
    {
        if (!EnsureWindowsPeSetup(text, disk, partition, error)) return false;
        if (!PatchDiskConfiguration(text, removedDiskEntries, error)) return false;
        if (!PatchImageInstall(text, disk, partition, removedAvailable, error)) return false;
        return true;
    }

    TextFile MakeMinimalUnattend(int disk, int partition)
    {
#ifdef _WIN64
        const wchar_t* arch = L"amd64";
#else
        const wchar_t* arch = L"x86";
#endif
        TextFile f;
        f.encoding = TextEncoding::Utf8Bom;
        f.text =
            L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
            L"<unattend xmlns=\"urn:schemas-microsoft-com:unattend\" xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\">\r\n"
            L"\t<settings pass=\"windowsPE\">\r\n"
            L"\t\t<component name=\"Microsoft-Windows-Setup\" processorArchitecture=\"" + std::wstring(arch) +
            L"\" publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\">\r\n"
            L"\t\t\t<DiskConfiguration>\r\n"
            L"\t\t\t\t<WillShowUI>OnError</WillShowUI>\r\n"
            L"\t\t\t</DiskConfiguration>\r\n"
            L"\t\t\t<ImageInstall>\r\n"
            L"\t\t\t\t<OSImage>\r\n"
            L"\t\t\t\t\t<InstallTo>\r\n"
            L"\t\t\t\t\t\t<DiskID>" + std::to_wstring(disk) + L"</DiskID>\r\n"
            L"\t\t\t\t\t\t<PartitionID>" + std::to_wstring(partition) + L"</PartitionID>\r\n"
            L"\t\t\t\t\t</InstallTo>\r\n"
            L"\t\t\t\t\t<WillShowUI>OnError</WillShowUI>\r\n"
            L"\t\t\t\t</OSImage>\r\n"
            L"\t\t\t</ImageInstall>\r\n"
            L"\t\t\t<UserData>\r\n"
            L"\t\t\t\t<ProductKey>\r\n"
            L"\t\t\t\t\t<Key></Key>\r\n"
            L"\t\t\t\t\t<WillShowUI>Never</WillShowUI>\r\n"
            L"\t\t\t\t</ProductKey>\r\n"
            L"\t\t\t\t<AcceptEula>true</AcceptEula>\r\n"
            L"\t\t\t</UserData>\r\n"
            L"\t\t</component>\r\n"
            L"\t</settings>\r\n"
            L"</unattend>\r\n";
        return f;
    }

    std::wstring OutputDirectory()
    {
        wchar_t drive[16]{};
        DWORD n = GetEnvironmentVariableW(L"SystemDrive", drive, ARRAYSIZE(drive));
        if (n > 0 && n < ARRAYSIZE(drive))
            return std::wstring(drive) + L"\\DiskPrep";

        wchar_t windowsDir[MAX_PATH]{};
        if (GetWindowsDirectoryW(windowsDir, ARRAYSIZE(windowsDir)) >= 2 && windowsDir[1] == L':')
            return std::wstring(windowsDir, 2) + L"\\DiskPrep";
        return L"X:\\DiskPrep";
    }

    bool EnsureDirectory(const std::wstring& dir, std::wstring& error)
    {
        if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
        DWORD e = GetLastError();
        if (e == ERROR_ALREADY_EXISTS)
        {
            DWORD attr = GetFileAttributesW(dir.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;
        }
        error = L"Cannot create DiskPrep working directory:\n" + dir + L"\n\n" + Win32ErrorText(e);
        return false;
    }

    bool GetExistingUnattend(std::wstring& path, bool& pointerMissing, bool& fileMissing)
    {
        path.clear();
        pointerMissing = false;
        fileMissing = false;

        HKEY key = nullptr;
        LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\Setup", 0, KEY_QUERY_VALUE, &key);
        if (rc != ERROR_SUCCESS)
        {
            pointerMissing = true;
            return false;
        }

        DWORD type = 0;
        DWORD bytes = 0;
        rc = RegQueryValueExW(key, L"UnattendFile", nullptr, &type, nullptr, &bytes);
        if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t))
        {
            RegCloseKey(key);
            pointerMissing = true;
            return false;
        }

        std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1u, L'\0');
        rc = RegQueryValueExW(key, L"UnattendFile", nullptr, &type,
            reinterpret_cast<LPBYTE>(value.data()), &bytes);
        RegCloseKey(key);
        if (rc != ERROR_SUCCESS)
        {
            pointerMissing = true;
            return false;
        }

        value.back() = L'\0';
        path = value.data();
        if (type == REG_EXPAND_SZ)
        {
            DWORD need = ExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
            if (need > 0)
            {
                std::vector<wchar_t> expanded(need, L'\0');
                if (ExpandEnvironmentStringsW(path.c_str(), expanded.data(), need)) path = expanded.data();
            }
        }

        DWORD attr = GetFileAttributesW(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
        {
            fileMissing = true;
            return false;
        }
        return true;
    }

    bool SetUnattendPointer(const std::wstring& path, std::wstring& error)
    {
        HKEY key = nullptr;
        DWORD disposition = 0;
        LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\Setup", 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, &disposition);
        if (rc != ERROR_SUCCESS)
        {
            error = L"Cannot open HKLM\\SYSTEM\\Setup for writing.\n\n" + Win32ErrorText(static_cast<DWORD>(rc));
            return false;
        }

        const DWORD bytes = static_cast<DWORD>((path.size() + 1u) * sizeof(wchar_t));
        rc = RegSetValueExW(key, L"UnattendFile", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(path.c_str()), bytes);
        RegCloseKey(key);
        if (rc != ERROR_SUCCESS)
        {
            error = L"Cannot set HKLM\\SYSTEM\\Setup\\UnattendFile.\n\n" + Win32ErrorText(static_cast<DWORD>(rc));
            return false;
        }
        return true;
    }
}

namespace WinSetupIntegration
{
    Result PrepareTarget(const Target& target)
    {
        Result result;
        result.diagnostics.push_back(L"Unattend discovery: checking HKLM\\SYSTEM\\Setup\\UnattendFile.");

        if (target.diskNumber < 0 || target.partitionNumber <= 0)
        {
            result.error = L"Invalid Windows Setup target.";
            return result;
        }

        const std::wstring dir = OutputDirectory();
        result.diagnostics.push_back(L"Working directory: " + dir);
        if (!EnsureDirectory(dir, result.error)) return result;
        result.outputPath = dir + L"\\Unattend.xml";

        bool pointerMissing = false;
        bool fileMissing = false;
        std::wstring existing;
        bool haveExisting = GetExistingUnattend(existing, pointerMissing, fileMissing);

        if (haveExisting)
        {
            result.diagnostics.push_back(L"Registry UnattendFile: " + existing + L" — file exists; using registry source.");
        }
        else if (fileMissing && !existing.empty())
        {
            result.diagnostics.push_back(L"Registry UnattendFile: " + existing +
                L" — file is missing; using the built-in minimal answer file instead.");
        }
        else if (pointerMissing)
        {
            result.diagnostics.push_back(L"Registry UnattendFile: not present or not usable; using the built-in minimal answer file.");
        }

        result.sourcePointerWasMissing = pointerMissing;
        result.sourceFileWasMissing = fileMissing;
        result.sourcePath = existing;

        TextFile file;
        if (haveExisting)
        {
            result.usedExistingUnattend = true;
            if (!ReadTextFile(existing, file, result.error)) return result;
            result.diagnostics.push_back(L"Source answer file loaded: " + existing + L"; encoding " +
                TextEncodingName(file.encoding) + L"; characters " + std::to_wstring(file.text.size()) + L".");
            if (!PatchUnattend(file.text, target.diskNumber, target.partitionNumber,
                result.removedDiskConfigurationEntries,
                result.removedInstallToAvailablePartition, result.error))
                return result;
            result.diagnostics.push_back(L"Answer file patch: success.");
        }
        else
        {
            result.diagnostics.push_back(L"Answer-file source: built-in minimal template.");
            file = MakeMinimalUnattend(target.diskNumber, target.partitionNumber);
#ifdef _WIN64
            result.diagnostics.push_back(L"Minimal answer file architecture: amd64.");
#else
            result.diagnostics.push_back(L"Minimal answer file architecture: x86.");
#endif
        }

        // The registry pointer is deliberately updated only after a complete patched
        // file has been written successfully. A failed patch can never redirect Setup
        // to a partial file.
        result.diagnostics.push_back(L"Writing answer file atomically: " + result.outputPath +
            L"; encoding " + TextEncodingName(file.encoding) + L".");
        if (!WriteTextFileAtomic(result.outputPath, file, result.error)) return result;
        result.diagnostics.push_back(L"Answer file write: success.");

        result.diagnostics.push_back(L"Updating HKLM\\SYSTEM\\Setup\\UnattendFile -> " + result.outputPath);
        if (!SetUnattendPointer(result.outputPath, result.error)) return result;
        result.diagnostics.push_back(L"Registry pointer update: success.");

        result.ok = true;
        return result;
    }
}
