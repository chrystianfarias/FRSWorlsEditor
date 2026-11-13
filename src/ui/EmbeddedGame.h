#pragma once

#include <windows.h>

#include <string>

// The game, running over the viewport.
//
// It stays its own process with its own top-level window -- made a child of
// the editor across processes, its DirectInput would lose the keyboard. What
// makes it look embedded: its frame is taken off, the editor becomes its owner
// (so it stays over the editor, and minimises and hides with it) and it is
// kept exactly over the viewport's rectangle as the editor moves and resizes.
//
// The game has to run windowed (NFSU2ExtraOptions WindowedMode); a fullscreen
// game is left alone.
class EmbeddedGame {
public:
    ~EmbeddedGame() { Release(); }

    // Starts SPEED2.EXE from its folder. False when it could not be started.
    bool Launch(const std::wstring& gameFolder, HWND owner);

    // True while the game process is alive.
    bool Running() const;

    // Finds the game's window once it exists and keeps it over `area` (screen
    // coordinates). Cheap when nothing changed; call it on a timer and on
    // every move or resize of the editor.
    void Update(const RECT& area);

    // The window has been taken over.
    bool Embedded() const { return window_ != nullptr; }
    // The game gave up a window it cannot share: it runs fullscreen.
    bool Fullscreen() const { return fullscreen_; }

    // Shows the game over the viewport, or hides it to show the map.
    void SetShown(bool shown);
    bool Shown() const { return shown_; }

    // Gives the window its frame and independence back, and forgets the game.
    // Must run before the editor's window is destroyed: owned windows go with
    // their owner.
    void Release();

private:
    bool Adopt(HWND window);

    HANDLE process_ = nullptr;
    DWORD  processId_ = 0;
    HWND   owner_ = nullptr;
    HWND   window_ = nullptr;
    LONG_PTR style_ = 0;        // the window's own, restored on release
    RECT   placed_{};
    bool   shown_ = true;
    bool   fullscreen_ = false;
};
