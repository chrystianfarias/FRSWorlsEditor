#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Asks for the name of a new world; `taken` are the names already in use.
// Returns false when cancelled.
bool AskNewWorldName(HINSTANCE instance, HWND parent, const std::vector<std::wstring>& taken,
                     std::wstring& name);

// Picks one of the stored worlds to open in place of `active`.
bool ChooseWorldToOpen(HINSTANCE instance, HWND parent, const std::wstring& active,
                       const std::vector<std::wstring>& worlds, std::wstring& chosen);
