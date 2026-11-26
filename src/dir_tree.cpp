#include "dir_tree.h"

#include <set>

namespace s3tc {

std::map<std::wstring, TreeDir> BuildDirectoryTree(const std::vector<S3Entry>& objects, bool truncated) {
    std::map<std::wstring, TreeDir> dirs;
    std::set<std::wstring> known;  // directory keys whose entry was added to their parent
    dirs[L""];

    for (const auto& obj : objects) {
        const std::wstring& key = obj.name;
        if (key.empty()) continue;  // the folder marker of the subtree root itself
        // Directory keys are key prefixes ending in '/': unambiguous even for empty segments
        // ("/x" lives in directory "/", which is an empty-named folder, not the root).
        std::wstring parent;
        size_t start = 0;
        for (;;) {
            size_t slash = key.find(L'/', start);
            if (slash == std::wstring::npos) {
                S3Entry file = obj;
                file.name = key.substr(start);
                file.isDir = false;
                dirs[parent].entries.push_back(file);
                break;
            }
            std::wstring dir = key.substr(0, slash + 1);
            if (known.insert(dir).second) {
                S3Entry e;
                e.name = key.substr(start, slash - start);
                e.isDir = true;
                dirs[parent].entries.push_back(e);
                dirs[dir];  // exists even if it ends up empty
            }
            parent = dir;
            start = slash + 1;
            if (start == key.size()) break;  // "a/b/" is a folder marker: the directory alone
        }
    }

    if (truncated) {
        // Keys come sorted, so only directories containing the last key can have missing items.
        dirs[L""].complete = false;
        if (!objects.empty()) {
            const std::wstring& last = objects.back().name;
            for (auto& [rel, dir] : dirs) {
                if (!rel.empty() && last.compare(0, rel.size(), rel) == 0) dir.complete = false;
            }
        }
    }
    return dirs;
}

std::vector<std::wstring> SplitDirKey(const std::wstring& rel) {
    std::vector<std::wstring> segments;
    size_t start = 0;
    while (start < rel.size()) {
        size_t slash = rel.find(L'/', start);
        if (slash == std::wstring::npos) slash = rel.size();
        segments.push_back(rel.substr(start, slash - start));
        start = slash + 1;
    }
    return segments;
}

}  // namespace s3tc
