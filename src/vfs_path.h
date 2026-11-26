// Mapping between plugin paths ("\profile\bucket\dir\file") and S3 locations.
#pragma once

#include <string>

namespace s3tc {

struct VfsPath {
    enum class Level { Root, Profile, Bucket, Inside };
    Level level = Level::Root;
    std::wstring profile;  // decoded
    std::wstring bucket;   // decoded
    std::wstring key;      // decoded, '/' separated, no leading or trailing '/'; empty at bucket root
};

// Parses a TC plugin path. Trailing backslashes are ignored.
VfsPath ParseVfsPath(const std::wstring& path);

// S3 prefix for listing a directory: "" at the bucket root, otherwise key + "/".
std::wstring ListPrefix(const VfsPath& p);

// Normalizes a TC path for use as a cache key: no trailing backslash, root stays "\".
std::wstring NormalizePath(const std::wstring& path);

}  // namespace s3tc
