#pragma once

// Port of NFSU2Unity Core/Physics/Input/KeyboardAxis.cs.

#include "drive/numerics/Speed2Image.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bb {

// Joystick state as the input layer sees it: dword_86EAF0 (current) / dword_86B7E0 (previous), 16 bytes.
// W0/W1 = 64 button bits, ACTIVE LOW (a pressed button clears its bit); W2 = analog bytes +8..+0xB
// (+8/+9 triggers, reset to 0 each frame; +0xA/+0xB stick axes, reset to 0x80); W3 = +0xC (never reset).
// The mapping functions address it as a bit array: bit i = (W[i >> 5] >> (i & 31)) & 1, so bit
// indices >= 64 read the analog bytes.
// No floating point anywhere in this layer: the x87 precision rules do not apply.
struct JoyState {
    uint32_t W0 = 0, W1 = 0, W2 = 0, W3 = 0;

    uint32_t Word(int i) const;

    // (W[idx >> 5] >> (idx & 31)) for a byte index (0..255; the original reads past +0x10
    // for idx >= 128, which no table entry uses).
    uint32_t Shifted(int idx) const;

    // The state sub_5CFD60 starts each frame from: all buttons released, triggers 0, axes 0x80.
    static JoyState Released(uint32_t w3 = 0);

    // sub_5CFD60 (port 0 part) + sub_5CF220 for the keyboard device (DI8DEVTYPE_KEYBOARD = 0x13):
    // previous = current; current buttons = all released, triggers = 0, axes = 0x80 (+0xC kept); then each
    // device ANDs its button mask (device+8/+0xC). The keyboard contributes no analog bytes (0x5CF26B skips
    // the trigger/axis merge for type 0x13).
    static void BeginFrame(JoyState& cur, JoyState& prev, uint64_t keyboardMask);
};

// The DirectInput keyboard device as seen by sub_5C1840 (type 0x13 branch): a 64-bit button mask
// (device+8/+0xC) updated from the buffered key events (DIDEVICEOBJECTDATA, uAppData = bit index from the
// keyboard action format 0x7FCAC0). Key down clears the bit, key up sets it (active low). Several events in
// one poll collapse into the final state (a press + release inside one frame leaves no trace).
class KeyboardDevice {
public:
    // DIACTION uAppData of the keyboard action format at 0x7FCAC0 (two DIK keys per action).
    static constexpr int Throttle = 1;   // DIK_UP 0xC8, DIK_W 0x11
    static constexpr int Brake = 2;      // DIK_DOWN 0xD0, DIK_S 0x1F
    static constexpr int Left = 3;       // DIK_LEFT 0xCB, DIK_A 0x1E
    static constexpr int Right = 4;      // DIK_RIGHT 0xCD, DIK_D 0x20
    static constexpr int Handbrake = 5;  // DIK_SPACE 0x39, DIK_END 0xCF
    static constexpr int Nitrous = 6;    // DIK_LMENU 0x38, DIK_RMENU 0xB8
    static constexpr int ShiftUp = 7;    // DIK_LSHIFT 0x2A, DIK_APOSTROPHE 0x27 (layout dependent)
    static constexpr int ShiftDown = 8;  // DIK_LCONTROL 0x1D, DIK_PERIOD 0x34
    static constexpr int LookBack = 9, Camera = 10, Hud = 11, Pause = 12, Respot = 14, Engage = 15;

    // device+8 (low) / device+0xC (high). All ones = nothing pressed.
    uint64_t Mask = ~0ull;

    // One buffered key event: `dwData != 0` (down) clears bit uAppData, up sets it. __allshl
    // semantics: a shift of 64 or more gives 0 (the mask is then unchanged).
    void KeyEvent(int appData, bool down);

    bool IsDown(int appData) const;
};

// One action descriptor of the table at 0x7FDD10 (0x20 bytes per entry, count dword_7FE770 = 83), chosen per
// node by sub_572120: +0 eight device-type bytes (0xFF = any device, 0 = unused slot), +8 JoyEvent,
// +0xC mapping function, +0x10..+0x17 its parameters, +0x18 dword, +0x1C name pointer.
class JoyMapEntry {
public:
    uint32_t Va = 0;                 // address in SPEED2.EXE (0 for synthetic entries)
    uint8_t Raw[0x20] = {};

    uint8_t Device(int i) const { return Raw[i]; }
    int Event() const;
    uint32_t Function() const;
    // +0x10: xor mask of bit A (1 = active low -> "pressed").
    uint8_t InvA() const { return Raw[0x10]; }
    // +0x11: xor mask of bit B; for 0x571B10 a non-zero value also turns B into a chord modifier.
    uint8_t InvB() const { return Raw[0x11]; }
    // +0x12: bit index A (0 = none for 0x571B10).
    uint8_t BitA() const { return Raw[0x12]; }
    // +0x13: bit index B (0 = none).
    uint8_t BitB() const { return Raw[0x13]; }
    // +0x14: value sent while active (0 = the test result, 1).
    uint8_t Value() const { return Raw[0x14]; }
    // +0x16: still processed while a replay is playing (0x57B1CA).
    uint8_t ReplayOk() const { return Raw[0x16]; }

    static std::unique_ptr<JoyMapEntry> FromBytes(const uint8_t* raw, uint32_t va = 0);
};

// The action descriptor table 0x7FDD10, read from the exe (.data; never patched at runtime).
class JoyMapTable {
public:
    static constexpr uint32_t TableVa = 0x7FDD10;
    static constexpr uint32_t CountVa = 0x7FE770;     // dword_7FE770
    std::vector<std::unique_ptr<JoyMapEntry>> Entries;

    explicit JoyMapTable(const Speed2Image& exe);
    JoyMapTable(const JoyMapTable&) = delete;
    JoyMapTable& operator=(const JoyMapTable&) = delete;

    JoyMapEntry* AtVa(uint32_t va) const;

    // The search loop of sub_572120(JoyEvent, deviceType): first entry with this event whose device bytes
    // contain 0xFF or deviceType (a 0 byte is skipped). deviceType = 0 (the result of sub_5720D0 when no
    // device type is known) matches only events 1 and 2. sub_5720D0 itself (device-type enumeration through
    // sub_571FD0) is not ported: the caller passes its result.
    JoyMapEntry* Find(int joyEvent, int deviceType) const;
};

// A node of a port's handler list (AddJoyHandler 0x579410, appended at the tail; 0x24 bytes):
// +8 JoyEvent, +0xC descriptor, +0x10 callback, +0x14 callback argument, +0x18 last value sent,
// +0x1C "send the current value" flag (set to 1 on registration and on a remap), +0x20 repeat timer
// (0x571D50 only, not ported).
class JoyNode {
public:
    int Event = 0;
    JoyMapEntry* Desc = nullptr;
    int Callback = 0;            // node+0x10 (opaque here: the dispatcher maps events to handlers)
    int Param = 0;               // node+0x14
    int Last = 0;                // node+0x18
    int Force = 1;               // node+0x1C
};

// One queued event (12 bytes): word = event | value << 8, port byte, node event byte,
// callback, argument. GenerateJoyEvents dispatches it as callback(port, event, value, argument).
struct JoyEventRecord {
    uint16_t Word = 0;
    uint8_t Port = 0;
    uint8_t NodeEvent = 0;
    int Callback = 0;
    int Param = 0;

    int Event() const { return Word & 0xFF; }
    uint8_t Value() const { return static_cast<uint8_t>(Word >> 8); }
    std::string ToString() const;
};

// The digital mapping functions of the descriptor table (cdecl fn(node, cur, prev, desc, joy) -> ax).
namespace JoyMapFunctions {
constexpr uint32_t FnTest = 0x571B10, FnDeviceCount = 0x571BD0, FnEdge = 0x571C10, FnLevel = 0x571C70,
                   FnAxis2 = 0x571CC0, FnRepeat = 0x571D50, FnAnalog = 0x5795C0;

// sub_571B10(desc, cur, prev): bit / chord test, 0 or 1.
// A = BitA ? ((cur bit A) ^ InvA) & 1 : 1. Without BitB the result is A. With BitB and InvB == 0:
// A & ((cur bit B) & 1). With BitB and InvB != 0 (chord): B pressed now and unchanged since the
// previous frame, and A pressed now and not before (the previous-A term reads bit BitA even when BitA = 0,
// as the original does).
uint32_t Test(const JoyMapEntry& d, const JoyState& cur, const JoyState& prev);

// 0x571BD0: sends the port's device count (joy+0x1B8, low byte) when it changes.
uint16_t DeviceCount(JoyNode& n, int deviceCount);

// 0x571C10: button, press edge only (value = Value or 1). A forced send only re-arms (no event).
uint16_t Edge(JoyNode& n, const JoyState& cur, const JoyState& prev, const JoyMapEntry& d);

// 0x571C70: button, level: sends Value (or 1) on press and 0 on release.
uint16_t Level(JoyNode& n, const JoyState& cur, const JoyState& prev, const JoyMapEntry& d);

// 0x571CC0: two buttons as one 8-bit axis (keyboard / d-pad steering, JoyEvent 0x8F with bits 3 = left,
// 4 = right): only A -> 0x00, only B -> 0xFF, none or both -> 0x80. An instantaneous step: there is no
// ramp in the input layer; the gradual wheel motion comes from PlayerSteering::DoSteering.
uint16_t Axis2(JoyNode& n, const JoyState& cur, const JoyMapEntry& d);

// Calls the descriptor's function. The analog functions (0x5795C0 axes/triggers through the
// dead-zone Graph, 0x571D50 auto-repeat) are not ported.
uint16_t Map(JoyNode& n, const JoyState& cur, const JoyState& prev, const JoyMapEntry& d, int deviceCount);

bool IsPorted(const JoyMapEntry* d);
} // namespace JoyMapFunctions

// A joystick port (unk_8655A0 + port * 0x1C8): the handler list and sub_57B0B0, the per-port step of
// GenerateJoyEvents 0x5809C0.
class JoyPort {
public:
    static constexpr int QueueCapacity = 0x100;
    static constexpr int DebounceFrames = 0x20;

    std::vector<JoyNode> Nodes;     // [joy] list, registration order
    int Port = 0;                   // +0x1B4
    int DeviceCount = 0;            // +0x1B8
    int PendingCount = 0;           // +0x1BC
    int Debounce = 0;               // +0x1C0
    // byte_865658[port * 0x1C8 + event]: last value dispatched per event.
    uint8_t LastEventValue[256] = {};
    // sub_572120 for a remap (re-resolves every node's descriptor after a device-count change).
    std::function<JoyMapEntry*(int)> Resolve;

    // sub_57B0B0(deviceCount, cur, prev, queue): a changed device count must stay the same for 32 calls
    // before the nodes are remapped (dead zones sub_572210, descriptors sub_572120, force flag 1, last 0)
    // and events flow again; meanwhile nothing is sent. Then every node with a descriptor runs its mapping
    // function in list order and non-zero results are queued (at most 0x100; the returned count keeps
    // counting past that). While a replay plays (and the race state is not 7) only descriptors with +0x16 set
    // are processed.
    int Poll(int deviceCount, const JoyState& cur, const JoyState& prev, std::vector<JoyEventRecord>& queue, bool replayGate = false);

    // GenerateJoyEvents 0x580D20 loop for one record: records the value per event and tells the
    // caller whether to call the handler (event byte 0 is skipped).
    bool Dispatch(const JoyEventRecord& r);
};

// Keyboard -> joystick events for the player car, as the PC game does it (docs/re/input-keyboard.md):
// key state -> active-low mask (sub_5C1840) -> per-frame joystick state (sub_5CFD60) -> per-action mapping
// (sub_57B0B0 with the descriptors of 0x7FDD10) -> JoyEvent records for RealDriver / Player handlers.
// Steering is a step (0x00 / 0x80 / 0xFF on JoyEvent 0x8F); pedals are 0 / 0xFF (0x91, 0x94, 0x99);
// events are only sent when the value changes.
class KeyboardAxis {
public:
    // RealDriver::AddJoyHandlers 0x41E060 order, then the Player racing event for nitrous (0xA3).
    static constexpr int EventCount = 19;
    static const int Events[EventCount];

    // Device type passed to sub_572120 (result of sub_5720D0; any non-zero value selects the
    // 0xFF = "any device" entries, which are the only ones the keyboard actions have).
    static constexpr int KeyboardDeviceType = 0x13;

    JoyMapTable Table;
    JoyPort Port;
    KeyboardDevice Keyboard;
    JoyState Current = JoyState::Released();
    JoyState Previous = JoyState::Released();
    // dword_870764 (number of devices); constant for a keyboard-only setup.
    int DeviceCount = 1;

    explicit KeyboardAxis(const Speed2Image& exe, int port = 0);
    KeyboardAxis(const KeyboardAxis&) = delete;
    KeyboardAxis& operator=(const KeyboardAxis&) = delete;

    // Sets the held keys (state at the time DirectInput is polled, once per rendered frame).
    void SetKeys(bool left, bool right, bool up, bool down, bool handbrake, bool nitrous, bool shiftUp, bool shiftDown);

    // One GenerateJoyEvents: builds this frame's state and appends the events to send.
    int Poll(std::vector<JoyEventRecord>& queue);

private:
    // Analog descriptors (gamepad axes / triggers, 0x5795C0) are left out: with only a keyboard
    // their state is constant (triggers 0, axes 0x80), so after the one forced send at registration (which
    // writes the values RealDriver already starts with) they never emit.
    static JoyMapEntry* Usable(JoyMapEntry* e);
};

} // namespace bb
