#include "io/LightBake.h"

#include "core/Log.h"
#include "nfs/World.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lightbake {

namespace {

Vec3 ColourOf(u32 colour) {
    return { (float)((colour >> 16) & 0xFF) / 128.0f, (float)((colour >> 8) & 0xFF) / 128.0f,
             (float)(colour & 0xFF) / 128.0f };
}

float Luma(const Vec3& c) { return c.x * 0.3f + c.y * 0.59f + c.z * 0.11f; }

bool Overlaps(const Aabb& a, const Aabb& b) {
    return a.Valid() && b.Valid() && a.max.x >= b.min.x && a.min.x <= b.max.x && a.max.y >= b.min.y &&
           a.min.y <= b.max.y && a.max.z >= b.min.z && a.min.z <= b.max.z;
}

// Every placed mesh of the map: (model, transform) through `visit`.
template <typename Visit>
void ForEachPlaced(const nfs::World& world, const Aabb& area, Visit visit) {
    for (const auto& section : world.Loaded()) {
        for (const nfs::SceneryInstance& instance : section->scenery.instances) {
            if (!Overlaps(instance.worldBounds, area)) continue;
            if (const nfs::SolidObject* model = world.FindModel(section->scenery.ModelHash(instance)))
                visit(*model, instance.transform);
        }
        if (world.IsLibrary(*section)) continue;
        for (const nfs::SolidList& list : section->solidLists)
            for (const nfs::SolidObject& model : list.objects)
                if (model.bounds.Valid() && Overlaps(Transform(model.bounds, model.transform), area))
                    visit(model, model.transform);
    }
}

// The head of a lamp: the middle of its highest vertices.
Vec3 LampHead(const nfs::SolidObject& model, const Mat4& transform) {
    float top = -1e30f, bottom = 1e30f;
    for (const auto& v : model.vertices) {
        const float z = transform.TransformPoint(v.position).z;
        top = std::max(top, z);
        bottom = std::min(bottom, z);
    }
    const float band = top - (top - bottom) * 0.15f;
    Vec3 sum{};
    int n = 0;
    for (const auto& v : model.vertices) {
        const Vec3 p = transform.TransformPoint(v.position);
        if (p.z >= band) { sum = sum + p; ++n; }
    }
    return n ? sum * (1.0f / (float)n) : Vec3{ transform.m[3][0], transform.m[3][1], top };
}

float Falloff(float distance, float radius) {
    if (distance >= radius) return 0.0f;
    const float t = 1.0f - distance / radius;
    return t * t;
}

} // namespace

bool IsLamp(const char* name) {
    std::string n = name ? name : "";
    for (char& c : n) c = (char)std::toupper((unsigned char)c);
    if (n.find("TRAFFIC") != std::string::npos) return false;
    return n.find("STREETLIGHT") != std::string::npos || n.find("LAMP") != std::string::npos ||
           n.find("STREETLAMP") != std::string::npos;
}

Environment Measure(const nfs::World& world, const Aabb& area, const Settings& settings) {
    Environment environment;

    // --- lamps within reach -------------------------------------------------------
    Aabb reach = area;
    reach.min = reach.min - Vec3{ settings.lampRadius, settings.lampRadius, 30.0f };
    reach.max = reach.max + Vec3{ settings.lampRadius, settings.lampRadius, 30.0f };
    ForEachPlaced(world, reach, [&](const nfs::SolidObject& model, const Mat4& transform) {
        if (!IsLamp(model.name.c_str()) || model.vertices.empty()) return;
        Lamp lamp;
        lamp.position = LampHead(model, transform);
        lamp.base = { transform.m[3][0], transform.m[3][1], transform.m[3][2] };
        for (const Lamp& other : environment.lamps)   // one per post: a lamp is often stored twice
            if (Length(other.position - lamp.position) < 0.5f) return;
        environment.lamps.push_back(lamp);
    });

    // --- ambient: the darker colours of the map's meshes around -------------------
    Aabb around = area;
    around.min = around.min - Vec3{ settings.searchRadius, settings.searchRadius, 15.0f };
    around.max = around.max + Vec3{ settings.searchRadius, settings.searchRadius, 15.0f };
    std::vector<Vec3> samples;
    ForEachPlaced(world, around, [&](const nfs::SolidObject& model, const Mat4& transform) {
        if (IsLamp(model.name.c_str())) return;
        const size_t step = std::max<size_t>(1, model.vertices.size() / 400);
        for (size_t i = 0; i < model.vertices.size(); i += step) {
            const Vec3 p = transform.TransformPoint(model.vertices[i].position);
            if (p.x < around.min.x || p.x > around.max.x || p.y < around.min.y || p.y > around.max.y) continue;
            samples.push_back(ColourOf(model.vertices[i].color));
        }
    });
    environment.ambientSamples = samples.size();
    if (samples.size() >= 20) {
        std::sort(samples.begin(), samples.end(), [](const Vec3& a, const Vec3& b) { return Luma(a) < Luma(b); });
        Vec3 sum{};
        const size_t from = samples.size() * 25 / 100, to = std::max(from + 1, samples.size() * 40 / 100);
        for (size_t i = from; i < to; ++i) sum = sum + samples[i];
        environment.ambient = sum * (1.0f / (float)(to - from));
    }

    // --- a lamp's strength: the pool the game baked under it ----------------------
    std::vector<Vec3> pool;
    float heights = 0.0f;
    for (const Lamp& lamp : environment.lamps) {
        heights += lamp.position.z - lamp.base.z;
        Aabb under;
        under.Add(lamp.base - Vec3{ 4.0f, 4.0f, 3.0f });
        under.Add(lamp.base + Vec3{ 4.0f, 4.0f, 3.0f });
        ForEachPlaced(world, under, [&](const nfs::SolidObject& model, const Mat4& transform) {
            if (IsLamp(model.name.c_str())) return;
            for (const auto& v : model.vertices) {
                const Vec3 p = transform.TransformPoint(v.position);
                if (std::fabs(p.x - lamp.base.x) < 4.0f && std::fabs(p.y - lamp.base.y) < 4.0f &&
                    std::fabs(p.z - lamp.base.z) < 3.0f)
                    pool.push_back(ColourOf(v.color));
            }
        });
    }
    environment.poolSamples = pool.size();
    if (!environment.lamps.empty()) environment.lampHeight = std::max(2.0f, heights / (float)environment.lamps.size());
    if (pool.size() >= 10) {
        std::sort(pool.begin(), pool.end(), [](const Vec3& a, const Vec3& b) { return Luma(a) > Luma(b); });
        Vec3 sum{};
        const size_t count = std::max<size_t>(1, pool.size() / 4);
        for (size_t i = 0; i < count; ++i) sum = sum + pool[i];
        const Vec3 peak = sum * (1.0f / (float)count);
        // What the lamp adds at the ground under it, which is its own height away.
        Vec3 added{ std::max(0.0f, peak.x - environment.ambient.x), std::max(0.0f, peak.y - environment.ambient.y),
                    std::max(0.0f, peak.z - environment.ambient.z) };
        const float atGround = Falloff(environment.lampHeight, settings.lampRadius);
        if (atGround > 0.01f && Luma(added) > 0.02f) environment.lampColour = added * (1.0f / atGround);
    }
    LOG_INFO("light bake: ambient (%.2f %.2f %.2f) from %zu samples, %zu lamps, lamp (%.2f %.2f %.2f) from %zu pool "
             "samples, head %.1f up",
             environment.ambient.x, environment.ambient.y, environment.ambient.z, environment.ambientSamples,
             environment.lamps.size(), environment.lampColour.x, environment.lampColour.y, environment.lampColour.z,
             environment.poolSamples, environment.lampHeight);
    return environment;
}

std::vector<u32> Bake(const nfs::SolidObject& model, const Mat4& transform, const Environment& environment,
                      const Settings& settings) {
    const size_t count = model.vertices.size();
    // Normals in the world: the stored ones, or rebuilt from the triangles.
    std::vector<Vec3> normals(count, Vec3{ 0, 0, 0 });
    if (model.hasNormals) {
        for (size_t i = 0; i < count; ++i) normals[i] = transform.TransformDirection(model.vertices[i].normal);
    } else {
        for (size_t t = 0; t + 2 < model.indices.size(); t += 3) {
            const u16 a = model.indices[t], b = model.indices[t + 1], c = model.indices[t + 2];
            if (a >= count || b >= count || c >= count) continue;
            const Vec3 n = Cross(model.vertices[b].position - model.vertices[a].position,
                                 model.vertices[c].position - model.vertices[a].position);
            normals[a] = normals[a] + n;
            normals[b] = normals[b] + n;
            normals[c] = normals[c] + n;
        }
        for (Vec3& n : normals) n = transform.TransformDirection(n);
    }

    std::vector<u32> colours(count);
    const Vec3 ambient = environment.ambient * settings.ambientScale;
    for (size_t i = 0; i < count; ++i) {
        const Vec3 p = transform.TransformPoint(model.vertices[i].position);
        const Vec3 n = Length(normals[i]) > 1e-6f ? Normalize(normals[i]) : Vec3{ 0, 0, 1 };
        Vec3 c = ambient;
        for (const Lamp& lamp : environment.lamps) {
            const Vec3 toLamp = lamp.position - p;
            const float distance = Length(toLamp);
            const float f = Falloff(distance, settings.lampRadius);
            if (f <= 0.0f) continue;
            // Facing the lamp counts most; the soft floor keeps the far side from going black.
            const float facing = distance > 1e-4f ? std::max(0.0f, Dot(n, toLamp * (1.0f / distance))) : 1.0f;
            c = c + environment.lampColour * (settings.lampScale * f * (0.35f + 0.65f * facing));
        }
        const auto channel = [](float v) { return (u32)std::clamp(v * 128.0f, 0.0f, 255.0f); };
        const u32 alpha = model.vertices[i].color & 0xFF000000u;
        colours[i] = alpha | channel(c.x) << 16 | channel(c.y) << 8 | channel(c.z);
    }
    return colours;
}

} // namespace lightbake
