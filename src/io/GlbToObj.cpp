#pragma warning(push, 0)
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#pragma warning(pop)

#include "io/GlbToObj.h"

#include "core/Log.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

namespace {

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)size, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Folder(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring Stem(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}

struct Vertex { float p[3], n[3], uv[2]; };

struct Part {
    std::string material;           // name in the MTL
    std::vector<Vertex> vertices;
    std::vector<unsigned> indices;
};

void Transform(const float m[16], const float in[3], float out[3], bool direction) {
    // cgltf matrices are column-major.
    const float w = direction ? 0.0f : 1.0f;
    for (int r = 0; r < 3; ++r) out[r] = m[r] * in[0] + m[4 + r] * in[1] + m[8 + r] * in[2] + m[12 + r] * w;
}

} // namespace

bool ConvertGlbToObj(const std::wstring& glbPath, const std::wstring& objPath, const std::string& objectName,
                     float fallbackHeight, GlbConversion& out) {
    out = {};
    cgltf_options options{};
    cgltf_data* data = nullptr;
    const std::string path = Narrow(glbPath);
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        out.error = L"That GLB could not be read.";
        return false;
    }
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        out.error = L"The GLB's data could not be loaded.";
        return false;
    }

    const std::wstring folder = Folder(objPath);
    const std::wstring stem = Stem(objPath);

    // --- textures: the base colour of each material, written out once ---------
    std::map<const cgltf_material*, std::string> materialName;
    std::map<const cgltf_image*, std::string> imageFile;
    std::vector<std::pair<std::string, std::string>> mtl;   // name, texture file (may be empty)
    const auto materialFor = [&](const cgltf_material* material) -> std::string {
        if (!material) material = nullptr;
        const auto found = materialName.find(material);
        if (found != materialName.end()) return found->second;
        const std::string name = "mat" + std::to_string(materialName.size());
        std::string file;
        const cgltf_texture* texture = material && material->has_pbr_metallic_roughness
                                           ? material->pbr_metallic_roughness.base_color_texture.texture : nullptr;
        const cgltf_image* image = texture ? texture->image : nullptr;
        if (image) {
            const auto known = imageFile.find(image);
            if (known != imageFile.end()) {
                file = known->second;
            } else if (image->buffer_view && image->buffer_view->buffer && image->buffer_view->buffer->data) {
                const bool png = image->mime_type && std::string(image->mime_type).find("png") != std::string::npos;
                file = Narrow(stem) + "_tex" + std::to_string(imageFile.size()) + (png ? ".png" : ".jpg");
                FILE* f = nullptr;
                if (::_wfopen_s(&f, (folder + L"\\" + std::wstring(file.begin(), file.end())).c_str(), L"wb") == 0 && f) {
                    const uint8_t* bytes = (const uint8_t*)image->buffer_view->buffer->data + image->buffer_view->offset;
                    std::fwrite(bytes, 1, image->buffer_view->size, f);
                    std::fclose(f);
                    imageFile[image] = file;
                    ++out.textures;
                } else {
                    file.clear();
                }
            }
        }
        materialName[material] = name;
        mtl.push_back({ name, file });
        return name;
    };

    // --- every primitive of every node, in world space ---------------------------
    std::map<std::string, Part> parts;
    for (cgltf_size n = 0; n < data->nodes_count; ++n) {
        const cgltf_node& node = data->nodes[n];
        if (!node.mesh) continue;
        float world[16];
        cgltf_node_transform_world(&node, world);
        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
            const cgltf_primitive& primitive = node.mesh->primitives[p];
            if (primitive.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor *position = nullptr, *normal = nullptr, *uv = nullptr;
            for (cgltf_size a = 0; a < primitive.attributes_count; ++a) {
                const cgltf_attribute& attribute = primitive.attributes[a];
                if (attribute.type == cgltf_attribute_type_position) position = attribute.data;
                else if (attribute.type == cgltf_attribute_type_normal) normal = attribute.data;
                else if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0) uv = attribute.data;
            }
            if (!position) continue;
            Part& part = parts[materialFor(primitive.material)];
            part.material = materialFor(primitive.material);
            const unsigned base = (unsigned)part.vertices.size();
            for (cgltf_size v = 0; v < position->count; ++v) {
                Vertex vertex{};
                float local[3] = {}, ln[3] = { 0, 1, 0 };
                cgltf_accessor_read_float(position, v, local, 3);
                Transform(world, local, vertex.p, false);
                if (normal) cgltf_accessor_read_float(normal, v, ln, 3);
                Transform(world, ln, vertex.n, true);
                if (uv) cgltf_accessor_read_float(uv, v, vertex.uv, 2);
                part.vertices.push_back(vertex);
            }
            // A mirrored node turns the winding inside out.
            const float det = world[0] * (world[5] * world[10] - world[9] * world[6]) -
                              world[4] * (world[1] * world[10] - world[9] * world[2]) +
                              world[8] * (world[1] * world[6] - world[5] * world[2]);
            const auto index = [&](cgltf_size k) {
                return primitive.indices ? (unsigned)cgltf_accessor_read_index(primitive.indices, k) : (unsigned)k;
            };
            const cgltf_size count = primitive.indices ? primitive.indices->count : position->count;
            for (cgltf_size k = 0; k + 2 < count; k += 3) {
                const unsigned a = index(k), b = index(k + 1), c = index(k + 2);
                if (det < 0) { part.indices.insert(part.indices.end(), { base + a, base + c, base + b }); }
                else { part.indices.insert(part.indices.end(), { base + a, base + b, base + c }); }
            }
        }
    }
    cgltf_free(data);

    // --- size and stance ------------------------------------------------------------
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (const auto& [name, part] : parts)
        for (const Vertex& v : part.vertices)
            for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], v.p[k]); hi[k] = std::max(hi[k], v.p[k]); }
    if (lo[0] > hi[0]) {
        out.error = L"The GLB holds no triangles.";
        return false;
    }
    const float height = hi[1] - lo[1];
    float scale = 1.0f;
    if (height > 120.0f || height < 0.05f) scale = fallbackHeight / std::max(height, 1e-4f);
    out.scale = scale;
    out.height = height * scale;
    const float cx = (lo[0] + hi[0]) * 0.5f, cz = (lo[2] + hi[2]) * 0.5f, by = lo[1];

    // --- write -------------------------------------------------------------------------
    const std::wstring mtlPath = folder + L"\\" + stem + L".mtl";
    FILE* m = nullptr;
    if (::_wfopen_s(&m, mtlPath.c_str(), L"wb") != 0 || !m) { out.error = L"Cannot write the MTL."; return false; }
    for (const auto& [name, file] : mtl) {
        std::fprintf(m, "newmtl %s\nKd 1 1 1\n", name.c_str());
        if (!file.empty()) std::fprintf(m, "map_Kd %s\n", file.c_str());
        std::fprintf(m, "\n");
    }
    std::fclose(m);

    FILE* f = nullptr;
    if (::_wfopen_s(&f, objPath.c_str(), L"wb") != 0 || !f) { out.error = L"Cannot write the OBJ."; return false; }
    std::fprintf(f, "# converted from a GLB by FRS World Editor\nmtllib %s.mtl\no %s\n", Narrow(stem).c_str(),
                 objectName.c_str());
    size_t written = 0;
    for (const auto& [name, part] : parts) {
        for (const Vertex& v : part.vertices)
            std::fprintf(f, "v %.5f %.5f %.5f\n", (v.p[0] - cx) * scale, (v.p[1] - by) * scale, (v.p[2] - cz) * scale);
        for (const Vertex& v : part.vertices) std::fprintf(f, "vt %.5f %.5f\n", v.uv[0], 1.0f - v.uv[1]);
        for (const Vertex& v : part.vertices) {
            const float length = std::sqrt(v.n[0] * v.n[0] + v.n[1] * v.n[1] + v.n[2] * v.n[2]);
            const float s = length > 1e-6f ? 1.0f / length : 0.0f;
            std::fprintf(f, "vn %.4f %.4f %.4f\n", v.n[0] * s, v.n[1] * s, v.n[2] * s);
        }
        std::fprintf(f, "usemtl %s\n", part.material.c_str());
        for (size_t t = 0; t + 2 < part.indices.size(); t += 3) {
            const size_t a = written + part.indices[t] + 1, b = written + part.indices[t + 1] + 1,
                         c = written + part.indices[t + 2] + 1;
            std::fprintf(f, "f %zu/%zu/%zu %zu/%zu/%zu %zu/%zu/%zu\n", a, a, a, b, b, b, c, c, c);
        }
        written += part.vertices.size();
        out.triangles += part.indices.size() / 3;
    }
    std::fclose(f);
    out.vertices = written;
    out.materials = parts.size();
    LOG_INFO("GLB -> OBJ: %zu vertices, %zu triangles, %zu materials, %zu textures, scale %.3f, %.2f tall",
             out.vertices, out.triangles, out.materials, out.textures, out.scale, out.height);
    return true;
}
