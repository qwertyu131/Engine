// KHR_draco_mesh_compression (plan F3 Task 3; extension spec: github.com/KhronosGroup/glTF extensions/2.0/Khronos/
// KHR_draco_mesh_compression). The primitive's extension object names a bufferView holding a Draco stream and maps
// each glTF attribute semantic to a Draco attribute unique id. The stream is decoded with the pinned Draco decoder
// (cmake/Dependencies.cmake), and every mapped attribute is written into Context::accessorData in the layout of its
// glTF accessor (componentType / type / normalized as the accessor declares, Draco converts), so Accessors.cpp,
// sparse substitution and the KHR_mesh_quantization conversions treat it like any other accessor. The indices
// accessor gets the Draco faces.
//
// Errors (exit 2, Review Focus 1): a stream that does not decode, an unknown attribute id, a decoded point count other
// than the accessor's count (the extension requires them equal), a face count other than indices.count / 3, a value
// that does not fit the accessor's component type, r.Asset.MaxVertices exceeded.
#include "Internal.h"

#include <cstring>
#include <format>
#include <limits>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <draco/compression/decode.h>
#include <draco/mesh/mesh.h>
#include <draco/point_cloud/point_cloud.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace eng::asset::detail {

namespace {

uint32_t componentBytesOf(fastgltf::ComponentType t) {
    switch (t) {
    case fastgltf::ComponentType::Byte:
    case fastgltf::ComponentType::UnsignedByte: return 1;
    case fastgltf::ComponentType::Short:
    case fastgltf::ComponentType::UnsignedShort: return 2;
    case fastgltf::ComponentType::UnsignedInt:
    case fastgltf::ComponentType::Float: return 4;
    default: return 0;
    }
}

template <class T>
bool convertAll(const draco::PointAttribute& attr, uint32_t points, uint32_t comps, std::vector<std::byte>& out) {
    out.resize(size_t(points) * comps * sizeof(T));
    std::array<T, 16> tmp{};
    for (uint32_t i = 0; i < points; ++i) {
        if (!attr.ConvertValue<T>(attr.mapped_index(draco::PointIndex(i)), static_cast<int8_t>(comps), tmp.data())) {
            return false;
        }
        std::memcpy(out.data() + size_t(i) * comps * sizeof(T), tmp.data(), comps * sizeof(T));
    }
    return true;
}

// Matrices of 1- and 2-byte components need column padding (glTF 2.0 §3.6.2.4); attributes of a Draco primitive are
// scalars / vectors, so a matrix accessor is refused.
bool matrixType(fastgltf::AccessorType t) {
    return t == fastgltf::AccessorType::Mat2 || t == fastgltf::AccessorType::Mat3 || t == fastgltf::AccessorType::Mat4;
}

} // namespace

Result<void> decodeDracoPrimitive(const Context& c, const fastgltf::Primitive& in, const std::string& where) {
    if (!in.dracoCompression) {
        return {};
    }
    const fastgltf::DracoCompressedPrimitive& dc = *in.dracoCompression;
    const std::string what = where + " (KHR_draco_mesh_compression)";
    auto bytes = bufferViewBytes(c, dc.bufferView, what);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    draco::DecoderBuffer buffer;
    buffer.Init(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    auto type = draco::Decoder::GetEncodedGeometryType(&buffer);
    if (!type.ok()) {
        return std::unexpected(fail(std::format("{}: bufferView {} is not a Draco stream: {}", what, dc.bufferView,
                                                type.status().error_msg_string())));
    }
    draco::Decoder decoder;
    std::unique_ptr<draco::PointCloud> geometry;
    const draco::Mesh* mesh = nullptr;
    if (type.value() == draco::TRIANGULAR_MESH) {
        auto m = decoder.DecodeMeshFromBuffer(&buffer);
        if (!m.ok()) {
            return std::unexpected(fail(std::format("{}: Draco mesh does not decode: {}", what,
                                                    m.status().error_msg_string())));
        }
        mesh = m.value().get();
        geometry = std::move(m).value();
    } else if (type.value() == draco::POINT_CLOUD) {
        auto p = decoder.DecodePointCloudFromBuffer(&buffer);
        if (!p.ok()) {
            return std::unexpected(fail(std::format("{}: Draco point cloud does not decode: {}", what,
                                                    p.status().error_msg_string())));
        }
        geometry = std::move(p).value();
    } else {
        return std::unexpected(fail(std::format("{}: unsupported Draco geometry type {}", what, int(type.value()))));
    }
    const uint64_t points = geometry->num_points();
    if (points > c.options.maxVertices) {
        return std::unexpected(fail(std::format("{}: {} Draco points exceed r.Asset.MaxVertices ({})", what, points,
                                                c.options.maxVertices)));
    }
    const auto& accessors = c.asset.accessors;
    for (const fastgltf::Attribute& a : dc.attributes) {
        const std::string an = std::string(a.name);
        const auto glAttr = in.findAttribute(an);
        if (glAttr == in.attributes.end()) {
            return std::unexpected(fail(std::format("{}: Draco attribute {} is not an attribute of the primitive", what, an)));
        }
        const size_t acc = glAttr->accessorIndex;
        if (acc >= accessors.size()) {
            return std::unexpected(fail(std::format("{} {}: accessor {} does not exist", what, an, acc)));
        }
        const fastgltf::Accessor& ga = accessors[acc];
        const draco::PointAttribute* attr = geometry->GetAttributeByUniqueId(static_cast<uint32_t>(a.accessorIndex));
        if (!attr) {
            return std::unexpected(fail(std::format("{} {}: no Draco attribute with id {}", what, an, a.accessorIndex)));
        }
        if (ga.count != points) {
            return std::unexpected(fail(std::format("{} {}: accessor {} count {} != {} decoded Draco points", what, an,
                                                    acc, ga.count, points)));
        }
        const uint32_t comps = componentCount(ga.type);
        if (comps == 0 || matrixType(ga.type) || componentBytesOf(ga.componentType) == 0) {
            return std::unexpected(fail(std::format("{} {}: accessor {} type not allowed for a Draco attribute", what,
                                                    an, acc)));
        }
        std::vector<std::byte> data;
        bool converted = false;
        const uint32_t n = static_cast<uint32_t>(points);
        switch (ga.componentType) {
        case fastgltf::ComponentType::Byte: converted = convertAll<int8_t>(*attr, n, comps, data); break;
        case fastgltf::ComponentType::UnsignedByte: converted = convertAll<uint8_t>(*attr, n, comps, data); break;
        case fastgltf::ComponentType::Short: converted = convertAll<int16_t>(*attr, n, comps, data); break;
        case fastgltf::ComponentType::UnsignedShort: converted = convertAll<uint16_t>(*attr, n, comps, data); break;
        case fastgltf::ComponentType::UnsignedInt: converted = convertAll<uint32_t>(*attr, n, comps, data); break;
        case fastgltf::ComponentType::Float: converted = convertAll<float>(*attr, n, comps, data); break;
        default: break;
        }
        if (!converted) {
            return std::unexpected(fail(std::format("{} {}: Draco attribute {} ({} components) cannot be converted to "
                                                    "accessor {}",
                                                    what, an, a.accessorIndex, int(attr->num_components()), acc)));
        }
        c.accessorData[acc] = std::move(data);
    }
    if (in.indicesAccessor) {
        const size_t acc = *in.indicesAccessor;
        if (acc >= accessors.size()) {
            return std::unexpected(fail(std::format("{}: indices accessor {} does not exist", what, acc)));
        }
        const fastgltf::Accessor& ga = accessors[acc];
        if (!mesh) {
            return std::unexpected(fail(what + ": indices with a Draco point cloud"));
        }
        const uint64_t faces = mesh->num_faces();
        if (ga.count != faces * 3) {
            return std::unexpected(fail(std::format("{}: indices accessor {} count {} != 3 x {} Draco faces", what, acc,
                                                    ga.count, faces)));
        }
        const uint32_t ib = componentBytesOf(ga.componentType);
        if (ga.type != fastgltf::AccessorType::Scalar ||
            (ga.componentType != fastgltf::ComponentType::UnsignedByte &&
             ga.componentType != fastgltf::ComponentType::UnsignedShort &&
             ga.componentType != fastgltf::ComponentType::UnsignedInt)) {
            return std::unexpected(fail(std::format("{}: indices accessor {} must be UNSIGNED_BYTE / SHORT / INT SCALAR",
                                                    what, acc)));
        }
        const uint64_t maxValue = ib == 1 ? 0xFFu : ib == 2 ? 0xFFFFu : 0xFFFFFFFFu;
        std::vector<std::byte> data(size_t(faces) * 3 * ib);
        for (uint32_t f = 0; f < faces; ++f) {
            const draco::Mesh::Face& face = mesh->face(draco::FaceIndex(f));
            for (int k = 0; k < 3; ++k) {
                const uint32_t v = face[k].value();
                if (v > maxValue) {
                    return std::unexpected(fail(std::format("{}: Draco index {} does not fit accessor {}", what, v, acc)));
                }
                std::memcpy(data.data() + (size_t(f) * 3 + k) * ib, &v, ib); // little endian: low bytes first
            }
        }
        c.accessorData[acc] = std::move(data);
    }
    return {};
}

} // namespace eng::asset::detail
