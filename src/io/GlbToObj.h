#pragma once

#include <string>

// A glTF binary (.glb) -- what Sketchfab hands out -- as a Wavefront OBJ the
// editor's own import reads (io/SelectionImport.h, a new object): every mesh
// of the scene with its node transforms applied, one material per base-colour
// texture (written beside it as PNG or JPEG), Y up as glTF and Blender both
// have it, and the model standing on its origin: centred in plan, its lowest
// point at zero.
//
// Sizes: glTF is in metres, as the game is near enough. A model made at
// another scale -- a lamp post a kilometre tall, a house of a centimetre --
// is brought to a believable size: anything taller than 120 units or smaller
// than 0.05 is scaled to `fallbackHeight`.
struct GlbConversion {
    size_t vertices = 0, triangles = 0, materials = 0, textures = 0;
    float  scale = 1.0f;          // what was applied
    float  height = 0.0f;         // of the result
    std::wstring error;
};

// `objectName` names the OBJ object -- the new model is FRS_<objectName>.
bool ConvertGlbToObj(const std::wstring& glbPath, const std::wstring& objPath, const std::string& objectName,
                     float fallbackHeight, GlbConversion& out);
