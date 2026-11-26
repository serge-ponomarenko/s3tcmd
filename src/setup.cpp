#include "setup.h"

#include <windows.h>

#include <cwctype>

namespace s3tc {

namespace {

std::wstring GetEnv(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, _countof(buf));
    return (n > 0 && n < _countof(buf)) ? std::wstring(buf) : std::wstring();
}

std::wstring ExpandEnvironment(const std::wstring& s) {
    if (s.find(L'%') == std::wstring::npos) return s;
    wchar_t buf[MAX_PATH * 4];
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, _countof(buf));
    return (n > 0 && n <= _countof(buf)) ? std::wstring(buf) : s;
}

std::wstring ReadIni(const std::wstring& ini, const std::wstring& section, const wchar_t* key) {
    wchar_t buf[2048];
    GetPrivateProfileStringW(section.c_str(), key, L"", buf, _countof(buf), ini.c_str());
    return buf;
}

bool SectionExists(const std::wstring& ini, const std::wstring& section) {
    wchar_t buf[64];
    // Returns the key names of the section; 0 characters for a missing (or empty) section.
    return GetPrivateProfileStringW(section.c_str(), nullptr, L"", buf, _countof(buf), ini.c_str()) > 0;
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsAscii(const std::wstring& s) {
    for (wchar_t c : s) {
        if (c > 0x7E) return false;
    }
    return true;
}

}  // namespace

const std::vector<UserCommand>& OurUserCommands() {
    static const std::vector<UserCommand> commands = {
        {L"em_s3_copy_uri", L"uri %Y%y %UL --cwd %P", L"Copy names with full path (s3:// inside S3)"},
        {L"em_s3_presign", L"presign %UL", L"Copy presigned URL (S3 files)"},
    };
    return commands;
}

std::wstring PortablePath(const std::wstring& path, const std::vector<std::pair<std::wstring, std::wstring>>& vars) {
    for (const auto& [name, value] : vars) {
        std::wstring dir = value;
        while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
        if (dir.empty() || path.size() <= dir.size()) continue;
        if (_wcsnicmp(path.c_str(), dir.c_str(), dir.size()) != 0) continue;
        if (path[dir.size()] != L'\\') continue;  // whole directory names only
        return L"%" + name + L"%" + path.substr(dir.size());
    }
    return path;
}

std::wstring PortablePath(const std::wstring& path) {
    std::vector<std::pair<std::wstring, std::wstring>> vars;
    for (const wchar_t* name : {L"COMMANDER_PATH", L"APPDATA", L"LOCALAPPDATA"}) {
        std::wstring v = GetEnv(name);
        if (!v.empty()) vars.emplace_back(name, v);
    }
    return PortablePath(path, vars);
}

std::wstring UserCmdIniPath(const std::wstring& fallbackDir) {
    std::wstring ini = GetEnv(L"COMMANDER_INI");
    std::wstring dir = fallbackDir;
    if (!ini.empty()) {
        ini = ExpandEnvironment(ini);
        size_t slash = ini.find_last_of(L"\\/");
        if (slash != std::wstring::npos) dir = ini.substr(0, slash + 1);
    }
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir.push_back(L'\\');
    return dir + L"usercmd.ini";
}

std::vector<UserCmdStatus> CheckUserCommands(const std::wstring& userCmdIni) {
    std::vector<UserCmdStatus> out;
    const bool exists = FileExists(userCmdIni);
    for (const auto& c : OurUserCommands()) {
        UserCmdStatus s;
        s.section = c.section;
        if (exists && SectionExists(userCmdIni, c.section)) {
            s.cmd = ReadIni(userCmdIni, c.section, L"cmd");
            if (s.cmd.empty()) {
                s.state = UserCmdState::Missing;
            } else {
                std::wstring exe = ExpandEnvironment(s.cmd);
                if (exe.size() >= 2 && exe.front() == L'"' && exe.back() == L'"') exe = exe.substr(1, exe.size() - 2);
                // Only absolute paths can be judged; TC resolves relative ones itself.
                const bool absolute = (exe.size() > 2 && exe[1] == L':') || exe.rfind(L"\\\\", 0) == 0;
                s.state = (absolute && !FileExists(exe)) ? UserCmdState::StaleCmd : UserCmdState::Ok;
            }
        }
        out.push_back(s);
    }
    return out;
}

bool NeedsSetup(const std::vector<UserCmdStatus>& status) {
    for (const auto& s : status) {
        if (s.state != UserCmdState::Ok) return true;
    }
    return false;
}

SetupResult ApplyUserCommands(const std::wstring& userCmdIni, const std::wstring& clipExe) {
    SetupResult r;
    const auto status = CheckUserCommands(userCmdIni);
    if (!NeedsSetup(status)) {
        r.ok = true;
        return r;
    }
    if (FileExists(userCmdIni)) {
        r.backup = userCmdIni + L".s3tcmd-bak";
        if (!CopyFileW(userCmdIni.c_str(), r.backup.c_str(), FALSE)) {
            r.error = L"Cannot create the backup " + r.backup + L" (error " + std::to_wstring(GetLastError()) + L")";
            return r;
        }
    } else if (!IsAscii(clipExe)) {
        // A new file created by WritePrivateProfileString is ANSI. Start it as UTF-16LE (BOM), which
        // the profile API and TC read, so a non-ASCII install path survives.
        HANDLE h = CreateFileW(userCmdIni.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const unsigned char bom[] = {0xFF, 0xFE};
            DWORD written = 0;
            WriteFile(h, bom, sizeof(bom), &written, nullptr);
            CloseHandle(h);
        }
    }
    const auto& commands = OurUserCommands();
    for (size_t i = 0; i < commands.size(); ++i) {
        const auto& c = commands[i];
        const auto state = status[i].state;
        if (state == UserCmdState::Ok) continue;
        bool ok = WritePrivateProfileStringW(c.section.c_str(), L"cmd", clipExe.c_str(), userCmdIni.c_str()) != 0;
        if (ok && state == UserCmdState::Missing) {
            // Keep a param=/menu= the user already has (a section without cmd=).
            if (ReadIni(userCmdIni, c.section, L"param").empty()) {
                ok = WritePrivateProfileStringW(c.section.c_str(), L"param", c.param.c_str(), userCmdIni.c_str()) != 0;
            }
            if (ok && ReadIni(userCmdIni, c.section, L"menu").empty()) {
                ok = WritePrivateProfileStringW(c.section.c_str(), L"menu", c.menu.c_str(), userCmdIni.c_str()) != 0;
            }
        }
        if (!ok) {
            r.error = L"Cannot write " + userCmdIni + L" (error " + std::to_wstring(GetLastError()) + L")";
            return r;
        }
        r.written.push_back(c.section);
    }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, userCmdIni.c_str());  // flush the profile cache
    r.ok = true;
    return r;
}

std::vector<std::pair<std::wstring, std::wstring>> ReadShortcuts(const std::wstring& wincmdIni) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    if (wincmdIni.empty()) return out;
    std::vector<wchar_t> buf(65536);
    DWORD n = GetPrivateProfileSectionW(L"Shortcuts", buf.data(), static_cast<DWORD>(buf.size()), wincmdIni.c_str());
    for (const wchar_t* p = buf.data(); p < buf.data() + n && *p; p += wcslen(p) + 1) {
        std::wstring line = p;
        size_t eq = line.find(L'=');
        if (eq != std::wstring::npos && eq > 0) out.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
    return out;
}

std::wstring HotkeyDisplayName(const std::wstring& key) {
    size_t plus = key.find(L'+');
    if (plus == std::wstring::npos || plus == 0 || plus + 1 >= key.size()) return key;
    std::wstring out;
    for (size_t i = 0; i < plus; ++i) {
        switch (towupper(key[i])) {
            case L'C': out += L"Ctrl+"; break;
            case L'A': out += L"Alt+"; break;
            case L'S': out += L"Shift+"; break;
            case L'W': out += L"Win+"; break;
            default: return key;
        }
    }
    return out + key.substr(plus + 1);
}

std::wstring DescribeCopyHotkey(const std::vector<std::pair<std::wstring, std::wstring>>& shortcuts) {
    std::wstring bound, ctrlShiftC;
    for (const auto& [key, command] : shortcuts) {
        if (_wcsicmp(command.c_str(), L"em_s3_copy_uri") == 0) {
            if (!bound.empty()) bound += L", ";
            bound += HotkeyDisplayName(key);
        }
        if (_wcsicmp(key.c_str(), L"CS+C") == 0) ctrlShiftC = command;
    }
    if (!bound.empty()) return L"Hotkey for em_s3_copy_uri: " + bound + L".";
    std::wstring text = L"No hotkey runs em_s3_copy_uri yet. ";
    if (ctrlShiftC.empty()) {
        // Not in [Shortcuts]: TC's built-in default (copy names with full path).
        text += L"Ctrl+Shift+C has its default meaning (cm_CopyFullNamesToClip).";
    } else {
        text += L"Ctrl+Shift+C currently runs " + ctrlShiftC + L".";
    }
    return text;
}

}  // namespace s3tc
