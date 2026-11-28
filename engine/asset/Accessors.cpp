// Accessor decoding (plan F3 Task 2, Review Focus 1). Every byte range is checked before it is read:
//   accessor.byteOffset + byteStride * (count - 1) + elementSize <= bufferView.byteLength,
//   bufferView.byteOffset + byteLength <= buffer.byteLength (and the bytes actually present),
// in overflow-safe 64-bit arithmetic, BEFORE the output is allocated, so a huge count (e.g. 2^31) is rejected
// arithmetically. Sparse substitution (glTF 2.0 §3.6.2.3): indices strictly < count, both views range-checked.
// An accessor without bufferView is zeros (sparse-only); its size is capped by r.Asset.MaxFileBytes.
// Normalized integers: glTF 2.0 §3.11 (signed: max(c / (2^(b-1) - 1), -1), unsigned: c / (2^b - 1)).
// Matrices of 1- and 2-byte components have each column padded to 4 bytes (§3.6.2.4).
#include "Internal.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace eng::asset::detail {

namespace {

uint32_t componentBytes(fastgltf::ComponentType t) {
    switch (t) {
    case fastgltf::ComponentType::Byte:
    case fastgltf::ComponentType::UnsignedByte: return 1;
    case fastgltf::ComponentType::Short:
    case fastgltf::ComponentType::UnsignedShort: return 2;
    case fastgltf::ComponentType::UnsignedInt:
    case fastgltf::ComponentType::Float: return 4;
    default: return 0; // Int (5124) is not a glTF 2.0 accessor type; 64-bit / half / double are extensions
    }
}

const char* componentName(fastgltf::ComponentType t) {
    switch (t) {
    case fastgltf::ComponentType::Byte: return "BYTE";
    case fastgltf::ComponentType::UnsignedByte: return "UNSIGNED_BYTE";
    case fastgltf::ComponentType::Short: return "SHORT";
    case fastgltf::ComponentType::UnsignedShort: return "UNSIGNED_SHORT";
    case fastgltf::ComponentType::UnsignedInt: return "UNSIGNED_INT";
    case fastgltf::ComponentType::Float: return "FLOAT";
    default: return "an unsupported component type";
    }
}

// Layout of one element: total size and the byte offset of each component (matrix column padding).
struct Layout {
    uint32_t components = 0;
    uint32_t compBytes = 0;
    uint32_t elementBytes = 0;
    std::array<uint32_t, 16> offset{};
};

Layout layoutOf(fastgltf::AccessorType type, fastgltf::ComponentType ct) {
    Layout l;
    l.components = componentCount(type);
    l.compBytes = componentBytes(ct);
    uint32_t columns = 1, rows = l.components;
    if (type == fastgltf::AccessorType::Mat2) {
        columns = rows = 2;
    } else if (type == fastgltf::AccessorType::Mat3) {
        columns = rows = 3;
    } else if (type == fastgltf::AccessorType::Mat4) {
        columns = rows = 4;
    }
    const bool matrix = columns > 1;
    const uint32_t columnBytes = matrix ? (rows * l.compBytes + 3u) & ~3u : rows * l.compBytes;
    for (uint32_t c = 0; c < columns; ++c) {
        for (uint32_t r = 0; r < rows; ++r) {
            l.offset[c * rows + r] = c * columnBytes + r * l.compBytes;
        }
    }
    l.elementBytes = columns * columnBytes;
    return l;
}

template <class T>
T load(const std::byte* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

float toFloat(const std::byte* p, fastgltf::ComponentType ct, bool normalized) {
    switch (ct) {
    case fastgltf::ComponentType::Byte: {
        const auto v = load<int8_t>(p);
        return normalized ? std::max(static_cast<float>(v) / 127.0f, -1.0f) : static_cast<float>(v);
    }
    case fastgltf::ComponentType::UnsignedByte: {
        const auto v = load<uint8_t>(p);
        return normalized ? static_cast<float>(v) / 255.0f : static_cast<float>(v);
    }
    case fastgltf::ComponentType::Short: {
        const auto v = load<int16_t>(p);
        return normalized ? std::max(static_cast<float>(v) / 32767.0f, -1.0f) : static_cast<float>(v);
    }
    case fastgltf::ComponentType::UnsignedShort: {
        const auto v = load<uint16_t>(p);
        return normalized ? static_cast<float>(v) / 65535.0f : static_cast<float>(v);
    }
    case fastgltf::ComponentType::UnsignedInt: return static_cast<float>(load<uint32_t>(p));
    case fastgltf::ComponentType::Float: return load<float>(p);
    default: return 0.0f;
    }
}

uint32_t toUint(const std::byte* p, fastgltf::ComponentType ct) {
    switch (ct) {
    case fastgltf::ComponentType::UnsignedByte: return load<uint8_t>(p);
    case fastgltf::ComponentType::UnsignedShort: return load<uint16_t>(p);
    case fastgltf::ComponentType::UnsignedInt: return load<uint32_t>(p);
    default: return 0;
    }
}

// The checked source of an accessor: element i starts at base + i * stride (base null = no bufferView, zeros).
struct Source {
    const std::byte* base = nullptr;
    uint64_t stride = 0;
    Layout layout;
    size_t count = 0;
};

Result<Source> checkAccessor(const Context& c, size_t index) {
    const auto& accessors = c.asset.accessors;
    if (index >= accessors.size()) {
        return std::unexpected(fail(std::format("accessor {} does not exist ({} accessors)", index, accessors.size())));
    }
    const fastgltf::Accessor& a = accessors[index];
    Source s;
    s.layout = layoutOf(a.type, a.componentType);
    if (s.layout.components == 0) {
        return std::unexpected(fail(std::format("accessor {}: invalid type", index)));
    }
    if (s.layout.compBytes == 0) {
        return std::unexpected(fail(std::format("accessor {}: componentType {} is not allowed", index,
                                                static_cast<unsigned>(a.componentType))));
    }
    s.count = a.count;
    const uint64_t count = a.count;
    if (!a.bufferViewIndex) {
        // Zeros (+ sparse): nothing bounds the count by file data, so it is bounded by r.Asset.MaxVertices (such
        // accessors are sparse morph targets / attributes, one element per vertex) and the decoded output size by
        // r.Asset.MaxFileBytes, both before anything is allocated.
        uint64_t decoded = 0;
        if (count > c.options.maxVertices || !mulAdd(count, uint64_t{s.layout.components} * 4u, 0, decoded) ||
            decoded > c.options.maxFileBytes) {
            return std::unexpected(fail(std::format("accessor {}: count {} without a bufferView exceeds "
                                                    "r.Asset.MaxVertices ({}) / r.Asset.MaxFileBytes ({} bytes decoded)",
                                                    index, count, c.options.maxVertices, c.options.maxFileBytes)));
        }
        return s;
    }
    const size_t viewIndex = *a.bufferViewIndex;
    auto view = bufferViewBytes(c, viewIndex, std::format("accessor {}", index));
    if (!view) {
        return std::unexpected(view.error());
    }
    const fastgltf::BufferView& bv = c.asset.bufferViews[viewIndex];
    s.stride = bv.byteStride ? *bv.byteStride : s.layout.elementBytes;
    if (s.stride < s.layout.elementBytes) {
        return std::unexpected(fail(std::format("accessor {}: bufferView {} byteStride {} < element size {}", index,
                                                viewIndex, s.stride, s.layout.elementBytes)));
    }
    if (count > 0) {
        uint64_t end = 0;
        if (!mulAdd(count - 1, s.stride, a.byteOffset, end) || !mulAdd(1, end, s.layout.elementBytes, end) ||
            end > view->size()) {
            return std::unexpected(fail(std::format("accessor {}: {} elements of {} bytes (stride {}) from byte {} "
                                                    "exceed bufferView {} ({} bytes)",
                                                    index, count, s.layout.elementBytes, s.stride, a.byteOffset,
                                                    viewIndex, view->size())));
        }
    }
    if (a.byteOffset % s.layout.compBytes != 0) {
        return std::unexpected(fail(std::format("accessor {}: byteOffset {} not aligned to {}", index, a.byteOffset,
                                                componentName(a.componentType))));
    }
    s.base = view->data() + a.byteOffset;
    return s;
}

// Sparse substitution: calls put(element, bytes of the substituted element) for every sparse entry.
template <class Put>
Result<void> applySparse(const Context& c, size_t index, const Layout& layout, Put&& put) {
    const fastgltf::Accessor& a = c.asset.accessors[index];
    if (!a.sparse) {
        return {};
    }
    const fastgltf::SparseAccessor& sp = *a.sparse;
    const std::string what = std::format("accessor {} sparse", index);
    if (sp.count == 0 || sp.count > a.count) {
        return std::unexpected(fail(std::format("{}: count {} not in [1, {}]", what, sp.count, a.count)));
    }
    const uint32_t idxBytes = componentBytes(sp.indexComponentType);
    if (idxBytes == 0 || sp.indexComponentType == fastgltf::ComponentType::Byte ||
        sp.indexComponentType == fastgltf::ComponentType::Short || sp.indexComponentType == fastgltf::ComponentType::Float) {
        return std::unexpected(fail(what + ": indices componentType must be UNSIGNED_BYTE / SHORT / INT"));
    }
    auto iv = bufferViewBytes(c, sp.indicesBufferView, what + " indices");
    if (!iv) {
        return std::unexpected(iv.error());
    }
    auto vv = bufferViewBytes(c, sp.valuesBufferView, what + " values");
    if (!vv) {
        return std::unexpected(vv.error());
    }
    uint64_t iend = 0, vend = 0;
    if (!mulAdd(sp.count, idxBytes, sp.indicesByteOffset, iend) || iend > iv->size()) {
        return std::unexpected(fail(std::format("{}: {} indices from byte {} exceed bufferView {} ({} bytes)", what,
                                                sp.count, sp.indicesByteOffset, sp.indicesBufferView, iv->size())));
    }
    if (!mulAdd(sp.count, layout.elementBytes, sp.valuesByteOffset, vend) || vend > vv->size()) {
        return std::unexpected(fail(std::format("{}: {} values from byte {} exceed bufferView {} ({} bytes)", what,
                                                sp.count, sp.valuesByteOffset, sp.valuesBufferView, vv->size())));
    }
    uint64_t previous = 0;
    for (size_t k = 0; k < sp.count; ++k) {
        const uint64_t i = toUint(iv->data() + sp.indicesByteOffset + k * idxBytes, sp.indexComponentType);
        if (i >= a.count) {
            return std::unexpected(fail(std::format("{}: index {} >= count {}", what, i, a.count)));
        }
        if (k > 0 && i <= previous) {
            return std::unexpected(fail(std::format("{}: indices not strictly increasing ({} after {})", what, i, previous)));
        }
        previous = i;
        put(static_cast<size_t>(i), vv->data() + sp.valuesByteOffset + k * layout.elementBytes);
    }
    return {};
}

} // namespace

uint32_t componentCount(fastgltf::AccessorType t) {
    switch (t) {
    case fastgltf::AccessorType::Scalar: return 1;
    case fastgltf::AccessorType::Vec2: return 2;
    case fastgltf::AccessorType::Vec3: return 3;
    case fastgltf::AccessorType::Vec4: return 4;
    case fastgltf::AccessorType::Mat2: return 4;
    case fastgltf::AccessorType::Mat3: return 9;
    case fastgltf::AccessorType::Mat4: return 16;
    default: return 0;
    }
}

Result<std::span<const std::byte>> bufferViewBytes(const Context& c, size_t view, const std::string& what) {
    const auto& views = c.asset.bufferViews;
    if (view >= views.size()) {
        return std::unexpected(fail(std::format("{}: bufferView {} does not exist ({} bufferViews)", what, view,
                                                views.size())));
    }
    const fastgltf::BufferView& bv = views[view];
    if (bv.bufferIndex >= c.buffers.size()) {
        return std::unexpected(fail(std::format("bufferView {}: buffer {} does not exist", view, bv.bufferIndex)));
    }
    const std::span<const std::byte> buffer = c.buffers[bv.bufferIndex];
    uint64_t end = 0;
    if (!mulAdd(1, bv.byteOffset, bv.byteLength, end) || end > buffer.size()) {
        return std::unexpected(fail(std::format("bufferView {}: bytes [{}, {}) exceed buffer {} ({} bytes{})", view,
                                                bv.byteOffset, static_cast<uint64_t>(bv.byteOffset) + bv.byteLength,
                                                bv.bufferIndex, buffer.size(),
                                                buffer.empty() ? ", no data: EXT_meshopt fallback or missing" : "")));
    }
    return buffer.subspan(bv.byteOffset, bv.byteLength);
}

Result<void> validateAccessor(const Context& c, size_t accessor) {
    auto src = checkAccessor(c, accessor);
    if (!src) {
        return std::unexpected(src.error());
    }
    return applySparse(c, accessor, src->layout, [](size_t, const std::byte*) {});
}

Result<std::vector<float>> readFloats(const Context& c, size_t accessor, uint32_t& components) {
    auto src = checkAccessor(c, accessor);
    if (!src) {
        return std::unexpected(src.error());
    }
    const fastgltf::Accessor& a = c.asset.accessors[accessor];
    const Layout& l = src->layout;
    components = l.components;
    std::vector<float> out(src->count * l.components, 0.0f);
    if (src->base) {
        for (size_t i = 0; i < src->count; ++i) {
            const std::byte* e = src->base + i * src->stride;
            for (uint32_t k = 0; k < l.components; ++k) {
                out[i * l.components + k] = toFloat(e + l.offset[k], a.componentType, a.normalized);
            }
        }
    }
    auto sp = applySparse(c, accessor, l, [&](size_t i, const std::byte* e) {
        for (uint32_t k = 0; k < l.components; ++k) {
            out[i * l.components + k] = toFloat(e + l.offset[k], a.componentType, a.normalized);
        }
    });
    if (!sp) {
        return std::unexpected(sp.error());
    }
    return out;
}

Result<std::vector<uint32_t>> readUints(const Context& c, size_t accessor, uint32_t& components) {
    auto src = checkAccessor(c, accessor);
    if (!src) {
        return std::unexpected(src.error());
    }
    const fastgltf::Accessor& a = c.asset.accessors[accessor];
    if (a.componentType != fastgltf::ComponentType::UnsignedByte &&
        a.componentType != fastgltf::ComponentType::UnsignedShort &&
        a.componentType != fastgltf::ComponentType::UnsignedInt) {
        return std::unexpected(fail(std::format("accessor {}: componentType {} where UNSIGNED_BYTE / SHORT / INT is "
                                                "required",
                                                accessor, componentName(a.componentType))));
    }
    const Layout& l = src->layout;
    components = l.components;
    std::vector<uint32_t> out(src->count * l.components, 0u);
    if (src->base) {
        for (size_t i = 0; i < src->count; ++i) {
            const std::byte* e = src->base + i * src->stride;
            for (uint32_t k = 0; k < l.components; ++k) {
                out[i * l.components + k] = toUint(e + l.offset[k], a.componentType);
            }
        }
    }
    auto sp = applySparse(c, accessor, l, [&](size_t i, const std::byte* e) {
        for (uint32_t k = 0; k < l.components; ++k) {
            out[i * l.components + k] = toUint(e + l.offset[k], a.componentType);
        }
    });
    if (!sp) {
        return std::unexpected(sp.error());
    }
    return out;
}

} // namespace eng::asset::detail
