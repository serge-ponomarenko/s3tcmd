// Integration tests for write operations against real AWS.
// Writes ONLY below a unique prefix "<base>s3tcmd-it-<pid>-<tick>/" and deletes it at the end.
//
// Configuration (environment variables; without them the test is skipped):
//   S3TCMD_ITW_PROFILE     profile allowed to write below the base prefix
//   S3TCMD_ITW_BUCKET      bucket
//   S3TCMD_ITW_PREFIX      base prefix, e.g. "tests/" (may be empty = bucket root)
//   S3TCMD_ITW_RO_PROFILE  optional: a profile that can read but not delete there (AccessDenied check)
// Command-line arguments [write-profile] [bucket] [base-prefix] [read-only-profile] override them.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "../src/aws_cli.h"
#include "../src/aws_s3.h"
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

static void WriteLocal(const std::wstring& path, const std::string& content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD w = 0;
    WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &w, nullptr);
    CloseHandle(h);
}

static bool HasEntry(const S3Listing& l, const std::wstring& name, bool dir) {
    return std::any_of(l.entries.begin(), l.entries.end(),
                       [&](const S3Entry& e) { return e.name == name && e.isDir == dir; });
}

static std::wstring Setting(int argc, wchar_t** argv, int index, const wchar_t* var) {
    if (argc > index) return argv[index];
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(var, buf, _countof(buf));
    return (n > 0 && n < _countof(buf)) ? std::wstring(buf) : std::wstring();
}

int wmain(int argc, wchar_t** argv) {
    const std::wstring profile = Setting(argc, argv, 1, L"S3TCMD_ITW_PROFILE");
    const std::wstring bucket = Setting(argc, argv, 2, L"S3TCMD_ITW_BUCKET");
    std::wstring basePrefix = Setting(argc, argv, 3, L"S3TCMD_ITW_PREFIX");
    const std::wstring roProfile = Setting(argc, argv, 4, L"S3TCMD_ITW_RO_PROFILE");
    if (profile.empty() || bucket.empty()) {
        std::printf("SKIPPED: set S3TCMD_ITW_PROFILE and S3TCMD_ITW_BUCKET (and S3TCMD_ITW_PREFIX)\n");
        return 0;
    }
    if (!basePrefix.empty() && basePrefix.back() != L'/') basePrefix.push_back(L'/');
    const std::wstring base = basePrefix + L"s3tcmd-it-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(GetTickCount64() % 100000) + L"/";
    SetDisableImds(true);
    AwsContext ctx{ResolveAwsExe(L"aws.exe"), profile, nullptr};
    std::wprintf(L"write test in s3://%ls/%ls (profile %ls)\n", bucket.c_str(), base.c_str(), profile.c_str());
    auto noop = [](const std::wstring&, const std::wstring&, int) { return 0; };

    // Local tree: dirA/{a1.txt, sub/s1.txt, "тест файл.txt"}, f1.txt, f2.txt
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring local = std::wstring(tmp) + L"s3tcmd_itw_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(local.c_str(), nullptr);
    CreateDirectoryW((local + L"\\dirA").c_str(), nullptr);
    CreateDirectoryW((local + L"\\dirA\\sub").c_str(), nullptr);
    WriteLocal(local + L"\\dirA\\a1.txt", std::string(100, 'a'));
    WriteLocal(local + L"\\dirA\\sub\\s1.txt", "s1");
    WriteLocal(local + L"\\dirA\\тест файл.txt", "cyrillic");
    WriteLocal(local + L"\\f1.txt", "f1");
    WriteLocal(local + L"\\f2.txt", "f2");

    std::vector<UploadItem> items = {
        {profile, bucket, base + L"dirA/a1.txt", local + L"\\dirA\\a1.txt", 100},
        {profile, bucket, base + L"dirA/sub/s1.txt", local + L"\\dirA\\sub\\s1.txt", 2},
        {profile, bucket, base + L"dirA/тест файл.txt", local + L"\\dirA\\тест файл.txt", 8},
        {profile, bucket, base + L"f1.txt", local + L"\\f1.txt", 2},
        {profile, bucket, base + L"f2.txt", local + L"\\f2.txt", 2},
    };
    auto tree = [](const std::wstring& dir, std::vector<std::wstring>* files) { return ListLocalTree(dir, files, 1000); };
    auto cmds = PlanUploads(items, base, tree);
    CHECK(cmds.size() == 2);
    if (cmds.size() == 2) {
        CHECK(cmds[0].kind == CpCommand::Kind::Subtree);
        CHECK(cmds[1].kind == CpCommand::Kind::Filtered);
    }
    UploadResult up = RunUploads(ctx.awsExe, items, cmds, noop);
    CHECK(!up.cancelled && up.failures.empty());
    CHECK(std::all_of(up.succeeded.begin(), up.succeeded.end(), [](bool b) { return b; }));

    S3Listing flat;
    AwsError err;
    CHECK(ListTree(ctx, bucket, base, 0, &flat, &err));
    CHECK(flat.entries.size() == 5);
    CHECK(std::any_of(flat.entries.begin(), flat.entries.end(),
                      [](const S3Entry& e) { return e.name == L"dirA/тест файл.txt" && e.size == 8; }));

    // F7: folder marker
    CHECK(PutDirMarker(ctx, bucket, base + L"empty/", &err));
    S3Listing top;
    CHECK(ListFolder(ctx, bucket, base, 0, &top, &err));
    CHECK(HasEntry(top, L"empty", true) && HasEntry(top, L"dirA", true) && HasEntry(top, L"f1.txt", false));

    // F6 of a folder: one server-side recursive move
    AwsResult mv = RunAws(ctx.awsExe, {L"s3", L"mv", L"s3://" + bucket + L"/" + base + L"dirA/",
                                       L"s3://" + bucket + L"/" + base + L"dirB/", L"--recursive", L"--profile", profile});
    CHECK(mv.ok);
    CHECK(ListFolder(ctx, bucket, base, 0, &top, &err));
    CHECK(HasEntry(top, L"dirB", true) && !HasEntry(top, L"dirA", true));

    // F8 without permission: per-key AccessDenied (nothing is deleted)
    if (!roProfile.empty()) {
        AwsContext ro{ctx.awsExe, roProfile, nullptr};
        std::vector<DeleteFailure> roFails;
        AwsError roErr;
        bool roOk = DeleteObjects(ro, bucket, {base + L"f1.txt"}, &roFails, &roErr);
        CHECK(!roOk || !roFails.empty());
        std::wprintf(L"  read-only delete: %ls\n",
                     !roFails.empty() ? roFails[0].message.c_str() : roErr.detail.substr(0, 120).c_str());
    } else {
        std::printf("  SKIPPED read-only delete: S3TCMD_ITW_RO_PROFILE not set\n");
    }

    // F8: delete everything with delete-objects
    CHECK(ListTree(ctx, bucket, base, 0, &flat, &err));
    std::vector<std::wstring> keys;
    for (const auto& e : flat.entries) keys.push_back(base + e.name);
    std::vector<DeleteFailure> fails;
    CHECK(DeleteObjects(ctx, bucket, keys, &fails, &err));
    CHECK(fails.empty());
    CHECK(ListTree(ctx, bucket, base, 0, &flat, &err));
    CHECK(flat.entries.empty());

    // Safety net: remove anything left below the unique prefix.
    RunAws(ctx.awsExe, {L"s3", L"rm", L"s3://" + bucket + L"/" + base, L"--recursive", L"--profile", profile});
    for (const wchar_t* f : {L"\\dirA\\a1.txt", L"\\dirA\\sub\\s1.txt", L"\\dirA\\тест файл.txt", L"\\f1.txt", L"\\f2.txt"}) {
        DeleteFileW((local + f).c_str());
    }
    RemoveDirectoryW((local + L"\\dirA\\sub").c_str());
    RemoveDirectoryW((local + L"\\dirA").c_str());
    RemoveDirectoryW(local.c_str());

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
