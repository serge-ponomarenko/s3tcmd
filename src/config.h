// Plugin settings stored in s3tcmd.ini next to the INI file suggested by Total Commander.
#pragma once

#include <string>
#include <vector>

namespace s3tc {

struct ProfileConfig {
    std::vector<std::wstring> pinnedBuckets;  // always shown, even if list-buckets is denied
    bool hidden = false;                      // hide the profile from the plugin root
    bool readOnly = false;                    // refuse all write operations for this profile
};

struct Config {
    std::wstring iniPath;
    std::wstring awsExe = L"aws.exe";  // name on PATH or full path
    std::wstring logFile;              // empty = logging disabled
    unsigned cacheTtlSec = 0;          // 0 = cached listings never expire (refresh with Ctrl+R)
    bool refreshHeuristic = true;      // a repeated listing of the same path means Ctrl+R
    int maxListItems = 50000;          // safety cap for one folder listing
    int prefetchMaxKeys = 200000;      // cap for one recursive (tree) listing; 0 = disabled
    bool batchDownloads = true;        // F5: queue files and download them in few aws calls at the end
    bool disableImds = true;           // set AWS_EC2_METADATA_DISABLED=true (avoids 2-12 s timeouts)
    bool readOnly = false;             // refuse all write operations (all profiles)
    bool diskCache = true;             // keep listings on disk across TC restarts
    std::wstring diskCacheDir;         // default: %LOCALAPPDATA%/S3Tcmd/cache
    int diskCacheMaxAgeDays = 30;      // older listings are not loaded (0 = no limit)
    int presignExpirySec = 3600;       // s3tcmd-clip.exe presign: URL lifetime (1 s .. 7 days)
};

// Reads [General]; writes missing keys with defaults so users can discover them.
Config LoadConfig(const std::wstring& iniPath);

// Reads [Profile:<name>] (does not write defaults).
ProfileConfig LoadProfileConfig(const std::wstring& iniPath, const std::wstring& profile);

}  // namespace s3tc
