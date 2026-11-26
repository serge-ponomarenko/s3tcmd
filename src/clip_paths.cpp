#include "clip_paths.h"

#include <windows.h>

#include <cwchar>

#include "../third_party/nlohmann/json.hpp"
#include "util.h"
#include "vfs_path.h"

namespace s3tc {

std::vector<std::wstring> ParseListFile(const std::string& bytes) {
    std::wstring text;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        text.assign(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / 2);
    } else if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        text = Utf8ToWide(bytes.substr(3));
    } else {
        text = Utf8ToWide(bytes);
    }
    std::vector<std::wstring> lines;
    std::wstring cur;
    auto flush = [&]() {
        if (!cur.empty()) lines.push_back(cur);
        cur.clear();
    };
    for (wchar_t c : text) {
        if (c == L'\r' || c == L'\n') {
            flush();
        } else {
            cur.push_back(c);
        }
    }
    flush();
    return lines;
}

bool ParsePluginPath(const std::wstring& line, const std::wstring& rootName, S3Ref* out) {
    const std::wstring prefix = L"\\\\\\" + rootName + L"\\";
    if (line.size() <= prefix.size() || _wcsnicmp(line.c_str(), prefix.c_str(), prefix.size()) != 0)
        return false;
    std::wstring rest = line.substr(prefix.size() - 1);  // keep the leading backslash
    VfsPath p = ParseVfsPath(rest);
    if (p.level != VfsPath::Level::Bucket && p.level != VfsPath::Level::Inside) return false;
    out->profile = p.profile;
    out->bucket = p.bucket;
    out->key = p.key;
    out->isDir = p.key.empty() || line.back() == L'\\';
    return true;
}

std::wstring S3Uri(const S3Ref& ref) {
    std::wstring uri = L"s3://" + ref.bucket + L"/" + ref.key;
    if (ref.isDir && !ref.key.empty()) uri.push_back(L'/');
    return uri;
}

std::wstring ConvertLineToUri(const std::wstring& line, const std::wstring& rootName) {
    S3Ref ref;
    return ParsePluginPath(line, rootName, &ref) ? S3Uri(ref) : line;
}

std::wstring PickPluginRootName(const std::vector<std::pair<std::wstring, std::wstring>>& entries,
                                const std::wstring& pluginFileName) {
    for (const auto& e : entries) {
        const std::wstring& path = e.second;
        size_t slash = path.find_last_of(L"\\/");
        std::wstring file = slash == std::wstring::npos ? path : path.substr(slash + 1);
        if (!e.first.empty() && _wcsicmp(file.c_str(), pluginFileName.c_str()) == 0) return e.first;
    }
    return L"S3";
}

std::wstring ReadPluginRootName(const std::wstring& wincmdIni, const std::wstring& pluginFileName) {
    if (wincmdIni.empty()) return L"S3";
    std::vector<wchar_t> buf(32768);
    DWORD n = GetPrivateProfileSectionW(L"FileSystemPlugins", buf.data(), static_cast<DWORD>(buf.size()),
                                        wincmdIni.c_str());
    std::vector<std::pair<std::wstring, std::wstring>> entries;
    for (const wchar_t* p = buf.data(); p < buf.data() + n && *p; p += wcslen(p) + 1) {
        std::wstring line = p;
        size_t eq = line.find(L'=');
        if (eq != std::wstring::npos) entries.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
    return PickPluginRootName(entries, pluginFileName);
}

std::wstring CwdFromArgs(const std::vector<std::wstring>& args) {
    std::wstring s;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) s.push_back(L' ');
        s += args[i];
    }
    std::wstring out;
    for (wchar_t c : s) {
        if (c != L'"') out.push_back(c);
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    if (!out.empty() && out.back() != L'\\') out.push_back(L'\\');
    return out;
}

std::wstring RegionFromLocationJson(const std::string& json) {
    auto j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("LocationConstraint")) return L"";
    const auto& v = j["LocationConstraint"];
    if (v.is_null()) return L"us-east-1";
    if (!v.is_string()) return L"";
    std::string s = v.get<std::string>();
    if (s.empty()) return L"us-east-1";
    if (s == "EU") return L"eu-west-1";
    return Utf8ToWide(s);
}

}  // namespace s3tc
