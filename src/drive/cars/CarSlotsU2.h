#pragma once

// Port of NFSU2Unity Core/Cars/CarSlotsU2.cs: static slot/part tables from SPEED2.EXE v1.2.

namespace bb {

struct CarSlotsU2 {
    static constexpr int NumSlots = 170;
    static constexpr int BasePaint = 63, HoodUnder = 25, Hood = 9, Engine = 13, WideBody = 6, FrontWheel = 29,
                         Hydraulics = 161;

    // CarSlotIDNames (0x802DE8): CAR_SLOT_ID names, id == index.
    static const char* const SlotNames[NumSlots];
    // CarPartIDNames (0x803338), in TABLE order (ids differ from indices at 16..26).
    static const char* const PartIdTableNames[69];
    // CarPartIDNames ids, in table order.
    static const int PartIdTableIds[69];
    // CarPartSlotMap (0x802960): CAR_SLOT_ID -> CarPartID.
    static const int SlotToPartId[NumSlots];
};

} // namespace bb
