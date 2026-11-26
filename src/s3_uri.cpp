#include "s3_uri.h"

#include <cwchar>
#include <cwctype>

#include "names.h"
#include "util.h"

namespace s3tc {

namespace {

bool IsSep(wchar_t c) { return c == L'/' || c == L'\\'; }

}  // namespace

bool ParseS3Uri(const std::wstring& text, S3Target* out) {
    size_t b = 0, e = text.size();
    while (b < e && (iswspace(text[b]) || text[b] == L'"')) ++b;
    while (e > b && (iswspace(text[e - 1]) || text[e - 1] == L'"')) --e;
    std::wstring s = text.substr(b, e - b);
    if (s.size() < 3 || _wcsnicmp(s.c_str(), L"s3:", 3) != 0) return false;
    size_t i = 3;
    while (i < s.size() && IsSep(s[i])) ++i;
    if (i == 3) return false;  // "s3:bucket" is not a URI
    std::wstring rest = s.substr(i);
    size_t slash = rest.find(L'/');
    std::wstring bucket = rest.substr(0, slash);
    if (bucket.empty() || bucket.find(L'\\') != std::wstring::npos) return false;
    std::wstring key = slash == std::wstring::npos ? std::wstring() : rest.substr(slash + 1);
    S3Target t;
    t.bucket = bucket;
    t.isDir = key.empty() || key.back() == L'/';
    if (!key.empty() && key.back() == L'/') key.pop_back();
    t.key = key;
    *out = t;
    return true;
}

bool ParseS3AliasPath(const std::wstring& path, S3Target* out) {
    std::wstring p = path;
    while (!p.empty() && p.front() == L'\\') p.erase(p.begin());
    // Find the first segment that starts with "s3:".
    size_t start = 0;
    int index = 0;
    while (_wcsnicmp(p.c_str() + start, L"s3:", 3) != 0) {
        size_t sep = p.find(L'\\', start);
        if (sep == std::wstring::npos) return false;
        start = sep + 1;
        ++index;
    }
    std::wstring firstSegment = index > 0 ? DecodeName(p.substr(0, p.find(L'\\'))) : std::wstring();
    p = p.substr(start);
    // In a plugin path an ASCII backslash is always a separator (a real '\' in a key is displayed
    // as U+FF3C), so a URI that TC turned into "s3:\\b\k" (e.g. after "cd") still parses.
    for (auto& c : p) {
        if (c == L'\\') c = L'/';
    }
    S3Target t;
    if (!ParseS3Uri(p, &t)) return false;  // a trailing backslash became '/': a folder
    if (index == 1) t.profile = firstSegment;
    if (index >= 2) t.hintProfile = firstSegment;
    *out = t;
    return true;
}

std::wstring ChooseProfileForBucket(const std::vector<ProfileBucketInfo>& profiles,
                                    const std::wstring& preferred) {
    for (const auto& p : profiles) {
        if (p.hasBucket && !preferred.empty() && p.profile == preferred) return p.profile;
    }
    for (const auto& p : profiles) {
        if (p.hasBucket && !p.pinned) return p.profile;
    }
    for (const auto& p : profiles) {
        if (p.hasBucket) return p.profile;
    }
    return L"";
}

std::string BuildTcCdCommand(const std::wstring& tcPath) {
    std::string data = "\xEF\xBB\xBF" + WideToUtf8(tcPath);
    data += '\r';
    data += '\0';
    data += 'S';
    data += '\0';
    return data;
}

}  // namespace s3tc
