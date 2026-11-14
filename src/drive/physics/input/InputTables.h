#pragma once

// Port of NFSU2Unity Core/Physics/Input/InputTables.cs.

#include "drive/numerics/Speed2Image.h"
#include "drive/physics/wheel/Graph.h"

#include <cstdint>
#include <vector>

namespace bb {

// Table (0x5764D0 GetValue): +0 int n, +4 min, +8 max, +0xC index multiplier, +0x10 float* data.
class InputTable {
public:
    uint32_t Va = 0;
    int Count = 0;
    float Min = 0.0f, Max = 0.0f, Mult = 0.0f;
    std::vector<float> Data;

    InputTable() = default;
    InputTable(const Speed2Image& exe, uint32_t va);
    InputTable(int count, float min, float max, float mult, std::vector<float> data);

    // Table::GetValue(float) 0x5764D0 (port in BWareMath).
    float GetValue(float x) const;
};

// Time-windowed running average (vtbl 0x7A0610; base Average 0x579F20). Layout: +5 u8 capacity,
// +6 u8 count, +7 u8 write index, +8 float sum, +0xC float average, +0x10 float* values,
// +0x28 float window (s), +0x2C int oldest index, +0x30 float* sample times.
class AverageWindow {
public:
    uint8_t Capacity = 0, Count = 0, Index = 0;
    float Sum = 0.0f, Average = 0.0f, Window = 0.0f;
    int Oldest = 0;
    std::vector<float> Values, Times;

    // sub_579F80(float window, float rate): capacity = (u8)__ftol2(window*rate + 0.5).
    AverageWindow(float window, float rate);

    // sub_576AA0(float value, float time) (thiscall, ret 8).
    void Record(float value, float time);
};

// Data tables of the player input chain (.data of SPEED2.EXE, read through Speed2Image).
class InputTables {
public:
    InputTable SteerResponse;        // 0x7E9E60: input -> curve (normal)
    InputTable SteerResponseSpecial; // 0x7E9E9C: input -> curve (drift / burnout)
    InputTable CounterSteer;         // 0x7E9EB0: |input| -> yaw factor (counter-steer assist)
    InputTable CounterSteerDrift;    // 0x7E9EC4: same, drift
    InputTable SpeedLimit;           // 0x7E9ED8: speed -> steering limit
    InputTable CurveAverageGain;     // 0x7E9F00: |avg curve| -> limit factor
    InputTable SpeedFactor;          // 0x7E9F14: speed -> pedal term / rate factor
    InputTable RateInput;            // 0x7E9F28: |d input / dt| -> rate sample
    InputTable RateCurveAverage;     // 0x7E9F3C: |avg curve| -> rate factor
    InputTable BurnoutThrottle;      // 0x7EA014: throttle curve in burnout
    Graph DriftYaw;                  // Graph 0x7E9F50: |car+0x128| -> max-angle factor (drift)
    Graph NeutralThrottle;           // Graph 0x7EA004: throttle curve in neutral
    Graph NeutralThrottleAlt;        // Graph 0x7EA00C: same, when _return_0(port) (never on PC)
    Graph CollisionRecovery;         // Graph unk_81E5B0 = {0x7E9F58, 4} (static init 0x77A7E0)

    explicit InputTables(const Speed2Image& exe);
};

} // namespace bb
