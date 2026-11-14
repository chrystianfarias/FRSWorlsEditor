#pragma once

// A quick fingerprint of a game file, shared by the editor and the loader.
//
// A published world is a set of byte ranges of the publisher's track files.
// On a game whose TRACKS\STREAML4RA.BUN differs -- another map mod, another
// release -- the same ranges are some other mesh, and splicing them in crashes
// the game. So the editor records what each file it edited looked like, and the
// loader leaves a region's edits out when the file there is not the same one.
//
// Hashing all of a 118 MB stream at every start would cost the game a second
// before it shows anything, so this takes the size and 33 blocks spread over
// the file: any repack, any mod that moves a section or changes the size shows.
// Header-only and Win32-only so the 32-bit loader can include it as it is.

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace frs {

struct FileFingerprint {
    unsigned long long size = 0;
    unsigned long long hash = 0;
    bool valid = false;
};

inline FileFingerprint FingerprintFile(const std::wstring& path) {
    FileFingerprint print;
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return print;

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size)) {
        ::CloseHandle(file);
        return print;
    }
    print.size = (unsigned long long)size.QuadPart;

    // FNV-1a, 64 bits, over the size and then each block.
    unsigned long long hash = 1469598103934665603ull;
    auto mix = [&hash](const unsigned char* bytes, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
    };
    mix((const unsigned char*)&print.size, sizeof(print.size));

    constexpr unsigned long long kBlock = 64 * 1024;
    constexpr int kBlocks = 32;
    std::vector<unsigned char> buffer((size_t)kBlock);
    bool ok = true;
    for (int i = 0; i <= kBlocks && ok; ++i) {
        // Blocks 0..31 evenly from the start; the last one ends at the end.
        unsigned long long at = i < kBlocks ? print.size / kBlocks * (unsigned long long)i
                                            : (print.size > kBlock ? print.size - kBlock : 0);
        LARGE_INTEGER offset{};
        offset.QuadPart = (LONGLONG)at;
        DWORD got = 0;
        ok = ::SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) &&
             ::ReadFile(file, buffer.data(), (DWORD)kBlock, &got, nullptr);
        if (ok) mix(buffer.data(), got);
    }
    ::CloseHandle(file);

    print.hash = hash;
    print.valid = ok;
    return print;
}

} // namespace frs
