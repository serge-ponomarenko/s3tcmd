// Minimal console host that loads a WFX plugin like Total Commander does and lists a path.
// Usage: wfxhost.exe <plugin.wfx64> <ini-dir> [path ...]
// Lets us exercise the plugin (and aws.exe calls) without TC. Not shipped.
#include <windows.h>

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "../sdk/fsplugin_cpp.h"

namespace {

// WFXHOST_CANCEL_MS=<n>: simulate the user pressing Cancel n ms after the first progress call.
int __stdcall ProgressW(int, WCHAR* src, WCHAR* dst, int pct) {
    static ULONGLONG first = 0;
    static long long cancelMs = [] {
        wchar_t buf[32];
        return GetEnvironmentVariableW(L"WFXHOST_CANCEL_MS", buf, 32) ? _wtoi64(buf) : -1;
    }();
    if (!first) first = GetTickCount64();
    std::wprintf(L"  progress %3d%% %ls -> %ls\n", pct, src, dst);
    if (cancelMs >= 0 && GetTickCount64() - first > static_cast<ULONGLONG>(cancelMs)) {
        std::wprintf(L"  progress: CANCEL\n");
        return 1;
    }
    return 0;
}
void __stdcall LogW(int, int type, WCHAR* text) { std::wprintf(L"  log[%d] %ls\n", type, text); }
// Messages and questions are printed. Questions are answered "no", or "yes" with WFXHOST_ANSWER=yes.
BOOL __stdcall RequestW(int, int type, WCHAR* title, WCHAR* text, WCHAR*, int) {
    wchar_t answer[8] = {};
    GetEnvironmentVariableW(L"WFXHOST_ANSWER", answer, 8);
    const BOOL yes = _wcsicmp(answer, L"yes") == 0;
    std::wprintf(L"  request[%d] %ls: %ls\n", type, title ? title : L"", text ? text : L"");
    if (type == 9 /* RT_MsgYesNo */) std::wprintf(L"  answer: %ls\n", yes ? L"yes" : L"no");
    return yes;
}

template <typename T>
T Get(HMODULE m, const char* name) {
    auto p = reinterpret_cast<T>(GetProcAddress(m, name));
    if (!p) std::printf("missing export %s\n", name);
    return p;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: wfxhost <plugin.wfx64> <ini-dir> [path ...]\n");
        return 2;
    }
    HMODULE m = LoadLibraryW(argv[1]);
    if (!m) {
        std::printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    auto initW = Get<decltype(&FsInitW)>(m, "FsInitW");
    auto setParams = Get<decltype(&FsSetDefaultParams)>(m, "FsSetDefaultParams");
    auto findFirstW = Get<decltype(&FsFindFirstW)>(m, "FsFindFirstW");
    auto findNextW = Get<decltype(&FsFindNextW)>(m, "FsFindNextW");
    auto findClose = Get<decltype(&FsFindClose)>(m, "FsFindClose");
    if (!initW || !setParams || !findFirstW || !findNextW || !findClose) return 1;

    initW(1, ProgressW, LogW, RequestW);
    FsDefaultParamStruct dps{};
    dps.size = sizeof(dps);
    dps.PluginInterfaceVersionHi = 2;
    dps.PluginInterfaceVersionLow = 10;
    std::string iniDir;
    for (const wchar_t* p = argv[2]; *p; ++p) iniDir.push_back(static_cast<char>(*p));
    strcpy_s(dps.DefaultIniName, (iniDir + "\\fsplugin.ini").c_str());
    setParams(&dps);

    auto statusW = Get<decltype(&FsStatusInfoW)>(m, "FsStatusInfoW");

    // "calc:<path>" imitates Space (CALCSIZE): a recursive walk inside FsStatusInfo START/END.
    struct Walker {
        decltype(&FsFindFirstW) first;
        decltype(&FsFindNextW) next;
        decltype(&FsFindClose) close;
        int dirs = 0, files = 0;
        ULONGLONG bytes = 0;
        void Walk(const std::wstring& path) {
            ++dirs;
            WIN32_FIND_DATAW fd;
            HANDLE h = first(const_cast<WCHAR*>(path.c_str()), &fd);
            if (h == INVALID_HANDLE_VALUE) return;
            std::vector<std::wstring> subdirs;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    subdirs.push_back(path + L"\\" + fd.cFileName);
                } else {
                    ++files;
                    bytes += (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                }
            } while (next(h, &fd));
            close(h);
            for (const auto& d : subdirs) Walk(d);
        }
    };

    auto getFileW = Get<decltype(&FsGetFileW)>(m, "FsGetFileW");
    auto putFileW = Get<decltype(&FsPutFileW)>(m, "FsPutFileW");
    auto mkDirW = Get<decltype(&FsMkDirW)>(m, "FsMkDirW");
    auto deleteFileW = Get<decltype(&FsDeleteFileW)>(m, "FsDeleteFileW");
    auto removeDirW = Get<decltype(&FsRemoveDirW)>(m, "FsRemoveDirW");
    auto renMovW = Get<decltype(&FsRenMovFileW)>(m, "FsRenMovFileW");

    // "put:<local dir>=<remote dir>" imitates F5 of a local folder into a plugin folder (PUT_MULTI):
    // FsMkDir for each folder, FsPutFile per file (retry with OVERWRITE on FS_FILE_EXISTS).
    struct Uploader {
        decltype(&FsPutFileW) put;
        decltype(&FsMkDirW) mkdir;
        int files = 0, failed = 0;
        void Upload(const std::wstring& local, const std::wstring& remote) {
            mkdir(const_cast<WCHAR*>(remote.c_str()));
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((local + L"\\*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) return;
            std::vector<WIN32_FIND_DATAW> entries;
            do {
                if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) entries.push_back(fd);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
            for (const auto& e : entries) {
                std::wstring l = local + L"\\" + e.cFileName, r = remote + L"\\" + e.cFileName;
                if (e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    Upload(l, r);
                    continue;
                }
                int rc = put(l.data(), r.data(), 0);
                if (rc == FS_FILE_EXISTS) rc = put(l.data(), r.data(), FS_COPYFLAGS_OVERWRITE | FS_COPYFLAGS_EXISTS_SAMECASE);
                ++files;
                if (rc != FS_FILE_OK) {
                    ++failed;
                    std::wprintf(L"  FsPutFile %ls -> %d\n", r.c_str(), rc);
                }
            }
        }
    };

    // "del:<remote>" imitates F8 on a folder (DELETE): walk, delete files, remove folders bottom-up.
    struct Deleter {
        decltype(&FsFindFirstW) first;
        decltype(&FsFindNextW) next;
        decltype(&FsFindClose) close;
        decltype(&FsDeleteFileW) del;
        decltype(&FsRemoveDirW) rmdir;
        int files = 0, dirs = 0, failed = 0;
        void Delete(const std::wstring& path) {
            WIN32_FIND_DATAW fd;
            HANDLE h = first(const_cast<WCHAR*>(path.c_str()), &fd);
            std::vector<WIN32_FIND_DATAW> entries;
            if (h != INVALID_HANDLE_VALUE) {
                do entries.push_back(fd);
                while (next(h, &fd));
                close(h);
            }
            for (const auto& e : entries) {
                std::wstring p = path + L"\\" + e.cFileName;
                if (e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    Delete(p);
                } else {
                    ++files;
                    if (!del(p.data())) ++failed;
                }
            }
            ++dirs;
            if (!rmdir(const_cast<WCHAR*>(path.c_str()))) ++failed;
        }
    };

    // "copy:<remote dir>=<local dir>" imitates F5 on a folder (GET_MULTI): TC pre-scans the tree,
    // then creates local folders and calls FsGetFile per file (first without, then with OVERWRITE).
    struct Copier {
        decltype(&FsFindFirstW) first;
        decltype(&FsFindNextW) next;
        decltype(&FsFindClose) close;
        decltype(&FsGetFileW) get;
        int files = 0, failed = 0;
        void Copy(const std::wstring& remote, const std::wstring& local) {
            CreateDirectoryW(local.c_str(), nullptr);
            WIN32_FIND_DATAW fd;
            HANDLE h = first(const_cast<WCHAR*>(remote.c_str()), &fd);
            if (h == INVALID_HANDLE_VALUE) return;
            std::vector<WIN32_FIND_DATAW> entries;
            do entries.push_back(fd);
            while (next(h, &fd));
            close(h);
            for (const auto& e : entries) {
                std::wstring r = remote + L"\\" + e.cFileName, l = local + L"\\" + e.cFileName;
                if (e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    Copy(r, l);
                    continue;
                }
                RemoteInfoStruct ri{e.nFileSizeLow, e.nFileSizeHigh, e.ftLastWriteTime, 0};
                int rc = get(r.data(), l.data(), 0, &ri);
                if (rc == FS_FILE_EXISTS) rc = get(r.data(), l.data(), FS_COPYFLAGS_OVERWRITE, &ri);
                ++files;
                if (rc != FS_FILE_OK) {
                    ++failed;
                    std::wprintf(L"  FsGetFile %ls -> %d\n", r.c_str(), rc);
                }
            }
        }
    };

    auto runOne = [&](std::wstring arg) {
        DWORD t0 = GetTickCount();
        if (arg.rfind(L"copy:", 0) == 0 && statusW && getFileW) {
            size_t eq = arg.find(L'=');
            std::wstring remote = arg.substr(5, eq - 5), local = arg.substr(eq + 1);
            std::wstring parent = remote.substr(0, remote.find_last_of(L'\\') + 1);
            statusW(parent.data(), FS_STATUS_START, FS_STATUS_OP_GET_MULTI);
            Walker pre{findFirstW, findNextW, findClose};
            pre.Walk(remote);  // TC's pre-scan
            Copier c{findFirstW, findNextW, findClose, getFileW};
            c.Copy(remote, local);
            statusW(parent.data(), FS_STATUS_END, FS_STATUS_OP_GET_MULTI);
            std::wprintf(L"copy %ls: %d files, %d failed (%lu ms)\n", remote.c_str(), c.files, c.failed,
                         GetTickCount() - t0);
            return;
        }
        if (arg.rfind(L"put:", 0) == 0 && statusW && putFileW && mkDirW) {
            size_t eq = arg.find(L'=');
            std::wstring local = arg.substr(4, eq - 4), remoteDir = arg.substr(eq + 1);
            std::wstring target = remoteDir + L"\\" + local.substr(local.find_last_of(L'\\') + 1);
            statusW(remoteDir.data(), FS_STATUS_START, FS_STATUS_OP_PUT_MULTI);
            Uploader u{putFileW, mkDirW};
            u.Upload(local, target);
            statusW(remoteDir.data(), FS_STATUS_END, FS_STATUS_OP_PUT_MULTI);
            std::wprintf(L"put %ls -> %ls: %d files, %d failed (%lu ms)\n", local.c_str(), target.c_str(), u.files,
                         u.failed, GetTickCount() - t0);
            return;
        }
        if (arg.rfind(L"mkdir:", 0) == 0 && statusW && mkDirW) {
            std::wstring path = arg.substr(6);
            statusW(path.data(), FS_STATUS_START, FS_STATUS_OP_MKDIR);
            BOOL ok = mkDirW(path.data());
            statusW(path.data(), FS_STATUS_END, FS_STATUS_OP_MKDIR);
            std::wprintf(L"mkdir %ls -> %d (%lu ms)\n", path.c_str(), ok, GetTickCount() - t0);
            return;
        }
        if (arg.rfind(L"del:", 0) == 0 && statusW && deleteFileW && removeDirW) {
            std::wstring path = arg.substr(4);
            std::wstring parent = path.substr(0, path.find_last_of(L'\\') + 1);
            statusW(parent.data(), FS_STATUS_START, FS_STATUS_OP_DELETE);
            Deleter d{findFirstW, findNextW, findClose, deleteFileW, removeDirW};
            d.Delete(path);
            statusW(parent.data(), FS_STATUS_END, FS_STATUS_OP_DELETE);
            std::wprintf(L"del %ls: %d files, %d dirs, %d failed (%lu ms)\n", path.c_str(), d.files, d.dirs, d.failed,
                         GetTickCount() - t0);
            return;
        }
        if (arg.rfind(L"ren:", 0) == 0 && statusW && renMovW) {
            // "ren:<old>=<new>" (folder) or "renf:<old>=<new>" is not distinguished: folders end with '\'.
            size_t eq = arg.find(L'=');
            std::wstring oldp = arg.substr(4, eq - 4), newp = arg.substr(eq + 1);
            bool dir = !oldp.empty() && oldp.back() == L'\\';
            if (dir) oldp.pop_back();
            RemoteInfoStruct ri{};
            if (dir) ri.SizeHigh = 0xFFFFFFFF;
            statusW(oldp.data(), FS_STATUS_START, FS_STATUS_OP_RENMOV_SINGLE);
            int rc = renMovW(oldp.data(), newp.data(), TRUE, FALSE, &ri);
            statusW(oldp.data(), FS_STATUS_END, FS_STATUS_OP_RENMOV_SINGLE);
            std::wprintf(L"ren %ls -> %ls: %d (%lu ms)\n", oldp.c_str(), newp.c_str(), rc, GetTickCount() - t0);
            return;
        }
        if (arg.rfind(L"get1:", 0) == 0 && statusW && getFileW) {
            size_t eq = arg.find(L'=');
            std::wstring remote = arg.substr(5, eq - 5), local = arg.substr(eq + 1);
            statusW(remote.data(), FS_STATUS_START, FS_STATUS_OP_GET_SINGLE);
            RemoteInfoStruct ri{};
            int rc = getFileW(remote.data(), local.data(), FS_COPYFLAGS_OVERWRITE, &ri);
            statusW(remote.data(), FS_STATUS_END, FS_STATUS_OP_GET_SINGLE);
            std::wprintf(L"get1 %ls -> %d (%lu ms)\n", remote.c_str(), rc, GetTickCount() - t0);
            return;
        }
        if (arg.rfind(L"calc:", 0) == 0 && statusW) {
            std::wstring path = arg.substr(5);
            statusW(path.data(), FS_STATUS_START, FS_STATUS_OP_CALCSIZE);
            Walker w{findFirstW, findNextW, findClose};
            w.Walk(path);
            statusW(path.data(), FS_STATUS_END, FS_STATUS_OP_CALCSIZE);
            std::wprintf(L"calc %ls: %d dirs, %d files, %llu bytes (%lu ms)\n", path.c_str(), w.dirs,
                         w.files, w.bytes, GetTickCount() - t0);
            return;
        }
        if (arg.rfind(L"exec:", 0) == 0) {
            // "exec:<dir>|<command>" imitates a command typed on TC's command line in <dir>.
            auto execW = Get<decltype(&FsExecuteFileW)>(m, "FsExecuteFileW");
            size_t bar = arg.find(L'|');
            if (!execW || bar == std::wstring::npos) return;
            std::wstring dir = arg.substr(5, bar - 5), verb = L"quote " + arg.substr(bar + 1);
            int rc = execW(nullptr, dir.data(), verb.data());
            std::wprintf(L"exec %ls: %d (%lu ms)\n", verb.c_str(), rc, GetTickCount() - t0);
            if (rc == FS_EXEC_OK && statusW) {  // TC re-lists the panel after FS_EXEC_OK
                statusW(dir.data(), FS_STATUS_START, FS_STATUS_OP_LIST);
                WIN32_FIND_DATAW fd;
                HANDLE h = findFirstW(dir.data(), &fd);
                if (h != INVALID_HANDLE_VALUE) findClose(h);
                statusW(dir.data(), FS_STATUS_END, FS_STATUS_OP_LIST);
            }
            return;
        }
        // "list:<path>" is a panel listing (inside FS_STATUS_OP_LIST, like TC); a bare path is not.
        const bool panel = arg.rfind(L"list:", 0) == 0 && statusW;
        if (panel) arg = arg.substr(5);
        if (panel) statusW(arg.data(), FS_STATUS_START, FS_STATUS_OP_LIST);
        struct EndList {
            std::remove_reference_t<decltype(statusW)> s;
            std::wstring* p;
            ~EndList() {
                if (s) s(p->data(), FS_STATUS_END, FS_STATUS_OP_LIST);
            }
        } endList{panel ? statusW : nullptr, &arg};
        WIN32_FIND_DATAW fd;
        HANDLE h = findFirstW(const_cast<WCHAR*>(arg.c_str()), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            std::wprintf(L"%ls: <none> (error %lu)\n", const_cast<WCHAR*>(arg.c_str()), GetLastError());
            return;
        }
        int count = 0;
        std::wprintf(L"%ls:\n", const_cast<WCHAR*>(arg.c_str()));
        do {
            ULONGLONG size = (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            std::wprintf(L"  %-5ls %12llu  %ls\n", dir ? L"<DIR>" : L"", size, fd.cFileName);
            ++count;
        } while (findNextW(h, &fd));
        findClose(h);
        std::wprintf(L"  (%d entries, %lu ms)\n", count, GetTickCount() - t0);
    };

    // "--parallel": every following command runs in its own thread at the same time, like a TC
    // background transfer next to panel navigation.
    bool parallel = false;
    std::vector<std::thread> threads;
    for (int i = 3; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--parallel") {
            parallel = true;
        } else if (parallel) {
            threads.emplace_back(runOne, arg);
        } else {
            runOne(arg);
        }
    }
    for (auto& t : threads) t.join();
    Sleep(300);  // a "go to s3://" redirect thread may still be finishing inside the plugin
    FreeLibrary(m);
    return 0;
}
