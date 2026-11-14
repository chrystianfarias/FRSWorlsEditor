#include "drive/numerics/Speed2Image.h"

#include <windows.h>

#include <cstring>
#include <fstream>

namespace bb {

namespace {

template <typename T>
T ReadAt(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + sizeof(T) > data.size()) throw std::runtime_error("SPEED2.EXE: read past the end");
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

} // namespace

Speed2Image::Speed2Image(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("SPEED2.EXE not found");
    data_.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (data_.size() < 0x400 || data_[0] != 'M' || data_[1] != 'Z') throw std::runtime_error("SPEED2.EXE is not a PE file");
    int pe = ReadAt<int32_t>(data_, 0x3C);
    int nsec = ReadAt<uint16_t>(data_, pe + 6);
    int optsz = ReadAt<uint16_t>(data_, pe + 20);
    int opt = pe + 24;
    imageBase_ = ReadAt<uint32_t>(data_, opt + 28);
    for (int i = 0; i < nsec; ++i) {
        int o = opt + optsz + 40 * i;
        uint32_t vsize = ReadAt<uint32_t>(data_, o + 8), rva = ReadAt<uint32_t>(data_, o + 12);
        uint32_t rsize = ReadAt<uint32_t>(data_, o + 16), raw = ReadAt<uint32_t>(data_, o + 20);
        sections_.push_back({imageBase_ + rva, vsize > rsize ? vsize : rsize, raw, rsize});
    }
}

int Speed2Image::Offset(uint32_t va, int n) const {
    for (const auto& s : sections_)
        if (va >= s.va && va + static_cast<uint32_t>(n) <= s.va + s.rawSize)
            return static_cast<int>(s.raw + (va - s.va));
    char text[96];
    std::snprintf(text, sizeof text, "SPEED2.EXE: VA 0x%08X not backed by file data", va);
    throw std::runtime_error(text);
}

std::vector<uint8_t> Speed2Image::Bytes(uint32_t va, int n) const {
    int o = Offset(va, n);
    return std::vector<uint8_t>(data_.begin() + o, data_.begin() + o + n);
}

uint32_t Speed2Image::U32(uint32_t va) const { return ReadAt<uint32_t>(data_, Offset(va, 4)); }
uint16_t Speed2Image::U16(uint32_t va) const { return ReadAt<uint16_t>(data_, Offset(va, 2)); }
float Speed2Image::F32(uint32_t va) const { return ReadAt<float>(data_, Offset(va, 4)); }

bool Speed2Image::IsV12() const {
    // float_1 at 0x784250 and the bSin constants are where v1.2 has them.
    try {
        return U32(0x784250) == 0x3F800000u && U32(0x786D98) == 0x40490FDBu;
    } catch (...) {
        return false;
    }
}

} // namespace bb
