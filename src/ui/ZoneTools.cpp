#include "ui/ZoneTools.h"

#include "core/Log.h"
#include "io/MeshOverlay.h"
#include "nfs/FreeRoam.h"
#include "nfs/World.h"
#include "render/Gizmo.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <cstdlib>
#include <map>
#include <set>
#include <fstream>
#include <iterator>
#include <sstream>

namespace {

constexpr int kSectionsField = 1;

std::wstring Widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

std::wstring Format(const wchar_t* format, ...) {
    wchar_t buffer[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    return buffer;
}

D3DCOLOR ZoneColour(u32 id) {
    // Distinct, readable hues from the id.
    const float h = std::fmod(id * 0.61803398875f, 1.0f) * 6.0f;
    const float f = h - std::floor(h);
    float r = 0, g = 0, b = 0;
    switch ((int)h) {
        case 0: r = 1; g = f; break;
        case 1: r = 1 - f; g = 1; break;
        case 2: g = 1; b = f; break;
        case 3: g = 1 - f; b = 1; break;
        case 4: r = f; b = 1; break;
        default: r = 1; b = 1 - f; break;
    }
    return D3DCOLOR_ARGB(200, (int)(80 + 175 * r), (int)(80 + 175 * g), (int)(80 + 175 * b));
}

} // namespace

void ZoneTools::OnCreate() {
    // First in line: while zones are shown a click is theirs, not a terrain
    // brush's or a road tool's that was left on (which raised the ground under
    // the zone at every click).
    host_.viewport->AddToolFirst(this);
    undo::Register(this);
    if (!host_.menu) return;
    for (int i = 0; i < ::GetMenuItemCount(host_.menu); ++i) {
        wchar_t text[64] = {};
        ::GetMenuStringW(host_.menu, (UINT)i, text, 63, MF_BYPOSITION);
        if (std::wstring(text).find(L"View") == std::wstring::npos) continue;
        if (HMENU view = ::GetSubMenu(host_.menu, i)) {
            ::AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
            ::AppendMenuW(view, MF_STRING, kShowCommand, L"&Visibility Zones (click one to see and edit its sections)");
            ::AppendMenuW(view, MF_STRING, kFitCommand, L"Load New Pieces in the &Zones Around Them");
        }
        break;
    }
}

bool ZoneTools::OnCommand(int id) {
    if (id == kFitCommand) { FitListsToPieces(); return true; }
    if (id != kShowCommand) return false;
    shown_ = !shown_;
    if (!shown_) { selected_ = -1; editing_ = false; vertex_ = -1; }
    UpdateOverlay();
    if (host_.menuChecks) host_.menuChecks();
    Status(shown_ ? L"Visibility zones shown: click one to see the sections the game loads there." : L"Visibility zones hidden.");
    return true;
}

void ZoneTools::UpdateMenuChecks(HMENU menu) {
    ::CheckMenuItem(menu, kShowCommand, MF_BYCOMMAND | (shown_ ? MF_CHECKED : MF_UNCHECKED));
}

std::wstring ZoneTools::SpanKey(u32 zoneId) const {
    nfs::World* world = World();
    return (world ? world->Region() : std::wstring(L"?")) + L"-zone-" + std::to_wstring(zoneId);
}

namespace {
float DistanceToRingDump(const std::vector<Vec3>& ring, float x, float y) {
    float best = 1e30f;
    for (size_t k = 0; k < ring.size(); ++k) {
        const Vec3& a = ring[k];
        const Vec3& b = ring[(k + 1) % ring.size()];
        const float dx = b.x - a.x, dy = b.y - a.y, len = dx * dx + dy * dy;
        const float t = len > 0.0f ? std::clamp(((x - a.x) * dx + (y - a.y) * dy) / len, 0.0f, 1.0f) : 0.0f;
        best = std::min(best, std::hypot(a.x + t * dx - x, a.y + t * dy - y));
    }
    return best;
}
} // namespace

void ZoneTools::OnRegionLoaded() {
    heights_.clear();
    // FRS_ZONE_DUMP: every FRS_ piece, its section, and whether the zones
    // around it (the one under each corner of its box, and every zone within
    // 60 m) list that section.
    if (std::getenv("FRS_ZONE_DUMP") && World()) {
        nfs::World& w = *World();
        const auto& vis = w.Visibility();
        size_t pieces = 0;
        for (const auto& section : w.Loaded()) {
            if (!section->info) continue;
            for (const auto& placed : section->placements) {
                const auto& in = placed.instance;
                if (in.modelName.rfind("FRS_", 0) != 0) continue;
                ++pieces;
                const Vec3 c{ in.transform.m[3][0], in.transform.m[3][1], in.transform.m[3][2] };
                std::set<int> zones;
                const Aabb& b = in.modelBounds;
                for (int k = 0; k < 4; ++k) {
                    const Vec3 local{ (k & 1) ? b.max.x : b.min.x, (k & 2) ? b.max.y : b.min.y, 0.0f };
                    const Vec3 p = in.transform.TransformPoint(local);
                    zones.insert(vis.ZoneAt(p.x, p.y));
                }
                for (size_t z = 0; z < vis.Zones().size(); ++z) {
                    const auto& zone = vis.Zones()[z];
                    if (c.x < zone.boundsMin.x - 60 || c.x > zone.boundsMax.x + 60 ||
                        c.y < zone.boundsMin.y - 60 || c.y > zone.boundsMax.y + 60) continue;
                    zones.insert((int)z);
                }
                std::string names;
                for (int z : zones) {
                    if (z < 0) { names += " (none)"; continue; }
                    const u32 id = vis.Zones()[(size_t)z].id;
                    const auto* list = vis.ListOf(id);
                    const bool has = list && std::find(list->begin(), list->end(), section->info->number) != list->end();
                    names += " " + std::to_string(id) + (has ? "+" : "-");
                }
                LOG_INFO("zone dump: %s in %s at (%.0f %.0f %.0f), zones%s", in.modelName.c_str(),
                         section->info->name.c_str(), c.x, c.y, c.z, names.c_str());
            }
        }
        LOG_INFO("zone dump: %zu pieces", pieces);
        // Sections named in FRS_ZONE_DUMP_SECTIONS (e.g. "F99 D24 R26"): their
        // edited collision, and which zones near FRS_ZONE_DUMP_AT list them.
        if (const char* names = std::getenv("FRS_ZONE_DUMP_SECTIONS")) {
            float ax = 0, ay = 0;
            if (const char* at = std::getenv("FRS_ZONE_DUMP_AT")) std::sscanf(at, "%f %f", &ax, &ay);
            std::istringstream words(names);
            std::string name;
            while (words >> name) {
                const nfs::Section* sec = nullptr;
                for (const auto& s2 : w.Table().Sections()) if (s2.name == name) sec = &s2;
                if (!sec) continue;
                size_t polygons = 0;
                bool loaded = false;
                for (const auto& l : w.Loaded())
                    if (l->info == sec) { loaded = true; for (const auto& pack : l->collision) polygons += pack.polygons.size(); }
                std::string zonesText;
                for (const auto& zone : vis.Zones()) {
                    const float d = vis.Contains(zone, ax, ay) ? 0.0f : DistanceToRingDump(zone.ring, ax, ay);
                    if (d > 150.0f) continue;
                    const auto* list = vis.ListOf(zone.id);
                    const bool has = list && std::find(list->begin(), list->end(), sec->number) != list->end();
                    zonesText += " " + std::to_string(zone.id) + (has ? "+" : "-") + "@" + std::to_string((int)d);
                }
                LOG_INFO("zone dump: section %s loaded %d, %zu collision polygons; zones near (%.0f %.0f):%s",
                         name.c_str(), loaded, polygons, ax, ay, zonesText.c_str());
            }
            // Every collision polygon within 10 m of the point, whatever its section.
            for (const auto& l : w.Loaded()) {
                if (!l->info) continue;
                for (size_t pk = 0; pk < l->collision.size(); ++pk) {
                    const auto& pack = l->collision[pk];
                    for (size_t q = 0; q < pack.polygons.size(); ++q) {
                        const auto& poly = pack.polygons[q];
                        const int n = poly.quad ? 4 : 3;
                        float d = 1e30f;
                        for (int k = 0; k < n; ++k) {
                            const Vec3& c0 = poly.corners[k];
                            const Vec3& c1 = poly.corners[(k + 1) % n];
                            const float dx = c1.x - c0.x, dy = c1.y - c0.y, len = dx * dx + dy * dy;
                            const float t = len > 0 ? std::clamp(((ax - c0.x) * dx + (ay - c0.y) * dy) / len, 0.0f, 1.0f) : 0.0f;
                            d = std::min(d, std::hypot(c0.x + t * dx - ax, c0.y + t * dy - ay));
                        }
                        if (d > 10.0f) continue;
                        LOG_INFO("zone dump: poly %s/%zu/%zu %s flags %02X surf %u group %u d %.1f: (%.1f %.1f %.1f) (%.1f %.1f %.1f) (%.1f %.1f %.1f) (%.1f %.1f %.1f)",
                                 l->info->name.c_str(), pk, q, poly.barrier ? "BARRIER" : "ground", poly.flags, poly.surface,
                                 poly.raceGroup, d, poly.corners[0].x, poly.corners[0].y, poly.corners[0].z,
                                 poly.corners[1].x, poly.corners[1].y, poly.corners[1].z, poly.corners[2].x,
                                 poly.corners[2].y, poly.corners[2].z, poly.corners[3].x, poly.corners[3].y, poly.corners[3].z);
                    }
                }
            }
        }
    }
    selected_ = -1;
    spans_.clear();
    nfs::World* world = World();
    const std::wstring folder = host_.modelsFolder ? host_.modelsFolder() : L"";
    if (world && !folder.empty()) {
        std::vector<SpanOverlayInfo> spans;
        ListSpanOverlays(folder, world->Region(), spans);
        const std::wstring marker = world->Region() + L"-zone-";
        for (const SpanOverlayInfo& span : spans)
            if (span.path.find(marker) != std::wstring::npos) spans_.push_back(span.path);
    }
    // FRS_SHOW_ZONES=1: shown from the start, the zone at the camera picked
    // (for a capture).
    if (std::getenv("FRS_SHOW_ZONES") && World()) {
        shown_ = true;
        const Vec3 eye = host_.viewport->GetCamera().Position();
        selected_ = World()->Visibility().ZoneAt(eye.x + 40.0f, eye.y + 40.0f);
    }
    UpdateOverlay();
}

bool ZoneTools::CornerGround(float x, float y, float& z) const {
    u32 bx, by;
    std::memcpy(&bx, &x, 4);
    std::memcpy(&by, &y, 4);
    const u64 key = ((u64)bx << 32) | by;
    if (auto it = heights_.find(key); it != heights_.end()) { z = it->second; return true; }
    nfs::World* world = World();
    if (!world) return false;
    float h = 0.0f;
    bool found = nfs::GroundHeight(*world, x, y, h);
    // Inside a building there is no ground: the nearest found around it.
    for (float r : { 4.0f, 10.0f, 20.0f, 40.0f }) {
        if (found) break;
        for (int k = 0; k < 8 && !found; ++k) {
            const float a = (float)k * 0.785398f;
            found = nfs::GroundHeight(*world, x + r * std::cos(a), y + r * std::sin(a), h);
        }
    }
    if (!found) return false;
    z = h + 1.0f;
    heights_[key] = z;
    return true;
}

std::vector<float> ZoneTools::RingHeights(const std::vector<Vec3>& ring) const {
    std::vector<float> z(ring.size(), 0.0f);
    std::vector<bool> found(ring.size(), false);
    float sum = 0.0f;
    int count = 0;
    for (size_t k = 0; k < ring.size(); ++k)
        if (CornerGround(ring[k].x, ring[k].y, z[k])) { found[k] = true; sum += z[k]; ++count; }
    const float mean = count ? sum / (float)count : 1.0f;
    for (size_t k = 0; k < ring.size(); ++k)
        if (!found[k]) z[k] = mean;
    return z;
}

void ZoneTools::UpdateOverlay() {
    nfs::World* world = World();
    if (!shown_ || !world) { Renderer().SetToolOverlay({}, {}, this); return; }
    const auto& zones = world->Visibility().Zones();
    std::vector<WorldRenderer::OverlayVertex> triangles, lines;
    for (size_t i = 0; i < zones.size(); ++i) {
        const auto& zone = zones[i];
        if (!world->Visibility().ListOf(zone.id)) continue;
        const bool lit = (int)i == selected_;
        const D3DCOLOR colour = lit ? D3DCOLOR_ARGB(255, 255, 220, 60) : ZoneColour(zone.id);
        std::vector<Vec3> ring;
        const std::vector<Vec3>& source = lit && dragging_ ? dragRing_ : zone.ring;
        const std::vector<float> heights = RingHeights(source);
        for (size_t k = 0; k < source.size(); ++k) ring.push_back({ source[k].x, source[k].y, heights[k] });
        if (lit && editing_)   // the outline's corners, the picked one larger
            for (size_t k = 0; k < ring.size(); ++k) {
                const float s = (int)k == vertex_ ? 2.5f : 1.2f;
                const D3DCOLOR c = (int)k == vertex_ ? D3DCOLOR_ARGB(255, 255, 255, 255) : colour;
                const Vec3& p = ring[k];
                lines.push_back({ p.x - s, p.y, p.z, c }); lines.push_back({ p.x + s, p.y, p.z, c });
                lines.push_back({ p.x, p.y - s, p.z, c }); lines.push_back({ p.x, p.y + s, p.z, c });
                lines.push_back({ p.x, p.y, p.z, c }); lines.push_back({ p.x, p.y, p.z + 2 * s, c });
            }
        for (size_t k = 0; k < ring.size(); ++k) {
            const Vec3& a = ring[k];
            const Vec3& b = ring[(k + 1) % ring.size()];
            lines.push_back({ a.x, a.y, a.z, colour });
            lines.push_back({ b.x, b.y, b.z, colour });
            if (lit) {   // a second line a little higher reads as a thicker one
                lines.push_back({ a.x, a.y, a.z + 0.6f, colour });
                lines.push_back({ b.x, b.y, b.z + 0.6f, colour });
            }
        }
        if (lit) {
            Vec3 centre{};
            for (const Vec3& p : ring) centre = centre + p;
            centre = centre * (1.0f / (float)ring.size());
            const D3DCOLOR fill = D3DCOLOR_ARGB(60, 255, 220, 60);
            for (size_t k = 0; k < ring.size(); ++k) {
                const Vec3& a = ring[k];
                const Vec3& b = ring[(k + 1) % ring.size()];
                triangles.push_back({ centre.x, centre.y, centre.z, fill });
                triangles.push_back({ a.x, a.y, a.z, fill });
                triangles.push_back({ b.x, b.y, b.z, fill });
            }
        }
    }
    Renderer().SetToolOverlay(std::move(triangles), std::move(lines), this);
}

bool ZoneTools::BeginEdit() {
    nfs::World* world = World();
    if (!shown_ || !world || selected_ < 0 || editing_) return false;
    editing_ = true;
    vertex_ = -1;
    UpdateOverlay();
    Status(L"Editing the zone's outline: click a corner, move it with the gizmo; Tab or Object Mode ends.");
    return true;
}

void ZoneTools::EndEdit() {
    editing_ = false;
    vertex_ = -1;
    dragging_ = false;
    UpdateOverlay();
    if (host_.menuChecks) host_.menuChecks();
    Status(L"Zone outline editing ended.");
}

void ZoneTools::Select(int zone) {
    if (zone != selected_) vertex_ = -1;
    selected_ = zone;
    UpdateOverlay();
    if (host_.menuChecks) host_.menuChecks();
}

bool ZoneTools::MouseDown(int x, int y, WPARAM keys) {
    (void)keys;
    nfs::World* world = World();
    if (!shown_ || !world) return false;
    if (!editing_ && host_.busy && host_.busy()) return false;
    // Editing: a corner of the picked zone under the pointer, for the gizmo;
    // anywhere else lets go of the corner and keeps the zone.
    if (editing_ && selected_ >= 0 && (size_t)selected_ < world->Visibility().Zones().size()) {
        const auto& ring = world->Visibility().Zones()[(size_t)selected_].ring;
        const GizmoView view = host_.viewport->ToolView();
        int best = -1;
        float bestDistance = 20.0f;
        const std::vector<float> heights = RingHeights(ring);
        for (size_t k = 0; k < ring.size(); ++k) {
            Vec2 pixel;
            if (!GizmoProject(view, { ring[k].x, ring[k].y, heights[k] }, pixel)) continue;
            const float d = std::hypot(pixel.x - (float)x, pixel.y - (float)y);
            if (d < bestDistance) { bestDistance = d; best = (int)k; }
        }
        vertex_ = best;
        UpdateOverlay();
        if (best >= 0) Status(L"Corner picked: move it with the gizmo (only across the ground; the height is not stored).");
        return true;
    }
    vertex_ = -1;
    const Ray ray = host_.viewport->RayAt(x, y);
    // Where the click meets the world: the first surface, else the ground
    // plane under the camera.
    Vec3 point;
    PickResult hit;
    if (Renderer().Pick(ray, hit)) {
        point = ray.origin + ray.direction * hit.distance;
    } else if (std::fabs(ray.direction.z) > 1e-4f) {
        const float t = (ray.origin.z - 20.0f - ray.origin.z) / ray.direction.z;
        point = ray.origin + ray.direction * std::max(t, 0.0f);
    } else {
        return true;
    }
    const int zone = world->Visibility().ZoneAt(point.x, point.y);
    Renderer().ClearSelection();   // no object's gizmo left standing beside the zone
    Select(zone);
    if (zone < 0) Status(L"No visibility zone there: the game keeps the zone the car came from.");
    else Status(Format(L"Visibility zone %u: its sections are in the properties.", world->Visibility().Zones()[(size_t)zone].id));
    return true;
}

bool ZoneTools::GizmoPivot(Vec3& pivot) {
    nfs::World* world = World();
    if (!shown_ || !editing_ || !world || selected_ < 0 || vertex_ < 0) return false;
    const auto& ring = world->Visibility().Zones()[(size_t)selected_].ring;
    if ((size_t)vertex_ >= ring.size()) return false;
    // Its height stays where the drag started, so the gizmo does not jump
    // while the corner crosses kerbs and roofs.
    const std::vector<float> heights = RingHeights(ring);
    const Vec3 p = dragging_ ? dragRing_[(size_t)vertex_] : ring[(size_t)vertex_];
    pivot = { p.x, p.y, heights[(size_t)vertex_] };
    return true;
}

void ZoneTools::GizmoChanged(const Mat4& transform, bool done, bool keep) {
    nfs::World* world = World();
    if (!shown_ || !editing_ || !world || selected_ < 0 || vertex_ < 0) return;
    const auto& zone = world->Visibility().Zones()[(size_t)selected_];
    if (!dragging_) { dragRing_ = zone.ring; dragging_ = true; }
    // The gizmo's frame starts at the corner: where its origin went is where
    // the corner goes, across the ground only.
    const Vec3 moved = transform.TransformPoint({ 0.0f, 0.0f, 0.0f });
    dragRing_[(size_t)vertex_].x = moved.x;
    dragRing_[(size_t)vertex_].y = moved.y;
    UpdateOverlay();
    if (!done) return;
    dragging_ = false;
    if (!keep) { UpdateOverlay(); return; }
    std::wstring error;
    if (!WriteOutline((size_t)selected_, dragRing_, error)) {
        Status(L"The zone's outline was not changed: " + error);
        UpdateOverlay();
        return;
    }
    Status(Format(L"Visibility zone %u reshaped. Reopening the region.", zone.id));
    const int keepZone = selected_, keepVertex = vertex_;
    if (host_.reopenRegion) host_.reopenRegion();
    shown_ = true;
    editing_ = true;
    selected_ = keepZone;
    vertex_ = keepVertex;
    UpdateOverlay();
}

std::vector<u8> ZoneTools::CurrentBytes(u64 offset, u32 length) const {
    nfs::World* world = World();
    const ByteSpan layout = world->LayoutBytes();
    if (offset + length > layout.size) return {};
    std::vector<u8> bytes(layout.data + offset, layout.data + offset + length);
    for (const SpanOverlay& span : world->Spans())
        if (span.layoutFile && span.fileOffset <= offset && span.fileOffset + span.bytes.size() >= offset + length)
            std::copy(span.bytes.begin() + (ptrdiff_t)(offset - span.fileOffset),
                      span.bytes.begin() + (ptrdiff_t)(offset - span.fileOffset + length), bytes.begin());
    return bytes;
}

bool ZoneTools::WriteOutline(size_t index, const std::vector<Vec3>& ring, std::wstring& error) {
    nfs::World* world = World();
    const std::wstring folder = host_.modelsFolder ? host_.modelsFolder() : L"";
    if (!world || folder.empty() || index >= world->Visibility().Zones().size()) { error = L"no zone"; return false; }
    const auto& zone = world->Visibility().Zones()[index];
    std::vector<u8> bytes = CurrentBytes(zone.recordOffset, zone.recordLength);
    u16 points = 0;
    if (bytes.size() >= 4) std::memcpy(&points, bytes.data() + 2, 2);
    if (bytes.empty() || points != ring.size() + 2 || 4 + (size_t)points * 8 > bytes.size()) {
        error = L"the outline record is not as expected";
        return false;
    }
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (const Vec3& p : ring) {
        minX = std::min(minX, p.x); minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x); maxY = std::max(maxY, p.y);
    }
    const float box[4] = { minX, minY, maxX, maxY };
    std::memcpy(bytes.data() + 4, box, sizeof(box));
    for (size_t k = 0; k < ring.size(); ++k) {
        const float xy[2] = { ring[k].x, ring[k].y };
        std::memcpy(bytes.data() + 4 + (k + 2) * 8, xy, sizeof(xy));
    }
    const std::string name = "visibility zone " + std::to_string(zone.id) + " outline";
    const std::wstring key = SpanKey(zone.id) + L"-outline";
    RememberSpan(key);
    if (!WriteSpanOverlay(folder, world->Region(), name, zone.recordOffset, true, bytes, key)) {
        error = L"the span could not be written to the models folder";
        return false;
    }
    LOG_INFO("zones: zone %u outline written (%zu points, box %.0f %.0f - %.0f %.0f)", zone.id, ring.size(), minX,
             minY, maxX, maxY);
    return true;
}

bool ZoneTools::StatusText(std::wstring& text) const {
    if (!shown_) return false;
    text = L"Visibility zones: click a zone to see what the game loads there; edit the list in the properties; "
           L"click a corner of the picked zone and move it with the gizmo to reshape it.";
    return true;
}

bool ZoneTools::FillProperties(PropertiesPanel::View& view) {
    nfs::World* world = World();
    if (!shown_ || !world || selected_ < 0 || (size_t)selected_ >= world->Visibility().Zones().size()) return false;
    const auto& zone = world->Visibility().Zones()[(size_t)selected_];
    const std::vector<u32>* list = world->Visibility().ListOf(zone.id);
    if (!list) return false;
    u64 offset = 0;
    u32 length = 0;
    world->Visibility().ListRecord(zone.id, offset, length);
    const size_t capacity = length >= 12 ? (length - 12) / 2 : list->size();

    std::wstring names;
    for (u32 number : *list) {
        std::wstring name = L"#" + std::to_wstring(number);
        for (const auto& s : world->Table().Sections())
            if (s.number == number) { name = Widen(s.name); break; }
        names += (names.empty() ? L"" : L" ") + name;
    }
    const auto add = [&view](const wchar_t* group, const wchar_t* name, std::wstring value, const wchar_t* description,
                             bool editable = false, int field = 0) {
        PropertiesPanel::Row row;
        row.group = group;
        row.name = name;
        row.value = std::move(value);
        row.description = description;
        row.editable = editable;
        row.field = field;
        view.rows.push_back(std::move(row));
    };
    view.mode = PropertiesPanel::Mode::Extension;
    view.title = Format(L"Visibility zone %u", zone.id);
    view.moveLabel.clear();
    view.removeLabel.clear();
    add(L"Zone", L"Sections loaded", names,
        L"What the game loads while the car is in this zone, by section name, separated by spaces. Add a section "
        L"here and what it holds (a road, its collision) stays loaded in this zone.", true, kSectionsField);
    add(L"Zone", L"Count", Format(L"%zu of at most %zu", list->size(), capacity),
        L"The file leaves room after each list; a list grows only into that room, so nothing else in it moves.");
    add(L"Zone", L"Outline", Format(L"%zu points, (%.0f %.0f) to (%.0f %.0f)", zone.ring.size(), zone.boundsMin.x,
                                    zone.boundsMin.y, zone.boundsMax.x, zone.boundsMax.y),
        L"Where the zone lies on the ground. The smallest zone holding the car is the one in force.");
    return true;
}

bool ZoneTools::ApplyProperties() {
    nfs::World* world = World();
    if (!shown_ || !world || selected_ < 0 || !host_.properties) return false;
    std::wstring text;
    if (!host_.properties->ReadText(kSectionsField, text)) return false;
    const auto& zone = world->Visibility().Zones()[(size_t)selected_];
    const std::vector<u32>* current = world->Visibility().ListOf(zone.id);
    if (!current) return false;

    std::vector<u32> sections;
    std::wstringstream words(text);
    std::wstring word;
    std::wstring unknown;
    while (words >> word) {
        for (wchar_t& c : word) c = (wchar_t)towupper(c);
        bool found = false;
        for (const auto& s : world->Table().Sections())
            if (Widen(s.name) == word) {
                if (std::find(sections.begin(), sections.end(), s.number) == sections.end()) sections.push_back(s.number);
                found = true;
                break;
            }
        if (!found) unknown += (unknown.empty() ? L"" : L", ") + word;
    }
    if (!unknown.empty()) {
        Status(L"No section called " + unknown + L" in this region; the list was not changed.");
        return true;
    }
    if (sections == *current) return true;
    std::wstring error;
    if (!WriteList(zone.id, sections, error)) {
        Status(L"The zone's list was not changed: " + error);
        return true;
    }
    Status(Format(L"Visibility zone %u now loads %zu sections. Reopening the region.", zone.id, sections.size()));
    const int keep = selected_;
    if (host_.reopenRegion) host_.reopenRegion();
    shown_ = true;
    Select(keep);
    return true;
}

bool ZoneTools::WriteList(u32 zoneId, const std::vector<u32>& sections, std::wstring& error) {
    nfs::World* world = World();
    const std::wstring folder = host_.modelsFolder ? host_.modelsFolder() : L"";
    u64 offset = 0;
    u32 length = 0;
    if (!world || folder.empty() || !world->Visibility().ListRecord(zoneId, offset, length) || length < 12) {
        error = L"the zone's list is not where it should be";
        return false;
    }
    if (sections.empty()) { error = L"a zone must load at least one section"; return false; }
    if (12 + sections.size() * 2 > length) {
        error = Format(L"room for %u sections only", (length - 12) / 2);
        return false;
    }
    // The record as the file has it now (an earlier edit included), with the
    // count and the numbers replaced and the rest of its room zeroed.
    std::vector<u8> bytes = CurrentBytes(offset, length);
    if (bytes.size() != length) { error = L"the layout file is shorter than expected"; return false; }
    const u32 count = (u32)sections.size();
    std::memcpy(bytes.data() + 8, &count, 4);
    std::fill(bytes.begin() + 12, bytes.end(), (u8)0);
    for (size_t i = 0; i < sections.size(); ++i) {
        const u16 number = (u16)sections[i];
        std::memcpy(bytes.data() + 12 + i * 2, &number, 2);
    }
    const std::string name = "visibility zone " + std::to_string(zoneId);
    RememberSpan(SpanKey(zoneId));
    if (!WriteSpanOverlay(folder, world->Region(), name, offset, true, bytes, SpanKey(zoneId))) {
        error = L"the span could not be written to the models folder";
        return false;
    }
    LOG_INFO("zones: zone %u now lists %u sections (span at %llu, %u bytes)", zoneId, count,
             (unsigned long long)offset, length);
    return true;
}

// FRS_ZONE_SELFTEST=1: the zone at the start of the R26 test road gets D24
// added, the region is reopened and the list read back, then the edit is
// reverted and the list checked to be the game's again.
void ZoneTools::Tick(float deltaSeconds) {
    static float clock = 0.0f;
    static int stage = 0;
    clock += deltaSeconds;
    nfs::World* world = World();
    // FRS_ZONE_CORNERTEST: the picked zone in edit mode, a click on its first
    // corner in view; the corner must be picked and carry the gizmo, and a
    // second pass must draw it at the same height.
    static int cornerStage = 0;
    if (std::getenv("FRS_ZONE_CORNERTEST") && world && clock > 8.0f && cornerStage == 0 && selected_ >= 0) {
        cornerStage = 1;
        const bool began = editing_ || BeginEdit();
        const auto& ring = world->Visibility().Zones()[(size_t)selected_].ring;
        const std::vector<float> before = RingHeights(ring);
        const GizmoView view = host_.viewport->ToolView();
        bool clicked = false;
        for (size_t k = 0; k < ring.size() && !clicked; ++k) {
            Vec2 pixel;
            if (!GizmoProject(view, { ring[k].x, ring[k].y, before[k] }, pixel)) continue;
            if (pixel.x < 5 || pixel.y < 5 || pixel.x > view.width - 5 || pixel.y > view.height - 5) continue;
            MouseDown((int)pixel.x, (int)pixel.y, 0);
            Vec3 pivot;
            const bool gizmo = GizmoPivot(pivot);
            LOG_INFO("zone cornertest: edit %d, corner %zu at pixel (%.0f %.0f) z %.1f -> picked %d, gizmo %d at z %.1f",
                     began, k, pixel.x, pixel.y, before[k], vertex_, gizmo, pivot.z);
            clicked = true;
        }
        if (!clicked) LOG_INFO("zone cornertest: no corner in view");
        const std::vector<float> after = RingHeights(ring);
        float drift = 0.0f;
        for (size_t k = 0; k < ring.size(); ++k) drift = std::max(drift, std::fabs(after[k] - before[k]));
        LOG_INFO("zone cornertest: heights drift %.2f -- %s", drift, clicked && vertex_ >= 0 && drift < 0.01f ? "PASSED" : "FAILED");
        // Ctrl+Z: corner 0 moved 5 m, then undone; the span file must be as before.
        const u32 id = world->Visibility().Zones()[(size_t)selected_].id;
        const std::wstring path = host_.modelsFolder() + L"\\" + SpanKey(id) + L"-outline.frssection";
        const auto readAll = [](const std::wstring& p, bool& exists) {
            std::ifstream in(p, std::ios::binary);
            exists = in.good();
            return std::vector<u8>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        };
        bool existed = false, exists = false;
        const std::vector<u8> was = readAll(path, existed);
        std::vector<Vec3> moved = ring;
        moved[0].x += 5.0f;
        std::wstring error;
        const bool written = WriteOutline((size_t)selected_, moved, error);
        const bool undone = undo::UndoLatest();
        const std::vector<u8> now = readAll(path, exists);
        LOG_INFO("zone cornertest: undo written %d undone %d, file before %d after %d -- %s", written, undone, existed,
                 exists, written && undone && exists == existed && now == was ? "PASSED" : "FAILED");
    }
    if (!std::getenv("FRS_ZONE_SELFTEST") || !world || clock < 10.0f || stage > 1) return;
    const auto numberOf = [&](const char* name) -> u32 {
        for (const auto& s : world->Table().Sections()) if (s.name == name) return s.number;
        return 0;
    };
    const int zone = world->Visibility().ZoneAt(276.0f, 495.0f);
    if (zone < 0) { LOG_ERROR("zone selftest: no zone at the road FAILED"); stage = 2; return; }
    const u32 id = world->Visibility().Zones()[(size_t)zone].id;
    const std::vector<u32> list = *world->Visibility().ListOf(id);
    const u32 d24 = numberOf("D24");
    const bool has = std::find(list.begin(), list.end(), d24) != list.end();
    if (stage == 0) {
        LOG_INFO("zone selftest: zone %u lists %zu sections, D24 %s", id, list.size(), has ? "already in" : "not in");
        std::vector<u32> grown = list;
        if (!has) grown.push_back(d24);
        std::wstring error;
        const bool written = WriteList(id, grown, error);
        LOG_INFO("zone selftest: written %s %S", written ? "ok" : "FAILED", error.c_str());
        // and its first corner moved 10 m east
        std::vector<Vec3> ring = world->Visibility().Zones()[(size_t)zone].ring;
        ring[0].x += 10.0f;
        const bool shaped = WriteOutline((size_t)zone, ring, error);
        LOG_INFO("zone selftest: outline written %s %S (corner 0 to %.1f %.1f)", shaped ? "ok" : "FAILED",
                 error.c_str(), ring[0].x, ring[0].y);
        spans_.push_back((host_.modelsFolder ? host_.modelsFolder() : L"") + L"\\" + SpanKey(id) + L".frssection");
        stage = 1;
        clock = 0.0f;
        if (host_.reopenRegion) host_.reopenRegion();
        return;
    }
    // stage 1: read back, then revert
    LOG_INFO("zone selftest: read back, zone %u lists %zu sections, D24 %s -- %s", id, list.size(),
             has ? "in" : "missing", has ? "ok" : "FAILED");
    const auto& ring = world->Visibility().Zones()[(size_t)zone].ring;
    LOG_INFO("zone selftest: read back, corner 0 at (%.1f %.1f)", ring[0].x, ring[0].y);
    stage = 2;
    const std::wstring path = (host_.modelsFolder ? host_.modelsFolder() : L"") + L"\\" + SpanKey(id) + L".frssection";
    const std::wstring outline = (host_.modelsFolder ? host_.modelsFolder() : L"") + L"\\" + SpanKey(id) + L"-outline.frssection";
    const bool removed = ::DeleteFileW(path.c_str()) != FALSE && ::DeleteFileW(outline.c_str()) != FALSE;
    LOG_INFO("zone selftest: reverted %s; %s", removed ? "ok" : "FAILED", has && removed ? "PASSED" : "FAILED");
    if (host_.reopenRegion) host_.reopenRegion();
}

void ZoneTools::PopulateTree(SceneTreeBuilder& tree) {
    nfs::World* world = World();
    if (!shown_ || !world) return;
    const auto& zones = world->Visibility().Zones();
    HTREEITEM root = tree.insert(TVI_ROOT, Format(L"Visibility zones (%zu)", zones.size()), -1);
    size_t shown = 0;
    for (size_t i = 0; i < zones.size(); ++i) {
        const std::vector<u32>* list = world->Visibility().ListOf(zones[i].id);
        if (!list) continue;
        const std::wstring text = Format(L"Zone %u  -  %zu sections", zones[i].id, list->size());
        if (!tree.matches(text)) continue;
        tree.insert(root, text, (intptr_t)i);
        ++shown;
    }
    tree.finish(root, shown, false);
}

void ZoneTools::OnTreeItem(intptr_t code) {
    nfs::World* world = World();
    if (!world || code < 0 || (size_t)code >= world->Visibility().Zones().size()) return;
    Select((int)code);
    const auto& zone = world->Visibility().Zones()[(size_t)code];
    Aabb box;
    box.Add({ zone.boundsMin.x, zone.boundsMin.y, host_.viewport->GetCamera().Position().z - 40.0f });
    box.Add({ zone.boundsMax.x, zone.boundsMax.y, host_.viewport->GetCamera().Position().z - 40.0f });
    host_.viewport->FrameBounds(box);
}

void ZoneTools::EditRows(std::vector<EditsPanel::ExtraRow>& rows) {
    for (size_t i = 0; i < spans_.size(); ++i) {
        EditsPanel::ExtraRow row;
        row.owner = this;
        row.index = i;
        row.kind = L"Visibility zone";
        const size_t slash = spans_[i].find_last_of(L'\\');
        row.what = slash == std::wstring::npos ? spans_[i] : spans_[i].substr(slash + 1);
        row.detail = L"the list of sections the game loads in it";
        rows.push_back(std::move(row));
    }
}

void ZoneTools::RememberSpan(const std::wstring& key) {
    const std::wstring folder = host_.modelsFolder ? host_.modelsFolder() : L"";
    if (folder.empty()) return;
    UndoStep step;
    step.path = folder + L"\\" + key + L".frssection";
    std::ifstream in(step.path, std::ios::binary);
    step.existed = in.good();
    if (step.existed) step.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    step.zone = selected_;
    step.vertex = vertex_;
    step.stamp = groupStamp_ ? groupStamp_ : undo::NextStamp();
    undo_.push_back(std::move(step));
}

bool ZoneTools::UndoLatest() {
    if (undo_.empty()) return false;
    const u64 stamp = undo_.back().stamp;
    UndoStep step;
    bool ok = true;
    // Every write of the step, newest first (one file written twice ends as
    // it was before the first).
    while (!undo_.empty() && undo_.back().stamp == stamp) {
        step = undo_.back();
        undo_.pop_back();
        if (step.existed) {
            std::ofstream out(step.path, std::ios::binary | std::ios::trunc);
            out.write((const char*)step.bytes.data(), (std::streamsize)step.bytes.size());
            ok = out.good() && ok;
        } else {
            ok = (::DeleteFileW(step.path.c_str()) != FALSE || ::GetLastError() == ERROR_FILE_NOT_FOUND) && ok;
        }
    }
    if (!ok) { Status(L"The zone edit could not be undone: its file is not writable."); return false; }
    const bool wasEditing = editing_;
    if (host_.reopenRegion) host_.reopenRegion();
    shown_ = true;
    editing_ = wasEditing;
    selected_ = step.zone;
    vertex_ = wasEditing ? step.vertex : -1;
    UpdateOverlay();
    if (host_.menuChecks) host_.menuChecks();
    Status(L"Zone edit undone.");
    return true;
}

namespace {
float DistanceToRing(const std::vector<Vec3>& ring, float x, float y) {
    float best = 1e30f;
    for (size_t k = 0; k < ring.size(); ++k) {
        const Vec3& a = ring[k];
        const Vec3& b = ring[(k + 1) % ring.size()];
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len = dx * dx + dy * dy;
        float t = len > 0.0f ? ((x - a.x) * dx + (y - a.y) * dy) / len : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        best = std::min(best, std::hypot(a.x + t * dx - x, a.y + t * dy - y));
    }
    return best;
}
} // namespace

void ZoneTools::FitListsToPieces() {
    nfs::World* world = World();
    if (!world) return;
    const auto& vis = world->Visibility();
    std::map<u32, std::vector<u32>> adds;   // zone id -> sections to add
    size_t pieces = 0;
    for (const auto& section : world->Loaded()) {
        if (!section->info) continue;
        const u32 number = section->info->number;
        for (const auto& placed : section->placements) {
            const auto& in = placed.instance;
            if (in.modelName.rfind("FRS_", 0) != 0) continue;
            ++pieces;
            // Its whole footprint, every 6 m: a long road piece crosses zones
            // that none of its corners stand in.
            std::vector<Vec3> points{ { in.transform.m[3][0], in.transform.m[3][1], 0.0f } };
            const Aabb& b = in.modelBounds;
            const int nx = std::clamp((int)((b.max.x - b.min.x) / 6.0f) + 1, 1, 200);
            const int ny = std::clamp((int)((b.max.y - b.min.y) / 6.0f) + 1, 1, 200);
            for (int ix = 0; ix <= nx; ++ix)
                for (int iy = 0; iy <= ny; ++iy)
                    points.push_back(in.transform.TransformPoint(
                        { b.min.x + (b.max.x - b.min.x) * (float)ix / (float)nx,
                          b.min.y + (b.max.y - b.min.y) * (float)iy / (float)ny, 0.0f }));
            // Partly in a hole no zone claims: the car crossing it keeps
            // whichever zone it came from, so every zone in reach of the hole.
            bool inHole = false;
            for (const Vec3& p : points) inHole = inHole || vis.ZoneAt(p.x, p.y) < 0;
            const float reach = inHole ? kFitHoleReach : kFitReach;
            for (const auto& zone : vis.Zones()) {
                const std::vector<u32>* list = vis.ListOf(zone.id);
                if (!list || std::find(list->begin(), list->end(), number) != list->end()) continue;
                bool close = false;
                float nearest = 1e30f;
                for (const Vec3& p : points) {
                    const float d = vis.Contains(zone, p.x, p.y) ? 0.0f : DistanceToRing(zone.ring, p.x, p.y);
                    nearest = std::min(nearest, d);
                    if (d <= reach) { close = true; break; }
                }
                if (close)
                    LOG_INFO("zones: fit %s (%s) zone %u at %.0f m%s", in.modelName.c_str(), section->info->name.c_str(),
                             zone.id, nearest, close ? " -> add" : "");
                if (!close) continue;
                auto& add = adds[zone.id];
                if (std::find(add.begin(), add.end(), number) == add.end()) add.push_back(number);
            }
        }
    }
    LOG_INFO("zones: fit, %zu pieces, %zu zones lack their sections", pieces, adds.size());
    if (!pieces) { Status(L"No new pieces (FRS_ models) in the sections loaded around the camera."); return; }
    if (adds.empty()) {
        Status(Format(L"All %zu new pieces are already loaded by every zone around them.", pieces));
        return;
    }
    groupStamp_ = undo::NextStamp();
    size_t written = 0;
    std::wstring failed;
    for (const auto& [id, add] : adds) {
        std::vector<u32> list = *vis.ListOf(id);
        list.insert(list.end(), add.begin(), add.end());
        std::wstring error;
        if (WriteList(id, list, error)) ++written;
        else failed += Format(L" zone %u (%s);", id, error.c_str());
        std::string names;
        for (u32 n : add)
            if (const auto* s = world->Table().FindByNumber(n)) names += " " + s->name;
        LOG_INFO("zones: fit zone %u +%s -- %s", id, names.c_str(), failed.empty() ? "ok" : "see status");
    }
    groupStamp_ = 0;
    const int keep = selected_;
    if (written && host_.reopenRegion) host_.reopenRegion();
    selected_ = keep;
    UpdateOverlay();
    Status(Format(L"%zu zone list(s) now load the new pieces around them (Ctrl+Z takes it back).", written) +
           (failed.empty() ? L"" : L" Not changed:" + failed));
}

void ZoneTools::RevertRows(const std::vector<size_t>& indices) {
    size_t removed = 0;
    for (size_t i : indices)
        if (i < spans_.size() && ::DeleteFileW(spans_[i].c_str())) ++removed;
    if (!removed) return;
    Status(Format(L"%zu zone list(s) back to the game's own. Reopening the region.", removed));
    if (host_.reopenRegion) host_.reopenRegion();
}
