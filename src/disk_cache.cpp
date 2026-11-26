#include "disk_cache.h"

#include <windows.h>

#include <cstdio>

#include "log.h"
#include "util.h"

namespace s3tc {

namespace {

const char kMagic[] = "S3TCMD-CACHE";

unsigned long long Fnv1a(const std::wstring& s) {
    unsigned long long h = 1469598103934665603ULL;
    for (wchar_t c : s) {
        h ^= static_cast<unsigned long long>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

std::vector<std::string> SplitTabs(const std::string& line, size_t maxParts) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (parts.size() + 1 < maxParts) {
        size_t tab = line.find('\t', start);
        if (tab == std::string::npos) break;
        parts.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    parts.push_back(line.substr(start));  // the last field may contain anything but CR/LF
    return parts;
}

bool ReadFileUtf8(const std::wstring& path, std::string* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 1024LL * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    BOOL ok = out->empty() || ReadFile(h, out->data(), static_cast<DWORD>(out->size()), &read, nullptr);
    CloseHandle(h);
    return ok && read == out->size();
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(h);
    if (!ok || written != data.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

void CreateDirectories(const std::wstring& dir) {
    for (size_t pos = dir.find(L'\\', 3); ; pos = dir.find(L'\\', pos + 1)) {
        CreateDirectoryW(dir.substr(0, pos).c_str(), nullptr);
        if (pos == std::wstring::npos) break;
    }
}

}  // namespace

void DiskCache::Configure(bool enabled, const std::wstring& dir, ULONGLONG maxAgeMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dir != dir_) loaded_.clear();
    enabled_ = enabled && !dir.empty();
    dir_ = dir;
    maxAgeMs_ = maxAgeMs;
}

bool DiskCache::Enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

std::wstring DiskCache::FileFor(const std::wstring& group) const {
    wchar_t name[32];
    swprintf_s(name, L"%016llx.cache", Fnv1a(group));
    return dir_ + L"\\" + name;
}

std::string DiskCache::Serialize(const std::wstring& group,
                                 const std::vector<std::pair<std::wstring, std::shared_ptr<const CachedDir>>>& dirs) {
    std::string out;
    out.reserve(64 * 1024);
    out += kMagic;
    out += "\t1\t" + WideToUtf8(group) + "\n";
    char buf[96];
    for (const auto& [path, dir] : dirs) {
        sprintf_s(buf, "D\t%llu\t%d\t", dir->listedAt, dir->subtreeComplete ? 1 : 0);
        out += buf;
        out += WideToUtf8(path) + "\n";
        for (const auto& e : dir->items) {
            sprintf_s(buf, "E\t%lx\t%llu\t%lu\t%lu\t", e.dwFileAttributes, e.size, e.ftLastWriteTime.dwHighDateTime,
                      e.ftLastWriteTime.dwLowDateTime);
            out += buf;
            out += WideToUtf8(e.cFileName) + "\n";
        }
    }
    return out;
}

bool DiskCache::Parse(const std::string& text, std::wstring* group,
                      std::vector<std::pair<std::wstring, std::shared_ptr<const CachedDir>>>* dirs, ULONGLONG maxAgeMs) {
    dirs->clear();
    size_t pos = 0;
    auto nextLine = [&](std::string* line) {
        if (pos >= text.size()) return false;
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        *line = text.substr(pos, nl - pos);
        if (!line->empty() && line->back() == '\r') line->pop_back();
        pos = nl + 1;
        return true;
    };
    std::string line;
    if (!nextLine(&line)) return false;
    auto header = SplitTabs(line, 3);
    if (header.size() != 3 || header[0] != kMagic || header[1] != "1") return false;
    *group = Utf8ToWide(header[2]);

    const ULONGLONG now = NowMs();
    std::shared_ptr<CachedDir> cur;
    bool skip = false;
    while (nextLine(&line)) {
        if (line.empty()) continue;
        if (line[0] == 'D') {
            auto f = SplitTabs(line, 4);
            if (f.size() != 4) return false;
            cur = std::make_shared<CachedDir>();
            cur->listedAt = std::strtoull(f[1].c_str(), nullptr, 10);
            cur->subtreeComplete = f[2] == "1";
            skip = maxAgeMs > 0 && now > cur->listedAt && now - cur->listedAt > maxAgeMs;
            if (!skip) dirs->emplace_back(Utf8ToWide(f[3]), cur);
        } else if (line[0] == 'E') {
            if (!cur) return false;
            if (skip) continue;
            auto f = SplitTabs(line, 6);
            if (f.size() != 6) return false;
            CacheEntry e;
            e.dwFileAttributes = static_cast<DWORD>(std::strtoul(f[1].c_str(), nullptr, 16));
            e.size = std::strtoull(f[2].c_str(), nullptr, 10);
            e.ftLastWriteTime.dwHighDateTime = static_cast<DWORD>(std::strtoul(f[3].c_str(), nullptr, 10));
            e.ftLastWriteTime.dwLowDateTime = static_cast<DWORD>(std::strtoul(f[4].c_str(), nullptr, 10));
            e.cFileName = Utf8ToWide(f[5]);
            cur->items.push_back(std::move(e));
        } else {
            return false;
        }
    }
    return true;
}

void DiskCache::EnsureLoaded(const std::wstring& path, DirCache* cache) {
    const std::wstring group = DirCache::GroupOf(path);
    std::wstring file;
    ULONGLONG maxAge = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_ || !loaded_.insert(group).second) return;
        file = FileFor(group);
        maxAge = maxAgeMs_;
    }
    std::string text;
    if (!ReadFileUtf8(file, &text)) return;
    const ULONGLONG t0 = GetTickCount64();
    std::wstring fileGroup;
    std::vector<std::pair<std::wstring, std::shared_ptr<const CachedDir>>> dirs;
    if (!Parse(text, &fileGroup, &dirs, maxAge) || fileGroup != group) {
        Log(L"disk cache: ignoring unreadable %s", file.c_str());
        return;
    }
    size_t entries = 0;
    for (auto& [p, d] : dirs) {
        entries += d->items.size();
        cache->Import(p, std::move(d));
    }
    // Importing marked nothing dirty (Import does not), so nothing is rewritten needlessly.
    Log(L"disk cache: loaded %s: %zu dirs, %zu entries (%zu KB) in %llu ms", group.c_str(), dirs.size(), entries,
        text.size() / 1024, GetTickCount64() - t0);
}

void DiskCache::SaveDirty(DirCache* cache, bool force, ULONGLONG minIntervalMs) {
    std::lock_guard<std::mutex> saveLock(saveMutex_);
    std::vector<std::wstring> groups = cache->TakeDirtyGroups();
    std::wstring dir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) return;
        pending_.insert(groups.begin(), groups.end());
        if (pending_.empty()) return;
        if (!force && GetTickCount64() - lastSave_ < minIntervalMs) return;
        groups.assign(pending_.begin(), pending_.end());
        pending_.clear();
        lastSave_ = GetTickCount64();
        dir = dir_;
    }
    CreateDirectories(dir);
    for (const auto& group : groups) {
        // A group that was never loaded from disk must not overwrite its file with a partial view:
        // load it first (normally a no-op, because every listing loads its group before use).
        EnsureLoaded(group, cache);
        auto dirs = cache->ExportGroup(group);
        const std::wstring file = FileFor(group);
        if (dirs.empty()) {
            DeleteFileW(file.c_str());
            continue;
        }
        const ULONGLONG t0 = GetTickCount64();
        std::string text = Serialize(group, dirs);
        if (!WriteFileAtomic(file, text)) {
            Log(L"disk cache: cannot write %s (error %lu)", file.c_str(), GetLastError());
            continue;
        }
        Log(L"disk cache: saved %s: %zu dirs (%zu KB) in %llu ms", group.c_str(), dirs.size(), text.size() / 1024,
            GetTickCount64() - t0);
    }
    cache->TakeDirtyGroups();  // EnsureLoaded() above may not mark anything, but stay clean
}

void DiskCache::DeleteAll() {
    std::wstring dir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dir = dir_;
        pending_.clear();
    }
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.cache").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    while (FindNextFileW(h, &fd));
    FindClose(h);
}

}  // namespace s3tc
