#include "vfs_path.h"

#include <vector>

#include "names.h"

namespace s3tc {

std::wstring NormalizePath(const std::wstring& path) {
    std::wstring p = path.empty() ? L"\\" : path;
    if (p[0] != L'\\') p.insert(p.begin(), L'\\');
    while (p.size() > 1 && p.back() == L'\\') p.pop_back();
    return p;
}

VfsPath ParseVfsPath(const std::wstring& path) {
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (wchar_t c : NormalizePath(path)) {
        if (c == L'\\') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);

    VfsPath r;
    if (parts.empty()) return r;
    r.level = VfsPath::Level::Profile;
    r.profile = DecodeName(parts[0]);
    if (parts.size() == 1) return r;
    r.level = VfsPath::Level::Bucket;
    r.bucket = DecodeName(parts[1]);
    if (parts.size() == 2) return r;
    r.level = VfsPath::Level::Inside;
    for (size_t i = 2; i < parts.size(); ++i) {
        if (i > 2) r.key.push_back(L'/');
        r.key += DecodeName(parts[i]);
    }
    return r;
}

std::wstring ListPrefix(const VfsPath& p) { return p.key.empty() ? std::wstring() : p.key + L"/"; }

}  // namespace s3tc
