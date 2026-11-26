// Minimal thread-safe debug log (UTF-8 file). Disabled when no file is configured.
#pragma once

#include <string>

namespace s3tc {

void LogSetFile(const std::wstring& path);  // empty path disables logging
bool LogEnabled();
void Log(const wchar_t* fmt, ...);

}  // namespace s3tc
