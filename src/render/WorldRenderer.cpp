#include "render/WorldRenderer.h"

#include "core/Log.h"
#include "render/RenderExtension.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace {

// Opaque mid grey. Drawn in place of a texture that could not be decoded, which
// reads as "asset missing" instead of vanishing into the night lighting.
constexpr D3DCOLOR kMissingTextureColor = 0xFF9A9A9A;

// Converts our row-major Mat4 into the D3DMATRIX the fixed-function pipeline
// expects. Both are row-major with translation in the last row, so the copy is
// a straight reinterpretation.
D3DMATRIX ToD3D(const Mat4& source) {
    D3DMATRIX result;
    std::memcpy(&result, source.m, sizeof(result));
    return result;
}

double SecondsNow() {
    LARGE_INTEGER frequency, counter;
    ::QueryPerformanceFrequency(&frequency);
    ::QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
}

} // namespace

void WorldRenderer::Clear() {
    objectByHash_.clear();
    for (RenderExtension* extension : extensions_) extension->ReleaseGpu();
    selectedOwner_ = nullptr;
    selectedExtra_ = -1;
    ReleaseThumbnailTargets();
    sections_.clear();
    // The selection names GPU slots of the world being dropped.
    selection_.clear();
    collisionSelection_.clear();
    collisionDrawn_.clear();
    uploadedSections_ = 0;
    unresolvedInstances_ = 0;
    textures_.SetWorld(nullptr);
    minimapPreview_.Release();   // MINIMAP PREVIEW (temporary)
    world_  = nullptr;
    bounds_ = {};
    stats_  = {};
}

void WorldRenderer::SyncWithWorld(const nfs::World& world) {
    if (!context_ || !context_->IsValid()) return;
    objectByHash_.clear();

    if (world_ != &world) {
        sections_.clear();
        uploadedSections_ = 0;
        bounds_ = {};
        world_ = &world;
        textures_.SetWorld(&world);
    }

    // Keep a one-to-one mapping with the world's sections. A section may hold
    // scenery and no geometry of its own, and its instances still have to be
    // drawn, so an entry is kept even when there is nothing to upload.
    const auto& loaded = world.Loaded();
    sections_.resize(loaded.size());

    for (size_t i = uploadedSections_; i < loaded.size(); ++i) {
        SectionRender& target = sections_[i];
        target.alternate = loaded[i]->info && world.IsUnusedAlternate(*loaded[i]->info);
        if (loaded[i]->info) {
            target.name = loaded[i]->info->name;
            target.number = loaded[i]->info->number;
            target.tablePosition = loaded[i]->info->position;
            target.positioned = true;
        }

        auto gpu = std::make_unique<GpuSection>();
        if (gpu->Upload(context_->Device(), *loaded[i])) {
            target.libraryBounds = gpu->Bounds();
            target.gpu = std::move(gpu);
        }

        auto collision = std::make_unique<CollisionMesh>();
        if (collision->Upload(context_->Device(), *loaded[i], showRaceGroups_))
            target.collision = std::move(collision);

        for (const nfs::SceneryInstance& instance : loaded[i]->scenery.instances)
            target.instanceBounds.Add(instance.worldBounds);

        bounds_.Add(target.instanceBounds);
    }
    uploadedSections_ = loaded.size();

    BuildInstances(world);

    u32 vertices = 0, triangles = 0;
    size_t instances = 0;
    for (const SectionRender& section : sections_) {
        if (section.gpu) {
            vertices  += section.gpu->VertexCount();
            triangles += section.gpu->TriangleCount();
        }
        instances += section.instances.size();
    }
    size_t collisionPolygons = 0, collisionBarriers = 0, collisionSections = 0;
    for (const SectionRender& section : sections_) {
        if (!section.collision) continue;
        collisionSections++;
        collisionPolygons += section.collision->PolygonCount();
        collisionBarriers += section.collision->BarrierCount();
    }
    LOG_INFO("collision: %zu sections, %zu polygons of which %zu barriers",
             collisionSections, collisionPolygons, collisionBarriers);

    size_t alternateSections = 0, alternateInstances = 0;
    for (const SectionRender& section : sections_) {
        if (!section.alternate) continue;
        alternateSections++;
        alternateInstances += section.instances.size();
    }

    LOG_INFO("uploaded %zu sections: %u vertices, %u triangles, %zu instances drawn "
             "(%zu unresolved, %zu redundant)",
             sections_.size(), vertices, triangles, instances,
             unresolvedInstances_, duplicateInstances_);
    LOG_INFO("  of those, %zu sections holding %zu instances are the alternate set",
             alternateSections, alternateInstances);
}

void WorldRenderer::BuildInstances(const nfs::World& world) {
    unresolvedInstances_ = 0;
    duplicateInstances_  = 0;

    // Where each model ended up on the GPU.
    std::unordered_map<u32, std::pair<u32, u32>> modelIndex;
    std::unordered_set<u64> placements;

    for (size_t s = 0; s < sections_.size(); ++s) {
        const GpuSection* gpu = sections_[s].gpu.get();
        if (!gpu) continue;
        const auto& objects = gpu->Objects();
        for (size_t o = 0; o < objects.size(); ++o) {
            if (objects[o].source)
                modelIndex.emplace(objects[o].source->hash, std::make_pair((u32)s, (u32)o));
        }
    }

    const auto& loaded = world.Loaded();
    for (size_t i = 0; i < loaded.size(); ++i) {
        const nfs::Scenery& scenery = loaded[i]->scenery;
        std::vector<GpuInstance>& out = sections_[i].instances;
        out.clear();
        out.reserve(scenery.instances.size());

        const auto& removed = loaded[i]->removedNatives;
        for (size_t n = 0; n < scenery.instances.size(); ++n) {
            const nfs::SceneryInstance& instance = scenery.instances[n];
            if (std::find(removed.begin(), removed.end(),
                          std::make_pair(instance.sceneryNumber, instance.record)) != removed.end())
                continue;
            const u32 modelHash = scenery.ModelHash(instance);
            const auto it = modelIndex.find(modelHash);
            if (it == modelIndex.end()) {
                unresolvedInstances_++;
                continue;
            }

            const nfs::SolidObject* model =
                sections_[it->second.first].gpu->Objects()[it->second.second].source;

            // Every record places its mesh, with no special case for terrain.
            //
            // There was one here, built on the belief that section Q38 listed the
            // same ground tile six times twelve units apart. It did not: those
            // records named trees and lamp posts, and a missing index base was
            // resolving them to terrain. With the base applied the duplication is
            // gone, and a special case that skipped the record took the asphalt
            // out from under the lane markings.
            const Mat4 transform = instance.transform;

            // Deduplicated on the transform the mesh is actually drawn with.
            // Neighbouring sections list the same shared prop, and a
            // pre-placed mesh is named by every record that describes it, so
            // without this the ground would still be drawn once per record.
            if (!placements.insert(nfs::PlacementKey(modelHash, transform)).second) {
                duplicateInstances_++;
                continue;
            }

            GpuInstance gpuInstance;
            gpuInstance.section     = it->second.first;
            gpuInstance.object      = it->second.second;
            gpuInstance.transform   = transform;
            gpuInstance.mirrored    = Determinant3x3(transform) < 0.0f;
            gpuInstance.native      = (int)n;

            // Built from the mesh, not taken from the instance record.
            //
            // The pair of vectors at the start of that record is not the
            // object's box: for the airport parking building the mesh is
            // 75x173x12 while the record's pair spans 8x8x21, and no scale
            // reconciles the two. Their midpoint is the instance position, so
            // they are something else. Culling against them decides what to
            // draw from a number that has nothing to do with where the mesh
            // actually is.
            if (model && model->bounds.Valid()) {
                for (int corner = 0; corner < 8; ++corner) {
                    const Vec3 point{
                        (corner & 1) ? model->bounds.max.x : model->bounds.min.x,
                        (corner & 2) ? model->bounds.max.y : model->bounds.min.y,
                        (corner & 4) ? model->bounds.max.z : model->bounds.min.z };
                    gpuInstance.worldBounds.Add(transform.TransformPoint(point));
                }
            }
            out.push_back(gpuInstance);
        }

        // The editor's placements, never deduplicated: each was put there on
        // purpose, even on top of something else.
        const auto& edited = loaded[i]->placements;
        for (size_t p = 0; p < edited.size(); ++p) {
            const nfs::PlacedInstance& placed = edited[p].instance;
            const auto it = modelIndex.find(placed.modelHash);
            if (it == modelIndex.end()) { unresolvedInstances_++; continue; }

            const nfs::SolidObject* model =
                sections_[it->second.first].gpu->Objects()[it->second.second].source;

            GpuInstance gpuInstance;
            gpuInstance.section   = it->second.first;
            gpuInstance.object    = it->second.second;
            gpuInstance.transform = placed.transform;
            gpuInstance.mirrored  = Determinant3x3(placed.transform) < 0.0f;
            gpuInstance.placement = (int)p;
            if (model && model->bounds.Valid())
                gpuInstance.worldBounds = Transform(model->bounds, placed.transform);
            out.push_back(gpuInstance);
            sections_[i].instanceBounds.Add(gpuInstance.worldBounds);
        }
    }
}

void WorldRenderer::RebuildInstances(const nfs::World& world) {
    if (world_ != &world) return;
    BuildInstances(world);
}

void WorldRenderer::UpdatePlacement(u32 loadedSection, int placement, const Mat4& transform) {
    if (loadedSection >= sections_.size()) return;
    for (GpuInstance& instance : sections_[loadedSection].instances) {
        if (instance.placement != placement) continue;
        const GpuSection* owner = sections_[instance.section].gpu.get();
        const nfs::SolidObject* model =
            owner && instance.object < owner->Objects().size() ? owner->Objects()[instance.object].source
                                                                : nullptr;
        instance.transform = transform;
        instance.mirrored  = Determinant3x3(transform) < 0.0f;
        if (model && model->bounds.Valid()) {
            instance.worldBounds = Transform(model->bounds, transform);
            sections_[loadedSection].instanceBounds.Add(instance.worldBounds);
        }
        for (PickResult& picked : selection_) {
            if (picked.loadedSection != loadedSection || picked.placement != placement) continue;
            picked.transform = transform;
            picked.worldBounds = instance.worldBounds;
        }
    }
}

// The other winding, for placements that mirror their mesh.
DWORD OppositeCull(DWORD mode) {
    return mode == D3DCULL_CW ? (DWORD)D3DCULL_CCW : (DWORD)D3DCULL_CW;
}

void WorldRenderer::ApplyRenderStates() {
    IDirect3DDevice9* device = context_->Device();

    device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    // Fences, foliage, railings and signs are cut out through the texture alpha
    // rather than modelled, so without an alpha test they show up as opaque
    // rectangles. A plain test avoids the draw ordering that blending would
    // require, and textures with no alpha are fully opaque and pass anyway.
    device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    device->SetRenderState(D3DRS_ALPHAREF, 128);

    // Stands in for a missing texture, and supplies opaque alpha wherever the
    // texture alpha is not the right source.
    device->SetRenderState(D3DRS_TEXTUREFACTOR, kMissingTextureColor);

    // Cull clockwise, not counter-clockwise.
    //
    // Direct3D culls by the winding a triangle ends up with on screen, and the
    // right-handed view and projection used here flip that parity compared with
    // the left-handed matrices the API assumes by default. The track meshes are
    // wound so that D3DCULL_CW discards their back faces; D3DCULL_CCW discards
    // the front ones instead, which leaves every surface in the city inside out.
    device->SetRenderState(D3DRS_CULLMODE,
                           backfaceCulling_ ? cullMode_ : D3DCULL_NONE);

    device->SetRenderState(D3DRS_FILLMODE,
                           mode_ == RenderMode::Wireframe ? D3DFILL_WIREFRAME : D3DFILL_SOLID);

    // Lighting is already baked into the vertex colours, so the dynamic lighting
    // pipeline stays off and the colours are used as they are.
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);

    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 4);

    device->SetFVF(kWorldVertexFvf);
}

void WorldRenderer::BindTextureStage(IDirect3DDevice9* device, const CachedTexture& cached) {
    IDirect3DTexture9* texture = cached.texture;
    device->SetTexture(0, texture);

    // Colour comes from the texture when it decoded and from the flat factor
    // colour when it did not, so an undecodable texture is obvious rather than
    // invisible. The lighting-only modes ignore both.
    const DWORD colorSource = texture ? (DWORD)D3DTA_TEXTURE : (DWORD)D3DTA_TFACTOR;

    switch (mode_) {
        case RenderMode::Textured:
            device->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_MODULATE2X);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, colorSource);
            device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            break;

        case RenderMode::Unlit:
            device->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, colorSource);
            break;

        default:
            device->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            break;
    }

    // Alpha is opacity only where the texture's header says so. An opaque
    // material takes the solid alpha of the factor colour, so the alpha test
    // always passes: the road textures keep gloss in their alpha, and testing
    // against it erases the asphalt and leaves only the lane markings.
    const bool alphaIsOpacity = texture && cached.blend != SurfaceBlend::Opaque;
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1,
                                 alphaIsOpacity ? D3DTA_TEXTURE : D3DTA_TFACTOR);
}

bool WorldRenderer::DrawObject(IDirect3DDevice9* device, const GpuObject& object,
                               RenderStats& stats, DrawPass pass) {
    const bool wantsTexture = mode_ == RenderMode::Textured || mode_ == RenderMode::Unlit;
    bool other = false;

    for (const GpuDraw& draw : object.draws) {
        if (wantsTexture) {
            const CachedTexture& texture = textures_.Get(device, AnimatedTexture(draw.textureHash));
            // A texture that failed to decode is drawn as the grey stand-in,
            // which is solid whatever the header asked for.
            const bool translucent = texture.texture && texture.Translucent();
            if (translucent != (pass == DrawPass::Translucent)) { other = true; continue; }

            if (pass == DrawPass::Translucent)
                device->SetRenderState(D3DRS_DESTBLEND,
                                       texture.blend == SurfaceBlend::Additive
                                           ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
            BindTextureStage(device, texture);
            ScrollTextureStage(device, texture);
        } else if (pass == DrawPass::Translucent) {
            continue;   // the lighting-only views draw everything solid
        }

        device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,
                                     (INT)object.baseVertex,
                                     draw.minVertex,
                                     draw.vertexCount,
                                     draw.startIndex,
                                     draw.primitiveCount);

        stats.drawCalls++;
        stats.triangles += draw.primitiveCount;
    }
    // Nothing drawn after this one should inherit the scroll (ImGui included).
    if (scrolling_) ScrollTextureStage(device, CachedTexture{});
    return other;
}

// The game's texture scrolling (TextureInfo::GetScroll): the fraction of
// time * speed, time counted in whole steps for the stepped kind.
void WorldRenderer::ScrollTextureStage(IDirect3DDevice9* device, const CachedTexture& texture) {
    const bool scroll = animate_ && texture.scrollType != 0;
    if (!scroll) {
        if (scrolling_) {
            device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
            scrolling_ = false;
        }
        return;
    }
    double t = animationClock_;
    if (texture.scrollType == 2) t = std::floor(t / texture.scrollStep);
    const auto fraction = [](double v) { return (float)(v - std::floor(v)); };
    D3DMATRIX m{};
    m._11 = m._22 = m._33 = m._44 = 1.0f;
    // Two texture coordinates: the third row moves them.
    m._31 = fraction(t * texture.scrollS);
    m._32 = fraction(t * texture.scrollT);
    device->SetTransform(D3DTS_TEXTURE0, &m);
    if (!scrolling_) device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
    scrolling_ = true;
}

void WorldRenderer::QueueTranslucent(const GpuObject& object, size_t section, const Mat4& transform,
                                     bool mirrored, const Aabb& bounds, const Vec3& eye) {
    const Vec3 centre = bounds.Valid() ? (bounds.min + bounds.max) * 0.5f
                                       : Vec3{ transform.m[3][0], transform.m[3][1], transform.m[3][2] };
    const Vec3 offset = centre - eye;
    translucent_.push_back({ &object, section, transform, mirrored, Dot(offset, offset) });
}

void WorldRenderer::BeginTranslucentStates(IDirect3DDevice9* device) {
    // Tested and blended both: a texel with no opacity at all is skipped
    // outright, and the rest is mixed in without touching the depth buffer, so
    // a glow never hides what stands behind it.
    device->SetRenderState(D3DRS_ALPHAREF, 4);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
}

void WorldRenderer::EndTranslucentStates(IDirect3DDevice9* device) {
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHAREF, 128);
}

void WorldRenderer::DrawTranslucent(IDirect3DDevice9* device, RenderStats& stats) {
    if (translucent_.empty()) return;

    // Farthest first, so nearer smoke and tape blend over what is behind them.
    std::sort(translucent_.begin(), translucent_.end(),
              [](const TranslucentObject& a, const TranslucentObject& b) {
                  return a.distance > b.distance;
              });

    BeginTranslucentStates(device);
    for (const TranslucentObject& item : translucent_) {
        const GpuSection& gpu = *sections_[item.section].gpu;
        device->SetStreamSource(0, gpu.VertexBuffer(), 0, sizeof(nfs::SolidVertex));
        device->SetIndices(gpu.IndexBuffer());
        if (backfaceCulling_)
            device->SetRenderState(D3DRS_CULLMODE,
                                   item.mirrored ? OppositeCull(cullMode_) : cullMode_);
        const D3DMATRIX world = ToD3D(item.transform);
        device->SetTransform(D3DTS_WORLD, &world);
        DrawObject(device, *item.object, stats, DrawPass::Translucent);
    }
    EndTranslucentStates(device);
    device->SetRenderState(D3DRS_CULLMODE, backfaceCulling_ ? cullMode_ : D3DCULL_NONE);
    translucent_.clear();
}

void WorldRenderer::Render(const Camera& camera) {
    if (!context_ || !context_->IsValid()) return;

    const double start = SecondsNow();

    IDirect3DDevice9* device = context_->Device();
    const float aspect = context_->Height()
                             ? (float)context_->Width() / (float)context_->Height()
                             : 1.0f;

    const Mat4 view       = camera.ViewMatrix();
    const Mat4 projection = camera.ProjectionMatrix(aspect);

    const D3DMATRIX d3dView       = ToD3D(view);
    const D3DMATRIX d3dProjection = ToD3D(projection);
    device->SetTransform(D3DTS_VIEW, &d3dView);
    device->SetTransform(D3DTS_PROJECTION, &d3dProjection);

    Frustum frustum;
    frustum.Build(view * projection);

    // A mirrored view swaps which winding faces the camera.
    cullMode_ = camera.ViewIsMirrored() ? D3DCULL_CCW : D3DCULL_CW;

    ApplyRenderStates();
    animationClock_ = start;

    // The sky first, behind everything.
    RenderStats skyStats;
    if (showSky_) DrawSky(device, camera, skyStats);

    // What the game would draw from here. Falling back to distance keeps the
    // viewport usable above the rooftops, where no zone claims the camera.
    bool usingVisibility = false;
    if (streaming_ && world_) {
        const Vec3 eye = camera.Position();
        const float moved = (eye.x - visibleFrom_.x) * (eye.x - visibleFrom_.x) +
                            (eye.y - visibleFrom_.y) * (eye.y - visibleFrom_.y);
        if (!haveVisible_ || moved > 4.0f) {
            // The zones do not tile the ground without gaps, and a camera that
            // steps on a seam should not empty the world for a frame. Keeping
            // the last set until another zone claims the camera is what the
            // game's own streaming does, and it stops the view flickering.
            std::vector<u32> found;
            if (world_->Visibility().VisibleAt(eye, found)) {
                visibleSections_.swap(found);
                haveVisible_ = true;
            }
            visibleFrom_ = eye;

            // Logged on every change of zone, not every frame: which zone the
            // camera is in decides the whole picture, and when the picture
            // looks wrong this is the first thing worth knowing.
            static size_t lastCount = (size_t)-1;
            static bool   lastClaimed = false;
            if (visibleSections_.size() != lastCount || haveVisible_ != lastClaimed) {
                lastCount = visibleSections_.size();
                lastClaimed = haveVisible_;
                LOG_INFO("camera (%.0f, %.0f, %.0f): %s, %zu sections visible",
                         eye.x, eye.y, eye.z,
                         haveVisible_ ? "inside a zone" : "no zone claims it, using distance",
                         visibleSections_.size());
            }
        }
        usingVisibility = haveVisible_ && !visibleSections_.empty();
    }

    const bool wantsTexture = mode_ == RenderMode::Textured || mode_ == RenderMode::Unlit;
    if (!wantsTexture) BindTextureStage(device, CachedTexture{});
    translucent_.clear();
    const Vec3 eye = camera.Position();

    RenderStats stats;
    stats.sectionsTotal = (u32)sections_.size();

    // Which section's buffers are currently bound. Instances reach into other
    // sections, so the binding is tracked rather than set once per section.
    size_t boundSection = (size_t)-1;
    auto bind = [&](size_t index) {
        if (boundSection == index) return;
        const GpuSection& gpu = *sections_[index].gpu;
        device->SetStreamSource(0, gpu.VertexBuffer(), 0, sizeof(nfs::SolidVertex));
        device->SetIndices(gpu.IndexBuffer());
        boundSection = index;
    };

    std::vector<size_t> collisionVisible;

    for (size_t i = 0; i < sections_.size(); ++i) {
        const SectionRender& section = sections_[i];

        if (section.gpu) stats.objectsTotal += (u32)section.gpu->Objects().size();
        stats.instancesTotal += (u32)section.instances.size();

        if (section.alternate && !showAlternate_) continue;

        // Collision is culled on its own footprint: a section's instances can
        // sit far from its ground, and the section may place nothing at all.
        if (showCollision_ && section.collision) {
            const Aabb& bounds = section.collision->Bounds();
            bool keep = true;
            if (usingVisibility)
                keep = std::binary_search(visibleSections_.begin(), visibleSections_.end(),
                                          section.number);
            else if (streaming_) {
                const Vec3 centre = (bounds.min + bounds.max) * 0.5f;
                const float dx = centre.x - camera.Position().x;
                const float dy = centre.y - camera.Position().y;
                keep = dx * dx + dy * dy <= streamRadius_ * streamRadius_;
            }
            if (keep && culling_ && !frustum.Intersects(bounds)) keep = false;
            if (keep) collisionVisible.push_back(i);
        }

        const Aabb cullBounds = section.CullBounds(showLibrary_);

        if (usingVisibility) {
            if (!std::binary_search(visibleSections_.begin(), visibleSections_.end(),
                                    section.number))
                continue;
        }
        // Distance is measured on the ground plane: the camera is often well
        // above the street, and counting that height in would drop the
        // surrounding blocks the moment anyone looked down at the city.
        else if (streaming_ && cullBounds.Valid()) {
            const Vec3 centre = (cullBounds.min + cullBounds.max) * 0.5f;
            const float dx = centre.x - camera.Position().x;
            const float dy = centre.y - camera.Position().y;
            if (dx * dx + dy * dy > streamRadius_ * streamRadius_) continue;
        }
        if (culling_ && cullBounds.Valid() && !frustum.Intersects(cullBounds)) continue;
        stats.sectionsVisible++;

        // The stored meshes, at the storage transforms in their headers. This is
        // the model library and it is not part of the world; see LibraryVisible.
        if (showLibrary_ && section.gpu) {
            const auto& objects = section.gpu->Objects();
            for (const GpuObject& object : objects) {
                if (object.isBackdrop && !showBackdrop_) continue;
                if (HiddenBuilding(object)) continue;   // MINIMAP PREVIEW (temporary)
                if (HiddenModel(object)) continue;
                // An object with no stored bounds cannot be culled, only drawn:
                // testing its empty-box sentinel would reject it every frame.
                if (culling_ && object.worldBounds.Valid() &&
                    !frustum.Intersects(object.worldBounds)) continue;

                stats.objectsVisible++;
                bind(i);

                const D3DMATRIX world = ToD3D(object.transform);
                device->SetTransform(D3DTS_WORLD, &world);
                if (DrawObject(device, object, stats))
                    QueueTranslucent(object, i, object.transform, false, object.worldBounds, eye);
            }
        }

        if (!showScenery_) continue;

        for (const GpuInstance& instance : section.instances) {
            if (culling_ && instance.worldBounds.Valid() &&
                !frustum.Intersects(instance.worldBounds)) continue;

            const GpuObject& object = sections_[instance.section].gpu->Objects()[instance.object];
            if (object.isBackdrop && !showBackdrop_) continue;
            if (HiddenBuilding(object)) continue;   // MINIMAP PREVIEW (temporary)
            if (HiddenModel(object)) continue;

            stats.instancesVisible++;
            bind(instance.section);

            if (backfaceCulling_)
                device->SetRenderState(D3DRS_CULLMODE,
                                       instance.mirrored ? OppositeCull(cullMode_)
                                                         : cullMode_);

            const D3DMATRIX world = ToD3D(instance.transform);
            device->SetTransform(D3DTS_WORLD, &world);
            if (DrawObject(device, object, stats))
                QueueTranslucent(object, instance.section, instance.transform, instance.mirrored,
                                 instance.worldBounds, eye);
        }
    }

    // MINIMAP PREVIEW (temporary)
    if (showMinimap_) {
        minimapPreview_.Draw(device, backfaceCulling_ ? cullMode_ : D3DCULL_NONE,
                             mode_ == RenderMode::Wireframe);
        device->SetFVF(kWorldVertexFvf);
    }

    if (showScenery_) DrawObjectAnimations(device, usingVisibility, eye, stats);
    DrawTranslucent(device, stats);
    for (RenderExtension* extension : extensions_) extension->DrawWithWorld(device, *this);
    DrawSelection(device);
    if (showCollision_) DrawCollision(device, collisionVisible);
    collisionDrawn_ = showCollision_ ? collisionVisible : std::vector<size_t>{};
    for (RenderExtension* extension : extensions_) extension->DrawOverlay(device, camera, *this);
    DrawToolOverlay(device);
    DrawGizmo(device, camera);

    device->SetTexture(0, nullptr);

    stats.frameMilliseconds = (float)((SecondsNow() - start) * 1000.0);
    stats_ = stats;
}

// ---------------------------------------------------------------------------
// Picking
// ---------------------------------------------------------------------------

void WorldRenderer::PickOne(const Ray& ray, const GpuObject& object, const Mat4& transform,
                            const Aabb& worldBounds, bool fromInstance, size_t sectionIndex,
                            u32 ownerSection, u32 ownerObject, PickResult& best,
                            int placement, int native) const {
    if (!object.source) return;
    if (placement >= 0 && placement == excludePlacement_ && sectionIndex == excludeSection_) return;
    if (object.isBackdrop && !showBackdrop_) return;
    if (HiddenBuilding(object)) return;   // MINIMAP PREVIEW (temporary)
    if (HiddenModel(object)) return;

    // A bounds test rejects almost everything; only survivors pay for triangles.
    // Objects with no bounds have to be tested directly.
    if (worldBounds.Valid() && IntersectRayAabb(ray, worldBounds) < 0.0f) return;

    const Mat4 toLocal = InverseAffine(transform);
    Ray local;
    local.origin    = toLocal.TransformPoint(ray.origin);
    local.direction = toLocal.TransformDirection(ray.direction);

    const nfs::SolidObject& source = *object.source;
    const size_t triangles = source.indices.size() / 3;

    for (size_t t = 0; t < triangles; ++t) {
        const u16 i0 = source.indices[t * 3 + 0];
        const u16 i1 = source.indices[t * 3 + 1];
        const u16 i2 = source.indices[t * 3 + 2];
        if (i0 >= source.vertices.size() || i1 >= source.vertices.size() ||
            i2 >= source.vertices.size())
            continue;

        const float hit = IntersectRayTriangle(local,
                                               source.vertices[i0].position,
                                               source.vertices[i1].position,
                                               source.vertices[i2].position);
        if (hit < 0.0f) continue;

        // The local ray is not unit length once a transform scales it, so the
        // distance has to be measured back in world space.
        const Vec3 localPoint = local.origin + local.direction * hit;
        const Vec3 worldPoint = transform.TransformPoint(localPoint);
        const float distance = Dot(worldPoint - ray.origin, ray.direction);
        if (distance <= 0.0f || (best.object && distance >= best.distance)) continue;

        best.object       = &source;
        best.transform    = transform;
        best.worldBounds  = worldBounds;
        best.distance     = distance;
        best.fromInstance = fromInstance;
        best.gpuSection   = ownerSection;
        best.gpuObject    = ownerObject;
        best.loadedSection = (u32)sectionIndex;
        best.placement    = placement;
        best.native       = native;
        best.sectionName.clear();

        if (world_ && sectionIndex < world_->Loaded().size()) {
            const nfs::Section* info = world_->Loaded()[sectionIndex]->info;
            if (info) best.sectionName = info->name;
        }
    }
}

// Checks that each instance really points at the model it claims.
//
// An instance names its model through an index into its section's info table,
// which turns into a hash, which turns into a mesh. A slip anywhere along that
// chain puts a real model, correctly placed, in a spot that belongs to another
// one -- the city fills with plausible objects in wrong places and loses the
// ones that should be there, which is exactly how it looks on screen.
//
// The file gives a way to test it: every instance stores its own world bounding
// box. Transforming the referenced model's own bounds by the instance matrix
// has to reproduce that box. Comparing the stored box with itself, which is
// what an earlier check did, proves nothing at all.
bool WorldRenderer::CheckReferences(const std::wstring& path) const {
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"w") != 0 || !file) return false;

    std::fprintf(file, "# model hash centreError sizeRatio rawError rawRatio modelIsWorldSpace section\n");

    size_t tested = 0;
    std::vector<float> errors;
    for (const SectionRender& section : sections_) {
        for (const GpuInstance& instance : section.instances) {
            if (instance.section >= sections_.size()) continue;
            const GpuSection* owner = sections_[instance.section].gpu.get();
            if (!owner || instance.object >= owner->Objects().size()) continue;

            const nfs::SolidObject* model = owner->Objects()[instance.object].source;
            if (!model || !model->bounds.Valid() || !instance.worldBounds.Valid()) continue;

            // The eight corners, so a rotated box is measured by what it really
            // covers rather than by its axis-aligned extent before rotation.
            Aabb derived;
            for (int corner = 0; corner < 8; ++corner) {
                const Vec3 point{
                    (corner & 1) ? model->bounds.max.x : model->bounds.min.x,
                    (corner & 2) ? model->bounds.max.y : model->bounds.min.y,
                    (corner & 4) ? model->bounds.max.z : model->bounds.min.z };
                derived.Add(instance.transform.TransformPoint(point));
            }

            const Vec3 storedCentre  = (instance.worldBounds.min + instance.worldBounds.max) * 0.5f;
            const Vec3 derivedCentre = (derived.min + derived.max) * 0.5f;
            const Vec3 storedSize    = instance.worldBounds.max - instance.worldBounds.min;
            const Vec3 derivedSize   = derived.max - derived.min;

            const float centreError = Length(derivedCentre - storedCentre);
            const float storedSpan  = Length(storedSize);
            const float derivedSpan = Length(derivedSize);
            const float ratio = storedSpan > 0.001f ? derivedSpan / storedSpan : 0.0f;

            // The same comparison with the model's bounds left alone. Terrain
            // is stored already standing in the world, so for those the raw
            // box is the answer and transforming it again is the mistake --
            // measuring both says which models are which instead of assuming.
            const Vec3 rawCentre = (model->bounds.min + model->bounds.max) * 0.5f;
            const Vec3 rawSize   = model->bounds.max - model->bounds.min;
            const float rawError = Length(rawCentre - storedCentre);
            const float rawSpan  = Length(rawSize);
            const float rawRatio = storedSpan > 0.001f ? rawSpan / storedSpan : 0.0f;

            ++tested;
            // Only the centre is judged. The file's box is tighter than the one
            // that comes from enclosing a rotated model's transformed corners,
            // so a size ratio above one is the normal case for anything turned
            // off the axes and says nothing about whether the placement is right.
            // A threshold would only hide the shape of the error, so the whole
            // distribution is reported instead.
            errors.push_back(centreError < rawError ? centreError : rawError);

            std::fprintf(file, "%s %08X %.3f %.4f %.3f %.4f %d %s\n",
                         model->name.c_str(), model->hash, centreError, ratio,
                         rawError, rawRatio,
                         rawError < centreError ? 1 : 0,
                         section.name.empty() ? "?" : section.name.c_str());
        }
    }

    std::fclose(file);
    std::sort(errors.begin(), errors.end());
    const auto at = [&](double q) {
        return errors.empty() ? 0.0f : errors[(size_t)(q * (errors.size() - 1))];
    };
    LOG_INFO("reference check: %zu instances, distance from the box the file stores "
             "for them -- median %.2f, p90 %.2f, p99 %.2f, worst %.1f",
             tested, at(0.5), at(0.9), at(0.99), errors.empty() ? 0.0f : errors.back());
    return true;
}

bool WorldRenderer::DumpPlacements(const std::wstring& path) const {
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"w") != 0 || !file) {
        LOG_ERROR("could not open the placement dump for writing");
        return false;
    }

    // One line per placement, so the file diffs cleanly against a list pulled
    // out of the running game. The position is the translation of the matrix
    // the renderer actually binds, not a recomputed one: a dump that took its
    // own path through the maths could agree with the game while the picture
    // still disagreed.
    std::fprintf(file, "# model hash x y z r00 r01 r02 r10 r11 r12 r20 r21 r22 source section sectionX sectionY alternate\n");

    size_t written = 0;
    for (size_t index = 0; index < sections_.size(); ++index) {
        const SectionRender& section = sections_[index];

        if (section.gpu) {
            for (const GpuObject& object : section.gpu->Objects()) {
                if (!object.source) continue;
                std::fprintf(file, "%s %08X %.3f %.3f %.3f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f library %s %.1f %.1f %d\n",
                             object.source->name.c_str(), object.source->hash,
                             object.transform.m[3][0], object.transform.m[3][1],
                             object.transform.m[3][2],
                             object.transform.m[0][0], object.transform.m[0][1],
                             object.transform.m[0][2], object.transform.m[1][0],
                             object.transform.m[1][1], object.transform.m[1][2],
                             object.transform.m[2][0], object.transform.m[2][1],
                             object.transform.m[2][2],
                             section.name.empty() ? "?" : section.name.c_str(),
                             section.tablePosition.x, section.tablePosition.y,
                             section.alternate ? 1 : 0);
                ++written;
            }
        }

        for (const GpuInstance& instance : section.instances) {
            if (instance.section >= sections_.size()) continue;
            const GpuSection* owner = sections_[instance.section].gpu.get();
            if (!owner || instance.object >= owner->Objects().size()) continue;

            const nfs::SolidObject* source = owner->Objects()[instance.object].source;
            if (!source) continue;

            // Where the mesh ends up, not the matrix's translation row. For a
            // pre-placed mesh the two are different things: the translation is
            // what remains after bringing the mesh back to its own origin, and
            // reading it as a position puts terrain in nonsense places.
            Vec3 centre = (source->bounds.min + source->bounds.max) * 0.5f;
            centre = instance.transform.TransformPoint(centre);

            std::fprintf(file, "%s %08X %.3f %.3f %.3f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f instance %s %.1f %.1f %d\n",
                         source->name.c_str(), source->hash,
                         centre.x, centre.y,
                         centre.z,
                         instance.transform.m[0][0], instance.transform.m[0][1],
                         instance.transform.m[0][2], instance.transform.m[1][0],
                         instance.transform.m[1][1], instance.transform.m[1][2],
                         instance.transform.m[2][0], instance.transform.m[2][1],
                         instance.transform.m[2][2],
                         section.name.empty() ? "?" : section.name.c_str(),
                         section.tablePosition.x, section.tablePosition.y,
                         section.alternate ? 1 : 0);
            ++written;
        }
    }

    std::fclose(file);
    LOG_INFO("placement dump written: %zu placements", written);
    return true;
}

bool WorldRenderer::Pick(const Ray& ray, PickResult& out) const {
    PickResult best;

    for (size_t i = 0; i < sections_.size(); ++i) {
        const SectionRender& section = sections_[i];
        if (section.alternate && !showAlternate_) continue;

        if (showLibrary_ && section.gpu) {
            const auto& objects = section.gpu->Objects();
            for (size_t o = 0; o < objects.size(); ++o)
                PickOne(ray, objects[o], objects[o].transform, objects[o].worldBounds,
                        false, i, (u32)i, (u32)o, best);
        }

        if (!showScenery_) continue;

        for (const GpuInstance& instance : section.instances) {
            const GpuObject& object =
                sections_[instance.section].gpu->Objects()[instance.object];
            PickOne(ray, object, instance.transform, instance.worldBounds, true, i,
                    instance.section, instance.object, best, instance.placement, instance.native);
        }
    }

    out = best;
    return best.Hit();
}

// Two placements are the same selection when they are the same mesh put in the
// same spot; a model drawn twice nearby is two different things to pick.
bool WorldRenderer::ToggleSelection(const PickResult& selection) {
    collisionSelection_.clear();
    selectedOwner_ = nullptr; selectedExtra_ = -1;
    const auto same = [&](const PickResult& other) {
        return other.object == selection.object &&
               std::memcmp(&other.transform, &selection.transform, sizeof(Mat4)) == 0;
    };

    const auto it = std::find_if(selection_.begin(), selection_.end(), same);
    if (it != selection_.end()) {
        selection_.erase(it);
        return false;
    }

    selection_.push_back(selection);
    return true;
}

void WorldRenderer::DrawSelection(IDirect3DDevice9* device) {
    if (selection_.empty()) return;

    // The meshes themselves are painted rather than boxed. A box says where an
    // object roughly is; the paint says which object, which is what someone
    // reporting a wrong model needs when the thing sits among a dozen
    // neighbours -- and with several selected, boxes would be a thicket.
    constexpr D3DCOLOR kSelectionColour = 0xA030FF90;   // alpha carries the tint

    device->SetTexture(0, nullptr);
    device->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    device->SetRenderState(D3DRS_TEXTUREFACTOR, kSelectionColour);

    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    // Drawn over the surface it belongs to, not into the depth buffer: equal
    // depth passes so the paint lands exactly on the object's own pixels, and
    // leaving the buffer alone keeps it from cutting into whatever draws next.
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

    // Both faces, because a selected object is often seen from inside or edge
    // on, and half a painted shell reads as a hole.
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetFVF(kWorldVertexFvf);

    for (const PickResult& picked : selection_) {
        if (picked.gpuSection >= sections_.size()) continue;
        const GpuSection* owner = sections_[picked.gpuSection].gpu.get();
        if (!owner || picked.gpuObject >= owner->Objects().size()) continue;

        const GpuObject& object = owner->Objects()[picked.gpuObject];

        const D3DMATRIX world = ToD3D(picked.transform);
        device->SetTransform(D3DTS_WORLD, &world);
        device->SetStreamSource(0, owner->VertexBuffer(), 0, sizeof(nfs::SolidVertex));
        device->SetIndices(owner->IndexBuffer());

        for (const GpuDraw& draw : object.draws)
            device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, (INT)object.baseVertex,
                                         draw.minVertex, draw.vertexCount,
                                         draw.startIndex, draw.primitiveCount);
    }

    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    device->SetRenderState(D3DRS_CULLMODE, backfaceCulling_ ? cullMode_ : D3DCULL_NONE);
}

void WorldRenderer::DrawCollision(IDirect3DDevice9* device, const std::vector<size_t>& visible) {
    if (visible.empty()) return;

    device->SetTexture(0, nullptr);
    device->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    const D3DMATRIX identity = ToD3D(Mat4::Identity());
    device->SetTransform(D3DTS_WORLD, &identity);
    device->SetFVF(CollisionMesh::kFvf);

    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    // Tested against the world so a building still hides the road behind it,
    // but not written: translucent layers would otherwise punch holes in one
    // another depending on the order the sections happen to come in.
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

    // Ground polygons have no consistent winding and walls are seen from both
    // sides, so nothing is culled.
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    for (size_t index : visible) sections_[index].collision->DrawSurfaces(device);

    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    for (size_t index : visible) sections_[index].collision->DrawOutlines(device);

    DrawCollisionSelection(device);

    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    device->SetRenderState(D3DRS_CULLMODE, backfaceCulling_ ? cullMode_ : D3DCULL_NONE);
    device->SetRenderState(D3DRS_FILLMODE,
                           mode_ == RenderMode::Wireframe ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
    device->SetFVF(kWorldVertexFvf);
}

// ---------------------------------------------------------------------------
// Collision selection
// ---------------------------------------------------------------------------

bool WorldRenderer::CollisionCorners(const CollisionPick& pick, Vec3 out[4], bool& quad) const {
    if (!world_ || pick.section >= world_->Loaded().size()) return false;
    const auto& packs = world_->Loaded()[pick.section]->collision;
    if (pick.pack >= packs.size() || pick.polygon >= packs[pick.pack].polygons.size())
        return false;

    const nfs::CollisionPack& pack = packs[pick.pack];
    const nfs::CollisionPolygon& polygon = pack.polygons[pick.polygon];
    if (polygon.barrier) {
        pack.BarrierQuad(polygon, out);
        quad = true;
        return true;
    }
    for (int c = 0; c < 4; ++c) out[c] = polygon.corners[c];
    quad = polygon.quad;
    return true;
}

u32 WorldRenderer::AnimatedTexture(u32 hash) const {
    if (!animate_ || !world_) return hash;
    const auto& animations = world_->TextureAnimations();
    if (animations.empty()) return hash;
    const auto found = animations.find(hash);
    if (found == animations.end()) return hash;
    const nfs::TextureAnimation& animation = found->second;
    const size_t frame = (size_t)(animationClock_ * animation.fps) % animation.frames.size();
    return animation.frames[frame];
}

void WorldRenderer::DrawSky(IDirect3DDevice9* device, const Camera& camera, RenderStats& stats) {
    // The dome is a backdrop model; it is drawn round the eye, so however far
    // the camera flies the sky stays as far away -- and with depth off, it is
    // behind whatever the world draws after it.
    for (size_t i = 0; i < sections_.size(); ++i) {
        if (!sections_[i].gpu) continue;
        for (const GpuObject& object : sections_[i].gpu->Objects()) {
            if (!object.source || !object.isBackdrop || object.source->name.find("SKYDOME") == std::string::npos)
                continue;
            const Aabb& local = object.source->bounds;
            const float radius = local.Valid() ? std::max({ local.max.x - local.min.x, local.max.y - local.min.y,
                                                            local.max.z - local.min.z }) * 0.5f : 1000.0f;
            // Inside the far plane, whatever size it was made.
            const float scale = std::min(1.0f, camera.FarPlane() * 0.45f / std::max(radius, 1.0f));
            static bool told = false;
            if (!told) {
                told = true;
                LOG_INFO("sky: %s, radius %.0f, drawn at scale %.3f round the camera", object.source->name.c_str(),
                         radius, scale);
                for (u32 hash : object.source->textureHashes) {
                    const nfs::Texture* found = world_->FindDisplayTexture(hash);
                    LOG_INFO("sky texture %08X: %s", hash, found ? found->name.c_str() : "NOT FOUND");
                    const auto anim = world_->TextureAnimations().find(hash);
                    if (anim != world_->TextureAnimations().end())
                        for (u32 frame : anim->second.frames) {
                            const nfs::Texture* f = world_->FindDisplayTexture(frame);
                            LOG_INFO("  frame %08X: %s", frame, f ? f->name.c_str() : "NOT FOUND");
                        }
                }
            }
            Mat4 transform = Mat4::Identity();
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) transform.m[r][c] = object.transform.m[r][c] * scale;
            const Vec3 eye = camera.Position();
            transform.m[3][0] = eye.x;
            transform.m[3][1] = eye.y;
            transform.m[3][2] = eye.z;

            const IDirect3DVertexBuffer9* none = nullptr;
            (void)none;
            device->SetStreamSource(0, sections_[i].gpu->VertexBuffer(), 0, sizeof(nfs::SolidVertex));
            device->SetIndices(sections_[i].gpu->IndexBuffer());
            device->SetFVF(kWorldVertexFvf);
            const D3DMATRIX world = ToD3D(transform);
            device->SetTransform(D3DTS_WORLD, &world);
            device->SetRenderState(D3DRS_ZENABLE, FALSE);
            device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            const bool layered = DrawObject(device, object, stats);
            if (layered) {
                BeginTranslucentStates(device);
                device->SetRenderState(D3DRS_ZENABLE, FALSE);
                device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                DrawObject(device, object, stats, DrawPass::Translucent);
                EndTranslucentStates(device);
            }
            device->SetRenderState(D3DRS_ZENABLE, TRUE);
            device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
            device->SetRenderState(D3DRS_CULLMODE, backfaceCulling_ ? cullMode_ : D3DCULL_NONE);
            return;   // one dome
        }
    }
}

void WorldRenderer::SetRaceGroupsVisible(bool visible) {
    if (showRaceGroups_ == visible) return;
    showRaceGroups_ = visible;
    if (!world_) return;
    for (size_t i = 0; i < sections_.size() && i < world_->Loaded().size(); ++i) RefreshCollision(i);
    collisionSelection_.erase(
        std::remove_if(collisionSelection_.begin(), collisionSelection_.end(), [&](const CollisionPick& pick) {
            const auto& packs = world_->Loaded()[pick.section]->collision;
            return !visible && pick.pack < packs.size() && pick.polygon < packs[pick.pack].polygons.size() &&
                   packs[pick.pack].polygons[pick.polygon].raceGroup != 0;
        }),
        collisionSelection_.end());
}

void WorldRenderer::RefreshCollision(size_t loadedSection) {
    if (!world_ || !context_ || loadedSection >= sections_.size() ||
        loadedSection >= world_->Loaded().size())
        return;
    auto collision = std::make_unique<CollisionMesh>();
    if (collision->Upload(context_->Device(), *world_->Loaded()[loadedSection], showRaceGroups_))
        sections_[loadedSection].collision = std::move(collision);
    else
        sections_[loadedSection].collision.reset();
}

void WorldRenderer::SetCollisionSelectionList(std::vector<CollisionPick> picks) {
    selection_.clear();
    selectedOwner_ = nullptr; selectedExtra_ = -1;
    collisionSelection_ = std::move(picks);
}

void WorldRenderer::DrawToolOverlay(IDirect3DDevice9* device) {
    bool any = false;
    for (const auto& [owner, layer] : toolLayers_) any = any || !layer.triangles.empty() || !layer.lines.empty();
    if (!any) return;
    BeginOverlay(device);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    for (const auto& [owner, layer] : toolLayers_) {
        device->SetRenderState(D3DRS_ZENABLE, TRUE);
        if (!layer.triangles.empty())
            device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(layer.triangles.size() / 3),
                                    layer.triangles.data(), sizeof(OverlayVertex));
        // The lines show through everything: a point being placed behind a
        // kerb still has to be seen.
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        if (!layer.lines.empty())
            device->DrawPrimitiveUP(D3DPT_LINELIST, (UINT)(layer.lines.size() / 2),
                                    layer.lines.data(), sizeof(OverlayVertex));
    }
    EndOverlay(device);
}

void WorldRenderer::UpdateModelColours(u32 modelHash, const std::vector<u32>& colours) {
    for (SectionRender& section : sections_) {
        if (!section.gpu) continue;
        for (const GpuObject& object : section.gpu->Objects()) {
            if (!object.source || object.source->hash != modelHash) continue;
            const size_t count = std::min(colours.size(), object.source->vertices.size());
            IDirect3DVertexBuffer9* buffer = section.gpu->VertexBuffer();
            void* data = nullptr;
            const UINT offset = (UINT)(object.baseVertex * sizeof(nfs::SolidVertex));
            const UINT bytes = (UINT)(count * sizeof(nfs::SolidVertex));
            if (!buffer || !count || FAILED(buffer->Lock(offset, bytes, &data, 0))) continue;
            auto* vertices = (nfs::SolidVertex*)data;
            for (size_t i = 0; i < count; ++i) vertices[i].color = colours[i];
            buffer->Unlock();
        }
    }
}

void WorldRenderer::UpdateModelPositions(u32 modelHash, const std::vector<Vec3>& positions) {
    for (SectionRender& section : sections_) {
        if (!section.gpu) continue;
        for (const GpuObject& object : section.gpu->Objects()) {
            if (!object.source || object.source->hash != modelHash) continue;
            const size_t count = std::min(positions.size(), object.source->vertices.size());
            IDirect3DVertexBuffer9* buffer = section.gpu->VertexBuffer();
            void* data = nullptr;
            const UINT offset = (UINT)(object.baseVertex * sizeof(nfs::SolidVertex));
            const UINT bytes = (UINT)(count * sizeof(nfs::SolidVertex));
            if (!buffer || !count || FAILED(buffer->Lock(offset, bytes, &data, 0))) continue;
            auto* vertices = (nfs::SolidVertex*)data;
            for (size_t i = 0; i < count; ++i) vertices[i].position = positions[i];
            buffer->Unlock();
        }
    }
}

bool WorldRenderer::PickCollision(const Ray& ray, CollisionPick& out) const {
    if (!world_ || !showCollision_) return false;

    CollisionPick best;
    bool found = false;

    for (size_t index : collisionDrawn_) {
        if (index >= world_->Loaded().size() || !sections_[index].collision) continue;
        if (IntersectRayAabb(ray, sections_[index].collision->Bounds()) < 0.0f) continue;

        const auto& packs = world_->Loaded()[index]->collision;
        for (size_t p = 0; p < packs.size(); ++p) {
            for (size_t i = 0; i < packs[p].polygons.size(); ++i) {
                if (!showRaceGroups_ && packs[p].polygons[i].raceGroup != 0) continue;
                CollisionPick candidate{ (u32)index, (u32)p, (u32)i };
                Vec3 corners[4];
                bool quad = false;
                if (!CollisionCorners(candidate, corners, quad)) continue;

                float hit = IntersectRayTriangle(ray, corners[0], corners[1], corners[2]);
                if (quad) {
                    const float second = IntersectRayTriangle(ray, corners[0], corners[2], corners[3]);
                    if (second >= 0.0f && (hit < 0.0f || second < hit)) hit = second;
                }
                if (hit < 0.0f || (found && hit >= best.distance)) continue;

                candidate.distance = hit;
                best  = candidate;
                found = true;
            }
        }
    }

    if (found) out = best;
    return found;
}

void WorldRenderer::SetCollisionSelection(const CollisionPick& pick) {
    selection_.clear();
    selectedOwner_ = nullptr; selectedExtra_ = -1;
    collisionSelection_ = { pick };
}

bool WorldRenderer::ToggleCollisionSelection(const CollisionPick& pick) {
    selection_.clear();
    selectedOwner_ = nullptr; selectedExtra_ = -1;
    const auto it = std::find_if(collisionSelection_.begin(), collisionSelection_.end(),
                                 [&](const CollisionPick& other) { return other.SamePolygon(pick); });
    if (it != collisionSelection_.end()) {
        collisionSelection_.erase(it);
        return false;
    }
    collisionSelection_.push_back(pick);
    return true;
}

size_t WorldRenderer::SelectCollisionPack(const CollisionPick& pick, bool add) {
    selection_.clear();
    selectedOwner_ = nullptr; selectedExtra_ = -1;
    if (!add) collisionSelection_.clear();
    if (!world_ || pick.section >= world_->Loaded().size()) return 0;
    const auto& packs = world_->Loaded()[pick.section]->collision;
    if (pick.pack >= packs.size()) return 0;

    size_t added = 0;
    for (size_t i = 0; i < packs[pick.pack].polygons.size(); ++i) {
        const CollisionPick one{ pick.section, pick.pack, (u32)i };
        const bool present = std::any_of(collisionSelection_.begin(), collisionSelection_.end(),
                                         [&](const CollisionPick& other) { return other.SamePolygon(one); });
        if (present) continue;
        collisionSelection_.push_back(one);
        ++added;
    }
    return added;
}

void WorldRenderer::DrawCollisionSelection(IDirect3DDevice9* device) {
    if (collisionSelection_.empty()) return;

    struct Vertex { float x, y, z; D3DCOLOR colour; };
    constexpr D3DCOLOR kFill    = 0xB000E8FF;
    constexpr D3DCOLOR kOutline = 0xFFFFFFFF;
    constexpr float    kLift    = 0.1f;   // above the ordinary collision layer

    std::vector<Vertex> fill, lines;
    fill.reserve(collisionSelection_.size() * 6);
    lines.reserve(collisionSelection_.size() * 8);

    for (const CollisionPick& pick : collisionSelection_) {
        Vec3 c[4];
        bool quad = false;
        if (!CollisionCorners(pick, c, quad)) continue;
        auto push = [](std::vector<Vertex>& out, const Vec3& p, D3DCOLOR colour) {
            out.push_back({ p.x, p.y, p.z + kLift, colour });
        };
        for (int k : { 0, 1, 2 }) push(fill, c[k], kFill);
        if (quad) for (int k : { 0, 2, 3 }) push(fill, c[k], kFill);
        const int corners = quad ? 4 : 3;
        for (int k = 0; k < corners; ++k) {
            push(lines, c[k], kOutline);
            push(lines, c[(k + 1) % corners], kOutline);
        }
    }

    device->SetFVF(CollisionMesh::kFvf);
    if (!fill.empty()) {
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(fill.size() / 3),
                                fill.data(), sizeof(Vertex));
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    }
    if (!lines.empty())
        device->DrawPrimitiveUP(D3DPT_LINELIST, (UINT)(lines.size() / 2),
                                lines.data(), sizeof(Vertex));
}

bool WorldRenderer::SelectPlacement(u32 loadedSection, int placement) {
    if (loadedSection >= sections_.size()) return false;
    for (const GpuInstance& instance : sections_[loadedSection].instances) {
        if (instance.placement != placement) continue;
        const GpuSection* owner = sections_[instance.section].gpu.get();
        if (!owner || instance.object >= owner->Objects().size()) return false;

        PickResult picked;
        picked.object        = owner->Objects()[instance.object].source;
        picked.transform     = instance.transform;
        picked.worldBounds   = instance.worldBounds;
        picked.fromInstance  = true;
        picked.gpuSection    = instance.section;
        picked.gpuObject     = instance.object;
        picked.loadedSection = loadedSection;
        picked.placement     = placement;
        if (world_ && loadedSection < world_->Loaded().size() && world_->Loaded()[loadedSection]->info)
            picked.sectionName = world_->Loaded()[loadedSection]->info->name;
        SetSelection(picked);
        return true;
    }
    return false;
}

void WorldRenderer::SelectPlacements(const std::vector<std::pair<u32, int>>& placements) {
    std::vector<PickResult> chosen;
    for (const auto& [section, placement] : placements)
        if (SelectPlacement(section, placement)) chosen.push_back(selection_.front());
    selection_ = std::move(chosen);
    collisionSelection_.clear();
    selectedOwner_ = nullptr; selectedExtra_ = -1;
}

// ---------------------------------------------------------------------------
// Extensions
// ---------------------------------------------------------------------------

bool WorldRenderer::PickExtension(const Ray& ray, const RenderExtension*& owner, int& index,
                                  float& distance) const {
    owner = nullptr;
    index = -1;
    for (const RenderExtension* extension : extensions_) {
        int hit = -1;
        float at = 0.0f;
        if (!extension->Pick(ray, hit, at) || hit < 0) continue;
        if (owner && at >= distance) continue;
        owner = extension;
        index = hit;
        distance = at;
    }
    return owner != nullptr;
}

void WorldRenderer::BeginModels(IDirect3DDevice9* device, const GpuSection& gpu) {
    device->SetFVF(kWorldVertexFvf);
    device->SetStreamSource(0, gpu.VertexBuffer(), 0, sizeof(nfs::SolidVertex));
    device->SetIndices(gpu.IndexBuffer());
    device->SetRenderState(D3DRS_CULLMODE, backfaceCulling_ ? cullMode_ : D3DCULL_NONE);
    if (mode_ != RenderMode::Textured && mode_ != RenderMode::Unlit) BindTextureStage(device, CachedTexture{});
}

void WorldRenderer::DrawModel(IDirect3DDevice9* device, const GpuObject& object, bool translucent) {
    RenderStats unused;
    DrawObject(device, object, unused, translucent ? DrawPass::Translucent : DrawPass::Solid);
}

void WorldRenderer::BeginTranslucentModels(IDirect3DDevice9* device) { BeginTranslucentStates(device); }
void WorldRenderer::EndTranslucentModels(IDirect3DDevice9* device) { EndTranslucentStates(device); }

void WorldRenderer::DrawObjectAnimations(IDirect3DDevice9* device, bool usingVisibility, const Vec3& eye,
                                         RenderStats& stats) {
    if (!world_) return;
    const nfs::WorldAnimations& animations = world_->ObjectAnimations();
    if (animations.Placements().empty()) return;
    const GpuSection* unusedGpu = nullptr;
    const GpuObject* unusedObject = nullptr;
    FindGpuModel(0, unusedGpu, unusedObject);   // fills objectByHash_
    const double seconds = animate_ ? animationClock_ : 0.0;
    std::vector<Mat4> pose;
    device->SetFVF(kWorldVertexFvf);
    size_t bound = (size_t)-1;
    for (const nfs::WorldAnimationPlacement& placement : animations.Placements()) {
        // The game shows a tree only while its section is streamed in.
        if (usingVisibility && !std::binary_search(visibleSections_.begin(), visibleSections_.end(),
                                                   (u32)placement.section))
            continue;
        animations.Pose(placement, seconds, pose);
        const nfs::WorldAnimationTree& tree = animations.Trees()[placement.tree];
        for (size_t i = 0; i < tree.parts.size() && i < pose.size(); ++i) {
            if (!tree.parts[i].model) continue;
            const auto found = objectByHash_.find(tree.parts[i].model);
            if (found == objectByHash_.end()) continue;
            const size_t s = found->second.first;
            const GpuSection& gpu = *sections_[s].gpu;
            const GpuObject& object = gpu.Objects()[found->second.second];
            if (bound != s) {
                device->SetStreamSource(0, gpu.VertexBuffer(), 0, sizeof(nfs::SolidVertex));
                device->SetIndices(gpu.IndexBuffer());
                bound = s;
            }
            const bool mirrored = Determinant3x3(pose[i]) < 0.0f;
            if (backfaceCulling_)
                device->SetRenderState(D3DRS_CULLMODE, mirrored ? OppositeCull(cullMode_) : cullMode_);
            const D3DMATRIX world = ToD3D(pose[i]);
            device->SetTransform(D3DTS_WORLD, &world);
            stats.instancesVisible++;
            if (DrawObject(device, object, stats))
                QueueTranslucent(object, s, pose[i], mirrored,
                                 object.source ? Transform(object.source->bounds, pose[i]) : Aabb{}, eye);
        }
    }
    if (backfaceCulling_) device->SetRenderState(D3DRS_CULLMODE, cullMode_);
    static bool told = false;
    if (!told) {
        told = true;
        size_t visible = 0, parts = 0, missing = 0;
        for (const nfs::WorldAnimationPlacement& placement : animations.Placements()) {
            if (usingVisibility && !std::binary_search(visibleSections_.begin(), visibleSections_.end(),
                                                       (u32)placement.section))
                continue;
            ++visible;
            for (const nfs::WorldAnimationPart& part : animations.Trees()[placement.tree].parts)
                if (part.model) (objectByHash_.count(part.model) ? parts : missing)++;
        }
        LOG_INFO("object animations: %zu of %zu placements in view, %zu parts drawn, %zu models not found",
                 visible, animations.Placements().size(), parts, missing);
    }
}

bool WorldRenderer::FindGpuModel(u32 hash, const GpuSection*& gpu, const GpuObject*& object) {
    if (objectByHash_.empty()) {
        for (size_t s = 0; s < sections_.size(); ++s) {
            if (!sections_[s].gpu) continue;
            const auto& objects = sections_[s].gpu->Objects();
            for (size_t o = 0; o < objects.size(); ++o)
                if (objects[o].source && !objects[o].draws.empty())
                    objectByHash_.emplace(objects[o].source->hash, std::make_pair(s, o));
        }
    }
    const auto found = objectByHash_.find(hash);
    if (found == objectByHash_.end()) return false;
    gpu = sections_[found->second.first].gpu.get();
    object = &gpu->Objects()[found->second.second];
    return true;
}

void WorldRenderer::BeginUserMesh(IDirect3DDevice9* device) {
    const D3DMATRIX identity = ToD3D(Mat4::Identity());
    device->SetTransform(D3DTS_WORLD, &identity);
    device->SetFVF(kWorldVertexFvf);
    device->SetRenderState(D3DRS_CULLMODE, backfaceCulling_ ? cullMode_ : D3DCULL_NONE);
    device->SetRenderState(D3DRS_FILLMODE, mode_ == RenderMode::Wireframe ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
}

void WorldRenderer::BindUserTexture(IDirect3DDevice9* device, u32 textureHash) {
    const bool wantsTexture = mode_ == RenderMode::Textured || mode_ == RenderMode::Unlit;
    BindTextureStage(device, wantsTexture ? textures_.Get(device, textureHash) : CachedTexture{});
}

void WorldRenderer::BeginOverlay(IDirect3DDevice9* device) {
    device->SetTexture(0, nullptr);
    device->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    const D3DMATRIX identity = ToD3D(Mat4::Identity());
    device->SetTransform(D3DTS_WORLD, &identity);
    device->SetFVF(CollisionMesh::kFvf);
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
}

void WorldRenderer::EndOverlay(IDirect3DDevice9* device) {
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    device->SetRenderState(D3DRS_CULLMODE, backfaceCulling_ ? cullMode_ : D3DCULL_NONE);
    device->SetRenderState(D3DRS_FILLMODE,
                           mode_ == RenderMode::Wireframe ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
    device->SetFVF(kWorldVertexFvf);
}

// ---------------------------------------------------------------------------
// Thumbnails
// ---------------------------------------------------------------------------

namespace {

// A neutral mid grey that neither dark roofs nor pale concrete disappear into.
constexpr D3DCOLOR kThumbnailBackground = 0xFF3C4148;

void ReleaseCom(IUnknown*& object) {
    if (object) object->Release();
    object = nullptr;
}

} // namespace

void WorldRenderer::ReleaseThumbnailTargets() {
    IUnknown* target = thumbTarget_;
    IUnknown* depth = thumbDepth_;
    IUnknown* readback = thumbReadback_;
    ReleaseCom(target);
    ReleaseCom(depth);
    ReleaseCom(readback);
    thumbTarget_ = nullptr;
    thumbDepth_ = nullptr;
    thumbReadback_ = nullptr;
    thumbSize_ = 0;
}

bool WorldRenderer::RenderThumbnail(u32 modelHash, u32 size, std::vector<u32>& bgra) {
    if (!context_ || !context_->IsValid() || size == 0 || size > 1024) return false;
    IDirect3DDevice9* device = context_->Device();
    if (device->TestCooperativeLevel() != D3D_OK) return false;

    if (objectByHash_.empty()) {
        for (size_t s = 0; s < sections_.size(); ++s) {
            if (!sections_[s].gpu) continue;
            const auto& objects = sections_[s].gpu->Objects();
            for (size_t o = 0; o < objects.size(); ++o)
                if (objects[o].source && !objects[o].draws.empty())
                    objectByHash_.emplace(objects[o].source->hash, std::make_pair(s, o));
        }
    }
    const GpuSection* gpuFound = nullptr;
    const GpuObject* objectFound = nullptr;
    const auto found = objectByHash_.find(modelHash);
    if (found != objectByHash_.end()) {
        gpuFound = sections_[found->second.first].gpu.get();
        objectFound = &gpuFound->Objects()[found->second.second];
    } else {
        // A model of an extension's own, which the region does not hold.
        for (RenderExtension* extension : extensions_)
            if (extension->FindModel(modelHash, gpuFound, objectFound)) break;
    }
    if (!gpuFound || !objectFound || !objectFound->source) return false;
    const GpuSection& gpu = *gpuFound;
    const GpuObject& object = *objectFound;
    const nfs::SolidObject& model = *object.source;

    Aabb box = model.bounds;
    if (!box.Valid())
        for (const nfs::SolidVertex& vertex : model.vertices) box.Add(vertex.position);
    if (!box.Valid()) return false;

    // Drawn at twice the size and averaged down: the cheapest antialiasing
    // there is, and thin things -- poles, rails, wires -- survive it.
    const u32 render = size * 2;
    if (thumbSize_ != render) {
        ReleaseThumbnailTargets();
        if (FAILED(device->CreateTexture(render, render, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                         D3DPOOL_DEFAULT, &thumbTarget_, nullptr)) ||
            (FAILED(device->CreateDepthStencilSurface(render, render, D3DFMT_D24S8,
                                                      D3DMULTISAMPLE_NONE, 0, TRUE, &thumbDepth_,
                                                      nullptr)) &&
             FAILED(device->CreateDepthStencilSurface(render, render, D3DFMT_D16,
                                                      D3DMULTISAMPLE_NONE, 0, TRUE, &thumbDepth_,
                                                      nullptr))) ||
            FAILED(device->CreateOffscreenPlainSurface(render, render, D3DFMT_A8R8G8B8,
                                                       D3DPOOL_SYSTEMMEM, &thumbReadback_,
                                                       nullptr))) {
            ReleaseThumbnailTargets();
            return false;
        }
        thumbSize_ = render;
    }

    IDirect3DSurface9* oldTarget = nullptr;
    IDirect3DSurface9* oldDepth = nullptr;
    D3DVIEWPORT9 oldViewport{};
    device->GetRenderTarget(0, &oldTarget);
    device->GetDepthStencilSurface(&oldDepth);
    device->GetViewport(&oldViewport);

    IDirect3DSurface9* target = nullptr;
    thumbTarget_->GetSurfaceLevel(0, &target);
    device->SetRenderTarget(0, target);
    device->SetDepthStencilSurface(thumbDepth_);
    const D3DVIEWPORT9 viewport{ 0, 0, render, render, 0.0f, 1.0f };
    device->SetViewport(&viewport);
    device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, kThumbnailBackground, 1.0f, 0);

    // From in front, to the left and above, the way a catalogue photographs
    // a thing: three faces showing. Two shapes need a view of their own,
    // since from there they are a line: something lying flat -- a start line
    // painted on the road, a planter bed -- is seen from nearly overhead, and
    // a panel standing up -- a wall, a sign, a fence -- from in front of its
    // face.
    const Vec3 extent = box.max - box.min;
    const Vec3 centre = (box.min + box.max) * 0.5f;
    const float across = std::max(extent.x, extent.y);
    Vec3 toEye{ -0.62f, -0.78f, 0.52f };
    if (extent.z < across * 0.12f)
        toEye = { -0.22f, -0.38f, 1.0f };
    else if (std::min(extent.x, extent.y) < across * 0.15f)
        toEye = extent.x < extent.y ? Vec3{ -1.0f, -0.32f, 0.3f } : Vec3{ -0.32f, -1.0f, 0.3f };
    toEye = Normalize(toEye);

    const float radius = std::max(0.25f, Length(extent) * 0.5f);
    const Mat4 view = LookAtRH(centre + toEye * (radius * 4.0f), centre, Vec3{ 0.0f, 0.0f, 1.0f });

    // Orthographic, fitted to the box as it lands on screen rather than to
    // the sphere around it: a guard rail forty units long and one high fills
    // its square instead of being a thread across the middle of it.
    float left = 1e30f, right = -1e30f, bottom = 1e30f, top = -1e30f, nearZ = 1e30f, farZ = -1e30f;
    for (int corner = 0; corner < 8; ++corner) {
        const Vec3 p{ corner & 1 ? box.max.x : box.min.x, corner & 2 ? box.max.y : box.min.y,
                      corner & 4 ? box.max.z : box.min.z };
        const Vec3 v = view.TransformPoint(p);
        left = std::min(left, v.x);     right = std::max(right, v.x);
        bottom = std::min(bottom, v.y); top = std::max(top, v.y);
        nearZ = std::min(nearZ, -v.z);  farZ = std::max(farZ, -v.z);
    }
    const float half = std::max(0.05f, std::max(right - left, top - bottom) * 0.5f * 1.06f);
    const float midX = (left + right) * 0.5f, midY = (bottom + top) * 0.5f;
    left = midX - half; right = midX + half; bottom = midY - half; top = midY + half;
    nearZ = std::max(0.01f, nearZ - radius * 0.1f);
    farZ += radius * 0.1f;

    Mat4 projection;
    projection.m[0][0] = 2.0f / (right - left);
    projection.m[1][1] = 2.0f / (top - bottom);
    projection.m[2][2] = 1.0f / (nearZ - farZ);
    projection.m[3][0] = (left + right) / (left - right);
    projection.m[3][1] = (top + bottom) / (bottom - top);
    projection.m[3][2] = nearZ / (nearZ - farZ);
    projection.m[3][3] = 1.0f;

    const D3DMATRIX d3dView = ToD3D(view);
    const D3DMATRIX d3dProjection = ToD3D(projection);
    const D3DMATRIX d3dWorld = ToD3D(Mat4::Identity());

    // Unlit: the colours hold the game's night lighting, which leaves half the
    // city too dark to recognise at this size. Both sides drawn, since a shell
    // seen from the wrong side would otherwise come out empty.
    const RenderMode mode = mode_;
    mode_ = RenderMode::Unlit;
    ApplyRenderStates();
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    bool drawn = false;
    if (SUCCEEDED(device->BeginScene())) {
        device->SetTransform(D3DTS_VIEW, &d3dView);
        device->SetTransform(D3DTS_PROJECTION, &d3dProjection);
        device->SetTransform(D3DTS_WORLD, &d3dWorld);
        device->SetStreamSource(0, gpu.VertexBuffer(), 0, sizeof(nfs::SolidVertex));
        device->SetIndices(gpu.IndexBuffer());
        RenderStats stats;
        if (DrawObject(device, object, stats)) {
            BeginTranslucentStates(device);
            DrawObject(device, object, stats, DrawPass::Translucent);
            EndTranslucentStates(device);
        }
        device->EndScene();
        drawn = true;
    }
    mode_ = mode;

    bool read = false;
    if (drawn && SUCCEEDED(device->GetRenderTargetData(target, thumbReadback_))) {
        D3DLOCKED_RECT locked{};
        if (SUCCEEDED(thumbReadback_->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
            bgra.assign((size_t)size * size, 0);
            for (u32 y = 0; y < size; ++y) {
                const u32* a = (const u32*)((const u8*)locked.pBits + (size_t)(2 * y) * locked.Pitch);
                const u32* b = (const u32*)((const u8*)locked.pBits + (size_t)(2 * y + 1) * locked.Pitch);
                for (u32 x = 0; x < size; ++x) {
                    const u32 p[4] = { a[2 * x], a[2 * x + 1], b[2 * x], b[2 * x + 1] };
                    u32 out = 0xFF000000u;
                    for (u32 shift = 0; shift < 24; shift += 8) {
                        u32 sum = 0;
                        for (u32 k = 0; k < 4; ++k) sum += (p[k] >> shift) & 0xFF;
                        out |= ((sum + 2) / 4) << shift;
                    }
                    bgra[(size_t)y * size + x] = out;
                }
            }
            thumbReadback_->UnlockRect();
            read = true;
        }
    }

    device->SetRenderTarget(0, oldTarget);
    device->SetDepthStencilSurface(oldDepth);
    device->SetViewport(&oldViewport);
    if (target) target->Release();
    if (oldTarget) oldTarget->Release();
    if (oldDepth) oldDepth->Release();
    return read;
}

void WorldRenderer::DrawGizmo(IDirect3DDevice9* device, const Camera& camera) {
    if (!gizmoVisible_ || !context_) return;
    GizmoView view;
    const float aspect = context_->Height() ? (float)context_->Width() / (float)context_->Height() : 1.0f;
    view.viewProjection = camera.ViewMatrix() * camera.ProjectionMatrix(aspect);
    view.width = (float)context_->Width();
    view.height = (float)context_->Height();
    view.eye = camera.Position();

    std::vector<GizmoVertex> lines, triangles;
    BuildGizmo(gizmoMode_, gizmoOrigin_, view, gizmoHot_, lines, triangles);

    device->SetTexture(0, nullptr);
    device->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    const D3DMATRIX identity = ToD3D(Mat4::Identity());
    device->SetTransform(D3DTS_WORLD, &identity);
    device->SetFVF(CollisionMesh::kFvf);
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    // Over the world, never buried in the building it belongs to.
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    if (!triangles.empty())
        device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(triangles.size() / 3), triangles.data(),
                                sizeof(GizmoVertex));
    if (!lines.empty())
        device->DrawPrimitiveUP(D3DPT_LINELIST, (UINT)(lines.size() / 2), lines.data(), sizeof(GizmoVertex));
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    device->SetRenderState(D3DRS_CULLMODE, backfaceCulling_ ? cullMode_ : D3DCULL_NONE);
}

// ---------------------------------------------------------------------------
// MINIMAP PREVIEW (temporary)
// ---------------------------------------------------------------------------

std::vector<minimap::PlacedModel> WorldRenderer::PlacedBuildings() const {
    std::vector<minimap::PlacedModel> out;
    for (const SectionRender& section : sections_) {
        if (section.alternate && !showAlternate_) continue;
        for (const GpuInstance& instance : section.instances) {
            const GpuSection* gpu = sections_[instance.section].gpu.get();
            if (!gpu || instance.object >= gpu->Objects().size()) continue;
            const nfs::SolidObject* model = gpu->Objects()[instance.object].source;
            if (model && minimap::IsMinimapBuilding(*model))
                out.push_back({ model, instance.transform });
        }
    }
    return out;
}

bool WorldRenderer::SetMinimapBuildings(const std::vector<minimap::Building>& buildings) {
    if (!context_ || !context_->IsValid()) return false;
    return minimapPreview_.Upload(context_->Device(), buildings);
}
