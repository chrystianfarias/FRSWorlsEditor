#pragma once

// Port of NFSU2Unity Core/Numerics/Speed2Image.cs: read-only view of SPEED2.EXE
// addressed by virtual address (image base 0x400000), so the maths tables and
// constants come straight from the installed game.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace bb {

class Speed2Image {
public:
    // Throws std::runtime_error when the file cannot be read or is not a PE.
    explicit Speed2Image(const std::wstring& path);

    std::vector<uint8_t> Bytes(uint32_t va, int n) const;
    uint32_t U32(uint32_t va) const;
    uint16_t U16(uint32_t va) const;
    float F32(uint32_t va) const;

    // Whether this is the v1.2 exe the port was measured against.
    bool IsV12() const;

private:
    int Offset(uint32_t va, int n) const;

    struct Section { uint32_t va, size, raw, rawSize; };
    std::vector<uint8_t> data_;
    uint32_t imageBase_ = 0;
    std::vector<Section> sections_;
};

} // namespace bb
