#include "drive/fx/CarFx.h"

#include "drive/CarModel.h"
#include "drive/cars/CarFxSetupU2.h"
#include "drive/cars/CarRenderLayoutU2.h"
#include "drive/cars/CarTypeInfoU2.h"
#include "drive/cars/RideInfoU2.h"
#include "drive/io/BinHash.h"
#include "drive/textures/TexturePack.h"

#include <cstdio>

namespace bb {

namespace {
constexpr uint32_t kShadowRamp = 0xBADB4475;   // SHADOWRAMP (GLOBALB), CRI+0x340
}

bool CarFx::Init(IDirect3DDevice9* device, const CarModel& model) {
    Release();
    const CarRenderLayoutU2* layout = model.Layout();
    const RideInfoU2* ride = model.Ride();
    const CarTypeInfoU2* info = model.CarType();
    if (!layout || !ride || !info) return false;
    device_ = device;
    flares_ = CarFxSetupU2::CreateFlares(*layout);
    headlightColour_ = CarFxSetupU2::HeadlightColour(*ride);
    customSpoiler_ = CarFxSetupU2::HasCustomSpoiler(*ride);
    carType_ = info->Index;
    traffic_ = info->UsageType == 2;

    CarFxSetupU2::PartsBoundingBox(*layout, shadow_.BBoxMin, shadow_.BBoxMax);
    for (int w = 0; w < 4; w++) {
        shadow_.WheelPos[w][0] = info->Wheels[w].X;
        shadow_.WheelPos[w][1] = info->Wheels[w].PhysicsY;
        shadow_.WheelPos[w][2] = info->Wheels[w].RideHeight;
    }
    shadow_.HullPoints = CarFxSetupU2::HullPoints(*ride, model.SolidPacks());
    // CRI+0x33C: "%s_SHADOWIG" of the base model name (the front end uses _SHADOWFE).
    shadowTextureName_ = info->BaseModelName + "_SHADOWIG";
    if (device) {
        if (const Texture* t = model.FindTexture(BinHash::Hash(shadowTextureName_))) {
            shadowTexture_ = fx::DescribeTexture(*t);
            shadowTexture_.texture = fx::CreateGameTexture(device, *t);
        }
    }
    ready_ = true;
    return true;
}

void CarFx::Release() {
    if (shadowTexture_.texture) shadowTexture_.texture->Release();
    shadowTexture_ = fx::FxTexture{};
    flares_.clear();
    shadow_.HullPoints.clear();
    ready_ = false;
    device_ = nullptr;
}

std::string CarFx::Describe() const {
    char line[256];
    std::snprintf(line, sizeof line,
                  "car fx: %zu flares, headlight colour %06X, spoiler %d, type %d, box (%.3f %.3f %.3f)..(%.3f %.3f %.3f), "
                  "hull %zu points, shadow texture %s%s",
                  flares_.size(), headlightColour_, customSpoiler_ ? 1 : 0, carType_, shadow_.BBoxMin[0], shadow_.BBoxMin[1],
                  shadow_.BBoxMin[2], shadow_.BBoxMax[0], shadow_.BBoxMax[1], shadow_.BBoxMax[2], shadow_.HullPoints.size(),
                  shadowTextureName_.c_str(), shadowTexture_.texture ? "" : " (missing)");
    return line;
}

void CarFx::DrawShadow(fx::StateGuard& guard, fx::PolyRenderer& polys, const fx::DeviceCamera& cam, const Mat4& body,
                       const ShadowElevation& ground, const BWareMath* math, fx::GlobalTextures& globals) {
    if (!ready_) return;
    float E[16];
    fx::ToFloats(body, E);
    shadow_.Build(E, ground, math);
    if (shadow_.Strips.empty()) return;
    // sub_48AE30: every vertex pulled toward the camera, s = (d - 0.104) / d with d the camera-car distance.
    const float camPos[3] = { cam.eye.x, cam.eye.y, cam.eye.z };
    const float carPos[3] = { E[12], E[13], E[14] };
    const float s = CarShadowU2::DepthBiasScale(camPos, carPos, false);
    const fx::FxTexture& ramp = globals.Get(device_, kShadowRamp);
    std::vector<fx::PolyRenderer::Vertex> v;
    for (const ShadowStripU2& strip : shadow_.Strips) {
        const fx::FxTexture& tex = strip.Texture == 0 ? shadowTexture_ : ramp;
        v.clear();
        for (const ShadowVertexU2& p : strip.Vertices) {
            fx::PolyRenderer::Vertex q;
            q.x = camPos[0] + s * (p.X - camPos[0]);
            q.y = camPos[1] + s * (p.Y - camPos[1]);
            q.z = camPos[2] + s * (p.Z - camPos[2]);
            q.colour = p.Colour;
            q.u = p.U;
            q.v = p.V;
            v.push_back(q);
        }
        // sub_5CF420 0x5CF863: triangle strips
        polys.Draw(guard, tex, D3DPT_TRIANGLESTRIP, v.data(), (int)v.size());
    }
}

void CarFx::DrawFlares(fx::StateGuard& guard, fx::PolyRenderer& polys, const FlareViewU2& view, const Mat4& body,
                       fx::GlobalTextures& globals, int* quadCount) {
    if (quadCount) *quadCount = 0;
    if (!ready_ || flares_.empty()) return;
    float E[16];
    fx::ToFloats(body, E);
    const float carPos[3] = { E[12], E[13], E[14] };
    if (!CarLightFlaresU2::CarBigEnough(view, carPos, false)) return;
    // TheGameFlowManager == 6: the test drive is the game's free roam.
    const FlareTableU2 table = CarLightFlaresU2::TableForView(view.ViewId, true);
    CarLightFlaresU2::LightState st;
    st.HasCar = true;
    st.Traffic = traffic_;
    st.HeadlightsOn = headlightsOn_;
    st.Braking = braking_;
    st.CustomSpoiler = customSpoiler_;
    st.CarType = carType_;
    st.FeSelectedSlot = 0x45;
    st.ForceFlags = forceFlags_;
    FlarePolyU2 out[4];
    int quads = 0;
    for (const LightFlareU2& f : flares_) {
        uint32_t colour = 0;
        const float I = CarLightFlaresU2::Intensity(f, st, headlightColour_, 0.0f, colour);
        if (!(I > 0.0f)) continue;
        const int n = CarLightFlaresU2::Render(view, f, E, I, colour, table, out, 4);
        for (int k = 0; k < n && k < 4; ++k) {
            const FlarePolyU2& q = out[k];
            const int tex = q.TexIndex < 0 || q.TexIndex > 2 ? 2 : q.TexIndex;
            const fx::FxTexture& t = globals.Get(device_, CarLightFlaresU2::TextureHashes[tex]);
            const D3DCOLOR c = fx::PolyColour(q.Colour);
            // ePoly default uvs (ctor 0x5BB1C0); TRIANGLEFAN v0 v1 v2 v3 (sub_5C5580)
            const fx::PolyRenderer::Vertex v[4] = {
                { q.X0, q.Y0, q.Z0, c, 0.0f, 0.0f },
                { q.X1, q.Y1, q.Z1, c, 1.0f, 0.0f },
                { q.X2, q.Y2, q.Z2, c, 1.0f, 1.0f },
                { q.X3, q.Y3, q.Z3, c, 0.0f, 1.0f },
            };
            polys.Draw(guard, t, D3DPT_TRIANGLEFAN, v, 4);
            ++quads;
        }
    }
    if (quadCount) *quadCount = quads;
}

} // namespace bb
