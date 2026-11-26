// Thin wrapper around aws.exe invocations.
#pragma once

#include <string>
#include <vector>

#include "process.h"

namespace s3tc {

// Resolves the configured aws executable to a full path (searches PATH for bare names).
std::wstring ResolveAwsExe(const std::wstring& configured);

struct AwsResult {
    bool ok = false;
    std::string out;     // UTF-8 stdout
    std::wstring error;  // human-readable error (stderr or launch failure)
    ProcessResult raw;
};

// Sets AWS_EC2_METADATA_DISABLED=true for all aws.exe calls (default: on).
void SetDisableImds(bool disable);

// Runs aws.exe with UTF-8 output forced, logging the command line and timing.
AwsResult RunAws(const std::wstring& awsExe, const std::vector<std::wstring>& args,
                 ProcessOptions options = {});

// `aws configure list-profiles`
bool ListProfiles(const std::wstring& awsExe, std::vector<std::wstring>* profiles,
                  std::wstring* error);

}  // namespace s3tc
