#pragma once

#include <cstring>
#include <cstdint>
#include <cstddef>

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

// A non-owning view over a block of bytes inside a mapped file.
struct ByteSpan {
    const u8* data = nullptr;
    size_t    size = 0;

    bool   empty() const { return size == 0; }
    const u8* begin() const { return data; }
    const u8* end()   const { return data + size; }

    ByteSpan sub(size_t offset, size_t count) const {
        if (offset > size) return {};
        if (count > size - offset) count = size - offset;
        return { data + offset, count };
    }

    // Reads a little-endian value at `offset`. Returns zero when out of range,
    // so a malformed file degrades into empty geometry instead of a crash.
    template <class T>
    T read(size_t offset) const {
        T value{};
        if (offset + sizeof(T) <= size)
            std::memcpy(&value, data + offset, sizeof(T));
        return value;
    }
};
