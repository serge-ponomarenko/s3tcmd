// Runs a child process without a console window, captures stdout/stderr, supports cancellation.
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace s3tc {

struct ProcessResult {
    bool started = false;     // false if CreateProcess failed (see winError)
    bool cancelled = false;   // true if shouldCancel returned true and the process was killed
    bool timedOut = false;
    DWORD winError = 0;
    DWORD exitCode = 0;
    std::string out;          // raw stdout bytes (UTF-8 when PYTHONUTF8=1 is honoured)
    std::string err;          // raw stderr bytes
    DWORD elapsedMs = 0;
};

struct ProcessOptions {
    // Extra environment variables (NAME=VALUE) that override the inherited environment.
    std::vector<std::wstring> env;
    // Called on the calling thread with each new stdout chunk.
    std::function<void(const std::string& chunk)> onStdout;
    // Polled on the calling thread (about every 50 ms); return true to kill the process tree.
    std::function<bool()> shouldCancel;
    DWORD timeoutMs = INFINITE;
};

ProcessResult RunProcess(const std::wstring& program, const std::vector<std::wstring>& args,
                         const ProcessOptions& options);

}  // namespace s3tc
