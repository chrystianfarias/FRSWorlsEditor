#include "nfs/SceneryEdit.h"

#include "nfs/ChunkReader.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nfs {

namespace {

constexpr u32 kSceneryPack      = 0x80034100;
constexpr u32 kSceneryHeader    = 0x00034101;
constexpr u32 kSceneryInfos     = 0x00034102;
constexpr u32 kSceneryInstances = 0x00034103;
constexpr u32 kSceneryTree      = 0x00034104;

constexpr size_t kHeaderNumber = 0x0C;

constexpr size_t kInfoSize        = 68;
constexpr size_t kInfoName        = 0x00;
constexpr size_t kInfoHash        = 0x20;
constexpr size_t kInfoHashAgain   = 0x24;
constexpr size_t kInfoTriangles   = 0x2C;   // u16, and again at 0x2E
constexpr size_t kInfoRadius      = 0x3C;

constexpr size_t kInstanceSize        = 64;
constexpr size_t kInstBoundsMin       = 0x00;
constexpr size_t kInstBoundsMax       = 0x0C;
constexpr size_t kInstInfoIndex       = 0x18;
constexpr size_t kInstFlags           = 0x1A;
constexpr size_t kInstTranslation     = 0x20;
constexpr size_t kInstRotation        = 0x2C;

constexpr float kRotationScale = 8192.0f;

// A tree node: the box of everything under it, a child count and up to five
// children. A child below 0x8000 is an instance; one above is the node at
// 0x10000 - child. Node 0 is the root.
constexpr size_t kNodeSize     = 36;
constexpr size_t kNodeChildren = 5;

struct Node {
    Aabb bounds;
    std::vector<u16> children;
};

bool IsNodeRef(u16 child) { return child >= 0x8000; }
u16  NodeRef(size_t index) { return (u16)(0x10000 - index); }
size_t NodeIndex(u16 child) { return 0x10000 - (size_t)child; }

// The pieces of a scenery chunk, as spans into the source.
struct Parts {
    struct Child { u32 id; ByteSpan raw; ByteSpan data; };
    std::vector<Child> children;
    ByteSpan header, infos, instances, tree;
};

bool Split(ByteSpan chunk, u64 fileOffset, Parts& out) {
    if (chunk.size < 8 || chunk.read<u32>(0) != kSceneryPack) return false;
    Chunk container;
    container.id = kSceneryPack;
    container.headerOffset = fileOffset;
    container.raw = chunk.sub(8, chunk.read<u32>(4));
    container.data = container.raw;

    ChunkReader reader = ChunkReader::Children(container);
    reader.SetSkipZeroPadding(false);
    Chunk child;
    while (reader.Next(child)) {
        out.children.push_back({ child.id, child.raw, child.data });
        switch (child.id) {
            // Only the instance records carry alignment filler. The other
            // tables start straight after their header, and one shipped tree
            // legitimately begins with a 0x11 byte that a filler check would
            // eat, so they are taken raw.
            case kSceneryHeader:    out.header    = child.raw;  break;
            case kSceneryInfos:     out.infos     = child.raw;  break;
            case kSceneryInstances: out.instances = child.data; break;
            case kSceneryTree:      out.tree      = child.raw;  break;
            default: break;
        }
    }
    return !out.infos.empty() || !out.instances.empty();
}

void Put16(std::vector<u8>& out, size_t at, u16 v) { std::memcpy(out.data() + at, &v, 2); }
void Put32(std::vector<u8>& out, size_t at, u32 v) { std::memcpy(out.data() + at, &v, 4); }
void PutF(std::vector<u8>& out, size_t at, float v) { std::memcpy(out.data() + at, &v, 4); }
void PutVec3(std::vector<u8>& out, size_t at, const Vec3& v) {
    PutF(out, at, v.x); PutF(out, at + 4, v.y); PutF(out, at + 8, v.z);
}

Vec3 ReadVec3(ByteSpan span, size_t at) {
    return { span.read<float>(at), span.read<float>(at + 4), span.read<float>(at + 8) };
}

Aabb InstanceBounds(ByteSpan instances, size_t index) {
    Aabb box;
    box.min = ReadVec3(instances, index * kInstanceSize + kInstBoundsMin);
    box.max = ReadVec3(instances, index * kInstanceSize + kInstBoundsMax);
    return box;
}

float Grow(const Aabb& box, const Aabb& add) {
    Aabb both = box;
    both.Add(add);
    const Vec3 a = box.max - box.min, b = both.max - both.min;
    return b.x * b.y * b.z - a.x * a.y * a.z + (b.x + b.y + b.z) - (a.x + a.y + a.z);
}

// Puts one record into the tree, changing as little of it as it can.
void Insert(std::vector<Node>& nodes, u16 instance, const Aabb& box,
            const std::vector<Aabb>& instanceBoxes) {
    if (nodes.empty()) {
        nodes.push_back({ box, { instance } });
        return;
    }

    std::vector<size_t> path;
    size_t at = 0;
    while (true) {
        path.push_back(at);
        Node& node = nodes[at];
        if (node.children.size() < kNodeChildren) {
            node.children.push_back(instance);
            break;
        }

        // Down the child node that grows least; failing any, the instance
        // child that grows least is paired with the new one under a new node.
        size_t bestNode = SIZE_MAX, bestInstance = SIZE_MAX;
        float nodeCost = 1e30f, instanceCost = 1e30f;
        for (size_t k = 0; k < node.children.size(); ++k) {
            const u16 child = node.children[k];
            if (IsNodeRef(child)) {
                const float cost = Grow(nodes[NodeIndex(child)].bounds, box);
                if (cost < nodeCost) { nodeCost = cost; bestNode = NodeIndex(child); }
            } else {
                const float cost = Grow(instanceBoxes[child], box);
                if (cost < instanceCost) { instanceCost = cost; bestInstance = k; }
            }
        }

        if (bestNode != SIZE_MAX) { at = bestNode; continue; }

        const u16 paired = node.children[bestInstance];
        Node pair;
        pair.bounds = instanceBoxes[paired];
        pair.bounds.Add(box);
        pair.children = { paired, instance };
        nodes.push_back(pair);
        nodes[at].children[bestInstance] = NodeRef(nodes.size() - 1);
        break;
    }

    for (size_t index : path) nodes[index].bounds.Add(box);
}

} // namespace

Aabb PlacedInstance::WorldBounds() const {
    return Transform(modelBounds, transform);
}

bool ReadSceneryChunk(ByteSpan chunk, u64 fileOffset, SceneryChunkInfo& out) {
    Parts parts;
    if (!Split(chunk, fileOffset, parts)) return false;
    out.number = parts.header.read<u32>(kHeaderNumber);
    out.infoCount = parts.infos.size / kInfoSize;
    out.instanceCount = parts.instances.size / kInstanceSize;
    return true;
}

bool ReadSceneryPlacements(ByteSpan chunk, u64 fileOffset, size_t first,
                           std::vector<PlacedInstance>& out) {
    Parts parts;
    if (!Split(chunk, fileOffset, parts)) return false;

    const size_t count = parts.instances.size / kInstanceSize;
    for (size_t i = first; i < count; ++i) {
        const ByteSpan record = parts.instances.sub(i * kInstanceSize, kInstanceSize);
        const u16 infoIndex = record.read<u16>(kInstInfoIndex);
        const ByteSpan info = parts.infos.sub((size_t)infoIndex * kInfoSize, kInfoSize);

        PlacedInstance placed;
        placed.modelHash = info.read<u32>(kInfoHash);
        placed.modelName.assign((const char*)info.data,
                                ::strnlen((const char*)info.data, std::min<size_t>(info.size, 32)));
        placed.flags = record.read<u16>(kInstFlags);
        placed.infoTemplate.assign(info.data, info.data + info.size);

        placed.transform = Mat4::Identity();
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                placed.transform.m[row][col] =
                    record.read<i16>(kInstRotation + (size_t)(row * 3 + col) * 2) / kRotationScale;
        const Vec3 t = ReadVec3(record, kInstTranslation);
        placed.transform.m[3][0] = t.x;
        placed.transform.m[3][1] = t.y;
        placed.transform.m[3][2] = t.z;
        out.push_back(std::move(placed));
    }
    return true;
}

// Each node's box made the union of its children's again, leaves first.
void RecomputeBounds(std::vector<Node>& nodes, const std::vector<Aabb>& boxes, size_t at) {
    Aabb box;
    for (const u16 child : nodes[at].children) {
        if (IsNodeRef(child)) {
            RecomputeBounds(nodes, boxes, NodeIndex(child));
            box.Add(nodes[NodeIndex(child)].bounds);
        } else if (child < boxes.size()) {
            box.Add(boxes[child]);
        }
    }
    nodes[at].bounds = box;
}

bool ReadSceneryRecords(ByteSpan chunk, u64 fileOffset, std::vector<std::vector<u8>>& out) {
    Parts parts;
    if (!Split(chunk, fileOffset, parts)) return false;
    for (size_t i = 0; i < parts.instances.size / kInstanceSize; ++i) {
        const ByteSpan record = parts.instances.sub(i * kInstanceSize, kInstanceSize);
        out.emplace_back(record.data, record.data + record.size);
    }
    return true;
}

std::string SceneryRecordModel(ByteSpan chunk, u64 fileOffset, size_t index) {
    Parts parts;
    if (!Split(chunk, fileOffset, parts)) return {};
    const u16 info = parts.instances.read<u16>(index * kInstanceSize + kInstInfoIndex);
    const ByteSpan entry = parts.infos.sub((size_t)info * kInfoSize, kInfoSize);
    if (entry.size < 32) return {};
    return std::string((const char*)entry.data, ::strnlen((const char*)entry.data, 32));
}

std::vector<u8> DeletedRecord(const std::vector<u8>& record) {
    std::vector<u8> out = record;
    if (out.size() >= kInstRotation + 18) std::memset(out.data() + kInstRotation, 0, 18);
    return out;
}

bool IsDeletedRecord(const std::vector<u8>& record) {
    if (record.size() < kInstRotation + 18) return false;
    for (size_t i = 0; i < 18; ++i)
        if (record[kInstRotation + i] != 0) return false;
    return true;
}

bool AppendScenery(ByteSpan shipped, u64 fileOffset, const std::vector<PlacedInstance>& added,
                   std::vector<u8>& out, std::wstring& error,
                   const std::vector<ReplacedRecord>& replaced) {
    Parts parts;
    if (!Split(shipped, fileOffset, parts) || parts.header.empty()) {
        error = L"That is not a scenery chunk.";
        return false;
    }

    // --- the model table ------------------------------------------------------
    std::vector<u8> infos(parts.infos.data, parts.infos.data + parts.infos.size);
    std::vector<u16> infoFor(added.size());
    for (size_t k = 0; k < added.size(); ++k) {
        const PlacedInstance& placed = added[k];
        const size_t count = infos.size() / kInfoSize;
        size_t found = count;
        for (size_t i = 0; i < count && found == count; ++i) {
            u32 hash;
            std::memcpy(&hash, infos.data() + i * kInfoSize + kInfoHash, 4);
            if (hash == placed.modelHash) found = i;
        }

        if (found == count) {
            if (count >= 0xFFFF) { error = L"The section lists too many models already."; return false; }
            std::vector<u8> entry(kInfoSize, 0);
            if (placed.infoTemplate.size() == kInfoSize) {
                entry = placed.infoTemplate;
            } else {
                std::memcpy(entry.data() + kInfoName, placed.modelName.data(),
                            std::min<size_t>(placed.modelName.size(), 31));
                const u16 triangles = (u16)std::min<u32>(placed.triangles, 0xFFFF);
                Put16(entry, kInfoTriangles, triangles);
                Put16(entry, kInfoTriangles + 2, triangles);
                PutF(entry, kInfoRadius, placed.radius);
            }
            Put32(entry, kInfoHash, placed.modelHash);
            Put32(entry, kInfoHashAgain, placed.modelHash);
            infos.insert(infos.end(), entry.begin(), entry.end());
        }
        infoFor[k] = (u16)found;
    }

    // --- the records ----------------------------------------------------------
    const size_t shippedCount = parts.instances.size / kInstanceSize;
    if (shippedCount + added.size() >= 0x8000) {
        error = L"The section places too many instances for its tree.";
        return false;
    }

    std::vector<u8> instances(parts.instances.data, parts.instances.data + parts.instances.size);
    bool boxesMoved = false;
    for (const ReplacedRecord& swap : replaced) {
        if (swap.index >= shippedCount || swap.record.size() != kInstanceSize) {
            error = L"An edited record does not fit the section's scenery.";
            return false;
        }
        const ByteSpan before = parts.instances.sub(swap.index * kInstanceSize, kInstanceSize);
        if (std::memcmp(before.data, swap.record.data(), 24) != 0) boxesMoved = true;
        std::memcpy(instances.data() + swap.index * kInstanceSize, swap.record.data(), kInstanceSize);
    }
    const ByteSpan edited{ instances.data(), instances.size() };
    std::vector<Aabb> boxes;
    for (size_t i = 0; i < shippedCount; ++i) boxes.push_back(InstanceBounds(edited, i));

    for (size_t k = 0; k < added.size(); ++k) {
        const PlacedInstance& placed = added[k];
        std::vector<u8> record(kInstanceSize, 0);
        const Aabb box = placed.WorldBounds();
        PutVec3(record, kInstBoundsMin, box.min);
        PutVec3(record, kInstBoundsMax, box.max);
        Put16(record, kInstInfoIndex, infoFor[k]);
        Put16(record, kInstFlags, placed.flags);
        PutVec3(record, kInstTranslation,
                { placed.transform.m[3][0], placed.transform.m[3][1], placed.transform.m[3][2] });
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                const long fixed = std::lround(placed.transform.m[row][col] * kRotationScale);
                if (fixed < -32768 || fixed > 32767) {
                    error = L"A placement is scaled by more than four; the file cannot store it.";
                    return false;
                }
                Put16(record, kInstRotation + (size_t)(row * 3 + col) * 2, (u16)(i16)fixed);
            }
        }
        instances.insert(instances.end(), record.begin(), record.end());
        boxes.push_back(box);
    }

    // --- the tree -------------------------------------------------------------
    std::vector<Node> nodes;
    for (size_t n = 0; n < parts.tree.size / kNodeSize; ++n) {
        const ByteSpan raw = parts.tree.sub(n * kNodeSize, kNodeSize);
        Node node;
        node.bounds.min = ReadVec3(raw, 0);
        node.bounds.max = ReadVec3(raw, 12);
        const u16 count = std::min<u16>(raw.read<u16>(24), (u16)kNodeChildren);
        for (u16 c = 0; c < count; ++c) node.children.push_back(raw.read<u16>(26 + (size_t)c * 2));
        nodes.push_back(node);
    }
    if (boxesMoved && !nodes.empty()) RecomputeBounds(nodes, boxes, 0);
    for (size_t k = 0; k < added.size(); ++k)
        Insert(nodes, (u16)(shippedCount + k), boxes[shippedCount + k], boxes);

    std::vector<u8> tree;
    if (added.empty() && !boxesMoved) {
        tree.assign(parts.tree.data, parts.tree.data + parts.tree.size);
    } else {
        tree.assign(nodes.size() * kNodeSize, 0);
        for (size_t n = 0; n < nodes.size(); ++n) {
            PutVec3(tree, n * kNodeSize, nodes[n].bounds.min);
            PutVec3(tree, n * kNodeSize + 12, nodes[n].bounds.max);
            Put16(tree, n * kNodeSize + 24, (u16)nodes[n].children.size());
            for (size_t c = 0; c < nodes[n].children.size(); ++c)
                Put16(tree, n * kNodeSize + 26 + c * 2, nodes[n].children[c]);
        }
    }

    // --- the chunk ------------------------------------------------------------
    out.clear();
    out.resize(8);
    Put32(out, 0, kSceneryPack);

    for (const Parts::Child& child : parts.children) {
        const std::vector<u8>* replacement = nullptr;
        switch (child.id) {
            case kSceneryInfos:     replacement = &infos;     break;
            case kSceneryInstances: replacement = &instances; break;
            case kSceneryTree:      replacement = &tree;      break;
            default: break;
        }

        const size_t headerAt = out.size();
        out.resize(headerAt + 8);
        Put32(out, headerAt, child.id);

        if (!replacement) {
            out.insert(out.end(), child.raw.data, child.raw.data + child.raw.size);
        } else {
            // Only the instance records are aligned in any shipped pack: to
            // sixteen bytes by their position in the file. The rest start
            // straight after their header.
            if (child.id == kSceneryInstances) {
                const u64 payloadAt = fileOffset + out.size();
                const size_t filler = (size_t)((16 - payloadAt % 16) % 16);
                out.insert(out.end(), filler, 0x11);
            }
            out.insert(out.end(), replacement->begin(), replacement->end());
        }
        Put32(out, headerAt + 4, (u32)(out.size() - headerAt - 8));
    }
    Put32(out, 4, (u32)(out.size() - 8));
    return true;
}

bool ValidateScenery(ByteSpan chunk, u64 fileOffset, std::string& why) {
    Parts parts;
    if (!Split(chunk, fileOffset, parts)) { why = "not a scenery chunk"; return false; }

    const size_t count = parts.instances.size / kInstanceSize;
    const size_t infos = parts.infos.size / kInfoSize;
    const size_t nodes = parts.tree.size / kNodeSize;
    if (parts.instances.size % kInstanceSize || parts.infos.size % kInfoSize ||
        parts.tree.size % kNodeSize) { why = "a table is not a whole number of entries"; return false; }

    std::vector<Aabb> boxes;
    for (size_t i = 0; i < count; ++i) {
        boxes.push_back(InstanceBounds(parts.instances, i));
        if (parts.instances.read<u16>(i * kInstanceSize + kInstInfoIndex) >= infos) {
            why = "an instance names a model the section does not list";
            return false;
        }
    }
    if (count == 0) return true;
    if (nodes == 0) { why = "instances but no tree"; return false; }

    std::vector<int> seenInstance(count, 0), seenNode(nodes, 0);
    const float slack = 1e-3f;
    for (size_t n = 0; n < nodes; ++n) {
        const ByteSpan raw = parts.tree.sub(n * kNodeSize, kNodeSize);
        Aabb box;
        box.min = ReadVec3(raw, 0);
        box.max = ReadVec3(raw, 12);
        const u16 children = raw.read<u16>(24);
        if (children == 0 || children > kNodeChildren) { why = "a node has no or too many children"; return false; }
        for (u16 c = 0; c < children; ++c) {
            const u16 child = raw.read<u16>(26 + (size_t)c * 2);
            Aabb inner;
            if (IsNodeRef(child)) {
                const size_t k = NodeIndex(child);
                if (k == 0 || k >= nodes) { why = "a node points outside the tree"; return false; }
                ++seenNode[k];
                const ByteSpan other = parts.tree.sub(k * kNodeSize, kNodeSize);
                inner.min = ReadVec3(other, 0);
                inner.max = ReadVec3(other, 12);
            } else {
                if (child >= count) { why = "a node points past the instances"; return false; }
                ++seenInstance[child];
                inner = boxes[child];
            }
            if (inner.min.x < box.min.x - slack || inner.min.y < box.min.y - slack ||
                inner.min.z < box.min.z - slack || inner.max.x > box.max.x + slack ||
                inner.max.y > box.max.y + slack || inner.max.z > box.max.z + slack) {
                why = "a node's box does not hold one of its children";
                return false;
            }
        }
    }
    for (size_t i = 0; i < count; ++i)
        if (seenInstance[i] != 1) { why = "an instance is not in the tree exactly once"; return false; }
    for (size_t n = 1; n < nodes; ++n)
        if (seenNode[n] != 1) { why = "a node is not referenced exactly once"; return false; }
    return true;
}

Mat4 ComposePlacement(const Vec3& position, float heading, float pitch, float roll, float scale) {
    const float h = Radians(heading), p = Radians(pitch), r = Radians(roll);
    const float ch = std::cos(h), sh = std::sin(h);
    const float cp = std::cos(p), sp = std::sin(p);
    const float cr = std::cos(r), sr = std::sin(r);

    // Row vectors, so the product reads in the order it applies: roll about
    // x, then pitch about y, then heading about z.
    Mat4 m = Mat4::Identity();
    m.m[0][0] = cp * ch;                 m.m[0][1] = cp * sh;                 m.m[0][2] = -sp;
    m.m[1][0] = -cr * sh + sr * sp * ch; m.m[1][1] = cr * ch + sr * sp * sh;  m.m[1][2] = sr * cp;
    m.m[2][0] = sr * sh + cr * sp * ch;  m.m[2][1] = -sr * ch + cr * sp * sh; m.m[2][2] = cr * cp;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) m.m[row][col] *= scale;
    m.m[3][0] = position.x;
    m.m[3][1] = position.y;
    m.m[3][2] = position.z;
    return m;
}

void DecomposePlacement(const Mat4& m, Vec3& position, float& heading, float& pitch,
                        float& roll, float& scale) {
    position = { m.m[3][0], m.m[3][1], m.m[3][2] };
    scale = std::sqrt(m.m[0][0] * m.m[0][0] + m.m[0][1] * m.m[0][1] + m.m[0][2] * m.m[0][2]);
    const float s = scale > 1e-6f ? scale : 1.0f;
    const float sp = std::clamp(-m.m[0][2] / s, -1.0f, 1.0f);
    pitch   = Degrees(std::asin(sp));
    heading = Degrees(std::atan2(m.m[0][1], m.m[0][0]));
    roll    = Degrees(std::atan2(m.m[1][2], m.m[2][2]));
}

} // namespace nfs
