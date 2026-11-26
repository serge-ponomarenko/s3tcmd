// Exported WFX entry points. Keep this layer thin: translate TC calls into calls to the modules.
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../sdk/fsplugin_cpp.h"
#include "aws_cli.h"
#include "aws_s3.h"
#include "clip_paths.h"
#include "config.h"
#include "dir_cache.h"
#include "dir_tree.h"
#include "disk_cache.h"
#include "errors.h"
#include "log.h"
#include "names.h"
#include "progress_window.h"
#include "s3_uri.h"
#include "setup.h"
#include "transfer.h"
#include "util.h"
#include "version.h"
#include "vfs_path.h"

using namespace s3tc;

namespace {

const wchar_t kTitle[] = L"S3 (AWS CLI)";

// Tree listings run in chunks of this many keys (one aws.exe call each, ~15 s): progress and
// Cancel between chunks, at ~10% extra process start-up cost.
constexpr int kTreeChunkKeys = 20000;

int g_pluginNr = 0;
tProgressProcW g_progressW = nullptr;
tProgressProc g_progressA = nullptr;
tRequestProcW g_requestW = nullptr;

std::mutex g_configMutex;
Config g_config;
std::wstring g_awsExe;

DirCache g_cache;
DiskCache g_disk;
RefreshTracker g_refresh;

// ---- Per-operation state: thread_local, because with background transfers (FsGetBackgroundFlags)
// TC runs a whole F5/F6 operation in its own thread while the panel keeps working in the main
// thread. TC guarantees that FsStatusInfo START..END and all calls in between of one operation
// happen on the same thread. Shared state (cache, config, disk cache) is mutex-protected.

// Current top-level operation reported through FsStatusInfo (0 = none), and whether we are
// inside a panel listing (FS_STATUS_OP_LIST).
thread_local int g_currentOp = 0;
thread_local bool g_inPanelListing = false;

// Recursive operations (Space, F5 pre-scan, search): subtree roots already fetched with one flat
// listing during the current operation, and whether the user cancelled (or an error occurred).
thread_local std::vector<std::wstring> g_opTreeRoots;
thread_local std::vector<SubtreeInfo> g_opSubtrees;  // complete trees fetched in this operation (for batching)
thread_local bool g_opAborted = false;
// Space (CALCSIZE): one progress window for the whole operation, i.e. the AWS listing and TC's
// walk over the cached folders. Appears only if the operation takes longer than 1 s.
thread_local std::shared_ptr<ProgressWindow> g_opWindow;
thread_local size_t g_opFolders = 0;

// Downloads queued during FS_STATUS_OP_GET_MULTI, executed at FS_STATUS_END (measured: TC keeps its dialog open).
thread_local std::vector<DownloadItem> g_downloadQueue;

// Write operations deferred to FS_STATUS_END of the current operation.
thread_local std::wstring g_opRemoteDir;              // RemoteDir passed with FS_STATUS_START (normalized)
thread_local bool g_opMessageShown = false;           // one error/read-only message per operation
thread_local std::vector<UploadItem> g_uploadQueue;   // PUT_MULTI
thread_local std::vector<std::wstring> g_uploadPaths; // plugin paths of g_uploadQueue items
thread_local std::vector<std::wstring> g_mkdirQueue;  // folders "created" during PUT_MULTI (markers only if empty)
thread_local std::set<std::wstring> g_newDirs;        // same, for existence checks without AWS calls
struct DeleteQueue {
    std::map<std::pair<std::wstring, std::wstring>, std::vector<std::wstring>> keys;  // (profile, bucket)
    std::vector<std::wstring> files;  // plugin paths, for cache invalidation
    std::vector<std::wstring> dirs;
    size_t Size() const {
        size_t n = 0;
        for (const auto& [k, v] : keys) n += v.size();
        return n;
    }
};
thread_local DeleteQueue g_deletes;

// Operations during which TC displays its own progress dialog for ProgressProc calls (verified:
// GET_MULTI/PUT_MULTI; not CALCSIZE or command-line EXEC).
bool TcShowsProgress(int op) {
    switch (op) {
        case FS_STATUS_OP_GET_SINGLE:
        case FS_STATUS_OP_GET_MULTI:
        case FS_STATUS_OP_GET_MULTI_THREAD:
        case FS_STATUS_OP_PUT_SINGLE:
        case FS_STATUS_OP_PUT_MULTI:
        case FS_STATUS_OP_PUT_MULTI_THREAD:
        case FS_STATUS_OP_RENMOV_SINGLE:
        case FS_STATUS_OP_RENMOV_MULTI:
        case FS_STATUS_OP_SYNC_GET:
        case FS_STATUS_OP_SYNC_PUT:
            return true;
        default:
            return false;
    }
}

bool IsRecursiveOp(int op) {
    switch (op) {
        case FS_STATUS_OP_GET_MULTI:
        case FS_STATUS_OP_GET_MULTI_THREAD:
        case FS_STATUS_OP_DELETE:
        case FS_STATUS_OP_RENMOV_MULTI:
        case FS_STATUS_OP_CALCSIZE:
        case FS_STATUS_OP_SEARCH:
        case FS_STATUS_OP_SEARCH_TEXT:
        case FS_STATUS_OP_SYNC_SEARCH:
        case FS_STATUS_OP_SYNC_GET:
            return true;
        default:
            return false;
    }
}

struct FindState {
    std::shared_ptr<const CachedDir> dir;
    size_t next = 0;
};

const wchar_t* OpName(int op) {
    static const wchar_t* names[] = {
        L"-",           L"LIST",          L"GET_SINGLE",   L"GET_MULTI",   L"PUT_SINGLE",
        L"PUT_MULTI",   L"RENMOV_SINGLE", L"RENMOV_MULTI", L"DELETE",      L"ATTRIB",
        L"MKDIR",       L"EXEC",          L"CALCSIZE",     L"SEARCH",      L"SEARCH_TEXT",
        L"SYNC_SEARCH", L"SYNC_GET",      L"SYNC_PUT",     L"SYNC_DELETE", L"GET_MULTI_THREAD",
        L"PUT_MULTI_THREAD"};
    return (op >= 0 && op < static_cast<int>(_countof(names))) ? names[op] : L"UNKNOWN";
}

Config CurrentConfig() {
    std::lock_guard<std::mutex> lock(g_configMutex);
    return g_config;
}

std::wstring AwsExe() {
    std::lock_guard<std::mutex> lock(g_configMutex);
    return g_awsExe;
}

void ReloadConfig() {
    std::lock_guard<std::mutex> lock(g_configMutex);
    g_config = LoadConfig(g_config.iniPath);
    LogSetFile(g_config.logFile);
    g_awsExe = ResolveAwsExe(g_config.awsExe);
    SetDisableImds(g_config.disableImds);
    g_disk.Configure(g_config.diskCache, g_config.diskCacheDir,
                     g_config.diskCacheMaxAgeDays > 0 ? g_config.diskCacheMaxAgeDays * 86400000ULL : 0);
}

int Progress(const std::wstring& src, const std::wstring& dst, int pct) {
    if (g_progressW) {
        return g_progressW(g_pluginNr, const_cast<WCHAR*>(src.c_str()),
                           const_cast<WCHAR*>(dst.c_str()), pct);
    }
    if (g_progressA) {
        std::string s = WideToAnsi(src), d = WideToAnsi(dst);
        return g_progressA(g_pluginNr, s.data(), d.data(), pct);
    }
    return 0;
}

void ShowMessage(const std::wstring& text) {
    Log(L"message: %s", text.c_str());
    if (g_requestW) {
        g_requestW(g_pluginNr, RT_MsgOK, const_cast<WCHAR*>(kTitle), const_cast<WCHAR*>(text.c_str()),
                   nullptr, 0);
    } else {
        MessageBoxW(nullptr, text.c_str(), kTitle, MB_OK | MB_ICONWARNING);
    }
}

// Lets TC show its progress dialog (with Cancel) for slow listings. TC itself ignores calls
// during the first 5 seconds of a directory change, so short listings show nothing.
std::function<bool()> ListingCancelCheck(const std::wstring& path) {
    auto lastCall = std::make_shared<ULONGLONG>(GetTickCount64());
    return [path, lastCall]() {
        ULONGLONG now = GetTickCount64();
        if (now - *lastCall < 500) return false;
        *lastCall = now;
        return Progress(path, path, 0) != 0;
    };
}

CacheEntry MakeEntry(const std::wstring& displayName, bool isDir, unsigned long long size, const FILETIME* mtime) {
    CacheEntry e;
    e.cFileName = displayName;
    e.dwFileAttributes = isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    e.size = size;
    if (mtime) {
        e.ftLastWriteTime = *mtime;
    } else {
        e.ftLastWriteTime.dwHighDateTime = 0xFFFFFFFF;  // "no time" marker understood by TC
        e.ftLastWriteTime.dwLowDateTime = 0xFFFFFFFE;
    }
    return e;
}

// Appends S3 entries, encoding names for Windows. Names too long for TC are skipped.
void AppendEntries(const std::vector<S3Entry>& entries, std::vector<CacheEntry>* items) {
    for (const auto& e : entries) {
        std::wstring name = EncodeName(e.name);
        if (name.size() >= MAX_PATH) {
            Log(L"skipped: name longer than %d characters: %s", MAX_PATH - 1, name.c_str());
            continue;
        }
        items->push_back(MakeEntry(name, e.isDir, e.size, e.hasTime ? &e.mtime : nullptr));
    }
}

const wchar_t kTruncatedMarkerPrefix[] = L"[listing truncated at ";

bool IsTruncatedMarker(const std::wstring& name) {
    return name.rfind(kTruncatedMarkerPrefix, 0) == 0;
}

// Bucket list of a profile (list-buckets + PinnedBuckets). No messages: false + *err on failure.
bool ListProfileBuckets(const AwsContext& ctx, const std::wstring& iniPath, std::vector<CacheEntry>* items,
                        AwsError* err) {
    const ProfileConfig pc = LoadProfileConfig(iniPath, ctx.profile);
    std::vector<S3Entry> buckets;
    if (!ListBuckets(ctx, &buckets, err)) {
        if (pc.pinnedBuckets.empty() || err->kind != AwsErrorKind::AccessDenied) return false;
        Log(L"list-buckets denied for %s; showing pinned buckets only", ctx.profile.c_str());
        buckets.clear();
    }
    for (const auto& pinned : pc.pinnedBuckets) {
        bool present = false;
        for (const auto& b : buckets) present = present || b.name == pinned;
        if (!present) buckets.push_back({pinned, true, 0, {}, false});
    }
    AppendEntries(buckets, items);
    return true;
}

// Fetches a directory listing from AWS. On failure shows a message and returns false.
bool FetchDirectory(const std::wstring& path, std::vector<CacheEntry>* items) {
    const Config cfg = CurrentConfig();
    const VfsPath vp = ParseVfsPath(path);
    AwsContext ctx{AwsExe(), vp.profile, ListingCancelCheck(path)};
    AwsError err;

    switch (vp.level) {
        case VfsPath::Level::Root: {
            std::vector<std::wstring> profiles;
            std::wstring error;
            if (!ListProfiles(ctx.awsExe, &profiles, &error)) {
                ShowMessage(L"Cannot list AWS CLI profiles.\n\n" + error);
                return false;
            }
            for (const auto& p : profiles) {
                if (!LoadProfileConfig(cfg.iniPath, p).hidden) {
                    items->push_back(MakeEntry(EncodeName(p), true, 0, nullptr));
                }
            }
            return true;
        }
        case VfsPath::Level::Profile: {
            if (ListProfileBuckets(ctx, cfg.iniPath, items, &err)) return true;
            if (err.kind != AwsErrorKind::Cancelled) ShowMessage(DescribeAwsError(err, vp.profile));
            return false;
        }
        case VfsPath::Level::Bucket:
        case VfsPath::Level::Inside: {
            S3Listing listing;
            if (!ListFolder(ctx, vp.bucket, ListPrefix(vp), cfg.maxListItems, &listing, &err)) {
                if (err.kind != AwsErrorKind::Cancelled) ShowMessage(DescribeAwsError(err, vp.profile));
                return false;
            }
            AppendEntries(listing.entries, items);
            if (listing.truncated) {
                items->push_back(MakeEntry(kTruncatedMarkerPrefix + std::to_wstring(cfg.maxListItems) +
                                               L" items - see MaxListItems]",
                                           false, 0, nullptr));
            }
            return true;
        }
    }
    return false;
}

// Returns the listing for `path`, from the cache unless a refresh was requested.
std::shared_ptr<const CachedDir> GetDirectory(const std::wstring& path, bool refresh) {
    const Config cfg = CurrentConfig();
    g_disk.EnsureLoaded(path, &g_cache);  // listings from earlier sessions, once per bucket
    if (!refresh) {
        if (auto cached = g_cache.Get(path, cfg.cacheTtlSec * 1000ULL)) {
            Log(L"cache hit \"%s\" (%zu items)", path.c_str(), cached->items.size());
            return cached;
        }
    }
    std::vector<CacheEntry> items;
    if (!FetchDirectory(path, &items)) return nullptr;
    g_cache.Put(path, std::move(items));
    return g_cache.Get(path, 0);
}

bool IsUnderOpTreeRoot(const std::wstring& path) {
    for (const auto& root : g_opTreeRoots) {
        if (path == root || path.compare(0, root.size() + 1, root + L"\\") == 0) return true;
    }
    return false;
}

// Fetches the whole subtree of `path` with one flat listing and caches every directory that
// was fully covered. Returns false on error or cancel (sets g_opAborted).
bool PrefetchTree(const std::wstring& path, const VfsPath& vp, int maxKeys) {
    // Progress: inside F5/F6 (and other operations with a TC progress dialog) through ProgressProc;
    // otherwise (Space, command line, search, F8) TC shows nothing, so the plugin opens its own
    // window with a key counter and Cancel.
    const std::wstring uri = L"s3://" + vp.bucket + L"/" + ListPrefix(vp);
    std::shared_ptr<ProgressWindow> window;
    if (g_opWindow) {
        window = g_opWindow;  // the whole Space operation already has one
    } else if (!TcShowsProgress(g_currentOp)) {
        window = std::make_shared<ProgressWindow>(L"S3: listing folder tree");
    }
    auto keys = std::make_shared<size_t>(0);
    auto lastCall = std::make_shared<ULONGLONG>(GetTickCount64());
    auto report = [uri, path, keys, maxKeys, window]() {
        int pct = maxKeys > 0 ? static_cast<int>(*keys * 100 / static_cast<size_t>(maxKeys)) : 0;
        pct = (std::min)(pct, 100);
        if (window) {
            window->Update(L"Listing " + uri + L"\n" + std::to_wstring(*keys) + L" keys so far (limit " +
                               std::to_wstring(maxKeys) + L")",
                           pct);
            return window->Cancelled();
        }
        std::wstring label = L"Listing " + uri + L"   [" + std::to_wstring(*keys) + L" keys]";
        return Progress(label, path, pct) != 0;
    };
    AwsContext ctx{AwsExe(), vp.profile, [report, lastCall, window]() {
                       if (window && window->Cancelled()) return true;  // react within ~50 ms
                       if (GetTickCount64() - *lastCall < 500) return false;
                       *lastCall = GetTickCount64();
                       return report();
                   }};
    report();
    S3Listing flat;
    AwsError err;
    const bool ok = ListTree(ctx, vp.bucket, ListPrefix(vp), maxKeys, &flat, &err, kTreeChunkKeys,
                             [keys, report](size_t n) {
                                 *keys = n;
                                 return report();
                             });
    if (!ok) {
        g_opAborted = true;
        if (err.kind != AwsErrorKind::Cancelled) ShowMessage(DescribeAwsError(err, vp.profile));
        if (flat.entries.empty()) return false;
        Log(L"tree \"%s\": interrupted after %zu keys; caching the fully listed folders", path.c_str(),
            flat.entries.size());
    }
    if (!flat.truncated) {
        SubtreeInfo info{vp.profile, vp.bucket, ListPrefix(vp), {}};
        for (const auto& e : flat.entries) {
            if (!e.name.empty() && e.name.back() != L'/') info.files.push_back(e.name);
        }
        g_opSubtrees.push_back(std::move(info));
    }
    auto tree = BuildDirectoryTree(flat.entries, flat.truncated);
    size_t cached = 0;
    for (const auto& [rel, dir] : tree) {
        if (!dir.complete) continue;  // re-listed folder by folder if TC asks for it
        std::wstring dirPath = path;
        for (const auto& segment : SplitDirKey(rel)) dirPath += L"\\" + EncodeName(segment);
        std::vector<CacheEntry> items;
        AppendEntries(dir.entries, &items);
        g_cache.Put(dirPath, std::move(items), true);
        ++cached;
    }
    Log(L"tree \"%s\": %zu keys%s, %zu of %zu dirs cached", path.c_str(), flat.entries.size(),
        flat.truncated ? L" (truncated)" : L"", cached, tree.size());
    return ok;  // false after a cancel or error: the rest of TC's walk is aborted
}

// Collects all file keys (relative, decoded, '/'-separated) below a cached directory.
// Returns false if any subdirectory is missing from the cache.
bool CollectCachedFiles(const std::wstring& path, const std::wstring& rel, std::vector<std::wstring>* out) {
    auto dir = g_cache.Get(path, 0);
    if (!dir) return false;
    for (const auto& fd : dir->items) {
        const std::wstring name = DecodeName(fd.cFileName);
        const std::wstring childRel = rel.empty() ? name : rel + L"/" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!CollectCachedFiles(path + L"\\" + fd.cFileName, childRel, out)) return false;
        } else if (IsTruncatedMarker(fd.cFileName)) {
            return false;
        } else {
            out->push_back(childRel);
        }
    }
    return true;
}

// Directory lookup during recursive operations: one flat listing per selected subtree.
std::shared_ptr<const CachedDir> GetDirectoryForRecursiveOp(const std::wstring& path) {
    if (g_opAborted) return nullptr;
    if (g_opWindow) {
        if (g_opWindow->Cancelled()) {
            g_opAborted = true;
            return nullptr;
        }
        if (++g_opFolders % 256 == 1) {
            g_opWindow->Update(L"Scanning cached folders...\n" + std::to_wstring(g_opFolders) + L" folders", 0);
        }
    }
    const Config cfg = CurrentConfig();
    g_disk.EnsureLoaded(path, &g_cache);
    const VfsPath vp = ParseVfsPath(path);
    if (vp.level == VfsPath::Level::Root || vp.level == VfsPath::Level::Profile || cfg.prefetchMaxKeys <= 0) {
        return GetDirectory(path, false);
    }
    auto cached = g_cache.Get(path, cfg.cacheTtlSec * 1000ULL);
    if (cached && cached->subtreeComplete) {
        // Already known from an earlier tree listing (e.g. Space before F5): no AWS call, but the
        // batch planner still needs the full file list of the selected folder.
        if (!IsUnderOpTreeRoot(path)) {
            SubtreeInfo info{vp.profile, vp.bucket, ListPrefix(vp), {}};
            if (CollectCachedFiles(path, L"", &info.files)) {
                g_opSubtrees.push_back(std::move(info));
                g_opTreeRoots.push_back(path);
            }
        }
        return cached;
    }
    if (IsUnderOpTreeRoot(path)) return cached ? cached : GetDirectory(path, false);

    if (!PrefetchTree(path, vp, cfg.prefetchMaxKeys)) return nullptr;
    g_opTreeRoots.push_back(path);
    if (auto c = g_cache.Get(path, 0); c && c->subtreeComplete) return c;
    return GetDirectory(path, false);  // the tree was truncated inside this folder
}

void ToAnsiFindData(const WIN32_FIND_DATAW& w, WIN32_FIND_DATAA* a) {
    memset(a, 0, sizeof(*a));
    a->dwFileAttributes = w.dwFileAttributes;
    a->ftCreationTime = w.ftCreationTime;
    a->ftLastAccessTime = w.ftLastAccessTime;
    a->ftLastWriteTime = w.ftLastWriteTime;
    a->nFileSizeHigh = w.nFileSizeHigh;
    a->nFileSizeLow = w.nFileSizeLow;
    strncpy_s(a->cFileName, WideToAnsi(w.cFileName).c_str(), _TRUNCATE);
}

const wchar_t kHelp[] =
    L"Commands (type them on the Total Commander command line inside the plugin):\n"
    L"  refresh       re-read the current folder from AWS (same as Ctrl+R)\n"
    L"  refresh-all   drop all cached listings\n"
    L"  prefetch      list the whole tree below the current folder (like Space)\n"
    L"  go s3://bucket/key   go to that folder or file (or: cd s3://...)\n"
    L"  setup         add the s3:// copy commands to usercmd.ini (asks first)\n"
    L"  help          show this text";

// ---------------------------------------------------------------------------------------------
// "Go to s3://...": a URI typed into the address bar ("\\\S3\s3://b/k", "\\\S3\profile\s3://b/k")
// or into the command line while the panel is in the plugin. The WFX API cannot redirect a
// listing, so the plugin returns an empty listing and then sends TC its documented "CD" command
// (WM_COPYDATA) with the real path, once TC has finished the current listing.

std::wstring VfsPathFor(const std::wstring& profile, const std::wstring& bucket, const std::wstring& prefix);
std::wstring ParentPath(const std::wstring& path);
std::wstring LeafName(const std::wstring& path);

HINSTANCE g_module = nullptr;                                     // this DLL (DllMain)
HANDLE g_listEndEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // set at FS_STATUS_END(LIST)

// TC root name of this plugin ("S3" unless renamed in TC's plugin configuration).
std::wstring TcRootName() {
    wchar_t dll[MAX_PATH] = {};
    GetModuleFileNameW(g_module, dll, MAX_PATH);
    std::wstring file = dll;
    file = file.substr(file.find_last_of(L"\\/") + 1);
    wchar_t ini[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"COMMANDER_INI", ini, MAX_PATH);
    return ReadPluginRootName(n > 0 && n < MAX_PATH ? ini : L"", file.empty() ? L"s3tcmd.wfx64" : file);
}

BOOL CALLBACK FindTcWindowProc(HWND hwnd, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    wchar_t cls[32] = {};
    if (pid == GetCurrentProcessId() && GetClassNameW(hwnd, cls, _countof(cls)) && wcscmp(cls, L"TTOTAL_CMD") == 0) {
        *reinterpret_cast<HWND*>(lp) = hwnd;
        return FALSE;
    }
    return TRUE;
}

// Sends TC "CD <tcPath>" for the active panel after the running panel listing has ended.
void ScheduleTcCd(const std::wstring& tcPath) {
    ResetEvent(g_listEndEvent);
    std::thread([tcPath]() {
        WaitForSingleObject(g_listEndEvent, 3000);
        Sleep(50);  // let TC finish painting the (empty) listing
        HWND tc = nullptr;
        EnumWindows(FindTcWindowProc, reinterpret_cast<LPARAM>(&tc));
        if (!tc) {
            Log(L"go to: Total Commander main window not found");
            return;
        }
        std::string data = BuildTcCdCommand(tcPath);
        COPYDATASTRUCT cds{};
        cds.dwData = 'C' + 256 * 'D';
        cds.cbData = static_cast<DWORD>(data.size());
        cds.lpData = data.data();
        DWORD_PTR result = 0;
        LRESULT ok = SendMessageTimeoutW(tc, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_NORMAL, 5000,
                                         &result);
        Log(L"go to: sent CD \"%s\" (%s)", tcPath.c_str(), ok ? L"ok" : L"timeout");
    }).detach();
}

bool DirHasEntry(const CachedDir& dir, const std::wstring& name, bool* isDir) {
    for (const auto& e : dir.items) {
        if (e.cFileName == name) {
            if (isDir) *isDir = (e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            return true;
        }
    }
    return false;
}

// Fills in t->profile if the URI did not name one. Shows a message and returns false if no
// profile lists the bucket.
bool ResolveProfile(S3Target* t, const std::wstring& preferredProfile) {
    if (!t->profile.empty()) return true;
    auto root = GetDirectory(L"\\", false);
    if (!root) return false;
    const Config cfg = CurrentConfig();
    const std::wstring bucketName = EncodeName(t->bucket);
    std::vector<ProfileBucketInfo> infos;
    std::vector<size_t> uncached;
    for (const auto& e : root->items) {
        ProfileBucketInfo pi;
        pi.profile = DecodeName(e.cFileName);
        const ProfileConfig pc = LoadProfileConfig(cfg.iniPath, pi.profile);
        pi.pinned = std::find(pc.pinnedBuckets.begin(), pc.pinnedBuckets.end(), t->bucket) != pc.pinnedBuckets.end();
        const std::wstring ppath = L"\\" + e.cFileName;
        g_disk.EnsureLoaded(ppath, &g_cache);
        if (auto d = g_cache.Get(ppath, cfg.cacheTtlSec * 1000ULL)) {
            pi.hasBucket = DirHasEntry(*d, bucketName, nullptr);
        } else {
            uncached.push_back(infos.size());
        }
        infos.push_back(pi);
    }
    std::wstring chosen = ChooseProfileForBucket(infos, preferredProfile);
    if (chosen.empty() && !uncached.empty()) {
        // Bucket lists of the remaining profiles, in parallel (one aws.exe each, ~1.5 s).
        ProgressWindow window(L"S3: looking for a bucket", 500);
        window.Update(L"Looking for bucket " + t->bucket + L" in " + std::to_wstring(uncached.size()) +
                          L" profile(s)...",
                      0);
        std::vector<std::vector<CacheEntry>> lists(uncached.size());
        std::vector<char> ok(uncached.size(), 0);
        std::vector<std::thread> threads;
        const std::wstring awsExe = AwsExe();
        for (size_t i = 0; i < uncached.size(); ++i) {
            threads.emplace_back([&, i]() {
                AwsContext ctx{awsExe, infos[uncached[i]].profile, [&window]() { return window.Cancelled(); }};
                AwsError err;
                ok[i] = ListProfileBuckets(ctx, cfg.iniPath, &lists[i], &err) ? 1 : 0;
            });
        }
        for (auto& th : threads) th.join();
        for (size_t i = 0; i < uncached.size(); ++i) {
            if (!ok[i]) continue;
            ProfileBucketInfo& pi = infos[uncached[i]];
            pi.hasBucket = std::any_of(lists[i].begin(), lists[i].end(),
                                       [&](const CacheEntry& e) { return e.cFileName == bucketName; });
            g_cache.Put(L"\\" + EncodeName(pi.profile), std::move(lists[i]));
        }
        if (window.Cancelled()) return false;
        chosen = ChooseProfileForBucket(infos, preferredProfile);
    }
    if (chosen.empty()) {
        ShowMessage(L"Bucket \"" + t->bucket + L"\" is not in the bucket list of any AWS profile.\n\n"
                    L"Name the profile explicitly, e.g. \\\\\\" + TcRootName() + L"\\<profile>\\s3://" + t->bucket +
                    L"/...\nor add the bucket to PinnedBuckets of that profile.");
        return false;
    }
    t->profile = chosen;
    return true;
}

// Resolves the target and schedules TC's CD to it. False (message shown) if it cannot be resolved.
bool GoToS3Target(S3Target t, const std::wstring& preferredProfile) {
    Log(L"go to: s3://%s/%s profile=%s", t.bucket.c_str(), t.key.c_str(), t.profile.c_str());
    if (!ResolveProfile(&t, preferredProfile)) return false;
    const std::wstring path = VfsPathFor(t.profile, t.bucket, t.key);
    // "s3://b/dir/file.txt": TC goes to the folder and puts the cursor on the file when the path
    // has no trailing backslash. Without a trailing '/' the key may be either; the parent decides.
    bool asFile = false;
    if (!t.isDir && !t.key.empty()) {
        bool isDir = true;
        if (auto parent = GetDirectory(ParentPath(path), false)) {
            asFile = DirHasEntry(*parent, LeafName(path), &isDir) && !isDir;
        }
    }
    const std::wstring tcPath = L"\\\\\\" + TcRootName() + path + (asFile ? L"" : L"\\");
    Log(L"go to: -> %s", tcPath.c_str());
    ScheduleTcCd(tcPath);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Setup of the TC user commands for s3tcmd-clip.exe (usercmd.ini). TC's plugin installer only
// copies files, so the plugin offers this itself: once per version, on the first panel listing
// of the plugin root, and only if a section is missing or points to a missing exe. The hotkey
// (wincmd.ini [Shortcuts]) is never written; the messages only describe it.

const wchar_t kSetupSection[] = L"Setup";
const wchar_t kSetupOfferedKey[] = L"UserCmdOffered";  // version in which the user was asked

std::wstring ClipExePath() {
    wchar_t dll[MAX_PATH] = {};
    GetModuleFileNameW(g_module, dll, MAX_PATH);
    return DirectoryOf(dll) + L"s3tcmd-clip.exe";
}

std::wstring WincmdIniPath() {
    wchar_t ini[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"COMMANDER_INI", ini, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? std::wstring(ini) : std::wstring();
}

bool AskYesNo(const std::wstring& text) {
    Log(L"question: %s", text.c_str());
    if (!g_requestW) return false;
    BOOL yes = g_requestW(g_pluginNr, RT_MsgYesNo, const_cast<WCHAR*>(kTitle), const_cast<WCHAR*>(text.c_str()),
                          nullptr, 0);
    Log(L"answer: %s", yes ? L"yes" : L"no");
    return yes != FALSE;
}

std::wstring DescribeUserCmdProblems(const std::vector<UserCmdStatus>& status) {
    std::wstring text;
    for (const auto& s : status) {
        if (s.state == UserCmdState::Missing) text += L"  [" + s.section + L"] is missing\n";
        if (s.state == UserCmdState::StaleCmd) {
            text += L"  [" + s.section + L"] runs a file that does not exist:\n    " + s.cmd + L"\n";
        }
    }
    return text;
}

std::wstring CopyCommandsHelp() {
    return L"em_s3_copy_uri copies the selected names with full path, like TC's cm_CopyFullNamesToClip, "
           L"but names inside S3 become s3://bucket/key (folders end with /). Other paths are copied "
           L"unchanged, so one hotkey works everywhere. On \"..\" it copies the current folder.\n"
           L"em_s3_presign copies presigned HTTPS URLs of the selected S3 files.\n\n" +
           DescribeCopyHotkey(ReadShortcuts(WincmdIniPath())) +
           L"\nTo bind a key yourself: Configuration > Options > Misc > Redefine hotkeys: choose the key "
           L"(e.g. Ctrl+Shift+C), then the command em_s3_copy_uri (category usercmd.ini).";
}

// Asks, then writes. Returns false if the user said no or the setup failed (message shown).
bool AskAndApplySetup(const std::wstring& userCmdIni, const std::vector<UserCmdStatus>& status) {
    const std::wstring clipExe = ClipExePath();
    const std::wstring question =
        L"S3 plugin " S3TCMD_VERSION_WSTR L": set up the commands for copying s3:// paths?\n\n" +
        DescribeUserCmdProblems(status) + L"\nThis adds the user commands em_s3_copy_uri and em_s3_presign to\n  " +
        userCmdIni + L"\n(only these two sections; a backup is made first). They run\n  " + clipExe +
        L"\nYou can bind a hotkey to em_s3_copy_uri afterwards.\n\n"
        L"Later: type \"setup\" on the command line inside the plugin.";
    if (!AskYesNo(question)) return false;
    const SetupResult r = ApplyUserCommands(userCmdIni, PortablePath(clipExe));
    if (!r.ok) {
        ShowMessage(L"Setup failed: " + r.error);
        return false;
    }
    std::wstring sections;
    for (const auto& s : r.written) sections += (sections.empty() ? L"" : L", ") + s;
    Log(L"setup: wrote %s to %s (backup %s)", sections.c_str(), userCmdIni.c_str(), r.backup.c_str());
    ShowMessage(L"Added to " + userCmdIni + L": " + sections + L".\n" +
                (r.backup.empty() ? std::wstring(L"(new file)") : L"Backup: " + r.backup) + L"\n\n" +
                CopyCommandsHelp());
    return true;
}

// The `setup` command: (re)run the setup, or show that it is done.
void RunSetupCommand() {
    const std::wstring userCmdIni = UserCmdIniPath(DirectoryOf(CurrentConfig().iniPath));
    const auto status = CheckUserCommands(userCmdIni);
    if (NeedsSetup(status)) {
        AskAndApplySetup(userCmdIni, status);
        return;
    }
    std::wstring text = L"S3 plugin " S3TCMD_VERSION_WSTR L": the user commands are set up in\n  " + userCmdIni + L"\n";
    for (const auto& s : status) text += L"  [" + s.section + L"] cmd=" + s.cmd + L"\n";
    ShowMessage(text + L"\n" + CopyCommandsHelp());
}

// Called on panel listings of the plugin root (TC's main thread).
void OfferSetupOnce() {
    static bool checked = false;
    if (checked) return;
    checked = true;
    const std::wstring ini = CurrentConfig().iniPath;
    if (ini.empty()) return;
    wchar_t offered[64] = {};
    GetPrivateProfileStringW(kSetupSection, kSetupOfferedKey, L"", offered, _countof(offered), ini.c_str());
    if (wcscmp(offered, S3TCMD_VERSION_WSTR) == 0) return;  // asked in this version already
    const std::wstring userCmdIni = UserCmdIniPath(DirectoryOf(ini));
    const auto status = CheckUserCommands(userCmdIni);
    if (!NeedsSetup(status)) return;
    if (GetFileAttributesW(ClipExePath().c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log(L"setup: %s not found, not offered", ClipExePath().c_str());
        return;
    }
    WritePrivateProfileStringW(kSetupSection, kSetupOfferedKey, S3TCMD_VERSION_WSTR, ini.c_str());
    AskAndApplySetup(userCmdIni, status);
}

int HandleCommand(const std::wstring& dir, const std::wstring& commandLine) {
    std::wstring cmd = commandLine;
    while (!cmd.empty() && iswspace(cmd.back())) cmd.pop_back();
    while (!cmd.empty() && iswspace(cmd.front())) cmd.erase(cmd.begin());
    std::wstring verb = cmd.substr(0, cmd.find(L' '));
    const std::wstring path = NormalizePath(dir);

    // "go s3://b/k" or "cd s3://b/k". TC 11.58 does not pass a bare "s3://..." typed on the command
    // line to the plugin at all, and passes "cd s3://..." on as a command (measured).
    S3Target target;
    std::wstring uri = cmd;
    if (_wcsicmp(verb.c_str(), L"go") == 0 || _wcsicmp(verb.c_str(), L"cd") == 0) uri = cmd.substr(2);
    if (ParseS3Uri(uri, &target)) {
        // Prefer the profile of the current panel when it can see the bucket.
        GoToS3Target(target, ParseVfsPath(path).profile);
        g_refresh.SuppressNext(path);  // TC re-lists the panel next; that is not Ctrl+R
        return FS_EXEC_OK;
    }

    if (_wcsicmp(verb.c_str(), L"refresh") == 0) {
        g_cache.Invalidate(path);
        g_refresh.ForceNext(path);
        return FS_EXEC_OK;  // TC re-lists the panel
    }
    if (_wcsicmp(verb.c_str(), L"refresh-all") == 0) {
        g_cache.Clear();
        g_disk.DeleteAll();
        g_refresh.ForceNext(path);
        return FS_EXEC_OK;
    }
    if (_wcsicmp(verb.c_str(), L"prefetch") == 0) {
        // Same as Space on the current folder, without the size calculation.
        const VfsPath vp = ParseVfsPath(path);
        if (vp.level != VfsPath::Level::Bucket && vp.level != VfsPath::Level::Inside) {
            ShowMessage(L"prefetch works inside a bucket.");
            return FS_EXEC_OK;
        }
        g_disk.EnsureLoaded(path, &g_cache);
        PrefetchTree(path, vp, CurrentConfig().prefetchMaxKeys);
        g_refresh.SuppressNext(path);  // TC re-lists the panel next: serve it from the new tree
        return FS_EXEC_OK;
    }
    if (_wcsicmp(verb.c_str(), L"setup") == 0) {
        RunSetupCommand();
        g_refresh.SuppressNext(path);
        return FS_EXEC_OK;
    }
    if (_wcsicmp(verb.c_str(), L"mark") == 0) {
        g_refresh.SuppressNext(path);
        return FS_EXEC_OK;  // log marker for manual test sessions; the command itself is logged
    }
    if (_wcsicmp(verb.c_str(), L"help") != 0) ShowMessage(L"Unknown command: " + cmd + L"\n\n" + kHelp);
    else ShowMessage(kHelp);
    return FS_EXEC_OK;
}

// ---------------------------------------------------------------------------------------------
// Downloads

// Plugin path of an S3 directory prefix ("" or "a/b/").
std::wstring VfsPathFor(const std::wstring& profile, const std::wstring& bucket, const std::wstring& prefix) {
    std::wstring path = L"\\" + EncodeName(profile) + L"\\" + EncodeName(bucket);
    size_t start = 0;
    while (start < prefix.size()) {
        size_t slash = prefix.find(L'/', start);
        if (slash == std::wstring::npos) slash = prefix.size();
        path += L"\\" + EncodeName(prefix.substr(start, slash - start));
        start = slash + 1;
    }
    return path;
}

// How many keys `aws s3 cp --recursive` would list under `prefix` (see PlanDownloads).
long long SubtreeKeyCount(const std::wstring& profile, const std::wstring& bucket, const std::wstring& prefix,
                          size_t queuedFiles) {
    for (const auto& s : g_opSubtrees) {
        if (s.profile != profile || s.bucket != bucket || prefix.compare(0, s.prefix.size(), s.prefix) != 0) continue;
        const std::wstring rel = prefix.substr(s.prefix.size());
        long long n = 0;
        for (const auto& f : s.files) n += f.compare(0, rel.size(), rel) == 0 ? 1 : 0;
        return n;
    }
    if (auto cached = g_cache.Get(VfsPathFor(profile, bucket, prefix), 0)) {
        bool hasDirs = false, truncated = false;
        for (const auto& fd : cached->items) {
            hasDirs = hasDirs || (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            truncated = truncated || IsTruncatedMarker(fd.cFileName);
        }
        if (!hasDirs && !truncated) return static_cast<long long>(cached->items.size());
        // Every subfolder is cached (from a tree listing or earlier navigation): count locally.
        std::vector<std::wstring> files;
        if (!truncated && CollectCachedFiles(VfsPathFor(profile, bucket, prefix), L"", &files)) {
            return static_cast<long long>(files.size());
        }
    }
    // Unknown subtree: probing costs one aws.exe call, worth it only for several files.
    if (queuedFiles < 4) return -1;
    AwsContext ctx{AwsExe(), profile, nullptr};
    S3Listing probe;
    AwsError err;
    if (!ListTree(ctx, bucket, prefix, static_cast<int>(kMaxFilteredListing), &probe, &err) || probe.truncated) {
        return -1;
    }
    return static_cast<long long>(probe.entries.size());
}

std::wstring FormatFailures(const std::vector<DownloadItem>& items, const DownloadResult& r) {
    std::wstring text = std::to_wstring(r.failures.size()) + L" file(s) could not be downloaded:\n";
    size_t shown = 0;
    for (const auto& f : r.failures) {
        if (++shown > 15) {
            text += L"...\n";
            break;
        }
        text += L"\n" + items[f.item].key + L"\n    " + f.message.substr(0, 300) + L"\n";
    }
    return text;
}

void RunQueuedDownloads() {
    std::vector<DownloadItem> items;
    items.swap(g_downloadQueue);
    if (items.empty()) return;
    auto commands = PlanDownloads(items, g_opSubtrees, SubtreeKeyCount);
    Log(L"batch: %zu files -> %zu aws commands", items.size(), commands.size());
    DownloadResult r = RunDownloads(AwsExe(), items, commands, Progress);
    if (r.cancelled) {
        Log(L"batch: cancelled");
    } else if (!r.failures.empty()) {
        ShowMessage(FormatFailures(items, r));
    }
}

int DownloadNow(const DownloadItem& item) {
    std::vector<DownloadItem> items = {item};
    std::vector<CpCommand> commands = PlanDownloads(items, {}, nullptr);
    DownloadResult r = RunDownloads(AwsExe(), items, commands, Progress);
    if (r.cancelled) return FS_FILE_USERABORT;
    if (!r.failures.empty()) {
        Log(L"download failed: %s", r.failures[0].message.c_str());
        return r.failures[0].fsCode;
    }
    return FS_FILE_OK;
}

// ---------------------------------------------------------------------------------------------
// Write operations

std::wstring ParentPath(const std::wstring& path) {
    size_t pos = path.find_last_of(L'\\');
    return (pos == 0 || pos == std::wstring::npos) ? std::wstring(L"\\") : path.substr(0, pos);
}

std::wstring LeafName(const std::wstring& path) { return path.substr(path.find_last_of(L'\\') + 1); }

void InvalidateParent(const std::wstring& path) { g_cache.Invalidate(ParentPath(path)); }

void ShowOpMessageOnce(const std::wstring& text) {
    if (g_opMessageShown) {
        Log(L"message suppressed: %s", text.c_str());
        return;
    }
    g_opMessageShown = true;
    ShowMessage(text);
}

// Write operations are allowed inside buckets only, and not for read-only profiles.
bool CheckWritable(const VfsPath& vp) {
    if (vp.level != VfsPath::Level::Inside) return false;
    const Config cfg = CurrentConfig();
    if (cfg.readOnly || LoadProfileConfig(cfg.iniPath, vp.profile).readOnly) {
        ShowOpMessageOnce(L"Profile \"" + vp.profile + L"\" is read-only (ReadOnly=1 in " + cfg.iniPath + L").");
        return false;
    }
    return true;
}

// Looks up an entry in its parent's listing. Folders created in the current operation are known
// to be empty; other parents are listed (and cached) if needed.
bool RemoteEntryExists(const std::wstring& path, bool* isDir) {
    const std::wstring parent = ParentPath(path);
    if (g_newDirs.count(parent)) return false;
    auto dir = GetDirectory(parent, false);
    if (!dir) return false;
    const std::wstring leaf = LeafName(path);
    for (const auto& fd : dir->items) {
        if (leaf == fd.cFileName) {
            if (isDir) *isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            return true;
        }
    }
    return false;
}

// Runs one `aws s3 cp/mv` with TC progress and maps the outcome to an FS_FILE_* code.
int RunTransferCommand(const std::vector<std::wstring>& args, const std::wstring& src, const std::wstring& dst,
                       unsigned long long bytes, const std::wstring& profile, int defaultError) {
    BatchProgress bp;
    bp.progress = Progress;
    bp.totalBytes = bytes;
    bp.totalFiles = 1;
    AwsResult r = RunCpStep(AwsExe(), args, src, dst, bytes, &bp);
    if (bp.cancelled) return FS_FILE_USERABORT;
    if (r.ok) {
        Progress(src, dst, 100);
        return FS_FILE_OK;
    }
    AwsError e = ClassifyAwsError(r.error);
    switch (e.kind) {
        case AwsErrorKind::NoSuchKey:
        case AwsErrorKind::NoSuchBucket:
            return FS_FILE_NOTFOUND;
        case AwsErrorKind::AccessDenied:
        case AwsErrorKind::SsoExpired:
        case AwsErrorKind::CredentialsExpired:
        case AwsErrorKind::NoCredentials:
        case AwsErrorKind::Network:
            ShowOpMessageOnce(DescribeAwsError(e, profile));
            return defaultError;
        default:
            Log(L"transfer failed: %s", r.error.c_str());
            return defaultError;
    }
}

std::wstring S3Uri(const std::wstring& bucket, const std::wstring& key) { return L"s3://" + bucket + L"/" + key; }

void RunQueuedUploads() {
    std::vector<UploadItem> items;
    std::vector<std::wstring> paths, mkdirs;
    items.swap(g_uploadQueue);
    paths.swap(g_uploadPaths);
    mkdirs.swap(g_mkdirQueue);
    if (items.empty() && mkdirs.empty()) return;

    UploadResult r;
    r.succeeded.assign(items.size(), false);
    if (!items.empty()) {
        // RemoteDir of PUT_MULTI is the target folder: whole local folders uploaded into it
        // become one --recursive command each.
        const VfsPath target = ParseVfsPath(g_opRemoteDir);
        const std::wstring opPrefix = target.level >= VfsPath::Level::Bucket ? ListPrefix(target) : std::wstring();
        auto commands = PlanUploads(items, opPrefix, [](const std::wstring& dir, std::vector<std::wstring>* files) {
            return ListLocalTree(dir, files, 100000);
        });
        Log(L"upload batch: %zu files -> %zu aws commands", items.size(), commands.size());
        r = RunUploads(AwsExe(), items, commands, Progress);
        if (!r.cancelled && !r.failures.empty()) {
            std::wstring text = std::to_wstring(r.failures.size()) + L" file(s) could not be uploaded:\n";
            for (size_t n = 0; n < r.failures.size() && n < 15; ++n) {
                text += L"\n" + items[r.failures[n].item].local + L"\n    " + r.failures[n].message.substr(0, 300) + L"\n";
            }
            ShowOpMessageOnce(text);
        }
    }

    // Folders only need a marker object if nothing was uploaded into them.
    if (!r.cancelled) {
        for (const auto& dirPath : mkdirs) {
            const VfsPath vp = ParseVfsPath(dirPath);
            const std::wstring dirKey = vp.key + L"/";
            bool hasContent = false;
            for (size_t i = 0; i < items.size() && !hasContent; ++i) {
                hasContent = r.succeeded[i] && items[i].profile == vp.profile && items[i].bucket == vp.bucket &&
                             items[i].key.compare(0, dirKey.size(), dirKey) == 0;
            }
            if (hasContent) continue;
            AwsContext ctx{AwsExe(), vp.profile, nullptr};
            AwsError err;
            if (!PutDirMarker(ctx, vp.bucket, dirKey, &err)) ShowOpMessageOnce(DescribeAwsError(err, vp.profile));
        }
    }
    for (const auto& p : paths) InvalidateParent(p);
    for (const auto& d : mkdirs) {
        InvalidateParent(d);
        g_cache.InvalidateTree(d);
    }
}

void RunQueuedDeletes() {
    DeleteQueue q;
    std::swap(q, g_deletes);
    const size_t total = q.Size();
    if (total == 0) return;
    size_t doneBefore = 0;
    bool cancelled = false;
    std::vector<DeleteFailure> failures;
    for (const auto& [where, keys] : q.keys) {
        if (cancelled) break;
        const auto& [profile, bucket] = where;
        const std::wstring label = L"Deleting " + std::to_wstring(total) + L" object(s) in s3://" + bucket;
        if (Progress(label, L"", static_cast<int>(doneBefore * 100 / total))) {
            cancelled = true;
            break;
        }
        AwsContext ctx{AwsExe(), profile, nullptr};
        AwsError err;
        bool ok = DeleteObjects(ctx, bucket, keys, &failures, &err, [&](size_t done) {
            cancelled = Progress(label, L"", static_cast<int>((doneBefore + done) * 100 / total)) != 0;
            return cancelled;
        });
        if (!ok) ShowOpMessageOnce(DescribeAwsError(err, profile));
        doneBefore += keys.size();
    }
    Log(L"delete batch: %zu keys, %zu failed%s", total, failures.size(), cancelled ? L", cancelled" : L"");
    if (!failures.empty()) {
        std::wstring text = std::to_wstring(failures.size()) + L" object(s) could not be deleted:\n";
        for (size_t n = 0; n < failures.size() && n < 15; ++n) {
            text += L"\n" + failures[n].key + L"\n    " + failures[n].message.substr(0, 300) + L"\n";
        }
        ShowOpMessageOnce(text);
    }
    for (const auto& f : q.files) InvalidateParent(f);
    for (const auto& d : q.dirs) {
        InvalidateParent(d);
        g_cache.InvalidateTree(d);
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Initialisation

int __stdcall FsInitW(int PluginNr, tProgressProcW pProgressProcW, tLogProcW,
                      tRequestProcW pRequestProcW) {
    g_pluginNr = PluginNr;
    g_progressW = pProgressProcW;
    g_requestW = pRequestProcW;
    return 0;
}

int __stdcall FsInit(int PluginNr, tProgressProc pProgressProc, tLogProc, tRequestProc) {
    g_pluginNr = PluginNr;
    g_progressA = pProgressProc;
    return 0;
}

void __stdcall FsSetDefaultParams(FsDefaultParamStruct* dps) {
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        g_config.iniPath = DirectoryOf(AnsiToWide(dps->DefaultIniName)) + L"s3tcmd.ini";
    }
    ReloadConfig();
    Log(L"FsSetDefaultParams: s3tcmd " S3TCMD_VERSION_WSTR L", interface %lu.%lu ini=%s aws=%s", dps->PluginInterfaceVersionHi,
        dps->PluginInterfaceVersionLow, CurrentConfig().iniPath.c_str(), AwsExe().c_str());
}

void __stdcall FsGetDefRootName(char* DefRootName, int maxlen) {
    strncpy_s(DefRootName, maxlen, "S3", _TRUNCATE);
}

// ---------------------------------------------------------------------------------------------
// Directory listing

HANDLE __stdcall FsFindFirstW(WCHAR* Path, WIN32_FIND_DATAW* FindData) {
    const std::wstring path = NormalizePath(Path);
    S3Target target;
    if (ParseS3AliasPath(path, &target)) {
        // s3:// URI typed into the address bar (or "cd s3://..."): show an empty folder, then TC
        // changes to the real path.
        Log(L"FsFindFirst \"%s\" (s3:// URI, op=%s)", path.c_str(), OpName(g_currentOp));
        if (!g_inPanelListing) {
            SetLastError(ERROR_PATH_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }
        if (!GoToS3Target(target, target.hintProfile)) {
            // Unknown bucket (message shown) or cancelled. A failed listing would make TC fall back
            // to the previous folder, possibly a local drive; stay in the plugin instead.
            std::wstring back = L"\\\\\\" + TcRootName() + L"\\";
            if (!target.profile.empty()) back += EncodeName(target.profile) + L"\\";
            else if (!target.hintProfile.empty()) {
                // "cd s3://..." from \profile\bucket\dir: back to that folder.
                std::wstring lower = path;
                for (auto& c : lower) c = towlower(c);
                size_t at = lower.find(L"\\s3:");
                if (at != std::wstring::npos) back = L"\\\\\\" + TcRootName() + path.substr(0, at) + L"\\";
            }
            Log(L"go to: failed -> %s", back.c_str());
            ScheduleTcCd(back);
        }
        SetLastError(ERROR_NO_MORE_FILES);
        return INVALID_HANDLE_VALUE;
    }
    if (g_inPanelListing && path == L"\\") OfferSetupOnce();
    const Config cfg = CurrentConfig();
    // Only panel listings take part in Ctrl+R detection; enumeration during copies does not.
    const bool refresh = g_inPanelListing && g_refresh.OnPanelListing(path, cfg.refreshHeuristic);
    Log(L"FsFindFirst \"%s\" (op=%s%s)", path.c_str(), OpName(g_currentOp), refresh ? L", refresh" : L"");

    std::shared_ptr<const CachedDir> dir;
    if (!g_inPanelListing && IsRecursiveOp(g_currentOp)) {
        dir = GetDirectoryForRecursiveOp(path);
    } else {
        dir = GetDirectory(path, refresh);
    }

    if (!dir) {
        SetLastError(g_opAborted ? ERROR_CANCELLED : ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    if (dir->items.empty()) {
        SetLastError(ERROR_NO_MORE_FILES);
        return INVALID_HANDLE_VALUE;
    }
    ToFindData(dir->items[0], FindData);
    auto state = std::make_unique<FindState>();
    state->dir = dir;
    state->next = 1;
    return reinterpret_cast<HANDLE>(state.release());
}

BOOL __stdcall FsFindNextW(HANDLE Hdl, WIN32_FIND_DATAW* FindData) {
    auto* state = reinterpret_cast<FindState*>(Hdl);
    if (!state || state->next >= state->dir->items.size()) return FALSE;
    ToFindData(state->dir->items[state->next++], FindData);
    return TRUE;
}

int __stdcall FsFindClose(HANDLE Hdl) {
    delete reinterpret_cast<FindState*>(Hdl);
    return 0;
}

HANDLE __stdcall FsFindFirst(char* Path, WIN32_FIND_DATA* FindData) {
    std::wstring path = AnsiToWide(Path);
    WIN32_FIND_DATAW w;
    HANDLE h = FsFindFirstW(path.data(), &w);
    if (h != INVALID_HANDLE_VALUE) ToAnsiFindData(w, FindData);
    return h;
}

BOOL __stdcall FsFindNext(HANDLE Hdl, WIN32_FIND_DATA* FindData) {
    WIN32_FIND_DATAW w;
    if (!FsFindNextW(Hdl, &w)) return FALSE;
    ToAnsiFindData(w, FindData);
    return TRUE;
}

// ---------------------------------------------------------------------------------------------
// File transfer

int __stdcall FsGetFileW(WCHAR* RemoteName, WCHAR* LocalName, int CopyFlags, RemoteInfoStruct* ri) {
    std::wstring remote = RemoteName, local = LocalName;
    Log(L"FsGetFile \"%s\" -> \"%s\" flags=0x%x (op=%s)", remote.c_str(), local.c_str(), CopyFlags,
        OpName(g_currentOp));
    const Config cfg = CurrentConfig();
    const VfsPath vp = ParseVfsPath(remote);
    if (vp.level != VfsPath::Level::Inside || IsTruncatedMarker(remote.substr(remote.find_last_of(L'\\') + 1))) {
        return FS_FILE_NOTFOUND;
    }
    if (CopyFlags & FS_COPYFLAGS_RESUME) return FS_FILE_NOTSUPPORTED;
    if (!(CopyFlags & FS_COPYFLAGS_OVERWRITE) && GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return FS_FILE_EXISTS;  // TC asks the user, then calls again with OVERWRITE
    }

    DownloadItem item{vp.profile, vp.bucket, vp.key, local, 0};
    if (ri) item.size = (static_cast<unsigned long long>(ri->SizeHigh) << 32) | ri->SizeLow;

    if (CopyFlags & FS_COPYFLAGS_MOVE) {
        // F6 from S3: one `aws s3 mv` per file (download + delete). Kept synchronous, because TC
        // removes the source folders right after the last file.
        if (!CheckWritable(vp)) return FS_FILE_NOTSUPPORTED;
        int r = RunTransferCommand({L"s3", L"mv", S3Uri(vp.bucket, vp.key), local, L"--profile", vp.profile},
                                   S3Uri(vp.bucket, vp.key), local, item.size, vp.profile, FS_FILE_READERROR);
        if (r != FS_FILE_OK) CleanupPartialDownload(local);
        InvalidateParent(NormalizePath(remote));
        return r;
    }

    if (g_currentOp == FS_STATUS_OP_GET_MULTI && cfg.batchDownloads) {
        g_downloadQueue.push_back(std::move(item));
        if (Progress(remote, local, 0) || Progress(remote, local, 100)) return FS_FILE_USERABORT;
        return FS_FILE_OK;  // downloaded at FS_STATUS_END together with the rest of the batch
    }
    int r = DownloadNow(item);
    Log(L"FsGetFile result=%d", r);
    return r;
}

int __stdcall FsGetFile(char* RemoteName, char* LocalName, int CopyFlags, RemoteInfoStruct* ri) {
    std::wstring r = AnsiToWide(RemoteName), l = AnsiToWide(LocalName);
    return FsGetFileW(r.data(), l.data(), CopyFlags, ri);
}

int __stdcall FsPutFileW(WCHAR* LocalName, WCHAR* RemoteName, int CopyFlags) {
    const std::wstring local = LocalName, remote = NormalizePath(RemoteName);
    Log(L"FsPutFile \"%s\" -> \"%s\" flags=0x%x (op=%s)", local.c_str(), remote.c_str(), CopyFlags,
        OpName(g_currentOp));
    const VfsPath vp = ParseVfsPath(remote);
    if (!CheckWritable(vp)) return FS_FILE_NOTSUPPORTED;
    if (CopyFlags & FS_COPYFLAGS_RESUME) return FS_FILE_NOTSUPPORTED;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(local.c_str(), GetFileExInfoStandard, &a)) return FS_FILE_NOTFOUND;
    if (!(CopyFlags & FS_COPYFLAGS_OVERWRITE)) {
        if (CopyFlags & (FS_COPYFLAGS_EXISTS_SAMECASE | FS_COPYFLAGS_EXISTS_DIFFERENTCASE)) return FS_FILE_EXISTS;
        bool isDir = false;
        if (RemoteEntryExists(remote, &isDir)) return FS_FILE_EXISTS;
    }
    UploadItem item{vp.profile, vp.bucket, vp.key, local,
                    (static_cast<unsigned long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow};

    const bool move = (CopyFlags & FS_COPYFLAGS_MOVE) != 0;
    if (!move && g_currentOp == FS_STATUS_OP_PUT_MULTI && CurrentConfig().batchDownloads) {
        g_uploadQueue.push_back(std::move(item));
        g_uploadPaths.push_back(remote);
        if (Progress(local, remote, 0) || Progress(local, remote, 100)) return FS_FILE_USERABORT;
        return FS_FILE_OK;  // uploaded at FS_STATUS_END together with the rest of the batch
    }
    // Single files and moves (TC deletes the moved source folders right away): synchronous.
    int r = RunTransferCommand({L"s3", move ? L"mv" : L"cp", local, S3Uri(vp.bucket, vp.key), L"--profile", vp.profile},
                               local, S3Uri(vp.bucket, vp.key), item.size, vp.profile, FS_FILE_WRITEERROR);
    InvalidateParent(remote);
    Log(L"FsPutFile result=%d", r);
    return r;
}

int __stdcall FsPutFile(char* LocalName, char* RemoteName, int CopyFlags) {
    std::wstring l = AnsiToWide(LocalName), r = AnsiToWide(RemoteName);
    return FsPutFileW(l.data(), r.data(), CopyFlags);
}

// ---------------------------------------------------------------------------------------------
// Folders, deletion, rename/move inside S3

BOOL __stdcall FsMkDirW(WCHAR* Path) {
    const std::wstring path = NormalizePath(Path);
    Log(L"FsMkDir \"%s\" (op=%s)", path.c_str(), OpName(g_currentOp));
    const VfsPath vp = ParseVfsPath(path);
    if (!CheckWritable(vp)) return FALSE;
    if (g_currentOp == FS_STATUS_OP_PUT_MULTI) {
        // Uploads into the folder make it exist; a marker is created at the end only if it stays empty.
        g_newDirs.insert(path);
        g_mkdirQueue.push_back(path);
        return TRUE;
    }
    AwsContext ctx{AwsExe(), vp.profile, nullptr};
    AwsError err;
    if (!PutDirMarker(ctx, vp.bucket, vp.key + L"/", &err)) {
        ShowOpMessageOnce(DescribeAwsError(err, vp.profile));
        return FALSE;
    }
    InvalidateParent(path);
    return TRUE;
}

BOOL __stdcall FsMkDir(char* Path) {
    std::wstring p = AnsiToWide(Path);
    return FsMkDirW(p.data());
}

// Deletes now (outside of an operation) or queues for one delete-objects batch at FS_STATUS_END.
static BOOL DeleteKey(const std::wstring& path, const VfsPath& vp, const std::wstring& key, bool isDir) {
    if (g_currentOp != 0) {
        g_deletes.keys[{vp.profile, vp.bucket}].push_back(key);
        (isDir ? g_deletes.dirs : g_deletes.files).push_back(path);
        return TRUE;
    }
    AwsContext ctx{AwsExe(), vp.profile, nullptr};
    AwsError err;
    std::vector<DeleteFailure> failures;
    bool ok = DeleteObjects(ctx, vp.bucket, {key}, &failures, &err);
    InvalidateParent(path);
    if (isDir) g_cache.InvalidateTree(path);
    if (!ok) ShowOpMessageOnce(DescribeAwsError(err, vp.profile));
    if (!failures.empty()) ShowOpMessageOnce(failures[0].key + L"\n" + failures[0].message);
    return ok && failures.empty();
}

BOOL __stdcall FsDeleteFileW(WCHAR* RemoteName) {
    const std::wstring path = NormalizePath(RemoteName);
    Log(L"FsDeleteFile \"%s\" (op=%s)", path.c_str(), OpName(g_currentOp));
    const VfsPath vp = ParseVfsPath(path);
    if (IsTruncatedMarker(LeafName(path)) || !CheckWritable(vp)) return FALSE;
    return DeleteKey(path, vp, vp.key, false);
}

BOOL __stdcall FsDeleteFile(char* RemoteName) {
    std::wstring r = AnsiToWide(RemoteName);
    return FsDeleteFileW(r.data());
}

BOOL __stdcall FsRemoveDirW(WCHAR* RemoteName) {
    const std::wstring path = NormalizePath(RemoteName);
    Log(L"FsRemoveDir \"%s\" (op=%s)", path.c_str(), OpName(g_currentOp));
    const VfsPath vp = ParseVfsPath(path);
    if (!CheckWritable(vp)) return FALSE;
    // TC removes a folder after deleting its contents; only the marker object may be left.
    return DeleteKey(path, vp, vp.key + L"/", true);
}

BOOL __stdcall FsRemoveDir(char* RemoteName) {
    std::wstring r = AnsiToWide(RemoteName);
    return FsRemoveDirW(r.data());
}

int __stdcall FsRenMovFileW(WCHAR* OldName, WCHAR* NewName, BOOL Move, BOOL OverWrite, RemoteInfoStruct* ri) {
    const std::wstring oldPath = NormalizePath(OldName), newPath = NormalizePath(NewName);
    Log(L"FsRenMovFile \"%s\" -> \"%s\" move=%d overwrite=%d dir=%d (op=%s)", oldPath.c_str(), newPath.c_str(),
        Move, OverWrite, ri && ri->SizeHigh == 0xFFFFFFFF, OpName(g_currentOp));
    const VfsPath from = ParseVfsPath(oldPath), to = ParseVfsPath(newPath);
    if (from.level != VfsPath::Level::Inside || to.level != VfsPath::Level::Inside) return FS_FILE_NOTSUPPORTED;
    if (from.profile != to.profile) {
        ShowOpMessageOnce(L"Copying or moving between different AWS profiles is not supported.\n"
                          L"Download to a local folder and upload from there instead.");
        return FS_FILE_NOTSUPPORTED;
    }
    if (!CheckWritable(to) || (Move && !CheckWritable(from))) return FS_FILE_NOTSUPPORTED;
    if (oldPath == newPath) return FS_FILE_OK;

    bool isDir = ri && ri->SizeHigh == 0xFFFFFFFF;  // TC marks folders this way
    bool targetIsDir = false;
    if (!OverWrite && RemoteEntryExists(newPath, &targetIsDir)) return FS_FILE_EXISTS;

    // Server-side copy/move. A folder is everything below its prefix.
    const std::wstring srcPrefix = from.key + (isDir ? L"/" : L"");
    const std::wstring dstPrefix = to.key + (isDir ? L"/" : L"");
    const std::wstring src = S3Uri(from.bucket, srcPrefix), dst = S3Uri(to.bucket, dstPrefix);
    unsigned long long bytes = (!isDir && ri) ? ((static_cast<unsigned long long>(ri->SizeHigh) << 32) | ri->SizeLow) : 0;
    AwsContext ctx{AwsExe(), from.profile, nullptr};
    std::vector<std::wstring> markers;  // folder marker keys below the source (relative, ending in '/')
    bool hasFiles = !isDir;
    std::vector<std::wstring> fileKeys;
    if (isDir) {
        S3Listing flat;
        AwsError err;
        if (!ListTree(ctx, from.bucket, srcPrefix, 0, &flat, &err)) {
            ShowOpMessageOnce(DescribeAwsError(err, from.profile));
            return FS_FILE_READERROR;
        }
        for (const auto& e : flat.entries) {
            if (e.name.empty() || e.name.back() == L'/') {
                markers.push_back(e.name);
            } else {
                fileKeys.push_back(e.name);
                bytes += e.size;
            }
        }
        hasFiles = !fileKeys.empty();
    }
    int r = FS_FILE_OK;
    if (hasFiles) {
        std::vector<std::wstring> args = {L"s3", Move ? L"mv" : L"cp", src, dst};
        if (isDir) args.push_back(L"--recursive");
        args.push_back(L"--profile");
        args.push_back(from.profile);
        r = RunTransferCommand(args, src, dst, bytes, from.profile, FS_FILE_WRITEERROR);
    }
    if (r == FS_FILE_OK && isDir) {
        // `aws s3 mv/cp --recursive` skips folder marker objects ("a/"). Recreate the markers that
        // keep otherwise empty folders alive, then drop the old ones.
        AwsError err;
        for (const auto& m : markers) {
            bool needed = std::none_of(fileKeys.begin(), fileKeys.end(),
                                       [&](const std::wstring& f) { return f.compare(0, m.size(), m) == 0; });
            if (needed && !PutDirMarker(ctx, to.bucket, dstPrefix + m, &err)) {
                ShowOpMessageOnce(DescribeAwsError(err, from.profile));
                r = FS_FILE_WRITEERROR;
                break;
            }
        }
        if (r == FS_FILE_OK && Move && !markers.empty()) {
            std::vector<std::wstring> keys;
            for (const auto& m : markers) keys.push_back(srcPrefix + m);
            std::vector<DeleteFailure> failures;
            if (!DeleteObjects(ctx, from.bucket, keys, &failures, &err) || !failures.empty()) {
                ShowOpMessageOnce(L"The folder was moved, but some old folder markers could not be deleted.");
            }
        }
    }

    InvalidateParent(newPath);
    if (Move) InvalidateParent(oldPath);
    if (isDir) {
        g_cache.InvalidateTree(newPath);
        if (Move) g_cache.InvalidateTree(oldPath);
    }
    Log(L"FsRenMovFile result=%d", r);
    return r;
}

int __stdcall FsRenMovFile(char* OldName, char* NewName, BOOL Move, BOOL OverWrite, RemoteInfoStruct* ri) {
    std::wstring o = AnsiToWide(OldName), n = AnsiToWide(NewName);
    return FsRenMovFileW(o.data(), n.data(), Move, OverWrite, ri);
}

// ---------------------------------------------------------------------------------------------
// Commands and status notifications

int __stdcall FsExecuteFileW(HWND, WCHAR* RemoteName, WCHAR* Verb) {
    std::wstring remote = RemoteName, verb = Verb;
    Log(L"FsExecuteFile \"%s\" verb=\"%s\"", remote.c_str(), verb.c_str());
    if (_wcsnicmp(verb.c_str(), L"quote ", 6) == 0) return HandleCommand(remote, verb.substr(6));
    if (_wcsicmp(verb.c_str(), L"open") == 0) {
        std::wstring name = remote.substr(remote.find_last_of(L'\\') + 1);
        if (IsTruncatedMarker(name)) return FS_EXEC_OK;
        return FS_EXEC_YOURSELF;  // TC downloads the file to a temp dir and opens it
    }
    if (_wcsicmp(verb.c_str(), L"properties") == 0 && NormalizePath(remote) == L"\\") {
        ShowMessage(L"Settings file:\n" + CurrentConfig().iniPath + L"\n\n" + kHelp);
    }
    return FS_EXEC_OK;
}

int __stdcall FsExecuteFile(HWND MainWin, char* RemoteName, char* Verb) {
    std::wstring r = AnsiToWide(RemoteName), v = AnsiToWide(Verb);
    return FsExecuteFileW(MainWin, r.data(), v.data());
}

void __stdcall FsStatusInfoW(WCHAR* RemoteDir, int InfoStartEnd, int InfoOperation) {
    const bool start = InfoStartEnd == FS_STATUS_START;
    if (start) ReloadConfig();
    Log(L"FsStatusInfo %s %s \"%s\"", start ? L"START" : L"END  ", OpName(InfoOperation),
        RemoteDir ? RemoteDir : L"");
    if (InfoOperation == FS_STATUS_OP_LIST) {
        g_inPanelListing = start;
        if (!start) {
            SetEvent(g_listEndEvent);  // a pending "go to s3://" may send its CD now
            g_disk.SaveDirty(&g_cache, false);  // debounced while navigating
        }
        return;
    }
    if (!start) {
        // TC's progress dialog is still open here and Cancel works (measured).
        if (InfoOperation == FS_STATUS_OP_GET_MULTI) RunQueuedDownloads();
        if (InfoOperation == FS_STATUS_OP_PUT_MULTI) RunQueuedUploads();
        RunQueuedDeletes();
        g_disk.SaveDirty(&g_cache, true);
    }
    g_downloadQueue.clear();
    g_uploadQueue.clear();
    g_uploadPaths.clear();
    g_mkdirQueue.clear();
    g_newDirs.clear();
    g_deletes = DeleteQueue{};
    g_opTreeRoots.clear();
    g_opSubtrees.clear();
    g_opAborted = false;
    g_opMessageShown = false;
    g_opRemoteDir = start && RemoteDir ? NormalizePath(RemoteDir) : std::wstring();
    g_currentOp = start ? InfoOperation : 0;
    g_opWindow.reset();  // closes the window of the previous operation
    g_opFolders = 0;
    if (start && InfoOperation == FS_STATUS_OP_CALCSIZE) {
        g_opWindow = std::make_shared<ProgressWindow>(L"S3: calculating folder size");
    }
}

void __stdcall FsStatusInfo(char* RemoteDir, int InfoStartEnd, int InfoOperation) {
    std::wstring d = AnsiToWide(RemoteDir);
    FsStatusInfoW(d.data(), InfoStartEnd, InfoOperation);
}

// Writes pending disk cache changes when TC unloads the plugin (also on TC exit).
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) g_module = instance;
    if (reason == DLL_PROCESS_DETACH) g_disk.SaveDirty(&g_cache, true);
    return TRUE;
}

// Background transfers: TC shows a "Background" button in the F5/F6 progress dialog and runs the
// whole operation (FsStatusInfo START..END, FsGetFile/FsPutFile, FsFindFirst for the pre-scan) in
// one dedicated thread. No BG_ASK_USER: aws.exe calls need no separate connection.
int __stdcall FsGetBackgroundFlags(void) { return BG_DOWNLOAD | BG_UPLOAD; }
