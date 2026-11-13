#include "io/MeshOverlay.h"

#include "core/Log.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr char kMagic[8] = { 'F', 'R', 'S', 'M', 'E', 'S', 'H', '1' };

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                           nullptr, 0);
    std::wstring out((size_t)size, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size);
    return out;
}

// A file name that says which mesh it is without anyone opening it.
std::wstring OverlayFileName(const MeshOverlayHeader& header) {
    char name[96];
    ::_snprintf_s(name, _TRUNCATE, "%.8s-%.48s-%08X.frsmesh",
                  header.region, header.name, header.hash);

    std::string safe;
    for (char c : std::string(name)) {
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        safe.push_back(plain ? c : '_');
    }
    return Widen(safe);
}

} // namespace

bool WriteMeshOverlay(const std::wstring& modelsFolder, const MeshOverlay& overlay) {
    // An overlay is found again by the mesh's hash, so a zero one would sit in
    // the folder matching nothing and quietly do nothing at all.
    if (overlay.header.hash == 0 || overlay.positions.empty()) {
        LOG_ERROR("refusing to write an overlay with no mesh behind it");
        return false;
    }

    ::CreateDirectoryW(modelsFolder.c_str(), nullptr);

    const std::wstring path = modelsFolder + L"\\" + OverlayFileName(overlay.header);

    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) {
        LOG_ERROR("could not write the overlay %S", path.c_str());
        return false;
    }

    MeshOverlayHeader header = overlay.header;
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.vertexCount = (u32)overlay.positions.size();

    std::fwrite(&header, sizeof(header), 1, file);
    std::fwrite(overlay.positions.data(), sizeof(Vec3), overlay.positions.size(), file);
    std::fclose(file);

    LOG_INFO("overlay written: %S (%zu vertices)", path.c_str(), overlay.positions.size());
    return true;
}

size_t ReadMeshOverlays(const std::wstring& modelsFolder, const std::wstring& region,
                        std::unordered_map<u32, std::vector<Vec3>>& out) {
    WIN32_FIND_DATAW found{};
    HANDLE search = ::FindFirstFileW((modelsFolder + L"\\*.frsmesh").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return 0;

    size_t loaded = 0;
    do {
        FILE* file = nullptr;
        if (::_wfopen_s(&file, (modelsFolder + L"\\" + found.cFileName).c_str(), L"rb") != 0 ||
            !file)
            continue;

        MeshOverlayHeader header{};
        const bool readHeader = std::fread(&header, sizeof(header), 1, file) == 1;

        if (!readHeader || std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0) {
            std::fclose(file);
            LOG_WARN("overlay %S is not one of ours, skipped", found.cFileName);
            continue;
        }

        // An overlay belongs to one region: the offsets inside it mean nothing
        // in another one's stream, and applying them would corrupt geometry
        // somewhere unrelated.
        char wanted[8] = {};
        for (size_t i = 0; i < region.size() && i < sizeof(wanted); ++i)
            wanted[i] = (char)region[i];

        if (std::memcmp(header.region, wanted, sizeof(wanted)) != 0) {
            std::fclose(file);
            continue;
        }

        std::vector<Vec3> positions(header.vertexCount);
        const bool readAll =
            std::fread(positions.data(), sizeof(Vec3), header.vertexCount, file) ==
            header.vertexCount;
        std::fclose(file);

        if (!readAll) {
            LOG_WARN("overlay %S is short, skipped", found.cFileName);
            continue;
        }

        out[header.hash] = std::move(positions);
        ++loaded;
    } while (::FindNextFileW(search, &found));

    ::FindClose(search);
    return loaded;
}

size_t ListMeshOverlays(const std::wstring& modelsFolder,
                        std::vector<MeshOverlayInfo>& out) {
    WIN32_FIND_DATAW found{};
    HANDLE search = ::FindFirstFileW((modelsFolder + L"\\*.frsmesh").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return 0;

    do {
        const std::wstring path = modelsFolder + L"\\" + found.cFileName;

        FILE* file = nullptr;
        if (::_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) continue;

        MeshOverlayHeader header{};
        const bool read = std::fread(&header, sizeof(header), 1, file) == 1;
        std::fclose(file);

        if (!read || std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0) continue;

        MeshOverlayInfo info;
        info.path        = path;
        info.hash        = header.hash;
        info.vertexCount = header.vertexCount;
        info.model       = std::string(header.name, ::strnlen(header.name, sizeof(header.name)));
        info.region      = std::string(header.region,
                                       ::strnlen(header.region, sizeof(header.region)));
        out.push_back(std::move(info));
    } while (::FindNextFileW(search, &found));

    ::FindClose(search);
    return out.size();
}

bool WriteSpanOverlay(const std::wstring& modelsFolder, const std::wstring& region,
                      const std::string& name, u64 fileOffset, bool layoutFile,
                      const std::vector<u8>& bytes, const std::wstring& key) {
    if (bytes.empty()) return false;

    ::CreateDirectoryW(modelsFolder.c_str(), nullptr);

    SpanOverlayHeader header{};
    std::memcpy(header.magic, "FRSSECT1", 8);
    for (size_t i = 0; i < region.size() && i < sizeof(header.region); ++i)
        header.region[i] = (char)region[i];
    for (size_t i = 0; i + 1 < sizeof(header.name) && i < name.size(); ++i)
        header.name[i] = name[i];

    header.byteCount  = (u32)bytes.size();
    header.target     = layoutFile ? 1u : 0u;
    header.fileOffset = fileOffset;

    const std::wstring path = modelsFolder + L"\\" + key + L".frssection";

    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) {
        LOG_ERROR("could not write the span %S", path.c_str());
        return false;
    }

    std::fwrite(&header, sizeof(header), 1, file);
    std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);

    LOG_INFO("span written: %S (%zu bytes at %llu of the %s)", path.c_str(), bytes.size(),
             fileOffset, layoutFile ? "region table" : "stream");
    return true;
}

size_t ReadSpanOverlays(const std::wstring& modelsFolder, const std::wstring& region,
                        std::vector<SpanOverlay>& out) {
    WIN32_FIND_DATAW found{};
    HANDLE search = ::FindFirstFileW((modelsFolder + L"\\*.frssection").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return 0;

    char wanted[8] = {};
    for (size_t i = 0; i < region.size() && i < sizeof(wanted); ++i)
        wanted[i] = (char)region[i];

    do {
        FILE* file = nullptr;
        if (::_wfopen_s(&file, (modelsFolder + L"\\" + found.cFileName).c_str(), L"rb") != 0 ||
            !file)
            continue;

        SpanOverlayHeader header{};
        const bool read = std::fread(&header, sizeof(header), 1, file) == 1;

        if (!read || std::memcmp(header.magic, "FRSSECT1", 8) != 0 ||
            std::memcmp(header.region, wanted, sizeof(wanted)) != 0) {
            std::fclose(file);
            continue;
        }

        SpanOverlay span;
        span.fileOffset = header.fileOffset;
        span.layoutFile = header.target != 0;
        span.bytes.resize(header.byteCount);

        const bool readAll =
            std::fread(span.bytes.data(), 1, header.byteCount, file) == header.byteCount;
        std::fclose(file);

        if (!readAll) {
            LOG_WARN("span %S is short, skipped", found.cFileName);
            continue;
        }

        out.push_back(std::move(span));
    } while (::FindNextFileW(search, &found));

    ::FindClose(search);
    return out.size();
}

size_t ListSpanOverlays(const std::wstring& modelsFolder, const std::wstring& region,
                        std::vector<SpanOverlayInfo>& out) {
    WIN32_FIND_DATAW found{};
    HANDLE search = ::FindFirstFileW((modelsFolder + L"\\*.frssection").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return 0;

    char wanted[8] = {};
    for (size_t i = 0; i < region.size() && i < sizeof(wanted); ++i)
        wanted[i] = (char)region[i];

    do {
        const std::wstring path = modelsFolder + L"\\" + found.cFileName;
        FILE* file = nullptr;
        if (::_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) continue;

        SpanOverlayHeader header{};
        const bool read = std::fread(&header, sizeof(header), 1, file) == 1;
        std::fclose(file);

        if (!read || std::memcmp(header.magic, "FRSSECT1", 8) != 0 ||
            std::memcmp(header.region, wanted, sizeof(wanted)) != 0)
            continue;

        SpanOverlayInfo info;
        info.path       = path;
        info.name       = std::string(header.name, ::strnlen(header.name, sizeof(header.name)));
        info.fileOffset = header.fileOffset;
        info.byteCount  = header.byteCount;
        info.layoutFile = header.target != 0;
        out.push_back(std::move(info));
    } while (::FindNextFileW(search, &found));

    ::FindClose(search);
    return out.size();
}
