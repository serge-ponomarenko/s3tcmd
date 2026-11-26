// Persistent directory cache: keeps DirCache listings on disk so a TC restart does not lose them
// (in particular the large trees fetched with Space / `prefetch`).
//
// One file per DirCache group (see DirCache::GroupOf): the profile/bucket lists, and one per
// bucket of a profile. Files are UTF-8 text:
//   S3TCMD-CACHE<TAB>1<TAB><group>
//   D<TAB><listedAtMs><TAB><subtreeComplete 0|1><TAB><plugin path>
//   E<TAB><attributes hex><TAB><size><TAB><mtime high><TAB><mtime low><TAB><display name>
// Display names never contain TAB/CR/LF (control characters are encoded by EncodeName).
#pragma once

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "dir_cache.h"

namespace s3tc {

class DiskCache {
public:
    // dir: cache folder (created on demand). maxAgeMs: listings older than this are not loaded
    // (0 = no limit). enabled = false turns every call into a no-op.
    void Configure(bool enabled, const std::wstring& dir, ULONGLONG maxAgeMs);
    bool Enabled() const;

    // Loads the group containing `path` into `cache`, once per group and session.
    void EnsureLoaded(const std::wstring& path, DirCache* cache);

    // Writes groups modified since the last save. Unless `force`, at most once per `minIntervalMs`.
    void SaveDirty(DirCache* cache, bool force, ULONGLONG minIntervalMs = 15000);

    // Deletes all cache files (refresh-all).
    void DeleteAll();

    // ---- exposed for tests ----
    static std::string Serialize(const std::wstring& group,
                                 const std::vector<std::pair<std::wstring, std::shared_ptr<const CachedDir>>>& dirs);
    // Parses a file; skips directories older than maxAgeMs (0 = keep all). False if malformed.
    static bool Parse(const std::string& text, std::wstring* group,
                      std::vector<std::pair<std::wstring, std::shared_ptr<const CachedDir>>>* dirs,
                      ULONGLONG maxAgeMs);
    std::wstring FileFor(const std::wstring& group) const;

private:
    mutable std::mutex mutex_;
    std::mutex saveMutex_;  // one SaveDirty at a time (background transfer threads)
    bool enabled_ = false;
    std::wstring dir_;
    ULONGLONG maxAgeMs_ = 0;
    std::set<std::wstring> loaded_;
    std::set<std::wstring> pending_;  // dirty groups not yet written (debounced)
    ULONGLONG lastSave_ = 0;
};

}  // namespace s3tc
