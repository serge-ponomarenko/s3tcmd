// s3tcmd-clip.exe: copies s3:// URIs or presigned URLs of the files selected in Total Commander.
//
// TC cannot change what cm_CopyFullNamesToClip copies for a file system plugin, so this helper is
// called from a TC user command (usercmd.ini) with a %UL list file:
//   s3tcmd-clip.exe uri %Y%y %UL --cwd %P
//                                 -> "s3://bucket/key" per line; other paths are copied unchanged;
//                                    on ".." (no list) the current folder %P is copied
//   s3tcmd-clip.exe presign %UL   -> presigned HTTPS URL per selected S3 file (folders skipped)
// For testing: "--paths <path>..." instead of list files, and "--out <file>" (before the paths)
// instead of the clipboard and message boxes.
//
// Settings come from s3tcmd.ini next to %COMMANDER_INI% (AwsExe, DisableImds, LogFile,
// PresignExpirySec). The plugin's TC root name is read from [FileSystemPlugins] in wincmd.ini.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "aws_cli.h"
#include "clip_paths.h"
#include "config.h"
#include "errors.h"
#include "log.h"
#include "progress_window.h"
#include "util.h"

using namespace s3tc;

namespace {

const wchar_t kTitle[] = L"S3Tcmd";

std::wstring EnvVar(const wchar_t* name) {
    wchar_t buf[2048];
    DWORD n = GetEnvironmentVariableW(name, buf, _countof(buf));
    return (n > 0 && n < _countof(buf)) ? std::wstring(buf) : std::wstring();
}

// wincmd.ini of the calling TC (COMMANDER_INI is set by TC for programs it starts).
std::wstring WincmdIni() {
    std::wstring ini = EnvVar(L"COMMANDER_INI");
    if (ini.empty()) ini = EnvVar(L"APPDATA") + L"\\GHISLER\\wincmd.ini";
    return ini;
}

bool ReadFileBytes(const std::wstring& path, std::string* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[65536];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0) out->append(buf, got);
    CloseHandle(h);
    return true;
}

bool SetClipboardText(const std::wstring& text) {
    for (int attempt = 0; attempt < 10; ++attempt) {  // another process may hold the clipboard
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            size_t bytes = (text.size() + 1) * sizeof(wchar_t);
            HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
            bool ok = false;
            if (mem) {
                memcpy(GlobalLock(mem), text.c_str(), bytes);
                GlobalUnlock(mem);
                ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
                if (!ok) GlobalFree(mem);
            }
            CloseClipboard();
            return ok;
        }
        Sleep(50);
    }
    return false;
}

std::wstring Join(const std::vector<std::wstring>& lines) {
    std::wstring s;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) s += L"\r\n";
        s += lines[i];
    }
    return s;
}

// --out <file> (tests): the clipboard text and all messages go to this UTF-8 file instead.
std::wstring g_outFile;

void AppendOut(const std::wstring& text) {
    HANDLE h = CreateFileW(g_outFile.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string u = WideToUtf8(text + L"\r\n");
    DWORD written = 0;
    WriteFile(h, u.data(), static_cast<DWORD>(u.size()), &written, nullptr);
    CloseHandle(h);
}

bool PutResult(const std::wstring& text) {
    if (g_outFile.empty()) return SetClipboardText(text);
    AppendOut(L"CLIPBOARD:\r\n" + text);
    return true;
}

void ShowMessage(const std::wstring& text, UINT icon) {
    if (!g_outFile.empty()) return AppendOut(L"MESSAGE: " + text);
    MessageBoxW(nullptr, text.c_str(), kTitle, MB_OK | icon | MB_SETFOREGROUND | MB_TOPMOST);
}

// Bucket regions, cached across runs: a presigned URL must be signed for the bucket's region, and
// get-bucket-location costs one aws.exe start (~1.5 s).
class RegionCache {
public:
    RegionCache() {
        std::wstring dir = EnvVar(L"LOCALAPPDATA") + L"\\S3Tcmd";
        CreateDirectoryW(dir.c_str(), nullptr);
        path_ = dir + L"\\bucket-regions.ini";
    }
    std::wstring Get(const std::wstring& bucket) const {
        wchar_t buf[128];
        GetPrivateProfileStringW(L"Regions", bucket.c_str(), L"", buf, _countof(buf), path_.c_str());
        return buf;
    }
    void Put(const std::wstring& bucket, const std::wstring& region) const {
        WritePrivateProfileStringW(L"Regions", bucket.c_str(), region.c_str(), path_.c_str());
    }

private:
    std::wstring path_;
};

std::wstring BucketRegion(const std::wstring& awsExe, const RegionCache& cache, const S3Ref& ref) {
    std::wstring region = cache.Get(ref.bucket);
    if (!region.empty()) return region;
    AwsResult r = RunAws(awsExe, {L"s3api", L"get-bucket-location", L"--bucket", ref.bucket, L"--profile",
                                  ref.profile, L"--output", L"json"});
    if (r.ok) region = RegionFromLocationJson(r.out);
    if (!region.empty()) cache.Put(ref.bucket, region);
    // Without a region (e.g. get-bucket-location denied) the CLI signs for the profile's region.
    return region;
}

int CopyUris(const std::vector<std::wstring>& lines, const std::wstring& root) {
    std::vector<std::wstring> out;
    for (const auto& l : lines) out.push_back(ConvertLineToUri(l, root));
    Log(L"clip: uri, %zu line(s)", out.size());
    if (!PutResult(Join(out))) {
        ShowMessage(L"Cannot open the clipboard.", MB_ICONERROR);
        return 1;
    }
    return 0;
}

int CopyPresigned(const std::vector<std::wstring>& lines, const std::wstring& root, const Config& cfg) {
    std::vector<S3Ref> files;
    for (const auto& l : lines) {
        S3Ref ref;
        if (ParsePluginPath(l, root, &ref) && !ref.isDir) files.push_back(ref);
    }
    if (files.empty()) {
        ShowMessage(L"Presigned URLs can only be made for files inside S3 (folders are skipped).",
                    MB_ICONINFORMATION);
        return 1;
    }
    const int expiry = std::clamp(cfg.presignExpirySec, 1, 7 * 24 * 3600);  // SigV4 maximum: 7 days
    const std::wstring awsExe = ResolveAwsExe(cfg.awsExe);

    ProgressWindow window(L"S3Tcmd: presigned URL", 700);
    window.Update(L"Presigning " + std::to_wstring(files.size()) + L" file(s)...", 0);

    RegionCache cache;
    std::map<std::wstring, std::wstring> regions;  // bucket -> region ("" = let the CLI decide)
    for (const auto& f : files) {
        if (window.Cancelled()) return 1;
        if (!regions.count(f.bucket)) regions[f.bucket] = BucketRegion(awsExe, cache, f);
    }

    std::vector<std::wstring> urls(files.size()), errors(files.size());
    std::atomic<size_t> next{0}, done{0};
    auto worker = [&]() {
        for (size_t i; (i = next++) < files.size();) {
            const S3Ref& f = files[i];
            std::vector<std::wstring> args = {L"s3", L"presign", S3Uri(f), L"--expires-in",
                                              std::to_wstring(expiry), L"--profile", f.profile};
            const std::wstring& region = regions[f.bucket];
            if (!region.empty()) {
                args.push_back(L"--region");
                args.push_back(region);
            }
            ProcessOptions opt;
            opt.shouldCancel = [&]() { return window.Cancelled(); };
            AwsResult r = RunAws(awsExe, args, opt);
            std::wstring url = r.ok ? Utf8ToWide(r.out) : L"";
            while (!url.empty() && iswspace(url.back())) url.pop_back();
            if (r.ok && !url.empty()) {
                urls[i] = url;
            } else {
                errors[i] = DescribeAwsError(ClassifyAwsError(r.error), f.profile);
            }
            size_t d = ++done;
            window.Update(L"Presigning " + std::to_wstring(files.size()) + L" file(s)... " +
                              std::to_wstring(d) + L" done",
                          static_cast<int>(d * 100 / files.size()));
        }
    };
    std::vector<std::thread> threads;
    size_t n = std::min<size_t>(files.size(), 8);
    for (size_t i = 0; i < n; ++i) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
    if (window.Cancelled()) return 1;

    std::vector<std::wstring> ok;
    std::wstring failed;
    int failedCount = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        if (!urls[i].empty()) {
            ok.push_back(urls[i]);
        } else if (++failedCount <= 5) {
            failed += L"\n" + S3Uri(files[i]) + L"\n    " + errors[i] + L"\n";
        }
    }
    Log(L"clip: presign, %zu ok, %d failed", ok.size(), failedCount);
    if (!ok.empty() && !PutResult(Join(ok))) {
        ShowMessage(L"Cannot open the clipboard.", MB_ICONERROR);
        return 1;
    }
    if (failedCount > 0) {
        std::wstring msg = std::to_wstring(failedCount) + L" of " + std::to_wstring(files.size()) +
                           L" presigned URL(s) failed" +
                           (ok.empty() ? L"; the clipboard was not changed.\n" : L"; the others were copied.\n") +
                           failed;
        if (failedCount > 5) msg += L"\n...";
        ShowMessage(msg, MB_ICONWARNING);
        return 1;
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    // Cursor on ".." with nothing selected and no %y: TC drops all parameters. Do nothing.
    if (argv && argc == 1) return 0;
    if (!argv || (wcscmp(argv[1], L"uri") != 0 && wcscmp(argv[1], L"presign") != 0)) {
        ShowMessage(L"Usage:\n  s3tcmd-clip.exe uri %UL\n  s3tcmd-clip.exe presign %UL\n"
                    L"  (or --paths <path>... instead of list files)",
                    MB_ICONINFORMATION);
        return 2;
    }
    const std::wstring mode = argv[1];

    std::vector<std::wstring> lines;
    std::wstring cwd;  // --cwd %P: current folder, copied when TC passes no list (cursor on "..")
    bool literal = false;
    for (int i = 2; i < argc; ++i) {
        if (wcscmp(argv[i], L"--cwd") == 0) {
            cwd = CwdFromArgs(std::vector<std::wstring>(argv + i + 1, argv + argc));
            break;  // %P must be last: it may end in a backslash right before TC's closing quote
        } else if (wcscmp(argv[i], L"--out") == 0 && i + 1 < argc) {
            g_outFile = argv[++i];
        } else if (wcscmp(argv[i], L"--paths") == 0) {
            literal = true;
        } else if (literal) {
            lines.push_back(argv[i]);
        } else {
            std::string bytes;
            if (!ReadFileBytes(argv[i], &bytes)) {
                ShowMessage(std::wstring(L"Cannot read the list file:\n") + argv[i], MB_ICONERROR);
                return 1;
            }
            for (auto& l : ParseListFile(bytes)) lines.push_back(std::move(l));
        }
    }
    // Cursor on ".." with nothing selected: TC passes no list ("%Y%y" keeps the other parameters).
    // uri copies the current folder then; otherwise the clipboard is left alone.
    if (lines.empty() && mode == L"uri" && !cwd.empty()) lines.push_back(cwd);
    if (lines.empty()) return 0;

    const std::wstring wincmd = WincmdIni();
    Config cfg = LoadConfig(DirectoryOf(wincmd) + L"s3tcmd.ini");
    LogSetFile(cfg.logFile);
    SetDisableImds(cfg.disableImds);
    const std::wstring root = ReadPluginRootName(wincmd, L"s3tcmd.wfx64");

    return mode == L"uri" ? CopyUris(lines, root) : CopyPresigned(lines, root, cfg);
}
