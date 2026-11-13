#pragma once

#include <windows.h>

#include <string>

// The first-run wizard: where the game is, and the plugins the editor's work
// needs inside it.
//
// The editor reads the game's own files, so all it needs to open a region is
// the folder that holds SPEED2.EXE. What it writes goes to <game>\models, and
// reaches the game through plugins loaded by an ASI loader:
//
//   dinput8.dll                  Ultimate ASI Loader (ThirteenAG), unless the
//                                game already has an ASI loader
//   scripts\FRSWorldLoader.asi   the editor's end in the game: edited meshes,
//                                sections and replaced files
//   scripts\FRSFastBoot.asi      Launch Game from the editor boots straight
//                                into free roam
//
// They come from the "game" folder beside the editor, as the release package
// lays them out. A game under Program Files is not writable without
// elevation, so the copying can run again as administrator: the editor starts
// itself with --install and the same choices (RunInstallCommand).

struct SetupChoices {
    std::wstring folder;
    bool asiLoader = true;       // copy dinput8.dll (only when no loader is there)
    bool fastBoot = true;
    bool fastBootAlways = false; // fast boot on every start, not only from the editor
    bool disableOld = true;      // rename older copies of the plugins to .off
};

// Runs the wizard. True with the chosen folder in `folder` when it finished;
// `folder` comes in as the folder to propose, if any.
bool RunSetupWizard(HINSTANCE instance, HWND owner, std::wstring& folder);

// The folder is one the editor can read: SPEED2.EXE and TRACKS.
bool IsGameFolder(const std::wstring& folder);

// Where the game is installed, as far as the registry and the usual places
// know. Empty when it was not found.
std::wstring DetectGameFolder();

// The editor was started with --install: does the copying and exits with 0
// when it all went in. Returns false when the command line is not an install.
bool RunInstallCommand(int& exitCode);

// The FRSFastBoot switch: the editor turns it on for the launch it makes and
// back off once the game has read it, unless the player wants it always.
std::wstring FastBootIni(const std::wstring& gameFolder);
bool FastBootInstalled(const std::wstring& gameFolder);
bool WorldLoaderInstalled(const std::wstring& gameFolder);
bool FastBootEnabled(const std::wstring& gameFolder);
bool SetFastBootEnabled(const std::wstring& gameFolder, bool enabled);
