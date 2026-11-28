// Mesh primitives (plan F3 Task 2): attribute streams decoded to the ModelData formats, indices checked against the
// vertex count, LINE_LOOP / TRIANGLE_FAN converted to lists, and the Review Focus 4 fallbacks:
//   no NORMAL (triangles)              -> flat normals (glTF 2.0 §3.7.2.1);
//   normal map without TANGENT         -> MikkTSpace (Tangents.cpp), info;
//   texture whose TEXCOORD_n is absent -> warning, a zero UV set (sampled at (0, 0));
//   no indices                         -> 0..n-1;
//   no material                        -> -1 (the renderer's default material).
#include "Geometry.h"
#include "Internal.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

namespace eng::asset {

namespace {

template <class T>
void gather(std::vector<T>& v, const std::vector<uint32_t>& idx) {
    if (v.empty()) {
        return;
    }
    std::vector<T> out(idx.size());
    for (size_t k = 0; k < idx.size(); ++k) {
        out[k] = v[idx[k]];
    }
    v = std::move(out);
}

} // namespace

void stripToList(Primitive& p) {
    const std::vector<uint32_t>& in = p.indices;
    std::vector<uint32_t> out;
    if (p.topology == Topology::TriangleStrip) {
        for (size_t i = 0; i + 2 < in.size(); ++i) {
            // Odd triangles swap their first two vertices so every triangle keeps the strip's winding (§3.7.2.1).
            if (i % 2 == 0) {
                out.insert(out.end(), {in[i], in[i + 1], in[i + 2]});
            } else {
                out.insert(out.end(), {in[i + 1], in[i], in[i + 2]});
            }
        }
        p.topology = Topology::TriangleList;
        p.indices = std::move(out);
    } else if (p.topology == Topology::LineStrip) {
        for (size_t i = 0; i + 1 < in.size(); ++i) {
            out.insert(out.end(), {in[i], in[i + 1]});
        }
        p.topology = Topology::LineList;
        p.indices = std::move(out);
    }
}

void deindex(Primitive& p) {
    stripToList(p);
    if (p.topology == Topology::TriangleList) {
        p.indices.resize(p.indices.size() - p.indices.size() % 3u);
    }
    const std::vector<uint32_t> idx = p.indices;
    gather(p.position, idx);
    gather(p.normal, idx);
    gather(p.tangent, idx);
    for (auto& t : p.texCoord) {
        gather(t, idx);
    }
    gather(p.color0, idx);
    for (auto& j : p.joints) {
        gather(j, idx);
    }
    for (auto& w : p.weights) {
        gather(w, idx);
    }
    for (MorphTarget& m : p.targets) {
        gather(m.position, idx);
        gather(m.normal, idx);
        gather(m.tangent, idx);
    }
    p.vertexCount = static_cast<uint32_t>(idx.size());
    for (uint32_t i = 0; i < p.vertexCount; ++i) {
        p.indices[i] = i;
    }
}

void generateFlatNormals(Primitive& p) {
    if (p.topology != Topology::TriangleList && p.topology != Topology::TriangleStrip) {
        return;
    }
    deindex(p);
    p.normal.assign(p.vertexCount, Vec3{0.0f, 0.0f, 1.0f});
    for (uint32_t f = 0; f + 2 < p.vertexCount; f += 3) {
        const Vec3& a = p.position[f];
        const Vec3& b = p.position[f + 1];
        const Vec3& c = p.position[f + 2];
        const Vec3 e1{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const Vec3 e2{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        Vec3 n{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 0.0f && std::isfinite(len)) {
            n = {n[0] / len, n[1] / len, n[2] / len};
        } else {
            n = {0.0f, 0.0f, 1.0f}; // degenerate triangle
        }
        p.normal[f] = p.normal[f + 1] = p.normal[f + 2] = n;
    }
    p.generatedNormals = true;
}

namespace detail {

namespace {

std::optional<size_t> findAttr(const std::vector<fastgltf::Attribute>& attrs, std::string_view name) {
    for (const auto& a : attrs) {
        if (a.name == name) {
            return a.accessorIndex;
        }
    }
    return std::nullopt;
}

// Reads a float attribute with `want` components (3 or 4 accepted for COLOR_0 via `alt`), count == vertexCount.
template <size_t N>
Result<std::vector<std::array<float, N>>> floatStream(const Context& c, size_t accessor, uint32_t vertexCount,
                                                      const std::string& what, uint32_t alt = 0) {
    uint32_t comps = 0;
    auto f = readFloats(c, accessor, comps);
    if (!f) {
        return std::unexpected(f.error());
    }
    if (comps != N && !(alt != 0 && comps == alt)) {
        return std::unexpected(fail(std::format("{}: accessor {} has {} components, {} expected", what, accessor, comps, N)));
    }
    const size_t count = comps ? f->size() / comps : 0;
    if (count != vertexCount) {
        return std::unexpected(fail(std::format("{}: accessor {} has {} elements, the primitive {} vertices", what,
                                                accessor, count, vertexCount)));
    }
    std::vector<std::array<float, N>> out(count);
    for (size_t i = 0; i < count; ++i) {
        for (size_t k = 0; k < N; ++k) {
            out[i][k] = k < comps ? (*f)[i * comps + k] : 1.0f; // VEC3 colour: alpha 1
        }
    }
    return out;
}

} // namespace

Result<bool> buildPrimitive(const Context& c, const ModelData& model, size_t mesh, size_t index,
                            const fastgltf::Primitive& in, Primitive& out) {
    const std::string where = std::format("mesh {} primitive {}", mesh, index);
    const auto pos = findAttr(in.attributes, "POSITION");
    if (!pos) {
        c.warn(where + ": no POSITION attribute: primitive skipped");
        return false;
    }
    // KHR_draco_mesh_compression (plan F3 Task 3): the Draco stream is decoded first; its attribute and index
    // accessors then read the decoded elements (Context::accessorData) like any other accessor.
    if (auto r = decodeDracoPrimitive(c, in, where); !r) {
        return std::unexpected(r.error());
    }
    // Every accessor of the primitive is range-checked first (Review Focus 1: an out-of-range or huge accessor is an
    // error even when the primitive would then be dropped by the r.Asset.MaxVertices cap).
    for (const fastgltf::Attribute& a : in.attributes) {
        if (auto r = validateAccessor(c, a.accessorIndex); !r) {
            return std::unexpected(fail(std::format("{} {}: {}", where, a.name, r.error().message)));
        }
    }
    for (const auto& target : in.targets) {
        for (const fastgltf::Attribute& a : target) {
            if (auto r = validateAccessor(c, a.accessorIndex); !r) {
                return std::unexpected(fail(std::format("{} target {}: {}", where, a.name, r.error().message)));
            }
        }
    }
    if (in.indicesAccessor) {
        if (auto r = validateAccessor(c, *in.indicesAccessor); !r) {
            return std::unexpected(fail(std::format("{} indices: {}", where, r.error().message)));
        }
    }
    const uint64_t count = c.asset.accessors[*pos].count;
    if (count > c.options.maxVertices) {
        c.warn(std::format("{}: {} vertices exceed r.Asset.MaxVertices ({}): primitive skipped", where, count,
                           c.options.maxVertices));
        return false;
    }
    const auto vertexCount = static_cast<uint32_t>(count);
    out.vertexCount = vertexCount;
    {
        auto p = floatStream<3>(c, *pos, vertexCount, where + " POSITION");
        if (!p) {
            return std::unexpected(p.error());
        }
        out.position = std::move(*p);
    }
    for (const fastgltf::Attribute& a : in.attributes) {
        const std::string what = where + " " + a.name;
        if (a.name == "POSITION") {
            continue;
        }
        if (a.name == "NORMAL") {
            auto s = floatStream<3>(c, a.accessorIndex, vertexCount, what);
            if (!s) {
                return std::unexpected(s.error());
            }
            out.normal = std::move(*s);
        } else if (a.name == "TANGENT") {
            auto s = floatStream<4>(c, a.accessorIndex, vertexCount, what);
            if (!s) {
                return std::unexpected(s.error());
            }
            out.tangent = std::move(*s);
        } else if (a.name.rfind("TEXCOORD_", 0) == 0) {
            const std::string n = a.name.substr(9);
            const bool digits = !n.empty() && std::all_of(n.begin(), n.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
            const unsigned set = digits && n.size() < 4 ? static_cast<unsigned>(std::stoul(n)) : 9999u;
            if (set >= kMaxTexCoords) {
                c.warn(what + ": only TEXCOORD_0..3 are used: ignored");
                continue;
            }
            auto s = floatStream<2>(c, a.accessorIndex, vertexCount, what);
            if (!s) {
                return std::unexpected(s.error());
            }
            out.texCoord[set] = std::move(*s);
        } else if (a.name == "COLOR_0") {
            auto s = floatStream<4>(c, a.accessorIndex, vertexCount, what, 3);
            if (!s) {
                return std::unexpected(s.error());
            }
            out.color0 = std::move(*s);
        } else if (a.name == "JOINTS_0" || a.name == "JOINTS_1") {
            uint32_t comps = 0;
            auto u = readUints(c, a.accessorIndex, comps);
            if (!u) {
                return std::unexpected(u.error());
            }
            if (comps != 4 || u->size() / 4 != vertexCount) {
                return std::unexpected(fail(std::format("{}: must be VEC4 with {} elements", what, vertexCount)));
            }
            auto& dst = out.joints[a.name.back() - '0'];
            dst.resize(vertexCount);
            for (size_t i = 0; i < vertexCount; ++i) {
                for (size_t k = 0; k < 4; ++k) {
                    const uint32_t j = (*u)[i * 4 + k];
                    if (j > UINT16_MAX) {
                        return std::unexpected(fail(std::format("{}: joint {} > 65535", what, j)));
                    }
                    dst[i][k] = static_cast<uint16_t>(j);
                }
            }
        } else if (a.name == "WEIGHTS_0" || a.name == "WEIGHTS_1") {
            auto s = floatStream<4>(c, a.accessorIndex, vertexCount, what);
            if (!s) {
                return std::unexpected(s.error());
            }
            out.weights[a.name.back() - '0'] = std::move(*s);
        } else if (a.name.rfind("COLOR_", 0) == 0 || a.name.rfind("JOINTS_", 0) == 0 ||
                   a.name.rfind("WEIGHTS_", 0) == 0 || a.name.rfind('_', 0) == 0) {
            // COLOR_1+, JOINTS_2+, WEIGHTS_2+ and application-specific "_NAME" attributes: not used by the renderer.
            continue;
        } else {
            c.warn(what + ": unknown attribute ignored");
        }
    }
    // Morph targets: POSITION / NORMAL / TANGENT deltas (VEC3).
    for (size_t t = 0; t < in.targets.size(); ++t) {
        MorphTarget mt;
        for (const fastgltf::Attribute& a : in.targets[t]) {
            const std::string what = std::format("{} target {} {}", where, t, a.name);
            std::vector<Vec3>* dst = a.name == "POSITION" ? &mt.position
                                     : a.name == "NORMAL" ? &mt.normal
                                     : a.name == "TANGENT" ? &mt.tangent
                                                           : nullptr;
            if (!dst) {
                continue; // TEXCOORD_n / COLOR_n morphs: not used by the renderer (F4 scope)
            }
            auto s = floatStream<3>(c, a.accessorIndex, vertexCount, what);
            if (!s) {
                return std::unexpected(s.error());
            }
            *dst = std::move(*s);
        }
        out.targets.push_back(std::move(mt));
    }
    // Indices.
    if (in.indicesAccessor) {
        uint32_t comps = 0;
        auto u = readUints(c, *in.indicesAccessor, comps);
        if (!u) {
            return std::unexpected(u.error());
        }
        if (comps != 1) {
            return std::unexpected(fail(std::format("{}: indices accessor {} is not SCALAR", where, *in.indicesAccessor)));
        }
        for (const uint32_t i : *u) {
            if (i >= vertexCount) {
                return std::unexpected(fail(std::format("{}: index {} >= vertex count {}", where, i, vertexCount)));
            }
        }
        out.indices = std::move(*u);
    } else {
        out.indices.resize(vertexCount);
        for (uint32_t i = 0; i < vertexCount; ++i) {
            out.indices[i] = i;
        }
        out.generatedIndices = true;
    }
    // Topology: loop / fan -> lists.
    std::vector<uint32_t>& idx = out.indices;
    switch (in.type) {
    case fastgltf::PrimitiveType::Points: out.topology = Topology::PointList; break;
    case fastgltf::PrimitiveType::Lines:
        out.topology = Topology::LineList;
        idx.resize(idx.size() - idx.size() % 2u);
        break;
    case fastgltf::PrimitiveType::LineStrip: out.topology = Topology::LineStrip; break;
    case fastgltf::PrimitiveType::LineLoop: {
        out.topology = Topology::LineList;
        std::vector<uint32_t> list;
        if (idx.size() >= 2) {
            for (size_t i = 0; i < idx.size(); ++i) {
                list.insert(list.end(), {idx[i], idx[(i + 1) % idx.size()]});
            }
        }
        idx = std::move(list);
        break;
    }
    case fastgltf::PrimitiveType::Triangles:
        out.topology = Topology::TriangleList;
        if (idx.size() % 3u != 0) {
            c.warn(std::format("{}: {} indices is not a multiple of 3: the last partial triangle is dropped", where,
                               idx.size()));
            idx.resize(idx.size() - idx.size() % 3u);
        }
        break;
    case fastgltf::PrimitiveType::TriangleStrip: out.topology = Topology::TriangleStrip; break;
    case fastgltf::PrimitiveType::TriangleFan: {
        out.topology = Topology::TriangleList;
        std::vector<uint32_t> list;
        for (size_t i = 1; i + 1 < idx.size(); ++i) {
            list.insert(list.end(), {idx[0], idx[i], idx[i + 1]});
        }
        idx = std::move(list);
        break;
    }
    default: return std::unexpected(fail(std::format("{}: unknown mode {}", where, static_cast<int>(in.type))));
    }
    // Bounds.
    if (!out.position.empty()) {
        out.bounds.valid = true;
        out.bounds.min = out.bounds.max = out.position[0];
        for (const Vec3& p : out.position) {
            for (int k = 0; k < 3; ++k) {
                out.bounds.min[k] = std::min(out.bounds.min[k], p[k]);
                out.bounds.max[k] = std::max(out.bounds.max[k], p[k]);
            }
        }
    }
    // gltf_utils.js getExtentsFromAccessor: accessor.min / max, gltfAccessor.dequantize when normalized.
    {
        const fastgltf::Accessor& acc = c.asset.accessors[*pos];
        const auto read = [&](const fastgltf::AccessorBoundsArray& b, Vec3& v) {
            if (b.size() < 3) {
                return false;
            }
            for (size_t k = 0; k < 3; ++k) {
                double x = b.isType<double>() ? b.data<double>()[k] : double(b.data<std::int64_t>()[k]);
                if (acc.normalized) {
                    switch (acc.componentType) {
                    case fastgltf::ComponentType::Byte: x = std::max(x / 127.0, -1.0); break;
                    case fastgltf::ComponentType::UnsignedByte: x = x / 255.0; break;
                    case fastgltf::ComponentType::Short: x = std::max(x / 32767.0, -1.0); break;
                    case fastgltf::ComponentType::UnsignedShort: x = x / 65535.0; break;
                    default: break;
                    }
                }
                v[k] = static_cast<float>(x);
            }
            return true;
        };
        if (acc.min && acc.max && read(*acc.min, out.accessorBounds.min) && read(*acc.max, out.accessorBounds.max)) {
            out.accessorBounds.valid = true;
        }
    }
    // Material.
    const Material* mat = nullptr;
    if (in.materialIndex) {
        if (*in.materialIndex >= model.materials.size()) {
            return std::unexpected(fail(std::format("{}: material {} does not exist", where, *in.materialIndex)));
        }
        out.material = static_cast<int>(*in.materialIndex);
        mat = &model.materials[*in.materialIndex];
    }
    const bool triangles = out.topology == Topology::TriangleList || out.topology == Topology::TriangleStrip;
    if (out.normal.empty() && triangles) {
        generateFlatNormals(out);
    }
    if (mat) {
        for (const TextureRef* t : {&mat->baseColor, &mat->metallicRoughness, &mat->normal, &mat->occlusion, &mat->emissive}) {
            if (t->valid() && t->texCoord < kMaxTexCoords && out.texCoord[t->texCoord].empty()) {
                c.warn(std::format("{}: material {} samples TEXCOORD_{}, which the primitive lacks: sampled at (0, 0)",
                                   where, out.material, t->texCoord));
                out.texCoord[t->texCoord].assign(out.vertexCount, Vec2{0.0f, 0.0f});
                out.generatedTexCoord = true;
            }
        }
        if (out.tangent.empty() && mat->normal.valid() && triangles && c.options.generateTangents) {
            std::string why;
            if (generateTangents(out, mat->normal.texCoord, &why)) {
                c.info(std::format("{}: normal map without TANGENT: MikkTSpace tangents generated", where));
            } else {
                c.warn(std::format("{}: normal map without TANGENT and no MikkTSpace tangents ({})", where, why));
            }
        }
    }
    return true;
}

} // namespace detail

} // namespace eng::asset
