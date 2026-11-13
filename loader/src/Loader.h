#pragma once

// FRS World Loader -- what the parts of the plugin share.
//
// One plugin carries everything the FRS World Editor puts into the game:
//
//   FileEdits.cpp    meshes, sections and whole files from <game>\models
//   Main.cpp         the log, the settings, the game-thread frame, the
//                    player's pose, the launch pose, DllMain
//   Recorder.cpp     a memory recorder for reverse engineering (off by default)
//
// A build can link parts of its own in (see CMakeLists.txt); this
// repository's build has none and compiles NoExtensions.cpp in their place.
//
// Written against SPEED2.EXE v1.2 (image base 0x400000). Every patch checks
// the bytes it replaces first and leaves the game alone when they differ.

#include <windows.h>

#include <cstdint>
#include <string>

namespace frs {

extern HMODULE g_module;

// ---- the log and the settings, both beside the plugin

void Log(const char* format, ...);
std::wstring BesidePlugin(const wchar_t* extension);   // ".log", ".ini"
int  Setting(const wchar_t* section, const wchar_t* key, int fallback);

// The folder that holds SPEED2.EXE.
std::wstring GameFolder();

// Set while the plugin does its own file work, so that opening the log cannot
// come back through the file hooks the log reports on.
extern __declspec(thread) bool t_inside;

struct Inside {
    Inside()  { t_inside = true; }
    ~Inside() { t_inside = false; }
};

// ---- reading and patching the game

template <typename T> T Peek(uintptr_t address) {
    return ::IsBadReadPtr((const void*)address, sizeof(T)) ? T{} : *(const T*)address;
}

void Put32(unsigned char* at, unsigned value);
void PutJump(unsigned char* at, const void* to);                   // into fresh code
void Patch(unsigned char* at, const void* bytes, size_t size);     // into the game's code
void PatchJump(unsigned char* at, size_t size, const void* to);    // jmp + nops
void RedirectCall(unsigned char* site, const void* to);            // an E8's target
unsigned char* NewCode(size_t size);

// ---- the game's state

constexpr uintptr_t kGameFlowState = 0x8654A4;   // 6 = in gameplay
inline bool InGameplay() { return Peek<int>(kGameFlowState) == 6; }

// The player's car: its rigid body (position at +0x20, the three axes --
// forward, side, up, stride 0x10 -- at +0x30), or 0 while there is none.
uintptr_t PlayerBody();

struct Pose { float position[3], forward[3], side[3]; };
bool PlayerPose(Pose& out);

// ---- the parts

void StartFileEdits();      // from DllMain: the file replacements must be early
void LogFileEditTotals();   // on the way out
void StartRecorder();
void StartRouteProbe();     // [RouteProbe] in the .ini; off by default
void RouteProbeFrame();

// Parts linked in from elsewhere: installed from DllMain on v1.2 only, then
// called on the game thread every frame.
void InstallExtensions();
void ExtensionsFrame();

} // namespace frs
