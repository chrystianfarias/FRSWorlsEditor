#include "drive/physics/input/KeyboardAxis.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace bb {

// ---------------------------------------------------------------------------------------- JoyState

uint32_t JoyState::Word(int i) const {
    switch (i & 3) {
    case 0: return W0;
    case 1: return W1;
    case 2: return W2;
    default: return W3;
    }
}

uint32_t JoyState::Shifted(int idx) const {
    if (idx >= 128) throw std::runtime_error("JoyState: bit index past the 16-byte state");
    return Word(idx >> 5) >> (idx & 31);
}

JoyState JoyState::Released(uint32_t w3) {
    JoyState s;
    s.W0 = 0xFFFFFFFFu; s.W1 = 0xFFFFFFFFu; s.W2 = 0x80800000u; s.W3 = w3;
    return s;
}

void JoyState::BeginFrame(JoyState& cur, JoyState& prev, uint64_t keyboardMask) {
    prev = cur;
    uint32_t w3 = cur.W3;
    cur = Released(w3);
    cur.W0 &= static_cast<uint32_t>(keyboardMask);
    cur.W1 &= static_cast<uint32_t>(keyboardMask >> 32);
}

// ---------------------------------------------------------------------------------------- KeyboardDevice

void KeyboardDevice::KeyEvent(int appData, bool down) {
    uint64_t bit = static_cast<uint32_t>(appData) < 64u ? 1ull << appData : 0ull;
    if (down) Mask &= ~bit;
    else Mask |= bit;
}

bool KeyboardDevice::IsDown(int appData) const {
    return static_cast<uint32_t>(appData) < 64u && ((Mask >> appData) & 1ull) == 0;
}

// ---------------------------------------------------------------------------------------- JoyMapEntry / Table

int JoyMapEntry::Event() const { int v; std::memcpy(&v, Raw + 8, 4); return v; }
uint32_t JoyMapEntry::Function() const { uint32_t v; std::memcpy(&v, Raw + 0xC, 4); return v; }

std::unique_ptr<JoyMapEntry> JoyMapEntry::FromBytes(const uint8_t* raw, uint32_t va) {
    auto e = std::make_unique<JoyMapEntry>();
    e->Va = va;
    std::memcpy(e->Raw, raw, 0x20);
    return e;
}

JoyMapTable::JoyMapTable(const Speed2Image& exe) {
    int n = static_cast<int>(exe.U32(CountVa));
    for (int i = 0; i < n; i++) {
        uint32_t va = TableVa + static_cast<uint32_t>(0x20 * i);
        std::vector<uint8_t> b = exe.Bytes(va, 0x20);
        Entries.push_back(JoyMapEntry::FromBytes(b.data(), va));
    }
}

JoyMapEntry* JoyMapTable::AtVa(uint32_t va) const {
    for (const auto& e : Entries) if (e->Va == va) return e.get();
    return nullptr;
}

JoyMapEntry* JoyMapTable::Find(int joyEvent, int deviceType) const {
    for (const auto& e : Entries) {
        if (e->Event() != joyEvent) continue;
        for (int k = 0; k < 8; k++) {
            if (deviceType == 0) {
                if (joyEvent == 1 || joyEvent == 2) return e.get();
                continue;
            }
            uint8_t b = e->Device(k);
            if (b == 0) continue;
            if (b == 0xFF) return e.get();
            if (static_cast<int8_t>(b) == deviceType) return e.get();
        }
    }
    return nullptr;
}

std::string JoyEventRecord::ToString() const {
    char text[64];
    std::snprintf(text, sizeof text, "%04X:%02X:%02X:%08X:%08X", Word, Port, NodeEvent,
                  static_cast<unsigned>(Callback), static_cast<unsigned>(Param));
    return text;
}

// ---------------------------------------------------------------------------------------- JoyMapFunctions

namespace JoyMapFunctions {

uint32_t Test(const JoyMapEntry& d, const JoyState& cur, const JoyState& prev) {
    int a = d.BitA();
    uint32_t esi = 1, eax = 1;
    if (a != 0) esi = ((cur.Shifted(a)) ^ d.InvA()) & eax;
    int b = d.BitB();
    if (b == 0) return eax & esi;
    uint32_t inv = d.InvB();
    eax = ((cur.Shifted(b)) ^ inv) & 1u;
    if (inv != 0) {
        uint32_t ebx = ((prev.Shifted(a)) ^ d.InvA()) & 1u;
        ebx ^= esi;
        esi &= ebx;
        uint32_t edx = (((prev.Shifted(b)) ^ inv) & 1u) ^ eax;
        edx = edx == 0 ? 1u : 0u;                                       // neg ; sbb ; inc
        eax &= edx;
    }
    return eax & esi;
}

static uint16_t Emit(JoyNode& n, int v) {
    n.Last = v;
    n.Force = 0;
    return static_cast<uint16_t>((v << 8) + n.Event);
}

uint16_t DeviceCount(JoyNode& n, int deviceCount) {
    int v = deviceCount & 0xFF;
    if (v == n.Last && n.Force == 0) return 0;
    return Emit(n, v);
}

uint16_t Edge(JoyNode& n, const JoyState& cur, const JoyState& prev, const JoyMapEntry& d) {
    int v = static_cast<int>(Test(d, cur, prev));
    if (v != 0 && d.Value() != 0) v = d.Value();
    if (n.Force != 0) { n.Force = 0; n.Last = v; }
    if (v == n.Last) return 0;
    n.Last = v;
    if (v == 0) return 0;
    return static_cast<uint16_t>((v << 8) + n.Event);
}

uint16_t Level(JoyNode& n, const JoyState& cur, const JoyState& prev, const JoyMapEntry& d) {
    int v = static_cast<int>(Test(d, cur, prev));
    if (v != 0 && d.Value() != 0) v = d.Value();
    if (v == n.Last && n.Force == 0) return 0;
    return Emit(n, v);
}

uint16_t Axis2(JoyNode& n, const JoyState& cur, const JoyMapEntry& d) {
    uint32_t a = ((cur.Shifted(d.BitA())) ^ d.InvA()) & 1u;
    uint32_t b = ((cur.Shifted(d.BitB())) ^ d.InvB()) & 1u;
    int v = 0x80;
    if (a != 0) { if (b == 0) v = 0; }
    else if (b != 0) v = 0xFF;
    if (n.Last == v && n.Force == 0) return 0;
    return Emit(n, v);
}

uint16_t Map(JoyNode& n, const JoyState& cur, const JoyState& prev, const JoyMapEntry& d, int deviceCount) {
    switch (d.Function()) {
    case FnDeviceCount: return DeviceCount(n, deviceCount);
    case FnEdge: return Edge(n, cur, prev, d);
    case FnLevel: return Level(n, cur, prev, d);
    case FnAxis2: return Axis2(n, cur, d);
    default: {
        char text[96];
        std::snprintf(text, sizeof text, "joystick mapping function 0x%X is not ported", d.Function());
        throw std::runtime_error(text);
    }
    }
}

bool IsPorted(const JoyMapEntry* d) {
    return d != nullptr && (d->Function() == FnDeviceCount || d->Function() == FnEdge || d->Function() == FnLevel || d->Function() == FnAxis2);
}

} // namespace JoyMapFunctions

// ---------------------------------------------------------------------------------------- JoyPort

int JoyPort::Poll(int deviceCount, const JoyState& cur, const JoyState& prev, std::vector<JoyEventRecord>& queue, bool replayGate) {
    if (deviceCount != DeviceCount) {
        if (deviceCount != PendingCount) {
            Debounce = 0;
            PendingCount = deviceCount;
            return 0;
        }
        Debounce++;
        if (Debounce < DebounceFrames) return 0;
        DeviceCount = deviceCount;
        // sub_572210 x4: analog dead zones (not used by the digital functions)
        for (auto& n : Nodes) {
            if (!Resolve) throw std::runtime_error("JoyPort.Resolve is needed for a remap");
            n.Desc = Resolve(n.Event);
            n.Force = 1;
            n.Last = 0;
        }
    }
    int count = 0;
    for (auto& n : Nodes) {
        JoyMapEntry* d = n.Desc;
        if (d == nullptr || d->Function() == 0) continue;
        if (replayGate && d->ReplayOk() == 0) continue;
        uint16_t ax = JoyMapFunctions::Map(n, cur, prev, *d, DeviceCount);
        if (ax == 0) continue;
        if (static_cast<int>(queue.size()) < QueueCapacity) {
            JoyEventRecord r;
            r.Word = ax; r.Port = static_cast<uint8_t>(Port); r.NodeEvent = static_cast<uint8_t>(n.Event);
            r.Callback = n.Callback; r.Param = n.Param;
            queue.push_back(r);
        }
        count++;
    }
    return count;
}

bool JoyPort::Dispatch(const JoyEventRecord& r) {
    int ev = r.Event();
    if (ev == 0) return false;
    LastEventValue[ev] = r.Value();
    return r.Callback != 0;
}

// ---------------------------------------------------------------------------------------- KeyboardAxis

const int KeyboardAxis::Events[KeyboardAxis::EventCount] = {
    0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9D, 0x9C, 0x9E, 0x9F, 0xA0, 0xA3
};

KeyboardAxis::KeyboardAxis(const Speed2Image& exe, int port) : Table(exe) {
    Port.Port = port; Port.DeviceCount = DeviceCount; Port.PendingCount = DeviceCount;
    Port.Resolve = [this](int ev) { return Usable(Table.Find(ev, KeyboardDeviceType)); };
    for (int i = 0; i < EventCount; i++) {
        JoyNode n;
        n.Event = Events[i]; n.Desc = Usable(Table.Find(Events[i], KeyboardDeviceType)); n.Callback = Events[i]; n.Param = i;
        Port.Nodes.push_back(n);
    }
}

JoyMapEntry* KeyboardAxis::Usable(JoyMapEntry* e) { return JoyMapFunctions::IsPorted(e) ? e : nullptr; }

void KeyboardAxis::SetKeys(bool left, bool right, bool up, bool down, bool handbrake, bool nitrous, bool shiftUp, bool shiftDown) {
    Keyboard.KeyEvent(KeyboardDevice::Left, left);
    Keyboard.KeyEvent(KeyboardDevice::Right, right);
    Keyboard.KeyEvent(KeyboardDevice::Throttle, up);
    Keyboard.KeyEvent(KeyboardDevice::Brake, down);
    Keyboard.KeyEvent(KeyboardDevice::Handbrake, handbrake);
    Keyboard.KeyEvent(KeyboardDevice::Nitrous, nitrous);
    Keyboard.KeyEvent(KeyboardDevice::ShiftUp, shiftUp);
    Keyboard.KeyEvent(KeyboardDevice::ShiftDown, shiftDown);
}

int KeyboardAxis::Poll(std::vector<JoyEventRecord>& queue) {
    JoyState::BeginFrame(Current, Previous, Keyboard.Mask);
    return Port.Poll(DeviceCount, Current, Previous, queue);
}

} // namespace bb
