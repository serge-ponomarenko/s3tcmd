// Integration tests against real AWS. READ-ONLY: never writes to the bucket.
//
// Configuration (environment variables; without them the test is skipped):
//   S3TCMD_IT_PROFILE  AWS CLI profile
//   S3TCMD_IT_BUCKET   bucket readable with that profile
//   S3TCMD_IT_PREFIX   optional folder to test in ("dir/sub/"; default: bucket root). It should
//                      contain subfolders and some small files (a few MB); the test looks for a
//                      folder with at least 4 files for the paging checks and a small leaf folder
//                      for the download checks, and skips those parts if there is none.
// Command-line arguments [profile] [bucket] [prefix] override the variables.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "../src/aws_cli.h"
#include "../src/aws_s3.h"
#include "../src/dir_tree.h"
#include "../src/names.h"
#include "../src/transfer.h"

using namespace s3tc;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_failures;                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

static std::wstring Setting(int argc, wchar_t** argv, int index, const wchar_t* var) {
    if (argc > index) return argv[index];
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(var, buf, _countof(buf));
    return (n > 0 && n < _countof(buf)) ? std::wstring(buf) : std::wstring();
}

static size_t CountFiles(const std::vector<S3Entry>& entries) {
    return static_cast<size_t>(std::count_if(entries.begin(), entries.end(), [](const S3Entry& e) { return !e.isDir; }));
}

int wmain(int argc, wchar_t** argv) {
    const std::wstring profile = Setting(argc, argv, 1, L"S3TCMD_IT_PROFILE");
    const std::wstring bucket = Setting(argc, argv, 2, L"S3TCMD_IT_BUCKET");
    std::wstring prefix = Setting(argc, argv, 3, L"S3TCMD_IT_PREFIX");
    if (profile.empty() || bucket.empty()) {
        std::printf("SKIPPED: set S3TCMD_IT_PROFILE and S3TCMD_IT_BUCKET (and optionally S3TCMD_IT_PREFIX)\n");
        return 0;
    }
    if (!prefix.empty() && prefix.back() != L'/') prefix.push_back(L'/');
    SetDisableImds(true);
    AwsContext ctx{ResolveAwsExe(L"aws.exe"), profile, nullptr};
    std::wprintf(L"aws=%ls profile=%ls s3://%ls/%ls\n", ctx.awsExe.c_str(), profile.c_str(), bucket.c_str(),
                 prefix.c_str());

    std::vector<std::wstring> profiles;
    std::wstring perr;
    CHECK(ListProfiles(ctx.awsExe, &profiles, &perr));
    CHECK(std::find(profiles.begin(), profiles.end(), profile) != profiles.end());

    AwsError err;
    std::vector<S3Entry> buckets;
    if (ListBuckets(ctx, &buckets, &err)) {
        // A bucket of another account is readable but not listed; that is what PinnedBuckets is for.
        const bool listed =
            std::any_of(buckets.begin(), buckets.end(), [&](const S3Entry& e) { return e.name == bucket; });
        std::printf("  buckets: %zu (test bucket %s)\n", buckets.size(),
                    listed ? "listed" : "not listed - cross-account, needs PinnedBuckets");
    } else {
        std::printf("  list-buckets failed (allowed: PinnedBuckets covers it)\n");
    }

    S3Listing top;
    CHECK(ListFolder(ctx, bucket, prefix, 0, &top, &err));
    CHECK(!top.entries.empty() && !top.truncated);
    std::printf("  entries in the test folder: %zu\n", top.entries.size());

    // One flat listing of the test folder (capped) must agree with the folder listing.
    S3Listing flat;
    DWORD t0 = GetTickCount();
    CHECK(ListTree(ctx, bucket, prefix, 5000, &flat, &err));
    std::printf("  tree: %zu keys in %lu ms%s\n", flat.entries.size(), GetTickCount() - t0,
                flat.truncated ? " (capped at 5000)" : "");
    auto tree = BuildDirectoryTree(flat.entries, flat.truncated);
    if (!flat.truncated) CHECK(tree[L""].entries.size() == top.entries.size());
    if (flat.entries.size() > 5) {
        S3Listing capTree;
        CHECK(ListTree(ctx, bucket, prefix, 5, &capTree, &err));
        CHECK(capTree.entries.size() == 5 && capTree.truncated);
    }

    // Paging: a folder with at least 4 files, listed with a cap of 3 files.
    std::wstring paged, leaf;
    for (const auto& [dir, d] : tree) {
        if (!d.complete) continue;
        const size_t files = CountFiles(d.entries);
        if (paged.empty() && files >= 4) paged = dir;
        // Download candidate: 2..20 files, no subfolders, <= 20 MB, Windows-safe names.
        unsigned long long bytes = 0;
        bool simple = files >= 2 && files <= 20 && files == d.entries.size();
        for (const auto& e : d.entries) {
            bytes += e.size;
            simple = simple && !e.name.empty() && e.name.back() != L'/' && EncodeName(e.name) == e.name;
        }
        if (leaf.empty() && simple && bytes <= 20ULL * 1024 * 1024) leaf = dir;
    }
    if (!paged.empty()) {
        S3Listing capped, full;
        CHECK(ListFolder(ctx, bucket, prefix + paged, 3, &capped, &err));
        // --max-items caps Contents (files) only; CommonPrefixes come on top.
        CHECK(CountFiles(capped.entries) == 3 && capped.truncated);
        CHECK(ListFolder(ctx, bucket, prefix + paged, 0, &full, &err));
        CHECK(CountFiles(full.entries) >= 4 && !full.truncated);
        for (const auto& e : full.entries) {
            if (!e.isDir) CHECK(e.hasTime);
        }
    } else {
        std::printf("  SKIPPED paging: no folder with 4+ files in the tree\n");
    }

    // Download a small leaf folder with one subtree command, then with one filtered command.
    if (!leaf.empty()) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring out = std::wstring(tmp) + L"s3tcmd_it_" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(out.c_str(), nullptr);
        const std::wstring leafPrefix = prefix + leaf;
        SubtreeInfo info{profile, bucket, leafPrefix, {}};
        std::vector<DownloadItem> items;
        for (const auto& e : tree[leaf].entries) {
            info.files.push_back(e.name);
            items.push_back({profile, bucket, leafPrefix + e.name, out + L"\\" + e.name, e.size});
        }
        auto cmds = PlanDownloads(items, {info}, nullptr);
        CHECK(cmds.size() == 1 && cmds[0].kind == CpCommand::Kind::Subtree);
        auto noop = [](const std::wstring&, const std::wstring&, int) { return 0; };
        DownloadResult r = RunDownloads(ctx.awsExe, items, cmds, noop);
        CHECK(!r.cancelled && r.failures.empty());
        for (const auto& it : items) {
            CHECK(GetFileAttributesW(it.local.c_str()) != INVALID_FILE_ATTRIBUTES);
            DeleteFileW(it.local.c_str());
        }
        auto count = [](const std::wstring&, const std::wstring&, const std::wstring&, size_t) { return 4LL; };
        cmds = PlanDownloads(items, {}, count);
        CHECK(cmds.size() == 1 && cmds[0].kind == CpCommand::Kind::Filtered);
        r = RunDownloads(ctx.awsExe, items, cmds, noop);
        CHECK(!r.cancelled && r.failures.empty());
        for (const auto& it : items) {
            CHECK(GetFileAttributesW(it.local.c_str()) != INVALID_FILE_ATTRIBUTES);
            DeleteFileW(it.local.c_str());
        }
        RemoveDirectoryW(out.c_str());
        std::wprintf(L"  downloads of %ls: %zu files, subtree + filtered OK\n", leaf.c_str(), items.size());
    } else {
        std::printf("  SKIPPED downloads: no small leaf folder (2-20 files, <= 20 MB) in the tree\n");
    }

    S3Listing missing;
    CHECK(ListFolder(ctx, bucket, prefix + L"no-such-prefix-s3tcmd-test/", 0, &missing, &err));
    CHECK(missing.entries.empty());

    AwsContext bad = ctx;
    bad.profile = L"s3tcmd-no-such-profile";
    CHECK(!ListFolder(bad, bucket, L"", 0, &missing, &err));
    CHECK(err.kind == AwsErrorKind::ProfileNotFound);
    if (err.kind != AwsErrorKind::ProfileNotFound) std::wprintf(L"  got: %ls\n", err.detail.c_str());

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
