#include "aws_s3.h"

#include <cstdio>

#include "../third_party/nlohmann/json.hpp"
#include "aws_cli.h"
#include "util.h"

namespace s3tc {

using nlohmann::json;

namespace {

// Strips the listing prefix and the trailing '/' of a common prefix.
std::string RelativeName(const std::string& full, const std::string& prefix) {
    std::string name = full.compare(0, prefix.size(), prefix) == 0 ? full.substr(prefix.size()) : full;
    if (!name.empty() && name.back() == '/') name.pop_back();
    return name;
}

bool ToError(const AwsResult& r, AwsError* error) {
    if (r.ok) return false;
    if (error) *error = ClassifyAwsError(r.error);
    return true;
}

ProcessOptions OptionsFor(const AwsContext& ctx) {
    ProcessOptions o;
    o.shouldCancel = ctx.shouldCancel;
    return o;
}

}  // namespace

bool ParseIsoTime(const std::string& s, FILETIME* ft) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    int consumed = 0;
    if (sscanf_s(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &sec, &consumed) != 6)
        return false;
    size_t i = static_cast<size_t>(consumed);
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && isdigit(static_cast<unsigned char>(s[i]))) ++i;
    }
    int offsetMinutes = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        int oh = 0, om = 0;
        if (sscanf_s(s.c_str() + i + 1, "%2d:%2d", &oh, &om) != 2) return false;
        offsetMinutes = (oh * 60 + om) * (s[i] == '-' ? -1 : 1);
    }
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(y);
    st.wMonth = static_cast<WORD>(mo);
    st.wDay = static_cast<WORD>(d);
    st.wHour = static_cast<WORD>(h);
    st.wMinute = static_cast<WORD>(mi);
    st.wSecond = static_cast<WORD>(sec);
    FILETIME local;
    if (!SystemTimeToFileTime(&st, &local)) return false;
    ULARGE_INTEGER u;
    u.LowPart = local.dwLowDateTime;
    u.HighPart = local.dwHighDateTime;
    // Convert the given wall time to UTC: subtract the offset (100 ns units).
    u.QuadPart -= static_cast<long long>(offsetMinutes) * 60LL * 10000000LL;
    ft->dwLowDateTime = u.LowPart;
    ft->dwHighDateTime = u.HighPart;
    return true;
}

bool ParseBucketsJson(const std::string& text, std::vector<S3Entry>* out, std::wstring* error) {
    out->clear();
    try {
        json j = json::parse(text);
        if (j.is_null()) return true;
        for (const auto& row : j) {
            S3Entry e;
            e.isDir = true;
            e.name = Utf8ToWide(row.at(0).get<std::string>());
            if (row.size() > 1 && row.at(1).is_string()) {
                e.hasTime = ParseIsoTime(row.at(1).get<std::string>(), &e.mtime);
            }
            out->push_back(std::move(e));
        }
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = L"Unexpected list-buckets output: " + Utf8ToWide(ex.what());
        return false;
    }
}

bool ParseListObjectsJson(const std::string& text, const std::string& prefix, S3Listing* out,
                          std::wstring* error) {
    out->entries.clear();
    out->truncated = false;
    try {
        json j = json::parse(text);
        if (j.is_null()) return true;
        if (auto p = j.find("p"); p != j.end() && p->is_array()) {
            for (const auto& v : *p) {
                S3Entry e;
                e.isDir = true;
                e.name = Utf8ToWide(RelativeName(v.get<std::string>(), prefix));
                out->entries.push_back(std::move(e));
            }
        }
        if (auto c = j.find("c"); c != j.end() && c->is_array()) {
            for (const auto& row : *c) {
                std::string key = row.at(0).get<std::string>();
                // The folder marker object ("prefix/" itself) is the directory, not a child.
                if (key == prefix) continue;
                S3Entry e;
                e.name = Utf8ToWide(RelativeName(key, prefix));
                // A key ending in '/' inside the listing is an (empty) folder marker.
                e.isDir = !key.empty() && key.back() == '/';
                if (row.size() > 1 && row.at(1).is_number()) e.size = row.at(1).get<unsigned long long>();
                if (row.size() > 2 && row.at(2).is_string()) {
                    e.hasTime = ParseIsoTime(row.at(2).get<std::string>(), &e.mtime);
                }
                out->entries.push_back(std::move(e));
            }
        }
        if (auto t = j.find("t"); t != j.end() && t->is_string() && !t->get<std::string>().empty()) {
            out->truncated = true;
        }
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = L"Unexpected list-objects-v2 output: " + Utf8ToWide(ex.what());
        return false;
    }
}

bool ParseTreeJson(const std::string& text, const std::string& prefix, S3Listing* out,
                   std::wstring* error) {
    out->entries.clear();
    out->truncated = false;
    try {
        json j = json::parse(text);
        if (j.is_null()) return true;
        if (auto c = j.find("c"); c != j.end() && c->is_array()) {
            out->entries.reserve(c->size());
            for (const auto& row : *c) {
                std::string key = row.at(0).get<std::string>();
                S3Entry e;
                // Relative key; keep trailing '/' so folder markers stay recognisable.
                e.name = Utf8ToWide(key.compare(0, prefix.size(), prefix) == 0 ? key.substr(prefix.size()) : key);
                if (row.size() > 1 && row.at(1).is_number()) e.size = row.at(1).get<unsigned long long>();
                if (row.size() > 2 && row.at(2).is_string()) {
                    e.hasTime = ParseIsoTime(row.at(2).get<std::string>(), &e.mtime);
                }
                out->entries.push_back(std::move(e));
            }
        }
        if (auto t = j.find("t"); t != j.end() && t->is_string() && !t->get<std::string>().empty()) {
            out->truncated = true;
            out->nextToken = t->get<std::string>();
        }
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = L"Unexpected list-objects-v2 output: " + Utf8ToWide(ex.what());
        return false;
    }
}

bool ListTree(const AwsContext& ctx, const std::wstring& bucket, const std::wstring& prefix,
              int maxItems, S3Listing* out, AwsError* error, int chunkKeys,
              const std::function<bool(size_t keysSoFar)>& onChunk) {
    out->entries.clear();
    out->truncated = false;
    out->nextToken.clear();
    const std::string prefixUtf8 = WideToUtf8(prefix);
    std::string token;
    for (;;) {
        // Each call returns at most one chunk; the CLI's token continues where it stopped.
        size_t want = chunkKeys > 0 ? static_cast<size_t>(chunkKeys) : 0;
        if (maxItems > 0) {
            size_t left = static_cast<size_t>(maxItems) - out->entries.size();
            want = want ? (std::min)(want, left) : left;
        }
        std::vector<std::wstring> args = {
            L"s3api", L"list-objects-v2", L"--profile", ctx.profile, L"--bucket", bucket,
            L"--output", L"json", L"--query", L"{c:Contents[].[Key,Size,LastModified],t:NextToken}"};
        if (!prefix.empty()) {
            args.push_back(L"--prefix");
            args.push_back(prefix);
        }
        if (want > 0) {
            args.push_back(L"--max-items");
            args.push_back(std::to_wstring(want));
        }
        if (!token.empty()) {
            args.push_back(L"--starting-token");
            args.push_back(Utf8ToWide(token));
        }
        AwsResult r = RunAws(ctx.awsExe, args, OptionsFor(ctx));
        if (ToError(r, error)) {
            // Keep what was listed so far: complete directories can still be cached.
            out->truncated = true;
            return false;
        }
        S3Listing chunk;
        std::wstring perr;
        if (!ParseTreeJson(r.out, prefixUtf8, &chunk, &perr)) {
            if (error) *error = {AwsErrorKind::Other, perr};
            out->truncated = true;
            return false;
        }
        out->entries.insert(out->entries.end(), std::make_move_iterator(chunk.entries.begin()),
                            std::make_move_iterator(chunk.entries.end()));
        token = chunk.nextToken;
        const bool capped = maxItems > 0 && out->entries.size() >= static_cast<size_t>(maxItems);
        if (token.empty()) return true;  // complete
        if (capped) {
            out->truncated = true;
            out->nextToken = token;
            return true;
        }
        if (onChunk && onChunk(out->entries.size())) {
            out->truncated = true;
            if (error) *error = {AwsErrorKind::Cancelled, L"Cancelled"};
            return false;
        }
    }
}

bool ListBuckets(const AwsContext& ctx, std::vector<S3Entry>* out, AwsError* error) {
    AwsResult r = RunAws(ctx.awsExe,
                         {L"s3api", L"list-buckets", L"--profile", ctx.profile, L"--output", L"json",
                          L"--query", L"Buckets[].[Name,CreationDate]"},
                         OptionsFor(ctx));
    if (ToError(r, error)) return false;
    std::wstring perr;
    if (!ParseBucketsJson(r.out, out, &perr)) {
        if (error) *error = {AwsErrorKind::Other, perr};
        return false;
    }
    return true;
}

bool ListFolder(const AwsContext& ctx, const std::wstring& bucket, const std::wstring& prefix,
                int maxItems, S3Listing* out, AwsError* error) {
    std::vector<std::wstring> args = {
        L"s3api", L"list-objects-v2", L"--profile", ctx.profile, L"--bucket", bucket,
        L"--delimiter", L"/", L"--output", L"json",
        L"--query", L"{p:CommonPrefixes[].Prefix,c:Contents[].[Key,Size,LastModified],t:NextToken}"};
    if (!prefix.empty()) {
        args.push_back(L"--prefix");
        args.push_back(prefix);
    }
    if (maxItems > 0) {
        args.push_back(L"--max-items");
        args.push_back(std::to_wstring(maxItems));
    }
    AwsResult r = RunAws(ctx.awsExe, args, OptionsFor(ctx));
    if (ToError(r, error)) return false;
    std::wstring perr;
    if (!ParseListObjectsJson(r.out, WideToUtf8(prefix), out, &perr)) {
        if (error) *error = {AwsErrorKind::Other, perr};
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Write operations

bool PutDirMarker(const AwsContext& ctx, const std::wstring& bucket, const std::wstring& key, AwsError* error) {
    AwsResult r = RunAws(ctx.awsExe,
                         {L"s3api", L"put-object", L"--profile", ctx.profile, L"--bucket", bucket, L"--key", key,
                          L"--output", L"json"},
                         OptionsFor(ctx));
    return !ToError(r, error);
}

std::string BuildDeleteJson(const std::vector<std::wstring>& keys, size_t begin, size_t end) {
    json objects = json::array();
    for (size_t i = begin; i < end && i < keys.size(); ++i) objects.push_back({{"Key", WideToUtf8(keys[i])}});
    return json{{"Objects", objects}, {"Quiet", true}}.dump();
}

bool ParseDeleteErrors(const std::string& text, std::vector<DeleteFailure>* out) {
    try {
        if (text.find_first_not_of(" \t\r\n") == std::string::npos) return true;  // quiet + no errors
        json j = json::parse(text);
        if (auto e = j.find("Errors"); e != j.end() && e->is_array()) {
            for (const auto& item : *e) {
                DeleteFailure f;
                f.key = Utf8ToWide(item.value("Key", ""));
                f.message = Utf8ToWide(item.value("Code", "") + ": " + item.value("Message", ""));
                out->push_back(std::move(f));
            }
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool DeleteObjects(const AwsContext& ctx, const std::wstring& bucket, const std::vector<std::wstring>& keys,
                   std::vector<DeleteFailure>* failures, AwsError* error,
                   const std::function<bool(size_t done)>& onChunk) {
    wchar_t tmpDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpDir);
    const std::wstring file = std::wstring(tmpDir) + L"s3tcmd_delete_" + std::to_wstring(GetCurrentProcessId()) +
                              L"_" + std::to_wstring(GetTickCount64()) + L".json";
    for (size_t begin = 0; begin < keys.size(); begin += kDeleteChunk) {
        const size_t end = (std::min)(keys.size(), begin + kDeleteChunk);
        const std::string body = BuildDeleteJson(keys, begin, end);
        HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            if (error) *error = {AwsErrorKind::Other, L"Cannot write " + file};
            return false;
        }
        DWORD written = 0;
        WriteFile(h, body.data(), static_cast<DWORD>(body.size()), &written, nullptr);
        CloseHandle(h);
        AwsResult r = RunAws(ctx.awsExe,
                             {L"s3api", L"delete-objects", L"--profile", ctx.profile, L"--bucket", bucket,
                              L"--delete", L"file://" + file, L"--output", L"json"},
                             OptionsFor(ctx));
        DeleteFileW(file.c_str());
        if (ToError(r, error)) return false;
        if (!ParseDeleteErrors(r.out, failures)) {
            if (error) *error = {AwsErrorKind::Other, L"Unexpected delete-objects output"};
            return false;
        }
        if (onChunk && onChunk(end)) return true;
    }
    return true;
}

}  // namespace s3tc
