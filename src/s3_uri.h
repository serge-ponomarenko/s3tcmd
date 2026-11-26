// "Go to s3://..." support: parsing s3:// URIs typed into TC's address bar or command line, and
// choosing an AWS profile for a bucket.
//
// Measured in TC 11.58: text typed into the panel's address bar after "\\\S3\" reaches FsFindFirst
// unchanged, forward slashes included: "\s3://bucket/dir/" or "\profile\s3://bucket/dir/".
#pragma once

#include <string>
#include <vector>

namespace s3tc {

struct S3Target {
    std::wstring profile;  // empty = not given, choose one (ChooseProfileForBucket)
    std::wstring hintProfile;  // profile of the panel the URI was typed in (preferred if it has the bucket)
    std::wstring bucket;
    std::wstring key;      // no leading or trailing '/'; empty at the bucket root
    bool isDir = true;     // the URI ended with '/' (or names a bucket); otherwise unknown/file
};

// Parses "s3://bucket[/key]" (scheme case-insensitive; "s3:/" and "s3:\\" accepted too).
// Surrounding whitespace and double quotes are ignored. Returns false for anything else.
bool ParseS3Uri(const std::wstring& text, S3Target* out);

// Parses a plugin path in which a segment starts an s3:// URI (everything from there on is the URI):
//   "\s3://bucket/key"              address bar, no profile
//   "\profile\s3://bucket/key"      address bar, explicit profile
//   "\profile\bucket\dir\s3://b/k"  "cd s3://b/k" on the command line (TC appends the relative
//                                   path to the current folder): profile is only a hint.
// Unambiguous: display names never contain an ASCII ':' (EncodeName maps it to U+FF1A).
bool ParseS3AliasPath(const std::wstring& path, S3Target* out);

struct ProfileBucketInfo {
    std::wstring profile;
    bool hasBucket = false;  // the bucket is in the profile's (cached) bucket list
    bool pinned = false;     // ... because of PinnedBuckets (cross-account access, not the owner)
};

// Picks the profile for a bucket: preferred (the current panel's profile) if it has the bucket,
// else the first profile that lists it without pinning (the owner account), else the first one
// that has it pinned. Returns "" if none has it.
std::wstring ChooseProfileForBucket(const std::vector<ProfileBucketInfo>& profiles,
                                    const std::wstring& preferred);

// Builds the WM_COPYDATA payload of TC's "CD" command that changes only the source (active)
// panel: UTF-8 BOM + path, CR, empty second path, NUL, flags "S", NUL.
std::string BuildTcCdCommand(const std::wstring& tcPath);

}  // namespace s3tc
