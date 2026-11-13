#include "nfs/WorldAnimation.h"

#include "core/Log.h"
#include "nfs/ChunkReader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

namespace nfs {

namespace {

constexpr u32 kEntities  = 0x00037080;
constexpr u32 kInstances = 0x00037090;
constexpr u32 kBank      = 0x00E34010;
constexpr size_t kEntitySize   = 0x70;
constexpr size_t kInstanceSize = 0xA0;

u32 StringHash(const std::string& text, u32 hash = 0xFFFFFFFFu) {
    for (unsigned char c : text) hash = hash * 33 + c;
    return hash;
}

// These chunks carry the 0x11 alignment filler the reader does not know of.
ByteSpan SkipFiller(ByteSpan data) {
    size_t at = 0;
    while (at + 4 <= data.size && at < 16 && data.read<u32>(at) == 0x11111111u) at += 4;
    return data.sub(at, data.size - at);
}

Mat4 ReadMatrix(ByteSpan span, size_t at) {
    Mat4 m;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m.m[r][c] = span.read<float>(at + (r * 4 + c) * 4);
    return m;
}

// ---- the ELF object of an EAGL animation bank --------------------------------
// ELF32, relocatable; pointers inside .data are offsets into .data, marked by
// its relocation table.
struct Image {
    ByteSpan data;
    std::set<u32> pointers;

    u8  U8(size_t at) const { return data.read<u8>(at); }
    u16 U16(size_t at) const { return data.read<u16>(at); }
    u32 U32(size_t at) const { return data.read<u32>(at); }
    float F32(size_t at) const { return data.read<float>(at); }
    // A pointer, or -1 when the slot holds none.
    long long Ptr(size_t at) const {
        if (!pointers.count((u32)at)) return -1;
        const u32 value = U32(at);
        return value < data.size ? (long long)value : -1;
    }
    std::string Text(size_t at) const {
        if (at >= data.size) return {};
        const char* start = (const char*)data.data + at;
        return std::string(start, strnlen(start, data.size - at));
    }
};

bool ReadElf(ByteSpan chunk, Image& image, u32& bank) {
    size_t start = 0;
    while (start + 4 <= chunk.size && start < 64 && std::memcmp(chunk.data + start, "\x7f" "ELF", 4) != 0) ++start;
    if (start + 0x34 > chunk.size) return false;
    const ByteSpan elf = chunk.sub(start, chunk.size - start);
    const u32 sectionTable = elf.read<u32>(0x20);
    const u16 entrySize = elf.read<u16>(0x2E), count = elf.read<u16>(0x30), namesIndex = elf.read<u16>(0x32);
    if (entrySize < 40 || count == 0 || (size_t)sectionTable + (size_t)entrySize * count > elf.size) return false;
    struct Section { u32 name, type, offset, size, link, info; };
    std::vector<Section> sections(count);
    for (u16 i = 0; i < count; ++i) {
        const size_t at = sectionTable + (size_t)i * entrySize;
        sections[i] = { elf.read<u32>(at), elf.read<u32>(at + 4), elf.read<u32>(at + 0x10), elf.read<u32>(at + 0x14),
                        elf.read<u32>(at + 0x18), elf.read<u32>(at + 0x1C) };
    }
    const auto name = [&](const Section& s) -> std::string {
        if (namesIndex >= count) return {};
        const ByteSpan names = elf.sub(sections[namesIndex].offset, sections[namesIndex].size);
        if (s.name >= names.size) return {};
        const char* text = (const char*)names.data + s.name;
        return std::string(text, strnlen(text, names.size - s.name));
    };
    int dataIndex = -1;
    for (u16 i = 0; i < count; ++i)
        if (sections[i].type == 1 && name(sections[i]) == ".data") dataIndex = i;
    if (dataIndex < 0) return false;
    image.data = elf.sub(sections[dataIndex].offset, sections[dataIndex].size);
    bank = 0xFFFFFFFFu;
    for (const Section& s : sections) {
        if (s.type == 9 && (int)s.info == dataIndex)
            for (u32 k = 0; k + 8 <= s.size; k += 8) image.pointers.insert(elf.read<u32>(s.offset + k));
        if (s.type == 2 && s.link < count) {
            const Section& strings = sections[s.link];
            for (u32 k = 0; k + 16 <= s.size; k += 16) {
                const u32 nameAt = elf.read<u32>(s.offset + k);
                if (nameAt >= strings.size) continue;
                const char* text = (const char*)elf.data + strings.offset + nameAt;
                if (std::string(text, strnlen(text, strings.size - nameAt)) == "__AnimationBank:::bank")
                    bank = elf.read<u32>(s.offset + k + 4);
            }
        }
    }
    return bank != 0xFFFFFFFFu && bank + 0x18 <= image.data.size;
}

// One track decoded: per frame, the 12 floats of a bone's scale, rotation and
// translation (+0 scale, +4 quaternion, +8 translation) for each of 4 bones.
constexpr int kBoneFloats = 12, kBones = 4, kPose = kBoneFloats * kBones;

struct Track {
    int frames = 0;
    int bone = -1;                 // the bone it moves
    std::vector<float> pose;       // frames * kPose
    float* Frame(int f) { return pose.data() + (size_t)f * kPose; }
};

void StartTrack(Track& track, int frames) {
    track.frames = std::max(frames, 1);
    track.pose.assign((size_t)track.frames * kPose, 0.0f);
    for (int f = 0; f < track.frames; ++f)
        for (int b = 0; b < kBones; ++b) {
            float* bone = track.Frame(f) + b * kBoneFloats;
            bone[0] = bone[1] = bone[2] = 1.0f;   // scale
            bone[7] = 1.0f;                        // w
        }
}

void Put(Track& track, int frame, u32 index, float value) {
    if (index >= (u32)kPose) return;
    track.Frame(frame)[index] = value;
    if (track.bone < 0) track.bone = (int)(index / kBoneFloats);
}

// Type 11, DELTAQUAT: quaternions as deltas from the frame before.
bool DeltaQuat(const Image& image, size_t at, Track& track) {
    const long long dcd = image.Ptr(at + 4);
    if (dcd < 0) return false;
    const int frames = image.U16(at + 8);
    const u16 dofs = image.U16((size_t)dcd);
    const u8 bits = image.U8((size_t)dcd + 2);
    const int bones = dofs / 4;
    if (bones == 0 || (bits != 4 && bits != 8 && bits != 16)) return false;
    std::vector<float> minimum(dofs), range(dofs), value(dofs);
    for (u16 i = 0; i < dofs; ++i) {
        minimum[i] = image.F32((size_t)dcd + 4 + 12 * i);
        range[i] = image.F32((size_t)dcd + 8 + 12 * i);
        value[i] = image.F32((size_t)dcd + 12 + 12 * i);
    }
    const size_t deltas = (size_t)dcd + 4 + 12 * (size_t)dofs;
    const size_t frameBytes = bits == 16 ? dofs * 2 : bits == 8 ? dofs : (dofs + 1) / 2;
    StartTrack(track, frames);
    for (int f = 0; f < track.frames; ++f) {
        if (f > 0) {
            const size_t base = deltas + (size_t)(f - 1) * frameBytes;
            for (u16 i = 0; i < dofs; ++i) {
                u32 q;
                if (bits == 16) q = image.U16(base + 2 * i);
                else if (bits == 8) q = image.U8(base + i);
                else { const u8 b = image.U8(base + i / 2); q = (i & 1) ? (b >> 4) : (b & 15); }
                value[i] += (float)q * range[i] + minimum[i];
            }
        }
        for (int b = 0; b < bones; ++b) {
            const u16 dof = image.U16(at + 0xA + 2 * b);
            float q[4] = { value[4 * b], value[4 * b + 1], value[4 * b + 2], value[4 * b + 3] };
            float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (length < 1e-6f) length = 1.0f;
            for (int c = 0; c < 4; ++c) Put(track, f, dof + c, q[c] / length);
        }
    }
    return true;
}

// Types 20 and 21, DELTAF3 / DELTAF1: 3 or 1 floats per DOF, a full value
// at the start of each bin of 2^n frames and byte deltas after it.
bool DeltaF(const Image& image, size_t at, int components, Track& track) {
    const long long dofTable = image.Ptr(at + 4);
    const int frames = image.U16(at + 0xC);
    const int bones = image.U16(at + 0xE);
    const int binPower = image.U8(at + 0x10);
    const int constants = image.U8(at + 0x11);
    if (binPower > 12 || components < 1 || components > 3) return false;
    const int binLength = 1 << binPower;
    const size_t infoSize = components == 3 ? 0x24 : 0xC;
    constexpr float K = 1.5259022e-5f;
    struct Info { float pm[3], pr[3], dm[3], dr[3]; };
    std::vector<Info> info((size_t)bones);
    for (int i = 0; i < bones; ++i) {
        const size_t b = at + 0x14 + infoSize * i;
        for (int c = 0; c < components; ++c) {
            const float pmin = image.F32(b + 4 * c), prng = image.F32(b + 4 * components + 4 * c);
            const float qmin = image.U16(b + 8 * components + 2 * c), qrng = image.U16(b + 10 * components + 2 * c);
            info[i].pm[c] = pmin;
            info[i].pr[c] = prng * K;
            info[i].dm[c] = 2.0f * (qmin * K) * prng - prng;
            info[i].dr[c] = 2.0f * (qrng * K) * prng * 0.003921569f;
        }
    }
    const size_t bins = at + 0x14 + infoSize * bones;
    const size_t frameDeltas = (size_t)bones * components;
    const size_t binSize = (2 * components * bones + (binLength - 1) * frameDeltas + 1) & ~(size_t)1;
    const int fullBins = binLength ? frames / binLength : 0, rest = binLength ? frames % binLength : 0;
    size_t end = bins + binSize * fullBins;
    if (rest > 0) end = (end + 2 * components * bones + (rest - 1) * frameDeltas + 1) & ~(size_t)1;
    if (bones == 0) end = (end + 1) & ~(size_t)1;
    std::vector<u16> constantIndex((size_t)constants);
    for (int i = 0; i < constants; ++i) constantIndex[i] = image.U16(end + 2 * i);
    const size_t constantValues = (end + 2 * constants + 3) & ~(size_t)3;

    StartTrack(track, frames);
    for (int f = 0; f < track.frames; ++f) {
        const int bin = f / binLength, r = f % binLength;
        const size_t base = bins + (size_t)bin * binSize;
        for (int i = 0; i < bones && dofTable >= 0; ++i) {
            const u16 dof = image.U16((size_t)dofTable + 2 * i);
            for (int c = 0; c < components; ++c) {
                float v = image.U16(base + 2 * components * i + 2 * c) * info[i].pr[c] + info[i].pm[c];
                for (int j = 0; j < r; ++j)
                    v += image.U8(base + 2 * components * bones + j * frameDeltas + components * i + c) * info[i].dr[c] +
                         info[i].dm[c];
                Put(track, f, dof + c, v);
            }
        }
        for (int k = 0; k < constants; ++k)
            for (int c = 0; c < components; ++c)
                Put(track, f, constantIndex[k] + c, image.F32(constantValues + 4 * (components * k + c)));
    }
    return true;
}

// Type 0, RAWPOSE: plain floats, every frame (the trams).
bool RawPose(const Image& image, size_t at, int slot, Track& track) {
    const u32 components = image.U32(at + 8), frames = image.U32(at + 0xC);
    if (components == 0 || components > 4 || frames == 0 || frames > 100000) return false;
    if (at + 0x20 + (size_t)frames * components * 4 > image.data.size) return false;
    StartTrack(track, (int)frames);
    for (u32 f = 0; f < frames; ++f)
        for (u32 c = 0; c < components; ++c)
            Put(track, (int)f, kBoneFloats + slot + c, image.F32(at + 0x20 + 4 * (f * components + c)));
    return true;
}

struct BankAnimation {
    const Image* image = nullptr;
    size_t at = 0;
};

// A COMPOUND (type 15) holding one channel; `slot` says which part of the
// bone the name ends in (_s 0, _q 4, _t 8).
bool DecodeTrack(const BankAnimation& animation, int slot, Track& track, float& fps) {
    const Image& image = *animation.image;
    size_t at = animation.at;
    if (image.U16(at) == 15) {
        const long long attributes = image.Ptr(at + 4);
        if (attributes >= 0) {
            const u32 count = image.U32((size_t)attributes);
            for (u32 k = 0; k < count && k < 16; ++k)
                if (image.U16((size_t)attributes + 4 + 8 * k) == 1) fps = image.U8((size_t)attributes + 8 + 8 * k);
        }
        if (image.U16(at + 8) == 0) return false;
        const long long channel = image.Ptr(at + 0xC);
        if (channel < 0) return false;
        at = (size_t)channel;
    }
    switch (image.U16(at)) {
        case 11: return DeltaQuat(image, at, track);
        case 20: return DeltaF(image, at, 3, track);
        case 21: return DeltaF(image, at, 1, track);
        case 0:  return RawPose(image, at, slot, track);
        default: return false;   // RAWLINEAR scale of the trams: always 1
    }
}

Mat4 Compose(const Vec3& scale, const Vec4& q, const Vec3& t) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float rotation[3][3] = {
        { 1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y) },
        { 2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x) },
        { 2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y) },
    };
    const float s[3] = { scale.x, scale.y, scale.z };
    Mat4 m = Mat4::Identity();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) m.m[r][c] = rotation[r][c] * s[r];
    m.m[3][0] = t.x;
    m.m[3][1] = t.y;
    m.m[3][2] = t.z;
    return m;
}

float Length(const float* row) { return std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]); }

} // namespace

void WorldAnimations::Parse(ByteSpan layout) {
    Clear();
    struct Entity { u32 name, parent, model, tree, animation; u8 content; Mat4 rest; };
    std::vector<Entity> entities;
    std::vector<std::unique_ptr<Image>> images;
    std::unordered_map<std::string, BankAnimation> byName;
    std::unordered_map<u32, std::string> nameOf;   // hash of "<animation>" -> name

    ChunkReader reader(layout, 0);
    Chunk chunk;
    std::vector<ByteSpan> instanceChunks;
    while (reader.Next(chunk)) {
        if (chunk.id == kEntities) {
            const ByteSpan data = SkipFiller(chunk.data);
            for (size_t at = 0; at + kEntitySize <= data.size; at += kEntitySize) {
                Entity e;
                e.name = data.read<u32>(at + 4);
                e.parent = data.read<u32>(at + 8);
                e.model = data.read<u32>(at + 0xC);
                e.tree = data.read<u32>(at + 0x20);
                e.animation = data.read<u32>(at + 0x24);
                e.content = data.read<u8>(at + 0x28);
                e.rest = ReadMatrix(data, at + 0x30);
                entities.push_back(e);
            }
        } else if (chunk.id == kInstances) {
            instanceChunks.push_back(SkipFiller(chunk.data));
        } else if (chunk.id == kBank) {
            auto image = std::make_unique<Image>();
            u32 bank = 0;
            if (!ReadElf(chunk.data, *image, bank)) continue;
            const u32 count = image->U32(bank + 4);
            const long long animations = image->Ptr(bank + 0x10), names = image->Ptr(bank + 0x14);
            if (animations < 0 || names < 0) continue;
            for (u32 i = 0; i < count && i < 256; ++i) {
                const long long nameAt = image->Ptr((size_t)names + 4 * i), at = image->Ptr((size_t)animations + 4 * i);
                if (nameAt < 0 || at < 0) continue;
                const std::string name = image->Text((size_t)nameAt);
                byName[name] = { image.get(), (size_t)at };
                if (name.size() > 2) {
                    const std::string base = name.substr(0, name.size() - 2);
                    nameOf[StringHash(base)] = base;
                }
            }
            images.push_back(std::move(image));
        }
    }

    // Trees, parents first.
    std::unordered_map<u32, size_t> treeIndex;
    size_t decoded = 0, failed = 0;
    for (const Entity& root : entities) {
        if (root.name != root.tree || treeIndex.count(root.tree)) continue;
        WorldAnimationTree tree;
        tree.hash = root.tree;
        std::vector<const Entity*> pending;
        for (const Entity& e : entities)
            if (e.tree == root.tree) pending.push_back(&e);
        std::unordered_map<u32, int> partIndex;
        for (int guard = 0; !pending.empty() && guard < 64; ++guard) {
            for (size_t k = 0; k < pending.size();) {
                const Entity& e = *pending[k];
                const bool isRoot = e.name == e.tree;
                const auto parent = partIndex.find(e.parent);
                if (!isRoot && parent == partIndex.end()) { ++k; continue; }
                WorldAnimationPart part;
                part.name = e.name;
                part.model = isRoot ? 0 : e.model;
                part.parent = isRoot ? -1 : parent->second;
                part.rest = e.rest;
                const auto animation = nameOf.find(e.animation);
                if (e.animation && e.content && animation != nameOf.end()) {
                    part.animation = animation->second;
                    const struct { const char* suffix; int slot; u8 bit; } channels[] = {
                        { "_t", 8, 1 }, { "_q", 4, 2 }, { "_s", 0, 4 } };
                    for (const auto& channel : channels) {
                        if (!(e.content & channel.bit)) continue;
                        const auto found = byName.find(part.animation + channel.suffix);
                        if (found == byName.end()) continue;
                        Track track;
                        if (!DecodeTrack(found->second, channel.slot, track, part.fps)) { ++failed; continue; }
                        ++decoded;
                        const int bone = track.bone < 0 ? 1 : track.bone;
                        for (int f = 0; f < track.frames; ++f) {
                            const float* v = track.Frame(f) + bone * kBoneFloats + channel.slot;
                            if (channel.slot == 8) part.translation.push_back({ v[0], v[1], v[2] });
                            else if (channel.slot == 4) part.rotation.push_back({ v[0], v[1], v[2], v[3] });
                            else part.scale.push_back({ v[0], v[1], v[2] });
                        }
                        part.frames = std::max(part.frames, track.frames);
                    }
                }
                partIndex[e.name] = (int)tree.parts.size();
                tree.parts.push_back(std::move(part));
                pending.erase(pending.begin() + (long long)k);
            }
        }
        treeIndex[tree.hash] = trees_.size();
        trees_.push_back(std::move(tree));
    }

    for (const ByteSpan& data : instanceChunks)
        for (size_t at = 0; at + kInstanceSize <= data.size; at += kInstanceSize) {
            const auto tree = treeIndex.find(data.read<u32>(at + 4));
            if (tree == treeIndex.end()) continue;
            WorldAnimationPlacement placement;
            placement.tree = tree->second;
            placement.section = data.read<i32>(at + 8);
            placement.speed = data.read<float>(at + 0x10);
            if (!(placement.speed > 0.0f)) placement.speed = 1.0f;
            placement.loopDelay = std::max(0.0f, data.read<float>(at + 0x18));
            placement.matrix = ReadMatrix(data, at + 0x40);
            placements_.push_back(placement);
        }
    LOG_INFO("world animations: %zu trees, %zu placements, %zu tracks decoded, %zu not", trees_.size(),
             placements_.size(), decoded, failed);
}

void WorldAnimations::Pose(const WorldAnimationPlacement& placement, double seconds, std::vector<Mat4>& out) const {
    out.clear();
    if (placement.tree >= trees_.size()) return;
    const WorldAnimationTree& tree = trees_[placement.tree];
    double length = 0.0;
    for (const WorldAnimationPart& part : tree.parts)
        if (part.frames > 1 && part.fps > 0) length = std::max(length, (double)(part.frames - 1) / part.fps);
    const double period = length + placement.loopDelay;
    const double t = period > 0 ? std::fmod(seconds * placement.speed, period) : 0.0;

    out.resize(tree.parts.size());
    for (size_t i = 0; i < tree.parts.size(); ++i) {
        const WorldAnimationPart& part = tree.parts[i];
        Mat4 local = part.rest;
        if (part.frames > 0) {
            const double frame = std::min(t * part.fps, (double)(part.frames - 1));
            const int a = (int)frame;
            const float blend = (float)(frame - a);
            const auto at = [&](const auto& list, int k) { return list[std::min<size_t>((size_t)k, list.size() - 1)]; };
            // Absent tracks keep what the matrix at rest says.
            const float* rows[3] = { part.rest.m[0], part.rest.m[1], part.rest.m[2] };
            Vec3 scale{ Length(rows[0]), Length(rows[1]), Length(rows[2]) };
            Vec3 translation{ part.rest.m[3][0], part.rest.m[3][1], part.rest.m[3][2] };
            if (!part.translation.empty()) {
                const Vec3 p = at(part.translation, a), q = at(part.translation, a + 1);
                translation = p + (q - p) * blend;
            }
            if (!part.scale.empty()) {
                const Vec3 p = at(part.scale, a), q = at(part.scale, a + 1);
                scale = p + (q - p) * blend;
            }
            if (!part.rotation.empty()) {
                Vec4 p = at(part.rotation, a), q = at(part.rotation, a + 1);
                if (p.x * q.x + p.y * q.y + p.z * q.z + p.w * q.w < 0) q = { -q.x, -q.y, -q.z, -q.w };
                Vec4 r{ p.x + (q.x - p.x) * blend, p.y + (q.y - p.y) * blend, p.z + (q.z - p.z) * blend,
                        p.w + (q.w - p.w) * blend };
                float n = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
                if (n < 1e-6f) n = 1.0f;
                local = Compose(scale, { r.x / n, r.y / n, r.z / n, r.w / n }, translation);
            } else {
                for (int r = 0; r < 3; ++r) {
                    const float len = Length(rows[r]);
                    const float s = (r == 0 ? scale.x : r == 1 ? scale.y : scale.z) / (len > 1e-6f ? len : 1.0f);
                    for (int c = 0; c < 3; ++c) local.m[r][c] = part.rest.m[r][c] * s;
                }
                local.m[3][0] = translation.x;
                local.m[3][1] = translation.y;
                local.m[3][2] = translation.z;
            }
        }
        out[i] = part.parent >= 0 && (size_t)part.parent < i ? local * out[(size_t)part.parent] : local * placement.matrix;
    }
}

} // namespace nfs
