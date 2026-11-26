// String and command-line helpers shared by the plugin and the tests.
#pragma once

#include <string>
#include <vector>

namespace s3tc {

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);
std::wstring AnsiToWide(const char* s);
std::string WideToAnsi(const std::wstring& s);

// Quotes one argument following the MSVC CRT parsing rules (CommandLineToArgvW compatible).
std::wstring QuoteArg(const std::wstring& arg);

// Builds a full command line: quoted program path followed by quoted arguments.
std::wstring BuildCommandLine(const std::wstring& program, const std::vector<std::wstring>& args);

// Splits text into lines, accepting "\n", "\r\n" and bare "\r" as separators. Empty lines are dropped.
std::vector<std::string> SplitLines(const std::string& text);

// Returns the directory part of a path including the trailing backslash ("" if none).
std::wstring DirectoryOf(const std::wstring& path);

}  // namespace s3tc
