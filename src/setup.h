// One-time setup of the TC user commands of s3tcmd-clip.exe (em_s3_copy_uri, em_s3_presign) in
// usercmd.ini, offered by the plugin after an install or upgrade (TC's plugin installer only
// copies files). Only our sections are written (WritePrivateProfileString keeps everything else);
// the hotkey in wincmd.ini [Shortcuts] is never written, only reported.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace s3tc {

struct UserCommand {
    std::wstring section;  // "em_s3_copy_uri"
    std::wstring param;    // "uri %Y%y %UL --cwd %P"
    std::wstring menu;     // description shown by TC
};

// em_s3_copy_uri and em_s3_presign.
const std::vector<UserCommand>& OurUserCommands();

enum class UserCmdState {
    Ok,        // section present and cmd= names an existing file (or cannot be judged)
    Missing,   // no such section, or no cmd=
    StaleCmd,  // cmd= names a file that does not exist (plugin moved or reinstalled elsewhere)
};

struct UserCmdStatus {
    std::wstring section;
    UserCmdState state = UserCmdState::Missing;
    std::wstring cmd;  // current cmd= value (unexpanded)
};

// Replaces a leading %COMMANDER_PATH%, %APPDATA% or %LOCALAPPDATA% directory (values given in
// `vars`, first match wins) by the variable, so the entry keeps working with roaming profiles
// and portable TC installs. Case-insensitive; only whole directory prefixes are replaced.
std::wstring PortablePath(const std::wstring& path, const std::vector<std::pair<std::wstring, std::wstring>>& vars);

// Same, with the current environment.
std::wstring PortablePath(const std::wstring& path);

// usercmd.ini next to wincmd.ini (%COMMANDER_INI%); fallbackDir if the variable is not set.
std::wstring UserCmdIniPath(const std::wstring& fallbackDir);

// State of our sections in usercmd.ini (the file may not exist).
std::vector<UserCmdStatus> CheckUserCommands(const std::wstring& userCmdIni);

bool NeedsSetup(const std::vector<UserCmdStatus>& status);

struct SetupResult {
    bool ok = false;
    std::wstring backup;                 // backup of usercmd.ini ("" if the file did not exist)
    std::vector<std::wstring> written;   // sections added or fixed
    std::wstring error;
};

// Writes the Missing sections completely and fixes cmd= of StaleCmd sections (param= and menu=
// of an existing section are kept). Copies usercmd.ini to usercmd.ini.s3tcmd-bak first.
// clipExe is the value for cmd= (already made portable). Idempotent: nothing is written (and no
// backup made) when all sections are Ok.
SetupResult ApplyUserCommands(const std::wstring& userCmdIni, const std::wstring& clipExe);

// Hotkeys of wincmd.ini [Shortcuts] (lines "CS+C=em_s3_copy_uri").
std::vector<std::pair<std::wstring, std::wstring>> ReadShortcuts(const std::wstring& wincmdIni);

// "CS+C" -> "Ctrl+Shift+C", "A+F1" -> "Alt+F1", "W" = Win.
std::wstring HotkeyDisplayName(const std::wstring& key);

// Text about the hotkey for em_s3_copy_uri: which keys run it, and what Ctrl+Shift+C runs now.
std::wstring DescribeCopyHotkey(const std::vector<std::pair<std::wstring, std::wstring>>& shortcuts);

}  // namespace s3tc
