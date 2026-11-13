#include "io/SectionSplice.h"

#include "core/Log.h"
#include "io/SectionRebuild.h"
#include "nfs/World.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace {

u64 AlignUp(u64 value, u64 to) { return (value + to - 1) / to * to; }

u32 Read32(const std::vector<u8>& bytes, size_t at) {
    u32 v = 0;
    if (at + 4 <= bytes.size()) std::memcpy(&v, bytes.data() + at, 4);
    return v;
}

// The end of the last chunk that is not zero-id padding.
size_t UsedLength(const std::vector<u8>& bytes) {
    size_t at = 0, used = 0;
    while (at + 8 <= bytes.size()) {
        const u32 id = Read32(bytes, at), size = Read32(bytes, at + 4);
        if (at + 8 + (size_t)size > bytes.size()) break;
        if (id != 0) used = at + 8 + size;
        at += 8 + (size_t)size;
    }
    return used;
}

} // namespace

std::vector<u8> SpliceSection(ByteSpan section, std::vector<ChunkEdit> edits) {
    std::sort(edits.begin(), edits.end(),
              [](const ChunkEdit& a, const ChunkEdit& b) { return a.start < b.start; });

    std::vector<u8> out;
    out.reserve(section.size + 65536);
    size_t cursor = 0;

    for (ChunkEdit& edit : edits) {
        // Padding the last save left behind belongs to the chunk it followed.
        size_t end = edit.end;
        while (end + 8 <= section.size && section.read<u32>(end) == 0) {
            const u32 size = section.read<u32>(end + 4);
            if (end + 8 + (size_t)size > section.size) break;
            end += 8 + (size_t)size;
        }

        out.insert(out.end(), section.data + cursor, section.data + edit.start);
        out.insert(out.end(), edit.bytes.begin(), edit.bytes.end());

        const long long delta = (long long)edit.bytes.size() - (long long)(end - edit.start);
        size_t pad = (size_t)(((-delta) % 128 + 128) % 128);
        if (pad != 0 && pad < 8) pad += 128;
        if (pad) {
            const size_t at = out.size();
            out.resize(at + pad, 0);
            const u32 size = (u32)(pad - 8);
            std::memcpy(out.data() + at + 4, &size, 4);
        }
        cursor = end;
    }
    out.insert(out.end(), section.data + cursor, section.data + section.size);

    // The zeros after the last chunk are only filler to the section's length,
    // so they give way to growth before the section itself grows.
    if (out.size() > section.size)
        out.resize(std::max(UsedLength(out), section.size), 0);
    if (out.size() < section.size) out.resize(section.size, 0);
    out.resize((size_t)AlignUp(out.size(), 16), 0);
    return out;
}

void PlanSectionSpans(const nfs::World& world, const nfs::Section& section,
                      std::vector<u8> bytes, const std::vector<SpanOverlayInfo>& existing,
                      const std::string& label, SectionSpanPlan& out) {
    const ByteSpan stream = world.StreamBytes();

    // Where the next thing in the stream starts after `at`: another section,
    // or a span some other section was moved into.
    const auto nextAfter = [&](u64 at) {
        u64 next = at < stream.size ? stream.size : ~0ull;
        for (const nfs::Section& other : world.Table().Sections()) {
            if (other.tableEntryOffset == section.tableEntryOffset) continue;
            if (other.streamOffset > at && other.streamOffset < next) next = other.streamOffset;
        }
        for (const SpanOverlayInfo& span : existing)
            if (!span.layoutFile && span.fileOffset != section.streamOffset &&
                span.fileOffset > at && span.fileOffset < next)
                next = span.fileOffset;
        return next;
    };
    u64 highestEnd = stream.size;
    for (const nfs::Section& other : world.Table().Sections())
        highestEnd = std::max<u64>(highestEnd, other.streamOffset + other.streamSize);
    for (const SpanOverlayInfo& span : existing)
        if (!span.layoutFile) highestEnd = std::max<u64>(highestEnd, span.fileOffset + span.byteCount);

    // Home first -- where the game shipped it -- then wherever an earlier
    // edit moved it, then somewhere new past the end of everything.
    u64 offset;
    if (section.originalStreamOffset + bytes.size() <= nextAfter(section.originalStreamOffset))
        offset = section.originalStreamOffset;
    else if (section.streamOffset >= stream.size &&
             section.streamOffset + bytes.size() <= nextAfter(section.streamOffset))
        offset = section.streamOffset;
    else
        offset = AlignUp(highestEnd + 4096, 2048);

    const u32 size = (u32)bytes.size();
    const u32 geometry = SectionGeometrySize(bytes);

    for (const SpanOverlayInfo& span : existing) {
        const bool sameSection =
            span.layoutFile
                ? span.fileOffset < section.tableEntryOffset + 80 &&
                  span.fileOffset + span.byteCount > section.tableEntryOffset
                : span.fileOffset == section.streamOffset;
        if (sameSection &&
            std::find(out.superseded.begin(), out.superseded.end(), span.path) == out.superseded.end())
            out.superseded.push_back(span.path);
    }

    const std::wstring region = world.Region();
    const std::wstring name(section.name.begin(), section.name.end());

    SectionSpan body;
    body.name = label + " (section " + section.name + ")";
    body.key = region + L"-" + name + L"-stream";
    body.fileOffset = offset;
    body.bytes = std::move(bytes);
    out.spans.push_back(std::move(body));

    out.tableChanged = offset != section.originalStreamOffset ||
                       size != section.originalStreamSize ||
                       geometry != section.originalOtherSize;
    if (out.tableChanged) {
        SectionSpan table;
        table.name = "section " + section.name + " in the region table";
        table.key = region + L"-" + name + L"-table";
        table.fileOffset = section.tableEntryOffset + 0x14;
        table.layoutFile = true;
        table.bytes.resize(12);
        const u32 offset32 = (u32)offset;
        std::memcpy(table.bytes.data(), &offset32, 4);
        std::memcpy(table.bytes.data() + 4, &size, 4);
        std::memcpy(table.bytes.data() + 8, &geometry, 4);
        out.spans.push_back(std::move(table));
    }

    out.offset = offset;
    out.size = size;
    out.moved = offset != section.streamOffset;
}

size_t WriteSectionSpans(const std::vector<SectionSpan>& spans,
                         const std::vector<std::wstring>& superseded,
                         const std::wstring& modelsFolder, const std::wstring& region) {
    size_t written = 0;
    std::vector<std::wstring> writtenPaths;
    for (const SectionSpan& span : spans) {
        if (WriteSpanOverlay(modelsFolder, region, span.name, span.fileOffset, span.layoutFile,
                             span.bytes, span.key)) {
            ++written;
            writtenPaths.push_back(modelsFolder + L"\\" + span.key + L".frssection");
        }
    }
    if (written != spans.size()) return written;

    for (const std::wstring& path : superseded) {
        bool rewritten = false;
        for (const std::wstring& done : writtenPaths)
            if (::lstrcmpiW(done.c_str(), path.c_str()) == 0) rewritten = true;
        if (rewritten) continue;   // the same file, just rewritten
        if (::DeleteFileW(path.c_str()))
            LOG_INFO("removed the superseded span %S", path.c_str());
        else
            LOG_WARN("could not remove %S", path.c_str());
    }
    return written;
}
