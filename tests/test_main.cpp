// Unit tests (no network). Build with build.bat; exits non-zero on failure.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/aws_s3.h"
#include "../src/clip_paths.h"
#include "../src/dir_cache.h"
#include "../src/dir_tree.h"
#include "../src/disk_cache.h"
#include "../src/errors.h"
#include "../src/names.h"
#include "../src/process.h"
#include "../src/s3_uri.h"
#include "../src/setup.h"
#include "../src/progress.h"
#include "../src/transfer.h"
#include "../src/util.h"
#include "../src/vfs_path.h"

using namespace s3tc;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

static void TestQuoteArgRoundTrip() {
    const std::vector<std::wstring> cases = {
        L"simple",
        L"with space",
        L"",
        L"trailing\\",
        L"trailing space\\",
        L"quote\"inside",
        L"back\\\\\"slash",
        L"C:\\Program Files\\x\\",
        L"s3://bucket/key with spaces/тест €.txt",
        L"--include",
        L"*",
        L"a\tb",
    };
    for (const auto& c : cases) {
        std::wstring cmd = L"prog.exe " + QuoteArg(c);
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(cmd.c_str(), &argc);
        CHECK(argv != nullptr);
        CHECK(argc == 2);
        if (argv && argc == 2) {
            CHECK(c == argv[1]);
            if (c != argv[1]) std::printf("  case: %ls -> %ls\n", c.c_str(), argv[1]);
        }
        LocalFree(argv);
    }
    CHECK(QuoteArg(L"plain") == L"plain");
}

static void TestSplitLines() {
    auto v = SplitLines("a\r\nb\rc\n\nd");
    CHECK(v.size() == 4);
    if (v.size() == 4) {
        CHECK(v[0] == "a");
        CHECK(v[1] == "b");
        CHECK(v[2] == "c");
        CHECK(v[3] == "d");
    }
    CHECK(SplitLines("").empty());
}

static void TestUtf8() {
    std::wstring w = L"тест € 中文";
    CHECK(Utf8ToWide(WideToUtf8(w)) == w);
    CHECK(DirectoryOf(L"C:\\a\\b.ini") == L"C:\\a\\");
    CHECK(DirectoryOf(L"b.ini").empty());
}

static std::wstring CmdExe() {
    wchar_t buf[MAX_PATH];
    GetEnvironmentVariableW(L"ComSpec", buf, MAX_PATH);
    return buf;
}

static void TestRunProcess() {
    ProcessOptions opt;
    opt.env.push_back(L"S3TC_TEST_VAR=hello42");
    std::string streamed;
    opt.onStdout = [&](const std::string& c) { streamed += c; };
    ProcessResult r = RunProcess(CmdExe(), {L"/c", L"echo %S3TC_TEST_VAR%& echo err 1>&2& exit 3"}, opt);
    CHECK(r.started);
    CHECK(r.exitCode == 3);
    CHECK(r.out.find("hello42") != std::string::npos);
    CHECK(r.err.find("err") != std::string::npos);
    CHECK(streamed == r.out);

    ProcessResult bad = RunProcess(L"C:\\does\\not\\exist.exe", {}, {});
    CHECK(!bad.started);
    CHECK(bad.winError != 0);
}

static void TestCancel() {
    ProcessOptions opt;
    DWORD start = GetTickCount();
    opt.shouldCancel = [&]() { return GetTickCount() - start > 300; };
    // ping keeps running ~10 s unless killed.
    ProcessResult r = RunProcess(CmdExe(), {L"/c", L"ping -n 10 127.0.0.1"}, opt);
    CHECK(r.started);
    CHECK(r.cancelled);
    CHECK(r.elapsedMs < 3000);
}

static bool IsValidWindowsName(const std::wstring& n) {
    if (n.empty()) return false;
    if (n.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return false;
    for (wchar_t c : n) {
        if (c < 0x20) return false;
    }
    return n.back() != L'.' && n.back() != L' ';
}

static void TestNames() {
    const std::vector<std::wstring> cases = {
        L"plain.txt", L"", L"a:b", L"what?", L"x*y", L"q\"uote", L"<tag>", L"pi|pe", L"back\\slash",
        L"dot.", L"space ", L"..", L".", L"tab\there", L"\x01\x1f", L"\xFF1A already fullwidth",
        L"\x201B escape", L"\x201B", L"\xFF0E", L"\x2420", L"тест €.txt", L"mid. dot", L" lead",
    };
    for (const auto& c : cases) {
        std::wstring enc = EncodeName(c);
        CHECK(IsValidWindowsName(enc));
        CHECK(DecodeName(enc) == c);
        if (DecodeName(enc) != c) std::printf("  name case failed (len %zu)\n", c.size());
    }
    CHECK(EncodeName(L"plain.txt") == L"plain.txt");
    CHECK(EncodeName(L"a:b") == L"a\xFF1A" L"b");
}

static void TestVfsPath() {
    VfsPath p = ParseVfsPath(L"\\");
    CHECK(p.level == VfsPath::Level::Root);
    p = ParseVfsPath(L"\\prof-a\\");
    CHECK(p.level == VfsPath::Level::Profile && p.profile == L"prof-a");
    p = ParseVfsPath(L"\\prof-a\\bucket");
    CHECK(p.level == VfsPath::Level::Bucket && p.bucket == L"bucket" && p.key.empty());
    CHECK(ListPrefix(p).empty());
    p = ParseVfsPath(L"\\prof-a\\bucket\\a\\b c\\d\xFF1A" L"e");
    CHECK(p.level == VfsPath::Level::Inside);
    CHECK(p.key == L"a/b c/d:e");
    CHECK(ListPrefix(p) == L"a/b c/d:e/");
    CHECK(NormalizePath(L"\\x\\") == L"\\x");
    CHECK(NormalizePath(L"\\") == L"\\");
    CHECK(NormalizePath(L"") == L"\\");
}

static void TestIsoTime() {
    FILETIME a{}, b{}, c{};
    CHECK(ParseIsoTime("2026-05-29T18:51:26+00:00", &a));
    CHECK(ParseIsoTime("2026-05-29T18:51:26.000Z", &b));
    CHECK(ParseIsoTime("2026-05-29T20:51:26+02:00", &c));
    CHECK(CompareFileTime(&a, &b) == 0);
    CHECK(CompareFileTime(&a, &c) == 0);
    SYSTEMTIME st;
    FileTimeToSystemTime(&a, &st);
    CHECK(st.wYear == 2026 && st.wMonth == 5 && st.wDay == 29 && st.wHour == 18 && st.wSecond == 26);
    CHECK(!ParseIsoTime("garbage", &a));
}

static void TestParseJson() {
    std::vector<S3Entry> buckets;
    CHECK(ParseBucketsJson(R"([["b1","2016-02-22T14:10:36+00:00"],["b2",null]])", &buckets, nullptr));
    CHECK(buckets.size() == 2);
    if (buckets.size() == 2) {
        CHECK(buckets[0].name == L"b1" && buckets[0].isDir && buckets[0].hasTime);
        CHECK(buckets[1].name == L"b2" && !buckets[1].hasTime);
    }
    CHECK(ParseBucketsJson("null", &buckets, nullptr) && buckets.empty());

    S3Listing l;
    const char* json = R"({"p":["data/EU/","data/NA/"],
        "c":[["data/","0","x"],["data/",0,"2026-01-01T00:00:00+00:00"],
             ["data/readme.txt",590,"2026-05-29T18:51:26+00:00"],
             ["data/тест.bin",5000000000,"2026-05-29T18:51:26+00:00"],
             ["data/empty/",0,"2026-05-29T18:51:26+00:00"]],
        "t":"eyJ0b2tlbiI6IDF9"})";
    std::wstring err;
    CHECK(ParseListObjectsJson(json, "data/", &l, &err));
    CHECK(l.truncated);
    CHECK(l.entries.size() == 5);
    if (l.entries.size() == 5) {
        CHECK(l.entries[0].name == L"EU" && l.entries[0].isDir);
        CHECK(l.entries[1].name == L"NA" && l.entries[1].isDir);
        CHECK(l.entries[2].name == L"readme.txt" && !l.entries[2].isDir && l.entries[2].size == 590);
        CHECK(l.entries[3].name == L"тест.bin" && l.entries[3].size == 5000000000ULL);
        CHECK(l.entries[4].name == L"empty" && l.entries[4].isDir);
    }
    CHECK(ParseListObjectsJson(R"({"p":null,"c":null,"t":null})", "", &l, nullptr));
    CHECK(l.entries.empty() && !l.truncated);
    CHECK(!ParseListObjectsJson("not json", "", &l, &err));
    CHECK(!err.empty());
}

static void TestErrors() {
    CHECK(ClassifyAwsError(L"").kind == AwsErrorKind::None);
    CHECK(ClassifyAwsError(L"An error occurred (AccessDenied) when calling the ListObjectsV2 operation: x")
              .kind == AwsErrorKind::AccessDenied);
    CHECK(ClassifyAwsError(L"An error occurred (InvalidAccessKeyId) when calling ...").kind ==
          AwsErrorKind::AccessDenied);
    CHECK(ClassifyAwsError(L"Error when retrieving token from sso: Token has expired and refresh failed")
              .kind == AwsErrorKind::SsoExpired);
    CHECK(ClassifyAwsError(L"An error occurred (ExpiredToken) when calling ...").kind ==
          AwsErrorKind::CredentialsExpired);
    CHECK(ClassifyAwsError(L"Unable to locate credentials. You can configure ...").kind ==
          AwsErrorKind::NoCredentials);
    CHECK(ClassifyAwsError(L"The config profile (zzz) could not be found").kind ==
          AwsErrorKind::ProfileNotFound);
    CHECK(ClassifyAwsError(L"An error occurred (NoSuchBucket) when calling ...").kind ==
          AwsErrorKind::NoSuchBucket);
    CHECK(ClassifyAwsError(L"Could not connect to the endpoint URL: ...").kind == AwsErrorKind::Network);
    CHECK(ClassifyAwsError(L"Cancelled").kind == AwsErrorKind::Cancelled);
    AwsError sso{AwsErrorKind::SsoExpired, L"x"};
    CHECK(DescribeAwsError(sso, L"prof-a").find(L"aws sso login --profile prof-a") != std::wstring::npos);
}

static S3Entry Obj(const wchar_t* key, unsigned long long size = 1) {
    S3Entry e;
    e.name = key;
    e.size = size;
    return e;
}

static const S3Entry* FindEntry(const TreeDir& d, const wchar_t* name) {
    for (const auto& e : d.entries) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

static void TestDirectoryTree() {
    std::vector<S3Entry> objs = {Obj(L""),        Obj(L"a/"),      Obj(L"a/1.txt", 10), Obj(L"a/b/2.txt", 20),
                                 Obj(L"a/b/3.txt"), Obj(L"c/empty/"), Obj(L"root.txt", 5), Obj(L"x//y")};
    auto t = BuildDirectoryTree(objs, false);
    CHECK(t.count(L"") && t.count(L"a/") && t.count(L"a/b/") && t.count(L"c/") && t.count(L"c/empty/"));
    CHECK(t.count(L"x/") && t.count(L"x//"));  // empty segment: folder "" inside x
    CHECK(t.size() == 7);
    const TreeDir& root = t[L""];
    CHECK(root.entries.size() == 4);  // a, c, root.txt, x
    CHECK(FindEntry(root, L"a") && FindEntry(root, L"a")->isDir);
    CHECK(FindEntry(root, L"root.txt") && FindEntry(root, L"root.txt")->size == 5);
    CHECK(t[L"a/"].entries.size() == 2);  // 1.txt, b (marker "a/" adds no entry)
    CHECK(t[L"a/b/"].entries.size() == 2);
    CHECK(t[L"c/empty/"].entries.empty());
    CHECK(t[L"x//"].entries.size() == 1 && FindEntry(t[L"x//"], L"y"));
    for (const auto& [rel, d] : t) CHECK(d.complete);

    // Keys starting with '/' (s3://b//x): an empty-named folder below the root, not the root itself.
    std::vector<S3Entry> slash = {Obj(L"/ansible/a.yml"), Obj(L"//deep.txt"), Obj(L"ansible/b.yml"), Obj(L"top.txt")};
    auto ts = BuildDirectoryTree(slash, false);
    CHECK(ts.count(L"/") && ts.count(L"/ansible/") && ts.count(L"//") && ts.count(L"ansible/"));
    CHECK(ts[L""].entries.size() == 3);  // "", ansible, top.txt
    CHECK(FindEntry(ts[L""], L"") && FindEntry(ts[L""], L"")->isDir);
    CHECK(ts[L"/"].entries.size() == 2);  // ansible, ""
    CHECK(ts[L"/ansible/"].entries.size() == 1 && FindEntry(ts[L"/ansible/"], L"a.yml"));
    CHECK(ts[L"ansible/"].entries.size() == 1 && FindEntry(ts[L"ansible/"], L"b.yml"));
    CHECK(ts[L"//"].entries.size() == 1 && FindEntry(ts[L"//"], L"deep.txt"));
    CHECK(SplitDirKey(L"").empty());
    CHECK(SplitDirKey(L"/") == std::vector<std::wstring>{L""});
    CHECK((SplitDirKey(L"a//b/") == std::vector<std::wstring>{L"a", L"", L"b"}));

    // Truncated after "a/b/2.txt": a and a/b may miss items, the root too; nothing after is known.
    std::vector<S3Entry> part = {Obj(L"a/1.txt"), Obj(L"a/b/2.txt"), Obj(L"a0/z.txt"), Obj(L"a/b/3.txt")};
    part.resize(2);
    auto tt = BuildDirectoryTree(part, true);
    CHECK(!tt[L""].complete);
    CHECK(!tt[L"a/"].complete);
    CHECK(!tt[L"a/b/"].complete);
    std::vector<S3Entry> part2 = {Obj(L"a/1.txt"), Obj(L"a/b/2.txt"), Obj(L"z/9.txt")};
    auto t2 = BuildDirectoryTree(part2, true);
    CHECK(t2[L"a/"].complete && t2[L"a/b/"].complete);  // the listing went past them
    CHECK(!t2[L"z/"].complete && !t2[L""].complete);

    S3Listing l;
    CHECK(ParseTreeJson(R"({"c":[["p/","0","x"],["p/a/1.txt",3,"2026-05-29T18:51:26+00:00"],["p/b/",0,null]],"t":null})",
                        "p/", &l, nullptr));
    CHECK(l.entries.size() == 3 && !l.truncated);
    if (l.entries.size() == 3) {
        CHECK(l.entries[0].name.empty());
        CHECK(l.entries[1].name == L"a/1.txt" && l.entries[1].size == 3 && l.entries[1].hasTime);
        CHECK(l.entries[2].name == L"b/");
    }
}

static void TestProgressParse() {
    CpProgress p;
    CHECK(ParseCpProgress("Completed 8.1 MiB/22.2 MiB (3.0 MiB/s) with 3 file(s) remaining", &p));
    CHECK(p.doneBytes > 8.0 * 1048576 && p.doneBytes < 8.2 * 1048576);
    CHECK(p.totalBytes > 22.1 * 1048576 && !p.totalIsEstimate);
    CHECK(ParseCpProgress("Completed 256.0 KiB/~1.2 GiB (1.1 MiB/s) with ~3 file(s) remaining (calculating...)", &p));
    CHECK(p.doneBytes == 256.0 * 1024 && p.totalIsEstimate && p.totalBytes > 1.1 * 1073741824.0);
    CHECK(ParseCpProgress("Completed 0 Bytes/590 Bytes (0 Bytes/s) with 1 file(s) remaining", &p));
    CHECK(p.totalBytes == 590);
    CHECK(!ParseCpProgress("download: s3://b/k to x", &p));

    std::vector<std::string> lines;
    LineSplitter s([&](const std::string& l) { lines.push_back(l); });
    s.Feed("Completed 1 MiB/2 MiB\rCompl");
    s.Feed("eted 2 MiB/2 MiB\rdownload: s3://b/k to x\r\n");
    s.Flush();
    CHECK(lines.size() == 3 && lines[1] == "Completed 2 MiB/2 MiB" && lines[2] == "download: s3://b/k to x");

    CHECK(EscapeFilterPattern(L"a*b?c[d].txt") == L"a[*]b[?]c[[]d].txt");
    CHECK(EscapeFilterPattern(L"plain.txt") == L"plain.txt");
}

static DownloadItem Item(const wchar_t* key, const wchar_t* local, unsigned long long size = 10) {
    return DownloadItem{L"p", L"b", key, local, size};
}

static void TestPlanDownloads() {
    auto noInfo = [](const std::wstring&, const std::wstring&, const std::wstring&, size_t) { return -1LL; };
    auto small = [](const std::wstring&, const std::wstring&, const std::wstring&, size_t) { return 20LL; };

    // A single file: always a plain cp to the exact target.
    auto c1 = PlanDownloads({Item(L"d/f.txt", L"C:\\t\\f.txt")}, {}, small);
    CHECK(c1.size() == 1 && c1[0].kind == CpCommand::Kind::Single && c1[0].target == L"C:\\t\\f.txt");
    auto a1 = BuildCpArgs(c1[0]);
    CHECK(a1.size() >= 4 && a1[2] == L"s3://b/d/f.txt" && a1[3] == L"C:\\t\\f.txt");

    // Several files of one directory with a small subtree: one filtered command.
    std::vector<DownloadItem> many = {Item(L"d/a.txt", L"C:\\t\\a.txt"), Item(L"d/b[1].txt", L"C:\\t\\b[1].txt"),
                                      Item(L"d/c.txt", L"C:\\t\\c.txt")};
    auto c2 = PlanDownloads(many, {}, small);
    CHECK(c2.size() == 1 && c2[0].kind == CpCommand::Kind::Filtered && c2[0].items.size() == 3);
    CHECK(c2[0].source == L"d/" && c2[0].target == L"C:\\t" && c2[0].bytes == 30);
    auto a2 = BuildCpArgs(c2[0]);
    CHECK(a2[2] == L"s3://b/d/" && a2[4] == L"--recursive" && a2[5] == L"--exclude" && a2[6] == L"*");
    CHECK(std::find(a2.begin(), a2.end(), L"b[[]1].txt") != a2.end());
    // Unknown or huge subtree: per-file commands.
    auto c3 = PlanDownloads(many, {}, noInfo);
    CHECK(c3.size() == 3 && c3[0].kind == CpCommand::Kind::Single);

    // Renamed on copy, or a name that needed substitution: per-file.
    auto c4 = PlanDownloads({Item(L"d/a.txt", L"C:\\t\\other.txt"), Item(L"d/x:y", L"C:\\t\\x\xFF1Ay"),
                             Item(L"d/c.txt", L"C:\\t\\c.txt")},
                            {}, small);
    CHECK(c4.size() == 3);

    // A whole subtree with matching local paths: one recursive command.
    SubtreeInfo tree{L"p", L"b", L"root/dirA/", {L"a1.txt", L"a2.txt", L"sub/s1.txt"}};
    std::vector<DownloadItem> sub = {Item(L"root/dirA/a1.txt", L"C:\\t\\dirA\\a1.txt"),
                                     Item(L"root/dirA/a2.txt", L"C:\\t\\dirA\\a2.txt"),
                                     Item(L"root/dirA/sub/s1.txt", L"C:\\t\\dirA\\sub\\s1.txt"),
                                     Item(L"root/f1.txt", L"C:\\t\\f1.txt")};
    auto c5 = PlanDownloads(sub, {tree}, noInfo);
    CHECK(c5.size() == 2);
    if (c5.size() == 2) {
        CHECK(c5[0].kind == CpCommand::Kind::Subtree && c5[0].source == L"root/dirA/" &&
              c5[0].target == L"C:\\t\\dirA" && c5[0].items.size() == 3);
        CHECK(c5[1].kind == CpCommand::Kind::Single && c5[1].source == L"root/f1.txt");
        auto a5 = BuildCpArgs(c5[0]);
        CHECK(a5[2] == L"s3://b/root/dirA/" && a5[3] == L"C:\\t\\dirA" && a5[4] == L"--recursive");
    }
    // A key that needs look-alike characters on Windows is excluded from the tree command and
    // copied on its own to the name TC chose.
    SubtreeInfo odd{L"p", L"b", L"r/", {L"ok.txt", L"sub/a*:b.tgz"}};
    auto c7 = PlanDownloads({Item(L"r/ok.txt", L"C:\\t\\r\\ok.txt"),
                             Item(L"r/sub/a*:b.tgz", L"C:\\t\\r\\sub\\a\xFF0A\xFF1A" L"b.tgz")},
                            {odd}, noInfo);
    CHECK(c7.size() == 2);
    if (c7.size() == 2) {
        CHECK(c7[0].kind == CpCommand::Kind::Subtree && c7[0].items.size() == 1 && c7[0].excludes.size() == 1);
        auto a7 = BuildCpArgs(c7[0]);
        CHECK(std::find(a7.begin(), a7.end(), L"sub/a[*]:b.tgz") != a7.end());
        CHECK(c7[1].kind == CpCommand::Kind::Single && c7[1].target == L"C:\\t\\r\\sub\\a\xFF0A\xFF1A" L"b.tgz");
    }
    // Only part of the subtree queued (e.g. TC copy filter or skipped files): not one command.
    sub.erase(sub.begin());
    auto c6 = PlanDownloads(sub, {tree}, noInfo);
    for (const auto& c : c6) CHECK(c.kind != CpCommand::Kind::Subtree);
}

static UploadItem Up(const wchar_t* local, const wchar_t* key, unsigned long long size = 10) {
    return UploadItem{L"p", L"b", key, local, size};
}

static void TestPlanUploads() {
    // Fake local disk: C:\src\dirA has a1.txt, sub\s1.txt; C:\src has f1.txt, f2.txt (+ dirA).
    auto disk = [](const std::wstring& dir, std::vector<std::wstring>* files) {
        if (dir == L"C:\\src\\dirA") {
            *files = {L"a1.txt", L"sub\\s1.txt"};
            return true;
        }
        if (dir == L"C:\\src") {
            *files = {L"f1.txt", L"f2.txt", L"dirA\\a1.txt", L"dirA\\sub\\s1.txt"};
            return true;
        }
        return false;
    };
    std::vector<UploadItem> items = {Up(L"C:\\src\\dirA\\a1.txt", L"t/dirA/a1.txt"),
                                     Up(L"C:\\src\\dirA\\sub\\s1.txt", L"t/dirA/sub/s1.txt"),
                                     Up(L"C:\\src\\f1.txt", L"t/f1.txt"), Up(L"C:\\src\\f2.txt", L"t/f2.txt")};
    auto c = PlanUploads(items, L"t/", disk);
    CHECK(c.size() == 2);
    if (c.size() == 2) {
        CHECK(c[0].kind == CpCommand::Kind::Subtree && c[0].upload && c[0].source == L"C:\\src\\dirA" &&
              c[0].target == L"t/dirA/" && c[0].items.size() == 2);
        CHECK(c[1].kind == CpCommand::Kind::Filtered && c[1].source == L"C:\\src" && c[1].target == L"t/" &&
              c[1].items.size() == 2);
        auto a = BuildCpArgs(c[0]);
        CHECK(a[2] == L"C:\\src\\dirA" && a[3] == L"s3://b/t/dirA/" && a[4] == L"--recursive");
        auto f = BuildCpArgs(c[1]);
        CHECK(f[2] == L"C:\\src" && f[3] == L"s3://b/t/" && f[5] == L"--exclude");
    }
    // A file of dirA skipped by the user (exists, not overwritten): no whole-folder command.
    std::vector<UploadItem> partial = {items[1], items[2]};
    auto p = PlanUploads(partial, L"t/", disk);
    for (const auto& cmd : p) CHECK(cmd.kind != CpCommand::Kind::Subtree);
    // Single file; and a name with a look-alike character (decoded key differs): per-file.
    auto s = PlanUploads({Up(L"C:\\src\\f1.txt", L"t/f1.txt")}, L"t/", disk);
    CHECK(s.size() == 1 && s[0].kind == CpCommand::Kind::Single);
    auto a = BuildCpArgs(s[0]);
    CHECK(a[1] == L"cp" && a[2] == L"C:\\src\\f1.txt" && a[3] == L"s3://b/t/f1.txt");
    auto w = PlanUploads({Up(L"C:\\src\\x\xFF1Ay", L"t/x:y"), Up(L"C:\\src\\f1.txt", L"t/f1.txt")}, L"t/", disk);
    CHECK(w.size() == 2 && w[0].kind == CpCommand::Kind::Single && w[1].kind == CpCommand::Kind::Single);

    // delete-objects body and error parsing
    std::vector<std::wstring> keys = {L"a/b.txt", L"тест \"q\".txt", L"dir/"};
    std::string body = BuildDeleteJson(keys, 0, 3);
    CHECK(body.find("\"Quiet\":true") != std::string::npos);
    CHECK(body.find("\"Key\":\"a/b.txt\"") != std::string::npos);
    CHECK(body.find("\\\"q\\\"") != std::string::npos);
    CHECK(BuildDeleteJson(keys, 1, 2).find("a/b.txt") == std::string::npos);
    std::vector<DeleteFailure> fails;
    CHECK(ParseDeleteErrors("", &fails) && fails.empty());
    CHECK(ParseDeleteErrors(R"({"Errors":[{"Key":"k1","Code":"AccessDenied","Message":"Access Denied"}]})", &fails));
    CHECK(fails.size() == 1 && fails[0].key == L"k1" && fails[0].message == L"AccessDenied: Access Denied");
    CHECK(!ParseDeleteErrors("{bad", &fails));
}

static CacheEntry CE(const wchar_t* name, bool dir, unsigned long long size = 0) {
    CacheEntry e;
    e.cFileName = name;
    e.dwFileAttributes = dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    e.size = size;
    e.ftLastWriteTime.dwHighDateTime = 0x01DCABCD;
    e.ftLastWriteTime.dwLowDateTime = 0x12345678;
    return e;
}

static void TestDiskCache() {
    CHECK(DirCache::GroupOf(L"\\") == L"\\");
    CHECK(DirCache::GroupOf(L"\\p") == L"\\");
    CHECK(DirCache::GroupOf(L"\\p\\b") == L"\\p\\b");
    CHECK(DirCache::GroupOf(L"\\p\\b\\x\\y") == L"\\p\\b");

    DirCache c;
    c.Put(L"\\", {CE(L"p", true)});
    c.Put(L"\\p", {CE(L"b", true), CE(L"bb", true)});
    c.Put(L"\\p\\b", {CE(L"тест файл.txt", false, 5000000000ULL), CE(L"\x201B", true), CE(L"a\xFF1A" L"b", false)}, true);
    c.Put(L"\\p\\b\\\x201B", {CE(L"x", false, 1)}, true);
    c.Put(L"\\p\\b!", {CE(L"other", false)});
    c.Put(L"\\p\\bb", {CE(L"z", false)});
    auto dirty = c.TakeDirtyGroups();
    CHECK(dirty.size() == 4);  // "\", "\p\b", "\p\b!", "\p\bb"
    CHECK(c.TakeDirtyGroups().empty());
    auto g = c.ExportGroup(L"\\p\\b");
    CHECK(g.size() == 2);  // "\p\b" and "\p\b\‛", not "\p\b!" or "\p\bb"
    CHECK(c.ExportGroup(L"\\").size() == 2);

    std::string text = DiskCache::Serialize(L"\\p\\b", g);
    std::wstring group;
    std::vector<std::pair<std::wstring, std::shared_ptr<const CachedDir>>> back;
    CHECK(DiskCache::Parse(text, &group, &back, 0));
    CHECK(group == L"\\p\\b" && back.size() == 2);
    if (back.size() == 2) {
        CHECK(back[0].first == L"\\p\\b" && back[0].second->subtreeComplete);
        CHECK(back[0].second->items.size() == 3);
        const auto& e0 = back[0].second->items[0];
        CHECK(e0.cFileName == L"тест файл.txt" && e0.size == 5000000000ULL && !(e0.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
        CHECK(e0.ftLastWriteTime.dwHighDateTime == 0x01DCABCD && e0.ftLastWriteTime.dwLowDateTime == 0x12345678);
        CHECK(back[0].second->items[1].cFileName == L"\x201B" && (back[0].second->items[1].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY));
        CHECK(back[1].first == L"\\p\\b\\\x201B" && back[1].second->items.size() == 1);
    }
    // Old listings are skipped; malformed files are rejected.
    std::string old = "S3TCMD-CACHE\t1\t\\p\\b\nD\t1000\t0\t\\p\\b\nE\t10\t0\t0\t0\tx\n";
    CHECK(DiskCache::Parse(old, &group, &back, 60000) && back.empty());
    CHECK(DiskCache::Parse(old, &group, &back, 0) && back.size() == 1);
    CHECK(!DiskCache::Parse("garbage", &group, &back, 0));
    CHECK(!DiskCache::Parse("S3TCMD-CACHE\t1\tg\nE\t10\t0\t0\t0\tx\n", &group, &back, 0));  // entry before dir

    // Import never replaces fresher in-memory data and does not mark groups dirty.
    DirCache c2;
    c2.Put(L"\\p\\b", {CE(L"new", false)});
    c2.TakeDirtyGroups();
    for (auto& [p, d] : back) c2.Import(p, d);
    c2.Import(L"\\p\\b", std::make_shared<CachedDir>());
    CHECK(c2.Get(L"\\p\\b", 0)->items.size() == 1 && c2.Get(L"\\p\\b", 0)->items[0].cFileName == L"new");
    CHECK(c2.TakeDirtyGroups().empty());

    // A command's own re-listing is not a refresh, a real Ctrl+R afterwards is.
    RefreshTracker t;
    CHECK(!t.OnPanelListing(L"\\p\\b", true));
    t.SuppressNext(L"\\p\\b");
    CHECK(!t.OnPanelListing(L"\\p\\b", true));
    CHECK(t.OnPanelListing(L"\\p\\b", true));
}

static void TestCacheAndRefresh() {
    DirCache cache;
    CacheEntry fd;
    cache.Put(L"\\p\\b", {fd});
    cache.Put(L"\\p\\b\\x", {fd, fd});
    cache.Put(L"\\p\\bb", {fd});
    CHECK(cache.Get(L"\\p\\b", 0) != nullptr);
    CHECK(cache.Get(L"\\p\\B", 0) == nullptr);  // case-sensitive like S3
    cache.InvalidateTree(L"\\p\\b");
    CHECK(cache.Get(L"\\p\\b", 0) == nullptr);
    CHECK(cache.Get(L"\\p\\b\\x", 0) == nullptr);
    CHECK(cache.Get(L"\\p\\bb", 0) != nullptr);  // sibling with the same prefix survives
    cache.Clear();
    CHECK(cache.Size() == 0);

    RefreshTracker t;
    CHECK(!t.OnPanelListing(L"\\a", true));      // enter
    CHECK(!t.OnPanelListing(L"\\a\\b", true));   // enter child
    CHECK(!t.OnPanelListing(L"\\a", true));      // go up: navigation, not refresh
    CHECK(t.OnPanelListing(L"\\a", true));       // Ctrl+R
    CHECK(t.OnPanelListing(L"\\a", true));       // Ctrl+R again
    RefreshTracker t2;
    CHECK(!t2.OnPanelListing(L"\\a", false));
    CHECK(!t2.OnPanelListing(L"\\a", false));    // heuristic off
    t2.ForceNext(L"\\a");
    CHECK(t2.OnPanelListing(L"\\a", false));     // forced by the refresh command
    CHECK(!t2.OnPanelListing(L"\\a", false));    // force is one-shot
}

static void TestClipPaths() {
    const std::wstring root = L"S3";
    // List file exactly as TC 11.58 writes it for %UL in the plugin panel (measured).
    std::string list = std::string("\xEF\xBB\xBF") + R"(\\\S3\prof-c\b\p\dir with space\)" + "\r\n" +
                       R"(\\\S3\prof-c\b\p\a b.txt)" + "\r\n" +
                       R"(\\\S3\prof-c\b\p\colon)" + "\xEF\xBC\x9Aq\xEF\xBC\x82uote.txt\r\n" +
                       R"(c:\Users\x\Local Dir\)" + "\r\n";
    auto lines = ParseListFile(list);
    CHECK(lines.size() == 4);
    if (lines.size() == 4) {
        CHECK(ConvertLineToUri(lines[0], root) == L"s3://b/p/dir with space/");
        CHECK(ConvertLineToUri(lines[1], root) == L"s3://b/p/a b.txt");
        CHECK(ConvertLineToUri(lines[2], root) == L"s3://b/p/colon:q\"uote.txt");
        CHECK(ConvertLineToUri(lines[3], root) == LR"(c:\Users\x\Local Dir\)");  // unchanged
    }
    // UTF-16LE list (%WL), no trailing newline.
    std::wstring w = LR"(\\\S3\p\b\f.txt)";
    std::string u16 = "\xFF\xFE" + std::string(reinterpret_cast<const char*>(w.data()), w.size() * 2);
    lines = ParseListFile(u16);
    CHECK(lines.size() == 1 && lines[0] == w);
    CHECK(ParseListFile("").empty());

    S3Ref r;
    CHECK(ParsePluginPath(LR"(\\\s3\p\bucket\)", root, &r));  // root name is case-insensitive
    CHECK(S3Uri(r) == L"s3://bucket/" && r.profile == L"p");
    CHECK(ParsePluginPath(LR"(\\\S3\p\bucket)", root, &r) && S3Uri(r) == L"s3://bucket/");
    CHECK(!ParsePluginPath(LR"(\\\S3\p\)", root, &r));  // profile level: no URI
    CHECK(ConvertLineToUri(LR"(\\\S3\p\)", root) == LR"(\\\S3\p\)");
    CHECK(!ParsePluginPath(LR"(\\\S3\)", root, &r));
    CHECK(!ParsePluginPath(LR"(\\\S3x\p\b\k)", root, &r));  // another plugin
    CHECK(ConvertLineToUri(LR"(\\\Rclone\x\y)", root) == LR"(\\\Rclone\x\y)");
    // An empty key segment (key "a//c") is shown as a lone U+201B.
    CHECK(ConvertLineToUri(LR"(\\\S3\p\b\a\)" + std::wstring(1, 0x201B) + LR"(\c)", root) == L"s3://b/a//c");
    CHECK(ConvertLineToUri(LR"(\\\MyS3\p\b\k)", L"MyS3") == L"s3://b/k");

    CHECK(PickPluginRootName({{L"Secure FTP", LR"(C:\tc\sftpplug.wfx)"},
                              {L"Amazon", LR"(%COMMANDER_PATH%\plugins\S3TCMD.WFX64)"}},
                             L"s3tcmd.wfx64") == L"Amazon");
    CHECK(PickPluginRootName({{L"Other", LR"(C:\x.wfx)"}}, L"s3tcmd.wfx64") == L"S3");

    // --cwd %P as the last parameter: TC-quoted with a space ("...\" -> escaped quote), unquoted
    // (split at spaces), plain, and a trimmed path without the trailing backslash.
    {
        std::wstring cmd = LR"(x.exe uri --cwd "\\\S3\p\b\dir with space\")";
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(cmd.c_str(), &argc);
        CHECK(argc == 4);
        if (argc == 4) CHECK(CwdFromArgs({argv[3]}) == LR"(\\\S3\p\b\dir with space\)");
        LocalFree(argv);
    }
    CHECK(CwdFromArgs({LR"(\\\S3\p\b\dir)", L"with", LR"(space\)"}) == LR"(\\\S3\p\b\dir with space\)");
    CHECK(CwdFromArgs({LR"(c:\Users\x\)"}) == LR"(c:\Users\x\)");
    CHECK(CwdFromArgs({LR"(c:\Users\x)"}) == LR"(c:\Users\x\)");
    CHECK(CwdFromArgs({}).empty());
    CHECK(ConvertLineToUri(CwdFromArgs({LR"(\\\S3\p\b\k\)"}), root) == L"s3://b/k/");


    CHECK(RegionFromLocationJson("{\"LocationConstraint\": null}") == L"us-east-1");
    CHECK(RegionFromLocationJson("{\"LocationConstraint\": \"\"}") == L"us-east-1");
    CHECK(RegionFromLocationJson("{\"LocationConstraint\": \"EU\"}") == L"eu-west-1");
    CHECK(RegionFromLocationJson("{\"LocationConstraint\": \"eu-central-1\"}") == L"eu-central-1");
    CHECK(RegionFromLocationJson("garbage").empty());
}

static void TestS3Uri() {
    S3Target t;
    CHECK(ParseS3Uri(L"s3://my-bucket/team/", &t));
    CHECK(t.bucket == L"my-bucket" && t.key == L"team" && t.isDir && t.profile.empty());
    CHECK(ParseS3Uri(L"  \"S3://b/dir/file name.txt\" ", &t));
    CHECK(t.bucket == L"b" && t.key == L"dir/file name.txt" && !t.isDir);
    CHECK(ParseS3Uri(L"s3://b", &t) && t.bucket == L"b" && t.key.empty() && t.isDir);
    CHECK(ParseS3Uri(L"s3://b/", &t) && t.key.empty() && t.isDir);
    CHECK(ParseS3Uri(L"s3:/b/k", &t) && t.bucket == L"b" && t.key == L"k");
    CHECK(ParseS3Uri(LR"(s3:\\b/k)", &t) && t.bucket == L"b");
    CHECK(ParseS3Uri(L"s3://b/a//c/", &t) && t.key == L"a//c" && t.isDir);  // empty segment kept
    CHECK(!ParseS3Uri(L"s3:b/k", &t));
    CHECK(!ParseS3Uri(L"s3://", &t));
    CHECK(!ParseS3Uri(L"https://b.s3.amazonaws.com/k", &t));
    CHECK(!ParseS3Uri(L"refresh", &t));

    // Address bar paths as TC passes them to FsFindFirst (measured).
    CHECK(ParseS3AliasPath(L"\\s3://my-bucket/team/", &t));
    CHECK(t.profile.empty() && t.bucket == L"my-bucket" && t.key == L"team" && t.isDir);
    CHECK(ParseS3AliasPath(L"\\prof-c\\s3://my-bucket/team/x.txt", &t));
    CHECK(t.profile == L"prof-c" && t.key == L"team/x.txt" && !t.isDir);
    CHECK(ParseS3AliasPath(L"\\p\\s3://b/k\\", &t) && t.key == L"k" && t.isDir);
    // "cd s3://..." on the command line: TC appends it to the current folder; profile is a hint.
    CHECK(ParseS3AliasPath(L"\\prof-a\\bucket\\dir\\s3://b2/k/", &t));
    CHECK(t.profile.empty() && t.hintProfile == L"prof-a" && t.bucket == L"b2" && t.key == L"k" && t.isDir);
    CHECK(ParseS3AliasPath(L"\\prof-a\\bucket\\S3:\\b2\\k\\f.txt", &t));  // backslashes, collapsed "//"
    CHECK(t.bucket == L"b2" && t.key == L"k/f.txt" && !t.isDir && t.hintProfile == L"prof-a");
    CHECK(ParseS3AliasPath(L"\\p\\s3://b/k", &t) && t.profile == L"p" && t.hintProfile.empty());
    CHECK(!ParseS3AliasPath(L"\\prof-c\\bucket\\s3x", &t));
    CHECK(!ParseS3AliasPath(L"\\prof-c\\bucket\\s3\xFF1Ax", &t));  // displayed key "s3:x" (U+FF1A)
    CHECK(!ParseS3AliasPath(L"\\prof-c\\bucket", &t));
    CHECK(!ParseS3AliasPath(L"\\", &t));

    std::vector<ProfileBucketInfo> ps = {
        {L"prof-a", true, true}, {L"prof-b", true, true}, {L"prof-c", true, false}, {L"other", false, false}};
    CHECK(ChooseProfileForBucket(ps, L"") == L"prof-c");         // owner beats pinned
    CHECK(ChooseProfileForBucket(ps, L"prof-a") == L"prof-a");         // current panel's profile first
    CHECK(ChooseProfileForBucket(ps, L"other") == L"prof-c");    // ... only if it has the bucket
    CHECK(ChooseProfileForBucket({{L"a", true, true}}, L"") == L"a");
    CHECK(ChooseProfileForBucket({{L"a", false, false}}, L"").empty());

    std::string cd = BuildTcCdCommand(LR"(\\\S3\p\b\)");
    CHECK(cd == std::string("\xEF\xBB\xBF") + R"(\\\S3\p\b\)" + "\r" + std::string(1, '\0') + "S" +
                    std::string(1, '\0'));
}

static std::string ReadAllBytes(const std::wstring& path) {
    std::string out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
    CloseHandle(h);
    return out;
}

static void WriteAllBytes(const std::wstring& path, const std::string& bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD n = 0;
    WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &n, nullptr);
    CloseHandle(h);
}

static std::wstring IniValue(const std::wstring& ini, const wchar_t* section, const wchar_t* key) {
    wchar_t buf[1024];
    GetPrivateProfileStringW(section, key, L"", buf, _countof(buf), ini.c_str());
    return buf;
}

static void TestSetup() {
    // PortablePath
    const std::vector<std::pair<std::wstring, std::wstring>> vars = {
        {L"COMMANDER_PATH", L"C:\\Program Files\\totalcmd"}, {L"APPDATA", L"C:\\Users\\u\\AppData\\Roaming\\"}};
    CHECK(PortablePath(L"C:\\Users\\u\\AppData\\Roaming\\GHISLER\\plugins\\wfx\\s3tcmd\\s3tcmd-clip.exe", vars) ==
          L"%APPDATA%\\GHISLER\\plugins\\wfx\\s3tcmd\\s3tcmd-clip.exe");
    CHECK(PortablePath(L"c:\\program files\\TOTALCMD\\plugins\\x.exe", vars) == L"%COMMANDER_PATH%\\plugins\\x.exe");
    CHECK(PortablePath(L"C:\\Program Files\\totalcmd2\\x.exe", vars) == L"C:\\Program Files\\totalcmd2\\x.exe");
    CHECK(PortablePath(L"D:\\tools\\x.exe", vars) == L"D:\\tools\\x.exe");

    // Hotkey description
    CHECK(HotkeyDisplayName(L"CS+C") == L"Ctrl+Shift+C");
    CHECK(HotkeyDisplayName(L"A+F1") == L"Alt+F1");
    CHECK(HotkeyDisplayName(L"F5") == L"F5");
    CHECK(DescribeCopyHotkey({{L"C+1", L"cm_x"}, {L"CS+C", L"em_s3_copy_uri"}}) ==
          L"Hotkey for em_s3_copy_uri: Ctrl+Shift+C.");
    CHECK(DescribeCopyHotkey({{L"CS+C", L"cm_CopyNamesToClip"}}).find(L"currently runs cm_CopyNamesToClip") !=
          std::wstring::npos);
    CHECK(DescribeCopyHotkey({}).find(L"default meaning") != std::wstring::npos);

    // usercmd.ini merge, in a temp folder
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"s3tcmd_setup_test_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring ini = dir + L"usercmd.ini";
    const std::wstring exe = dir + L"s3tcmd-clip.exe";
    WriteAllBytes(exe, "x");

    // No file: both missing; apply creates it without backup.
    auto st = CheckUserCommands(ini);
    CHECK(st.size() == 2 && st[0].state == UserCmdState::Missing && st[1].state == UserCmdState::Missing);
    CHECK(NeedsSetup(st));
    SetupResult r = ApplyUserCommands(ini, exe);
    CHECK(r.ok && r.backup.empty() && r.written.size() == 2);
    CHECK(IniValue(ini, L"em_s3_copy_uri", L"cmd") == exe);
    CHECK(IniValue(ini, L"em_s3_copy_uri", L"param") == L"uri %Y%y %UL --cwd %P");
    CHECK(IniValue(ini, L"em_s3_presign", L"param") == L"presign %UL");
    CHECK(!NeedsSetup(CheckUserCommands(ini)));
    // Idempotent: nothing written, no backup.
    std::string before = ReadAllBytes(ini);
    r = ApplyUserCommands(ini, exe);
    CHECK(r.ok && r.written.empty() && r.backup.empty());
    CHECK(ReadAllBytes(ini) == before);

    // Existing file: other sections and the user's param are kept; a stale cmd= is fixed; backup made.
    const std::string original =
        "[em_notepad]\r\ncmd=notepad.exe\r\nparam=%P%N\r\n\r\n"
        "[em_s3_copy_uri]\r\ncmd=C:\\gone\\s3tcmd-clip.exe\r\nparam=uri %UL --cwd %P\r\nmenu=mine\r\n";
    WriteAllBytes(ini, original);
    st = CheckUserCommands(ini);
    CHECK(st[0].state == UserCmdState::StaleCmd && st[0].cmd == L"C:\\gone\\s3tcmd-clip.exe");
    CHECK(st[1].state == UserCmdState::Missing);
    r = ApplyUserCommands(ini, exe);
    CHECK(r.ok && r.written.size() == 2 && r.backup == ini + L".s3tcmd-bak");
    CHECK(ReadAllBytes(r.backup) == original);
    CHECK(IniValue(ini, L"em_notepad", L"cmd") == L"notepad.exe");
    CHECK(IniValue(ini, L"em_notepad", L"param") == L"%P%N");
    CHECK(IniValue(ini, L"em_s3_copy_uri", L"cmd") == exe);
    CHECK(IniValue(ini, L"em_s3_copy_uri", L"param") == L"uri %UL --cwd %P");  // user's choice kept
    CHECK(IniValue(ini, L"em_s3_copy_uri", L"menu") == L"mine");
    CHECK(IniValue(ini, L"em_s3_presign", L"cmd") == exe);

    // cmd= with an environment variable that resolves to an existing file is Ok; relative is Ok.
    SetEnvironmentVariableW(L"S3TCMD_TEST_DIR", dir.c_str());
    WriteAllBytes(ini, "[em_s3_copy_uri]\r\ncmd=%S3TCMD_TEST_DIR%s3tcmd-clip.exe\r\n"
                       "[em_s3_presign]\r\ncmd=s3tcmd-clip.exe\r\n");
    st = CheckUserCommands(ini);
    CHECK(st[0].state == UserCmdState::Ok && st[1].state == UserCmdState::Ok);

    // UserCmdIniPath follows COMMANDER_INI.
    wchar_t saved[MAX_PATH] = {};
    DWORD had = GetEnvironmentVariableW(L"COMMANDER_INI", saved, MAX_PATH);
    SetEnvironmentVariableW(L"COMMANDER_INI", L"D:\\tc cfg\\wincmd.ini");
    CHECK(UserCmdIniPath(L"C:\\x") == L"D:\\tc cfg\\usercmd.ini");
    SetEnvironmentVariableW(L"COMMANDER_INI", nullptr);
    CHECK(UserCmdIniPath(L"C:\\x") == L"C:\\x\\usercmd.ini");
    if (had) SetEnvironmentVariableW(L"COMMANDER_INI", saved);

    // ReadShortcuts
    const std::wstring wincmd = dir + L"wincmd.ini";
    WriteAllBytes(wincmd, "[Configuration]\r\nx=1\r\n[Shortcuts]\r\nC+1=cm_x\r\nCS+C=em_s3_copy_uri\r\n");
    auto sc = ReadShortcuts(wincmd);
    CHECK(sc.size() == 2 && sc[1].first == L"CS+C" && sc[1].second == L"em_s3_copy_uri");

    for (const wchar_t* f : {L"usercmd.ini", L"usercmd.ini.s3tcmd-bak", L"s3tcmd-clip.exe", L"wincmd.ini"}) {
        DeleteFileW((dir + f).c_str());
    }
    RemoveDirectoryW(dir.c_str());
}

int main() {
    TestQuoteArgRoundTrip();
    TestSplitLines();
    TestUtf8();
    TestRunProcess();
    TestCancel();
    TestNames();
    TestVfsPath();
    TestIsoTime();
    TestParseJson();
    TestErrors();
    TestDirectoryTree();
    TestProgressParse();
    TestPlanDownloads();
    TestPlanUploads();
    TestDiskCache();
    TestCacheAndRefresh();
    TestClipPaths();
    TestS3Uri();
    TestSetup();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
