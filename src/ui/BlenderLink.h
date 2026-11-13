#pragma once

#include <windows.h>

#include <string>

// Quick edit: the selection opened straight in Blender, and every save there
// brought back into the editor.
//
// The export and import are the ones the File menu already has; what this adds
// is the part a person otherwise does by hand. The selection is written to a
// folder of its own, Blender is started on it with a script that sets the
// import and export options the round trip depends on, and the editor watches
// for the small marker file that script touches after each save.
//
// The script is carried inside the editor and written out on every start, so
// nothing has to be installed in Blender and the two cannot drift apart.
class BlenderLink {
public:
    ~BlenderLink() { Stop(); }

    // Where Blender is: the path chosen before, then the .blend file
    // association, then the newest install under Program Files. Asks when none
    // of those finds one, and remembers the answer. Empty when the user cancels.
    static std::wstring FindBlender(HWND owner, const wchar_t* settingsKey);
    // Asks which blender.exe to use, starting from the one in use, and keeps
    // the answer. Empty when cancelled.
    static std::wstring ChooseBlender(HWND owner, const wchar_t* settingsKey);

    // A fresh folder for one quick edit, under %LOCALAPPDATA%.
    static std::wstring NewSessionFolder(const std::wstring& label);

    // Starts Blender on `objPath` and begins watching it. A session already
    // running is let go first; its Blender stays open but is no longer watched.
    bool Start(const std::wstring& blender, const std::wstring& objPath, std::wstring& error);
    void Stop();

    // True once for each save Blender has finished since the last call.
    bool PollSaved();

    // Blender is still open on the watched file.
    bool Running() const;
    bool Active() const { return !objPath_.empty(); }
    const std::wstring& ObjPath() const { return objPath_; }

private:
    std::wstring objPath_;
    HANDLE       process_ = nullptr;
    FILETIME     lastSent_{};
};
