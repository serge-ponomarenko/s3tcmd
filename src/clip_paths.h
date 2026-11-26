// Pure helpers of s3tcmd-clip.exe: turning Total Commander plugin paths (as TC writes them into
// %UL / %WL list files) into s3:// URIs.
//
// Measured in TC 11.58 (user command with %UL in the plugin panel):
// - UTF-8 list file with BOM, one full name per line, CRLF;
// - names are TC display names, e.g. "\\\S3\prof\bucket\dir\colon：x.txt" (DecodeName needed);
// - folders end with a backslash, files do not;
// - with the cursor on ".." and nothing selected, TC passes no list at all.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace s3tc {

// Splits a TC list file (UTF-8 with or without BOM, or UTF-16LE with BOM) into lines.
// Empty lines are dropped; trailing CR/LF are removed.
std::vector<std::wstring> ParseListFile(const std::string& bytes);

struct S3Ref {
    std::wstring profile;  // decoded
    std::wstring bucket;   // decoded
    std::wstring key;      // decoded, '/' separated, no leading or trailing '/'; empty at bucket root
    bool isDir = false;    // TC marked the name as a folder (trailing backslash)
};

// Parses "\\\<rootName>\profile\bucket[\seg...][\]" (root name compared case-insensitively).
// Returns false for other paths and for plugin paths above bucket level (root, profile).
bool ParsePluginPath(const std::wstring& line, const std::wstring& rootName, S3Ref* out);

// "s3://bucket/key", with a trailing '/' for folders; "s3://bucket/" for a bucket.
std::wstring S3Uri(const S3Ref& ref);

// The s3:// URI of a plugin path, or the line unchanged for anything else (local paths, other
// plugins, profile folders), so one hotkey works everywhere.
std::wstring ConvertLineToUri(const std::wstring& line, const std::wstring& rootName);

// Picks the TC root name of this plugin from the [FileSystemPlugins] entries of wincmd.ini
// (name = path to the .wfx/.wfx64). Matches entries whose file name equals pluginFileName
// (case-insensitive); returns "S3" if none matches.
std::wstring PickPluginRootName(const std::vector<std::pair<std::wstring, std::wstring>>& entries,
                                const std::wstring& pluginFileName);

// Same, reading [FileSystemPlugins] from the given wincmd.ini ("S3" if unreadable).
std::wstring ReadPluginRootName(const std::wstring& wincmdIni, const std::wstring& pluginFileName);

// The folder passed as "--cwd %P" (the last parameter). TC quotes %P when it contains a space, and
// the closing quote follows the trailing backslash, so CommandLineToArgvW yields 'C:\x\a b"' (the
// backslash escapes the quote). Without quoting, a space splits it into several arguments.
// Joins the arguments with spaces, drops quotes and makes sure the result ends with a backslash.
std::wstring CwdFromArgs(const std::vector<std::wstring>& args);

// Region from `s3api get-bucket-location` output: null/"" = us-east-1, "EU" = eu-west-1.
// Returns "" if the output cannot be parsed.
std::wstring RegionFromLocationJson(const std::string& json);

}  // namespace s3tc
