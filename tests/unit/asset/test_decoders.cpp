// F3 decoders (plan F3 Task 3): KHR_draco_mesh_compression, EXT_meshopt_compression / KHR_meshopt_compression,
// KHR_mesh_quantization, KHR_texture_basisu (KTX2 / Basis Universal) and EXT_texture_webp.
//
// Suites:
//   asset.decoders  every decoder variant directory of glTF-Sample-Assets at the pinned commit (listed in kVariants,
//                   enumerated from tests/assets/glTF-Sample-Assets/Models/*/<variant> at e124e8d8): loads (or exits 2
//                   naming the later-phase extension it REQUIRES), decoded vertex counts = the counts the file
//                   declares, total index count = the plain glTF variant's, rest-pose world-space bounds = the plain
//                   variant's within one quantization step; KTX2 images keep their payload with the DFD transfer
//                   function; KTX2 -> BC7 -> CPU decode (bcdec) vs the PNG of the plain variant: mean abs error
//                   <= 8/255; WebP decoded to RGBA8.
// Expected values come from the files (accessor counts and the plain variant read here from the JSON, the PNG
// sources decoded by the loader's PNG path), never from a decoder run.
#include "AssetTestEnv.h"

#include "asset/Ktx2.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <draco/attributes/attribute_quantization_transform.h>
#include <draco/compression/decode.h>
#include <meshoptimizer.h>
#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include <bcdec.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>

using namespace eng::asset;
using namespace eng::asset::test;

namespace {

struct Variant {
    const char* model;
    const char* variant;
    // nullptr = loads; else the exit-2 message substring (the file REQUIRES an extension of a later phase).
    const char* requiredLaterPhase = nullptr;
};

// Every decoder variant directory at the pinned Sample-Assets commit (glob Models/*/<dir>, 2026-11-28).
constexpr Variant kVariants[] = {
    // glTF-Draco (KHR_draco_mesh_compression)
    {"Avocado", "glTF-Draco"},
    {"BarramundiFish", "glTF-Draco"},
    {"BoomBox", "glTF-Draco"},
    {"Box", "glTF-Draco"},
    {"BrainStem", "glTF-Draco"},
    {"CesiumMan", "glTF-Draco"},
    {"CesiumMilkTruck", "glTF-Draco"},
    {"Corset", "glTF-Draco"},
    {"Duck", "glTF-Draco"},
    {"Lantern", "glTF-Draco"},
    {"MorphPrimitivesTest", "glTF-Draco"},
    {"RiggedFigure", "glTF-Draco"},
    {"RiggedSimple", "glTF-Draco"},
    {"SunglassesKhronos", "glTF-Draco"}, // + EXT_texture_webp required
    {"VirtualCity", "glTF-Draco"},
    {"WaterBottle", "glTF-Draco"},
    // glTF-Meshopt (KHR_meshopt_compression + KHR_mesh_quantization), glTF-Meshopt-EXT (EXT_meshopt_compression)
    {"BrainStem", "glTF-Meshopt"},
    {"DragonAttenuation", "glTF-Meshopt"},
    {"MeshoptCubeTest", "glTF-Meshopt"},
    {"BrainStem", "glTF-Meshopt-EXT"},
    {"DragonAttenuation", "glTF-Meshopt-EXT"},
    // glTF-Quantized (KHR_mesh_quantization)
    {"AnimatedMorphCube", "glTF-Quantized"},
    {"Avocado", "glTF-Quantized"},
    {"Duck", "glTF-Quantized"},
    {"Lantern", "glTF-Quantized"},
    // glTF-KTX-BasisU (KHR_texture_basisu)
    {"AnisotropyBarnLamp", "glTF-KTX-BasisU"},
    {"ChronographWatch", "glTF-KTX-BasisU", "KHR_materials_transmission is supported from F6"},
    {"FlightHelmet", "glTF-KTX-BasisU"},
    {"StainedGlassLamp", "glTF-KTX-BasisU"},
    // Combined / other decoder variants
    {"ABeautifulGame", "glTF-Binary-KTX-ETC1S-Draco"}, // .glb: Draco streams and KTX2 images in bufferViews
    {"CarConcept", "glTF-KTX-BasisU-Draco"},
    {"CarConcept", "glTF-WEBP"},
    {"ChronographWatch", "glTF-WEBP", "KHR_materials_transmission is supported from F6"},
};

std::filesystem::path variantFile(const Variant& v) {
    return sampleFile(v.model, v.variant);
}

std::vector<char> readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open " << toUtf8(p));
    return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// GLB chunk `index` (0 JSON, 1 BIN) of a .glb, glTF 2.0 §4.4; empty for a .gltf.
std::vector<char> glbChunk(const std::vector<char>& f, int index) {
    if (f.size() < 12 || std::memcmp(f.data(), "glTF", 4) != 0) {
        return {};
    }
    size_t pos = 12;
    for (int i = 0; pos + 8 <= f.size(); ++i) {
        uint32_t len = 0;
        std::memcpy(&len, f.data() + pos, 4);
        REQUIRE(pos + 8 + len <= f.size());
        if (i == index) {
            return std::vector<char>(f.begin() + static_cast<ptrdiff_t>(pos + 8),
                                     f.begin() + static_cast<ptrdiff_t>(pos + 8 + len));
        }
        pos += 8 + len;
    }
    return {};
}

nlohmann::json readJson(const std::filesystem::path& p) {
    const std::vector<char> f = readFile(p);
    const std::vector<char> json = glbChunk(f, 0);
    return json.empty() ? nlohmann::json::parse(f.begin(), f.end()) : nlohmann::json::parse(json.begin(), json.end());
}

// POSITION accessor count of every primitive (mesh order) and the total index count, from the JSON.
struct JsonCounts {
    std::vector<uint64_t> vertices;
    uint64_t indices = 0;
};
JsonCounts jsonCounts(const nlohmann::json& d) {
    JsonCounts c;
    for (const auto& m : d.value("meshes", nlohmann::json::array())) {
        for (const auto& p : m.at("primitives")) {
            c.vertices.push_back(d.at("accessors").at(p.at("attributes").at("POSITION").get<size_t>()).at("count"));
            if (p.contains("indices")) {
                c.indices += d.at("accessors").at(p.at("indices").get<size_t>()).at("count").get<uint64_t>();
            }
        }
    }
    return c;
}

// --- rest-pose world-space bounds (independent of the renderer) ---------------------------------------------------
using M4 = std::array<double, 16>; // column-major

M4 mul(const M4& a, const M4& b) {
    M4 r{};
    for (int c = 0; c < 4; ++c) {
        for (int rr = 0; rr < 4; ++rr) {
            double s = 0;
            for (int k = 0; k < 4; ++k) {
                s += a[k * 4 + rr] * b[c * 4 + k];
            }
            r[c * 4 + rr] = s;
        }
    }
    return r;
}

M4 localMatrix(const Node& n) {
    M4 m{};
    if (n.hasMatrix) {
        for (int i = 0; i < 16; ++i) {
            m[i] = n.matrix[i];
        }
        return m;
    }
    const double x = n.rotation[0], y = n.rotation[1], z = n.rotation[2], w = n.rotation[3];
    const double sx = n.scale[0], sy = n.scale[1], sz = n.scale[2];
    m = {(1 - 2 * (y * y + z * z)) * sx, 2 * (x * y + z * w) * sx, 2 * (x * z - y * w) * sx, 0,
         2 * (x * y - z * w) * sy, (1 - 2 * (x * x + z * z)) * sy, 2 * (y * z + x * w) * sy, 0,
         2 * (x * z + y * w) * sz, 2 * (y * z - x * w) * sz, (1 - 2 * (x * x + y * y)) * sz, 0,
         n.translation[0], n.translation[1], n.translation[2], 1};
    return m;
}

std::array<double, 3> xform(const M4& m, const std::array<double, 3>& p) {
    return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}

struct Box3 {
    std::array<double, 3> lo{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                             std::numeric_limits<double>::max()};
    std::array<double, 3> hi{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
                             std::numeric_limits<double>::lowest()};
    void add(const std::array<double, 3>& p) {
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], p[k]);
            hi[k] = std::max(hi[k], p[k]);
        }
    }
};

// World matrices of every node (rest pose: node TRS / matrices) and the mesh nodes of the default scene
// (gltf.scene ?? 0).
struct World {
    std::vector<M4> world;
    std::vector<int> meshNodes;
};
World worldOf(const ModelData& m) {
    World w;
    w.world.assign(m.nodes.size(), M4{});
    const M4 id{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const int scene = m.defaultScene >= 0 ? m.defaultScene : 0;
    REQUIRE(static_cast<size_t>(scene) < m.scenes.size());
    std::vector<bool> inScene(m.nodes.size(), false);
    for (const int r : m.scenes[scene].nodes) {
        std::vector<int> stack{r};
        while (!stack.empty()) {
            const int n = stack.back();
            stack.pop_back();
            inScene[n] = true;
            for (const int ch : m.nodes[n].children) {
                stack.push_back(ch);
            }
        }
    }
    // Every root (joints may sit outside the scene's node list).
    for (size_t i = 0; i < m.nodes.size(); ++i) {
        if (m.nodes[i].parent >= 0) {
            continue;
        }
        std::vector<std::pair<int, M4>> stack{{static_cast<int>(i), id}};
        while (!stack.empty()) {
            auto [n, parent] = stack.back();
            stack.pop_back();
            w.world[n] = mul(parent, localMatrix(m.nodes[n]));
            if (inScene[n] && m.nodes[n].mesh >= 0) {
                w.meshNodes.push_back(n);
            }
            for (const int ch : m.nodes[n].children) {
                stack.emplace_back(ch, w.world[n]);
            }
        }
    }
    return w;
}

// The matrices that take a mesh node's vertices to world space: world(node), or for a skinned mesh
// world(joint) * inverseBindMatrix per joint (the mesh node's own transform is ignored, glTF 2.0 §3.7.3.3).
std::vector<M4> vertexMatrices(const ModelData& m, const World& w, int n) {
    const Node& node = m.nodes[n];
    if (node.skin < 0) {
        return {w.world[n]};
    }
    std::vector<M4> out;
    const Skin& s = m.skins[node.skin];
    for (size_t j = 0; j < s.joints.size(); ++j) {
        M4 ibm{};
        for (int k = 0; k < 16; ++k) {
            ibm[k] = s.inverseBindMatrices[j][k];
        }
        out.push_back(mul(w.world[s.joints[j]], ibm));
    }
    return out;
}

// Rest-pose world-space bounds of the default scene: default morph weights (node / mesh weights), skinning with the
// joint matrices of vertexMatrices() (weights normalised).
Box3 sceneBounds(const ModelData& m) {
    const World w = worldOf(m);
    Box3 b;
    for (const int n : w.meshNodes) {
        const Node& node = m.nodes[n];
        const Mesh& mesh = m.meshes[node.mesh];
        const std::vector<float>& weights = !node.weights.empty() ? node.weights : mesh.weights;
        const std::vector<M4> mats = vertexMatrices(m, w, n);
        for (const Primitive& p : mesh.primitives) {
            for (uint32_t v = 0; v < p.vertexCount; ++v) {
                std::array<double, 3> pos{p.position[v][0], p.position[v][1], p.position[v][2]};
                for (size_t t = 0; t < p.targets.size() && t < weights.size(); ++t) {
                    if (!p.targets[t].position.empty()) {
                        for (int k = 0; k < 3; ++k) {
                            pos[k] += weights[t] * p.targets[t].position[v][k];
                        }
                    }
                }
                if (node.skin >= 0 && !p.joints[0].empty() && !p.weights[0].empty()) {
                    std::array<double, 3> acc{0, 0, 0};
                    double wsum = 0;
                    for (int set = 0; set < 2; ++set) {
                        if (p.joints[set].empty() || p.weights[set].empty()) {
                            continue;
                        }
                        for (int k = 0; k < 4; ++k) {
                            const double wt = p.weights[set][v][k];
                            const uint16_t j = p.joints[set][v][k];
                            if (wt == 0.0 || j >= mats.size()) {
                                continue;
                            }
                            const auto q = xform(mats[j], pos);
                            for (int c = 0; c < 3; ++c) {
                                acc[c] += wt * q[c];
                            }
                            wsum += wt;
                        }
                    }
                    if (wsum > 0) {
                        b.add({acc[0] / wsum, acc[1] / wsum, acc[2] / wsum});
                    }
                } else {
                    b.add(xform(mats[0], pos));
                }
            }
        }
    }
    return b;
}

// --- quantization step of a variant file, from the file itself ----------------------------------------------------
// The whole of buffer `buffer`: an external .bin (percent-decoded URI) or, without a URI, the GLB BIN chunk.
std::vector<char> bufferBytes(const std::filesystem::path& gltf, const nlohmann::json& d, size_t buffer) {
    const auto& buf = d.at("buffers").at(buffer);
    if (!buf.contains("uri")) {
        return glbChunk(readFile(gltf), 1);
    }
    const std::string uri = buf.at("uri").get<std::string>();
    REQUIRE_MESSAGE(uri.rfind("data:", 0) != 0, "data-URI buffer");
    std::string decoded;
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            decoded.push_back(static_cast<char>(std::stoi(uri.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            decoded.push_back(uri[i]);
        }
    }
    return readFile(gltf.parent_path() / u8path(decoded));
}

// Bytes of bufferView `view`.
std::vector<char> bufferViewOf(const std::filesystem::path& gltf, const nlohmann::json& d, size_t view) {
    const auto& bv = d.at("bufferViews").at(view);
    const std::vector<char> all = bufferBytes(gltf, d, bv.at("buffer").get<size_t>());
    const size_t off = bv.value("byteOffset", size_t{0}), len = bv.at("byteLength").get<size_t>();
    REQUIRE(off + len <= all.size());
    return std::vector<char>(all.begin() + static_cast<ptrdiff_t>(off), all.begin() + static_cast<ptrdiff_t>(off + len));
}

// Bytes [offset, offset + length) of buffer `buffer`.
std::vector<char> bufferRange(const std::filesystem::path& gltf, const nlohmann::json& d, size_t buffer, size_t offset,
                              size_t length) {
    const std::vector<char> all = bufferBytes(gltf, d, buffer);
    REQUIRE(offset + length <= all.size());
    return std::vector<char>(all.begin() + static_cast<ptrdiff_t>(offset),
                             all.begin() + static_cast<ptrdiff_t>(offset + length));
}

// Position quantization step (local units) of an accessor:
//   integer component types (KHR_mesh_quantization): 1, normalized 1 / (2^(bits-1) - 1) (signed) or 1 / (2^bits - 1);
//   FLOAT in a meshopt view with the EXPONENTIAL filter (lossy: 24-bit mantissa M, 8-bit exponent E, value M * 2^E):
//     2^E, the largest exponent of the view's stream (decoded here without the filter);
//   other FLOAT: 0.
double accessorStep(const std::filesystem::path& gltf, const nlohmann::json& d, const nlohmann::json& a) {
    const int ct = a.at("componentType").get<int>();
    const bool norm = a.value("normalized", false);
    switch (ct) {
    case 5120: return norm ? 1.0 / 127.0 : 1.0;
    case 5121: return norm ? 1.0 / 255.0 : 1.0;
    case 5122: return norm ? 1.0 / 32767.0 : 1.0;
    case 5123: return norm ? 1.0 / 65535.0 : 1.0;
    default: break;
    }
    if (!a.contains("bufferView")) {
        return 0.0;
    }
    const auto& bv = d.at("bufferViews").at(a.at("bufferView").get<size_t>());
    const nlohmann::json* ext = nullptr;
    for (const char* name : {"KHR_meshopt_compression", "EXT_meshopt_compression"}) {
        if (bv.contains("extensions") && bv["extensions"].contains(name)) {
            ext = &bv["extensions"][name];
        }
    }
    if (!ext || ext->value("filter", std::string("NONE")) != "EXPONENTIAL") {
        return 0.0;
    }
    const size_t count = ext->at("count"), stride = ext->at("byteStride");
    const std::vector<char> src = bufferRange(gltf, d, ext->at("buffer"), ext->value("byteOffset", size_t{0}),
                                              ext->at("byteLength"));
    std::vector<int32_t> raw(count * stride / 4);
    REQUIRE(meshopt_decodeVertexBuffer(raw.data(), count, stride, reinterpret_cast<const unsigned char*>(src.data()),
                                       src.size()) == 0);
    int e = -127;
    for (const int32_t v : raw) {
        e = std::max(e, v >> 24); // signed 8-bit exponent in the top byte
    }
    return std::ldexp(1.0, e);
}

// Draco position quantization step (local units): the stream decoded here with the attribute transform kept
// (Decoder::SetSkipAttributeTransform), step = range / (2^bits - 1) of its AttributeQuantizationTransform; 0 when the
// positions are not quantized.
double dracoStep(const std::filesystem::path& gltf, const nlohmann::json& d, const nlohmann::json& ext) {
    const std::vector<char> bytes = bufferViewOf(gltf, d, ext.at("bufferView").get<size_t>());
    draco::DecoderBuffer buf;
    buf.Init(bytes.data(), bytes.size());
    draco::Decoder dec;
    dec.SetSkipAttributeTransform(draco::GeometryAttribute::POSITION);
    auto pc = dec.DecodePointCloudFromBuffer(&buf); // a mesh decodes as a point cloud too; only the attribute is used
    REQUIRE(pc.ok());
    const draco::PointAttribute* pa =
        pc.value()->GetAttributeByUniqueId(ext.at("attributes").at("POSITION").get<uint32_t>());
    REQUIRE(pa != nullptr);
    draco::AttributeQuantizationTransform q;
    if (!q.InitFromAttribute(*pa)) {
        return 0.0;
    }
    return static_cast<double>(q.range()) / static_cast<double>((1u << q.quantization_bits()) - 1u);
}

// One quantization step of the variant's world-space positions, per axis: for every mesh primitive of the default
// scene, its local step (Draco stream, or the POSITION / morph POSITION accessor types) times sum_i |M_ki| over the
// matrices taking it to world space (a local error of half a step per component moves world axis k by at most
// step / 2 * sum_i |M_ki|; one full step is the tolerance).
std::array<double, 3> worldStep(const std::filesystem::path& gltf, const nlohmann::json& d, const ModelData& m) {
    const World w = worldOf(m);
    std::array<double, 3> step{0, 0, 0};
    for (const int n : w.meshNodes) {
        const Node& node = m.nodes[n];
        const auto& jprims = d.at("meshes").at(node.mesh).at("primitives");
        const std::vector<float>& weights = !node.weights.empty() ? node.weights : m.meshes[node.mesh].weights;
        for (const auto& jp : jprims) {
            double unit = 0.0;
            if (jp.contains("extensions") && jp["extensions"].contains("KHR_draco_mesh_compression")) {
                unit = dracoStep(gltf, d, jp["extensions"]["KHR_draco_mesh_compression"]);
            } else {
                unit = accessorStep(gltf, d, d.at("accessors").at(jp.at("attributes").at("POSITION").get<size_t>()));
            }
            const auto targets = jp.value("targets", nlohmann::json::array());
            for (size_t t = 0; t < targets.size() && t < weights.size(); ++t) {
                if (targets[t].contains("POSITION")) {
                    unit += std::abs(weights[t]) *
                            accessorStep(gltf, d, d.at("accessors").at(targets[t]["POSITION"].get<size_t>()));
                }
            }
            for (const M4& mm : vertexMatrices(m, w, n)) {
                for (int k = 0; k < 3; ++k) {
                    step[k] = std::max(step[k], unit * (std::abs(mm[0 * 4 + k]) + std::abs(mm[1 * 4 + k]) +
                                                        std::abs(mm[2 * 4 + k])));
                }
            }
        }
    }
    return step;
}

// CPU decode of a transcoded level (bcdec for BC1 / BC3 / BC7; RGBA8 copied) -> RGBA8 rows.
std::vector<uint8_t> cpuDecode(const TranscodedLevel& l, TranscodeTarget t) {
    std::vector<uint8_t> out(size_t(l.width) * l.height * 4u);
    if (t == TranscodeTarget::RGBA8) {
        REQUIRE(l.data.size() == out.size());
        return l.data;
    }
    const uint32_t bx = (l.width + 3) / 4, by = (l.height + 3) / 4;
    const size_t blockBytes = t == TranscodeTarget::BC1 ? 8 : 16;
    REQUIRE(l.data.size() == size_t(bx) * by * blockBytes);
    uint8_t block[4 * 4 * 4];
    for (uint32_t y = 0; y < by; ++y) {
        for (uint32_t x = 0; x < bx; ++x) {
            const uint8_t* src = l.data.data() + (size_t(y) * bx + x) * blockBytes;
            switch (t) {
            case TranscodeTarget::BC1: bcdec_bc1(src, block, 16); break;
            case TranscodeTarget::BC3: bcdec_bc3(src, block, 16); break;
            case TranscodeTarget::BC7: bcdec_bc7(src, block, 16); break;
            default: REQUIRE_MESSAGE(false, "no CPU decoder for " << toString(t));
            }
            for (uint32_t r = 0; r < 4 && y * 4 + r < l.height; ++r) {
                for (uint32_t c = 0; c < 4 && x * 4 + c < l.width; ++c) {
                    std::memcpy(&out[((size_t(y) * 4 + r) * l.width + x * 4 + c) * 4], &block[(r * 4 + c) * 4], 4);
                }
            }
        }
    }
    return out;
}

// Mean absolute error over the first `channels` channels, in 1/255 units.
double meanAbsError(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int channels) {
    REQUIRE(a.size() == b.size());
    double sum = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4) {
        for (int c = 0; c < channels; ++c) {
            sum += std::abs(int(a[i + c]) - int(b[i + c]));
            ++n;
        }
    }
    return n ? sum / double(n) : 0.0;
}

// KTX2 DFD (KTX 2.0 spec §3.10 / Khronos Data Format 1.3): an alpha sample = ETC1S channel 15 (AAA) or UASTC
// channel 3 (RGBA). Parsed here, independent of libktx.
bool ktx2HasAlpha(const std::vector<uint8_t>& f) {
    const auto u32 = [&](size_t o) {
        uint32_t v = 0;
        REQUIRE(o + 4 <= f.size());
        std::memcpy(&v, f.data() + o, 4);
        return v;
    };
    const size_t dfd = u32(48);
    const uint32_t model = u32(dfd + 4 + 8) & 0xFFu;
    const uint32_t blockBytes = u32(dfd + 4 + 4) >> 16;
    const uint32_t samples = (blockBytes - 24) / 16;
    for (uint32_t i = 0; i < samples; ++i) {
        const uint32_t channel = (u32(dfd + 4 + 24 + 16 * i) >> 24) & 0xFu;
        if ((model == 163 && channel == 15) || (model == 166 && channel == 3)) {
            return true;
        }
    }
    return false;
}

std::string stem(const std::string& uri) {
    const size_t slash = uri.find_last_of('/');
    const std::string f = slash == std::string::npos ? uri : uri.substr(slash + 1);
    return f.substr(0, f.find_last_of('.'));
}

} // namespace

TEST_SUITE("asset.decoders") {

TEST_CASE("every decoder variant loads (or names the later-phase extension it requires)") {
    for (const Variant& v : kVariants) {
        const std::string name = std::string(v.model) + "/" + v.variant;
        CAPTURE(name);
        const Loaded l = load(variantFile(v));
        if (v.requiredLaterPhase) {
            REQUIRE_FALSE(l.model.has_value());
            CHECK(l.model.error().exitCode == 2);
            CHECK(l.model.error().message.find(v.requiredLaterPhase) != std::string::npos);
            continue;
        }
        const ModelData& m = ok(l);
        CHECK_FALSE(m.meshes.empty());
        // No decoder extension is reported as "not implemented" any more.
        CHECK_FALSE(l.has(Diagnostic::Info, "decoder not implemented"));
        CHECK_FALSE(l.has(Diagnostic::Info, "kept encoded, decoder in plan F3 Task 3"));
    }
}

TEST_CASE("decoded vertex counts = the file's accessor counts; index totals = the plain glTF variant's") {
    for (const Variant& v : kVariants) {
        if (v.requiredLaterPhase) {
            continue;
        }
        const std::string name = std::string(v.model) + "/" + v.variant;
        CAPTURE(name);
        const nlohmann::json vj = readJson(variantFile(v));
        const nlohmann::json pj = readJson(sampleFile(v.model, "glTF"));
        const JsonCounts vc = jsonCounts(vj), pc = jsonCounts(pj);
        // The encoders keep every triangle: the index totals of the two files agree (checked on the JSON first, so a
        // failure here is the file, not the decoder). One file differs by design: ABeautifulGame's KTX-ETC1S-Draco .glb
        // declares 1721856 indices, its plain glTF 1723584 (pinned here, so a change of either file is noticed).
        if (std::string(v.model) == "ABeautifulGame") {
            CHECK(vc.indices == 1721856u);
            CHECK(pc.indices == 1723584u);
        } else {
            REQUIRE(vc.indices == pc.indices);
        }
        const Loaded l = load(variantFile(v));
        const ModelData& m = ok(l);
        std::vector<uint64_t> decoded;
        uint64_t indices = 0, decodedTotal = 0;
        size_t prim = 0;
        for (const Mesh& mesh : m.meshes) {
            for (const Primitive& p : mesh.primitives) {
                // Flat normals de-index a primitive (Task 2); none of these files lacks NORMAL.
                CHECK_FALSE(p.generatedNormals);
                // MikkTSpace (normal map without TANGENT, Task 2) splits vertices at tangent seams: the count is then
                // the generator's, compared with the plain variant's below.
                if (!p.generatedTangents && prim < vc.vertices.size()) {
                    CAPTURE(prim);
                    CHECK(p.vertexCount == vc.vertices[prim]);
                }
                decoded.push_back(p.vertexCount);
                decodedTotal += p.vertexCount;
                indices += p.indices.size();
                ++prim;
            }
        }
        CHECK(decoded.size() == vc.vertices.size());
        CHECK(indices == vc.indices);
        // The plan's "decoded vertex count = the plain variant's" holds where the encoder kept the vertex set; Draco
        // (attribute seams / dedup) and gltfpack (mesh merging, dedup) change it in the files themselves, so for
        // those the file's own counts above are the check (numbers in the plan F3 Task 3 worklist row).
        uint64_t vt = 0, pt = 0;
        for (const uint64_t c : vc.vertices) {
            vt += c;
        }
        for (const uint64_t c : pc.vertices) {
            pt += c;
        }
        if (vt == pt) {
            const Loaded lp = load(sampleFile(v.model, "glTF"));
            uint64_t plainTotal = 0;
            for (const Mesh& mesh : ok(lp).meshes) {
                for (const Primitive& p : mesh.primitives) {
                    plainTotal += p.vertexCount;
                }
            }
            CHECK(decodedTotal == plainTotal);
        } else {
            MESSAGE(name << ": the file declares " << vt << " vertices, the plain glTF " << pt);
        }
    }
}

TEST_CASE("rest-pose world-space bounds match the plain glTF variant within one quantization step") {
    for (const Variant& v : kVariants) {
        if (v.requiredLaterPhase) {
            continue;
        }
        const std::string name = std::string(v.model) + "/" + v.variant;
        CAPTURE(name);
        const Loaded lv = load(variantFile(v));
        const Loaded lp = load(sampleFile(v.model, "glTF"));
        const Box3 bv = sceneBounds(ok(lv));
        const Box3 bp = sceneBounds(ok(lp));
        const std::array<double, 3> step = worldStep(variantFile(v), readJson(variantFile(v)), ok(lv));
        for (int k = 0; k < 3; ++k) {
            const double extent = bp.hi[k] - bp.lo[k];
            // + float32 rounding of the plain variant's own positions
            const double tol = step[k] + 1e-6 * std::max(extent, 1.0);
            CAPTURE(k);
            CAPTURE(extent);
            CAPTURE(step[k]);
            CHECK(std::abs(bv.lo[k] - bp.lo[k]) <= tol);
            CHECK(std::abs(bv.hi[k] - bp.hi[k]) <= tol);
            MESSAGE(name << " axis " << k << ": bound errors " << std::abs(bv.lo[k] - bp.lo[k]) << " / "
                         << std::abs(bv.hi[k] - bp.hi[k]) << ", one quantization step " << step[k]);
        }
    }
}

TEST_CASE("WebP images decode to RGBA8 (EXT_texture_webp)") {
    const Loaded l = load(sampleFile("CarConcept", "glTF-WEBP"));
    const ModelData& m = ok(l);
    REQUIRE_FALSE(m.images.empty());
    for (const Image& img : m.images) {
        CAPTURE(img.uri);
        CHECK(img.mimeType == "image/webp");
        CHECK(img.decoded);
        CHECK(img.rgba8.size() == size_t(img.width) * img.height * 4u);
    }
}

TEST_CASE("KHR_texture_basisu: the texture source is the KTX2 image") {
    const Loaded l = load(sampleFile("FlightHelmet", "glTF-KTX-BasisU"));
    const ModelData& m = ok(l);
    REQUIRE_FALSE(m.materials.empty());
    for (const Material& mat : m.materials) {
        for (const TextureRef* t : {&mat.baseColor, &mat.normal, &mat.metallicRoughness, &mat.occlusion}) {
            if (t->valid()) {
                CHECK(m.images[t->image].mimeType == "image/ktx2");
            }
        }
    }
}

} // TEST_SUITE

TEST_SUITE("asset.decoders") {

TEST_CASE("KTX2: payload kept, levels and DFD transfer function (FlightHelmet: ETC1S base colours, UASTC others)") {
    const Loaded l = load(sampleFile("FlightHelmet", "glTF-KTX-BasisU"));
    const ModelData& m = ok(l);
    REQUIRE(m.images.size() == 15);
    for (const Image& img : m.images) {
        CAPTURE(img.uri);
        CHECK(img.ktx2);
        CHECK_FALSE(img.decoded);
        CHECK_FALSE(img.encoded.empty());
        const bool base = img.uri.find("BaseColor") != std::string::npos;
        // The DFD says sRGB exactly for the base colour textures (the only sRGB slot this asset uses).
        CHECK(img.ktx2Srgb == base);
        CHECK(img.colorSpace == (base ? ColorSpace::Srgb : ColorSpace::Linear));
        const bool lenses = img.uri.find("Lenses") != std::string::npos;
        CHECK(img.width == (lenses ? 1024u : 2048u));
        CHECK(img.ktx2BaseLevel == 0);
        CHECK(img.ktx2Levels == (lenses ? 11u : 12u)); // full chains, as the KTX2 headers say
    }
}

TEST_CASE("KTX2 -> BC7 -> CPU decode vs the PNG of the plain variant: mean abs error <= 8/255") {
    const Loaded lk = load(sampleFile("FlightHelmet", "glTF-KTX-BasisU"));
    const Loaded lp = load(sampleFile("FlightHelmet", "glTF"));
    const ModelData& mk = ok(lk);
    const ModelData& mp = ok(lp);
    size_t compared = 0;
    for (const Image& k : mk.images) {
        CAPTURE(k.uri);
        const auto png = std::find_if(mp.images.begin(), mp.images.end(),
                                      [&](const Image& p) { return stem(p.uri) == stem(k.uri); });
        REQUIRE(png != mp.images.end());
        REQUIRE(png->decoded);
        auto t = transcodeKtx2(k, TranscodeTarget::BC7);
        REQUIRE_MESSAGE(t.has_value(), (t ? std::string() : t.error().message));
        CHECK((t->target == TranscodeTarget::BC7));
        CHECK(t->srgb == k.ktx2Srgb);
        REQUIRE(t->levels.size() == k.ktx2Levels);
        REQUIRE(t->levels[0].width == png->width);
        REQUIRE(t->levels[0].height == png->height);
        const std::vector<uint8_t> px = cpuDecode(t->levels[0], t->target);
        // The channels the KTX2 file carries: RGB, + alpha when its DFD has an alpha sample. (LensesMat_BaseColor.ktx2
        // has one RGB sample; its PNG's alpha (mean 0.43) is unused: the material is OPAQUE.)
        const int channels = ktx2HasAlpha(k.encoded) ? 4 : 3;
        const double err = meanAbsError(px, png->rgba8, channels);
        MESSAGE(stem(k.uri) << ": BC7 mean abs error " << err << " / 255 over " << channels << " channels");
        CHECK(err <= 8.0);
        ++compared;
    }
    CHECK(compared == 15);
}

TEST_CASE("KTX2 -> BC1 / BC3 and RGBA8 transcodes; target choice from the device's support") {
    const Loaded l = load(sampleFile("FlightHelmet", "glTF-KTX-BasisU"));
    const ModelData& m = ok(l);
    const Image& img = m.images.front();
    auto bc = transcodeKtx2(img, TranscodeTarget::BC3);
    REQUIRE(bc.has_value());
    CHECK((bc->target == TranscodeTarget::BC1 || bc->target == TranscodeTarget::BC3));
    auto rgba = transcodeKtx2(img, TranscodeTarget::RGBA8);
    REQUIRE(rgba.has_value());
    CHECK((rgba->target == TranscodeTarget::RGBA8));
    // The BC1 / BC3 texels and the RGBA8 transcode of the same Basis data agree closely (both from the same ETC1S /
    // UASTC blocks; the BC encode adds its own error).
    const double err = meanAbsError(cpuDecode(bc->levels[0], bc->target), rgba->levels[0].data, 3);
    MESSAGE(std::string(toString(bc->target)) << " vs RGBA8 transcode: mean abs error " << err << " / 255");
    CHECK(err <= 8.0);

    TranscodeSupport s;
    CHECK((chooseTranscodeTarget(s, img) == TranscodeTarget::RGBA8));
    s.etc2 = true;
    CHECK((chooseTranscodeTarget(s, img) == TranscodeTarget::ETC2_RGBA8));
    s.astc = true;
    CHECK((chooseTranscodeTarget(s, img) == TranscodeTarget::ASTC_4x4));
    s.bc1bc3 = true;
    CHECK((chooseTranscodeTarget(s, img) == TranscodeTarget::BC3));
    s.bc7 = true;
    CHECK((chooseTranscodeTarget(s, img) == TranscodeTarget::BC7));
    Image odd = img;
    odd.width = 30; // not a multiple of 4: block formats refused
    CHECK((chooseTranscodeTarget(s, odd) == TranscodeTarget::RGBA8));
}

TEST_CASE("KTX2 with alpha (StainedGlassLamp: UASTC RGBA, ETC1S RGB + A): BC1/BC3 -> BC3, BC7 alpha vs the PNG") {
    const Loaded lk = load(sampleFile("StainedGlassLamp", "glTF-KTX-BasisU"));
    const Loaded lp = load(sampleFile("StainedGlassLamp", "glTF"));
    const ModelData& mk = ok(lk);
    const ModelData& mp = ok(lp);
    size_t withAlpha = 0;
    for (const Image& k : mk.images) {
        if (!ktx2HasAlpha(k.encoded)) {
            continue;
        }
        CAPTURE(k.uri);
        ++withAlpha;
        auto bc = transcodeKtx2(k, TranscodeTarget::BC1);
        REQUIRE(bc.has_value());
        CHECK((bc->target == TranscodeTarget::BC3)); // an image with alpha never goes to BC1
        auto bc7 = transcodeKtx2(k, TranscodeTarget::BC7);
        REQUIRE(bc7.has_value());
        // Reference: the plain variant's PNG when it has the KTX2's size (glass, 2048), else the RGBA8 transcode of the
        // same KTX2 (grill: KTX2 1024 x 1024, PNG 2048 x 2048).
        const auto png = std::find_if(mp.images.begin(), mp.images.end(),
                                      [&](const Image& p) { return stem(p.uri) == stem(k.uri); });
        REQUIRE(png != mp.images.end());
        std::vector<uint8_t> ref;
        std::string refName = "PNG";
        if (png->width == bc7->levels[0].width && png->height == bc7->levels[0].height) {
            ref = png->rgba8;
        } else {
            auto rgba = transcodeKtx2(k, TranscodeTarget::RGBA8);
            REQUIRE(rgba.has_value());
            ref = rgba->levels[0].data;
            refName = "RGBA8 transcode";
        }
        const auto alphaError = [&](const std::vector<uint8_t>& q) {
            REQUIRE(q.size() == ref.size());
            double e = 0;
            for (size_t i = 3; i < q.size(); i += 4) {
                e += std::abs(int(q[i]) - int(ref[i]));
            }
            return e / double(q.size() / 4);
        };
        const std::vector<uint8_t> px = cpuDecode(bc7->levels[0], bc7->target);
        const double err = meanAbsError(px, ref, 4);
        const double bc7Alpha = alphaError(px);
        const double bc3Alpha = alphaError(cpuDecode(bc->levels[0], bc->target));
        MESSAGE(stem(k.uri) << " vs " << refName << ": BC7 mean abs error " << err << " / 255 over 4 channels, alpha "
                            << bc7Alpha << "; BC3 alpha " << bc3Alpha);
        CHECK(err <= 8.0);
        CHECK(bc7Alpha <= 8.0);
        CHECK(bc3Alpha <= 8.0);
    }
    CHECK(withAlpha == 2); // glass (UASTC RGBA) and grill (ETC1S RGB + AAA)
}

TEST_CASE("KTX2 with r.Asset.MaxTextureSize: larger mip levels are skipped with a warning") {
    LoadOptions o;
    o.maxTextureSize = 512;
    const Loaded l = load(sampleFile("FlightHelmet", "glTF-KTX-BasisU"), o);
    const ModelData& m = ok(l);
    CHECK(l.has(Diagnostic::Warning, "r.Asset.MaxTextureSize"));
    for (const Image& img : m.images) {
        CAPTURE(img.uri);
        CHECK(img.ktx2);
        CHECK(img.width == 512);
        CHECK(img.height == 512);
        const bool lenses = img.uri.find("Lenses") != std::string::npos;
        CHECK(img.ktx2BaseLevel == (lenses ? 1u : 2u));
        auto t = transcodeKtx2(img, TranscodeTarget::BC7);
        REQUIRE(t.has_value());
        CHECK(t->levels[0].width == 512);
        CHECK(t->levels.size() == img.ktx2Levels);
    }
}

} // TEST_SUITE
