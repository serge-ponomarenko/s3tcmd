#include "dir_cache.h"

namespace s3tc {

ULONGLONG NowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000;
}

void ToFindData(const CacheEntry& e, WIN32_FIND_DATAW* fd) {
    memset(fd, 0, sizeof(*fd));
    fd->dwFileAttributes = e.dwFileAttributes;
    fd->nFileSizeHigh = static_cast<DWORD>(e.size >> 32);
    fd->nFileSizeLow = static_cast<DWORD>(e.size & 0xFFFFFFFF);
    fd->ftLastWriteTime = e.ftLastWriteTime;
    wcsncpy_s(fd->cFileName, e.cFileName.c_str(), _TRUNCATE);
}

std::shared_ptr<const CachedDir> DirCache::Get(const std::wstring& path, ULONGLONG ttlMs) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = dirs_.find(path);
    if (it == dirs_.end()) return nullptr;
    if (ttlMs > 0 && NowMs() - it->second->listedAt > ttlMs) return nullptr;
    return it->second;
}

void DirCache::Put(const std::wstring& path, std::vector<CacheEntry> items, bool subtreeComplete) {
    auto dir = std::make_shared<CachedDir>();
    dir->items = std::move(items);
    dir->listedAt = NowMs();
    dir->subtreeComplete = subtreeComplete;
    std::lock_guard<std::mutex> lock(mutex_);
    dirs_[path] = std::move(dir);
    MarkDirty(path);
}

void DirCache::Invalidate(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dirs_.erase(path)) MarkDirty(path);
}

void DirCache::InvalidateTree(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::wstring childPrefix = path == L"\\" ? path : path + L"\\";
    for (auto it = dirs_.begin(); it != dirs_.end();) {
        if (it->first == path || it->first.compare(0, childPrefix.size(), childPrefix) == 0) {
            MarkDirty(it->first);
            it = dirs_.erase(it);
        } else {
            ++it;
        }
    }
}

void DirCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [path, dir] : dirs_) MarkDirty(path);
    dirs_.clear();
}

size_t DirCache::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dirs_.size();
}

std::wstring DirCache::GroupOf(const std::wstring& path) {
    // "\profile\bucket[\...]" -> "\profile\bucket"; "\" and "\profile" -> "\".
    size_t second = path.find(L'\\', 1);
    if (second == std::wstring::npos) return L"\\";
    size_t third = path.find(L'\\', second + 1);
    return third == std::wstring::npos ? path : path.substr(0, third);
}

std::vector<std::pair<std::wstring, std::shared_ptr<const CachedDir>>> DirCache::ExportGroup(
    const std::wstring& group) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::wstring, std::shared_ptr<const CachedDir>>> out;
    if (group == L"\\") {
        for (const auto& [path, dir] : dirs_) {
            if (GroupOf(path) == L"\\") out.emplace_back(path, dir);
        }
        return out;
    }
    const std::wstring childPrefix = group + L"\\";
    for (auto it = dirs_.lower_bound(group); it != dirs_.end(); ++it) {
        if (it->first != group && it->first.compare(0, childPrefix.size(), childPrefix) != 0) {
            // std::map order: "\p\b" < "\p\b\..." < "\p\b!" ...; stop at the first unrelated key
            // after the group's range has started.
            if (it->first.compare(0, group.size(), group) != 0) break;
            continue;
        }
        out.emplace_back(it->first, it->second);
    }
    return out;
}

void DirCache::Import(const std::wstring& path, std::shared_ptr<const CachedDir> dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    dirs_.emplace(path, std::move(dir));  // keeps an existing in-memory entry
}

std::vector<std::wstring> DirCache::TakeDirtyGroups() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::wstring> out(dirty_.begin(), dirty_.end());
    dirty_.clear();
    return out;
}

void DirCache::MarkDirty(const std::wstring& path) { dirty_.insert(GroupOf(path)); }


bool RefreshTracker::OnPanelListing(const std::wstring& path, bool useHeuristic) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (path == forced_) {
        forced_.clear();
        suppressed_.clear();
        last_ = path;
        return true;
    }
    bool refresh = useHeuristic && path == last_ && path != suppressed_;
    suppressed_.clear();
    last_ = path;
    return refresh;
}

void RefreshTracker::SuppressNext(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    suppressed_ = path;
}

void RefreshTracker::ForceNext(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    forced_ = path;
}

}  // namespace s3tc
