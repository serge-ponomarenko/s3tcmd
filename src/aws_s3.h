// Typed S3 operations on top of aws.exe, plus the pure JSON parsers they use (unit-tested).
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "errors.h"

namespace s3tc {

struct S3Entry {
    std::wstring name;  // raw S3 name (decoded; not yet encoded for TC)
    bool isDir = false;
    unsigned long long size = 0;
    FILETIME mtime{};
    bool hasTime = false;
};

struct S3Listing {
    std::vector<S3Entry> entries;
    bool truncated = false;  // more items exist beyond maxItems
    std::string nextToken;   // CLI pagination token to continue (tree listings)
};

// Parses ISO-8601 timestamps as returned by the CLI ("2026-05-29T18:51:26+00:00",
// "2026-05-29T18:51:26.000Z") into a UTC FILETIME.
bool ParseIsoTime(const std::string& s, FILETIME* ft);

// Parses `list-buckets --query "Buckets[].[Name,CreationDate]"` output.
bool ParseBucketsJson(const std::string& json, std::vector<S3Entry>* out, std::wstring* error);

// Parses `list-objects-v2 --query "{p:CommonPrefixes[].Prefix,c:Contents[].[Key,Size,LastModified],t:NextToken}"`
// output for a listing of `prefix` (UTF-8, with trailing '/', or empty).
bool ParseListObjectsJson(const std::string& json, const std::string& prefix, S3Listing* out,
                          std::wstring* error);

// Parses a flat listing: `list-objects-v2 --query "{c:Contents[].[Key,Size,LastModified],t:NextToken}"`
// (no delimiter). Entry names are keys relative to `prefix`, files only (isDir = false).
bool ParseTreeJson(const std::string& json, const std::string& prefix, S3Listing* out,
                   std::wstring* error);

struct AwsContext {
    std::wstring awsExe;
    std::wstring profile;
    // Polled while aws.exe runs; return true to cancel.
    std::function<bool()> shouldCancel;
};

bool ListBuckets(const AwsContext& ctx, std::vector<S3Entry>* out, AwsError* error);

bool ListFolder(const AwsContext& ctx, const std::wstring& bucket, const std::wstring& prefix,
                int maxItems, S3Listing* out, AwsError* error);

// Recursive listing of every key under `prefix`, in chunks of `chunkKeys` (one aws.exe call each,
// continued with --starting-token; 0 = one call). `onChunk(keysSoFar)` runs after each chunk except
// the last; return true to cancel. On error or cancel, `out` keeps the keys listed so far with
// truncated = true, so fully listed directories can still be used.
bool ListTree(const AwsContext& ctx, const std::wstring& bucket, const std::wstring& prefix,
              int maxItems, S3Listing* out, AwsError* error, int chunkKeys = 0,
              const std::function<bool(size_t keysSoFar)>& onChunk = nullptr);

// ---- write operations ----

// Creates the zero-byte folder marker object "dir/" (key must end with '/').
bool PutDirMarker(const AwsContext& ctx, const std::wstring& bucket, const std::wstring& key, AwsError* error);

struct DeleteFailure {
    std::wstring key;
    std::wstring message;
};

// `delete-objects` request body ({"Objects":[{"Key":...}],"Quiet":true}) for keys[begin, end).
std::string BuildDeleteJson(const std::vector<std::wstring>& keys, size_t begin, size_t end);

// Parses the "Errors" array of a quiet `delete-objects` response.
bool ParseDeleteErrors(const std::string& json, std::vector<DeleteFailure>* out);

// S3 allows at most 1000 keys per DeleteObjects request.
constexpr size_t kDeleteChunk = 1000;

// Deletes keys with one `s3api delete-objects` call per 1000 keys. Deleting a missing key is not
// an error in S3. `onChunk(done)` is called after each chunk; return true from it to stop.
// Returns false if a whole request failed (error set); per-key failures go to `failures`.
bool DeleteObjects(const AwsContext& ctx, const std::wstring& bucket, const std::vector<std::wstring>& keys,
                   std::vector<DeleteFailure>* failures, AwsError* error,
                   const std::function<bool(size_t done)>& onChunk = nullptr);

}  // namespace s3tc
