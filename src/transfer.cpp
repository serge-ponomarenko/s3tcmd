#include "transfer.h"

#include <windows.h>

#include <map>
#include <set>
#include <tuple>

#include "../sdk/fsplugin_cpp.h"
#include "aws_cli.h"
#include "errors.h"
#include "log.h"
#include "names.h"
#include "progress.h"
#include "util.h"

namespace s3tc {

namespace {

// Stay well below the 32767-character CreateProcess limit.
constexpr size_t kMaxCommandLine = 28000;
// At most this many keys are excluded from a whole-tree command (each costs a separate command).
constexpr size_t kMaxSubtreeExcludes = 50;

std::wstring BaseName(const std::wstring& p, wchar_t sep) {
    size_t pos = p.find_last_of(sep);
    return pos == std::wstring::npos ? p : p.substr(pos + 1);
}

std::wstring ParentOf(const std::wstring& p, wchar_t sep) {
    size_t pos = p.find_last_of(sep);
    return pos == std::wstring::npos ? std::wstring() : p.substr(0, pos);
}

// True if every '/'-separated segment is shown in TC under its own name (no substitutions), so
// aws.exe writing raw key names produces exactly the paths TC expects.
bool SegmentsAreWindowsSafe(const std::wstring& rel) {
    size_t start = 0;
    for (;;) {
        size_t slash = rel.find(L'/', start);
        std::wstring seg = rel.substr(start, slash == std::wstring::npos ? std::wstring::npos : slash - start);
        if (seg.empty() || EncodeName(seg) != seg) return false;
        if (slash == std::wstring::npos) return true;
        start = slash + 1;
    }
}

std::wstring SlashToBackslash(std::wstring s) {
    for (auto& c : s) {
        if (c == L'/') c = L'\\';
    }
    return s;
}

CpCommand SingleCommand(const DownloadItem& item, size_t index) {
    CpCommand c;
    c.kind = CpCommand::Kind::Single;
    c.profile = item.profile;
    c.bucket = item.bucket;
    c.source = item.key;
    c.target = item.local;
    c.items = {index};
    c.bytes = item.size;
    return c;
}

// Tries to cover all queued files of a subtree with one --recursive command.
bool PlanSubtree(const SubtreeInfo& s, const std::vector<DownloadItem>& items, std::vector<bool>* used,
                 std::vector<CpCommand>* out) {
    std::vector<size_t> idx;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        if ((*used)[i] || it.profile != s.profile || it.bucket != s.bucket) continue;
        if (it.key.size() > s.prefix.size() && it.key.compare(0, s.prefix.size(), s.prefix) == 0) idx.push_back(i);
    }
    if (idx.empty() || idx.size() != s.files.size()) return false;

    std::set<std::wstring> expected(s.files.begin(), s.files.end());
    std::wstring localRoot;
    // Keys whose names need look-alike characters on Windows (':' '*' ...) cannot be written by
    // the CLI under their raw names: exclude them from the recursive command and copy each one to
    // the name TC chose. A few of them must not spoil the single command for the whole tree.
    std::vector<size_t> unsafe;
    std::vector<size_t> safe;
    for (size_t i : idx) {
        std::wstring rel = items[i].key.substr(s.prefix.size());
        if (!expected.erase(rel)) return false;
        if (!SegmentsAreWindowsSafe(rel)) {
            unsafe.push_back(i);
            continue;
        }
        safe.push_back(i);
        const std::wstring relLocal = L"\\" + SlashToBackslash(rel);
        const std::wstring& local = items[i].local;
        if (local.size() <= relLocal.size() ||
            _wcsicmp(local.c_str() + local.size() - relLocal.size(), relLocal.c_str()) != 0) {
            return false;  // renamed on copy, or mapped differently
        }
        std::wstring root = local.substr(0, local.size() - relLocal.size());
        if (localRoot.empty()) {
            localRoot = root;
        } else if (_wcsicmp(localRoot.c_str(), root.c_str()) != 0) {
            return false;
        }
    }
    if (!expected.empty() || safe.empty() || unsafe.size() > kMaxSubtreeExcludes) return false;

    CpCommand c;
    c.kind = CpCommand::Kind::Subtree;
    c.profile = s.profile;
    c.bucket = s.bucket;
    c.source = s.prefix;
    c.target = localRoot;
    for (size_t i : safe) {
        c.items.push_back(i);
        c.bytes += items[i].size;
        (*used)[i] = true;
    }
    for (size_t i : unsafe) c.excludes.push_back(items[i].key.substr(s.prefix.size()));
    out->push_back(std::move(c));
    for (size_t i : unsafe) {
        out->push_back(SingleCommand(items[i], i));
        (*used)[i] = true;
    }
    return true;
}

}  // namespace

std::wstring FormatBytes(double bytes) {
    static const wchar_t* kUnits[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    int u = 0;
    while (bytes >= 1024.0 && u < 4) {
        bytes /= 1024.0;
        ++u;
    }
    wchar_t buf[32];
    swprintf_s(buf, u == 0 ? L"%.0f %s" : L"%.1f %s", bytes, kUnits[u]);
    return buf;
}

std::wstring EscapeFilterPattern(const std::wstring& name) {
    std::wstring out;
    for (wchar_t c : name) {
        if (c == L'*' || c == L'?' || c == L'[') {
            out.push_back(L'[');
            out.push_back(c);
            out.push_back(L']');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::vector<CpCommand> PlanDownloads(const std::vector<DownloadItem>& items,
                                     const std::vector<SubtreeInfo>& subtrees,
                                     const SubtreeKeyCountFn& subtreeKeyCount) {
    std::vector<CpCommand> out;
    std::vector<bool> used(items.size(), false);

    for (const auto& s : subtrees) PlanSubtree(s, items, &used, &out);

    // Group the rest by (profile, bucket, S3 directory, local directory).
    using GroupKey = std::tuple<std::wstring, std::wstring, std::wstring, std::wstring>;
    std::map<GroupKey, std::vector<size_t>> groups;
    for (size_t i = 0; i < items.size(); ++i) {
        if (used[i]) continue;
        const auto& it = items[i];
        const std::wstring name = BaseName(it.key, L'/');
        const bool sameName = !name.empty() && EncodeName(name) == name &&
                              BaseName(it.local, L'\\') == name;
        if (!sameName) {
            out.push_back(SingleCommand(it, i));
            used[i] = true;
            continue;
        }
        std::wstring dir = ParentOf(it.key, L'/');
        if (!dir.empty()) dir += L'/';
        groups[{it.profile, it.bucket, dir, ParentOf(it.local, L'\\')}].push_back(i);
    }

    for (const auto& [key, idx] : groups) {
        const auto& [profile, bucket, dir, localDir] = key;
        long long listing = idx.size() >= 2 && subtreeKeyCount ? subtreeKeyCount(profile, bucket, dir, idx.size()) : -1;
        if (listing < 0 || listing > kMaxFilteredListing) {
            for (size_t i : idx) out.push_back(SingleCommand(items[i], i));
            continue;
        }
        CpCommand cur;
        auto flush = [&]() {
            if (!cur.items.empty()) out.push_back(std::move(cur));
            cur = CpCommand{};
            cur.kind = CpCommand::Kind::Filtered;
            cur.profile = profile;
            cur.bucket = bucket;
            cur.source = dir;
            cur.target = localDir;
        };
        flush();
        size_t length = 0;
        for (size_t i : idx) {
            const std::wstring name = BaseName(items[i].key, L'/');
            size_t add = EscapeFilterPattern(name).size() + 16;
            if (!cur.items.empty() && length + add > kMaxCommandLine - 1000) {
                flush();
                length = 0;
            }
            cur.includes.push_back(name);
            cur.items.push_back(i);
            cur.bytes += items[i].size;
            length += add;
        }
        flush();
    }
    return out;
}

std::vector<std::wstring> BuildCpArgs(const CpCommand& cmd) {
    std::vector<std::wstring> args =
        cmd.upload ? std::vector<std::wstring>{L"s3", L"cp", cmd.source, L"s3://" + cmd.bucket + L"/" + cmd.target}
                   : std::vector<std::wstring>{L"s3", L"cp", L"s3://" + cmd.bucket + L"/" + cmd.source, cmd.target};
    if (cmd.kind != CpCommand::Kind::Single) {
        args.push_back(L"--recursive");
    }
    if (cmd.kind == CpCommand::Kind::Filtered) {
        args.push_back(L"--exclude");
        args.push_back(L"*");
        for (const auto& name : cmd.includes) {
            args.push_back(L"--include");
            args.push_back(EscapeFilterPattern(name));
        }
    }
    for (const auto& rel : cmd.excludes) {
        args.push_back(L"--exclude");
        args.push_back(EscapeFilterPattern(rel));
    }
    args.push_back(L"--profile");
    args.push_back(cmd.profile);
    return args;
}

void CleanupPartialDownload(const std::wstring& local) {
    const std::wstring dir = DirectoryOf(local);
    const std::wstring base = BaseName(local, L'\\');
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((local + L".*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name.size() != base.size() + 9 || name[base.size()] != L'.') continue;
        bool alnum = true;
        for (size_t i = base.size() + 1; i < name.size(); ++i) alnum = alnum && iswalnum(name[i]);
        if (alnum) {
            Log(L"cleanup: deleting partial download %s%s", dir.c_str(), name.c_str());
            DeleteFileW((dir + name).c_str());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

namespace {

int FsCodeForError(const std::wstring& text) {
    if (text.find(L"Errno") != std::wstring::npos || text.find(L"WinError") != std::wstring::npos ||
        text.find(L"No space") != std::wstring::npos || text.find(L"Permission denied") != std::wstring::npos) {
        return FS_FILE_WRITEERROR;
    }
    AwsError e = ClassifyAwsError(text);
    return e.kind == AwsErrorKind::NoSuchKey || e.kind == AwsErrorKind::NoSuchBucket ? FS_FILE_NOTFOUND
                                                                                      : FS_FILE_READERROR;
}

// A download counts as done if the file exists and either the CLI reported success for the whole
// command or the size matches (the CLI may fail a batch partially).
bool LocalFileMatches(const std::wstring& path, unsigned long long size, bool commandOk) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    unsigned long long actual = (static_cast<unsigned long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    return commandOk || actual == size;
}

// Finds the CLI's error line about one file ("download failed: s3://b/k to ... <error>").
std::wstring ErrorFor(const std::wstring& err, const std::wstring& uri) {
    size_t pos = err.find(uri + L" ");
    if (pos == std::wstring::npos) return {};
    size_t lineStart = err.rfind(L'\n', pos);
    size_t lineEnd = err.find(L'\n', pos);
    return err.substr(lineStart == std::wstring::npos ? 0 : lineStart + 1,
                      lineEnd == std::wstring::npos ? std::wstring::npos : lineEnd - lineStart - 1);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Batch execution with progress

std::wstring BatchProgress::Label(const std::wstring& src, double done) const {
    double secs = static_cast<double>(GetTickCount64() - start) / 1000.0;
    std::wstring label = src + L"   [" + FormatBytes(done);
    if (totalBytes) label += L" of " + FormatBytes(static_cast<double>(totalBytes));
    if (secs >= 1.0) label += L", " + FormatBytes(done / secs) + L"/s";
    if (totalFiles > 1) label += L", " + std::to_wstring(totalFiles) + L" files";
    return label + L"]";
}

AwsResult RunCpStep(const std::wstring& awsExe, const std::vector<std::wstring>& args, const std::wstring& src,
                    const std::wstring& dst, unsigned long long stepBytes, BatchProgress* bp) {
    double stepDone = 0;
    // TC's own total bar and speed are meaningless for a deferred batch (all FsGetFile/FsPutFile
    // calls returned instantly), so the real numbers go into the source name line.
    auto report = [&]() -> bool {
        double done = static_cast<double>(bp->doneBytes) + (std::min)(stepDone, static_cast<double>(stepBytes));
        int pct = bp->totalBytes ? static_cast<int>(done * 100.0 / static_cast<double>(bp->totalBytes)) : 0;
        pct = (std::max)(0, (std::min)(pct, 100));
        return bp->progress(bp->Label(src, done), dst, pct) != 0;
    };
    AwsResult r;
    if (bp->cancelled || report()) {
        bp->cancelled = true;
        r.error = L"Cancelled";
        return r;
    }
    LineSplitter splitter([&](const std::string& line) {
        CpProgress p;
        if (ParseCpProgress(line, &p)) {
            stepDone = p.doneBytes;
            if (report()) bp->cancelled = true;
        }
    });
    ULONGLONG lastPoll = GetTickCount64();
    ProcessOptions opt;
    opt.onStdout = [&](const std::string& chunk) { splitter.Feed(chunk); };
    opt.shouldCancel = [&]() {
        // Keep the dialog responsive even while the CLI prints nothing.
        if (!bp->cancelled && GetTickCount64() - lastPoll > 500) {
            lastPoll = GetTickCount64();
            if (report()) bp->cancelled = true;
        }
        return bp->cancelled;
    };
    r = RunAws(awsExe, args, opt);
    splitter.Flush();
    bp->doneBytes += stepBytes;
    return r;
}

DownloadResult RunDownloads(const std::wstring& awsExe, const std::vector<DownloadItem>& items,
                            const std::vector<CpCommand>& commands, const TransferProgressFn& progress) {
    DownloadResult result;
    BatchProgress bp;
    bp.progress = progress;
    for (const auto& c : commands) {
        bp.totalBytes += c.bytes;
        bp.totalFiles += c.items.size();
    }

    for (const CpCommand& cmd : commands) {
        if (bp.cancelled) break;
        AwsResult r = RunCpStep(awsExe, BuildCpArgs(cmd), L"s3://" + cmd.bucket + L"/" + cmd.source, cmd.target,
                                cmd.bytes, &bp);
        const std::wstring errText = Utf8ToWide(r.raw.err);
        for (size_t i : cmd.items) {
            const DownloadItem& it = items[i];
            if (LocalFileMatches(it.local, it.size, r.ok && !bp.cancelled)) continue;
            CleanupPartialDownload(it.local);
            if (bp.cancelled) continue;
            std::wstring msg = ErrorFor(errText, L"s3://" + it.bucket + L"/" + it.key);
            if (msg.empty()) msg = r.ok ? L"file missing or size mismatch after download" : r.error;
            result.failures.push_back({i, FsCodeForError(msg), msg});
        }
    }
    result.cancelled = bp.cancelled;
    if (result.cancelled) {
        // Files of the interrupted command may have left temp files behind.
        for (const auto& it : items) CleanupPartialDownload(it.local);
    } else if (!commands.empty()) {
        const CpCommand& last = commands.back();
        progress(bp.Label(L"s3://" + last.bucket + L"/" + last.source, static_cast<double>(bp.totalBytes)),
                 last.target, 100);
    }
    return result;
}

// ---------------------------------------------------------------------------------------------
// Uploads

namespace {

// True if every '\'-separated local segment becomes the same S3 key segment, i.e. the name has no
// look-alike characters that the plugin would decode into different key characters.
bool LocalSegmentsMapVerbatim(const std::wstring& relBackslash) {
    size_t start = 0;
    for (;;) {
        size_t sep = relBackslash.find(L'\\', start);
        std::wstring seg = relBackslash.substr(start, sep == std::wstring::npos ? std::wstring::npos : sep - start);
        if (seg.empty() || DecodeName(seg) != seg || EncodeName(seg) != seg) return false;
        if (sep == std::wstring::npos) return true;
        start = sep + 1;
    }
}

std::wstring BackslashToSlash(std::wstring s) {
    for (auto& c : s) {
        if (c == L'\\') c = L'/';
    }
    return s;
}

CpCommand SingleUpload(const UploadItem& item, size_t index) {
    CpCommand c;
    c.upload = true;
    c.kind = CpCommand::Kind::Single;
    c.profile = item.profile;
    c.bucket = item.bucket;
    c.source = item.local;
    c.target = item.key;
    c.items = {index};
    c.bytes = item.size;
    return c;
}

}  // namespace

std::vector<CpCommand> PlanUploads(const std::vector<UploadItem>& items, const std::wstring& opPrefix,
                                   const LocalTreeFn& localTree) {
    std::vector<CpCommand> out;
    std::vector<bool> used(items.size(), false);

    // 1. Whole local folders uploaded into the operation's target directory: group items by the
    //    first path segment below opPrefix ("dirA/..." -> local ...\dirA).
    using TopKey = std::tuple<std::wstring, std::wstring, std::wstring, std::wstring>;  // profile, bucket, prefix, localTop
    std::map<TopKey, std::vector<size_t>> tops;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        if (it.key.size() <= opPrefix.size() || it.key.compare(0, opPrefix.size(), opPrefix) != 0) continue;
        const std::wstring rel = it.key.substr(opPrefix.size());
        const size_t slash = rel.find(L'/');
        if (slash == std::wstring::npos) continue;  // a file directly in the target directory
        const std::wstring relLocal = L"\\" + SlashToBackslash(rel.substr(slash + 1));
        if (it.local.size() <= relLocal.size() ||
            _wcsicmp(it.local.c_str() + it.local.size() - relLocal.size(), relLocal.c_str()) != 0) {
            continue;
        }
        const std::wstring localTop = it.local.substr(0, it.local.size() - relLocal.size());
        if (BaseName(localTop, L'\\') != rel.substr(0, slash)) continue;
        tops[{it.profile, it.bucket, opPrefix + rel.substr(0, slash + 1), localTop}].push_back(i);
    }
    for (const auto& [key, idx] : tops) {
        const auto& [profile, bucket, prefix, localTop] = key;
        std::vector<std::wstring> onDisk;
        if (!localTree || !localTree(localTop, &onDisk) || onDisk.size() != idx.size()) continue;
        std::set<std::wstring> expected(onDisk.begin(), onDisk.end());
        bool ok = true;
        for (size_t i : idx) {
            const std::wstring relBack = items[i].local.substr(localTop.size() + 1);
            ok = ok && expected.erase(relBack) == 1 && LocalSegmentsMapVerbatim(relBack) &&
                 items[i].key == prefix + BackslashToSlash(relBack);
        }
        if (!ok || !expected.empty()) continue;
        CpCommand c;
        c.upload = true;
        c.kind = CpCommand::Kind::Subtree;
        c.profile = profile;
        c.bucket = bucket;
        c.source = localTop;
        c.target = prefix;
        for (size_t i : idx) {
            c.items.push_back(i);
            c.bytes += items[i].size;
            used[i] = true;
        }
        out.push_back(std::move(c));
    }

    // 2. Several files of one local folder into one S3 folder with unchanged names: filtered.
    using GroupKey = std::tuple<std::wstring, std::wstring, std::wstring, std::wstring>;  // profile, bucket, keyDir, localDir
    std::map<GroupKey, std::vector<size_t>> groups;
    for (size_t i = 0; i < items.size(); ++i) {
        if (used[i]) continue;
        const auto& it = items[i];
        const std::wstring name = BaseName(it.key, L'/');
        if (name.empty() || name != BaseName(it.local, L'\\') || !LocalSegmentsMapVerbatim(name)) {
            out.push_back(SingleUpload(it, i));
            used[i] = true;
            continue;
        }
        std::wstring dir = ParentOf(it.key, L'/');
        if (!dir.empty()) dir += L'/';
        groups[{it.profile, it.bucket, dir, ParentOf(it.local, L'\\')}].push_back(i);
    }
    for (const auto& [key, idx] : groups) {
        const auto& [profile, bucket, dir, localDir] = key;
        // The CLI walks the whole local source folder before filtering: only for small folders.
        std::vector<std::wstring> onDisk;
        const bool small = idx.size() >= 2 && localTree && localTree(localDir, &onDisk) &&
                           static_cast<long long>(onDisk.size()) <= kMaxFilteredListing;
        if (!small) {
            for (size_t i : idx) out.push_back(SingleUpload(items[i], i));
            continue;
        }
        CpCommand cur;
        size_t length = 0;
        auto flush = [&]() {
            if (!cur.items.empty()) out.push_back(std::move(cur));
            cur = CpCommand{};
            cur.upload = true;
            cur.kind = CpCommand::Kind::Filtered;
            cur.profile = profile;
            cur.bucket = bucket;
            cur.source = localDir;
            cur.target = dir;
            length = 0;
        };
        flush();
        for (size_t i : idx) {
            const std::wstring name = BaseName(items[i].key, L'/');
            size_t add = EscapeFilterPattern(name).size() + 16;
            if (!cur.items.empty() && length + add > kMaxCommandLine - 1000) flush();
            cur.includes.push_back(name);
            cur.items.push_back(i);
            cur.bytes += items[i].size;
            length += add;
        }
        flush();
    }
    return out;
}

UploadResult RunUploads(const std::wstring& awsExe, const std::vector<UploadItem>& items,
                        const std::vector<CpCommand>& commands, const TransferProgressFn& progress) {
    UploadResult result;
    result.succeeded.assign(items.size(), false);
    BatchProgress bp;
    bp.progress = progress;
    for (const auto& c : commands) {
        bp.totalBytes += c.bytes;
        bp.totalFiles += c.items.size();
    }
    for (const CpCommand& cmd : commands) {
        if (bp.cancelled) break;
        AwsResult r = RunCpStep(awsExe, BuildCpArgs(cmd), cmd.source, L"s3://" + cmd.bucket + L"/" + cmd.target,
                                cmd.bytes, &bp);
        const std::wstring errText = Utf8ToWide(r.raw.err);
        for (size_t i : cmd.items) {
            const UploadItem& it = items[i];
            if (r.ok) {
                result.succeeded[i] = true;
                continue;
            }
            if (bp.cancelled) continue;
            // In a partially failed batch the CLI prints "upload failed: <local> to s3://... <error>"
            // for each failed file; files without such a line were uploaded.
            std::wstring msg = ErrorFor(errText, it.local);
            if (msg.empty() && cmd.kind != CpCommand::Kind::Single &&
                errText.find(L"upload failed:") != std::wstring::npos) {
                result.succeeded[i] = true;
                continue;
            }
            if (msg.empty()) msg = r.error;
            int code = msg.find(L"does not exist") != std::wstring::npos ? FS_FILE_NOTFOUND : FS_FILE_WRITEERROR;
            result.failures.push_back({i, code, msg});
        }
    }
    result.cancelled = bp.cancelled;
    if (!result.cancelled && !commands.empty()) {
        const CpCommand& last = commands.back();
        progress(bp.Label(last.source, static_cast<double>(bp.totalBytes)), L"s3://" + last.bucket + L"/" + last.target,
                 100);
    }
    return result;
}

bool ListLocalTree(const std::wstring& dir, std::vector<std::wstring>* files, size_t limit) {
    files->clear();
    std::vector<std::wstring> stack = {L""};
    while (!stack.empty()) {
        const std::wstring rel = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        const std::wstring pattern = dir + L"\\" + (rel.empty() ? L"" : rel + L"\\") + L"*";
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            if (rel.empty()) return false;
            continue;
        }
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring childRel = rel.empty() ? name : rel + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) stack.push_back(childRel);
            } else {
                files->push_back(childRel);
                if (files->size() > limit) {
                    FindClose(h);
                    return false;
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return true;
}

}  // namespace s3tc
