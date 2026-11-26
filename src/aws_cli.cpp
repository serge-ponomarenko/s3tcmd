#include "aws_cli.h"

#include <atomic>

#include <windows.h>

#include "log.h"
#include "util.h"

namespace s3tc {

std::wstring ResolveAwsExe(const std::wstring& configured) {
    if (configured.find_first_of(L"\\/") != std::wstring::npos) return configured;
    wchar_t buf[MAX_PATH];
    DWORD n = SearchPathW(nullptr, configured.c_str(), L".exe", MAX_PATH, buf, nullptr);
    return (n > 0 && n < MAX_PATH) ? std::wstring(buf) : configured;
}

namespace {

std::atomic<bool> g_disableImds{true};

}  // namespace

void SetDisableImds(bool disable) { g_disableImds = disable; }

AwsResult RunAws(const std::wstring& awsExe, const std::vector<std::wstring>& args,
                 ProcessOptions options) {
    // Without PYTHONUTF8 the CLI writes to pipes in the ANSI code page and loses characters.
    options.env.push_back(L"PYTHONUTF8=1");
    options.env.push_back(L"AWS_PAGER=");
    // file:// parameters (delete-objects request bodies) are otherwise read in the ANSI code page:
    // a non-ASCII key is then silently "deleted" under a wrong name and stays (measured).
    options.env.push_back(L"AWS_CLI_FILE_ENCODING=UTF-8");
    // Without a configured region the CLI probes the EC2 instance metadata service and waits for
    // its timeouts: +2 s per listing, +12 s per s3 cp (measured). Desktop use never needs IMDS.
    if (g_disableImds) options.env.push_back(L"AWS_EC2_METADATA_DISABLED=true");

    AwsResult r;
    if (LogEnabled()) Log(L"aws> %s", BuildCommandLine(awsExe, args).c_str());
    r.raw = RunProcess(awsExe, args, options);
    if (!r.raw.started) {
        r.error = L"Cannot start " + awsExe + L" (error " + std::to_wstring(r.raw.winError) + L")";
    } else if (r.raw.cancelled) {
        r.error = L"Cancelled";
    } else if (r.raw.timedOut) {
        r.error = L"Timed out";
    } else if (r.raw.exitCode != 0) {
        r.error = Utf8ToWide(r.raw.err);
        if (r.error.empty()) r.error = L"aws exited with code " + std::to_wstring(r.raw.exitCode);
    } else {
        r.ok = true;
    }
    r.out = r.raw.out;
    Log(L"aws< exit=%lu %lums out=%zu err=%zu%s", r.raw.exitCode, r.raw.elapsedMs, r.raw.out.size(),
        r.raw.err.size(), r.ok ? L"" : (L" ERROR: " + r.error).c_str());
    return r;
}

bool ListProfiles(const std::wstring& awsExe, std::vector<std::wstring>* profiles,
                  std::wstring* error) {
    AwsResult r = RunAws(awsExe, {L"configure", L"list-profiles"});
    if (!r.ok) {
        if (error) *error = r.error;
        return false;
    }
    profiles->clear();
    for (const auto& line : SplitLines(r.out)) profiles->push_back(Utf8ToWide(line));
    return true;
}

}  // namespace s3tc
