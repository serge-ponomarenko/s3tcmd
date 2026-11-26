#include "config.h"

#include <windows.h>

#include "util.h"

namespace s3tc {

namespace {

const wchar_t kSection[] = L"General";

std::wstring ReadString(const std::wstring& ini, const wchar_t* key, const std::wstring& def) {
    wchar_t probe[8];
    // Detect a missing key by using a sentinel default, then persist the real default.
    DWORD n = GetPrivateProfileStringW(kSection, key, L"\x01", probe, _countof(probe), ini.c_str());
    if (n == 1 && probe[0] == L'\x01') {
        WritePrivateProfileStringW(kSection, key, def.c_str(), ini.c_str());
        return def;
    }
    wchar_t buf[2048];
    GetPrivateProfileStringW(kSection, key, def.c_str(), buf, _countof(buf), ini.c_str());
    return buf;
}

bool ParseBool(const std::wstring& v) {
    return v == L"1" || _wcsicmp(v.c_str(), L"true") == 0 || _wcsicmp(v.c_str(), L"yes") == 0;
}

bool ReadBool(const std::wstring& ini, const wchar_t* key, bool def) {
    return ParseBool(ReadString(ini, key, def ? L"1" : L"0"));
}

int ReadInt(const std::wstring& ini, const wchar_t* key, int def) {
    std::wstring v = ReadString(ini, key, std::to_wstring(def));
    wchar_t* end = nullptr;
    long n = wcstol(v.c_str(), &end, 10);
    return (end && end != v.c_str()) ? static_cast<int>(n) : def;
}

std::wstring ExpandEnv(const std::wstring& s) {
    if (s.find(L'%') == std::wstring::npos) return s;
    wchar_t buf[2048];
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, _countof(buf));
    return (n > 0 && n <= _countof(buf)) ? std::wstring(buf) : s;
}

std::vector<std::wstring> SplitList(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    auto flush = [&]() {
        size_t b = cur.find_first_not_of(L" \t");
        size_t e = cur.find_last_not_of(L" \t");
        if (b != std::wstring::npos) out.push_back(cur.substr(b, e - b + 1));
        cur.clear();
    };
    for (wchar_t c : s) {
        if (c == L',' || c == L';') {
            flush();
        } else {
            cur.push_back(c);
        }
    }
    flush();
    return out;
}

}  // namespace

Config LoadConfig(const std::wstring& iniPath) {
    Config c;
    c.iniPath = iniPath;
    if (iniPath.empty()) return c;
    c.awsExe = ExpandEnv(ReadString(iniPath, L"AwsExe", c.awsExe));
    c.logFile = ExpandEnv(ReadString(iniPath, L"LogFile", L""));
    int ttl = ReadInt(iniPath, L"CacheTtlSec", 0);
    c.cacheTtlSec = ttl > 0 ? static_cast<unsigned>(ttl) : 0;
    c.refreshHeuristic = ReadBool(iniPath, L"RefreshHeuristic", true);
    c.maxListItems = ReadInt(iniPath, L"MaxListItems", c.maxListItems);
    c.prefetchMaxKeys = ReadInt(iniPath, L"PrefetchMaxKeys", c.prefetchMaxKeys);
    c.batchDownloads = ReadBool(iniPath, L"BatchDownloads", true);
    c.disableImds = ReadBool(iniPath, L"DisableImds", true);
    c.readOnly = ReadBool(iniPath, L"ReadOnly", false);
    c.diskCache = ReadBool(iniPath, L"DiskCache", true);
    c.diskCacheDir = ExpandEnv(ReadString(iniPath, L"DiskCacheDir", L"%LOCALAPPDATA%\\S3Tcmd\\cache"));
    c.diskCacheMaxAgeDays = ReadInt(iniPath, L"DiskCacheMaxAgeDays", c.diskCacheMaxAgeDays);
    c.presignExpirySec = ReadInt(iniPath, L"PresignExpirySec", c.presignExpirySec);
    return c;
}

ProfileConfig LoadProfileConfig(const std::wstring& iniPath, const std::wstring& profile) {
    ProfileConfig p;
    if (iniPath.empty()) return p;
    std::wstring section = L"Profile:" + profile;
    wchar_t buf[4096];
    GetPrivateProfileStringW(section.c_str(), L"PinnedBuckets", L"", buf, _countof(buf), iniPath.c_str());
    p.pinnedBuckets = SplitList(buf);
    GetPrivateProfileStringW(section.c_str(), L"Hide", L"0", buf, _countof(buf), iniPath.c_str());
    p.hidden = ParseBool(buf);
    GetPrivateProfileStringW(section.c_str(), L"ReadOnly", L"0", buf, _countof(buf), iniPath.c_str());
    p.readOnly = ParseBool(buf);
    return p;
}

}  // namespace s3tc
