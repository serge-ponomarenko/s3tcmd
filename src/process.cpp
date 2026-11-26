#include "process.h"

#include <map>
#include <mutex>
#include <thread>

#include "util.h"

namespace s3tc {

namespace {

struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE x) : h(x) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset(HANDLE x = nullptr) {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = x;
    }
};

// Case-insensitive key comparison for environment variable names.
struct EnvLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};

// Builds a sorted Unicode environment block: the inherited environment plus overrides.
std::wstring BuildEnvironmentBlock(const std::vector<std::wstring>& overrides) {
    std::map<std::wstring, std::wstring, EnvLess> vars;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            std::wstring entry(p);
            // Entries like "=C:=C:\dir" start with '=': keep them, the name includes the first '='.
            size_t eq = entry.find(L'=', 1);
            if (eq == std::wstring::npos) continue;
            vars[entry.substr(0, eq)] = entry.substr(eq + 1);
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& o : overrides) {
        size_t eq = o.find(L'=');
        if (eq == std::wstring::npos || eq == 0) continue;
        vars[o.substr(0, eq)] = o.substr(eq + 1);
    }
    std::wstring out;
    for (const auto& [k, v] : vars) {
        out += k;
        out += L'=';
        out += v;
        out += L'\0';
    }
    out += L'\0';
    return out;
}

// Reads a pipe until EOF, appending to a shared buffer.
void PipeReader(HANDLE pipe, std::string* buffer, std::mutex* mutex) {
    char chunk[8192];
    DWORD read = 0;
    while (ReadFile(pipe, chunk, sizeof(chunk), &read, nullptr) && read > 0) {
        std::lock_guard<std::mutex> lock(*mutex);
        buffer->append(chunk, read);
    }
}

}  // namespace

ProcessResult RunProcess(const std::wstring& program, const std::vector<std::wstring>& args,
                         const ProcessOptions& options) {
    ProcessResult result;
    const DWORD startTick = GetTickCount();

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0) || !CreatePipe(&errRead, &errWrite, &sa, 0)) {
        result.winError = GetLastError();
        return result;
    }
    Handle hOutRead(outRead), hOutWrite(outWrite), hErrRead(errRead), hErrWrite(errWrite);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

    Handle hNul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                            OPEN_EXISTING, 0, nullptr));

    // Inherit only our three handles, not every inheritable handle Total Commander owns.
    HANDLE inheritList[3] = {hNul.h, outWrite, errWrite};
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrBuf(attrSize);
    auto attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize);
    UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritList,
                              sizeof(inheritList), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = hNul.h;
    si.StartupInfo.hStdOutput = outWrite;
    si.StartupInfo.hStdError = errWrite;
    si.lpAttributeList = attrList;

    // A job object makes sure cancellation also kills grandchildren of aws.exe.
    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));

    std::wstring cmdLine = BuildCommandLine(program, args);
    std::wstring env = BuildEnvironmentBlock(options.env);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(program.c_str(), cmdLine.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                                 EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
                             env.data(), nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrList);
    if (!ok) {
        result.winError = GetLastError();
        return result;
    }
    result.started = true;
    Handle hProcess(pi.hProcess), hThread(pi.hThread);
    AssignProcessToJobObject(job.h, pi.hProcess);
    ResumeThread(pi.hThread);

    // Close our copies of the write ends so the readers see EOF when the child exits.
    hOutWrite.reset();
    hErrWrite.reset();
    hNul.reset();

    std::mutex mutex;
    std::string pendingOut;
    std::thread outThread(PipeReader, outRead, &pendingOut, &mutex);
    std::thread errThread(PipeReader, errRead, &result.err, &mutex);

    auto drain = [&]() {
        std::string chunk;
        {
            std::lock_guard<std::mutex> lock(mutex);
            chunk.swap(pendingOut);
        }
        if (!chunk.empty()) {
            result.out += chunk;
            if (options.onStdout) options.onStdout(chunk);
        }
    };

    for (;;) {
        DWORD wait = WaitForSingleObject(pi.hProcess, 50);
        drain();
        if (wait == WAIT_OBJECT_0) break;
        if (options.shouldCancel && options.shouldCancel()) {
            result.cancelled = true;
            TerminateJobObject(job.h, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
        if (options.timeoutMs != INFINITE && GetTickCount() - startTick > options.timeoutMs) {
            result.timedOut = true;
            TerminateJobObject(job.h, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
    }

    // Kill leftovers that may still hold the pipes open, then collect the remaining output.
    TerminateJobObject(job.h, 1);
    outThread.join();
    errThread.join();
    drain();

    GetExitCodeProcess(pi.hProcess, &result.exitCode);
    result.elapsedMs = GetTickCount() - startTick;
    return result;
}

}  // namespace s3tc
