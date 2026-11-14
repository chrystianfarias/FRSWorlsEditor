#pragma once

// Port of NFSU2Unity Core/Physics/TrackTopology.cs.

namespace bb {

/// The per-car topology side of a ground provider that models the game's TopologyCoordinate (Car+0x5C4,
/// pointed to by CarState+0x4). Optional: VehicleSim only uses it when the ground passed to it implements it
/// (bb::TrackWorld does); FlatGround / TestGround do not, so their behaviour and the goldens are unchanged.
/// Coordinates are the C# `object`: opaque pointers owned by the provider.
class ITrackTopology {
public:
    virtual ~ITrackTopology() = default;

    /// TopologyCoordinate::TopologyCoordinate 0x5E0B40: a new client (one per car).
    virtual void* CreateTopologyCoordinate() = 0;

    /// TopologyCoordinate::SetSectionPosition 0x5E68D0 as called by Car::BeginMovement 0x5FBBE0 and
    /// Car::CheckForLoadedTopology 0x5F6CD0 (slack 0, the car's position).
    virtual void SetSectionPosition(void* coordinate, float x, float y, float slack) = 0;

    /// TopologyCoordinate+0x08 != 0: the coordinate is connected to a loaded section (Car+0x5C0).
    virtual bool IsConnected(void* coordinate) = 0;
};

} // namespace bb
