#pragma once

// Port of NFSU2Unity Core/Physics/Engine/Burnout.cs.

namespace bb {

class BWareMath;
class Car;
class CarPhysicsInfo;
class DriveTrain;
class EngineTables;
class Wheel;
class World;

// PhysicsMover::CalcBurnout 0x5B6300 (called once per Move before the substeps): grip boost after a good
// drag shift and the launch "wheel hop" (alternating diagonals). Ported here because it is part of the
// drivetrain launch behaviour; it only touches the wheels' grip scale and the CarState burnout fields.
// Not modelled: the lazy RidePhysicsInfo::RebuildPhysicsInfo(1, 1, 1) call made when RPI+4 == 0xFF
// (the caller must pass an already built level).
namespace Burnout {

// PhysicsMover::CalcBurnout() 0x5B6300. Reads Car+0x24 (driver type), Car::IsBlinking, Car+0x42C
// (= CarState+0x3EC), cpi+0x1C0, (s8)RidePhysicsInfo+4, the burnout mode flags, pCurrentRace->state,
// WorldTimer and WorldTimeElapsed. wheels = 4 wheels.
void CalcBurnout(const EngineTables& k, const BWareMath& math, World& world, Car& car, const CarPhysicsInfo& cpi,
                 Wheel* const* wheels, DriveTrain& dt);

} // namespace Burnout

} // namespace bb
