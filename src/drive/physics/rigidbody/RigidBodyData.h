#pragma once

// Port of NFSU2Unity Core/Physics/RigidBody/RigidBodyData.cs.

#include "drive/numerics/Speed2Image.h"
#include "drive/numerics/X87.h"

#include <cstdint>
#include <vector>

namespace bb {

// Constants and tables used by RigidBody / Aerodynamics / CarCollisionBody, loaded from SPEED2.EXE.
// .rdata constants are kept as exact bit patterns and verified against the exe; .data values
// (tweakables that the game could change at run time) and the two damping tables are read from it.
class RigidBodyData {
public:
    // ---- .rdata (exact bit patterns)
    static inline const float Zero = X87::Bits(0x00000000u);                 // _zero 0x7D7D3C
    static inline const float One = X87::Bits(0x3F800000u);                  // float_1 0x784250
    static inline const float Half = X87::Bits(0x3F000000u);                 // flt_784260 0.5
    static inline const float TopSpeedBias = X87::Bits(0x3FF33333u);         // flt_7A1E80 1.9
    static inline const float RaceEngineTopSpeedMul = X87::Bits(0x3F83D70Au); // flt_7A1B18 1.03
    static inline const float UphillMaxSlope = X87::Bits(0x3E99999Au);       // flt_7A1AF4 0.3
    static inline const float UphillMinFactor = X87::Bits(0x3F666666u);      // flt_7A1AF0 0.9
    static inline const float DownhillMaxSlope = X87::Bits(0x3E99999Au);     // flt_7A1AEC 0.3
    static inline const float DownhillMaxFactor = X87::Bits(0x3F8CCCCDu);    // flt_7A1AE8 1.1
    static inline const float MpsToMph = X87::Bits(0x400F1B08u);             // flt_7A1E7C 2.2360249
    static inline const float LimiterSlopeGain = X87::Bits(0x40A00000u);     // flt_7A1AFC 5.0
    static inline const float LimiterMaxForce = X87::Bits(0xBF000000u);      // flt_7A1AF8 -0.5
    static inline const float RestSpeedDiv = X87::Bits(0x40800000u);         // flt_7A1B00 4.0
    static inline const float RestSpinDiv = X87::Bits(0x40000000u);          // flt_7A1B04 2.0
    static inline const float RestLevelRise = X87::Bits(0x3DCCCCCDu);        // dword_7A1B10 0.1
    static inline const float RestLevelFall = X87::Bits(0x41200000u);        // flt_7A1B14 10.0 (negated)
    static inline const float RestAngularGain = X87::Bits(0x40000000u);      // flt_7A1B0C 2.0
    static inline const float RestLinearGain = X87::Bits(0x41000000u);       // flt_7A1B08 8.0
    static inline const float InvGravityG = X87::Bits(0x3DD0FAC6u);          // flt_7A1E84 0.10204081 (1/9.8)
    static inline const float MaxAngularSpeed = X87::Bits(0x437A0000u);      // flt_7A1B1C 250
    static constexpr double VelocityClampMinMass = 0.7;                      // dbl_7A1E88 (qword compare)
    static inline const float MaxForce = X87::Bits(0x44960000u);             // flt_7A1AD8 1200
    static inline const float MaxTorque = X87::Bits(0x42C80000u);            // flt_7A1ADC 100
    static inline const float MaxImpulse = X87::Bits(0x447A0000u);           // flt_7A1AE0 1000
    static inline const float MaxAngularImpulse = X87::Bits(0x41200000u);    // flt_7A1AE4 10
    static inline const float AeroMinDownforceFactor = X87::Bits(0x3ECCCCCDu); // flt_7A1E4C 0.4 (also front share)
    static inline const float AeroAirRearShare = X87::Bits(0x3ED1EB85u);     // flt_7A1F18 0.41
    static inline const float AeroMinSpeed = X87::Bits(0x38D1B717u);         // flt_7843EC 1e-4
    static inline const float CcbMaxInvMassDiv = X87::Bits(0x3FECCCCDu);     // flt_7A1884 1.85
    static inline const float CcbFriction = X87::Bits(0x3E99999Au);          // flt_78758C 0.3
    static inline const float CcbFlatNormalZ = X87::Bits(0x3F333333u);       // flt_7844D8 0.7
    static inline const float CcbFrictionPerAirWheel = X87::Bits(0x3E2E147Bu); // flt_7A1898 0.17
    static inline const float CcbFriction428 = X87::Bits(0x00000000u);       // flt_7A1894 0.0
    static inline const float CcbWallFrictionMul = X87::Bits(0x3E99999Au);   // flt_7A1890 0.3
    static inline const float CcbWallBounceMul = X87::Bits(0x3F000000u);     // flt_7A188C 0.5
    static inline const float PointOne = X87::Bits(0x3DCCCCCDu);             // _zero_point_one 0x784358 0.1

    // ---- .data (read from the exe)
    static constexpr uint32_t VA_VelocityClamp = 0x7FC6EC;     // 200.0
    static constexpr uint32_t VA_Gravity = 0x7FBEF0;           // 9.8
    static constexpr uint32_t VA_AeroThrottleDragMul = 0x7FBED0; // 2.0
    static constexpr uint32_t VA_AeroLiftLever = 0x7FBED4;     // -0.1
    static constexpr uint32_t VA_AngularDampingTable = 0x7FC92C; // Table {6, 0, 10, 0.5, ptr}
    static constexpr uint32_t VA_RestLevelTable = 0x7FC940;      // Table {8, 0, 1, 7, ptr}
    static constexpr uint32_t VA_AeroFrontLever = 0x7FC860;    // bVector4 (1,0,0,0)
    static constexpr uint32_t VA_AeroRearLever = 0x7FC870;     // bVector4 (-1,0,0,0)

    // Table (0x14 bytes): +0 count, +4 min, +8 max, +0xC index multiplier, +0x10 float* data.
    class Table {
    public:
        int Count = 0;
        float Min = 0, Max = 0, IndexMultiplier = 0;
        std::vector<float> Data;
        float GetValue(float x) const;
        static Table Load(const Speed2Image& exe, uint32_t va);
    };

    float VelocityClamp = 0;
    float Gravity = 0;
    float AeroThrottleDragMul = 0;
    float AeroLiftLever = 0;
    float AeroFrontLever[3] = {};
    float AeroRearLever[3] = {};
    Table AngularDamping;
    Table RestLevel;

    explicit RigidBodyData(const Speed2Image& exe);
};

} // namespace bb
