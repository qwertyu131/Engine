// glTF 2.0 loader (plan F3 Task 2; spec §7.1). Order:
//   1. the file (size cap r.Asset.MaxFileBytes), GLB container check (header / chunk lengths vs the file size);
//   2. the JSON parsed once with nlohmann (malformed JSON is an error) for the extension policy (Extensions.cpp),
//      the data-URI base64 check (fastgltf only reports "InvalidURI" without naming the buffer / image) and the
//      raw material "extensions" objects kept for F6;
//   3. fastgltf (pinned v1.0.1) parses the structure with every extension it knows enabled (the policy already
//      rejected the unsupported required ones); external buffers / images are read here, not by fastgltf, so URIs
//      (percent-encoded or raw UTF-8) resolve relative to the file and the size cap applies to them too;
//   4. conversion to ModelData: every index into another array is checked, accessors are decoded with bounds
//      checks (Accessors.cpp), primitives post-processed (Primitives.cpp), images decoded (Images.cpp).
// Every failure is Error{2, message}; nothing here throws past loadGltf (fastgltf is noexcept, nlohmann is used in
// non-throwing mode, std::bad_alloc is caught).
#include "Extensions.h"
#include "Internal.h"

#include "core/CVar.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <new>
#include <system_error>

namespace eng::asset {

namespace detail {

namespace {

std::string u8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

bool isBase64Char(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '+' || ch == '/';
}

// "data:<mime>;base64,<payload>": the payload is strict base64 (length % 4 == 0, '=' only as 1-2 trailing pads).
bool validBase64DataUri(std::string_view uri) {
    const size_t comma = uri.find(',');
    if (comma == std::string_view::npos) {
        return false;
    }
    const std::string_view header = uri.substr(0, comma);
    if (header.size() < 12 || header.substr(header.size() - 7) != ";base64") {
        return false;
    }
    const std::string_view payload = uri.substr(comma + 1);
    if (payload.size() < 4 || payload.size() % 4 != 0) {
        return false;
    }
    size_t pad = 0;
    while (pad < 2 && payload[payload.size() - 1 - pad] == '=') {
        ++pad;
    }
    for (size_t i = 0; i < payload.size() - pad; ++i) {
        if (!isBase64Char(static_cast<unsigned char>(payload[i]))) {
            return false;
        }
    }
    return true;
}

Result<void> checkDataUris(const nlohmann::json& doc, uint64_t cap) {
    for (const char* key : {"buffers", "images"}) {
        const auto it = doc.find(key);
        if (it == doc.end() || !it->is_array()) {
            continue;
        }
        for (size_t i = 0; i < it->size(); ++i) {
            const auto& e = (*it)[i];
            if (!e.is_object() || !e.contains("uri") || !e["uri"].is_string()) {
                continue;
            }
            const std::string& uri = e["uri"].get_ref<const std::string&>();
            if (uri.rfind("data:", 0) != 0) {
                continue;
            }
            const std::string what = std::format("{} {}", std::string_view(key).substr(0, std::strlen(key) - 1), i);
            if (!validBase64DataUri(uri)) {
                return std::unexpected(fail(what + ": data URI is not valid base64 (\"data:<mime>;base64,<payload>\")"));
            }
            if (uri.size() / 4 * 3 > cap) {
                return std::unexpected(
                    fail(std::format("{}: data URI decodes to more than r.Asset.MaxFileBytes ({} bytes)", what, cap)));
            }
        }
    }
    return {};
}

// GLB container (glTF 2.0 §4.4): 12-byte header, chunk 0 JSON, optional chunk 1 BIN, lengths within the file.
Result<std::string_view> glbJson(std::span<const std::byte> f) {
    const auto u32 = [&](size_t off) {
        uint32_t v = 0;
        std::memcpy(&v, f.data() + off, 4);
        return v;
    };
    if (f.size() < 12) {
        return std::unexpected(fail(std::format("GLB: {} bytes, shorter than the 12-byte header (truncated)", f.size())));
    }
    if (u32(4) != 2) {
        return std::unexpected(fail(std::format("GLB: container version {} (2 expected)", u32(4))));
    }
    const uint64_t length = u32(8);
    if (length > f.size()) {
        return std::unexpected(fail(std::format("GLB: header length {} > file size {} (truncated)", length, f.size())));
    }
    if (length < f.size()) {
        return std::unexpected(fail(std::format("GLB: header length {} != file size {}", length, f.size())));
    }
    uint64_t pos = 12;
    std::string_view json;
    for (int chunk = 0; pos < length; ++chunk) {
        if (pos + 8 > length) {
            return std::unexpected(fail(std::format("GLB chunk {}: header at byte {} past the end (truncated)", chunk, pos)));
        }
        const uint64_t clen = u32(static_cast<size_t>(pos));
        const uint32_t type = u32(static_cast<size_t>(pos) + 4);
        if (pos + 8 + clen > length) {
            return std::unexpected(fail(std::format("GLB chunk {}: length {} at byte {} extends past the end of the {}-byte "
                                                    "file (truncated)",
                                                    chunk, clen, pos, length)));
        }
        if (chunk == 0) {
            if (type != 0x4E4F534Au) { // "JSON"
                return std::unexpected(fail("GLB chunk 0 is not JSON"));
            }
            json = std::string_view(reinterpret_cast<const char*>(f.data() + pos + 8), static_cast<size_t>(clen));
        } else if (chunk == 1 && type != 0x004E4942u) { // "BIN\0"
            // Unknown chunk types are skipped (§4.4.3.3); only a second chunk that claims to be BIN is used.
        }
        pos += 8 + clen;
    }
    if (json.empty()) {
        return std::unexpected(fail("GLB: no JSON chunk"));
    }
    return json;
}

const nlohmann::json* arrayAt(const nlohmann::json& doc, const char* key) {
    const auto it = doc.find(key);
    return it != doc.end() && it->is_array() ? &*it : nullptr;
}

TextureRef textureRef(const Context& c, const fastgltf::TextureInfo& t, float scale, const std::string& what,
                      Result<void>& err) {
    TextureRef r;
    r.texCoord = static_cast<uint32_t>(t.texCoordIndex);
    r.scale = scale;
    if (t.textureIndex >= c.asset.textures.size()) {
        err = std::unexpected(fail(std::format("{}: texture {} does not exist", what, t.textureIndex)));
        return r;
    }
    const fastgltf::Texture& tex = c.asset.textures[t.textureIndex];
    // The core source; KHR_texture_basisu / EXT_texture_webp sources are used from plan F3 Task 3 on.
    const auto src = tex.imageIndex    ? tex.imageIndex
                     : tex.basisuImageIndex ? tex.basisuImageIndex
                                            : tex.webpImageIndex;
    if (src) {
        if (*src >= c.asset.images.size()) {
            err = std::unexpected(fail(std::format("{}: texture {} image {} does not exist", what, t.textureIndex, *src)));
            return r;
        }
        r.image = static_cast<int>(*src);
    }
    if (tex.samplerIndex) {
        if (*tex.samplerIndex >= c.asset.samplers.size()) {
            err = std::unexpected(
                fail(std::format("{}: texture {} sampler {} does not exist", what, t.textureIndex, *tex.samplerIndex)));
            return r;
        }
        r.sampler = static_cast<int>(*tex.samplerIndex);
    }
    return r;
}

Mat4 identity() {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

Result<ModelData> convert(Context& c) {
    const fastgltf::Asset& a = c.asset;
    ModelData m;
    m.extensionsUsed = a.extensionsUsed;
    m.extensionsRequired = a.extensionsRequired;
    const auto checkIndex = [](size_t i, size_t n, const std::string& what) -> Result<void> {
        if (i >= n) {
            return std::unexpected(fail(std::format("{} {} does not exist ({} available)", what, i, n)));
        }
        return {};
    };

    // Samplers.
    for (const fastgltf::Sampler& s : a.samplers) {
        Sampler o;
        if (s.magFilter) {
            o.magFilter = *s.magFilter == fastgltf::Filter::Nearest ? Filter::Nearest : Filter::Linear;
        }
        if (s.minFilter) {
            switch (*s.minFilter) {
            case fastgltf::Filter::Nearest: o.minFilter = Filter::Nearest, o.mipFilter = MipFilter::None; break;
            case fastgltf::Filter::Linear: o.minFilter = Filter::Linear, o.mipFilter = MipFilter::None; break;
            case fastgltf::Filter::NearestMipMapNearest: o.minFilter = Filter::Nearest, o.mipFilter = MipFilter::Nearest; break;
            case fastgltf::Filter::LinearMipMapNearest: o.minFilter = Filter::Linear, o.mipFilter = MipFilter::Nearest; break;
            case fastgltf::Filter::NearestMipMapLinear: o.minFilter = Filter::Nearest, o.mipFilter = MipFilter::Linear; break;
            case fastgltf::Filter::LinearMipMapLinear: o.minFilter = Filter::Linear, o.mipFilter = MipFilter::Linear; break;
            }
        }
        const auto wrap = [](fastgltf::Wrap w) {
            return w == fastgltf::Wrap::ClampToEdge      ? Wrap::ClampToEdge
                   : w == fastgltf::Wrap::MirroredRepeat ? Wrap::MirroredRepeat
                                                         : Wrap::Repeat;
        };
        o.wrapS = wrap(s.wrapS);
        o.wrapT = wrap(s.wrapT);
        m.samplers.push_back(o);
    }

    // Materials (before images: colour space by usage; before meshes: missing UV sets, tangents).
    const nlohmann::json* jm = arrayAt(c.json, "materials");
    for (size_t i = 0; i < a.materials.size(); ++i) {
        const fastgltf::Material& s = a.materials[i];
        const std::string what = std::format("material {}", i);
        Material o;
        o.name = s.name;
        o.baseColorFactor = {s.pbrData.baseColorFactor[0], s.pbrData.baseColorFactor[1], s.pbrData.baseColorFactor[2],
                             s.pbrData.baseColorFactor[3]};
        o.metallicFactor = s.pbrData.metallicFactor;
        o.roughnessFactor = s.pbrData.roughnessFactor;
        o.emissiveFactor = {s.emissiveFactor[0], s.emissiveFactor[1], s.emissiveFactor[2]};
        Result<void> err;
        if (s.pbrData.baseColorTexture) {
            o.baseColor = textureRef(c, *s.pbrData.baseColorTexture, 1.0f, what, err);
        }
        if (s.pbrData.metallicRoughnessTexture) {
            o.metallicRoughness = textureRef(c, *s.pbrData.metallicRoughnessTexture, 1.0f, what, err);
        }
        if (s.normalTexture) {
            o.normal = textureRef(c, *s.normalTexture, s.normalTexture->scale, what, err);
        }
        if (s.occlusionTexture) {
            o.occlusion = textureRef(c, *s.occlusionTexture, s.occlusionTexture->strength, what, err);
        }
        if (s.emissiveTexture) {
            o.emissive = textureRef(c, *s.emissiveTexture, 1.0f, what, err);
        }
        if (!err) {
            return std::unexpected(err.error());
        }
        o.alphaMode = s.alphaMode == fastgltf::AlphaMode::Mask    ? AlphaMode::Mask
                      : s.alphaMode == fastgltf::AlphaMode::Blend ? AlphaMode::Blend
                                                                  : AlphaMode::Opaque;
        o.alphaCutoff = s.alphaCutoff;
        o.doubleSided = s.doubleSided;
        if (jm && i < jm->size() && (*jm)[i].is_object() && (*jm)[i].contains("extensions")) {
            o.extensionsJson = (*jm)[i]["extensions"].dump();
        }
        m.materials.push_back(std::move(o));
    }

    if (auto r = loadImages(c, m); !r) {
        return std::unexpected(r.error());
    }

    // Meshes.
    for (size_t i = 0; i < a.meshes.size(); ++i) {
        const fastgltf::Mesh& s = a.meshes[i];
        Mesh o;
        o.name = s.name;
        o.weights.assign(s.weights.begin(), s.weights.end());
        for (size_t p = 0; p < s.primitives.size(); ++p) {
            Primitive prim;
            auto ok = buildPrimitive(c, m, i, p, s.primitives[p], prim);
            if (!ok) {
                return std::unexpected(ok.error());
            }
            if (*ok) {
                if (!o.weights.empty() && prim.targets.size() != o.weights.size()) {
                    c.warn(std::format("mesh {} primitive {}: {} morph targets, mesh.weights has {}", i, p,
                                       prim.targets.size(), o.weights.size()));
                }
                o.primitives.push_back(std::move(prim));
            }
        }
        m.meshes.push_back(std::move(o));
    }

    // Cameras.
    for (const fastgltf::Camera& s : a.cameras) {
        Camera o;
        o.name = s.name;
        if (const auto* p = std::get_if<fastgltf::Camera::Perspective>(&s.camera)) {
            o.type = Camera::Type::Perspective;
            o.yfov = p->yfov;
            o.aspectRatio = p->aspectRatio ? *p->aspectRatio : 0.0f;
            o.znear = p->znear;
            o.zfar = p->zfar ? *p->zfar : 0.0f;
        } else if (const auto* q = std::get_if<fastgltf::Camera::Orthographic>(&s.camera)) {
            o.type = Camera::Type::Orthographic;
            o.xmag = q->xmag;
            o.ymag = q->ymag;
            o.znear = q->znear;
            o.zfar = q->zfar;
        }
        m.cameras.push_back(std::move(o));
    }

    // Lights (KHR_lights_punctual).
    for (const fastgltf::Light& s : a.lights) {
        Light o;
        o.name = s.name;
        o.type = s.type == fastgltf::LightType::Directional ? Light::Type::Directional
                 : s.type == fastgltf::LightType::Spot      ? Light::Type::Spot
                                                            : Light::Type::Point;
        o.color = {s.color[0], s.color[1], s.color[2]};
        o.intensity = s.intensity;
        o.range = s.range ? *s.range : 0.0f;
        o.innerConeAngle = s.innerConeAngle ? *s.innerConeAngle : 0.0f;
        o.outerConeAngle = s.outerConeAngle ? *s.outerConeAngle : 0.785398163f;
        m.lights.push_back(std::move(o));
    }

    // Nodes: indices checked, parent links set, every node at most one parent and no cycles.
    m.nodes.resize(a.nodes.size());
    for (size_t i = 0; i < a.nodes.size(); ++i) {
        const fastgltf::Node& s = a.nodes[i];
        Node& o = m.nodes[i];
        const std::string what = std::format("node {}", i);
        o.name = s.name;
        if (s.meshIndex) {
            if (auto r = checkIndex(*s.meshIndex, a.meshes.size(), what + ": mesh"); !r) {
                return std::unexpected(r.error());
            }
            o.mesh = static_cast<int>(*s.meshIndex);
        }
        if (s.cameraIndex) {
            if (auto r = checkIndex(*s.cameraIndex, a.cameras.size(), what + ": camera"); !r) {
                return std::unexpected(r.error());
            }
            o.camera = static_cast<int>(*s.cameraIndex);
        }
        if (s.skinIndex) {
            if (auto r = checkIndex(*s.skinIndex, a.skins.size(), what + ": skin"); !r) {
                return std::unexpected(r.error());
            }
            o.skin = static_cast<int>(*s.skinIndex);
        }
        if (s.lightIndex) {
            if (auto r = checkIndex(*s.lightIndex, a.lights.size(), what + ": light"); !r) {
                return std::unexpected(r.error());
            }
            o.light = static_cast<int>(*s.lightIndex);
        }
        o.weights.assign(s.weights.begin(), s.weights.end());
        if (const auto* trs = std::get_if<fastgltf::TRS>(&s.transform)) {
            o.translation = {trs->translation[0], trs->translation[1], trs->translation[2]};
            o.rotation = {trs->rotation[0], trs->rotation[1], trs->rotation[2], trs->rotation[3]};
            o.scale = {trs->scale[0], trs->scale[1], trs->scale[2]};
        } else if (const auto* mat = std::get_if<fastgltf::math::nmat4x4>(&s.transform)) {
            o.hasMatrix = true;
            for (size_t col = 0; col < 4; ++col) {
                for (size_t row = 0; row < 4; ++row) {
                    o.matrix[col * 4 + row] = (*mat)[col][row];
                }
            }
        }
        for (const size_t ch : s.children) {
            if (auto r = checkIndex(ch, a.nodes.size(), what + ": child node"); !r) {
                return std::unexpected(r.error());
            }
            o.children.push_back(static_cast<int>(ch));
        }
    }
    for (size_t i = 0; i < m.nodes.size(); ++i) {
        for (const int ch : m.nodes[i].children) {
            if (m.nodes[ch].parent != -1 || static_cast<size_t>(ch) == i) {
                return std::unexpected(fail(std::format("node {} has more than one parent (glTF requires a tree)", ch)));
            }
            m.nodes[ch].parent = static_cast<int>(i);
        }
    }
    for (size_t i = 0; i < m.nodes.size(); ++i) { // a parent chain longer than the node count is a cycle
        int p = m.nodes[i].parent;
        for (size_t steps = 0; p != -1; ++steps) {
            if (steps > m.nodes.size()) {
                return std::unexpected(fail(std::format("node {}: the node hierarchy has a cycle", i)));
            }
            p = m.nodes[static_cast<size_t>(p)].parent;
        }
    }

    // Scenes.
    for (size_t i = 0; i < a.scenes.size(); ++i) {
        Scene o;
        o.name = a.scenes[i].name;
        for (const size_t n : a.scenes[i].nodeIndices) {
            if (auto r = checkIndex(n, a.nodes.size(), std::format("scene {}: node", i)); !r) {
                return std::unexpected(r.error());
            }
            o.nodes.push_back(static_cast<int>(n));
        }
        m.scenes.push_back(std::move(o));
    }
    if (a.defaultScene) {
        if (auto r = checkIndex(*a.defaultScene, a.scenes.size(), "default scene"); !r) {
            return std::unexpected(r.error());
        }
        m.defaultScene = static_cast<int>(*a.defaultScene);
    }

    // Skins.
    for (size_t i = 0; i < a.skins.size(); ++i) {
        const fastgltf::Skin& s = a.skins[i];
        const std::string what = std::format("skin {}", i);
        Skin o;
        o.name = s.name;
        for (const size_t j : s.joints) {
            if (auto r = checkIndex(j, a.nodes.size(), what + ": joint node"); !r) {
                return std::unexpected(r.error());
            }
            o.joints.push_back(static_cast<int>(j));
        }
        if (s.skeleton) {
            if (auto r = checkIndex(*s.skeleton, a.nodes.size(), what + ": skeleton node"); !r) {
                return std::unexpected(r.error());
            }
            o.skeleton = static_cast<int>(*s.skeleton);
        }
        o.inverseBindMatrices.assign(o.joints.size(), identity());
        if (s.inverseBindMatrices) {
            uint32_t comps = 0;
            auto f = readFloats(c, *s.inverseBindMatrices, comps);
            if (!f) {
                return std::unexpected(f.error());
            }
            if (comps != 16 || f->size() / 16 < o.joints.size()) {
                return std::unexpected(fail(std::format("{}: inverseBindMatrices must be MAT4 with {} elements", what,
                                                        o.joints.size())));
            }
            for (size_t j = 0; j < o.joints.size(); ++j) {
                std::copy_n(f->begin() + static_cast<ptrdiff_t>(j * 16), 16, o.inverseBindMatrices[j].begin());
            }
        }
        m.skins.push_back(std::move(o));
    }

    // Animations (parsed for F4; outputs decoded to float, normalized integers resolved).
    for (size_t i = 0; i < a.animations.size(); ++i) {
        const fastgltf::Animation& s = a.animations[i];
        const std::string what = std::format("animation {}", i);
        Animation o;
        o.name = s.name;
        for (size_t k = 0; k < s.samplers.size(); ++k) {
            const fastgltf::AnimationSampler& sm = s.samplers[k];
            AnimationSampler os;
            os.interpolation = sm.interpolation == fastgltf::AnimationInterpolation::Step ? AnimationSampler::Interpolation::Step
                               : sm.interpolation == fastgltf::AnimationInterpolation::CubicSpline
                                   ? AnimationSampler::Interpolation::CubicSpline
                                   : AnimationSampler::Interpolation::Linear;
            uint32_t comps = 0;
            auto in = readFloats(c, sm.inputAccessor, comps);
            if (!in) {
                return std::unexpected(in.error());
            }
            if (comps != 1) {
                return std::unexpected(fail(std::format("{} sampler {}: input must be SCALAR", what, k)));
            }
            auto out = readFloats(c, sm.outputAccessor, comps);
            if (!out) {
                return std::unexpected(out.error());
            }
            os.input = std::move(*in);
            os.output = std::move(*out);
            os.outputComponents = comps;
            o.samplers.push_back(std::move(os));
        }
        for (size_t k = 0; k < s.channels.size(); ++k) {
            const fastgltf::AnimationChannel& ch = s.channels[k];
            AnimationChannel oc;
            if (auto r = checkIndex(ch.samplerIndex, s.samplers.size(), std::format("{} channel {}: sampler", what, k)); !r) {
                return std::unexpected(r.error());
            }
            oc.sampler = static_cast<int>(ch.samplerIndex);
            if (ch.nodeIndex) {
                if (auto r = checkIndex(*ch.nodeIndex, a.nodes.size(), std::format("{} channel {}: node", what, k)); !r) {
                    return std::unexpected(r.error());
                }
                oc.node = static_cast<int>(*ch.nodeIndex);
            }
            oc.path = ch.path == fastgltf::AnimationPath::Rotation  ? AnimationChannel::Path::Rotation
                      : ch.path == fastgltf::AnimationPath::Scale   ? AnimationChannel::Path::Scale
                      : ch.path == fastgltf::AnimationPath::Weights ? AnimationChannel::Path::Weights
                                                                    : AnimationChannel::Path::Translation;
            o.channels.push_back(std::move(oc));
        }
        m.animations.push_back(std::move(o));
    }
    return m;
}

} // namespace

Result<std::vector<std::byte>> readFileCapped(const std::filesystem::path& p, uint64_t cap, const std::string& what) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) {
        return std::unexpected(fail(std::format("{}: cannot open '{}': not a file", what, u8(p))));
    }
    const uint64_t size = std::filesystem::file_size(p, ec);
    if (ec) {
        return std::unexpected(fail(std::format("{}: cannot read '{}': {}", what, u8(p), ec.message())));
    }
    if (size > cap) {
        return std::unexpected(fail(std::format("{}: '{}' is {} bytes, more than r.Asset.MaxFileBytes ({} bytes)", what,
                                                u8(p), size, cap)));
    }
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return std::unexpected(fail(std::format("{}: cannot open '{}'", what, u8(p))));
    }
    std::vector<std::byte> bytes(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (static_cast<uint64_t>(in.gcount()) != size) {
        return std::unexpected(fail(std::format("{}: short read of '{}'", what, u8(p))));
    }
    return bytes;
}

Result<std::filesystem::path> resolveUri(const std::filesystem::path& baseDir, const std::string& uri,
                                         const std::string& what) {
    if (uri.empty()) {
        return std::unexpected(fail(what + ": empty URI"));
    }
    if (uri.find("://") != std::string::npos || uri.rfind("file:", 0) == 0) {
        return std::unexpected(fail(std::format("{}: URI '{}' is not a relative path (remote / file URIs are not "
                                                "loaded)",
                                                what, uri)));
    }
    std::string decoded;
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size() && hexValue(uri[i + 1]) >= 0 && hexValue(uri[i + 2]) >= 0) {
            decoded.push_back(static_cast<char>(hexValue(uri[i + 1]) * 16 + hexValue(uri[i + 2])));
            i += 2;
        } else {
            decoded.push_back(uri[i]);
        }
    }
    if (decoded.find('\0') != std::string::npos) {
        return std::unexpected(fail(what + ": URI contains a NUL byte"));
    }
    const std::filesystem::path rel(std::u8string(decoded.begin(), decoded.end()));
    if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) {
        return std::unexpected(fail(std::format("{}: URI '{}' is an absolute path", what, uri)));
    }
    return (baseDir / rel).lexically_normal();
}

} // namespace detail

Result<ModelData> loadGltf(const std::filesystem::path& path, const LoadOptions& options, std::vector<Diagnostic>* diags) {
    using namespace detail;
    try {
        LoadOptions opts = options;
        opts.maxTextureSize = std::max(1u, opts.maxTextureSize);
        auto file = readFileCapped(path, opts.maxFileBytes, "glTF file");
        if (!file) {
            return std::unexpected(file.error());
        }
        const std::span<const std::byte> bytes(*file);
        if (bytes.empty()) {
            return std::unexpected(fail(std::format("'{}': empty file", u8(path))));
        }
        const bool glb = bytes.size() >= 4 && std::memcmp(bytes.data(), "glTF", 4) == 0;
        std::string_view jsonText;
        if (glb) {
            auto j = glbJson(bytes);
            if (!j) {
                return std::unexpected(j.error());
            }
            jsonText = *j;
        } else {
            jsonText = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (jsonText.size() >= 3 && static_cast<unsigned char>(jsonText[0]) == 0xEF &&
                static_cast<unsigned char>(jsonText[1]) == 0xBB && static_cast<unsigned char>(jsonText[2]) == 0xBF) {
                jsonText.remove_prefix(3); // UTF-8 BOM
            }
        }
        const nlohmann::json doc = nlohmann::json::parse(jsonText, nullptr, /*allow_exceptions=*/false);
        if (doc.is_discarded() || !doc.is_object()) {
            return std::unexpected(fail(std::format("'{}': invalid JSON (not a glTF JSON object)", u8(path))));
        }
        std::vector<Diagnostic> local;
        std::vector<Diagnostic>* d = diags ? diags : &local;
        if (auto r = checkExtensions(doc, d); !r) {
            return std::unexpected(r.error());
        }
        if (auto r = checkDataUris(doc, opts.maxFileBytes); !r) {
            return std::unexpected(r.error());
        }

        // fastgltf: every extension it knows (the policy above already rejected unsupported required ones).
        constexpr auto kAll = static_cast<fastgltf::Extensions>((uint64_t{1} << 37) - 2); // bits 1..36 = every enumerator
        fastgltf::Parser parser(kAll);
        auto data = fastgltf::GltfDataBuffer::FromBytes(bytes.data(), bytes.size());
        if (data.error() != fastgltf::Error::None) {
            return std::unexpected(fail(std::format("'{}': fastgltf: {}", u8(path), fastgltf::getErrorMessage(data.error()))));
        }
        const std::filesystem::path baseDir = path.parent_path();
        auto asset = parser.loadGltf(data.get(), baseDir, fastgltf::Options::None, fastgltf::Category::All);
        if (asset.error() != fastgltf::Error::None) {
            return std::unexpected(fail(std::format("'{}': glTF parse error ({}): {}", u8(path),
                                                    fastgltf::getErrorName(asset.error()),
                                                    fastgltf::getErrorMessage(asset.error()))));
        }
        Context c{asset.get(), doc, opts, d, baseDir, {}, {}};

        // Buffers: bytes of each glTF buffer, exactly byteLength long.
        const nlohmann::json* jb = arrayAt(doc, "buffers");
        c.owned.reserve(asset->buffers.size());
        for (size_t i = 0; i < asset->buffers.size(); ++i) {
            const fastgltf::Buffer& b = asset->buffers[i];
            const std::string what = std::format("buffer {}", i);
            std::span<const std::byte> s;
            if (const auto* arr = std::get_if<fastgltf::sources::Array>(&b.data)) {
                s = std::span<const std::byte>(arr->bytes.data(), arr->bytes.size());
            } else if (const auto* vec = std::get_if<fastgltf::sources::Vector>(&b.data)) {
                s = vec->bytes;
            } else if (const auto* view = std::get_if<fastgltf::sources::ByteView>(&b.data)) {
                s = view->bytes;
            } else if (std::holds_alternative<fastgltf::sources::URI>(b.data)) {
                std::string uri;
                if (jb && i < jb->size() && (*jb)[i].contains("uri") && (*jb)[i]["uri"].is_string()) {
                    uri = (*jb)[i]["uri"].get<std::string>();
                }
                auto p = resolveUri(baseDir, uri, what);
                if (!p) {
                    return std::unexpected(p.error());
                }
                auto f = readFileCapped(*p, opts.maxFileBytes, what);
                if (!f) {
                    return std::unexpected(f.error());
                }
                c.owned.push_back(std::move(*f));
                s = c.owned.back();
            } else if (std::holds_alternative<fastgltf::sources::Fallback>(b.data)) {
                c.buffers.emplace_back(); // EXT_meshopt_compression fallback buffer: no data (plan F3 Task 3)
                continue;
            } else {
                return std::unexpected(fail(what + ": no data (a GLB without a BIN chunk, or no uri)"));
            }
            if (s.size() < b.byteLength) {
                return std::unexpected(fail(std::format("{}: {} bytes of data, byteLength says {} (truncated)", what,
                                                        s.size(), b.byteLength)));
            }
            c.buffers.push_back(s.first(b.byteLength));
        }
        return convert(c);
    } catch (const std::bad_alloc&) {
        return std::unexpected(detail::fail(std::format("'{}': out of memory while loading", detail::u8(path))));
    } catch (const std::exception& e) {
        return std::unexpected(detail::fail(std::format("'{}': {}", detail::u8(path), e.what())));
    }
}

LoadOptions loadOptionsFromCVars() {
    CVarRegistry& reg = CVarRegistry::get();
    const LoadOptions def;
    CVar* tex = reg.find("r.Asset.MaxTextureSize");
    if (!tex) {
        tex = reg.registerInt("r.Asset.MaxTextureSize", static_cast<int32_t>(def.maxTextureSize),
                              "Largest glTF image edge kept; larger images are downscaled with a warning", CVarFlags::None,
                              1, 65536);
    }
    CVar* verts = reg.find("r.Asset.MaxVertices");
    if (!verts) {
        verts = reg.registerInt("r.Asset.MaxVertices", static_cast<int32_t>(def.maxVertices),
                                "Largest glTF primitive vertex count; larger primitives are skipped with a warning",
                                CVarFlags::None, 1, INT32_MAX);
    }
    CVar* mib = reg.find("r.Asset.MaxFileBytes");
    if (!mib) {
        mib = reg.registerInt("r.Asset.MaxFileBytes", static_cast<int32_t>(def.maxFileBytes >> 20),
                              "Largest glTF / bin / image file in MiB; a larger file is refused (exit 2)",
                              CVarFlags::None, 1, 1 << 20);
    }
    LoadOptions o;
    o.maxTextureSize = static_cast<uint32_t>(tex->getInt());
    o.maxVertices = static_cast<uint64_t>(verts->getInt());
    o.maxFileBytes = static_cast<uint64_t>(mib->getInt()) << 20;
    return o;
}

} // namespace eng::asset
