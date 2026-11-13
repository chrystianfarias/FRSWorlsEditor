#include "render/ModelThumbnails.h"

#include "core/Log.h"
#include "nfs/SolidList.h"
#include "render/WorldRenderer.h"

#include <windows.h>
#include <share.h>
#include <shlobj.h>
#include <wincodec.h>

#include <cstring>
#include <cwchar>

namespace {

// Bumped whenever the pictures are drawn differently, so old ones are not
// mixed in with new ones: 2 is blended and additive materials and the global
// texture packs. A pack of another version is started again.
constexpr u32 kPictureVersion = 2;

constexpr char kPackMagic[8] = { 'F', 'R', 'S', 'T', 'H', 'U', 'M', 'B' };
constexpr u32  kRecordMagic = 0x54535246;     // "FRST"
constexpr size_t kHeaderSize = 16;
constexpr size_t kRecordHeader = 20;

u64 MeshKey(const nfs::SolidObject& model) {
    u64 h = 1469598103934665603ull;
    const auto mix = [&](const void* data, size_t size) {
        const u8* bytes = (const u8*)data;
        for (size_t i = 0; i < size; ++i) h = (h ^ bytes[i]) * 1099511628211ull;
    };
    mix(&kPictureVersion, sizeof(kPictureVersion));
    const size_t counts[2] = { model.vertices.size(), model.indices.size() };
    mix(counts, sizeof(counts));
    for (const nfs::SolidVertex& vertex : model.vertices) mix(&vertex.position, sizeof(vertex.position));
    for (u32 texture : model.textureHashes) mix(&texture, sizeof(texture));
    return h;
}

IWICImagingFactory* Factory() {
    static IWICImagingFactory* factory = nullptr;
    if (!factory &&
        FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory))))
        factory = nullptr;
    return factory;
}

bool DecodePng(const std::vector<u8>& png, u32 size, std::vector<u32>& bgra) {
    IWICImagingFactory* factory = Factory();
    if (!factory || png.empty()) return false;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    UINT width = 0, height = 0;
    bool ok = SUCCEEDED(factory->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromMemory((BYTE*)png.data(), (DWORD)png.size())) &&
              SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand,
                                                         &decoder)) &&
              SUCCEEDED(decoder->GetFrame(0, &frame)) &&
              SUCCEEDED(frame->GetSize(&width, &height)) && width == size && height == size &&
              SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
              SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                              WICBitmapDitherTypeNone, nullptr, 0.0,
                                              WICBitmapPaletteTypeCustom));
    if (ok) {
        bgra.resize((size_t)size * size);
        ok = SUCCEEDED(converter->CopyPixels(nullptr, size * 4, size * size * 4, (BYTE*)bgra.data()));
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    return ok;
}

bool EncodePng(const std::vector<u32>& bgra, u32 size, std::vector<u8>& png) {
    IWICImagingFactory* factory = Factory();
    if (!factory) return false;
    IStream* memory = nullptr;
    if (FAILED(::CreateStreamOnHGlobal(nullptr, TRUE, &memory))) return false;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
              SUCCEEDED(encoder->Initialize(memory, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
              SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(size, size)) &&
              SUCCEEDED(frame->SetPixelFormat(&format)) &&
              SUCCEEDED(frame->WritePixels(size, size * 4, size * size * 4, (BYTE*)bgra.data())) &&
              SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    if (ok) {
        HGLOBAL global = nullptr;
        STATSTG stat{};
        ok = SUCCEEDED(::GetHGlobalFromStream(memory, &global)) &&
             SUCCEEDED(memory->Stat(&stat, STATFLAG_NONAME));
        const void* data = ok ? ::GlobalLock(global) : nullptr;
        if (data) {
            png.assign((const u8*)data, (const u8*)data + (size_t)stat.cbSize.QuadPart);
            ::GlobalUnlock(global);
        } else {
            ok = false;
        }
    }
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    memory->Release();
    return ok;
}

std::wstring EditorFolder() {
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring folder = path;
    const size_t slash = folder.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : folder.substr(0, slash);
}

bool ReadHeader(FILE* file) {
    char magic[8];
    u32 version = 0, size = 0;
    return std::fread(magic, 1, 8, file) == 8 && std::memcmp(magic, kPackMagic, 8) == 0 &&
           std::fread(&version, 4, 1, file) == 1 && std::fread(&size, 4, 1, file) == 1 &&
           version == kPictureVersion && size == ModelThumbnails::kSize;
}

bool WriteHeader(FILE* file) {
    const u32 version = kPictureVersion, size = ModelThumbnails::kSize;
    return std::fwrite(kPackMagic, 1, 8, file) == 8 && std::fwrite(&version, 4, 1, file) == 1 &&
           std::fwrite(&size, 4, 1, file) == 1;
}

long long FileSize(FILE* file) {
    const long long here = ::_ftelli64(file);
    ::_fseeki64(file, 0, SEEK_END);
    const long long size = ::_ftelli64(file);
    ::_fseeki64(file, here, SEEK_SET);
    return size;
}

} // namespace

ModelThumbnails::~ModelThumbnails() {
    Close();
}

void ModelThumbnails::Close() {
    if (pack_) std::fclose(pack_);
    if (shipped_) std::fclose(shipped_);
    pack_ = shipped_ = nullptr;
    index_.clear();
    liveBytes_ = totalBytes_ = 0;
}

void ModelThumbnails::SetRegion(const std::wstring& region) {
    if (region == region_ && !packPath_.empty()) return;
    Close();
    region_ = region;
    packPath_.clear();
    shippedPath_.clear();
    wchar_t localAppData[MAX_PATH] = {};
    if (region.empty() ||
        FAILED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData)))
        return;
    const std::wstring folder = std::wstring(localAppData) + L"\\FRSWorldEditor\\thumbs";
    const int made = ::SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    if (made != ERROR_SUCCESS && made != ERROR_ALREADY_EXISTS) return;

    packPath_ = folder + L"\\" + region + L".frsthumbs";
    shippedPath_ = EditorFolder() + L"\\thumbs\\" + region + L".frsthumbs";

    // The shipped set first, so what this machine drew since wins.
    Load(shippedPath_, true);
    Load(packPath_, false);

    // The loose PNGs of the editor before the pack: taken in once, then gone.
    const std::wstring loose = folder + L"\\v2\\" + region;
    if (::GetFileAttributesW(loose.c_str()) != INVALID_FILE_ATTRIBUTES) ImportFolder(loose);

    if (totalBytes_ > (1u << 20) && liveBytes_ * 2 < totalBytes_) Compact();
    LOG_INFO("thumbnails %S: %zu pictures kept", region.c_str(), index_.size());
}

void ModelThumbnails::Load(const std::wstring& path, bool shipped) {
    FILE* file = ::_wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!file) return;
    if (!ReadHeader(file)) {
        std::fclose(file);
        // Drawn another way, or not a pack at all: the user's is started again.
        if (!shipped) {
            ::DeleteFileW(path.c_str());
            LOG_INFO("thumbnails: %S is of another version and is started again", path.c_str());
        }
        return;
    }
    const long long size = FileSize(file);
    u64 at = kHeaderSize;
    while ((long long)(at + kRecordHeader) <= size) {
        u32 header[2] = {};
        u64 key = 0;
        u32 length = 0;
        if (std::fread(header, 4, 2, file) != 2 || header[0] != kRecordMagic ||
            std::fread(&key, 8, 1, file) != 1 || std::fread(&length, 4, 1, file) != 1)
            break;
        const u64 png = at + kRecordHeader;
        // A record cut short by a crash is where the pack ends.
        if ((long long)(png + length) > size) break;
        if (::_fseeki64(file, (long long)length, SEEK_CUR) != 0) break;

        const auto previous = index_.find(header[1]);
        if (previous != index_.end() && !previous->second.shipped)
            liveBytes_ -= kRecordHeader + previous->second.length;
        index_[header[1]] = Entry{ key, png, length, shipped };
        if (!shipped) {
            liveBytes_ += kRecordHeader + length;
            totalBytes_ += kRecordHeader + length;
        }
        at = png + length;
    }
    if (shipped) shipped_ = file;
    else pack_ = file;
}

void ModelThumbnails::ImportFolder(const std::wstring& folder) {
    WIN32_FIND_DATAW found{};
    HANDLE search = ::FindFirstFileW((folder + L"\\*.png").c_str(), &found);
    size_t taken = 0;
    if (search != INVALID_HANDLE_VALUE) {
        do {
            unsigned hash = 0;
            unsigned long long key = 0;
            if (::swscanf_s(found.cFileName, L"%8X-%16llX.png", &hash, &key) != 2) continue;
            const std::wstring path = folder + L"\\" + found.cFileName;
            FILE* file = nullptr;
            std::vector<u8> png;
            if (::_wfopen_s(&file, path.c_str(), L"rb") == 0 && file) {
                png.resize((size_t)FileSize(file));
                if (std::fread(png.data(), 1, png.size(), file) != png.size()) png.clear();
                std::fclose(file);
            }
            const auto have = index_.find(hash);
            if (!png.empty() && (have == index_.end() || have->second.key != key) &&
                Append(hash, key, png))
                ++taken;
            ::DeleteFileW(path.c_str());
        } while (::FindNextFileW(search, &found));
        ::FindClose(search);
    }
    ::RemoveDirectoryW(folder.c_str());
    const std::wstring parent = folder.substr(0, folder.find_last_of(L"\\/"));
    ::RemoveDirectoryW(parent.c_str());     // only goes when no other region is left in it
    LOG_INFO("thumbnails: %zu loose pictures taken into the pack", taken);
}

bool ModelThumbnails::Append(u32 hash, u64 key, const std::vector<u8>& png) {
    if (packPath_.empty()) return false;
    FILE* file = ::_wfsopen(packPath_.c_str(), L"ab", _SH_DENYNO);
    if (!file) return false;
    ::_fseeki64(file, 0, SEEK_END);
    long long at = ::_ftelli64(file);
    if (at == 0) {
        if (!WriteHeader(file)) { std::fclose(file); return false; }
        at = (long long)kHeaderSize;
    }
    const u32 header[2] = { kRecordMagic, hash };
    const u32 length = (u32)png.size();
    const bool ok = std::fwrite(header, 4, 2, file) == 2 && std::fwrite(&key, 8, 1, file) == 1 &&
                    std::fwrite(&length, 4, 1, file) == 1 &&
                    std::fwrite(png.data(), 1, png.size(), file) == png.size();
    std::fclose(file);
    if (!ok) return false;

    const auto previous = index_.find(hash);
    if (previous != index_.end() && !previous->second.shipped)
        liveBytes_ -= kRecordHeader + previous->second.length;
    index_[hash] = Entry{ key, (u64)at + kRecordHeader, length, false };
    liveBytes_ += kRecordHeader + length;
    totalBytes_ += kRecordHeader + length;
    if (!pack_) pack_ = ::_wfsopen(packPath_.c_str(), L"rb", _SH_DENYNO);
    return true;
}

bool ModelThumbnails::ReadPng(const Entry& entry, std::vector<u8>& png) {
    FILE* file = entry.shipped ? shipped_ : pack_;
    if (!file || ::_fseeki64(file, (long long)entry.offset, SEEK_SET) != 0) return false;
    png.resize(entry.length);
    return std::fread(png.data(), 1, png.size(), file) == png.size();
}

// Writes the pack again with only the newest picture of each model.
void ModelThumbnails::Compact() {
    const std::wstring temporary = packPath_ + L".new";
    FILE* out = nullptr;
    if (::_wfopen_s(&out, temporary.c_str(), L"wb") != 0 || !out) return;
    bool ok = WriteHeader(out);
    std::unordered_map<u32, Entry> moved;
    u64 at = kHeaderSize;
    std::vector<u8> png;
    for (const auto& [hash, entry] : index_) {
        if (entry.shipped) { moved[hash] = entry; continue; }
        if (!ok || !ReadPng(entry, png)) { ok = false; break; }
        const u32 header[2] = { kRecordMagic, hash };
        const u32 length = (u32)png.size();
        ok = std::fwrite(header, 4, 2, out) == 2 && std::fwrite(&entry.key, 8, 1, out) == 1 &&
             std::fwrite(&length, 4, 1, out) == 1 &&
             std::fwrite(png.data(), 1, png.size(), out) == png.size();
        moved[hash] = Entry{ entry.key, at + kRecordHeader, length, false };
        at += kRecordHeader + length;
    }
    std::fclose(out);
    if (!ok) { ::DeleteFileW(temporary.c_str()); return; }
    if (pack_) std::fclose(pack_);
    pack_ = nullptr;
    if (!::MoveFileExW(temporary.c_str(), packPath_.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileW(temporary.c_str());
        pack_ = ::_wfsopen(packPath_.c_str(), L"rb", _SH_DENYNO);
        return;
    }
    const u64 before = totalBytes_;
    index_ = std::move(moved);
    liveBytes_ = totalBytes_ = at - kHeaderSize;
    pack_ = ::_wfsopen(packPath_.c_str(), L"rb", _SH_DENYNO);
    LOG_INFO("thumbnails: pack compacted from %llu to %llu bytes", (unsigned long long)before,
             (unsigned long long)totalBytes_);
}

bool ModelThumbnails::Cached(const nfs::SolidObject& model) const {
    const auto found = index_.find(model.hash);
    return found != index_.end() && found->second.key == MeshKey(model);
}

bool ModelThumbnails::Get(WorldRenderer& renderer, const nfs::SolidObject& model,
                          std::vector<u32>& bgra) {
    const u64 key = MeshKey(model);
    const auto found = index_.find(model.hash);
    std::vector<u8> png;
    if (found != index_.end() && found->second.key == key && ReadPng(found->second, png) &&
        DecodePng(png, kSize, bgra))
        return true;
    if (!renderer.RenderThumbnail(model.hash, kSize, bgra)) return false;
    if (EncodePng(bgra, kSize, png)) Append(model.hash, key, png);
    return true;
}
