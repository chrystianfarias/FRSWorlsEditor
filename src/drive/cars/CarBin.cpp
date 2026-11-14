#include "drive/cars/CarBin.h"

#include <cstdio>

namespace bb {
namespace carbin {

bool ReadFile(const std::wstring& path, std::vector<uint8_t>& out) {
    out.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(f);
        return false;
    }
    out.resize((size_t)size);
    const size_t got = size ? std::fread(out.data(), 1, (size_t)size, f) : 0;
    std::fclose(f);
    if (got != (size_t)size) {
        out.clear();
        return false;
    }
    return true;
}

std::vector<Chunk> Parse(const std::vector<uint8_t>& buf, int offset, int end) {
    if (end < 0) end = (int)buf.size();
    std::vector<Chunk> list;
    while (offset + 8 <= end) {
        uint32_t id;
        int32_t size;
        std::memcpy(&id, buf.data() + offset, 4);
        std::memcpy(&size, buf.data() + offset + 4, 4);
        if (size < 0 || (int64_t)offset + 8 + size > end) break;
        Chunk c;
        c.Id = id;
        c.Offset = offset;
        c.Size = size;
        if (c.IsContainer() && size > 0) c.Children = Parse(buf, offset + 8, offset + 8 + size);
        list.push_back(std::move(c));
        offset += 8 + size;
    }
    return list;
}

} // namespace carbin
} // namespace bb
