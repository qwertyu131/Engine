// EXT_meshopt_compression and KHR_meshopt_compression (plan F3 Task 3, spec §7.2: the same decoder for both). A
// compressed bufferView carries {buffer, byteOffset, byteLength, byteStride, count, mode, filter}: the compressed
// bytes live in `buffer`, the view's own buffer may be a fallback buffer without data (Context::buffers entry empty).
// Each compressed view is decoded once here with the pinned meshoptimizer (cmake/Dependencies.cmake) into
// Context::meshoptViews; bufferViewBytes() then returns the decoded bytes, so accessors, sparse data and images read
// it unchanged.
//
// Decoders: ATTRIBUTES -> meshopt_decodeVertexBuffer (codec versions 0 / 1) + the filter (OCTAHEDRAL, QUATERNION,
// EXPONENTIAL, COLOR), TRIANGLES -> meshopt_decodeIndexBuffer, INDICES -> meshopt_decodeIndexSequence.
// Errors (exit 2): the compressed range outside its buffer, a stride / count the mode does not allow (checked before
// meshoptimizer is called: its asserts are not input validation), count * byteStride above the view's byteLength or
// r.Asset.MaxFileBytes, a stream the decoder rejects.
#include "Internal.h"

#include <format>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <meshoptimizer.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace eng::asset::detail {

Result<void> decodeMeshoptViews(Context& c) {
    const auto& views = c.asset.bufferViews;
    c.meshoptViews.assign(views.size(), {});
    c.meshoptDecoded.assign(views.size(), 0);
    for (size_t i = 0; i < views.size(); ++i) {
        if (!views[i].meshoptCompression) {
            continue;
        }
        const fastgltf::CompressedBufferView& m = *views[i].meshoptCompression;
        const std::string what = std::format("bufferView {} (meshopt)", i);
        if (m.bufferIndex >= c.buffers.size()) {
            return std::unexpected(fail(std::format("{}: buffer {} does not exist", what, m.bufferIndex)));
        }
        const std::span<const std::byte> buffer = c.buffers[m.bufferIndex];
        uint64_t end = 0;
        if (!mulAdd(1, m.byteOffset, m.byteLength, end) || end > buffer.size()) {
            return std::unexpected(fail(std::format("{}: compressed bytes [{}, {}) exceed buffer {} ({} bytes)", what,
                                                    m.byteOffset, uint64_t(m.byteOffset) + m.byteLength, m.bufferIndex,
                                                    buffer.size())));
        }
        uint64_t decodedBytes = 0;
        if (!mulAdd(m.count, m.byteStride, 0, decodedBytes) || decodedBytes > views[i].byteLength ||
            decodedBytes > c.options.maxFileBytes) {
            return std::unexpected(fail(std::format("{}: count {} x byteStride {} exceeds the view's byteLength {} / "
                                                    "r.Asset.MaxFileBytes",
                                                    what, m.count, m.byteStride, views[i].byteLength)));
        }
        const auto* src = reinterpret_cast<const unsigned char*>(buffer.data() + m.byteOffset);
        // Zero-filled to the view's byteLength (the decoded data is count * byteStride of it).
        std::vector<std::byte> out(views[i].byteLength);
        int rc = -1;
        switch (m.mode) {
        case fastgltf::MeshoptCompressionMode::Attributes:
            if (m.byteStride == 0 || m.byteStride % 4 != 0 || m.byteStride > 256) {
                return std::unexpected(fail(std::format("{}: ATTRIBUTES byteStride {} (multiple of 4, <= 256)", what,
                                                        m.byteStride)));
            }
            rc = meshopt_decodeVertexBuffer(out.data(), m.count, m.byteStride, src, m.byteLength);
            break;
        case fastgltf::MeshoptCompressionMode::Triangles:
            if ((m.byteStride != 2 && m.byteStride != 4) || m.count % 3 != 0) {
                return std::unexpected(fail(std::format("{}: TRIANGLES byteStride {} / count {} (2 or 4, multiple of 3)",
                                                        what, m.byteStride, m.count)));
            }
            rc = meshopt_decodeIndexBuffer(out.data(), m.count, m.byteStride, src, m.byteLength);
            break;
        case fastgltf::MeshoptCompressionMode::Indices:
            if (m.byteStride != 2 && m.byteStride != 4) {
                return std::unexpected(fail(std::format("{}: INDICES byteStride {} (2 or 4)", what, m.byteStride)));
            }
            rc = meshopt_decodeIndexSequence(out.data(), m.count, m.byteStride, src, m.byteLength);
            break;
        }
        if (rc != 0) {
            return std::unexpected(fail(std::format("{}: the meshopt stream does not decode (meshoptimizer error {})",
                                                    what, rc)));
        }
        if (m.filter != fastgltf::MeshoptCompressionFilter::None) {
            if (m.mode != fastgltf::MeshoptCompressionMode::Attributes) {
                return std::unexpected(fail(what + ": a filter on a non-ATTRIBUTES view"));
            }
            const size_t s = m.byteStride;
            switch (m.filter) {
            case fastgltf::MeshoptCompressionFilter::Octahedral:
                if (s != 4 && s != 8) {
                    return std::unexpected(fail(std::format("{}: OCTAHEDRAL byteStride {} (4 or 8)", what, s)));
                }
                meshopt_decodeFilterOct(out.data(), m.count, s);
                break;
            case fastgltf::MeshoptCompressionFilter::Quaternion:
                if (s != 8) {
                    return std::unexpected(fail(std::format("{}: QUATERNION byteStride {} (8)", what, s)));
                }
                meshopt_decodeFilterQuat(out.data(), m.count, s);
                break;
            case fastgltf::MeshoptCompressionFilter::Exponential:
                if (s % 4 != 0) {
                    return std::unexpected(fail(std::format("{}: EXPONENTIAL byteStride {} (multiple of 4)", what, s)));
                }
                meshopt_decodeFilterExp(out.data(), m.count, s);
                break;
            case fastgltf::MeshoptCompressionFilter::Color:
                if (s != 4 && s != 8) {
                    return std::unexpected(fail(std::format("{}: COLOR byteStride {} (4 or 8)", what, s)));
                }
                meshopt_decodeFilterColor(out.data(), m.count, s);
                break;
            case fastgltf::MeshoptCompressionFilter::None: break;
            }
        }
        c.meshoptViews[i] = std::move(out);
        c.meshoptDecoded[i] = 1;
    }
    return {};
}

} // namespace eng::asset::detail
