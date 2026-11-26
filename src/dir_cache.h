// In-memory directory cache (Layer A) and Ctrl+R detection.
#pragma once

#include <windows.h>

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace s3tc {

// Compact directory entry (~80 bytes instead of 592 for WIN32_FIND_DATAW): tree listings can
// hold hundreds of thousands of them. Field names follow WIN32_FIND_DATAW.
struct CacheEntry {
    std::wstring cFileName;  // display name (EncodeName applied)
    DWORD dwFileAttributes = 0;
    unsigned long long size = 0;
    FILETIME ftLastWriteTime{};  // 0xFFFFFFFF/0xFFFFFFFE = "no time" (TC convention)
};

// Fills a zeroed WIN32_FIND_DATAW for TC (names longer than MAX_PATH-1 are truncated).
void ToFindData(const CacheEntry& e, WIN32_FIND_DATAW* fd);

// Current wall-clock time in milliseconds (for cache ages that survive restarts).
ULONGLONG NowMs();

struct CachedDir {
    std::vector<CacheEntry> items;
    ULONGLONG listedAt = 0;        // NowMs() at listing time
    bool subtreeComplete = false;  // all descendants are cached too (from a tree listing)
};

// Thread-safe map from normalized plugin path to its listing. Keys are case-sensitive,
// because S3 keys are.
class DirCache {
public:
    // Returns nullptr if not cached or older than ttlMs (ttlMs == 0: never expires).
    std::shared_ptr<const CachedDir> Get(const std::wstring& path, ULONGLONG ttlMs) const;
    void Put(const std::wstring& path, std::vector<CacheEntry> items, bool subtreeComplete = false);
    void Invalidate(const std::wstring& path);
    void InvalidateTree(const std::wstring& path);  // path and everything below it
    void Clear();
    size_t Size() const;

    // ---- support for the disk cache ----
    // Persistence group of a path: "\" for the root and profile level (profile and bucket lists),
    // "\profile\bucket" for everything inside a bucket.
    static std::wstring GroupOf(const std::wstring& path);
    // All cached directories of a group.
    std::vector<std::pair<std::wstring, std::shared_ptr<const CachedDir>>> ExportGroup(const std::wstring& group) const;
    // Adds a directory loaded from disk unless a (fresher) in-memory listing exists.
    void Import(const std::wstring& path, std::shared_ptr<const CachedDir> dir);
    // Groups modified since the last call.
    std::vector<std::wstring> TakeDirtyGroups();

private:
    void MarkDirty(const std::wstring& path);  // caller holds mutex_
    mutable std::mutex mutex_;
    std::map<std::wstring, std::shared_ptr<const CachedDir>> dirs_;
    std::set<std::wstring> dirty_;
};

// Detects Ctrl+R: TC re-lists the same path, and no other panel listing happens in between
// (measured in TC). Only panel listings (inside FS_STATUS_OP_LIST) are fed in.
class RefreshTracker {
public:
    // Returns true if this panel listing of `path` is a refresh request.
    // With useHeuristic == false only ForceNext() requests count.
    bool OnPanelListing(const std::wstring& path, bool useHeuristic);
    // Forces the next listing of `path` to bypass the cache (used by the "refresh" command).
    void ForceNext(const std::wstring& path);
    // The next listing of `path` is not a refresh even if it repeats (TC re-lists the panel after
    // a command-line command that returned FS_EXEC_OK).
    void SuppressNext(const std::wstring& path);

private:
    std::mutex mutex_;
    std::wstring last_;
    std::wstring forced_;
    std::wstring suppressed_;
};

}  // namespace s3tc
