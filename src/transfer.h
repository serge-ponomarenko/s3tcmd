// Transfers via `aws s3 cp`: planning a batch of downloads or uploads into few CLI commands,
// running them with progress and cancellation, verifying results and cleaning up after cancel.
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "aws_cli.h"

namespace s3tc {

struct DownloadItem {
    std::wstring profile, bucket;
    std::wstring key;    // decoded S3 key
    std::wstring local;  // full local target path chosen by TC
    unsigned long long size = 0;
};

struct UploadItem {
    std::wstring profile, bucket;
    std::wstring key;    // decoded S3 target key
    std::wstring local;  // full local source path
    unsigned long long size = 0;
};

// All files of a subtree, as known from an operation-scoped tree listing.
struct SubtreeInfo {
    std::wstring profile, bucket;
    std::wstring prefix;              // "" or "dir/sub/"
    std::vector<std::wstring> files;  // keys relative to prefix (files only, no folder markers)
};

// One `aws s3 cp` invocation. Download: source = S3 key/prefix, target = local file/dir.
// Upload (upload == true): source = local file/dir, target = S3 key/prefix.
struct CpCommand {
    enum class Kind { Single, Filtered, Subtree };
    Kind kind = Kind::Single;
    bool upload = false;
    std::wstring profile, bucket;
    std::wstring source;
    std::wstring target;
    std::vector<std::wstring> includes;  // file names (Filtered), unescaped
    std::vector<std::wstring> excludes;  // relative keys left out of a Subtree command, unescaped
    std::vector<size_t> items;           // indexes into the planned item list
    unsigned long long bytes = 0;
};

// Number of keys under a prefix, recursively (what `aws s3 cp --recursive` would have to list),
// or -1 if unknown or larger than kMaxFilteredListing.
using SubtreeKeyCountFn =
    std::function<long long(const std::wstring& profile, const std::wstring& bucket, const std::wstring& prefix,
                            size_t queuedFiles)>;

// Lists all files below a local directory (relative paths with '\'); false if it fails or exceeds a limit.
using LocalTreeFn = std::function<bool(const std::wstring& dir, std::vector<std::wstring>* files)>;

// A filtered --recursive command lists the whole source subtree before filtering, so it is only
// used when that subtree has at most this many entries (about 4 s of S3 listing).
constexpr long long kMaxFilteredListing = 5000;

// Turns queued downloads into as few `aws s3 cp` invocations as is safe:
//  - a whole subtree queued with matching local paths -> one --recursive command;
//  - 2+ files of one directory with unchanged names and a small subtree -> --recursive with
//    --exclude "*" --include <name>...;
//  - everything else -> one command per file.
std::vector<CpCommand> PlanDownloads(const std::vector<DownloadItem>& items,
                                     const std::vector<SubtreeInfo>& subtrees,
                                     const SubtreeKeyCountFn& subtreeKeyCount);

// The same for uploads. `opPrefix` is the S3 prefix of the operation's target directory: a local
// folder uploaded there completely (all files on disk queued, names unchanged) becomes one
// --recursive command.
std::vector<CpCommand> PlanUploads(const std::vector<UploadItem>& items, const std::wstring& opPrefix,
                                   const LocalTreeFn& localTree);

// aws.exe arguments for a command.
std::vector<std::wstring> BuildCpArgs(const CpCommand& cmd);

// "1.5 MiB" style size.
std::wstring FormatBytes(double bytes);

// Escapes a file name for an aws CLI --include/--exclude pattern (fnmatch).
std::wstring EscapeFilterPattern(const std::wstring& name);

// Progress callback: returns non-zero to cancel (TC's ProgressProc semantics).
using TransferProgressFn = std::function<int(const std::wstring& src, const std::wstring& dst, int pct)>;

// Shared progress state of a batch of `aws s3` commands.
struct BatchProgress {
    TransferProgressFn progress;
    unsigned long long totalBytes = 0;
    unsigned long long doneBytes = 0;  // bytes of finished steps
    size_t totalFiles = 0;
    ULONGLONG start = GetTickCount64();
    bool cancelled = false;
    // "<src>   [X of Y, speed, N files]"
    std::wstring Label(const std::wstring& src, double done) const;
};

// Runs one `aws s3 cp/mv` command, mapping its "Completed X/Y" output onto the batch progress.
AwsResult RunCpStep(const std::wstring& awsExe, const std::vector<std::wstring>& args, const std::wstring& src,
                    const std::wstring& dst, unsigned long long stepBytes, BatchProgress* bp);

struct TransferFailure {
    size_t item;
    int fsCode;  // FS_FILE_* code
    std::wstring message;
};

struct DownloadResult {
    bool cancelled = false;
    std::vector<TransferFailure> failures;
};

struct UploadResult {
    bool cancelled = false;
    std::vector<TransferFailure> failures;
    std::vector<bool> succeeded;  // per item
};

DownloadResult RunDownloads(const std::wstring& awsExe, const std::vector<DownloadItem>& items,
                            const std::vector<CpCommand>& commands, const TransferProgressFn& progress);

UploadResult RunUploads(const std::wstring& awsExe, const std::vector<UploadItem>& items,
                        const std::vector<CpCommand>& commands, const TransferProgressFn& progress);

// Deletes `local.XXXXXXXX` leftovers that aws s3 cp creates while downloading.
void CleanupPartialDownload(const std::wstring& local);

// Recursive local file listing for PlanUploads (skips reparse points); false beyond `limit` files.
bool ListLocalTree(const std::wstring& dir, std::vector<std::wstring>* files, size_t limit);

}  // namespace s3tc
