// FRS World Loader
//
// The FRS World Editor's end inside Need for Speed Underground 2: one ASI
// plugin that puts into the game what the editor writes to <game>\models --
// edited meshes and sections, replaced files.
// Loader.h lists the parts.
//
// This file holds what they share: the log and the settings beside the plugin,
// the patching helpers, the game-thread frame the parts run in, the
// player's pose, and the launch pose the editor's Launch Game leaves behind.

#include "Loader.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace frs {

HMODULE g_module = nullptr;
__declspec(thread) bool t_inside = false;

// ---------------------------------------------------------------------------
// The log and the settings
// ---------------------------------------------------------------------------

// Beside the plugin, by its full path.
//
// A relative path follows the working directory, and the game changes that
// after it starts: the opening lines landed next to the plugin and every line
// after them in the game's root, which made a working run look silent.
std::wstring BesidePlugin(const wchar_t* extension) {
    wchar_t path[MAX_PATH] = {};
    if (!::GetModuleFileNameW(g_module, path, MAX_PATH)) return std::wstring(L".\\FRSWorldLoader") + extension;
    if (wchar_t* dot = ::wcsrchr(path, L'.')) *dot = 0;
    return std::wstring(path) + extension;
}

const wchar_t* LogFile() {
    static const std::wstring path = BesidePlugin(L".log");
    return path.c_str();
}

void Log(const char* format, ...) {
    const Inside inside;

    FILE* file = nullptr;
    if (::_wfopen_s(&file, LogFile(), L"a") != 0 || !file) return;

    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);

    std::fputc('\n', file);
    std::fclose(file);
}

int Setting(const wchar_t* section, const wchar_t* key, int fallback) {
    static const std::wstring ini = BesidePlugin(L".ini");
    return (int)::GetPrivateProfileIntW(section, key, fallback, ini.c_str());
}

std::wstring GameFolder() {
    wchar_t path[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);

    std::wstring folder = path;
    const size_t slash = folder.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : folder.substr(0, slash);
}

// ---------------------------------------------------------------------------
// Patching
// ---------------------------------------------------------------------------

void Put32(unsigned char* at, unsigned value) { std::memcpy(at, &value, 4); }

void PutJump(unsigned char* at, const void* to) {
    at[0] = 0xE9;
    const LONG rel = (LONG)((const unsigned char*)to - (at + 5));
    std::memcpy(at + 1, &rel, 4);
}

void Patch(unsigned char* at, const void* bytes, size_t size) {
    DWORD protection = 0;
    if (!::VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &protection)) return;
    std::memcpy(at, bytes, size);
    ::VirtualProtect(at, size, protection, &protection);
    ::FlushInstructionCache(::GetCurrentProcess(), at, size);
}

// Jumps from `at` to `to`, filling the rest of `size` bytes with nops.
void PatchJump(unsigned char* at, size_t size, const void* to) {
    unsigned char bytes[16];
    std::memset(bytes, 0x90, sizeof(bytes));
    bytes[0] = 0xE9;
    const LONG rel = (LONG)((const unsigned char*)to - (at + 5));
    std::memcpy(bytes + 1, &rel, 4);
    Patch(at, bytes, size);
}

void RedirectCall(unsigned char* site, const void* to) {
    const LONG rel = (LONG)((const unsigned char*)to - (site + 5));
    Patch(site + 1, &rel, 4);
}

unsigned char* NewCode(size_t size) {
    return (unsigned char*)::VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE,
                                          PAGE_EXECUTE_READWRITE);
}

namespace {

// Where a rel32 jump or call at `site` goes.
uintptr_t Target(const unsigned char* site) {
    LONG rel = 0;
    std::memcpy(&rel, site + 1, 4);
    return (uintptr_t)(site + 5 + rel);
}

// ---------------------------------------------------------------------------
// The player's pose
//
// The position and the axes live in the vehicle's physics, and the physics
// hangs off no global: the way to it is to wait for the game to call the
// vehicle update 0x5A5540 (thiscall, the physics object in ecx). It runs for
// every vehicle in the world, so only the one whose +0x58 points at the
// player's mirror -- 0x40 into the object at [[0x8900AC] + 4] -- is kept.
// Its rigid body is [physics + 0x20]: the position at +0x20, the three axes
// (forward, side, up; stride 0x10) at +0x30.
//
// When another plugin already put a jump at 0x5A5540 this one goes in front
// of it and continues there, so both run; otherwise the six bytes it opens
// with (push ebp / mov ebp, esp / sub esp, 0x24) move to a trampoline.
// ---------------------------------------------------------------------------

constexpr uintptr_t kVehicleUpdate = 0x5A5540;
constexpr uintptr_t kPlayerObject  = 0x8900AC;   // +4 its mirror
constexpr size_t    kPhysicsBody   = 0x20;
constexpr size_t    kPhysicsMirror = 0x58;       // == mirror + 0x40 on the player's
constexpr size_t    kMirrorSkew    = 0x40;
constexpr size_t    kBodyPosition  = 0x20;
constexpr size_t    kBodyAxes      = 0x30;

volatile uintptr_t g_physics = 0;
uintptr_t g_vehicleUpdateNext = 0;

uintptr_t PlayerMirror() {
    const uintptr_t player = Peek<uintptr_t>(kPlayerObject);
    const uintptr_t mirror = player ? Peek<uintptr_t>(player + 4) : 0;
    return mirror && !::IsBadReadPtr((void*)mirror, 0x600) ? mirror : 0;
}

extern "C" void __cdecl CapturePhysics(uintptr_t self) {
    if (!self || ::IsBadReadPtr((void*)self, kPhysicsMirror + 4)) return;
    const uintptr_t mirror = PlayerMirror();
    if (mirror && *(uintptr_t*)(self + kPhysicsMirror) == mirror + kMirrorSkew) g_physics = self;
}

__declspec(naked) void VehicleUpdateHook() {
    __asm {
        pushad
        pushfd
        push ecx
        call CapturePhysics
        add  esp, 4
        popfd
        popad
        jmp  dword ptr [g_vehicleUpdateNext]
    }
}

bool HookVehicleUpdate() {
    auto* site = (unsigned char*)kVehicleUpdate;
    if (site[0] == 0xE9) {
        g_vehicleUpdateNext = Target(site);
        RedirectCall(site, (const void*)&VehicleUpdateHook);
        return true;
    }
    const unsigned char prologue[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24 };
    if (std::memcmp(site, prologue, sizeof(prologue)) != 0) return false;
    unsigned char* trampoline = NewCode(16);
    if (!trampoline) return false;
    std::memcpy(trampoline, prologue, sizeof(prologue));
    PutJump(trampoline + sizeof(prologue), site + sizeof(prologue));
    g_vehicleUpdateNext = (uintptr_t)trampoline;
    PatchJump(site, sizeof(prologue), (const void*)&VehicleUpdateHook);
    return true;
}

} // namespace

uintptr_t PlayerBody() {
    const uintptr_t physics = g_physics;
    if (!physics || ::IsBadReadPtr((void*)physics, kPhysicsMirror + 4)) return 0;
    // Checked again on every read: the pointer goes stale when the player
    // changes car, and reading a freed object is worse than having no pose.
    const uintptr_t mirror = PlayerMirror();
    if (!mirror || *(uintptr_t*)(physics + kPhysicsMirror) != mirror + kMirrorSkew) return 0;
    const uintptr_t body = Peek<uintptr_t>(physics + kPhysicsBody);
    return body && !::IsBadWritePtr((void*)body, kBodyAxes + 0x30) ? body : 0;
}

bool PlayerPose(Pose& out) {
    const uintptr_t body = PlayerBody();
    if (!body) return false;
    const float* position = (const float*)(body + kBodyPosition);
    const float* axes = (const float*)(body + kBodyAxes);
    for (int i = 0; i < 3; ++i) {
        out.position[i] = position[i];
        out.forward[i] = axes[i];
        out.side[i] = axes[4 + i];
    }
    return true;
}

namespace {

// ---------------------------------------------------------------------------
// Where the editor's camera was: the car starts there
//
// Launch Game in the editor writes <game>\models\launch-pose.txt,
//
//   pose <x> <y> <z> <heading degrees>
//
// with the ground under its camera, and the car is put there once, a second
// after the session first has a player car in gameplay. The file is read and
// deleted at start, so it only ever applies to the launch that wrote it.
//
// The axes are the car's own, turned about the vertical to the heading, so the
// matrix keeps its handedness whatever the side axis's convention is.
// ---------------------------------------------------------------------------

constexpr float kDropHeight = 1.0f;   // over the ground, to settle onto it

struct LaunchPose {
    bool  pending = false;
    float x = 0, y = 0, z = 0, heading = 0;
    DWORD readySince = 0;
} g_launch;

void ReadLaunchPose() {
    const Inside inside;
    const std::wstring path = GameFolder() + L"\\models\\launch-pose.txt";
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"rt") != 0 || !file) return;
    const int read = std::fscanf(file, "pose %f %f %f %f", &g_launch.x, &g_launch.y,
                                 &g_launch.z, &g_launch.heading);
    std::fclose(file);
    ::DeleteFileW(path.c_str());
    g_launch.pending = read == 4;
    if (g_launch.pending)
        Log("launch pose: the car goes to (%.1f, %.1f, %.1f) heading %.1f once in gameplay",
            g_launch.x, g_launch.y, g_launch.z, g_launch.heading);
    else
        Log("launch pose: %S is not a pose", path.c_str());
}

void LaunchPoseFrame() {
    if (!g_launch.pending) return;
    const uintptr_t body = InGameplay() ? PlayerBody() : 0;
    if (!body) { g_launch.readySince = 0; return; }
    if (!g_launch.readySince) { g_launch.readySince = ::GetTickCount() | 1; return; }
    if (::GetTickCount() - g_launch.readySince < 1000) return;
    g_launch.pending = false;

    float* axes = (float*)(body + kBodyAxes);
    const float current = std::atan2(axes[1], axes[0]);
    const float turn = g_launch.heading * 3.14159265f / 180.0f - current;
    const float c = std::cos(turn), s = std::sin(turn);
    for (int row = 0; row < 3; ++row) {
        float* a = axes + row * 4;
        const float ax = a[0], ay = a[1];
        a[0] = ax * c - ay * s;
        a[1] = ax * s + ay * c;
    }
    float* position = (float*)(body + kBodyPosition);
    position[0] = g_launch.x;
    position[1] = g_launch.y;
    position[2] = g_launch.z + kDropHeight;
    Log("launch pose: car placed");
}

// ---------------------------------------------------------------------------
// The frame
//
// Calling into the game and changing its structures is only safe on the
// game's own thread, between the things it does in a frame. The main loop makes two calls in a row to an empty stub, at 0x581470
// and 0x581475; other plugins use them too, so the call at 0x581475 is pointed
// here and this calls on to whatever it called before: all of them run, in the
// order they were installed.
// ---------------------------------------------------------------------------

constexpr uintptr_t kMainLoopCall = 0x581475;

using FrameFn = void(__cdecl*)();
FrameFn g_previousFrame = nullptr;
bool g_firstFrame = true;

void __cdecl Frame() {
    if (g_previousFrame) g_previousFrame();
    if (g_firstFrame) {
        g_firstFrame = false;
        ReadLaunchPose();
    }
    PlacedShopsFrame();
    ExtensionsFrame();
    LaunchPoseFrame();
    RouteProbeFrame();
}

bool HookMainLoop() {
    auto* site = (unsigned char*)kMainLoopCall;
    if (site[0] != 0xE8) return false;
    g_previousFrame = (FrameFn)Target(site);
    RedirectCall(site, (const void*)&Frame);
    return true;
}

// The code at the heart of the plugin's patches must be the v1.2 executable's.
bool RightExecutable() {
    const unsigned char update[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24 };
    const auto* at = (const unsigned char*)kVehicleUpdate;
    return !::IsBadReadPtr(at, 6) && (at[0] == 0xE9 || std::memcmp(at, update, 6) == 0) &&
           !::IsBadReadPtr((const void*)kMainLoopCall, 5);
}

} // namespace

} // namespace frs

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    using namespace frs;

    if (reason == DLL_PROCESS_DETACH) {
        LogFileEditTotals();
        return TRUE;
    }
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    g_module = module;
    ::DisableThreadLibraryCalls(module);
    ::DeleteFileW(LogFile());
    Log("--- FRS World Loader ---");

    // The file edits do not depend on the executable's code, only on its
    // files, so they go in on any build.
    StartFileEdits();

    if (!RightExecutable()) {
        Log("this SPEED2.EXE is not v1.2: the launch pose stays off");
        return TRUE;
    }
    Log(HookVehicleUpdate() ? "player pose: vehicle update hooked at 0x5A5540"
                            : "player pose: 0x5A5540 is not the expected code; no launch pose");
    InstallPlacedShopHooks();
    InstallExtensions();
    Log(HookMainLoop() ? "frame: main loop hooked at 0x581475"
                       : "frame: 0x581475 is not a call; the launch pose stays off");
    StartRecorder();
    StartRouteProbe();
    return TRUE;
}
