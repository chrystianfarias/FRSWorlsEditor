#pragma once

#include <windows.h>

#include <string>

// When the game the editor launched ends on its own with an error, a window
// with everything worth reading to find out why: the exit code by name, the
// report Windows wrote (faulting module and offset), the tail of every log
// the game's mods wrote since the launch, and the tail of the editor's own.
// The same text is saved to %LOCALAPPDATA%\FRSWorldEditor\crashes.
namespace crash {

// "ACCESS_VIOLATION" for 0xC0000005 and the like; empty for a plain code.
std::wstring ExitCodeName(DWORD code);

// True for an exit code that is a crash rather than a quit.
bool IsCrash(DWORD code);

std::wstring BuildReport(const std::wstring& gameFolder, const FILETIME& launched, DWORD exitCode);

// Saves the report and shows it in its own window (not modal).
void Show(HWND owner, const std::wstring& report, const std::wstring& gameFolder);

} // namespace crash
