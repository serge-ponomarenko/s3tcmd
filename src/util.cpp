#include "util.h"

#include <windows.h>

namespace s3tc {

namespace {

std::wstring MultiByteToWide(UINT codePage, const char* data, int len) {
    if (len <= 0) return {};
    int n = MultiByteToWideChar(codePage, 0, data, len, nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(codePage, 0, data, len, out.data(), n);
    return out;
}

std::string WideToMultiByte(UINT codePage, const wchar_t* data, int len) {
    if (len <= 0) return {};
    int n = WideCharToMultiByte(codePage, 0, data, len, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(codePage, 0, data, len, out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace

std::wstring Utf8ToWide(const std::string& s) {
    return MultiByteToWide(CP_UTF8, s.data(), static_cast<int>(s.size()));
}

std::string WideToUtf8(const std::wstring& s) {
    return WideToMultiByte(CP_UTF8, s.data(), static_cast<int>(s.size()));
}

std::wstring AnsiToWide(const char* s) {
    if (!s) return {};
    return MultiByteToWide(CP_ACP, s, static_cast<int>(strlen(s)));
}

std::string WideToAnsi(const std::wstring& s) {
    return WideToMultiByte(CP_ACP, s.data(), static_cast<int>(s.size()));
}

std::wstring QuoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            // Double the trailing backslashes so the closing quote is not escaped.
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back(L'"');
    return out;
}

std::wstring BuildCommandLine(const std::wstring& program, const std::vector<std::wstring>& args) {
    // The program name is parsed differently (no backslash escaping), so plain quoting is enough.
    std::wstring cmd = L"\"" + program + L"\"";
    for (const auto& a : args) {
        cmd.push_back(L' ');
        cmd += QuoteArg(a);
    }
    return cmd;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n' || c == '\r') {
            if (!cur.empty()) lines.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) lines.push_back(std::move(cur));
    return lines;
}

std::wstring DirectoryOf(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos + 1);
}

}  // namespace s3tc
