#include "nfs/Routes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace nfs::routes {

namespace {

constexpr u32 kChunkRoutes = 0x34121, kChunkSignposts = 0x34122, kChunkIntersections = 0x34123;
constexpr size_t kIntersection = 0x114;

i8 ClampI8(float v) { return (i8)std::clamp((int)std::lround(v), -128, 127); }

void Box(const Route& r, float out[4]) {
    out[0] = out[1] = 1e30f;
    out[2] = out[3] = -1e30f;
    for (const Node& n : r.nodes) {
        const float dx = n.dirX / 32767.0f, dy = n.dirY / 32767.0f;
        const float hl = n.halfLeft256 / 256.0f, hr = n.halfRight256 / 256.0f;
        const float xs[2] = { n.x + dy * hr, n.x - dy * hl };
        const float ys[2] = { n.y - dx * hr, n.y + dx * hl };
        for (int k = 0; k < 2; ++k) {
            out[0] = std::min(out[0], xs[k]);
            out[1] = std::min(out[1], ys[k]);
            out[2] = std::max(out[2], xs[k]);
            out[3] = std::max(out[3], ys[k]);
        }
    }
}

} // namespace

u32 SignpostFlags(int a, int b, bool backward, bool flip) {
    return (u32)a | (u32)b << 4 | (u32)a << 8 | (u32)b << 12 | (u32)backward << 18 | (u32)flip << 23;
}

u16 Intersection::Route(int k) const { u16 v; std::memcpy(&v, raw.data() + 4 + 2 * k, 2); return v; }
u16 Intersection::NodeIndex(int k) const { u16 v; std::memcpy(&v, raw.data() + 0x0A + 2 * k, 2); return v; }
void Intersection::SetNodeIndex(int k, u16 node) { std::memcpy(raw.data() + 0x0A + 2 * k, &node, 2); }

bool Graph::Parse(const std::vector<u8>& bytes, std::wstring& error) {
    routes_.clear();
    signposts_.clear();
    intersections_.clear();
    order_.clear();
    extra_.clear();
    size_t off = 0;
    while (off + 8 <= bytes.size()) {
        u32 id = 0, size = 0;
        std::memcpy(&id, bytes.data() + off, 4);
        std::memcpy(&size, bytes.data() + off + 4, 4);
        if (off + 8 + size > bytes.size()) { error = L"a chunk runs past the end of the file"; return false; }
        const u8* data = bytes.data() + off + 8;
        order_.push_back(id);
        if (id == kChunkRoutes) {
            size_t p = 0;
            while (p < size) {
                if (p + sizeof(Header) > size) { error = L"a route header runs past its chunk"; return false; }
                Route r;
                std::memcpy(&r.header, data + p, sizeof(Header));
                p += sizeof(Header);
                if (r.header.count < 0 || p + (size_t)r.header.count * sizeof(Node) > size) {
                    error = L"a route's nodes run past their chunk";
                    return false;
                }
                r.nodes.resize((size_t)r.header.count);
                if (r.header.count) std::memcpy(r.nodes.data(), data + p, (size_t)r.header.count * sizeof(Node));
                p += (size_t)r.header.count * sizeof(Node);
                routes_.push_back(std::move(r));
            }
        } else if (id == kChunkSignposts) {
            if (size < 4 || (size - 4) % sizeof(Signpost)) { error = L"the signpost chunk has an odd size"; return false; }
            std::memcpy(&signHeader_, data, 4);
            signposts_.resize((size - 4) / sizeof(Signpost));
            if (!signposts_.empty()) std::memcpy(signposts_.data(), data + 4, size - 4);
        } else if (id == kChunkIntersections) {
            if (size % kIntersection) { error = L"the intersection chunk has an odd size"; return false; }
            for (size_t p = 0; p < size; p += kIntersection) {
                Intersection it;
                it.raw.assign(data + p, data + p + kIntersection);
                intersections_.push_back(std::move(it));
            }
        } else {
            extra_[id].assign(data, data + size);
        }
        off += 8 + size;
    }
    if (off != bytes.size()) { error = L"stray bytes at the end of the file"; return false; }
    return true;
}

std::vector<u8> Graph::Write() const {
    std::vector<u8> out;
    const auto put = [&](const void* p, size_t n) {
        const u8* b = (const u8*)p;
        out.insert(out.end(), b, b + n);
    };
    for (u32 id : order_) {
        std::vector<u8> data;
        if (id == kChunkRoutes) {
            for (const Route& r : routes_) {
                const u8* h = (const u8*)&r.header;
                data.insert(data.end(), h, h + sizeof(Header));
                const u8* n = (const u8*)r.nodes.data();
                data.insert(data.end(), n, n + r.nodes.size() * sizeof(Node));
            }
        } else if (id == kChunkSignposts) {
            const u8* h = (const u8*)&signHeader_;
            data.insert(data.end(), h, h + 4);
            const u8* s = (const u8*)signposts_.data();
            data.insert(data.end(), s, s + signposts_.size() * sizeof(Signpost));
        } else if (id == kChunkIntersections) {
            for (const Intersection& it : intersections_) data.insert(data.end(), it.raw.begin(), it.raw.end());
        } else {
            const auto found = extra_.find(id);
            if (found != extra_.end()) data = found->second;
        }
        const u32 size = (u32)data.size();
        put(&id, 4);
        put(&size, 4);
        out.insert(out.end(), data.begin(), data.end());
    }
    return out;
}

Route* Graph::Find(u16 id) {
    for (Route& r : routes_)
        if (r.header.id == id) return &r;
    return nullptr;
}

const Route* Graph::Find(u16 id) const {
    for (const Route& r : routes_)
        if (r.header.id == id) return &r;
    return nullptr;
}

u16 Graph::FreeRouteId(u16 start) const {
    for (int id = start; id <= kMaxRouteId; ++id)
        if (!Find((u16)id) && std::find(reserved_.begin(), reserved_.end(), (u16)id) == reserved_.end()) return (u16)id;
    return 0;
}

void Graph::AddRoute(Route route) {
    routes_.push_back(std::move(route));
    std::stable_sort(routes_.begin(), routes_.end(),
                     [](const Route& a, const Route& b) { return a.header.id < b.header.id; });
}

void Graph::Finish(Route& r) {
    auto& ns = r.nodes;
    r.header.count = r.header.count2 = (i16)ns.size();
    for (size_t k = 0; k < ns.size(); ++k) {
        Node& n = ns[k];
        n.flags = (n.flags & ~0x7FFu) | r.header.id;
        if (k + 1 < ns.size()) {
            const Node& m = ns[k + 1];
            const float dx = m.x - n.x, dy = m.y - n.y;
            const float d = std::max(0.05f, std::sqrt(dx * dx + dy * dy));
            n.segLen256 = (u16)std::clamp((long)std::lround(d * 256.0f), 1l, 65535l);
            n.dirX = (i16)std::lround(dx / d * 32767.0f);
            n.dirY = (i16)std::lround(dy / d * 32767.0f);
            n.slope = ClampI8(128.0f * (m.height8 - n.height8) / 8.0f / d);
        } else {
            n.segLen256 = 1280;                     // every shipped route ends with 5.0
            if (k > 0) {
                n.dirX = ns[k - 1].dirX;
                n.dirY = ns[k - 1].dirY;
                n.slope = ns[k - 1].slope;
            }
        }
    }
    float total = 0.0f;
    for (size_t k = 0; k + 1 < ns.size(); ++k) total += ns[k].segLen256 / 256.0f;
    r.header.length = total;
    Box(r, r.header.bbox);
    for (int j = 0; j < 8; ++j) {
        bool any = false;
        for (const Node& n : ns) any |= n.lanes[j] != -1;
        r.header.lanePresent[j] = any ? 1 : 0;
    }
}

size_t Graph::DropDanglingSlots() {
    const size_t count = signposts_.size();
    size_t fixed = 0;
    for (Route& r : routes_)
        for (Node& n : r.nodes) {
            u16 keep[2] = { kNoSlot, kNoSlot };
            int k = 0;
            for (u16 s : n.slots)
                if (s != kNoSlot && s < count) keep[k++] = s;
            if (keep[0] != n.slots[0] || keep[1] != n.slots[1]) {
                n.slots[0] = keep[0];
                n.slots[1] = keep[1];
                ++fixed;
            }
        }
    return fixed;
}

bool Graph::AddSignpost(u16 srcRoute, u16 srcNode, u16 dstRoute, u16 dstNode, bool backward, bool flip, int a,
                        int b, std::wstring& error) {
    Route* r = Find(srcRoute);
    if (!r || srcNode >= r->nodes.size()) { error = L"a signpost from a node that is not there"; return false; }
    Node& n = r->nodes[srcNode];
    const int free = n.slots[0] == kNoSlot ? 0 : n.slots[1] == kNoSlot ? 1 : -1;
    if (free < 0) { error = L"a road node already holds two links"; return false; }
    if (signposts_.size() >= kMaxSignposts) { error = L"the game's limit of 4000 links is reached"; return false; }
    Signpost sp;
    sp.srcRoute = srcRoute;
    sp.srcNode = srcNode;
    sp.dstRoute = dstRoute;
    sp.dstNode = dstNode;
    sp.flags = SignpostFlags(a, b, backward, flip);
    signposts_.push_back(sp);
    n.slots[free] = (u16)(signposts_.size() - 1);
    return true;
}

bool Graph::AddDeadEnd(u16 routeId, u16 node, std::wstring& error) {
    if (!AddSignpost(routeId, node, routeId, node, node == 0, false, 7, 7, error)) return false;
    signposts_.back().unk10 = 0;     // as on all 29 shipped ones
    return true;
}

bool Graph::InsertNode(u16 routeId, u16 index, float x, float y, float z, std::wstring& error) {
    Route* r = Find(routeId);
    if (!r || index == 0 || index >= r->nodes.size()) { error = L"a node can only go inside a road"; return false; }
    Node n = r->nodes[index - 1];
    n.x = x;
    n.y = y;
    n.height8 = (i16)std::lround(z * 8.0f);
    n.slots[0] = n.slots[1] = kNoSlot;
    n.flags = (n.flags & ~(0x3Fu << 19)) | kNoIntersection << 19;
    r->nodes.insert(r->nodes.begin() + index, n);
    for (Signpost& sp : signposts_) {
        if (sp.srcRoute == routeId && sp.srcNode >= index) ++sp.srcNode;
        if (sp.dstRoute == routeId && sp.dstNode >= index) ++sp.dstNode;
        if (sp.ext[0] != 0xFFFF) {
            if (sp.ext[0] == routeId && sp.ext[2] >= index) ++sp.ext[2];
            if (sp.ext[1] == routeId && sp.ext[3] >= index) ++sp.ext[3];
        }
    }
    for (Intersection& it : intersections_)
        for (int k = 0; k < 3; ++k)
            if (it.Route(k) == routeId && it.NodeIndex(k) >= index) it.SetNodeIndex(k, (u16)(it.NodeIndex(k) + 1));
    Finish(*r);
    return true;
}

bool Graph::Project(const Route& r, float x, float y, int& k, float& t, float& distance) const {
    distance = 1e30f;
    for (size_t i = 0; i + 1 < r.nodes.size(); ++i) {
        const Node& a = r.nodes[i];
        const Node& b = r.nodes[i + 1];
        const float vx = b.x - a.x, vy = b.y - a.y;
        const float l2 = std::max(1e-6f, vx * vx + vy * vy);
        const float tt = std::clamp(((x - a.x) * vx + (y - a.y) * vy) / l2, 0.0f, 1.0f);
        const float px = a.x + vx * tt - x, py = a.y + vy * tt - y;
        const float d = std::sqrt(px * px + py * py);
        if (d < distance) { distance = d; k = (int)i; t = tt; }
    }
    return distance < 1e29f;
}

int Graph::JunctionNode(u16 routeId, float x, float y, float z, std::wstring& error, float snap) {
    Route* r = Find(routeId);
    if (!r || r->nodes.size() < 2) { error = L"no such road"; return -1; }
    int k = 0;
    float t = 0.0f, d = 0.0f;
    Project(*r, x, y, k, t, d);
    for (int i : { k, k + 1 }) {
        const Node& n = r->nodes[(size_t)i];
        const float dx = n.x - x, dy = n.y - y;
        if (std::sqrt(dx * dx + dy * dy) <= snap && n.slots[0] == kNoSlot && n.slots[1] == kNoSlot && i > 0 &&
            i + 1 < (int)r->nodes.size())
            return i;
    }
    if (t < 0.05f || t > 0.95f) t = std::clamp(t, 0.25f, 0.75f);
    const Node& a = r->nodes[(size_t)k];
    const Node& b = r->nodes[(size_t)k + 1];
    const float px = a.x + (b.x - a.x) * t, py = a.y + (b.y - a.y) * t;
    const float pz = (a.height8 + (b.height8 - a.height8) * t) / 8.0f;
    (void)z;
    if (!InsertNode(routeId, (u16)(k + 1), px, py, pz, error)) return -1;
    return k + 1;
}

bool Graph::ConnectEnd(u16 newRoute, bool atStart, u16 oldRoute, u16 oldNode, bool traffic, std::wstring& error) {
    const Route* r = Find(newRoute);
    if (!r) { error = L"no such road"; return false; }
    const u16 k = atStart ? 0 : (u16)(r->nodes.size() - 1);
    const bool enterBackward = !atStart;     // entering at the last node means driving it backwards
    const bool leaveBackward = atStart;      // arriving at node 0 means driving it backwards
    const int b = traffic ? 2 : 7;
    for (bool oldBackward : { false, true })
        if (!AddSignpost(oldRoute, oldNode, newRoute, k, oldBackward, oldBackward != enterBackward, 2, b, error))
            return false;
    for (bool oldForward : { true, false })
        if (!AddSignpost(newRoute, k, oldRoute, oldNode, leaveBackward, leaveBackward != !oldForward, 2, b, error))
            return false;
    return true;
}

std::vector<std::string> Graph::Check() const {
    std::vector<std::string> bad;
    char text[160];
    for (size_t i = 1; i < routes_.size(); ++i)
        if (routes_[i - 1].header.id >= routes_[i].header.id) bad.push_back("route ids not sorted or not unique");
    if (signposts_.size() > kMaxSignposts) bad.push_back("more than 4000 signposts");
    for (const Route& r : routes_) {
        if (r.header.id == 0 || r.header.id > kMaxRouteId) {
            std::snprintf(text, sizeof(text), "route id %u out of range", r.header.id);
            bad.push_back(text);
        }
        if ((size_t)r.header.count != r.nodes.size() || r.nodes.size() < 2) {
            std::snprintf(text, sizeof(text), "route %u has %zu nodes", r.header.id, r.nodes.size());
            bad.push_back(text);
        }
        for (size_t k = 0; k < r.nodes.size(); ++k)
            for (u16 s : r.nodes[k].slots)
                if (s != kNoSlot && s < signposts_.size()) {
                    const Signpost& sp = signposts_[s];
                    if (sp.srcRoute != r.header.id || sp.srcNode != k) {
                        std::snprintf(text, sizeof(text), "route %u node %zu holds a signpost from elsewhere",
                                      r.header.id, k);
                        bad.push_back(text);
                    }
                }
    }
    for (size_t i = 0; i < signposts_.size(); ++i) {
        const Signpost& sp = signposts_[i];
        const Route* src = Find(sp.srcRoute);
        const Route* dst = Find(sp.dstRoute);
        if ((src && sp.srcNode >= src->nodes.size()) || (dst && sp.dstNode >= dst->nodes.size())) {
            std::snprintf(text, sizeof(text), "signpost %zu points past a road's end", i);
            bad.push_back(text);
        }
    }
    return bad;
}

} // namespace nfs::routes
