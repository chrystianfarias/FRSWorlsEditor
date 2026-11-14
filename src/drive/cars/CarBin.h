#pragma once

// Small binary helpers shared by the car loaders (src/drive/cars, src/drive/CarModel):
// the subset of NFSU2Unity Core/IO (BinHash, Chunk, BinReader) they need, over whole-file
// byte vectors. Kept in the bb::carbin namespace so they never clash with other ports.

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace bb {
namespace carbin {

// The engine's bStringHash (SPEED2.EXE 0x43DB50): h = h*33 + byte, seed 0xFFFFFFFF.
inline uint32_t Hash(const char* name, uint32_t h = 0xFFFFFFFFu) {
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p) h = h * 33u + *p;
    return h;
}
inline uint32_t Hash(const std::string& name, uint32_t h = 0xFFFFFFFFu) {
    for (unsigned char c : name) h = h * 33u + c;
    return h;
}

// Reads a whole file. Returns false when it cannot be opened.
bool ReadFile(const std::wstring& path, std::vector<uint8_t>& out);

// <gameFolder>\<relative>, relative in the game's own form (e.g. "CARS\\GTO\\GEOMETRY.BIN").
inline std::wstring GamePath(const std::wstring& gameFolder, const std::string& relative) {
    std::wstring path = gameFolder;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
    for (char c : relative) path += (wchar_t)(unsigned char)c;
    return path;
}

// One bChunk: u32 id, u32 size, payload. Id bit 31 = container.
struct Chunk {
    uint32_t Id = 0;
    int Offset = 0;   // absolute offset of the header
    int Size = 0;
    std::vector<Chunk> Children;

    bool IsContainer() const { return (Id & 0x80000000u) != 0; }
    int DataOffset() const { return Offset + 8; }
    int End() const { return Offset + 8 + Size; }

    const Chunk* Find(uint32_t id) const {
        for (const Chunk& c : Children)
            if (c.Id == id) return &c;
        return nullptr;
    }
};

// Parses [offset, end) recursing into containers. A chunk that overruns its parent stops the
// sequence (the C# throws; the callers here only need what parses).
std::vector<Chunk> Parse(const std::vector<uint8_t>& buf, int offset = 0, int end = -1);

// Depth-first visit of every chunk.
template <class F>
void Walk(const std::vector<Chunk>& roots, F&& f) {
    for (const Chunk& c : roots) {
        f(c);
        Walk(c.Children, f);
    }
}

// Bounds-checked little-endian reader over [start, end) of a buffer; offsets are relative to start.
class Reader {
public:
    Reader(const uint8_t* buf, int start, int end) : buf_(buf), start_(start), end_(end), pos_(start) {}

    int Length() const { return end_ - start_; }
    int Remaining() const { return end_ - pos_; }
    int Relative() const { return pos_ - start_; }
    void Seek(int rel) { pos_ = start_ + rel; }
    void Skip(int n) { pos_ += n; }

    template <class T>
    T Read() {
        if (pos_ < start_ || pos_ + (int)sizeof(T) > end_) throw std::runtime_error("read past the end of a chunk");
        T v;
        std::memcpy(&v, buf_ + pos_, sizeof(T));
        pos_ += (int)sizeof(T);
        return v;
    }
    uint8_t U8() { return Read<uint8_t>(); }
    int8_t S8() { return Read<int8_t>(); }
    uint16_t U16() { return Read<uint16_t>(); }
    int16_t S16() { return Read<int16_t>(); }
    uint32_t U32() { return Read<uint32_t>(); }
    int32_t S32() { return Read<int32_t>(); }
    float F32() { return Read<float>(); }

    // Fixed-size zero-terminated string.
    std::string FixedString(int n) {
        if (pos_ < start_ || pos_ + n > end_) throw std::runtime_error("string past the end of a chunk");
        const char* p = reinterpret_cast<const char*>(buf_ + pos_);
        size_t len = 0;
        while ((int)len < n && p[len]) ++len;
        pos_ += n;
        return std::string(p, len);
    }

private:
    const uint8_t* buf_;
    int start_, end_, pos_;
};

// Chunk.Reader(align): skips leading 0x11 filler up to the alignment (lenient variant used for cars).
inline Reader ChunkReader(const std::vector<uint8_t>& buf, const Chunk& c, int align = 1) {
    int p = c.DataOffset();
    if (align > 1)
        while (p < c.End() && buf[p] == 0x11 && p % align != 0) ++p;
    return Reader(buf.data(), p, c.End());
}

// Zero-terminated string at buf[offset], bounded by the buffer.
inline std::string CString(const std::vector<uint8_t>& buf, int offset) {
    if (offset < 0 || offset >= (int)buf.size()) return {};
    size_t n = 0;
    while (offset + (int)n < (int)buf.size() && buf[offset + n]) ++n;
    return std::string(reinterpret_cast<const char*>(buf.data() + offset), n);
}

} // namespace carbin
} // namespace bb
