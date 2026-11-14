#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Asks for the name of a new world; `taken` are the names already in use.
// Returns false when cancelled.
bool AskNewWorldName(HINSTANCE instance, HWND parent, const std::vector<std::wstring>& taken,
                     std::wstring& name);

// What a published world says about itself (File > Publish World). `note`
// is shown above the fields; the rest come in as defaults and go out edited.
struct PublishDetails {
    std::wstring note;
    std::wstring name;
    std::wstring author;
    std::wstring description;
};
bool AskPublishDetails(HINSTANCE instance, HWND parent, PublishDetails& details);

// Picks one of the stored worlds to open in place of `active`.
bool ChooseWorldToOpen(HINSTANCE instance, HWND parent, const std::wstring& active,
                       const std::vector<std::wstring>& worlds, std::wstring& chosen);
